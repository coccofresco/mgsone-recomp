#include <gtest/gtest.h>

#include "runtime/cpu_context.h"
#include "runtime/gte.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include "runtime/mips_construction_lineage.h"
#include "runtime/mips_interpreter.h"
#include "runtime/mips_value_flow.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

extern void (*g_testRecompDispatchHook)(recomp_context *ctx, uint32_t addr);

namespace {

class DynamicMipsTest : public ::testing::Test {
protected:
  void SetUp() override {
    ctx.reset();
    ctx.mem = &memory;
    ctx.bios = nullptr;
    ps1::mips::configureDynamicInterpreter(true, 0x80010000u, 0x80030000u);
    ps1::mips::resetValueFlowForTesting();
    ps1::mips::resetConstructionLineageForTesting();
    ps1::metrics::setTraceFrame(0);
    g_testRecompDispatchHook = nullptr;
  }

  void TearDown() override {
    g_testRecompDispatchHook = nullptr;
    ps1::mips::resetConstructionLineageForTesting();
    ps1::mips::resetValueFlowForTesting();
    ps1::mips::configureDynamicInterpreter(false, 0, 0);
    ps1::metrics::setTraceFrame(0);
  }

  ps1::Memory memory;
  recomp_context ctx{};
};

TEST_F(DynamicMipsTest, ExecutesBranchAndReturnDelaySlots) {
  constexpr uint32_t base = 0x80010000u;
  memory.write32(base + 0x00, 0x24020001u); // addiu v0, zero, 1
  memory.write32(base + 0x04, 0x10000001u); // beq zero, zero, +1
  memory.write32(base + 0x08, 0x24420002u); // addiu v0, v0, 2 (delay)
  memory.write32(base + 0x0C, 0x24420004u); // addiu v0, v0, 4
  memory.write32(base + 0x10, 0x03E00008u); // jr ra
  memory.write32(base + 0x14, 0x24420008u); // addiu v0, v0, 8 (delay)
  ctx.r[31] = 0x80001234u;

  EXPECT_TRUE(ps1::mips::interpretDynamicCall(memory.ramPtr(), &ctx, base));
  EXPECT_EQ(ctx.r[2], 15u);
  EXPECT_EQ(ctx.r[31], 0x80001234u);
}

TEST_F(DynamicMipsTest, CrashSxyPlusThreePreservesProjectedXyz) {
  constexpr uint32_t base = 0x8001E000u;
  constexpr uint32_t destination = 0x80019000u;
  constexpr uint32_t packed = (170u << 16) | 260u;
  constexpr uint32_t adjusted = (170u << 16) | 263u;
  const uint32_t code[] = {
      0x48196000u, // mfc2 r25,SXY0
      0x3C18FFFFu, // lui r24,0xffff
      0x3324FFFFu, // andi r4,r25,0xffff
      0x0338C824u, // and r25,r25,r24
      0x24840003u, // addiu r4,r4,+3
      0x3084FFFFu, // andi r4,r4,0xffff
      0x0324C825u, // or r25,r25,r4
      0x48996000u, // mtc2 r25,SXY0
      0xEAEC0008u, // swc2 SXY0,8(r23)
      0x03E00008u, // jr ra
      0x00000000u, // nop (delay)
  };
  for (std::size_t i = 0; i < std::size(code); ++i)
    memory.write32(base + static_cast<uint32_t>(i * 4), code[i]);
  ctx.cop2d[ps1::GTE_SXY0] = packed;
  ctx.pgxpGteData[ps1::GTE_SXY0] =
      ps1::PGXPValue{260.25f,
                     169.75f,
                     1000.0f,
                     packed,
                     ps1::PGXPValue::VALID_XY | ps1::PGXPValue::VALID_Z,
                     ps1::PGXPOrigin::GteProjection};
  ctx.r[23] = destination;
  ctx.r[31] = 0x80001234u;

  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, base, base,
                                             base + 0x100u));
  EXPECT_EQ(ctx.cop2d[ps1::GTE_SXY0], adjusted);
  EXPECT_EQ(memory.read32(destination + 8u), adjusted);
  ps1::PGXPValue stored;
  ASSERT_TRUE(memory.lookupPrecision(destination + 8u, adjusted, stored));
  EXPECT_TRUE(stored.hasXYZ(adjusted));
  EXPECT_FLOAT_EQ(stored.x, 263.25f);
  EXPECT_FLOAT_EQ(stored.y, 169.75f);
  EXPECT_FLOAT_EQ(stored.z, 1000.0f);
}

