// Tests for ps1Recomp -- Instruction Emitter
// Validates MIPS I -> C++ translation

#include <algorithm>
#include <gtest/gtest.h>
#include <ps1recomp/instruction_emitter.h>
#include <ps1recomp/mips_decoder.h>

using namespace ps1recomp;

// Encoding helpers (same as decoder tests)

static constexpr uint32_t encR(uint8_t op, uint8_t rs, uint8_t rt, uint8_t rd,
                               uint8_t shamt, uint8_t funct) {
  return (uint32_t(op) << 26) | (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
         (uint32_t(rd) << 11) | (uint32_t(shamt) << 6) | uint32_t(funct);
}
static constexpr uint32_t encI(uint8_t op, uint8_t rs, uint8_t rt,
                               uint16_t imm) {
  return (uint32_t(op) << 26) | (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
         uint32_t(imm);
}
static constexpr uint32_t encJ(uint8_t op, uint32_t target) {
  return (uint32_t(op) << 26) | (target & 0x03FFFFFF);
}

static InstructionEmitter makeEmitter() {
  InstructionEmitter emitter;
  emitter.setFuncResolver([](uint32_t addr) -> std::string {
    if (addr == 0x80020000)
      return "GameUpdate";
    if (addr == 0x80030000)
      return "RenderFrame";
    return "";
  });
  return emitter;
}

// ALU Tests

TEST(InstructionEmitter, ALU_ADDU) {
  auto emitter = makeEmitter();
  // ADDU $v0, $a0, $a1
  auto inst = MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x21));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("ctx->r2"), std::string::npos);
  EXPECT_NE(code.find("ctx->r4"), std::string::npos);
  EXPECT_NE(code.find("ctx->r5"), std::string::npos);
  EXPECT_NE(code.find("+"), std::string::npos);
}

TEST(InstructionEmitter, ALU_ADDIU) {
  auto emitter = makeEmitter();
  // ADDIU $sp, $sp, -32
  auto inst =
      MipsDecoder::decode(encI(0x09, 29, 29, static_cast<uint16_t>(-32)));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("pgxp_addiu(ctx, 29, 29"), std::string::npos);
  EXPECT_NE(code.find("-32"), std::string::npos);
}

TEST(InstructionEmitter, ALU_LUI) {
  auto emitter = makeEmitter();
  // LUI $v0, 0x8001
  auto inst = MipsDecoder::decode(encI(0x0F, 0, 2, 0x8001));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("ctx->r2"), std::string::npos);
  EXPECT_NE(code.find("80010000"), std::string::npos);
}

TEST(InstructionEmitter, ALU_BitwiseOps) {
  auto emitter = makeEmitter();

  // AND $v0, $a0, $a1
  auto code = emitter.emitInstruction(
      MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x24)), 0);
  EXPECT_NE(code.find("pgxp_and(ctx, 2, 4, 5)"), std::string::npos);

  // OR
  code = emitter.emitInstruction(MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x25)),
                                 0);
  EXPECT_NE(code.find("pgxp_or(ctx, 2, 4, 5)"), std::string::npos);

  // XOR
  code = emitter.emitInstruction(MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x26)),
                                 0);
  EXPECT_NE(code.find("^"), std::string::npos);
}

TEST(InstructionEmitter, CrashPackedSxyAluUsesAliasSafePgxpHelpers) {
  auto emitter = makeEmitter();
  const struct {
    uint32_t raw;
    const char *helper;
  } cases[] = {
      {0x3324FFFFu, "pgxp_andi(ctx, 4, 25"},
      {0x0338C824u, "pgxp_and(ctx, 25, 25, 24)"},
      {0x24840003u, "pgxp_addiu(ctx, 4, 4, 3)"},
      {0x0324C825u, "pgxp_or(ctx, 25, 25, 4)"},
  };

  for (const auto &testCase : cases) {
    const auto code =
        emitter.emitInstruction(MipsDecoder::decode(testCase.raw), 0x80040BA0u);
    EXPECT_NE(code.find(testCase.helper), std::string::npos) << code;
    EXPECT_EQ(code.find("pgxp_invalidate_gpr"), std::string::npos) << code;
  }
}

// Write-to-$zero Optimization

TEST(InstructionEmitter, SkipsWritesToZero) {
  auto emitter = makeEmitter();

  // ADDU $zero, $a0, $a1 -- should be optimized away
  auto inst = MipsDecoder::decode(encR(0, 4, 5, 0, 0, 0x21));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("NOP"), std::string::npos);

  // LW $zero, 0($sp) -- load into $zero, optimized away
  inst = MipsDecoder::decode(encI(0x23, 29, 0, 0));
  code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("NOP"), std::string::npos);
}

// Shift Tests

TEST(InstructionEmitter, Shifts) {
  auto emitter = makeEmitter();

  // SLL $v0, $a0, 4
  auto code = emitter.emitInstruction(
      MipsDecoder::decode(encR(0, 0, 4, 2, 4, 0x00)), 0);
  EXPECT_NE(code.find("<<"), std::string::npos);
  EXPECT_NE(code.find("4"), std::string::npos);

  // SRL $v0, $a0, 5
  code = emitter.emitInstruction(MipsDecoder::decode(encR(0, 0, 4, 2, 5, 0x02)),
                                 0);
  EXPECT_NE(code.find(">>"), std::string::npos);
}

TEST(InstructionEmitter, ExactInstructionOverrideLeavesOtherPcsNative) {
  auto emitter = makeEmitter();
  emitter.setInstructionOverrides(
      {{0x8003E168u, "ctx->r4 = profile_hook(ctx->r4);"}});
  const auto instruction =
      MipsDecoder::decode(encR(0, 0, 4, 6, 16, 0x00));

  EXPECT_EQ(emitter.emitInstruction(instruction, 0x8003E168u),
            "ctx->r4 = profile_hook(ctx->r4);");
  EXPECT_NE(emitter.emitInstruction(instruction, 0x8003E16Cu).find("<<"),
            std::string::npos);
}

TEST(InstructionEmitter, ExactInstructionOverrideRetainsDelaySlotOrdering) {
  auto emitter = makeEmitter();
  emitter.setInstructionOverrides(
      {{0x80010004u, "override_delay_slot(ctx);"}});
  RecompFunction func;
  func.name = "overridden_delay_slot";
  func.address = 0x80010000u;
  func.instructions = {
      encI(0x04, 4, 0, 1),              // BEQ $a0, $zero, +1
      encR(0, 0, 4, 6, 16, 0x00),       // SLL $a2, $a0, 16
      encR(0, 31, 0, 0, 0, 0x08),       // JR $ra
      0x00000000u,                       // NOP
  };
  func.isLabelTarget.assign(func.instructions.size(), false);
  func.size = static_cast<uint32_t>(func.instructions.size() * 4u);

  const std::string code = emitter.emitFunction(func);
  const std::size_t replacement = code.find("override_delay_slot(ctx);");
  const std::size_t branch = code.find("if ((int32_t)(ctx->r4)");
  ASSERT_NE(replacement, std::string::npos) << code;
  ASSERT_NE(branch, std::string::npos) << code;
  EXPECT_LT(replacement, branch) << code;
}

