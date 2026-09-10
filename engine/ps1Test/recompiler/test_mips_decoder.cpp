// Tests for ps1Recomp -- MIPS I Decoder
// Validates R3000A instruction decoding (MIPS I + COP0 + COP2/GTE)

#include <gtest/gtest.h>
#include <ps1recomp/mips_decoder.h>

using namespace ps1recomp;

// MIPS Instruction Encoding Helpers

// R-type: opcode(6) | rs(5) | rt(5) | rd(5) | shamt(5) | funct(6)
static constexpr uint32_t encR(uint8_t op, uint8_t rs, uint8_t rt, uint8_t rd, uint8_t shamt, uint8_t funct) {
    return (static_cast<uint32_t>(op) << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) |
           (static_cast<uint32_t>(shamt) << 6) |
           static_cast<uint32_t>(funct);
}

// I-type: opcode(6) | rs(5) | rt(5) | imm16(16)
static constexpr uint32_t encI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (static_cast<uint32_t>(op) << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           static_cast<uint32_t>(imm);
}

// J-type: opcode(6) | target(26)
static constexpr uint32_t encJ(uint8_t op, uint32_t target) {
    return (static_cast<uint32_t>(op) << 26) | (target & 0x03FFFFFF);
}

// NOP

TEST(MipsDecoder, NOP) {
    auto inst = MipsDecoder::decode(0x00000000);
    EXPECT_EQ(inst.id, InstrId::NOP);
    EXPECT_TRUE(inst.isNOP());
    EXPECT_EQ(inst.category, InstrCategory::Special);
    EXPECT_EQ(MipsDecoder::instrName(inst.id), "NOP");
}

// ALU R-type

TEST(MipsDecoder, ALU_R_Type) {
    // ADDU $v0, $a0, $a1 -- funct=0x21
    auto inst = MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x21));
    EXPECT_EQ(inst.id, InstrId::ADDU);
    EXPECT_EQ(inst.category, InstrCategory::ALU);
    EXPECT_EQ(inst.rs, 4);
    EXPECT_EQ(inst.rt, 5);
    EXPECT_EQ(inst.rd, 2);

    // SUBU $t0, $s0, $s1 -- funct=0x23
    inst = MipsDecoder::decode(encR(0, 16, 17, 8, 0, 0x23));
    EXPECT_EQ(inst.id, InstrId::SUBU);
    EXPECT_EQ(inst.rs, 16);
    EXPECT_EQ(inst.rt, 17);
    EXPECT_EQ(inst.rd, 8);

    // AND
    inst = MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x24));
    EXPECT_EQ(inst.id, InstrId::AND);

    // OR
    inst = MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x25));
    EXPECT_EQ(inst.id, InstrId::OR);

    // XOR
    inst = MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x26));
    EXPECT_EQ(inst.id, InstrId::XOR);

    // NOR
    inst = MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x27));
    EXPECT_EQ(inst.id, InstrId::NOR);

    // SLT
    inst = MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x2A));
    EXPECT_EQ(inst.id, InstrId::SLT);

    // SLTU
    inst = MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x2B));
    EXPECT_EQ(inst.id, InstrId::SLTU);

    // ADD (trap on overflow)
    inst = MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x20));
    EXPECT_EQ(inst.id, InstrId::ADD);

    // SUB (trap on overflow)
    inst = MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x22));
    EXPECT_EQ(inst.id, InstrId::SUB);
}

// Shifts

TEST(MipsDecoder, Shifts) {
    // SLL $v0, $a0, 4
    auto inst = MipsDecoder::decode(encR(0, 0, 4, 2, 4, 0x00));
    EXPECT_EQ(inst.id, InstrId::SLL);
    EXPECT_EQ(inst.shamt, 4);
    EXPECT_EQ(inst.rd, 2);
    EXPECT_EQ(inst.rt, 4);

    // SRL
    inst = MipsDecoder::decode(encR(0, 0, 4, 2, 5, 0x02));
    EXPECT_EQ(inst.id, InstrId::SRL);
    EXPECT_EQ(inst.shamt, 5);

    // SRA
    inst = MipsDecoder::decode(encR(0, 0, 4, 2, 5, 0x03));
    EXPECT_EQ(inst.id, InstrId::SRA);

    // SLLV
    inst = MipsDecoder::decode(encR(0, 3, 4, 2, 0, 0x04));
    EXPECT_EQ(inst.id, InstrId::SLLV);

    // SRLV
    inst = MipsDecoder::decode(encR(0, 3, 4, 2, 0, 0x06));
    EXPECT_EQ(inst.id, InstrId::SRLV);

    // SRAV
    inst = MipsDecoder::decode(encR(0, 3, 4, 2, 0, 0x07));
    EXPECT_EQ(inst.id, InstrId::SRAV);
}