TEST_F(DynamicMipsTest, HandlesCrashGoolJalrReturnConvention) {
  constexpr uint32_t base = 0x80011000u;
  memory.write32(base + 0x00, 0x00000000u); // nop
  memory.write32(base + 0x04, 0x03E0A809u); // jalr s5, ra
  memory.write32(base + 0x08, 0x00000000u); // nop (delay)
  ctx.r[31] = 0x80039A54u;

  EXPECT_TRUE(ps1::mips::interpretDynamicCall(memory.ramPtr(), &ctx, base));
  EXPECT_EQ(ctx.r[21], base + 0x0Cu);
  EXPECT_EQ(ctx.r[31], 0x80039A54u);
}

uint32_t g_observedTarget = 0;
uint32_t g_observedA0 = 0;

void compiledTarget(recomp_context *ctx, uint32_t address) {
  g_observedTarget = address;
  g_observedA0 = ctx->r[4];
  ctx->r[2] = 0x55u;
}

TEST_F(DynamicMipsTest, ExternalTailJumpIsDeferredToDispatcherTrampoline) {
  constexpr uint32_t base = 0x80011800u;
  constexpr uint32_t target = 0x80020000u;
  memory.write32(base + 0x00, 0x08008000u); // j 0x80020000
  memory.write32(base + 0x04, 0x24040007u); // addiu a0, zero, 7 (delay)
  ctx.r[31] = 0x80001234u;
  g_observedTarget = 0;
  g_testRecompDispatchHook = &compiledTarget;

  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, base, base,
                                             base + 0x100u));
  // The interpreter must not recurse into the generated dispatcher itself.
  EXPECT_EQ(g_observedTarget, 0u);
  EXPECT_EQ(ctx.r[4], 7u);
  uint32_t deferred = 0;
  EXPECT_TRUE(ps1::mips::takeDeferredTailDispatch(deferred));
  EXPECT_EQ(deferred, target);
  EXPECT_FALSE(ps1::mips::takeDeferredTailDispatch(deferred));
}

uint32_t g_budgetFrameCalls = 0;

void advanceBudgetFrameMidLoop(recomp_context *, uint32_t) {
  ++g_budgetFrameCalls;
  if (g_budgetFrameCalls == 150'000u)
    ps1::metrics::setTraceFrame(78);
}

TEST_F(DynamicMipsTest, CallsBackIntoStaticallyRecompiledCode) {
  constexpr uint32_t base = 0x80012000u;
  memory.write32(base + 0x00, 0x03E08021u); // addu s0, ra, zero
  memory.write32(base + 0x04, 0x0C008000u); // jal 0x80020000
  memory.write32(base + 0x08, 0x24040007u); // addiu a0, zero, 7 (delay)
  memory.write32(base + 0x0C, 0x00401821u); // addu v1, v0, zero
  memory.write32(base + 0x10, 0x0200F821u); // addu ra, s0, zero
  memory.write32(base + 0x14, 0x03E00008u); // jr ra
  memory.write32(base + 0x18, 0x00000000u); // nop (delay)
  ctx.r[31] = 0x80001234u;
  g_observedTarget = 0;
  g_observedA0 = 0;
  g_testRecompDispatchHook = &compiledTarget;

  EXPECT_TRUE(ps1::mips::interpretDynamicCall(memory.ramPtr(), &ctx, base));
  EXPECT_EQ(g_observedTarget, 0x80020000u);
  EXPECT_EQ(g_observedA0, 7u);
  EXPECT_EQ(ctx.r[3], 0x55u);
  EXPECT_EQ(ctx.r[31], 0x80001234u);
}

TEST_F(DynamicMipsTest, PersistentInterpreterBudgetRenewsAtGuestFrameBoundary) {
  constexpr uint32_t base = 0x80012800u;
  const uint32_t code[] = {
      0x03E08021u, // addu s0, ra, zero
      0x3C080005u, // lui t0, 5
      0x25086C20u, // addiu t0, t0, 0x6c20 -> 355,360 iterations
      0x0C008000u, // jal 0x80020000
      0x2508FFFFu, // addiu t0, t0, -1 (delay)
      0x1500FFFDu, // bne t0, zero, base+0x0c
      0x00000000u, // nop (delay)
      0x0200F821u, // addu ra, s0, zero
      0x03E00008u, // jr ra
      0x00000000u, // nop (delay)
  };
  for (std::size_t i = 0; i < std::size(code); ++i)
    memory.write32(base + static_cast<uint32_t>(i * 4), code[i]);
  ctx.r[31] = 0x80001234u;
  g_budgetFrameCalls = 0;
  ps1::metrics::setTraceFrame(77);
  g_testRecompDispatchHook = &advanceBudgetFrameMidLoop;

  // The complete call exceeds the one-million-instruction runaway limit,
  // while each side of the architectural frame boundary remains below it.
  EXPECT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, base, base,
                                             base + 0x100u));
  EXPECT_EQ(g_budgetFrameCalls, 355'360u);
  EXPECT_EQ(ctx.r[31], 0x80001234u);
}

