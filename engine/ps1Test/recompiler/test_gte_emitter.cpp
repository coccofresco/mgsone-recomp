// Tests for ps1Recomp -- GTE Emitter
// Validates COP2/GTE -> C++ translation

#include <gtest/gtest.h>
#include <ps1recomp/gte_emitter.h>
#include <ps1recomp/mips_decoder.h>

using namespace ps1recomp;

// Encoding helpers

static constexpr uint32_t encCOP2Reg(uint8_t sub, uint8_t rt, uint8_t rd) {
  // COP2 register move: opcode=0x12, sub=MF/MT/CF/CT (bits 25-21), rt, rd
  return (0x12u << 26) | (uint32_t(sub) << 21) | (uint32_t(rt) << 16) |
         (uint32_t(rd) << 11);
}

static constexpr uint32_t encCOP2Cmd(uint8_t funct, bool sf = false,
                                     bool lm = false) {
  // COP2 command: opcode=0x12, bit25=1 (CO), funct in bits 0-5
  // sf = bit 19, lm = bit 10
  return (0x12u << 26) | (1u << 25) | (uint32_t(sf) << 19) |
         (uint32_t(lm) << 10) | uint32_t(funct);
}

static constexpr uint32_t encMVMVA(uint8_t mx, uint8_t mv, uint8_t tv, bool sf,
                                   bool lm) {
  return (0x12u << 26) | (1u << 25) | (uint32_t(sf) << 19) |
         (uint32_t(mx) << 17) | (uint32_t(mv) << 15) | (uint32_t(tv) << 13) |
         (uint32_t(lm) << 10) | 0x12u;
}

static constexpr uint32_t encLWC2(uint8_t rt, uint8_t base, int16_t offset) {
  return (0x32u << 26) | (uint32_t(base) << 21) | (uint32_t(rt) << 16) |
         (uint16_t(offset));
}

static constexpr uint32_t encSWC2(uint8_t rt, uint8_t base, int16_t offset) {
  return (0x3Au << 26) | (uint32_t(base) << 21) | (uint32_t(rt) << 16) |
         (uint16_t(offset));
}

// Register Name Tests

TEST(GteEmitter, DataRegNames) {
  EXPECT_EQ(gteDataRegName(0), "VXY0");
  EXPECT_EQ(gteDataRegName(6), "RGBC");
  EXPECT_EQ(gteDataRegName(7), "OTZ");
  EXPECT_EQ(gteDataRegName(8), "IR0");
  EXPECT_EQ(gteDataRegName(9), "IR1");
  EXPECT_EQ(gteDataRegName(12), "SXY0");
  EXPECT_EQ(gteDataRegName(15), "SXYP");
  EXPECT_EQ(gteDataRegName(24), "MAC0");
  EXPECT_EQ(gteDataRegName(30), "LZCS");
  EXPECT_EQ(gteDataRegName(31), "LZCR");
}

TEST(GteEmitter, ControlRegNames) {
  EXPECT_EQ(gteControlRegName(0), "RT11RT12");
  EXPECT_EQ(gteControlRegName(5), "TRX");
  EXPECT_EQ(gteControlRegName(13), "RBK");
  EXPECT_EQ(gteControlRegName(24), "OFX");
  EXPECT_EQ(gteControlRegName(26), "H");
  EXPECT_EQ(gteControlRegName(31), "FLAG");
}

// Register Move Tests

TEST(GteEmitter, MFC2) {
  GteEmitter emitter;
  // MFC2 $v0, cop2d[8] (IR0)
  auto inst = MipsDecoder::decode(encCOP2Reg(0x00, 2, 8));
  auto code = emitter.emit(inst, 0);
  EXPECT_NE(code.find("pgxp_mfc2"), std::string::npos);
  EXPECT_NE(code.find("pgxp_invalidate_gpr"), std::string::npos);
  EXPECT_NE(code.find("ctx->r2"), std::string::npos);
  EXPECT_NE(code.find("IR0"), std::string::npos);
}

TEST(GteEmitter, MTC2) {
  GteEmitter emitter;
  // MTC2 $a0, cop2d[9] (IR1)
  auto inst = MipsDecoder::decode(encCOP2Reg(0x04, 4, 9));
  auto code = emitter.emit(inst, 0);
  EXPECT_NE(code.find("pgxp_mtc2"), std::string::npos);
  EXPECT_NE(code.find("ctx->r4"), std::string::npos);
  EXPECT_NE(code.find("IR1"), std::string::npos);
}

TEST(GteEmitter, CFC2) {
  GteEmitter emitter;
  // CFC2 $v0, cop2c[31] (FLAG)
  auto inst = MipsDecoder::decode(encCOP2Reg(0x02, 2, 31));
  auto code = emitter.emit(inst, 0);
  EXPECT_NE(code.find("gte_read_control"), std::string::npos);
  EXPECT_NE(code.find("pgxp_invalidate_gpr"), std::string::npos);
  EXPECT_NE(code.find("FLAG"), std::string::npos);
}

TEST(GteEmitter, CTC2) {
  GteEmitter emitter;
  // CTC2 $a0, cop2c[5] (TRX)
  auto inst = MipsDecoder::decode(encCOP2Reg(0x06, 4, 5));
  auto code = emitter.emit(inst, 0);
  EXPECT_NE(code.find("gte_write_control"), std::string::npos);
  EXPECT_NE(code.find("TRX"), std::string::npos);
}

TEST(GteEmitter, LWC2) {
  GteEmitter emitter;
  // LWC2 cop2d[9], 16($sp) -> load word to GTE data reg IR1
  auto inst = MipsDecoder::decode(encLWC2(9, 29, 16));
  auto code = emitter.emit(inst, 0);
  EXPECT_NE(code.find("pgxp_lwc2"), std::string::npos);
  EXPECT_NE(code.find("IR1"), std::string::npos);
}