// Multiply / Divide

TEST(MipsDecoder, MulDiv) {
    // MULT $a0, $a1
    auto inst = MipsDecoder::decode(encR(0, 4, 5, 0, 0, 0x18));
    EXPECT_EQ(inst.id, InstrId::MULT);
    EXPECT_EQ(inst.category, InstrCategory::MulDiv);

    // MULTU
    inst = MipsDecoder::decode(encR(0, 4, 5, 0, 0, 0x19));
    EXPECT_EQ(inst.id, InstrId::MULTU);

    // DIV
    inst = MipsDecoder::decode(encR(0, 4, 5, 0, 0, 0x1A));
    EXPECT_EQ(inst.id, InstrId::DIV);

    // DIVU
    inst = MipsDecoder::decode(encR(0, 4, 5, 0, 0, 0x1B));
    EXPECT_EQ(inst.id, InstrId::DIVU);

    // MFHI / MFLO
    inst = MipsDecoder::decode(encR(0, 0, 0, 2, 0, 0x10));
    EXPECT_EQ(inst.id, InstrId::MFHI);

    inst = MipsDecoder::decode(encR(0, 0, 0, 2, 0, 0x12));
    EXPECT_EQ(inst.id, InstrId::MFLO);

    // MTHI / MTLO
    inst = MipsDecoder::decode(encR(0, 4, 0, 0, 0, 0x11));
    EXPECT_EQ(inst.id, InstrId::MTHI);

    inst = MipsDecoder::decode(encR(0, 4, 0, 0, 0, 0x13));
    EXPECT_EQ(inst.id, InstrId::MTLO);
}

// I-type ALU

TEST(MipsDecoder, ALU_I_Type) {
    // ADDIU $v0, $a0, -16
    auto inst = MipsDecoder::decode(encI(0x09, 4, 2, static_cast<uint16_t>(-16)));
    EXPECT_EQ(inst.id, InstrId::ADDIU);
    EXPECT_EQ(inst.category, InstrCategory::ALU);
    EXPECT_EQ(inst.rs, 4);
    EXPECT_EQ(inst.rt, 2);
    EXPECT_EQ(inst.imm16, -16);

    // ADDI
    inst = MipsDecoder::decode(encI(0x08, 4, 2, 100));
    EXPECT_EQ(inst.id, InstrId::ADDI);
    EXPECT_EQ(inst.imm16, 100);

    // ANDI
    inst = MipsDecoder::decode(encI(0x0C, 4, 2, 0xFF));
    EXPECT_EQ(inst.id, InstrId::ANDI);
    EXPECT_EQ(inst.imm16, static_cast<int16_t>(0xFF));

    // ORI
    inst = MipsDecoder::decode(encI(0x0D, 4, 2, 0x1234));
    EXPECT_EQ(inst.id, InstrId::ORI);

    // XORI
    inst = MipsDecoder::decode(encI(0x0E, 4, 2, 0x5678));
    EXPECT_EQ(inst.id, InstrId::XORI);

    // SLTI
    inst = MipsDecoder::decode(encI(0x0A, 4, 2, 42));
    EXPECT_EQ(inst.id, InstrId::SLTI);

    // SLTIU
    inst = MipsDecoder::decode(encI(0x0B, 4, 2, 42));
    EXPECT_EQ(inst.id, InstrId::SLTIU);

    // LUI $v0, 0x8001
    inst = MipsDecoder::decode(encI(0x0F, 0, 2, 0x8001));
    EXPECT_EQ(inst.id, InstrId::LUI);
    EXPECT_EQ(inst.rt, 2);
}

// Loads

