#include <gtest/gtest.h>
#include <ps1recomp/overlay_scanner.h>
#include <cstring>

using namespace ps1recomp;

// Helper: build a minimal PS-X EXE blob

static std::vector<uint8_t> makePsxExe(uint32_t pc, uint32_t tAddr,
                                        uint32_t tSize,
                                        const std::vector<uint32_t> &code = {}) {
  std::vector<uint8_t> data(2048 + tSize, 0);
  // Magic
  std::memcpy(data.data(), "PS-X EXE", 8);
  // pc0 at offset 16
  std::memcpy(data.data() + 16, &pc, 4);
  // tAddr at offset 24
  std::memcpy(data.data() + 24, &tAddr, 4);
  // tSize at offset 28
  std::memcpy(data.data() + 28, &tSize, 4);

  // Fill code section
  for (size_t i = 0; i < code.size() && i * 4 < tSize; ++i) {
    std::memcpy(data.data() + 2048 + i * 4, &code[i], 4);
  }
  return data;
}

// Helper: build MIPS code buffer

static std::vector<uint8_t> makeMipsCode(const std::vector<uint32_t> &words) {
  std::vector<uint8_t> data(words.size() * 4);
  for (size_t i = 0; i < words.size(); ++i) {
    std::memcpy(data.data() + i * 4, &words[i], 4);
  }
  return data;
}

// PsxExeHeader tests

TEST(OverlayScannerTest, ParsePsxExeHeaderValid) {
  auto exe = makePsxExe(0x80010000, 0x80010000, 4096);
  auto hdr = OverlayScanner::parsePsxExeHeader(exe);
  ASSERT_TRUE(hdr.has_value());
  EXPECT_EQ(hdr->pc0, 0x80010000u);
  EXPECT_EQ(hdr->tAddr, 0x80010000u);
  EXPECT_EQ(hdr->tSize, 4096u);
}

TEST(OverlayScannerTest, ParsePsxExeHeaderInvalidMagic) {
  std::vector<uint8_t> data(2048, 0);
  auto hdr = OverlayScanner::parsePsxExeHeader(data);
  EXPECT_FALSE(hdr.has_value());
}

TEST(OverlayScannerTest, ParsePsxExeHeaderTooSmall) {
  std::vector<uint8_t> data(16, 0);
  auto hdr = OverlayScanner::parsePsxExeHeader(data);
  EXPECT_FALSE(hdr.has_value());
}

// MIPS Score tests

TEST(OverlayScannerTest, MipsScoreAllNop) {
  // All NOPs -- valid but heavily penalized
  std::vector<uint8_t> data(256, 0);
  float score = OverlayScanner::computeMipsScore(data.data(), data.size());
  // All valid (NOPs) but >50% NOPs so penalized by 0.5x
  EXPECT_LE(score, 0.55f);
}

TEST(OverlayScannerTest, MipsScoreValidCode) {
  // Mix of valid MIPS I instructions
  std::vector<uint32_t> code = {
      0x27BDFFF0, // addiu $sp, $sp, -16  (prologue)
      0xAFBF000C, // sw $ra, 12($sp)
      0x3C028001, // lui $v0, 0x8001
      0x8C420100, // lw $v0, 0x100($v0)
      0x00021040, // sll $v0, $v0, 1
      0x24420001, // addiu $v0, $v0, 1
      0x8FBF000C, // lw $ra, 12($sp)
      0x27BD0010, // addiu $sp, $sp, 16  (epilogue)
      0x03E00008, // jr $ra
      0x00000000, // nop (delay slot)
  };
  auto data = makeMipsCode(code);
  float score = OverlayScanner::computeMipsScore(data.data(), data.size());
  EXPECT_GE(score, 0.8f);
}

TEST(OverlayScannerTest, MipsScoreRandomData) {
  // Data using opcodes in the reserved/unused range (opcode 0x3x series without
  // valid MIPS I mappings, 0x1B-0x1F, etc.)
  std::vector<uint32_t> garbage = {
      0xFC000000, 0xF8000000, 0xF4000000, 0xF0000000,
      0xEC000000, 0xE8000000, 0xE4000000, 0xE0000000,
      0xDC000000, 0xD8000000, 0xD4000000, 0xD0000000,
      0xCC000000, 0xC8000000, 0xC4000000, 0xC0000000,
  };
  auto data = makeMipsCode(garbage);
  float score = OverlayScanner::computeMipsScore(data.data(), data.size());
  EXPECT_LT(score, 0.5f);
}