TEST_F(DynamicMipsTest, StaticIslandKeepsPrivateCallsAndReturnsLocal) {
  constexpr uint32_t base = 0x80014000u;
  memory.write32(base + 0x00, 0x03E08021u); // addu s0, ra, zero
  memory.write32(base + 0x04, 0x0C005008u); // jal base+0x20
  memory.write32(base + 0x08, 0x24040007u); // addiu a0, zero, 7 (delay)
  memory.write32(base + 0x0C, 0x24430001u); // addiu v1, v0, 1
  memory.write32(base + 0x10, 0x0200F821u); // addu ra, s0, zero
  memory.write32(base + 0x14, 0x03E00008u); // jr ra
  memory.write32(base + 0x18, 0x00000000u); // nop (delay)
  memory.write32(base + 0x1C, 0x00000000u); // private padding
  memory.write32(base + 0x20, 0x24020055u); // addiu v0, zero, 0x55
  memory.write32(base + 0x24, 0x03E00008u); // jr ra -> base+0x0c
  memory.write32(base + 0x28, 0x00000000u); // nop (delay)
  ctx.r[31] = 0x80001234u;

  EXPECT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, base, base,
                                             base + 0x100u));
  EXPECT_EQ(ctx.r[2], 0x55u);
  EXPECT_EQ(ctx.r[3], 0x56u);
  EXPECT_EQ(ctx.r[4], 7u);
  EXPECT_EQ(ctx.r[31], 0x80001234u);
}

TEST_F(DynamicMipsTest, StaticIslandTraceRecordsNativeBoundary) {
  constexpr uint32_t base = 0x80014800u;
  constexpr const char *tracePath = "/tmp/ps1Runtime_static_island_entry.jsonl";
  std::remove(tracePath);
  setenv("PS1_TRACE_JSONL", tracePath, 1);
  setenv("PS1_TRACE_EVENTS", "mips.static_entry", 1);
  ps1::metrics::resetTraceForTesting();
  memory.write32(base + 0x00, 0x03E00008u); // jr ra
  memory.write32(base + 0x04, 0x00000000u); // nop (delay)
  ctx.r[29] = 0x8001FFF0u;
  ctx.r[31] = 0x80001234u;

  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, base, base,
                                             base + 0x100u));

  std::ifstream in(tracePath);
  ASSERT_TRUE(in.good());
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string body = buffer.str();
  EXPECT_NE(body.find("\"event\":\"mips.static_entry\""), std::string::npos);
  EXPECT_NE(body.find("\"entry\":" + std::to_string(base)), std::string::npos);
  EXPECT_NE(body.find("\"top_level\":1"), std::string::npos);
  EXPECT_NE(body.find("\"ra\":" + std::to_string(ctx.r[31])),
            std::string::npos);
  EXPECT_NE(body.find("\"sp\":" + std::to_string(ctx.r[29])),
            std::string::npos);

  ps1::metrics::resetTraceForTesting();
  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_EVENTS");
  std::remove(tracePath);
}

TEST_F(DynamicMipsTest, StaticIslandHonorsSyntheticRaContinuation) {
  constexpr uint32_t base = 0x80015000u;
  memory.write32(base + 0x00, 0x03E08021u); // addu s0, ra, zero
  memory.write32(base + 0x04, 0x3C1F8001u); // lui ra, 0x8001
  memory.write32(base + 0x08, 0x27FF5018u); // addiu ra, ra, 0x5018
  memory.write32(base + 0x0C, 0x10000004u); // b base+0x20
  memory.write32(base + 0x10, 0x00000000u); // nop (delay)
  memory.write32(base + 0x14, 0x00000000u); // padding
  memory.write32(base + 0x18, 0x0200F821u); // addu ra, s0, zero
  memory.write32(base + 0x1C, 0x03E00008u); // jr ra
  memory.write32(base + 0x20, 0x2402002Au); // addiu v0, zero, 42
  memory.write32(base + 0x24, 0x03E00008u); // jr ra -> base+0x18
  memory.write32(base + 0x28, 0x00000000u); // nop (delay)
  ctx.r[31] = 0x80001234u;

  EXPECT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, base, base,
                                             base + 0x100u));
  EXPECT_EQ(ctx.r[2], 42u);
  EXPECT_EQ(ctx.r[31], 0x80001234u);
}