// Multiply / Divide Tests

TEST(InstructionEmitter, MulDiv) {
  auto emitter = makeEmitter();

  // MULT $a0, $a1
  auto code = emitter.emitInstruction(
      MipsDecoder::decode(encR(0, 4, 5, 0, 0, 0x18)), 0);
  EXPECT_NE(code.find("ctx->lo"), std::string::npos);
  EXPECT_NE(code.find("ctx->hi"), std::string::npos);

  // MFLO $v0
  code = emitter.emitInstruction(MipsDecoder::decode(encR(0, 0, 0, 2, 0, 0x12)),
                                 0);
  EXPECT_NE(code.find("ctx->r2"), std::string::npos);
  EXPECT_NE(code.find("ctx->lo"), std::string::npos);

  // DIV -- should check for zero
  code = emitter.emitInstruction(MipsDecoder::decode(encR(0, 4, 5, 0, 0, 0x1A)),
                                 0);
  EXPECT_NE(code.find("!= 0"), std::string::npos);
}

// Load/Store Tests

TEST(InstructionEmitter, LoadWord) {
  auto emitter = makeEmitter();
  // LW $v0, 16($sp)
  auto inst = MipsDecoder::decode(encI(0x23, 29, 2, 16));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("pgxp_load_word"), std::string::npos);
  EXPECT_NE(code.find("ctx->r2"), std::string::npos);
  EXPECT_NE(code.find("16"), std::string::npos);
}

TEST(InstructionEmitter, StoreWord) {
  auto emitter = makeEmitter();
  // SW $v0, 16($sp)
  auto inst = MipsDecoder::decode(encI(0x2B, 29, 2, 16));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("pgxp_store_word"), std::string::npos);
}

TEST(InstructionEmitter, LoadByte) {
  auto emitter = makeEmitter();
  auto lb =
      emitter.emitInstruction(MipsDecoder::decode(encI(0x20, 4, 2, 0)), 0);
  EXPECT_NE(lb.find("(int8_t)MEM_READ8"), std::string::npos)
      << "LB must sign-extend bit 7; got: " << lb;

  auto lbu =
      emitter.emitInstruction(MipsDecoder::decode(encI(0x24, 4, 2, 0)), 0);
  EXPECT_NE(lbu.find("(uint8_t)MEM_READ8"), std::string::npos)
      << "LBU must zero-extend bit 7; got: " << lbu;
}

// LB/LH are signed MIPS loads.  Keeping the raw uint8_t/uint16_t returned by
// Memory breaks negative sentinels (Crash 3's -1 sequence handle became
// 65535 and skipped music callback installation).
TEST(InstructionEmitter, SignedAndUnsignedHalfwordLoads) {
  auto emitter = makeEmitter();

  auto lh =
      emitter.emitInstruction(MipsDecoder::decode(encI(0x21, 4, 2, 0)), 0);
  EXPECT_NE(lh.find("(int16_t)MEM_READ16"), std::string::npos)
      << "LH must sign-extend bit 15; got: " << lh;

  auto lhu =
      emitter.emitInstruction(MipsDecoder::decode(encI(0x25, 4, 2, 0)), 0);
  EXPECT_NE(lhu.find("(uint16_t)MEM_READ16"), std::string::npos)
      << "LHU must zero-extend bit 15; got: " << lhu;
}

TEST(InstructionEmitter, UnalignedLoadStore) {
  auto emitter = makeEmitter();

  auto code =
      emitter.emitInstruction(MipsDecoder::decode(encI(0x22, 4, 2, 0)), 0);
  EXPECT_NE(code.find("DO_LWL"), std::string::npos);

  code = emitter.emitInstruction(MipsDecoder::decode(encI(0x26, 4, 2, 0)), 0);
  EXPECT_NE(code.find("DO_LWR"), std::string::npos);
}

// SWL/SWR signature is (ctx, rt, addr) -- same convention as DO_LWL/LWR.
// Regression for a swapped-args emitter bug that wrecked CdIntToPos output
// in Crash Bandicoot (LBA 18 -> BCD 0x105731 instead of 0x000218).
TEST(InstructionEmitter, UnalignedStoreArgOrder) {
  auto emitter = makeEmitter();
  // SWL $v0, 75($sp) -- opcode 0x2A
  auto swl =
      emitter.emitInstruction(MipsDecoder::decode(encI(0x2A, 29, 2, 75)), 0);
  EXPECT_NE(swl.find("DO_SWL(ctx, ctx->r2"), std::string::npos)
      << "DO_SWL must receive rt before addr; got: " << swl;
  // SWR $v0, 72($sp) -- opcode 0x2E
  auto swr =
      emitter.emitInstruction(MipsDecoder::decode(encI(0x2E, 29, 2, 72)), 0);
  EXPECT_NE(swr.find("DO_SWR(ctx, ctx->r2"), std::string::npos)
      << "DO_SWR must receive rt before addr; got: " << swr;
}

// Branch Tests

TEST(InstructionEmitter, BranchBEQ) {
  auto emitter = makeEmitter();
  // BEQ $a0, $a1, +4
  auto inst = MipsDecoder::decode(encI(0x04, 4, 5, 4));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("=="), std::string::npos);
  EXPECT_NE(code.find("goto"), std::string::npos);
  EXPECT_NE(code.find("L_"), std::string::npos);
}

TEST(InstructionEmitter, BranchBNE) {
  auto emitter = makeEmitter();
  auto code = emitter.emitInstruction(MipsDecoder::decode(encI(0x05, 4, 5, 4)),
                                      0x80010000);
  EXPECT_NE(code.find("!="), std::string::npos);
  EXPECT_NE(code.find("goto"), std::string::npos);
}

TEST(InstructionEmitter, BranchComparisons) {
  auto emitter = makeEmitter();

  // BLEZ
  auto code = emitter.emitInstruction(MipsDecoder::decode(encI(0x06, 4, 0, 4)),
                                      0x80010000);
  EXPECT_NE(code.find("<= 0"), std::string::npos);

  // BGTZ
  code = emitter.emitInstruction(MipsDecoder::decode(encI(0x07, 4, 0, 4)),
                                 0x80010000);
  EXPECT_NE(code.find("> 0"), std::string::npos);
}

// Jump Tests

TEST(InstructionEmitter, Jump_J) {
  auto emitter = makeEmitter();
  auto inst = MipsDecoder::decode(encJ(0x02, 0x00004000));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("goto"), std::string::npos);
}

TEST(InstructionEmitter, Jump_JAL) {
  auto emitter = makeEmitter();
  // JAL to GameUpdate (at 0x80020000 = target 0x00008000)
  auto inst = MipsDecoder::decode(encJ(0x03, 0x00008000));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("GameUpdate"), std::string::npos);
  EXPECT_NE(code.find("rdram"), std::string::npos);
  EXPECT_NE(code.find("ctx"), std::string::npos);
}