TEST(MipsDecoder, Loads) {
    // LW $v0, 16($sp) -- opcode 0x23
    auto inst = MipsDecoder::decode(encI(0x23, 29, 2, 16));
    EXPECT_EQ(inst.id, InstrId::LW);
    EXPECT_EQ(inst.category, InstrCategory::Memory);
    EXPECT_TRUE(inst.isLoad());
    EXPECT_FALSE(inst.isStore());
    EXPECT_EQ(inst.rs, 29); // $sp
    EXPECT_EQ(inst.rt, 2);  // $v0
    EXPECT_EQ(inst.imm16, 16);

    // LB
    inst = MipsDecoder::decode(encI(0x20, 4, 2, 0));
    EXPECT_EQ(inst.id, InstrId::LB);
    EXPECT_TRUE(inst.isLoad());

    // LBU
    inst = MipsDecoder::decode(encI(0x24, 4, 2, 0));
    EXPECT_EQ(inst.id, InstrId::LBU);

    // LH
    inst = MipsDecoder::decode(encI(0x21, 4, 2, 0));
    EXPECT_EQ(inst.id, InstrId::LH);

    // LHU
    inst = MipsDecoder::decode(encI(0x25, 4, 2, 0));
    EXPECT_EQ(inst.id, InstrId::LHU);

    // LWL
    inst = MipsDecoder::decode(encI(0x22, 4, 2, 0));
    EXPECT_EQ(inst.id, InstrId::LWL);

    // LWR
    inst = MipsDecoder::decode(encI(0x26, 4, 2, 0));
    EXPECT_EQ(inst.id, InstrId::LWR);
}

// Stores

TEST(MipsDecoder, Stores) {
    // SW $v0, 16($sp) -- opcode 0x2B
    auto inst = MipsDecoder::decode(encI(0x2B, 29, 2, 16));
    EXPECT_EQ(inst.id, InstrId::SW);
    EXPECT_EQ(inst.category, InstrCategory::Memory);
    EXPECT_TRUE(inst.isStore());
    EXPECT_FALSE(inst.isLoad());

    // SB
    inst = MipsDecoder::decode(encI(0x28, 4, 2, 0));
    EXPECT_EQ(inst.id, InstrId::SB);
    EXPECT_TRUE(inst.isStore());

    // SH
    inst = MipsDecoder::decode(encI(0x29, 4, 2, 0));
    EXPECT_EQ(inst.id, InstrId::SH);

    // SWL
    inst = MipsDecoder::decode(encI(0x2A, 4, 2, 0));
    EXPECT_EQ(inst.id, InstrId::SWL);

    // SWR
    inst = MipsDecoder::decode(encI(0x2E, 4, 2, 0));
    EXPECT_EQ(inst.id, InstrId::SWR);
}

// Branches

TEST(MipsDecoder, Branches) {
    // BEQ $a0, $a1, +16
    auto inst = MipsDecoder::decode(encI(0x04, 4, 5, 4));
    EXPECT_EQ(inst.id, InstrId::BEQ);
    EXPECT_EQ(inst.category, InstrCategory::Branch);
    EXPECT_TRUE(inst.isBranch());
    EXPECT_TRUE(inst.hasBranchDelaySlot());
    EXPECT_EQ(inst.rs, 4);
    EXPECT_EQ(inst.rt, 5);

    // Branch target: PC + 4 + (imm16 << 2)
    EXPECT_EQ(inst.branchTarget(0x80010000), 0x80010014); // +4 + 16

    // BNE
    inst = MipsDecoder::decode(encI(0x05, 4, 5, 4));
    EXPECT_EQ(inst.id, InstrId::BNE);

    // BLEZ
    inst = MipsDecoder::decode(encI(0x06, 4, 0, 4));
    EXPECT_EQ(inst.id, InstrId::BLEZ);

    // BGTZ
    inst = MipsDecoder::decode(encI(0x07, 4, 0, 4));
    EXPECT_EQ(inst.id, InstrId::BGTZ);
}

TEST(MipsDecoder, Branches_REGIMM) {
    // BLTZ $a0, +8 -- opcode=1, rt=0
    auto inst = MipsDecoder::decode(encI(0x01, 4, 0x00, 2));
    EXPECT_EQ(inst.id, InstrId::BLTZ);
    EXPECT_EQ(inst.category, InstrCategory::Branch);

    // BGEZ -- rt=1
    inst = MipsDecoder::decode(encI(0x01, 4, 0x01, 2));
    EXPECT_EQ(inst.id, InstrId::BGEZ);

    // BLTZAL -- rt=0x10
    inst = MipsDecoder::decode(encI(0x01, 4, 0x10, 2));
    EXPECT_EQ(inst.id, InstrId::BLTZAL);

    // BGEZAL -- rt=0x11
    inst = MipsDecoder::decode(encI(0x01, 4, 0x11, 2));
    EXPECT_EQ(inst.id, InstrId::BGEZAL);
}