TEST_F(DynamicMipsTest, StaticPrivateEntryReturnsToCompiledCaller) {
  constexpr uint32_t owner = 0x80016000u;
  constexpr uint32_t entry = owner + 0x20u;
  // The public function body is irrelevant here: an indirect callback points
  // directly at a valid private label later in its analyzer-owned range.
  memory.write32(entry + 0x00, 0x24021234u); // addiu v0, zero, 0x1234
  memory.write32(entry + 0x04, 0x03E00008u); // jr ra
  memory.write32(entry + 0x08, 0x24420001u); // addiu v0, v0, 1 (delay)
  ctx.r[31] = 0x80001234u;

  EXPECT_TRUE(ps1::mips::interpretStaticPrivateCall(
      memory.ramPtr(), &ctx, entry, owner, owner + 0x80u));
  EXPECT_EQ(ctx.r[2], 0x1235u);
  EXPECT_EQ(ctx.r[31], 0x80001234u);
}

TEST_F(DynamicMipsTest, TracesExactMemoryMultiplyShiftIntoGteControl) {
  constexpr uint32_t base = 0x80018000u;
  constexpr uint32_t loadedAddress = 0x80010200u;
  const uint32_t code[] = {
      0x3C088001u, // lui t0,0x8001
      0x8D090200u, // lw t1,0x200(t0)
      0x240A00C0u, // addiu t2,zero,0xc0
      0x012A0018u, // mult t1,t2
      0x00005812u, // mflo t3
      0x000B5A03u, // sra t3,t3,8
      0x48CB2800u, // ctc2 t3,TRX
      0x03E00008u, // jr ra
      0x00000000u, // nop (delay)
  };
  for (std::size_t i = 0; i < std::size(code); ++i)
    memory.write32(base + static_cast<uint32_t>(i * 4), code[i]);
  constexpr uint32_t secondEntry = base + 0x40u;
  memory.write32(secondEntry, 0x03E00008u);     // jr ra
  memory.write32(secondEntry + 4, 0x00000000u); // nop (delay)
  memory.write32(loadedAddress, 100u);
  ctx.r[31] = 0x80001234u;
  ps1::mips::setValueFlowEnabledForTesting(true);

  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, base, base,
                                             base + 0x100u));
  EXPECT_EQ(ctx.cop2c[ps1::GTE_TRX], 75u);
  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, secondEntry,
                                             base, base + 0x100u));

  const ps1::mips::GteProjectionValueFlow projection =
      ps1::mips::captureGteProjectionValueFlow(&ctx, 0, false);
  ps1::mips::MipsValueExpression expression;
  ASSERT_TRUE(ps1::mips::inspectMipsValueExpression(
      projection.transform[ps1::GTE_TRX], expression));
  EXPECT_EQ(expression.operation,
            ps1::mips::MipsValueOperation::GteControlWrite);
  EXPECT_TRUE(expression.complete);

  ASSERT_TRUE(
      ps1::mips::inspectMipsValueExpression(expression.lhs, expression));
  EXPECT_EQ(expression.operation,
            ps1::mips::MipsValueOperation::ShiftRightArithmetic);
  EXPECT_EQ(expression.immediate, 8);
  ASSERT_TRUE(
      ps1::mips::inspectMipsValueExpression(expression.lhs, expression));
  EXPECT_EQ(expression.operation,
            ps1::mips::MipsValueOperation::MultiplySignedLow);

  ps1::mips::MipsValueExpression left;
  ps1::mips::MipsValueExpression right;
  ASSERT_TRUE(ps1::mips::inspectMipsValueExpression(expression.lhs, left));
  ASSERT_TRUE(ps1::mips::inspectMipsValueExpression(expression.rhs, right));
  const ps1::mips::MipsValueExpression &constant =
      left.operation == ps1::mips::MipsValueOperation::Constant ? left : right;
  const ps1::mips::MipsValueExpression &loaded =
      left.operation == ps1::mips::MipsValueOperation::MemoryLoad ? left
                                                                  : right;
  EXPECT_EQ(constant.operation, ps1::mips::MipsValueOperation::Constant);
  EXPECT_EQ(constant.value, 0xc0u);
  EXPECT_EQ(loaded.operation, ps1::mips::MipsValueOperation::MemoryLoad);
  EXPECT_EQ(loaded.address, loadedAddress);
  EXPECT_TRUE(loaded.complete);
}