TEST(InstructionEmitter, UnknownJalUsesRuntimeDispatch) {
  auto emitter = makeEmitter();
  // 0x80040000 is deliberately absent from makeEmitter's resolver.
  auto inst = MipsDecoder::decode(encJ(0x03, 0x00010000));
  auto code = emitter.emitInstruction(inst, 0x80010000);

  EXPECT_NE(code.find("recomp_dispatch(rdram, ctx, 0x80040000)"),
            std::string::npos);
  EXPECT_EQ(code.find("func_80040000"), std::string::npos);
}

TEST(InstructionEmitter, Jump_JR_RA) {
  auto emitter = makeEmitter();
  // JR $ra -- should emit return
  auto inst = MipsDecoder::decode(encR(0, 31, 0, 0, 0, 0x08));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("return"), std::string::npos);
}

TEST(InstructionEmitter, Jump_JR_NonRA) {
  auto emitter = makeEmitter();
  // JR $t0 -- indirect jump (not return)
  auto inst = MipsDecoder::decode(encR(0, 8, 0, 0, 0, 0x08));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("JUMP_INDIRECT"), std::string::npos);
}

TEST(InstructionEmitter, ComputedJrChecksKnownLocalLabelsBeforeDispatch) {
  RecompFunction f;
  f.name = "shared_renderer_region";
  f.address = 0x80010000u;
  f.instructions = {
      encR(0, 8, 0, 0, 0, 0x08), // JR $t0
      0x00000000u,               // NOP (delay slot)
      0x00000000u,
      0x00000000u,                // unreachable padding
      0x24020001u,                // local computed entry
      encR(0, 31, 0, 0, 0, 0x08), // JR $ra
      0x00000000u,                // NOP (delay slot)
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);
  f.isLabelTarget[4] = true;

  const std::string code = makeEmitter().emitFunction(f);
  const size_t local =
      code.find("if (_sw_target == 0x80010010u) goto L_80010010;");
  const size_t fallback =
      code.find("recomp_dispatch(rdram, ctx, _sw_target);", local);
  ASSERT_NE(local, std::string::npos) << code;
  ASSERT_NE(fallback, std::string::npos) << code;
  EXPECT_LT(local, fallback);
}

TEST(InstructionEmitter, ComputedJrCanSelectInstructionAfterDelaySlot) {
  RecompFunction f;
  f.name = "adjacent_computed_entry";
  f.address = 0x80010000u;
  f.instructions = {
      encR(0, 6, 0, 0, 0, 0x08),  // JR $a2
      0x00000000u,                // NOP (delay slot)
      0x24020001u,                // valid computed entry at PC+8
      encR(0, 31, 0, 0, 0, 0x08), // JR $ra
      0x00000000u,                // NOP (delay slot)
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);

  const std::string code = makeEmitter().emitFunction(f);
  EXPECT_NE(code.find("if (_sw_target == 0x80010008u) goto L_80010008;"),
            std::string::npos)
      << code;
  EXPECT_NE(code.find("L_80010008:"), std::string::npos) << code;
  EXPECT_NE(code.find("pgxp_addiu(ctx, 2, 0, 1)"), std::string::npos)
      << "the adjacent target must be emitted as code, not padding\n"
      << code;
}

TEST(InstructionEmitter, ProfiledIndirectTargetBecomesLocalCodeEntry) {
  RecompFunction f;
  f.name = "profiled_renderer_region";
  f.address = 0x80010000u;
  f.instructions = {
      encR(0, 25, 0, 0, 0, 0x08), // JR $t9
      0x00000000u,                // NOP (delay slot)
      0xDEADBEEFu,                // unreachable data
      0xDEADBEEFu,                // unreachable data
      0x24020001u,                // profiled code entry
      encR(0, 31, 0, 0, 0, 0x08), // JR $ra
      0x00000000u,                // NOP (delay slot)
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);
  f.indirectTargets = {0x80010010u};

  const std::string code = makeEmitter().emitFunction(f);
  EXPECT_NE(code.find("if (_sw_target == 0x80010010u) goto L_80010010;"),
            std::string::npos)
      << code;
  EXPECT_NE(code.find("L_80010010:"), std::string::npos) << code;
  EXPECT_NE(code.find("pgxp_addiu(ctx, 2, 0, 1)"), std::string::npos) << code;
}

TEST(InstructionEmitter, ExpandedOwnerKeepsInRangeJalAndReturnLocal) {
  RecompFunction f;
  f.name = "expanded_shared_renderer";
  f.address = 0x80010000u;
  f.instructions = {
      encJ(0x03, 0x80010018u >> 2), // JAL shared helper
      0x00000000u,                  // NOP (delay slot)
      0x24020001u,                  // caller continuation
      encR(0, 31, 0, 0, 0, 0x08),   // JR $ra
      0x00000000u,                  // NOP (delay slot)
      0x00000000u,                  // padding
      0x24030002u,                  // shared helper
      encR(0, 31, 0, 0, 0, 0x08),   // JR $ra
      0x00000000u,                  // NOP (delay slot)
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);
  // Independent CFG evidence already identifies the helper as part of this
  // owner; mere physical containment is deliberately insufficient.
  f.isLabelTarget[6] = true;

  const std::string code = makeEmitter().emitFunction(f);
  EXPECT_NE(code.find("ctx->r31 = 0x80010008; goto L_80010018; // local "
                      "architectural JAL"),
            std::string::npos)
      << code;
  EXPECT_EQ(code.find("recomp_dispatch(rdram, ctx, 0x80010018)"),
            std::string::npos)
      << code;
  EXPECT_NE(code.find("if (ctx->r31 == 0x80010008u) goto L_80010008;"),
            std::string::npos)
      << code;
  EXPECT_NE(code.find("L_80010018:"), std::string::npos) << code;
}

TEST(InstructionEmitter, ExpandedOwnerPreservesNonLocalHelperReturn) {
  RecompFunction f;
  f.name = "expanded_visibility_owner";
  f.address = 0x80010000u;
  // The shape mirrors Crash 3's 0x8003E058 visibility routine. The helper is
  // normally a local subroutine, but its accepted path restores the owner's
  // caller RA and returns there directly. Keeping both regions in one C++
  // owner lets JR $ra select a local continuation or leave the owner without
  // accidentally executing the remaining visibility samples.
  f.instructions = {
      encJ(0x03, 0x80010018u >> 2), // JAL shared visibility helper
      0x00000000u,                  // NOP (delay slot)
      0x24020001u,                  // next local sample
      encR(0, 31, 0, 0, 0, 0x08),  // JR $ra
      0x00000000u,                  // NOP (delay slot)
      0x00000000u,                  // padding
      encI(0x23, 3, 31, 0),         // LW $ra, 0($v1): non-local caller RA
      encI(0x09, 0, 24, 0),         // ADDIU $t8, $zero, 0: visible
      encR(0, 31, 0, 0, 0, 0x08),  // JR $ra
      0x00000000u,                  // NOP (delay slot)
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);
  f.isLabelTarget[6] = true;

  const std::string code = makeEmitter().emitFunction(f);
  const size_t localCall = code.find(
      "ctx->r31 = 0x80010008; goto L_80010018; // local architectural JAL");
  const size_t helper = code.find("L_80010018:", localCall);
  const size_t localResume = code.find(
      "if (ctx->r31 == 0x80010008u) goto L_80010008;", helper);
  const size_t ownerReturn = code.find("return;", localResume);

  ASSERT_NE(localCall, std::string::npos) << code;
  ASSERT_NE(helper, std::string::npos) << code;
  ASSERT_NE(localResume, std::string::npos) << code;
  ASSERT_NE(ownerReturn, std::string::npos) << code;
  EXPECT_LT(localResume, ownerReturn)
      << "a restored non-local RA must leave the owner, not fall through";
}