// Jumps

TEST(MipsDecoder, Jumps) {
    // J 0x00004000 (target in word units)
    auto inst = MipsDecoder::decode(encJ(0x02, 0x00004000));
    EXPECT_EQ(inst.id, InstrId::J);
    EXPECT_EQ(inst.category, InstrCategory::Jump);
    EXPECT_TRUE(inst.isJump());
    EXPECT_TRUE(inst.hasBranchDelaySlot());
    EXPECT_EQ(inst.target26, 0x00004000u);
    // Jump target: (PC & 0xF0000000) | (target26 << 2)
    EXPECT_EQ(inst.jumpTarget(0x80010000), 0x80010000u);

    // JAL
    inst = MipsDecoder::decode(encJ(0x03, 0x00004100));
    EXPECT_EQ(inst.id, InstrId::JAL);
    EXPECT_EQ(inst.jumpTarget(0x80000000), 0x80010400u);

    // JR $ra -- funct=0x08, rs=31
    inst = MipsDecoder::decode(encR(0, 31, 0, 0, 0, 0x08));
    EXPECT_EQ(inst.id, InstrId::JR);
    EXPECT_EQ(inst.rs, 31); // $ra

    // JALR $t9 -- funct=0x09, rs=25, rd=31
    inst = MipsDecoder::decode(encR(0, 25, 0, 31, 0, 0x09));
    EXPECT_EQ(inst.id, InstrId::JALR);
    EXPECT_EQ(inst.rs, 25); // $t9
    EXPECT_EQ(inst.rd, 31); // $ra
}

// System

TEST(MipsDecoder, System) {
    // SYSCALL -- funct=0x0C
    auto inst = MipsDecoder::decode(encR(0, 0, 0, 0, 0, 0x0C));
    EXPECT_EQ(inst.id, InstrId::SYSCALL);
    EXPECT_EQ(inst.category, InstrCategory::System);

    // BREAK -- funct=0x0D
    inst = MipsDecoder::decode(encR(0, 0, 0, 0, 0, 0x0D));
    EXPECT_EQ(inst.id, InstrId::BREAK);
}

// COP0

TEST(MipsDecoder, COP0) {
    // MFC0 $v0, SR(12) -- opcode=0x10, rs=0x00, rt=2, rd=12
    auto inst = MipsDecoder::decode(encR(0x10, 0x00, 2, 12, 0, 0));
    EXPECT_EQ(inst.id, InstrId::MFC0);
    EXPECT_EQ(inst.category, InstrCategory::COP0);
    EXPECT_EQ(inst.rt, 2);
    EXPECT_EQ(inst.rd, 12);

    // MTC0 $v0, SR(12) -- rs=0x04
    inst = MipsDecoder::decode(encR(0x10, 0x04, 2, 12, 0, 0));
    EXPECT_EQ(inst.id, InstrId::MTC0);

    // RFE -- opcode=0x10, rs=0x10, funct=0x10
    inst = MipsDecoder::decode(encR(0x10, 0x10, 0, 0, 0, 0x10));
    EXPECT_EQ(inst.id, InstrId::RFE);
}

// COP2 / GTE Register Moves

TEST(MipsDecoder, GTE_RegisterMoves) {
    // MFC2 $v0, r0 -- opcode=0x12, rs=0x00, rt=2, rd=0
    auto inst = MipsDecoder::decode(encR(0x12, 0x00, 2, 0, 0, 0));
    EXPECT_EQ(inst.id, InstrId::MFC2);
    EXPECT_EQ(inst.category, InstrCategory::GTE);

    // MTC2 $v0, r1 -- rs=0x04
    inst = MipsDecoder::decode(encR(0x12, 0x04, 2, 1, 0, 0));
    EXPECT_EQ(inst.id, InstrId::MTC2);

    // CFC2 $v0, cr0 -- rs=0x02
    inst = MipsDecoder::decode(encR(0x12, 0x02, 2, 0, 0, 0));
    EXPECT_EQ(inst.id, InstrId::CFC2);

    // CTC2 $v0, cr0 -- rs=0x06
    inst = MipsDecoder::decode(encR(0x12, 0x06, 2, 0, 0, 0));
    EXPECT_EQ(inst.id, InstrId::CTC2);

    // LWC2 rt, offset(base) -- opcode=0x32
    inst = MipsDecoder::decode(encI(0x32, 4, 2, 100));
    EXPECT_EQ(inst.id, InstrId::LWC2);
    EXPECT_EQ(inst.category, InstrCategory::GTE);

    // SWC2 -- opcode=0x3A
    inst = MipsDecoder::decode(encI(0x3A, 4, 2, 100));
    EXPECT_EQ(inst.id, InstrId::SWC2);
}