TEST_F(DynamicMipsTest, TracesMvmvaResultBackToExactInputs) {
  constexpr uint32_t base = 0x80018100u;
  const uint32_t code[] = {
      0x24081000u, // addiu t0,zero,0x1000
      0x48C80000u, // ctc2 t0,RT11RT12
      0x48C81000u, // ctc2 t0,RT22RT23
      0x48C82000u, // ctc2 t0,RT33
      0x48C00800u, // ctc2 zero,RT13RT21
      0x48C01800u, // ctc2 zero,RT31RT32
      0x48800000u, // mtc2 zero,VXY0
      0x48800800u, // mtc2 zero,VZ0
      0x4A086012u, // mvmva mx=rotation,mv=V0,tv=none,sf=1
      0x48094800u, // mfc2 t1,IR1
      0x48C92800u, // ctc2 t1,TRX
      0x03E00008u, // jr ra
      0x00000000u, // nop (delay)
  };
  for (std::size_t i = 0; i < std::size(code); ++i)
    memory.write32(base + static_cast<uint32_t>(i * 4), code[i]);
  ctx.r[31] = 0x80001234u;
  ps1::mips::setValueFlowEnabledForTesting(true);

  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, base, base,
                                             base + 0x100u));
  const ps1::mips::GteProjectionValueFlow projection =
      ps1::mips::captureGteProjectionValueFlow(&ctx, 0, false);
  ps1::mips::MipsValueExpression expression;
  ASSERT_TRUE(ps1::mips::inspectMipsValueExpression(
      projection.transform[ps1::GTE_TRX], expression));
  ASSERT_EQ(expression.operation,
            ps1::mips::MipsValueOperation::GteControlWrite);
  ASSERT_TRUE(
      ps1::mips::inspectMipsValueExpression(expression.lhs, expression));
  EXPECT_EQ(expression.operation,
            ps1::mips::MipsValueOperation::GteCommandResult);
  EXPECT_EQ(expression.pc, base + 0x20u);
  EXPECT_TRUE(expression.complete);
  ASSERT_TRUE(
      ps1::mips::inspectMipsValueExpression(expression.lhs, expression));
  EXPECT_EQ(expression.operation,
            ps1::mips::MipsValueOperation::GteCommandInput);
  EXPECT_TRUE(expression.complete);
}

TEST_F(DynamicMipsTest, TracesUnalignedLoadWithoutSecondMemoryRead) {
  constexpr uint32_t base = 0x80018200u;
  constexpr uint32_t source = 0x80010201u;
  const uint32_t code[] = {
      0x3C088001u, // lui t0,0x8001
      0x35080201u, // ori t0,t0,0x201
      0x89090003u, // lwl t1,3(t0)
      0x99090000u, // lwr t1,0(t0)
      0x48890000u, // mtc2 t1,VXY0
      0x03E00008u, // jr ra
      0x00000000u, // nop (delay)
  };
  for (std::size_t i = 0; i < std::size(code); ++i)
    memory.write32(base + static_cast<uint32_t>(i * 4), code[i]);
  memory.write32(source & ~3u, 0x44332211u);
  memory.write32((source & ~3u) + 4u, 0x88776655u);
  ctx.r[31] = 0x80001234u;
  ps1::mips::setValueFlowEnabledForTesting(true);

  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, base, base,
                                             base + 0x100u));
  EXPECT_EQ(ctx.cop2d[ps1::GTE_VXY0], 0x55443322u);
  const ps1::mips::GteProjectionValueFlow projection =
      ps1::mips::captureGteProjectionValueFlow(&ctx, 0, false);
  ps1::mips::MipsValueExpression expression;
  ASSERT_TRUE(
      ps1::mips::inspectMipsValueExpression(projection.modelXy, expression));
  EXPECT_EQ(expression.operation, ps1::mips::MipsValueOperation::GteDataWrite);
  EXPECT_TRUE(expression.complete);
  ASSERT_TRUE(
      ps1::mips::inspectMipsValueExpression(expression.lhs, expression));
  EXPECT_EQ(expression.operation, ps1::mips::MipsValueOperation::LoadWordRight);
  EXPECT_TRUE(expression.complete);
}