TEST(InstructionEmitter, DetectsArithmeticJumpDispatch) {
  // t0 = 0x8003AB28 - (a2 << 2); jr t0, bounded by a2 < 9.
  std::vector<uint32_t> words = {
      encI(0x0B, 6, 1, 9),       // sltiu at, a2, 9
      encI(0x0F, 0, 8, 0x8004),  // lui t0, 0x8004 (signed low adjusts)
      encI(0x09, 8, 8, 0xAB28),  // addiu t0, t0, 0xAB28
      encR(0, 0, 6, 1, 2, 0x00), // sll at, a2, 2
      encR(0, 8, 1, 8, 0, 0x23), // subu t0, t0, at
      encR(0, 8, 0, 0, 0, 0x08), // jr t0
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
  };

  const auto targets = detectArithmeticJumpTargets(words, 5, 0x8003AAF0);
  EXPECT_NE(std::find(targets.begin(), targets.end(), 0x8003AB18),
            targets.end());
  EXPECT_NE(std::find(targets.begin(), targets.end(), 0x8003AB28),
            targets.end());
}

TEST(InstructionEmitter, DetectsWideStrideArithmeticJumpDispatch) {
  // Crash 3's alignment-specialized byte-copy tail dispatches to five inline
  // handlers 0x20 bytes apart.  Its `a1 < 5` bound is more than 32
  // instructions before the final `a1 << 5; addu; jr` sequence.
  std::vector<uint32_t> words(100, 0);
  words[0] = encI(0x0B, 5, 18, 5);          // sltiu s2, a1, 5
  words[40] = encI(0x0F, 0, 20, 0x8004);    // lui s4, 0x8004
  words[41] = encI(0x09, 20, 20, 0xA9C8);   // addiu s4, s4, -22072
  words[42] = encR(0, 0, 5, 19, 5, 0x00);   // sll s3, a1, 5
  words[43] = encR(0, 20, 19, 20, 0, 0x21); // addu s4, s4, s3
  words[44] = encR(0, 0, 5, 19, 1, 0x00);   // delay-slot address update
  words[45] = encR(0, 20, 0, 0, 0, 0x08);   // jr s4

  const auto targets = detectArithmeticJumpTargets(words, 45, 0x8003A900);
  const std::vector<uint32_t> expected = {0x8003A9C8, 0x8003A9E8, 0x8003AA08,
                                          0x8003AA28, 0x8003AA48};
  EXPECT_EQ(targets, expected);
}

TEST(InstructionEmitter, DetectsCompositeStrideArithmeticJumpDispatch) {
  // s2 = ((a2 << 2) + a2) << 1; t0 = 0x8003ACAC - s2.
  std::vector<uint32_t> words(128, 0);
  words[0] = encI(0x0B, 6, 1, 9);          // sltiu at, a2, 9
  words[40] = encI(0x0F, 0, 8, 0x8004);    // lui t0, 0x8004
  words[41] = encI(0x09, 8, 8, 0xACAC);    // addiu t0, t0, -21332
  words[42] = encR(0, 0, 6, 18, 2, 0x00);  // sll s2, a2, 2
  words[43] = encR(0, 18, 6, 18, 0, 0x21); // addu s2, s2, a2
  words[44] = encR(0, 0, 18, 18, 1, 0x00); // sll s2, s2, 1
  words[45] = encR(0, 8, 18, 8, 0, 0x23);  // subu t0, t0, s2
  words[46] = encR(0, 8, 0, 0, 0, 0x08);   // jr t0

  const auto targets = detectArithmeticJumpTargets(words, 46, 0x8003AB00);
  const std::vector<uint32_t> expected = {0x8003AC5C, 0x8003AC70, 0x8003AC84,
                                          0x8003AC98, 0x8003ACAC};
  EXPECT_EQ(targets, expected);
}

TEST(InstructionEmitter, CompletesIntrinsicBranchVeneerFamily) {
  constexpr uint32_t base = 0x80010000u;
  RecompFunction f;
  f.name = "branch_veneer_family";
  f.address = base;
  f.instructions.assign(12, 0);
  // Three adjacent two-word veneers all branch to the same local handler.
  // The recovered table deliberately omits the final member, matching the
  // real failure mode where a conservative selector bound was one short.
  f.instructions[4] = encI(0x01, 0, 0x01, static_cast<uint16_t>(-5));
  f.instructions[6] = encI(0x01, 0, 0x01, static_cast<uint16_t>(-7));
  f.instructions[8] = encI(0x01, 0, 0x01, static_cast<uint16_t>(-9));
  f.instructions[10] = encR(0, 8, 0, 0, 0, 0x08); // jr t0
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);
  f.isLabelTarget[0] = true;
  f.indirectTargets = {base + 0x10u, base + 0x18u};

  const std::vector<uint32_t> completed = completeBranchVeneerTargets(
      f.instructions, base, f.indirectTargets);
  ASSERT_EQ(completed.size(), 3u);
  EXPECT_EQ(completed.back(), base + 0x20u);

  const std::string code = makeEmitter().emitFunction(f);
  EXPECT_NE(code.find("L_80010020:"), std::string::npos) << code;
  EXPECT_NE(code.find("if (_sw_target == 0x80010020u) goto L_80010020;"),
            std::string::npos)
      << code;
}

TEST(InstructionEmitter, CompletesTerminalFallthroughVeneer) {
  constexpr uint32_t base = 0x80010000u;
  std::vector<uint32_t> words(6, 0);
  // Two branch veneers copy different sources into $ra, then skip to +0x14.
  words[0] = encI(0x01, 0, 0x01, 4);          // bgez zero, +0x14
  words[1] = encR(0, 23, 0, 31, 0, 0x21);    // move ra, s7
  words[2] = encI(0x01, 0, 0x01, 2);          // bgez zero, +0x14
  words[3] = encR(0, 24, 0, 31, 0, 0x21);    // move ra, t8
  words[4] = encR(0, 8, 0, 31, 0, 0x21);     // terminal: move ra, t0
  words[5] = 0;                               // common destination

  const std::vector<uint32_t> completed = completeBranchVeneerTargets(
      words, base, {base, base + 8u});
  const std::vector<uint32_t> expected = {base, base + 8u, base + 0x10u};
  EXPECT_EQ(completed, expected);
}

