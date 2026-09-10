// Tests for the GOOL bytecode VM shim (ps1Runtime/src/gool/).
//
// That module compiles into every build but was reachable only through a game
// run: `hle_gool_InterpretObject` is dispatched via an `[hle_overrides]` entry
// in an untracked config, so nothing in the repository exercised it. These
// tests drive the public entry point over a hand-built `goolobj` so the shim's
// memory access, operand decode, native-callout dispatch and return path are
// pinned without a disc.
//
// Deliberately narrow: a couple of opcodes and a terminator. Broad opcode
// coverage is Phase 4 work, not this file's job.

#include "runtime/cpu_context.h"
#include "runtime/emuptr.h"
#include "runtime/gool/emu_compat.h"
#include "runtime/gool/gool_types.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include "runtime/psyq/psyq_hle.h"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>

// Stand-in for the recompiled function a native callout would reach
// (ps1Test/runtime/test_stubs.cpp).
extern void (*g_testRecompDispatchHook)(recomp_context *, uint32_t);

namespace {

using ps1::gool::goolobj;

// All inside the 2 MB main RAM window, far enough apart not to overlap.
constexpr uint32_t kObj = 0x80110000;
constexpr uint32_t kCode = 0x80120000;  // bytecode
constexpr uint32_t kStack = 0x80130000; // the object's GOOL stack
constexpr uint32_t kSp = 0x801F0000;    // MIPS stack for the native frame
constexpr uint32_t kRaSentinel = 0x80001234;

// Error codes from gool_interp.cpp's game convention.
constexpr uint32_t kErrInvalid = 0xFFFFFFF2;

// Crash SCUS-94900 address of angle_distance, the callout opcode 0x21 makes.
constexpr uint32_t kFnAngDist = 0x800245F0;

// What the stand-in callout saw.
struct Callout {
  int calls = 0;
  uint32_t addr = 0, a0 = 0, a1 = 0;
};
Callout g_callout;

class GoolTest : public ::testing::Test {
protected:
  ps1::Memory mem;
  recomp_context ctx;

  void SetUp() override {
    mem.reset();
    ctx.reset();
    ctx.mem = &mem;
    ps1::emuptr_set_ram(mem.ramPtr());
    // interpretObject's prologue spills ten callee-saved registers at
    // SP-320..SP-4, so SP has to be real RAM with room below it.
    ctx.r[29] = kSp;
    ctx.r[31] = kRaSentinel;
    g_callout = Callout{};
    g_testRecompDispatchHook = nullptr;
    ps1::metrics::reset();
    ps1::metrics::setEnabledForTesting(true);
  }

  void TearDown() override {
    g_testRecompDispatchHook = nullptr;
    ps1::metrics::reset();
    ps1::metrics::setEnabledForTesting(false);
    ps1::emuptr_set_ram(nullptr);
  }

  // The GOOL register file is the run of uint32 slots starting at `self`.
  static uint32_t objReg(uint32_t index) {
    return kObj + offsetof(goolobj, self) + index * 4;
  }

  // A GOOL instruction: opcode in bits 31-24, operand A in 23-12, B in 11-0.
  static uint32_t ins(uint32_t opcode, uint32_t gopA = 0, uint32_t gopB = 0) {
    return (opcode << 24) | ((gopA & 0xFFF) << 12) | (gopB & 0xFFF);
  }

  // Operand referring to register-file slot `index` (the 0xE00 form). 0xE1F is
  // reserved: it is GOP_STACKTOP, the pop/push operand.
  static uint32_t regOperand(uint32_t index) { return 0xE00u | index; }
  static constexpr uint32_t kStackTop = 0xE1Fu;

  void bootObject() {
    mem.write32(kObj + offsetof(goolobj, pc), kCode);
    mem.write32(kObj + offsetof(goolobj, sp), kStack);
    mem.write32(kObj + offsetof(goolobj, fp), 0);
  }