TEST_F(DynamicMipsTest,
       ModelFingerprintsSeparateDynamicSourcesFromProgramPath) {
  constexpr uint32_t codeBase = 0x80018300u;
  constexpr uint32_t sourceA = 0x80010400u;
  constexpr uint32_t sourceB = 0x80010500u;
  const uint32_t code[] = {
      0x8C890000u, // lw t1,0(a0)
      0x8C8A0004u, // lw t2,4(a0)
      0x48890000u, // mtc2 t1,VXY0
      0x488A0800u, // mtc2 t2,VZ0
      0x03E00008u, // jr ra
      0x00000000u, // nop (delay)
  };
  for (std::size_t i = 0; i < std::size(code); ++i)
    memory.write32(codeBase + static_cast<uint32_t>(i * 4), code[i]);
  memory.write32(sourceA, 0x00020001u);
  memory.write32(sourceA + 4u, 3u);
  memory.write32(sourceB, 0x00090008u);
  memory.write32(sourceB + 4u, 10u);
  ctx.r[31] = 0x80001234u;
  ps1::mips::setValueFlowEnabledForTesting(true);

  ctx.r[4] = sourceA;
  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, codeBase,
                                             codeBase, codeBase + 0x100u));
  const ps1::mips::GteProjectionValueFlow first =
      ps1::mips::captureGteProjectionValueFlow(&ctx, 0, false);
  const ps1::mips::GteProjectionConstructionLineage firstCompact =
      ps1::mips::captureGteProjectionConstructionLineage(&ctx, 0);
  ASSERT_TRUE(first.modelFingerprintComplete);
  ASSERT_TRUE(firstCompact.complete);
  EXPECT_NE(first.modelSourceFingerprint, 0u);
  EXPECT_NE(first.modelProgramFingerprint, 0u);
  EXPECT_EQ(firstCompact.modelSourceFingerprint, first.modelSourceFingerprint);
  EXPECT_EQ(firstCompact.modelProgramFingerprint,
            first.modelProgramFingerprint);

  ctx.r[4] = sourceB;
  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, codeBase,
                                             codeBase, codeBase + 0x100u));
  const ps1::mips::GteProjectionValueFlow second =
      ps1::mips::captureGteProjectionValueFlow(&ctx, 0, false);
  const ps1::mips::GteProjectionConstructionLineage secondCompact =
      ps1::mips::captureGteProjectionConstructionLineage(&ctx, 0);
  ASSERT_TRUE(second.modelFingerprintComplete);
  ASSERT_TRUE(secondCompact.complete);
  EXPECT_EQ(secondCompact.modelSourceFingerprint,
            second.modelSourceFingerprint);
  EXPECT_EQ(secondCompact.modelProgramFingerprint,
            second.modelProgramFingerprint);
  EXPECT_NE(second.modelSourceFingerprint, first.modelSourceFingerprint);
  EXPECT_EQ(second.modelProgramFingerprint, first.modelProgramFingerprint);
}

TEST_F(DynamicMipsTest, CompactConstructionLineageDoesNotRequireFullDag) {
  constexpr uint32_t codeBase = 0x80018400u;
  constexpr uint32_t source = 0x80010600u;
  const uint32_t code[] = {
      0x8C890000u, // lw t1,0(a0)
      0x8C8A0004u, // lw t2,4(a0)
      0x48890000u, // mtc2 t1,VXY0
      0x488A0800u, // mtc2 t2,VZ0
      0x03E00008u, // jr ra
      0x00000000u, // nop (delay)
  };
  for (std::size_t i = 0; i < std::size(code); ++i)
    memory.write32(codeBase + static_cast<uint32_t>(i * 4), code[i]);
  memory.write32(source, 0x00020001u);
  memory.write32(source + 4u, 3u);
  ctx.r[4] = source;
  ctx.r[31] = 0x80001234u;
  ps1::mips::setValueFlowEnabledForTesting(false);
  ps1::mips::setConstructionLineageEnabledForTesting(true);

  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, codeBase,
                                             codeBase, codeBase + 0x100u));
  EXPECT_FALSE(ps1::mips::valueFlowEnabled());
  const ps1::mips::GteProjectionValueFlow full =
      ps1::mips::captureGteProjectionValueFlow(&ctx, 0, false);
  const ps1::mips::GteProjectionConstructionLineage compact =
      ps1::mips::captureGteProjectionConstructionLineage(&ctx, 0);
  EXPECT_FALSE(full.modelFingerprintComplete);
  ASSERT_TRUE(compact.complete);
  EXPECT_NE(compact.modelSourceFingerprint, 0u);
  EXPECT_NE(compact.modelProgramFingerprint, 0u);
}

TEST_F(DynamicMipsTest, CompactConstructionLineageUsesExactTransformAdmission) {
  constexpr uint32_t codeBase = 0x80018480u;
  constexpr uint32_t source = 0x80010680u;
  constexpr uint64_t admittedTransform = 0x123456789abcdef0ull;
  const uint32_t code[] = {
      0x8C890000u, // lw t1,0(a0)
      0x8C8A0004u, // lw t2,4(a0)
      0x48890000u, // mtc2 t1,VXY0
      0x488A0800u, // mtc2 t2,VZ0
      0x03E00008u, // jr ra
      0x00000000u, // nop (delay)
  };
  for (std::size_t i = 0; i < std::size(code); ++i)
    memory.write32(codeBase + static_cast<uint32_t>(i * 4), code[i]);
  memory.write32(source, 0x00020001u);
  memory.write32(source + 4u, 3u);
  ctx.r[4] = source;
  ctx.r[31] = 0x80001234u;
  ps1::mips::setValueFlowEnabledForTesting(false);
  ps1::mips::setConstructionLineageTransformFilter(
      std::span<const uint64_t>(&admittedTransform, 1));
  ps1::mips::setConstructionLineageEnabledForTesting(true);

  EXPECT_FALSE(ps1::mips::constructionLineageEnabled());
  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, codeBase,
                                             codeBase, codeBase + 0x80u));
  EXPECT_FALSE(
      ps1::mips::captureGteProjectionConstructionLineage(&ctx, 0).complete);

  ps1::mips::noteConstructionLineageProjectionTransform(admittedTransform);
  EXPECT_TRUE(ps1::mips::constructionLineageEnabled());
  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, codeBase,
                                             codeBase, codeBase + 0x80u));
  EXPECT_TRUE(
      ps1::mips::captureGteProjectionConstructionLineage(&ctx, 0).complete);

  ps1::mips::noteConstructionLineageProjectionTransform(admittedTransform + 1u);
  EXPECT_FALSE(ps1::mips::constructionLineageEnabled());
  EXPECT_FALSE(
      ps1::mips::captureGteProjectionConstructionLineage(&ctx, 0).complete);
}