TEST(InstructionEmitter, CompletesDifferentDestinationBranchVeneerArray) {
  constexpr uint32_t base = 0x80010000u;
  std::vector<uint32_t> words(20, 0);
  // A computed selector reaches a compact array of unconditional branches.
  // Every slot has the same two-word/NOP shape, but each branch selects a
  // different handler.  Two independently known entries bracket an
  // uninterrupted array; the remaining entries are completed from its
  // intrinsic shape.
  words[4] = encI(0x01, 0, 0x01, 11); // bgez zero, +0x40
  words[6] = encI(0x01, 0, 0x01, 10); // bgez zero, +0x48
  words[8] = encI(0x01, 0, 0x01, 9);  // bgez zero, +0x50
  words[10] = encI(0x01, 0, 0x01, 8); // bgez zero, +0x58
  words[12] = encI(0x01, 0, 0x01, 7); // bgez zero, +0x60
  const uint32_t sharedDelay = encR(0, 5, 4, 5, 0, 0x21); // addu a1,a1,a0
  words[5] = sharedDelay;
  words[7] = sharedDelay;
  words[9] = sharedDelay;
  words[11] = sharedDelay;
  words[13] = sharedDelay;
  words[14] = encR(0, 8, 0, 0, 0, 0x08); // non-veneer boundary

  const std::vector<uint32_t> completed = completeBranchVeneerTargets(
      words, base, {base + 0x10u, base + 0x30u});
  const std::vector<uint32_t> expected = {
      base + 0x10u, base + 0x18u, base + 0x20u,
      base + 0x28u, base + 0x30u};
  EXPECT_EQ(completed, expected);
}

TEST(InstructionEmitter, CompletesHoleBetweenSiblingTailBranchHandlers) {
  constexpr uint32_t base = 0x80010000u;
  std::vector<uint32_t> words(16, 0);
  // Three sibling bodies end at +0x08, +0x14 and +0x20 and all tail-branch
  // to +0x38. Their following words (+0x10, +0x1C, +0x28) are the sibling
  // entries. The first is also an ordinary conditional-branch target and the
  // last was recovered from an indirect selector; the middle is therefore an
  // intrinsic, bracketed hole rather than guessed dead fallthrough.
  words[0] = encI(0x04, 8, 9, 3);       // conditional CFG edge to +0x10
  words[2] = encI(0x01, 0, 0x01, 11);  // bgez zero, +0x38
  words[5] = encI(0x01, 0, 0x01, 8);   // bgez zero, +0x38
  words[8] = encI(0x01, 0, 0x01, 5);   // bgez zero, +0x38

  const std::vector<uint32_t> completed = completeBranchVeneerTargets(
      words, base, {base + 0x28u});
  const std::vector<uint32_t> expected = {
      base + 0x10u, base + 0x1Cu, base + 0x28u};
  EXPECT_EQ(completed, expected);
}

// COP0 Tests

TEST(InstructionEmitter, COP0) {
  auto emitter = makeEmitter();

  // MFC0 $v0, SR
  auto code = emitter.emitInstruction(
      MipsDecoder::decode(encR(0x10, 0x00, 2, 12, 0, 0)), 0);
  EXPECT_NE(code.find("cop0"), std::string::npos);
  EXPECT_NE(code.find("SR"), std::string::npos);

  // MTC0 $v0, SR
  code = emitter.emitInstruction(
      MipsDecoder::decode(encR(0x10, 0x04, 2, 12, 0, 0)), 0);
  EXPECT_NE(code.find("cop0"), std::string::npos);
}

// GTE Tests

TEST(InstructionEmitter, GTE_RegisterMoves) {
  auto emitter = makeEmitter();

  // MFC2 $v0, r5
  auto code = emitter.emitInstruction(
      MipsDecoder::decode(encR(0x12, 0x00, 2, 5, 0, 0)), 0x80012340);
  EXPECT_NE(code.find("pgxp_mfc2"), std::string::npos);
  EXPECT_NE(code.find("ctx->r2"), std::string::npos);
  EXPECT_NE(code.find("ctx->pc = 0x80012340"), std::string::npos);

  // MTC2 $v0, r5
  code = emitter.emitInstruction(
      MipsDecoder::decode(encR(0x12, 0x04, 2, 5, 0, 0)), 0);
  EXPECT_NE(code.find("pgxp_mtc2"), std::string::npos);

  // LWC2/SWC2 must also preserve register side effects rather than touching
  // the backing arrays directly.
  code = emitter.emitInstruction(MipsDecoder::decode(encI(0x32, 29, 15, 16)),
                                 0x80012348);
  EXPECT_NE(code.find("pgxp_lwc2"), std::string::npos);
  code = emitter.emitInstruction(MipsDecoder::decode(encI(0x3A, 4, 15, 0)),
                                 0x8001234C);
  EXPECT_NE(code.find("pgxp_swc2"), std::string::npos);
}

TEST(InstructionEmitter, GTE_Commands) {
  auto emitter = makeEmitter();

  // RTPS command
  uint32_t raw = (0x12u << 26) | (1u << 25) | 0x01;
  auto code = emitter.emitInstruction(MipsDecoder::decode(raw), 0);
  EXPECT_NE(code.find("ps1::GTE::RTPS"), std::string::npos);
  EXPECT_NE(code.find("ctx->pc"), std::string::npos);

  // NCLIP command
  raw = (0x12u << 26) | (1u << 25) | 0x06;
  code = emitter.emitInstruction(MipsDecoder::decode(raw), 0);
  EXPECT_NE(code.find("ps1::GTE::NCLIP"), std::string::npos);
}

// NOP Handling

TEST(InstructionEmitter, NOP) {
  auto emitter = makeEmitter();
  auto code = emitter.emitInstruction(MipsDecoder::decode(0), 0);
  EXPECT_NE(code.find("NOP"), std::string::npos);
}

// Function Emission

