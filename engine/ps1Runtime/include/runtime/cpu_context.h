#pragma once
/**
 * @file cpu_context.h
 * @brief PS1 R3000A CPU register state used by all recompiled functions.
 *
 * Every recompiled MIPS function receives a `recomp_context*` as its second
 * argument. This struct holds the full CPU state: 32 GPRs, HI/LO, COP0
 * (system control) and COP2 (GTE) registers.
 *
 * The `Register` and `COP0Reg` enums provide named aliases so that BIOS and
 * HLE code can reference registers by conventional MIPS ABI names (A0, V0, RA...)
 * rather than raw indices.
 */

#include <cstdint>
#include <cstring>
#include <exception>
#include <runtime/pgxp.h>

namespace ps1 {

enum class ExceptionCause : uint32_t {
  Interrupt = 0x00,
  Mod = 0x01,
  TLBL = 0x02,
  TLBS = 0x03,
  AdEL = 0x04,
  AdES = 0x05,
  IBE = 0x06,
  DBE = 0x07,
  Syscall = 0x08,
  Bp = 0x09,
  RI = 0x0A,
  CpU = 0x0B,
  Ov = 0x0C
};

struct CpuException : public std::exception {
  ExceptionCause cause;
  explicit CpuException(ExceptionCause c) : cause(c) {}
};

// Register aliases

enum Register : uint32_t {
  ZERO = 0,
  AT = 1,
  V0 = 2,
  V1 = 3,
  A0 = 4,
  A1 = 5,
  A2 = 6,
  A3 = 7,
  T0 = 8,
  T1 = 9,
  T2 = 10,
  T3 = 11,
  T4 = 12,
  T5 = 13,
  T6 = 14,
  T7 = 15,
  S0 = 16,
  S1 = 17,
  S2 = 18,
  S3 = 19,
  S4 = 20,
  S5 = 21,
  S6 = 22,
  S7 = 23,
  T8 = 24,
  T9 = 25,
  K0 = 26,
  K1 = 27,
  GP = 28,
  SP = 29,
  FP = 30, // Also S8
  RA = 31,
};

// COP0 Register indices

enum COP0Reg : uint32_t {
  COP0_BPC = 3,      // Breakpoint on execute
  COP0_BDA = 5,      // Breakpoint on data access
  COP0_JUMPDEST = 6, // Jump destination
  COP0_DCIC = 7,     // Breakpoint control
  COP0_BADVADDR = 8, // Bad virtual address
  COP0_BDAM = 9,     // Data access breakpoint mask
  COP0_BPCM = 11,    // Execute breakpoint mask
  COP0_SR = 12,      // Status Register
  COP0_CAUSE = 13,   // Cause Register
  COP0_EPC = 14,     // Exception PC
  COP0_PRID = 15,    // Processor ID
};

// CPU Context

/// PS1 R3000A CPU state -- used by recompiled code
/// Fields match what the InstructionEmitter and GteEmitter reference:
///   ctx->rN, ctx->hi, ctx->lo, ctx->cop0[N], ctx->cop2d[N], ctx->cop2c[N]
struct CPUContext {
  // General Purpose Registers (r0 = always zero)
  union {
    uint32_t r[32];
    struct {
      uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14,
          r15, r16, r17, r18, r19, r20, r21, r22, r23, r24, r25, r26, r27, r28,
          r29, r30, r31;
    };
  };

  // Program Counter
  uint32_t pc;

  // Multiply/Divide result registers
  uint32_t hi;
  uint32_t lo;

  // COP0 -- System Control Coprocessor (16 registers used on PS1)
  uint32_t cop0[16];

  // COP2 -- GTE (Geometry Transform Engine)
  uint32_t cop2d[32]; // Data registers (VXY0, IR0, RGBC, SXY0, MAC0, etc.)
  uint32_t cop2c[32]; // Control registers (RT11RT12, TRX, FLAG, etc.)

  // PGXP side channels. They never alter architectural CPU/GTE state and are
  // validated against the integer word before use.
  PGXPValue pgxpGpr[32];
  PGXPValue pgxpGteData[32];

  // Diagnostic-only provenance for exact GTE family analysis. A source token
  // is guestAddress+1, with zero reserved for an unknown/non-memory writer.
  // Generations order control writes but never affect architectural results.
  uint64_t gteDataSourceToken[32];
  uint64_t gteControlSourceToken[32];
  uint32_t gteControlWriterPc[32];
  uint64_t gteControlWriteGeneration[32];
  uint64_t gteControlWriteSerial;

  /// Reset all registers to zero
  void reset() { std::memset(this, 0, sizeof(*this)); }

  /// Enforce r0 = 0 (call after each instruction)
  void enforceR0() { r[0] = 0; }
};

} // namespace ps1

// Forward declare Memory and Bios so we can define recomp_context
namespace ps1 {
class Memory;
namespace bios {
class Bios;
}
} // namespace ps1

struct recomp_context : public ps1::CPUContext {
  ps1::Memory *mem;
  ps1::bios::Bios *bios;

  // Host-only continuation token used after a savestate replaces the native
  // C++ call stack. The recompiler consumes it at a registered MIPS call
  // continuation and chains architectural returns until execution has rebuilt
  // a live native stack. It is not part of PS1 architectural state.
  uint32_t nativeResumePc = 0;

  // Host-only one-shot selector for a configured internal entry of an already
  // compiled assembly body. Unlike nativeResumePc this is an ordinary guest
  // call and must return to the live C++ caller instead of rebuilding a stack.
  uint32_t nativeEntryPc = 0;

  // Recompiled code occasionally repurposes the architectural $sp as a data
  // cursor (Crash's renderer is one example). Guest callbacks must not be
  // entered while such a dynamic extent is active: their normal prologues
  // would treat the data cursor as a stack and overwrite guest objects.
  uint32_t callbackDrainBlockDepth = 0;
};

class recomp_callback_drain_guard {
public:
  explicit recomp_callback_drain_guard(recomp_context *ctx) : ctx_(ctx) {
    if (ctx_)
      ++ctx_->callbackDrainBlockDepth;
  }

  ~recomp_callback_drain_guard() {
    if (ctx_ && ctx_->callbackDrainBlockDepth != 0)
      --ctx_->callbackDrainBlockDepth;
  }

  recomp_callback_drain_guard(const recomp_callback_drain_guard &) = delete;
  recomp_callback_drain_guard &
  operator=(const recomp_callback_drain_guard &) = delete;

private:
  recomp_context *ctx_;
};