TEST_F(DynamicMipsTest, CompactConstructionLineageStartsOnlyAtExactSourceSeed) {
  constexpr uint32_t codeBase = 0x800184c0u;
  constexpr uint32_t source = 0x800106c0u;
  constexpr uint64_t admittedTransform = 0x123456789abcdef0ull;
  const uint32_t code[] = {
      0x8C890000u, // lw t1,0(a0) -- exact admission point
      0x8C8A0004u, // lw t2,4(a0)
      0x48890000u, // mtc2 t1,VXY0
      0x488A0800u, // mtc2 t2,VZ0
      0x03E00008u, // jr ra
      0x00000000u, // nop (delay)
  };
  for (std::size_t i = 0; i < std::size(code); ++i)
    memory.write32(codeBase + static_cast<uint32_t>(i * 4), code[i]);
  memory.write32(source, 0x00020001u);
  memory.write32(source + 4u, 3u);
  ctx.r[31] = 0x80001234u;
  ps1::mips::setValueFlowEnabledForTesting(false);
  ps1::mips::setConstructionLineageTransformFilter(
      std::span<const uint64_t>(&admittedTransform, 1));
  const ps1::mips::ConstructionLineageSeed seed{codeBase, code[0],
                                                source & 0x1fffffffu, 4};
  ps1::mips::setConstructionLineageSeedFilter(
      std::span<const ps1::mips::ConstructionLineageSeed>(&seed, 1));
  ps1::mips::setConstructionLineageEnabledForTesting(true);
  ps1::mips::noteConstructionLineageProjectionTransform(admittedTransform);

  ctx.r[4] = source + 0x40u;
  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, codeBase,
                                             codeBase, codeBase + 0x80u));
  EXPECT_FALSE(
      ps1::mips::captureGteProjectionConstructionLineage(&ctx, 0).complete);

  // The prior object's transform may be different at the construction seed.
  // Source admission is provisional; the real GTE projection endpoint below
  // (represented by noteConstructionLineageProjectionTransform) remains the
  // exact fail-closed transform check.
  ps1::mips::noteConstructionLineageProjectionTransform(admittedTransform + 1u);
  EXPECT_FALSE(ps1::mips::constructionLineageEnabled());
  ctx.r[4] = source;
  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, codeBase,
                                             codeBase, codeBase + 0x80u));
  const ps1::mips::GteProjectionConstructionLineage admitted =
      ps1::mips::captureGteProjectionConstructionLineage(&ctx, 0);
  EXPECT_TRUE(admitted.complete);
  EXPECT_NE(admitted.modelSourceFingerprint, 0u);
  EXPECT_NE(admitted.modelProgramFingerprint, 0u);

  ps1::mips::noteConstructionLineageProjectionTransform(admittedTransform + 1u);
  EXPECT_FALSE(
      ps1::mips::captureGteProjectionConstructionLineage(&ctx, 0).complete);
}