TEST(InstructionEmitter, EmitFunction) {
  auto emitter = makeEmitter();

  RecompFunction func;
  func.name = "test_func";
  func.address = 0x80010000;
  func.instructions = {
      encI(0x09, 29, 29, static_cast<uint16_t>(-32)), // ADDIU $sp, $sp, -32
      encI(0x2B, 29, 31, 28),                         // SW $ra, 28($sp)
      encJ(0x03, 0x00008000),                         // JAL GameUpdate
      0x00000000,                                     // NOP (delay slot)
      encI(0x23, 29, 31, 28),                         // LW $ra, 28($sp)
      encI(0x09, 29, 29, 32),                         // ADDIU $sp, $sp, 32
      encR(0, 31, 0, 0, 0, 0x08),                     // JR $ra
      0x00000000                                      // NOP (delay slot)
  };
  func.isLabelTarget.resize(func.instructions.size(), false);
  func.size = func.instructions.size() * 4;

  auto code = emitter.emitFunction(func);
  EXPECT_NE(code.find("PS1_FUNCTION_TRACE_HOOK"), std::string::npos);

  // Should contain function signature
  EXPECT_NE(code.find("test_func"), std::string::npos);
  EXPECT_NE(code.find("rdram"), std::string::npos);
  EXPECT_NE(code.find("recomp_context"), std::string::npos);

  // Should contain stack operations
  EXPECT_NE(code.find("-32"), std::string::npos);

  // Should contain function call
  EXPECT_NE(code.find("GameUpdate"), std::string::npos);

  // Should contain return
  EXPECT_NE(code.find("return"), std::string::npos);

  // Should handle delay slots
  EXPECT_NE(code.find("delay slot"), std::string::npos);
}

TEST(InstructionEmitter, EmitFunctionWithBranch) {
  auto emitter = makeEmitter();

  RecompFunction func;
  func.name = "branch_test";
  func.address = 0x80010000;
  func.instructions = {
      encI(0x04, 4, 0, 2),        // BEQ $a0, $zero, +2
      0x00000000,                 // NOP (delay slot)
      encI(0x09, 0, 2, 1),        // ADDIU $v0, $zero, 1
      encR(0, 31, 0, 0, 0, 0x08), // JR $ra
      0x00000000                  // NOP (delay slot)
  };
  func.isLabelTarget = {false, false, false, false, false};
  func.isLabelTarget.resize(func.instructions.size(), false);
  // The BEQ target would be instruction [3] (PC + 4 + 2*4 = addr+12)
  // We can mark it manually
  if (func.instructions.size() > 3)
    func.isLabelTarget[3] = true;
  func.size = func.instructions.size() * 4;

  auto code = emitter.emitFunction(func);

  // Should contain branch condition
  EXPECT_NE(code.find("=="), std::string::npos);
  EXPECT_NE(code.find("goto"), std::string::npos);

  // Should contain label for branch target
  EXPECT_NE(code.find("L_"), std::string::npos);
}

TEST(InstructionEmitter, EmitsPcCheckpointsAtCallableEntriesAndLabels) {
  RecompFunction func;
  func.name = "checkpoint_test";
  func.address = 0x80010000;
  func.instructions = {
      encI(0x04, 4, 0, 1),        // BEQ $a0, $zero, +1
      0x00000000,                 // NOP (delay slot)
      encR(0, 31, 0, 0, 0, 0x08), // JR $ra
      0x00000000,                 // NOP (delay slot)
  };
  func.isLabelTarget.assign(func.instructions.size(), false);
  func.size = static_cast<uint32_t>(func.instructions.size() * 4);

  const std::string code = makeEmitter().emitFunction(func);
  const size_t entryHook = code.find("PS1_PC_TRACE_HOOK(ctx, 0x80010000u)");
  const size_t targetHook = code.find("PS1_PC_TRACE_HOOK(ctx, 0x80010008u)");
  ASSERT_NE(entryHook, std::string::npos) << code;
  ASSERT_NE(targetHook, std::string::npos) << code;
  EXPECT_LT(entryHook, targetHook);
  EXPECT_EQ(code.find("PS1_PC_TRACE_HOOK(ctx, 0x80010004u)"), std::string::npos)
      << "ordinary delay slots must not inflate generated code\n"
      << code;
}

TEST(InstructionEmitter, RegionPrefixJumpsToLogicalEntry) {
  RecompFunction f;
  f.name = "func_80010008";
  f.address = 0x80010000;
  f.entryAddress = 0x80010008;
  f.instructions = {0, 0, 0x03E00008, 0};
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);

  const std::string out = makeEmitter().emitFunction(f);
  EXPECT_NE(out.find("goto L_80010008; // logical function entry"),
            std::string::npos);
  EXPECT_NE(out.find("L_80010008:"), std::string::npos);
}

TEST(InstructionEmitter, RegionPrefixScansReturnAliasAtLogicalEntry) {
  RecompFunction f;
  f.name = "func_80010008";
  f.address = 0x80010000;
  f.entryAddress = 0x80010008;
  f.instructions = {
      0x00000000,                 // shared prefix
      0x00000000,                 // shared prefix
      encR(0, 31, 0, 1, 0, 0x21), // ADDU $at, $ra, $zero
      encR(0, 1, 0, 0, 0, 0x08),  // JR $at
      0x00000000,                 // NOP (delay slot)
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);

  const std::string out = makeEmitter().emitFunction(f);
  EXPECT_NE(out.find("return; // return through saved $ra alias"),
            std::string::npos)
      << out;
  EXPECT_EQ(out.find("JUMP_INDIRECT(ctx, ctx->r1)"), std::string::npos) << out;
}

TEST(InstructionEmitter, SavedRaAliasReturnsToNativeCaller) {
  RecompFunction f;
  f.name = "saved_ra_return";
  f.address = 0x80010000;
  f.instructions = {
      0x00000000,                  // NOP
      encR(0, 31, 0, 1, 0, 0x21),  // ADDU $at, $ra, $zero
      encJ(0x03, 0x80020000 >> 2), // JAL nested function
      0x00000000,                  // NOP (delay slot)
      encR(0, 1, 0, 0, 0, 0x08),   // JR $at
      0x00000000,                  // NOP (delay slot)
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);

  const std::string out = makeEmitter().emitFunction(f);
  EXPECT_NE(out.find("GameUpdate(rdram, ctx)"), std::string::npos);
  EXPECT_NE(out.find("return; // return through saved $ra alias"),
            std::string::npos);
  EXPECT_EQ(out.find("JUMP_INDIRECT(ctx, ctx->r1)"), std::string::npos);
}

TEST(InstructionEmitter, OrdinaryJrAtRemainsIndirect) {
  RecompFunction f;
  f.name = "ordinary_jr_at";
  f.address = 0x80010000;
  f.instructions = {
      encR(0, 1, 0, 0, 0, 0x08), // JR $at
      0x00000000,                // NOP (delay slot)
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);

  const std::string out = makeEmitter().emitFunction(f);
  EXPECT_NE(out.find("JUMP_INDIRECT(ctx, ctx->r1)"), std::string::npos);
}

TEST(InstructionEmitter, FindsSavedRaAliasInEntryBranchDelaySlot) {
  RecompFunction f;
  f.name = "saved_ra_in_delay_slot";
  f.address = 0x80010000;
  f.instructions = {
      encI(0x04, 17, 0, 2),       // BEQ $s1, $zero, +2
      encR(0, 31, 0, 1, 0, 0x21), // ADDU $at, $ra, $zero (delay)
      encR(0, 1, 0, 0, 0, 0x08),  // JR $at
      0x00000000,                 // NOP (delay slot)
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);

  const std::string out = makeEmitter().emitFunction(f);
  EXPECT_NE(out.find("return; // return through saved $ra alias"),
            std::string::npos);
  EXPECT_EQ(out.find("JUMP_INDIRECT(ctx, ctx->r1)"), std::string::npos);
}