TEST(OverlayScannerTest, MipsScoreEmptyData) {
  float score = OverlayScanner::computeMipsScore(nullptr, 0);
  EXPECT_EQ(score, 0.0f);
}

// Function detection tests

TEST(OverlayScannerTest, DetectFunctionPrologues) {
  std::vector<uint32_t> code = {
      0x27BDFFF0, // addiu $sp, $sp, -16  (func1)
      0xAFBF000C, // sw $ra, 12($sp)
      0x00000000, // nop
      0x8FBF000C, // lw $ra, 12($sp)
      0x27BD0010, // addiu $sp, $sp, 16
      0x03E00008, // jr $ra
      0x00000000, // nop
      0x27BDFFE0, // addiu $sp, $sp, -32  (func2)
      0xAFBF001C, // sw $ra, 28($sp)
      0x00000000, // nop
      0x8FBF001C, // lw $ra, 28($sp)
      0x27BD0020, // addiu $sp, $sp, 32
      0x03E00008, // jr $ra
      0x00000000, // nop
  };
  auto data = makeMipsCode(code);

  auto [score, addrs] = OverlayScanner::detectFunctions(
      data.data(), data.size(), 0x80010000);

  EXPECT_GE(addrs.size(), 2u);
  if (addrs.size() >= 2) {
    EXPECT_EQ(addrs[0], 0x80010000u);
    EXPECT_EQ(addrs[1], 0x8001001Cu);
  }
}

TEST(OverlayScannerTest, DetectEntryBeforeDelayedPrologue) {
  std::vector<uint32_t> code = {
      0x03E00008, // jr $ra
      0x00000000, // delay slot
      0x3C028007, // lui $v0, 0x8007 -- real function entry
      0x8C4323C4, // lw $v1, 0x23c4($v0)
      0x27BDFFE0, // addiu $sp, $sp, -32 -- delayed prologue
      0xAFBF0018, // sw $ra, 24($sp)
      0x03E00008, // jr $ra
      0x27BD0020, // delay slot
  };
  auto data = makeMipsCode(code);

  auto [score, addrs] = OverlayScanner::detectFunctions(
      data.data(), data.size(), 0x8006EE44);
  (void)score;

  ASSERT_EQ(addrs.size(), 1u);
  EXPECT_EQ(addrs[0], 0x8006EE4Cu);
}

// RAM base inference tests

TEST(OverlayScannerTest, InferRamBaseFromLuiOri) {
  std::vector<uint32_t> code = {
      0x3C028004, // lui $v0, 0x8004
      0x34420100, // ori $v0, $v0, 0x0100  -> 0x80040100
      0x3C038004, // lui $v1, 0x8004
      0x34630200, // ori $v1, $v1, 0x0200  -> 0x80040200
      0x3C048004, // lui $a0, 0x8004
      0x34840300, // ori $a0, $a0, 0x0300  -> 0x80040300
      // Some code
      0x00000000,
      0x03E00008,
  };
  auto data = makeMipsCode(code);
  uint32_t base = OverlayScanner::inferRamBase(data.data(), data.size());
  // Should detect 0x8004xxxx region, base around 0x80040000
  EXPECT_GE(base, 0x80040000u);
  EXPECT_LE(base, 0x80040100u);
}

TEST(OverlayScannerTest, InferRamBaseFromLuiAddiu) {
  std::vector<uint32_t> code = {
      0x3C028005, // lui $v0, 0x8005
      0x24420800, // addiu $v0, $v0, 0x0800  -> 0x80050800
      0x3C038005, // lui $v1, 0x8005
      0x24631000, // addiu $v1, $v1, 0x1000  -> 0x80051000
      0x00000000,
      0x03E00008,
  };
  auto data = makeMipsCode(code);
  uint32_t base = OverlayScanner::inferRamBase(data.data(), data.size());
  EXPECT_GE(base, 0x80050000u);
  EXPECT_LE(base, 0x80050800u);
}