// COP2 / GTE Commands

TEST(MipsDecoder, GTE_Commands) {
    // GTE command encoding: opcode=0x12, bit25=1, cmd in bits 0-5
    auto gteCmd = [](uint8_t cmd) -> uint32_t {
        return (0x12u << 26) | (1u << 25) | cmd;
    };

    auto inst = MipsDecoder::decode(gteCmd(0x01));
    EXPECT_EQ(inst.id, InstrId::GTE_RTPS);
    EXPECT_EQ(inst.category, InstrCategory::GTE);

    inst = MipsDecoder::decode(gteCmd(0x06));
    EXPECT_EQ(inst.id, InstrId::GTE_NCLIP);

    inst = MipsDecoder::decode(gteCmd(0x0C));
    EXPECT_EQ(inst.id, InstrId::GTE_OP);

    inst = MipsDecoder::decode(gteCmd(0x10));
    EXPECT_EQ(inst.id, InstrId::GTE_DPCS);

    inst = MipsDecoder::decode(gteCmd(0x11));
    EXPECT_EQ(inst.id, InstrId::GTE_INTPL);

    inst = MipsDecoder::decode(gteCmd(0x12));
    EXPECT_EQ(inst.id, InstrId::GTE_MVMVA);

    inst = MipsDecoder::decode(gteCmd(0x13));
    EXPECT_EQ(inst.id, InstrId::GTE_NCDS);

    inst = MipsDecoder::decode(gteCmd(0x14));
    EXPECT_EQ(inst.id, InstrId::GTE_CDP);

    inst = MipsDecoder::decode(gteCmd(0x16));
    EXPECT_EQ(inst.id, InstrId::GTE_NCDT);

    inst = MipsDecoder::decode(gteCmd(0x1B));
    EXPECT_EQ(inst.id, InstrId::GTE_NCCS);

    inst = MipsDecoder::decode(gteCmd(0x1C));
    EXPECT_EQ(inst.id, InstrId::GTE_CC);

    inst = MipsDecoder::decode(gteCmd(0x1E));
    EXPECT_EQ(inst.id, InstrId::GTE_NCS);

    inst = MipsDecoder::decode(gteCmd(0x20));
    EXPECT_EQ(inst.id, InstrId::GTE_NCT);

    inst = MipsDecoder::decode(gteCmd(0x28));
    EXPECT_EQ(inst.id, InstrId::GTE_SQR);

    inst = MipsDecoder::decode(gteCmd(0x29));
    EXPECT_EQ(inst.id, InstrId::GTE_DCPL);

    inst = MipsDecoder::decode(gteCmd(0x2A));
    EXPECT_EQ(inst.id, InstrId::GTE_DPCT);

    inst = MipsDecoder::decode(gteCmd(0x2D));
    EXPECT_EQ(inst.id, InstrId::GTE_AVSZ3);

    inst = MipsDecoder::decode(gteCmd(0x2E));
    EXPECT_EQ(inst.id, InstrId::GTE_AVSZ4);

    inst = MipsDecoder::decode(gteCmd(0x30));
    EXPECT_EQ(inst.id, InstrId::GTE_RTPT);

    inst = MipsDecoder::decode(gteCmd(0x3D));
    EXPECT_EQ(inst.id, InstrId::GTE_GPF);

    inst = MipsDecoder::decode(gteCmd(0x3E));
    EXPECT_EQ(inst.id, InstrId::GTE_GPL);

    inst = MipsDecoder::decode(gteCmd(0x3F));
    EXPECT_EQ(inst.id, InstrId::GTE_NCCT);
}

// Instruction Helpers