  // Every test here terminates on an unported opcode, which warns on stderr.
  // Swallow it so the suite output stays clean.  Not asserted on: the warning
  // is warn-once per opcode per *process*, so any expectation about its text
  // would pass on the first run and fail under --gtest_repeat.  The
  // `gool.opcode_missing.XX` counter is the durable contract for that path,
  // and it is asserted below.  Each test still uses a distinct opcode so the
  // counter names never collide.
  void interpret() {
    ctx.r[4] = kObj; // obj
    ctx.r[5] = 0;    // flags
    ctx.r[6] = 0;    // transition
    testing::internal::CaptureStderr();
    ps1::psyq::hle_gool_InterpretObject(&ctx);
    (void)testing::internal::GetCapturedStderr();
  }
};

// Arithmetic over the register file and the object stack, terminated by an
// opcode the port does not implement. Pins operand decode (register-file refs
// and the GOP_STACKTOP pop/push form), the push/pop cursor in `goolobj::sp`,
// the `pc` advance, the abort return code, and the metric wiring -- all
// through ps1::Memory, which is what the shim routes every access through.
TEST_F(GoolTest, InterpretsArithmeticThenAbortsOnUnportedOpcode) {
  bootObject();
  mem.write32(objReg(2), 7);
  mem.write32(objReg(3), 5);
  mem.write32(objReg(4), 20);

  // 0x00 add: push(regB + regA) = 5 + 7 = 12
  mem.write32(kCode + 0, ins(0x00, regOperand(2), regOperand(3)));
  // 0x01 sub: A pops the 12 just pushed, B is reg4 -> push(20 - 12) = 8
  mem.write32(kCode + 4, ins(0x01, kStackTop, regOperand(4)));
  // 0x13 is not in the ported slice; it aborts this interpretation.
  mem.write32(kCode + 8, ins(0x13));

  interpret();

  EXPECT_EQ(ctx.r[2], kErrInvalid);
  EXPECT_EQ(mem.read32(kStack), 8u) << "stack top should hold 20 - (5 + 7)";
  EXPECT_EQ(mem.read32(kObj + offsetof(goolobj, sp)), kStack + 4)
      << "one net push";
  EXPECT_EQ(mem.read32(kObj + offsetof(goolobj, pc)), kCode + 12)
      << "pc must advance past all three instructions";

  EXPECT_EQ(ps1::metrics::get("gool.interpret"), 1u);
  EXPECT_EQ(ps1::metrics::get("gool.opcode_missing.13"), 1u);
}

// The whole point of the shim is that everything the VM does not implement
// stays native code reached through EMU_Invoke -> recomp_dispatch. Opcode 0x21
// (angular distance) is the cheapest opcode that makes such a callout: it must
// arrive at the right address with the operands in A0/A1, and its V0 must come
// back as the pushed value.
TEST_F(GoolTest, NativeCalloutReachesRecompDispatchAndReturnsThroughV0) {
  bootObject();
  mem.write32(objReg(2), 0x11110000);
  mem.write32(objReg(3), 0x22220000);

  mem.write32(kCode + 0, ins(0x21, regOperand(2), regOperand(3)));
  mem.write32(kCode + 4, ins(0x22)); // terminate (unported, own warning)

  g_testRecompDispatchHook = [](recomp_context *c, uint32_t addr) {
    ++g_callout.calls;
    g_callout.addr = addr;
    g_callout.a0 = c->r[4];
    g_callout.a1 = c->r[5];
    c->r[2] = 0xABCD1234u; // V0
  };

  interpret();

  EXPECT_EQ(g_callout.calls, 1);
  EXPECT_EQ(g_callout.addr, kFnAngDist);
  // case 0x21 calls EMU_Invoke(FN_ANG_DIST, 2, b, a) -- B first.
  EXPECT_EQ(g_callout.a0, 0x22220000u);
  EXPECT_EQ(g_callout.a1, 0x11110000u);
  EXPECT_EQ(mem.read32(kStack), 0xABCD1234u)
      << "the callout's V0 is what gets pushed";
}

// EMU_Invoke reserves outgoing-argument stack space and clobbers RA to make
// the call; the interpreter's own prologue spills 320 bytes. Both have to
// unwind, or the recompiled caller returns into the wrong frame.
TEST_F(GoolTest, InterpretationRestoresStackPointerAndReturnAddress) {
  bootObject();
  mem.write32(kCode + 0, ins(0x21, regOperand(2), regOperand(3)));
  mem.write32(kCode + 4, ins(0x25)); // terminate (unported, own warning)
  g_testRecompDispatchHook = [](recomp_context *c, uint32_t) { c->r[2] = 0; };

  interpret();

  EXPECT_EQ(ctx.r[29], kSp);
  EXPECT_EQ(ctx.r[31], kRaSentinel);
}

// --- EMU_SDivide --------------------------------------------------------
//
// R3000A `div` raises no exception on a divide error, it writes fixed garbage.
// psx-spx tabulates all three cases (docs/cpuspecifications.md:363-369):
//   div  0..+7FFFFFFFh   0   -->  HI = Rs, LO = -1
//   div  -80000000h..-1  0   -->  HI = Rs, LO = +1
//   div  -80000000h     -1   -->  HI = 0,  LO = -80000000h
// The last one cannot be delegated to C++ `/`: it is signed-overflow UB and
// raises SIGFPE on x86, killing the process.

class EmuDivideTest : public ::testing::Test {
protected:
  ps1::Memory mem;
  recomp_context ctx;

  void SetUp() override {
    mem.reset();
    ctx.reset();
    ctx.mem = &mem;
    ps1::emuptr_set_ram(mem.ramPtr());
    ps1::gool::emu_bind(static_cast<uint8_t *>(
                            ps1::emuptr_translate(0x80000000u)),
                        &ctx);
  }
  void TearDown() override {
    ps1::gool::emu_unbind();
    ps1::emuptr_set_ram(nullptr);
  }
};

TEST_F(EmuDivideTest, SignedMinDividedByMinusOneDoesNotTrap) {
  ps1::gool::EMU_SDivide(INT32_MIN, -1);
  EXPECT_EQ(ctx.lo, 0x80000000u);
  EXPECT_EQ(ctx.hi, 0u);
}

TEST_F(EmuDivideTest, SignedDivideByZeroSplitsOnDividendSign) {
  ps1::gool::EMU_SDivide(1234, 0);
  EXPECT_EQ(ctx.hi, 1234u);
  EXPECT_EQ(ctx.lo, 0xFFFFFFFFu) << "non-negative dividend -> LO = -1";

  ps1::gool::EMU_SDivide(-1234, 0);
  EXPECT_EQ(ctx.hi, static_cast<uint32_t>(-1234));
  EXPECT_EQ(ctx.lo, 1u) << "negative dividend -> LO = +1";
}

TEST_F(EmuDivideTest, OrdinarySignedDivideIsUnchanged) {
  ps1::gool::EMU_SDivide(-7, 2);
  EXPECT_EQ(static_cast<int32_t>(ctx.lo), -3);
  EXPECT_EQ(static_cast<int32_t>(ctx.hi), -1);
}

// divu has no overflow case, only the zero divisor (psx-spx, same table:
// HI = Rs, LO = FFFFFFFFh). Pinned so the sign split above is not copied here.
TEST_F(EmuDivideTest, UnsignedDivideByZeroReturnsAllOnes) {
  ps1::gool::EMU_UDivide(1234u, 0u);
  EXPECT_EQ(ctx.hi, 1234u);
  EXPECT_EQ(ctx.lo, 0xFFFFFFFFu);
}

} // namespace
