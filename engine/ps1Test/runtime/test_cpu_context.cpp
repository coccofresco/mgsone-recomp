// Tests for ps1Runtime -- CPU Context
// Validates the PS1 CPU register context structure

#include <gtest/gtest.h>
#include <runtime/cpu_context.h>

using namespace ps1;

// Context Initialization

TEST(CPUContext, Reset) {
  CPUContext ctx;
  ctx.r[SP] = 0xDEADBEEF;
  ctx.pc = 0x80050000;
  ctx.hi = 0xFFFFFFFF;
  ctx.lo = 0xFFFFFFFF;

  ctx.reset();

  EXPECT_EQ(ctx.pc, 0u);
  EXPECT_EQ(ctx.hi, 0u);
  EXPECT_EQ(ctx.lo, 0u);
  for (int i = 0; i < 32; i++) {
    EXPECT_EQ(ctx.r[i], 0u) << "Register r" << i << " not zeroed";
  }
}

TEST(CPUContext, R0AlwaysZero) {
  CPUContext ctx;
  ctx.reset();
  ctx.r[ZERO] = 0x12345678;
  ctx.enforceR0();
  EXPECT_EQ(ctx.r[ZERO], 0u);
}

TEST(CPUContext, StructLayout) {
  // GPRs: 32 x 4 = 128 bytes
  EXPECT_EQ(sizeof(CPUContext::r), 32 * sizeof(uint32_t));
  // COP0: 16 x 4 = 64 bytes
  EXPECT_EQ(sizeof(CPUContext::cop0), 16 * sizeof(uint32_t));
  // COP2: 32 x 4 x 2 = 256 bytes
  EXPECT_EQ(sizeof(CPUContext::cop2d), 32 * sizeof(uint32_t));
  EXPECT_EQ(sizeof(CPUContext::cop2c), 32 * sizeof(uint32_t));
}

// Register Aliases

TEST(CPUContext, RegisterAliases) {
  EXPECT_EQ(ZERO, 0u);
  EXPECT_EQ(AT, 1u);
  EXPECT_EQ(V0, 2u);
  EXPECT_EQ(A0, 4u);
  EXPECT_EQ(T0, 8u);
  EXPECT_EQ(S0, 16u);
  EXPECT_EQ(GP, 28u);
  EXPECT_EQ(SP, 29u);
  EXPECT_EQ(FP, 30u);
  EXPECT_EQ(RA, 31u);
}

TEST(CPUContext, RegisterReadWrite) {
  CPUContext ctx;
  ctx.reset();

  // Simulate: addiu $sp, $sp, -0x20
  ctx.r[SP] = 0x801FFFF0;
  ctx.r[SP] = ctx.r[SP] + static_cast<uint32_t>(-0x20);
  EXPECT_EQ(ctx.r[SP], 0x801FFFD0u);

  // Simulate: jal -> saves return address in $ra
  ctx.r[RA] = 0x80050004;
  EXPECT_EQ(ctx.r[RA], 0x80050004u);

  // Simulate: mult result
  ctx.hi = 0x00000001;
  ctx.lo = 0x00000064;
  EXPECT_EQ(ctx.hi, 1u);
  EXPECT_EQ(ctx.lo, 100u);
}

// COP0 Registers

TEST(CPUContext, COP0Registers) {
  CPUContext ctx;
  ctx.reset();

  // Status Register
  ctx.cop0[COP0_SR] = 0x10000000;
  EXPECT_EQ(ctx.cop0[COP0_SR], 0x10000000u);

  // Cause Register
  ctx.cop0[COP0_CAUSE] = 0x00000020;
  EXPECT_EQ(ctx.cop0[COP0_CAUSE], 0x00000020u);

  // Exception PC
  ctx.cop0[COP0_EPC] = 0x80010100;
  EXPECT_EQ(ctx.cop0[COP0_EPC], 0x80010100u);

  // Processor ID (R3000A = 0x00000002)
  ctx.cop0[COP0_PRID] = 0x00000002;
  EXPECT_EQ(ctx.cop0[COP0_PRID], 0x00000002u);
}

// COP2/GTE Registers

TEST(CPUContext, GteDataRegisters) {
  CPUContext ctx;
  ctx.reset();

  // VXY0 (reg 0) -- X,Y of vertex 0
  ctx.cop2d[0] = 0x00640032; // X=100, Y=50 (packed)
  EXPECT_EQ(ctx.cop2d[0], 0x00640032u);

  // IR0 (reg 8)
  ctx.cop2d[8] = 0x00001000; // 4096 (1.0 in 12-bit fixed)
  EXPECT_EQ(ctx.cop2d[8], 0x00001000u);

  // MAC0 (reg 24)
  ctx.cop2d[24] = 0xDEADBEEF;
  EXPECT_EQ(ctx.cop2d[24], 0xDEADBEEFu);
}

TEST(CPUContext, GteControlRegisters) {
  CPUContext ctx;
  ctx.reset();

  // FLAG (reg 31) -- overflow/saturation flags
  ctx.cop2c[31] = 0x7F87E000;
  EXPECT_EQ(ctx.cop2c[31], 0x7F87E000u);

  // RT11RT12 (reg 0) -- rotation matrix
  ctx.cop2c[0] = 0x10000000;
  EXPECT_EQ(ctx.cop2c[0], 0x10000000u);

  // H (reg 26) -- projection distance
  ctx.cop2c[26] = 0x00000100;
  EXPECT_EQ(ctx.cop2c[26], 0x00000100u);
}

// PS1 Entry Point

TEST(CPUContext, PS1EntryPoint) {
  CPUContext ctx;
  ctx.reset();

  ctx.pc = 0x80010000;
  ctx.r[SP] = 0x801FFF00;
  ctx.r[GP] = 0x800A0000;

  EXPECT_EQ(ctx.pc, 0x80010000u);
  EXPECT_EQ(ctx.r[SP], 0x801FFF00u);
  EXPECT_EQ(ctx.r[GP], 0x800A0000u);
}

TEST(CPUContext, ResetClearsAllFields) {
  CPUContext ctx;
  // Fill everything with garbage
  ctx.r[V0] = 0xDEAD;
  ctx.cop0[COP0_SR] = 0xBEEF;
  ctx.cop2d[8] = 0xCAFE;
  ctx.cop2c[31] = 0xF00D;
  ctx.hi = 0x1111;
  ctx.lo = 0x2222;

  ctx.reset();

  EXPECT_EQ(ctx.r[V0], 0u);
  EXPECT_EQ(ctx.cop0[COP0_SR], 0u);
  EXPECT_EQ(ctx.cop2d[8], 0u);
  EXPECT_EQ(ctx.cop2c[31], 0u);
  EXPECT_EQ(ctx.hi, 0u);
  EXPECT_EQ(ctx.lo, 0u);
}