TEST(InstructionEmitter, BackwardBranchYieldsWithConventionalStackPointer) {
  RecompFunction f;
  f.name = "poll_loop";
  f.address = 0x80010000;
  f.instructions = {
      encI(0x23, 4, 2, 0),                         // LW $v0, 0($a0)
      encI(0x04, 2, 0, static_cast<uint16_t>(-2)), // BEQ $v0,$zero,loop
      0x00000000,                                  // NOP (delay slot)
      encR(0, 31, 0, 0, 0, 0x08),                  // JR $ra
      0x00000000,                                  // NOP (delay slot)
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);

  const std::string out = makeEmitter().emitFunction(f);
  EXPECT_NE(out.find("runtimeBiosDrainPendingCallbacks"), std::string::npos);
}

TEST(InstructionEmitter, GeneralPurposeStackPointerSuppressesLoopYield) {
  RecompFunction f;
  f.name = "list_cursor_in_sp";
  f.address = 0x80010000;
  f.instructions = {
      encI(0x09, 29, 29, static_cast<uint16_t>(-16)), // stack prologue
      encI(0x23, 4, 29, 0),                           // LW $sp, 0($a0)
      encI(0x05, 29, 0, static_cast<uint16_t>(-2)),   // BNE $sp,$zero,loop
      0x00000000,                                     // NOP (delay slot)
      encR(0, 31, 0, 0, 0, 0x08),                     // JR $ra
      0x00000000,                                     // NOP (delay slot)
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);

  const std::string out = makeEmitter().emitFunction(f);
  EXPECT_EQ(out.find("runtimeBiosDrainPendingCallbacks"), std::string::npos);
  EXPECT_NE(out.find("recomp_callback_drain_guard"), std::string::npos);
}

TEST(InstructionEmitter, StatefulStoreLoopSuppressesCallbackYield) {
  RecompFunction f;
  f.name = "render_list_builder";
  f.address = 0x80010000;
  f.instructions = {
      encI(0x2B, 4, 2, 0),                         // SW $v0, 0($a0)
      encI(0x05, 4, 5, static_cast<uint16_t>(-2)), // BNE $a0,$a1,loop
      encI(0x09, 4, 4, 4),                         // ADDIU $a0,$a0,4
      encR(0, 31, 0, 0, 0, 0x08),                  // JR $ra
      0x00000000,                                  // NOP (delay slot)
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);

  const std::string out = makeEmitter().emitFunction(f);
  EXPECT_EQ(out.find("runtimeBiosDrainPendingCallbacks"), std::string::npos);
}

TEST(InstructionEmitter, LoopContainingCallSuppressesCallbackYield) {
  RecompFunction f;
  f.name = "stateful_call_loop";
  f.address = 0x80010000;
  f.instructions = {
      encI(0x23, 4, 2, 0),                         // LW $v0, 0($a0)
      encJ(0x03, 0x80020000u >> 2),                // JAL GameUpdate
      0x00000000,                                  // NOP (delay slot)
      encI(0x05, 2, 0, static_cast<uint16_t>(-4)), // BNE $v0,$zero,loop
      0x00000000,                                  // NOP (delay slot)
      encR(0, 31, 0, 0, 0, 0x08),                  // JR $ra
      0x00000000,                                  // NOP (delay slot)
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);

  const std::string out = makeEmitter().emitFunction(f);
  EXPECT_EQ(out.find("runtimeBiosDrainPendingCallbacks"), std::string::npos);
}

TEST(InstructionEmitter, ReadOnlyCountedScanSuppressesCallbackYield) {
  RecompFunction f;
  f.name = "audio_table_reduction";
  f.address = 0x80010000;
  f.instructions = {
      encI(0x23, 4, 8, 0),                          // LW $t0, 0($a0)
      encR(0, 16, 8, 16, 0, 0x24),                  // AND $s0, $s0, $t0
      encI(0x09, 4, 4, 4),                          // ADDIU $a0, $a0, 4
      encI(0x09, 9, 9, 1),                          // ADDIU $t1, $t1, 1
      encI(0x0A, 9, 10, 15),                        // SLTI $t2, $t1, 15
      encI(0x05, 10, 0, static_cast<uint16_t>(-6)), // BNE $t2,$zero,loop
      0x00000000,                                   // NOP (delay slot)
      encR(0, 31, 0, 0, 0, 0x08),                   // JR $ra
      0x00000000,                                   // NOP (delay slot)
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);

  const std::string out = makeEmitter().emitFunction(f);
  EXPECT_EQ(out.find("runtimeBiosDrainPendingCallbacks"), std::string::npos);
}

TEST(InstructionEmitter, RootEntryStackInitializationDoesNotBlockDescendants) {
  RecompFunction f;
  f.name = "__start";
  f.address = 0x80010000;
  f.instructions = {
      encR(0, 4, 0, 29, 0, 0x21),   // ADDU $sp, $a0, $zero
      encJ(0x03, 0x80020000u >> 2), // JAL GameUpdate/main
      0x00000000,                   // NOP (delay slot)
      encR(0, 0, 0, 0, 0, 0x0D),    // BREAK
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);

  const std::string out = makeEmitter().emitFunction(f);
  EXPECT_EQ(out.find("recomp_callback_drain_guard"), std::string::npos);
}

TEST(InstructionEmitter,
     OutOfBoundsSharedHelperCanResumeAtArchitecturalReturnAddress) {
  RecompFunction f;
  f.name = "func_shared_helper_caller";
  f.address = 0x80010000u;
  // lui   $ra, 0x8001
  // addiu $ra, $ra, 0x10       # synthetic continuation
  // b     0x80010400            # shared helper outside this body
  // nop
  // addiu $v0, $zero, 7        # continuation at 0x80010010
  // jr    $ra
  // nop
  f.instructions = {0x3C1F8001u, 0x27FF0010u, 0x100000FDu, 0x00000000u,
                    0x24020007u, 0x03E00008u, 0x00000000u};
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);
  // The continuation is a known CFG label, as it is in Crash 3's renderer
  // routine. The OOB dispatch must be able to return to it.
  f.isLabelTarget[4] = true;

  const std::string code = makeEmitter().emitFunction(f);
  const size_t dispatch = code.find("recomp_dispatch(rdram, ctx, 0x80010400);");
  const size_t resume =
      code.find("if (ctx->r31 == 0x80010010u) goto L_80010010;", dispatch);

  ASSERT_NE(dispatch, std::string::npos) << code;
  ASSERT_NE(resume, std::string::npos) << code;
  EXPECT_GT(resume, dispatch);
}