TEST(MipsDecoder, BranchTarget) {
    // BEQ with imm16 = 10 -> target = PC + 4 + 40
    auto inst = MipsDecoder::decode(encI(0x04, 4, 5, 10));
    EXPECT_EQ(inst.branchTarget(0x80010000), 0x8001002Cu);

    // Negative offset: imm16 = -4 -> target = PC + 4 + (-4 << 2) = PC + 4 - 16
    inst = MipsDecoder::decode(encI(0x04, 4, 5, static_cast<uint16_t>(-4)));
    EXPECT_EQ(inst.branchTarget(0x80010000), 0x8000FFF4u); // 0x80010000 + 4 - 16
}

TEST(MipsDecoder, JumpTarget) {
    // J target=0x00020400 -> full = (PC & 0xF0000000) | (0x00020400 << 2)
    auto inst = MipsDecoder::decode(encJ(0x02, 0x00020400));
    EXPECT_EQ(inst.jumpTarget(0x80000000), 0x80081000u);
}

TEST(MipsDecoder, DelaySlotDetection) {
    // J has delay slot
    EXPECT_TRUE(MipsDecoder::decode(encJ(0x02, 0)).hasBranchDelaySlot());
    // JAL has delay slot
    EXPECT_TRUE(MipsDecoder::decode(encJ(0x03, 0)).hasBranchDelaySlot());
    // JR $ra has delay slot
    EXPECT_TRUE(MipsDecoder::decode(encR(0, 31, 0, 0, 0, 0x08)).hasBranchDelaySlot());
    // BEQ has delay slot
    EXPECT_TRUE(MipsDecoder::decode(encI(0x04, 0, 0, 0)).hasBranchDelaySlot());
    // LW does NOT have delay slot
    EXPECT_FALSE(MipsDecoder::decode(encI(0x23, 29, 2, 0)).hasBranchDelaySlot());
    // ADDU does NOT
    EXPECT_FALSE(MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x21)).hasBranchDelaySlot());
}

// Register Names

TEST(MipsDecoder, RegisterNames) {
    EXPECT_EQ(gprName(0),  "$zero");
    EXPECT_EQ(gprName(1),  "$at");
    EXPECT_EQ(gprName(2),  "$v0");
    EXPECT_EQ(gprName(4),  "$a0");
    EXPECT_EQ(gprName(8),  "$t0");
    EXPECT_EQ(gprName(16), "$s0");
    EXPECT_EQ(gprName(29), "$sp");
    EXPECT_EQ(gprName(31), "$ra");
    EXPECT_EQ(gprName(32), "$??"); // out of range

    EXPECT_EQ(cop0Name(12), "SR");
    EXPECT_EQ(cop0Name(13), "Cause");
    EXPECT_EQ(cop0Name(14), "EPC");
}

// Name Tables

TEST(MipsDecoder, InstrNames) {
    EXPECT_EQ(MipsDecoder::instrName(InstrId::ADDU), "ADDU");
    EXPECT_EQ(MipsDecoder::instrName(InstrId::LW), "LW");
    EXPECT_EQ(MipsDecoder::instrName(InstrId::BEQ), "BEQ");
    EXPECT_EQ(MipsDecoder::instrName(InstrId::JAL), "JAL");
    EXPECT_EQ(MipsDecoder::instrName(InstrId::GTE_RTPS), "RTPS");
    EXPECT_EQ(MipsDecoder::instrName(InstrId::MFC0), "MFC0");

    EXPECT_EQ(MipsDecoder::categoryName(InstrCategory::ALU), "ALU");
    EXPECT_EQ(MipsDecoder::categoryName(InstrCategory::GTE), "GTE");
    EXPECT_EQ(MipsDecoder::categoryName(InstrCategory::Branch), "Branch");
}

// Invalid Instructions

TEST(MipsDecoder, InvalidInstructions) {
    // Unknown primary opcode (0x3F is unused in MIPS I)
    auto inst = MipsDecoder::decode(0xFC000000);
    EXPECT_EQ(inst.id, InstrId::INVALID);
    EXPECT_FALSE(inst.isValid());

    // Unknown SPECIAL funct
    inst = MipsDecoder::decode(encR(0, 0, 0, 0, 0, 0x3F));
    EXPECT_EQ(inst.id, InstrId::INVALID);

    // Unknown REGIMM
    inst = MipsDecoder::decode(encI(0x01, 4, 0x1F, 0));
    EXPECT_EQ(inst.id, InstrId::INVALID);
}
