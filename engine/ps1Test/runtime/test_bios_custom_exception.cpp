#include "runtime/bios/bios.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::bios;

class CustomExceptionTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx{};
  cdrom::VirtualFs fs;
  std::unique_ptr<Bios> bios;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
    bios = std::make_unique<Bios>(ctx, fs, mem);
  }

  // Lay out a PsyQ jmp_buf at `addr` and return it.  Layout:
  //   +0 RA  +4 SP  +8 FP  +12..40 S0..S7  +44 GP   (48 bytes total).
  void writeJmpBuf(uint32_t addr, uint32_t ra, uint32_t sp, uint32_t fp,
                   const uint32_t s[8], uint32_t gp) {
    mem.write32(addr + 0, ra);
    mem.write32(addr + 4, sp);
    mem.write32(addr + 8, fp);
    for (int i = 0; i < 8; i++)
      mem.write32(addr + 12 + i * 4, s[i]);
    mem.write32(addr + 44, gp);
  }
};

TEST_F(CustomExceptionTest, LongjmpEmulatorRestoresRegistersAndShiftsSR) {
  constexpr uint32_t kBuf = 0x80010000;
  const uint32_t kRA = 0x80012345;
  const uint32_t kSP = 0x801F0000;
  const uint32_t kFP = 0x801E0000;
  const uint32_t kGP = 0x80100000;
  const uint32_t kS[8] = {0xAAAAAAA0, 0xAAAAAAA1, 0xAAAAAAA2, 0xAAAAAAA3,
                          0xAAAAAAA4, 0xAAAAAAA5, 0xAAAAAAA6, 0xAAAAAAA7};
  writeJmpBuf(kBuf, kRA, kSP, kFP, kS, kGP);

  // Pre-fill ctx with garbage to prove the function actually overwrites.
  for (int i = 0; i < 32; i++) ctx.r[i] = 0xDEADBEEF;
  ctx.pc = 0xCCCCCCCC;
  ctx.cop0[COP0_CAUSE] = 0;
  ctx.cop0[COP0_SR] = 0x30; // expect shift -> (0x30 & ~0xF) | (0x30 >> 2 & 0xF)

  hle_longjmp_emulator(ctx, mem, kBuf);

  EXPECT_EQ(ctx.r[RA], kRA);
  EXPECT_EQ(ctx.r[SP], kSP);
  EXPECT_EQ(ctx.r[FP], kFP);
  for (int i = 0; i < 8; i++) EXPECT_EQ(ctx.r[S0 + i], kS[i]);
  EXPECT_EQ(ctx.r[GP], kGP);
  EXPECT_EQ(ctx.pc, kRA);
  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_EQ(ctx.cop0[COP0_CAUSE], 0x400u);
  EXPECT_EQ(ctx.cop0[COP0_SR], 0x3Cu); // (0x30 & ~0xF) | ((0x30>>2) & 0xF)
}

TEST_F(CustomExceptionTest, B019InstallsCallbackThatFiresOnTrigger) {
  constexpr uint32_t kBuf = 0x80020000;
  const uint32_t kRA = 0x80055555;
  const uint32_t kS[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  writeJmpBuf(kBuf, kRA, 0x801F1000, 0x801E1000, kS, 0x80100000);

  // Install handler via B0:0x19.
  ctx.r[A0] = kBuf;
  ctx.r[T1] = 0x19;
  bios->executeB0();

  // Pre-set V0 so we can confirm the callback wrote 1 (longjmp return).
  ctx.r[V0] = 0;
  bios->triggerCustomException();

  EXPECT_EQ(ctx.r[RA], kRA);
  EXPECT_EQ(ctx.pc, kRA);
  EXPECT_EQ(ctx.r[V0], 1u);
  for (int i = 0; i < 8; i++) EXPECT_EQ(ctx.r[S0 + i], kS[i]);
}

TEST_F(CustomExceptionTest, B019WithZeroClearsHandler) {
  constexpr uint32_t kBuf = 0x80030000;
  const uint32_t kS[8] = {0};
  writeJmpBuf(kBuf, 0x80077777, 0, 0, kS, 0);

  // Install, then clear by passing 0.
  ctx.r[A0] = kBuf;
  ctx.r[T1] = 0x19;
  bios->executeB0();
  ctx.r[A0] = 0;
  ctx.r[T1] = 0x19;
  bios->executeB0();

  // After clearing, triggering must not touch RA/PC.
  ctx.r[RA] = 0xCAFEBABE;
  ctx.pc = 0xFEEDFACE;
  bios->triggerCustomException();

  EXPECT_EQ(ctx.r[RA], 0xCAFEBABEu);
  EXPECT_EQ(ctx.pc, 0xFEEDFACEu);
}

TEST_F(CustomExceptionTest, TriggerIsNoOpWhenUnregistered) {
  ctx.r[RA] = 0x12345678;
  ctx.pc = 0x9ABCDEF0;
  ctx.r[V0] = 0x55555555;

  bios->triggerCustomException();

  EXPECT_EQ(ctx.r[RA], 0x12345678u);
  EXPECT_EQ(ctx.pc, 0x9ABCDEF0u);
  EXPECT_EQ(ctx.r[V0], 0x55555555u);
}

TEST_F(CustomExceptionTest, VBlankHleDoesNotAlsoInvokeHookEntryIntExit) {
  constexpr uint32_t kBuf = 0x80030000;
  constexpr uint32_t kHookRA = 0x80077777;
  const uint32_t kS[8] = {0};
  writeJmpBuf(kBuf, kHookRA, 0x801F1000, 0, kS, 0);

  ctx.r[A0] = kBuf;
  ctx.r[T1] = 0x19;
  bios->executeB0();

  ctx.r[RA] = 0x80012345;
  ctx.pc = 0x80023456;
  bios->triggerVBlankEvent();
  bios->drainPendingCallbacks();

  // triggerVBlankEvent is itself the HLE replacement for the interrupt
  // handler chain.  HookEntryInt is its exit route, not a second callback.
  EXPECT_EQ(ctx.r[RA], 0x80012345u);
  EXPECT_EQ(ctx.pc, 0x80023456u);
}