TEST_F(DynamicMipsTest,
       CompactConstructionLineagePublishesExactProjectionEvidence) {
  constexpr uint32_t codeBase = 0x80018500u;
  constexpr uint32_t source = 0x80010700u;
  const uint32_t code[] = {
      0x24081000u, // addiu t0,zero,0x1000
      0x48C80000u, // ctc2 t0,RT11RT12
      0x48C00800u, // ctc2 zero,RT13RT21
      0x48C81000u, // ctc2 t0,RT22RT23
      0x48C01800u, // ctc2 zero,RT31RT32
      0x48C82000u, // ctc2 t0,RT33
      0x48C02800u, // ctc2 zero,TRX
      0x48C03000u, // ctc2 zero,TRY
      0x48C03800u, // ctc2 zero,TRZ
      0x240B0100u, // addiu t3,zero,0x100
      0x48CBD000u, // ctc2 t3,H
      0x8C890000u, // lw t1,0(a0)
      0x8C8A0004u, // lw t2,4(a0)
      0x48890000u, // mtc2 t1,VXY0
      0x488A0800u, // mtc2 t2,VZ0
      0x4A080001u, // rtps sf=1
      0x03E00008u, // jr ra
      0x00000000u, // nop (delay)
  };
  for (std::size_t i = 0; i < std::size(code); ++i)
    memory.write32(codeBase + static_cast<uint32_t>(i * 4), code[i]);
  memory.write32(source, 0x00020001u);
  memory.write32(source + 4u, 100u);
  ctx.r[4] = source;
  ctx.r[31] = 0x80001234u;
  ps1::mips::setValueFlowEnabledForTesting(false);
  ps1::mips::setConstructionLineageEnabledForTesting(true);

  ASSERT_TRUE(ps1::mips::interpretStaticCall(memory.ramPtr(), &ctx, codeBase,
                                             codeBase, codeBase + 0x100u));
  const ps1::PGXPValue &projection = ctx.pgxpGteData[ps1::GTE_SXY2];
  ASSERT_TRUE(projection.hasSignedProjection(ctx.cop2d[ps1::GTE_SXY2]));
  EXPECT_EQ(projection.flags & ps1::PGXPValue::VALID_MODEL_VALUE_FLOW,
            ps1::PGXPValue::VALID_MODEL_VALUE_FLOW);
  EXPECT_NE(projection.modelValueFlowSourceFingerprint, 0u);
  EXPECT_NE(projection.modelValueFlowProgramFingerprint, 0u);
}

TEST_F(DynamicMipsTest, ValueFlowHonorsInclusiveFreshBootFrameBounds) {
  constexpr const char *tracePath =
      "/tmp/ps1Runtime_mips_value_flow_bounds.jsonl";
  setenv("PS1_TRACE_JSONL", tracePath, 1);
  setenv("PS1_TRACE_MIPS_VALUE_FLOW", "1", 1);
  setenv("PS1_TRACE_MIPS_VALUE_FLOW_FRAME_FROM", "10", 1);
  setenv("PS1_TRACE_MIPS_VALUE_FLOW_FRAME_TO", "20", 1);
  ps1::metrics::resetTraceForTesting();
  ps1::mips::resetValueFlowForTesting();

  ps1::metrics::setTraceFrame(9);
  EXPECT_FALSE(ps1::mips::valueFlowEnabled());
  ps1::metrics::setTraceFrame(10);
  EXPECT_TRUE(ps1::mips::valueFlowEnabled());
  ps1::metrics::setTraceFrame(20);
  EXPECT_TRUE(ps1::mips::valueFlowEnabled());
  ps1::metrics::setTraceFrame(21);
  EXPECT_FALSE(ps1::mips::valueFlowEnabled());

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_MIPS_VALUE_FLOW");
  unsetenv("PS1_TRACE_MIPS_VALUE_FLOW_FRAME_FROM");
  unsetenv("PS1_TRACE_MIPS_VALUE_FLOW_FRAME_TO");
  ps1::metrics::resetTraceForTesting();
  ps1::mips::resetValueFlowForTesting();
  std::remove(tracePath);
}

TEST_F(DynamicMipsTest, ValueFlowCanArmOnSuccessfulCheckpointLoad) {
  constexpr const char *tracePath =
      "/tmp/ps1Runtime_mips_value_flow_after_load.jsonl";
  setenv("PS1_TRACE_JSONL", tracePath, 1);
  setenv("PS1_TRACE_MIPS_VALUE_FLOW", "1", 1);
  setenv("PS1_TRACE_ARM_AFTER_LOAD", "1", 1);
  ps1::metrics::resetTraceForTesting();
  ps1::mips::resetValueFlowForTesting();

  EXPECT_FALSE(ps1::mips::valueFlowEnabled());
  ps1::metrics::armGuestTraceAfterLoad();
  EXPECT_TRUE(ps1::mips::valueFlowEnabled());

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_MIPS_VALUE_FLOW");
  unsetenv("PS1_TRACE_ARM_AFTER_LOAD");
  ps1::metrics::resetTraceForTesting();
  ps1::mips::resetValueFlowForTesting();
  std::remove(tracePath);
}

TEST_F(DynamicMipsTest, RejectsDataWithoutNativeReturnSignature) {
  constexpr uint32_t base = 0x80013000u;
  memory.write32(base + 0x00, 0x80065E50u);
  memory.write32(base + 0x04, 0x80065E80u);
  ctx.r[31] = 0x80001234u;

  EXPECT_FALSE(ps1::mips::interpretDynamicCall(memory.ramPtr(), &ctx, base));
}

} // namespace