TEST(OverlayScannerTest, InferRamBaseNoCode) {
  uint32_t base = OverlayScanner::inferRamBase(nullptr, 0);
  EXPECT_EQ(base, 0u);
}

// ScanFile tests

TEST(OverlayScannerTest, ScanFilePsxExe) {
  std::vector<uint32_t> code = {
      0x27BDFFF0, // addiu $sp, $sp, -16
      0xAFBF000C, // sw $ra, 12($sp)
      0x3C028001, // lui $v0, 0x8001
      0x8C420100, // lw $v0, 0x100($v0)
      0x8FBF000C, // lw $ra, 12($sp)
      0x27BD0010, // addiu $sp, $sp, 16
      0x03E00008, // jr $ra
      0x00000000, // nop
  };
  auto exe = makePsxExe(0x80020000, 0x80020000, 256, code);

  OverlayScanner scanner;
  auto result = scanner.scanFile(exe, "OVERLAY.BIN");

  EXPECT_TRUE(result.hasPsxExeHeader);
  EXPECT_EQ(result.ramBase, 0x80020000u);
  EXPECT_EQ(result.codeSize, 256u);
  EXPECT_TRUE(result.isLikelyCode());
}

TEST(OverlayScannerTest, ScanFileRawMips) {
  std::vector<uint32_t> code;
  // Generate ~1KB of realistic MIPS code
  for (int i = 0; i < 8; ++i) {
    code.push_back(0x27BDFFF0); // addiu $sp, $sp, -16
    code.push_back(0xAFBF000C); // sw $ra, 12($sp)
    code.push_back(0x3C028004); // lui $v0, 0x8004
    code.push_back(0x8C420000 + i * 4); // lw $v0, N($v0)
    code.push_back(0x24420001); // addiu $v0, $v0, 1
    code.push_back(0xAC620000 + i * 4); // sw $v0, N($v1)
    code.push_back(0x8FBF000C); // lw $ra, 12($sp)
    code.push_back(0x27BD0010); // addiu $sp, $sp, 16
    code.push_back(0x03E00008); // jr $ra
    code.push_back(0x00000000); // nop
  }
  auto data = makeMipsCode(code);

  OverlayScanner scanner;
  OverlayScanOptions opts;
  opts.minCodeSize = 32;
  auto result = scanner.scanFile(data, "CODE.BIN", opts);

  EXPECT_FALSE(result.hasPsxExeHeader);
  EXPECT_GE(result.mipsScore, 0.5f);
  EXPECT_TRUE(result.isLikelyCode());
  EXPECT_GT(result.functions.size(), 0u);
}

TEST(OverlayScannerTest, ScanFileNotCode) {
  std::vector<uint8_t> data(512, 0xFF); // Just 0xFF bytes -- not MIPS

  OverlayScanner scanner;
  auto result = scanner.scanFile(data, "DATA.BIN");

  EXPECT_FALSE(result.hasPsxExeHeader);
  EXPECT_LT(result.mipsScore, 0.5f);
  EXPECT_FALSE(result.isLikelyCode());
}

// TOML export tests

TEST(OverlayScannerTest, ExportTomlSingleOverlay) {
  OverlayCandidate c;
  c.name = "BATTLE.BIN";
  c.discPath = "/DATA/BATTLE.BIN";
  c.lba = 100;
  c.fileSize = 4096;
  c.hasPsxExeHeader = true;
  c.ramBase = 0x80040000;
  c.codeOffset = 2048;
  c.codeSize = 2048;
  c.mipsScore = 0.95f;
  c.functionScore = 0.2f;
  c.functions = {0x80040000, 0x80040100};

  auto toml = OverlayScanner::exportToml({c});

  EXPECT_NE(toml.find("[[overlays]]"), std::string::npos);
  EXPECT_NE(toml.find("BATTLE"), std::string::npos);
  EXPECT_NE(toml.find("80040000"), std::string::npos);
  EXPECT_NE(toml.find("2048"), std::string::npos);
  EXPECT_NE(toml.find("functions"), std::string::npos);
}

TEST(OverlayScannerTest, ExportTomlEmpty) {
  auto toml = OverlayScanner::exportToml({});
  EXPECT_NE(toml.find("Auto-generated"), std::string::npos);
  EXPECT_EQ(toml.find("[[overlays]]"), std::string::npos);
}