TEST(GteEmitter, SWC2) {
  GteEmitter emitter;
  // SWC2 cop2d[12], 0($a0) -> store GTE SXY0 to memory
  auto inst = MipsDecoder::decode(encSWC2(12, 4, 0));
  auto code = emitter.emit(inst, 0);
  EXPECT_NE(code.find("pgxp_swc2"), std::string::npos);
  EXPECT_NE(code.find("SXY0"), std::string::npos);
}

// GTE Command Tests

TEST(GteEmitter, RTPS) {
  GteEmitter emitter;
  auto inst = MipsDecoder::decode(encCOP2Cmd(0x01, true, false));
  auto code = emitter.emit(inst, 0);
  EXPECT_NE(code.find("gte_RTPS"), std::string::npos);
  EXPECT_NE(code.find("15 cycles"), std::string::npos);
}

TEST(GteEmitter, NCLIP) {
  GteEmitter emitter;
  auto inst = MipsDecoder::decode(encCOP2Cmd(0x06));
  auto code = emitter.emit(inst, 0);
  EXPECT_NE(code.find("gte_NCLIP"), std::string::npos);
}

TEST(GteEmitter, RTPT) {
  GteEmitter emitter;
  auto inst = MipsDecoder::decode(encCOP2Cmd(0x30, true, false));
  auto code = emitter.emit(inst, 0);
  EXPECT_NE(code.find("gte_RTPT"), std::string::npos);
  EXPECT_NE(code.find("23 cycles"), std::string::npos);
}

TEST(GteEmitter, AllGteCommandsGenerateOutput) {
  GteEmitter emitter;

  // All 22 GTE command functcodes
  uint8_t cmds[] = {0x01, 0x06, 0x0C, 0x10, 0x11, 0x12, 0x13, 0x14,
                    0x16, 0x1B, 0x1C, 0x1E, 0x20, 0x28, 0x29, 0x2A,
                    0x2D, 0x2E, 0x30, 0x3D, 0x3E, 0x3F};

  for (auto funct : cmds) {
    auto inst = MipsDecoder::decode(encCOP2Cmd(funct));
    auto code = emitter.emit(inst, 0);
    // Should contain "gte_" prefix and "(ctx,"
    EXPECT_NE(code.find("gte_"), std::string::npos)
        << "Failed for funct 0x" << std::hex << (int)funct;
    EXPECT_NE(code.find("(ctx,"), std::string::npos)
        << "Failed for funct 0x" << std::hex << (int)funct;
  }
}

// MVMVA Special Handling

TEST(GteEmitter, MVMVA_Flags) {
  GteEmitter emitter;
  // MVMVA: mx=1 (Light), mv=2 (V2), tv=0 (TR), sf=1, lm=0
  auto inst = MipsDecoder::decode(encMVMVA(1, 2, 0, true, false));
  auto code = emitter.emit(inst, 0);
  EXPECT_NE(code.find("gte_MVMVA"), std::string::npos);
  EXPECT_NE(code.find("Light"), std::string::npos);
  EXPECT_NE(code.find("V2"), std::string::npos);
  EXPECT_NE(code.find("TR"), std::string::npos);
}

TEST(GteEmitter, MVMVA_DifferentFlags) {
  GteEmitter emitter;
  // MVMVA: mx=0 (Rotation), mv=3 (IR), tv=1 (BK), sf=0, lm=1
  auto inst = MipsDecoder::decode(encMVMVA(0, 3, 1, false, true));
  auto code = emitter.emit(inst, 0);
  EXPECT_NE(code.find("gte_MVMVA"), std::string::npos);
  EXPECT_NE(code.find("Rotation"), std::string::npos);
  EXPECT_NE(code.find("IR"), std::string::npos);
  EXPECT_NE(code.find("BK"), std::string::npos);
}

// GteCommandInfo Lookup

TEST(GteEmitter, CommandInfoLookup) {
  auto *info = getGteCommandInfo(InstrId::GTE_RTPS);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->cmd, 0x01);
  EXPECT_STREQ(info->name, "RTPS");
  EXPECT_EQ(info->cycles, 15);

  info = getGteCommandInfo(InstrId::GTE_NCDT);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->cycles, 44);

  // Non-GTE instruction should return nullptr
  EXPECT_EQ(getGteCommandInfo(InstrId::ADD), nullptr);
}

// sf/lm Flag Extraction

TEST(GteEmitter, SfLmFlags) {
  GteEmitter emitter;

  // RTPS with sf=1, lm=1
  auto inst = MipsDecoder::decode(encCOP2Cmd(0x01, true, true));
  auto code = emitter.emit(inst, 0);
  EXPECT_NE(code.find("gte_RTPS(ctx, 1, 1)"), std::string::npos);

  // RTPS with sf=0, lm=0
  inst = MipsDecoder::decode(encCOP2Cmd(0x01, false, false));
  code = emitter.emit(inst, 0);
  EXPECT_NE(code.find("gte_RTPS(ctx, 0, 0)"), std::string::npos);
}

// Non-GTE Rejection

TEST(GteEmitter, NonGteRejected) {
  GteEmitter emitter;
  // ADDU -- not a GTE instruction
  uint32_t raw = (0u << 26) | (4u << 21) | (5u << 16) | (2u << 11) | 0x21u;
  auto inst = MipsDecoder::decode(raw);
  auto code = emitter.emit(inst, 0);
  EXPECT_NE(code.find("NOT GTE"), std::string::npos);
}