TEST(InstructionEmitter,
     InRangeSharedHelperCanResumeAtArchitecturalReturnAddress) {
  RecompFunction f;
  f.name = "func_local_shared_helper";
  f.address = 0x80010000u;
  // lui   $ra, 0x8001
  // addiu $ra, $ra, 0x10       # local continuation
  // b     0x80010020            # shared helper in this emitted region
  // nop
  // lw    $ra, 0($sp)           # caller epilogue at 0x80010010
  // jr    $ra
  // nop
  // nop                         # padding before the shared entry
  // jr    $ra                   # helper returns to 0x80010010
  // nop
  f.instructions = {
      encI(0x0F, 0, 31, 0x8001),
      encI(0x09, 31, 31, 0x0010),
      encI(0x04, 0, 0, 5),
      0x00000000u,
      encI(0x23, 29, 31, 0),
      encR(0, 31, 0, 0, 0, 0x08),
      0x00000000u,
      0x00000000u,
      encR(0, 31, 0, 0, 0, 0x08),
      0x00000000u,
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);

  const std::string code = makeEmitter().emitFunction(f);
  const std::string resume = "if (ctx->r31 == 0x80010010u) goto L_80010010;";
  EXPECT_NE(code.find("L_80010010:"), std::string::npos) << code;
  EXPECT_NE(code.find(resume), std::string::npos) << code;
}

TEST(InstructionEmitter,
     ExternalIndirectTrampolineCanResumeLocalArchitecturalCall) {
  RecompFunction f;
  f.name = "indirect_trampoline_owner";
  f.address = 0x80010000u;
  // jal   0x80010010          # $ra = local continuation 0x80010008
  // nop
  // jr    $ra                 # simplified continuation
  // nop
  // jr    $t0                 # external `jr $ra` trampoline
  // nop
  f.instructions = {
      encJ(0x03, 0x80010010u >> 2),
      0x00000000u,
      encR(0, 31, 0, 0, 0, 0x08),
      0x00000000u,
      encR(0, 8, 0, 0, 0, 0x08),
      0x00000000u,
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);
  f.isLabelTarget[4] = true;

  const std::string code = makeEmitter().emitFunction(f);
  const size_t fallback = code.find("recomp_dispatch(rdram, ctx, _sw_target);");
  const size_t resume = code.find(
      "if (ctx->r31 == 0x80010008u) goto L_80010008;", fallback);

  ASSERT_NE(fallback, std::string::npos) << code;
  ASSERT_NE(resume, std::string::npos) << code;
  EXPECT_GT(resume, fallback);
  EXPECT_EQ(code.find("JUMP_INDIRECT(ctx, ctx->r8); // fallback"),
            std::string::npos)
      << code;
}

// Helpers

TEST(InstructionEmitter, SavestateContinuationResumesWithoutReplayingPrologue) {
  RecompFunction f;
  f.name = "resumable_frame";
  f.address = 0x80010000u;
  f.instructions = {
      encI(0x09, 29, 29, static_cast<uint16_t>(-16)), // stack prologue
      encJ(0x03, 0x00008000),                         // JAL GameUpdate
      0x00000000u,                                    // delay slot
      encI(0x09, 16, 16, 1),                          // continuation
      encR(0, 31, 0, 0, 0, 0x08),                     // JR $ra
      0x00000000u,                                    // delay slot
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);
  f.resumeTargets = {0x8001000Cu};

  const std::string code = makeEmitter().emitFunction(f);
  const size_t resumeSwitch = code.find("case 0x8001000Cu: goto L_8001000C;");
  const size_t prologue = code.find("pgxp_addiu(ctx, 29, 29, -16)");
  const size_t continuation = code.find("L_8001000C:");
  ASSERT_NE(resumeSwitch, std::string::npos) << code;
  ASSERT_NE(prologue, std::string::npos) << code;
  ASSERT_NE(continuation, std::string::npos) << code;
  EXPECT_LT(resumeSwitch, prologue)
      << "resume selection must jump over the already-executed prologue";
  EXPECT_NE(code.find("recomp_resume_dispatch(rdram, ctx, ctx->r31)"),
            std::string::npos)
      << "the detached root must chain to its architectural caller";
}

TEST(InstructionEmitter, NativeEntryAliasSelectsInternalLabelAndClearsToken) {
  RecompFunction f;
  f.name = "renderer_owner";
  f.address = 0x80010000u;
  f.instructions = {
      encI(0x09, 29, 29, static_cast<uint16_t>(-16)), // owner prologue
      encI(0x09, 16, 16, 1),                          // internal entry
      encR(0, 31, 0, 0, 0, 0x08),                     // JR $ra
      0x00000000u,                                    // delay slot
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);
  f.entryAliases = {0x80010004u};

  const std::string code = makeEmitter().emitFunction(f);
  const size_t tokenRead =
      code.find("const uint32_t _native_entry_pc = ctx->nativeEntryPc;");
  const size_t tokenClear = code.find("ctx->nativeEntryPc = 0;", tokenRead);
  const size_t aliasSwitch =
      code.find("case 0x80010004u: goto L_80010004;", tokenClear);
  const size_t prologue = code.find("pgxp_addiu(ctx, 29, 29, -16)");
  const size_t internalEntry = code.find("L_80010004:");
  ASSERT_NE(tokenRead, std::string::npos) << code;
  ASSERT_NE(tokenClear, std::string::npos) << code;
  ASSERT_NE(aliasSwitch, std::string::npos) << code;
  ASSERT_NE(prologue, std::string::npos) << code;
  ASSERT_NE(internalEntry, std::string::npos) << code;
  EXPECT_LT(tokenRead, tokenClear);
  EXPECT_LT(tokenClear, aliasSwitch);
  EXPECT_LT(aliasSwitch, prologue);
  EXPECT_LT(prologue, internalEntry);
  EXPECT_EQ(code.find("ctx->nativeResumePc = 0;"), std::string::npos)
      << "live entry aliases must not consume the savestate continuation";
}

TEST(InstructionEmitter, RegHelper) {
  EXPECT_EQ(InstructionEmitter::reg(0), "(int32_t)0");
  EXPECT_EQ(InstructionEmitter::reg(2), "ctx->r2");
  EXPECT_EQ(InstructionEmitter::reg(29), "ctx->r29");
  EXPECT_EQ(InstructionEmitter::reg(31), "ctx->r31");
}

TEST(InstructionEmitter, LabelHelper) {
  EXPECT_EQ(InstructionEmitter::label(0x80010040), "L_80010040");
}

TEST(InstructionEmitter, RegisterOnlyIdleLoopYieldsForOtherGuestThreads) {
  RecompFunction f;
  f.name = "idle_thread";
  f.address = 0x80010000;
  f.instructions = {
      encI(0x23, 4, 2, 0),                       // load outside the idle loop
      0u,
      encI(0x04, 2, 0, static_cast<uint16_t>(-2)),
      0u,
      encR(0, 31, 0, 0, 0, 0x08), 0u,
  };
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  f.isLabelTarget.assign(f.instructions.size(), false);
  EXPECT_NE(makeEmitter().emitFunction(f).find("runtimeBiosDrainPendingCallbacks"),
            std::string::npos);
}
