#include "ps1recomp/overlay_scanner.h"
#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include <unordered_map>

namespace ps1recomp {

// Non-code file extensions

bool OverlayScanner::isNonCodeExtension(const std::string &name) {
  // Common PS1 data-only extensions
  static const std::vector<std::string> dataExts = {
      ".str", ".xa",  ".vag", ".tim", ".vab", ".seq", ".sep",
      ".bs",  ".mdec",".iki", ".dic", ".fnt", ".raw",
      ".dat", // ambiguous -- scan anyway if scanAllFiles
      ".mov", ".avi", ".mp3", ".wav"};

  auto dot = name.rfind('.');
  if (dot == std::string::npos)
    return false;

  std::string ext = name.substr(dot);
  // Lowercase
  for (auto &c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  // Strip version suffix ";1"
  auto semi = ext.find(';');
  if (semi != std::string::npos)
    ext = ext.substr(0, semi);

  for (const auto &de : dataExts) {
    if (ext == de)
      return true;
  }
  return false;
}

// PS-X EXE Header

std::optional<PsxExeHeader>
OverlayScanner::parsePsxExeHeader(const std::vector<uint8_t> &data) {
  if (data.size() < sizeof(PsxExeHeader))
    return std::nullopt;

  // Check magic "PS-X EXE"
  static const char MAGIC[] = "PS-X EXE";
  if (std::memcmp(data.data(), MAGIC, 8) != 0)
    return std::nullopt;

  PsxExeHeader hdr;
  std::memcpy(&hdr, data.data(), sizeof(hdr));
  return hdr;
}

// MIPS Instruction Validity

float OverlayScanner::computeMipsScore(const uint8_t *data, size_t size) {
  if (size < 4)
    return 0.0f;

  size_t wordCount = size / 4;
  size_t validCount = 0;
  size_t nopCount = 0;

  for (size_t i = 0; i < wordCount; ++i) {
    uint32_t word = static_cast<uint32_t>(data[i * 4]) |
                    (static_cast<uint32_t>(data[i * 4 + 1]) << 8) |
                    (static_cast<uint32_t>(data[i * 4 + 2]) << 16) |
                    (static_cast<uint32_t>(data[i * 4 + 3]) << 24);

    if (word == 0) {
      nopCount++;
      validCount++; // NOP is valid
      continue;
    }

    // Quick validity check: decode opcode field
    uint8_t opcode = (word >> 26) & 0x3F;

    bool valid = false;
    if (opcode == 0x00) {
      // SPECIAL: check funct field
      uint8_t funct = word & 0x3F;
      // Valid SPECIAL functs: SLL(0), SRL(2), SRA(3), SLLV(4), SRLV(6),
      // SRAV(7), JR(8), JALR(9), SYSCALL(12), BREAK(13),
      // MFHI(16), MTHI(17), MFLO(18), MTLO(19),
      // MULT(24), MULTU(25), DIV(26), DIVU(27),
      // ADD(32), ADDU(33), SUB(34), SUBU(35),
      // AND(36), OR(37), XOR(38), NOR(39), SLT(42), SLTU(43)
      static const uint8_t validFuncts[] = {0,  2,  3,  4,  6,  7,  8,
                                            9,  12, 13, 16, 17, 18, 19,
                                            24, 25, 26, 27, 32, 33, 34,
                                            35, 36, 37, 38, 39, 42, 43};
      for (auto f : validFuncts) {
        if (funct == f) {
          valid = true;
          break;
        }
      }
    } else if (opcode == 0x01) {
      // REGIMM: BLTZ(0), BGEZ(1), BLTZAL(16), BGEZAL(17)
      uint8_t rt = (word >> 16) & 0x1F;
      valid = (rt == 0 || rt == 1 || rt == 16 || rt == 17);
    } else if (opcode == 0x10) {
      // COP0
      uint8_t rs = (word >> 21) & 0x1F;
      valid = (rs == 0 || rs == 4 || rs == 16); // MFC0, MTC0, RFE-like
    } else if (opcode == 0x12) {
      // COP2 (GTE) -- always valid if it's a GTE instruction
      valid = true;
    } else {
      // Standard opcodes: J(2), JAL(3), BEQ(4), BNE(5), BLEZ(6), BGTZ(7),
      // ADDI(8), ADDIU(9), SLTI(10), SLTIU(11), ANDI(12), ORI(13),
      // XORI(14), LUI(15), LB(32), LH(33), LWL(34), LW(35),
      // LBU(36), LHU(37), LWR(38), SB(40), SH(41), SWL(42),
      // SW(43), SWR(46), LWC2(50), SWC2(58)
      static const uint8_t validOpcodes[] = {2,  3,  4,  5,  6,  7,  8,  9,
                                             10, 11, 12, 13, 14, 15, 32, 33,
                                             34, 35, 36, 37, 38, 40, 41, 42,
                                             43, 46, 50, 58};
      for (auto o : validOpcodes) {
        if (opcode == o) {
          valid = true;
          break;
        }
      }
    }

    if (valid)
      validCount++;
  }

  // Penalize if >50% NOP (likely zero-filled data, not real code)
  float nopRatio =
      wordCount > 0 ? static_cast<float>(nopCount) / wordCount : 0.0f;
  float rawScore =
      wordCount > 0 ? static_cast<float>(validCount) / wordCount : 0.0f;

  if (nopRatio > 0.5f)
    rawScore *= 0.5f; // Heavy penalty for mostly-zero data

  return rawScore;
}

// Function Prologue/Epilogue Detection

std::pair<float, std::vector<uint32_t>>
OverlayScanner::detectFunctions(const uint8_t *data, size_t size,
                                uint32_t baseAddr) {
  if (size < 8)
    return {0.0f, {}};

  size_t wordCount = size / 4;
  std::vector<uint32_t> funcAddrs;
  size_t prologueCount = 0;
  size_t epilogueCount = 0;

  for (size_t i = 0; i < wordCount; ++i) {
    uint32_t word = static_cast<uint32_t>(data[i * 4]) |
                    (static_cast<uint32_t>(data[i * 4 + 1]) << 8) |
                    (static_cast<uint32_t>(data[i * 4 + 2]) << 16) |
                    (static_cast<uint32_t>(data[i * 4 + 3]) << 24);

    // Pattern: ADDIU $sp, $sp, -N (stack frame allocation)
    // ADDIU opcode = 0x09, rs=29($sp), rt=29($sp), imm16 < 0
    uint8_t opcode = (word >> 26) & 0x3F;
    uint8_t rs = (word >> 21) & 0x1F;
    uint8_t rt = (word >> 16) & 0x1F;
    int16_t imm = static_cast<int16_t>(word & 0xFFFF);

    if (opcode == 0x09 && rs == 29 && rt == 29 && imm < 0) {
      // Function prologue: addiu $sp, $sp, -N
      prologueCount++;
      size_t entryIndex = i;

      // A function may read one or two globals before allocating its stack
      // frame. If those straight-line prefix instructions immediately follow
      // a previous JR $ra + delay slot, the callable entry is before the
      // ADDIU. This is common in stripped overlays (Crash 3 WARPSCES starts
      // one routine with LUI/LW before its prologue).
      constexpr size_t maxPrefixInstructions = 2;
      for (size_t prefix = 1;
           prefix <= maxPrefixInstructions && i >= prefix + 2; ++prefix) {
        const size_t returnIndex = i - prefix - 2;
        const uint8_t *returnPtr = data + returnIndex * 4;
        const uint32_t returnWord =
            static_cast<uint32_t>(returnPtr[0]) |
            (static_cast<uint32_t>(returnPtr[1]) << 8) |
            (static_cast<uint32_t>(returnPtr[2]) << 16) |
            (static_cast<uint32_t>(returnPtr[3]) << 24);
        if (returnWord != 0x03E00008)
          continue;

        bool straightLinePrefix = true;
        for (size_t p = i - prefix; p < i; ++p) {
          const uint8_t *prefixPtr = data + p * 4;
          const uint32_t prefixWord =
              static_cast<uint32_t>(prefixPtr[0]) |
              (static_cast<uint32_t>(prefixPtr[1]) << 8) |
              (static_cast<uint32_t>(prefixPtr[2]) << 16) |
              (static_cast<uint32_t>(prefixPtr[3]) << 24);
          const uint8_t prefixOpcode = (prefixWord >> 26) & 0x3F;
          const bool specialJump = prefixOpcode == 0 &&
                                   (prefixWord & 0x3F) == 0x08;
          const bool controlFlow =
              specialJump || prefixOpcode == 1 ||
              (prefixOpcode >= 2 && prefixOpcode <= 7);
          if (controlFlow) {
            straightLinePrefix = false;
            break;
          }
        }
        if (straightLinePrefix) {
          entryIndex = i - prefix;
          break;
        }
      }

      funcAddrs.push_back(baseAddr +
                          static_cast<uint32_t>(entryIndex * 4));
    }

    // Pattern: JR $ra (function epilogue)
    // SPECIAL(0x00), rs=31($ra), funct=JR(0x08)
    if (word == 0x03E00008) {
      epilogueCount++;
    }
  }

  // Score based on prologue/epilogue density per 1KB of code
  float density =
      size > 0
          ? (static_cast<float>(prologueCount + epilogueCount) / (size / 1024.0f))
          : 0.0f;
  // Normalize: ~2 functions per KB is typical for PS1 code
  float score = std::min(density / 4.0f, 1.0f);

  return {score, funcAddrs};
}

// RAM Base Address Inference

uint32_t OverlayScanner::inferRamBase(const uint8_t *data, size_t size) {
  if (size < 8)
    return 0;

  // Count LUI instructions and tally the upper 16-bit values
  // PS1 code typically has LUI $reg, 0x800X for RAM references
  std::unordered_map<uint16_t, int> luiCounts;
  size_t wordCount = size / 4;

  for (size_t i = 0; i < wordCount; ++i) {
    uint32_t word = static_cast<uint32_t>(data[i * 4]) |
                    (static_cast<uint32_t>(data[i * 4 + 1]) << 8) |
                    (static_cast<uint32_t>(data[i * 4 + 2]) << 16) |
                    (static_cast<uint32_t>(data[i * 4 + 3]) << 24);

    uint8_t opcode = (word >> 26) & 0x3F;
    if (opcode == 0x0F) { // LUI
      uint16_t imm = word & 0xFFFF;
      // Only count values in PS1 RAM range (0x8000-0x801F)
      if (imm >= 0x8000 && imm <= 0x801F) {
        luiCounts[imm]++;
      }
    }
  }

  if (luiCounts.empty())
    return 0;

  // Find the most common LUI upper value
  uint16_t bestUpper = 0;
  int bestCount = 0;
  for (const auto &[upper, count] : luiCounts) {
    if (count > bestCount) {
      bestCount = count;
      bestUpper = upper;
    }
  }

  // Also look for ORI/ADDIU that pair with this LUI to refine the base
  // The base is likely the lowest full address referenced with this upper half
  uint32_t minAddr = (static_cast<uint32_t>(bestUpper) << 16) | 0xFFFF;

  for (size_t i = 0; i + 1 < wordCount; ++i) {
    uint32_t w0 = static_cast<uint32_t>(data[i * 4]) |
                  (static_cast<uint32_t>(data[i * 4 + 1]) << 8) |
                  (static_cast<uint32_t>(data[i * 4 + 2]) << 16) |
                  (static_cast<uint32_t>(data[i * 4 + 3]) << 24);
    uint32_t w1 = static_cast<uint32_t>(data[(i + 1) * 4]) |
                  (static_cast<uint32_t>(data[(i + 1) * 4 + 1]) << 8) |
                  (static_cast<uint32_t>(data[(i + 1) * 4 + 2]) << 16) |
                  (static_cast<uint32_t>(data[(i + 1) * 4 + 3]) << 24);

    uint8_t op0 = (w0 >> 26) & 0x3F;
    uint16_t imm0 = w0 & 0xFFFF;

    if (op0 == 0x0F && imm0 == bestUpper) { // LUI with our best upper
      uint8_t luiRt = (w0 >> 16) & 0x1F;

      uint8_t op1 = (w1 >> 26) & 0x3F;
      uint8_t rs1 = (w1 >> 21) & 0x1F;

      if ((op1 == 0x0D || op1 == 0x09) &&
          rs1 == luiRt) { // ORI or ADDIU with same reg
        uint32_t fullAddr;
        if (op1 == 0x0D) { // ORI
          fullAddr =
              (static_cast<uint32_t>(bestUpper) << 16) | (w1 & 0xFFFF);
        } else { // ADDIU (sign-extended)
          int16_t addImm = static_cast<int16_t>(w1 & 0xFFFF);
          fullAddr = (static_cast<uint32_t>(bestUpper) << 16) +
                     static_cast<uint32_t>(addImm);
        }

        if (fullAddr < minAddr && fullAddr >= 0x80000000 &&
            fullAddr < 0x80200000) {
          minAddr = fullAddr;
        }
      }
    }
  }

  // Align to 2KB boundary (typical overlay alignment)
  if (minAddr < (static_cast<uint32_t>(bestUpper) << 16) | 0xFFFF) {
    return minAddr & 0xFFFFF800;
  }

  // Fallback: just use the upper half with 0 lower
  return static_cast<uint32_t>(bestUpper) << 16;
}

// Scan Single File

OverlayCandidate
OverlayScanner::scanFile(const std::vector<uint8_t> &data,
                         const std::string &filename,
                         const OverlayScanOptions &options) const {
  OverlayCandidate result;
  result.name = filename;
  result.discPath = filename;
  result.lba = 0;
  result.fileSize = static_cast<uint32_t>(data.size());
  result.hasPsxExeHeader = false;
  result.ramBase = 0;
  result.codeOffset = 0;
  result.codeSize = 0;
  result.mipsScore = 0.0f;
  result.functionScore = 0.0f;

  if (data.size() < options.minCodeSize)
    return result;

  // Try PS-X EXE header first
  auto hdr = parsePsxExeHeader(data);
  if (hdr) {
    result.hasPsxExeHeader = true;
    result.ramBase = hdr->tAddr;
    result.codeOffset = 2048; // PS-X EXE header is 2048 bytes
    result.codeSize = hdr->tSize;

    // Scan the code section
    if (result.codeOffset + result.codeSize <= data.size()) {
      const uint8_t *codePtr = data.data() + result.codeOffset;
      result.mipsScore = computeMipsScore(codePtr, result.codeSize);
      auto [fScore, fAddrs] =
          detectFunctions(codePtr, result.codeSize, result.ramBase);
      result.functionScore = fScore;
      result.functions = std::move(fAddrs);
    }
    return result;
  }

  // No PS-X EXE header -- scan raw data for MIPS code
  // Try the full file first
  result.mipsScore = computeMipsScore(data.data(), data.size());

  if (result.mipsScore >= options.minMipsScore) {
    result.codeOffset = 0;
    result.codeSize = static_cast<uint32_t>(data.size());

    // Infer RAM base from LUI patterns
    result.ramBase = inferRamBase(data.data(), data.size());

    auto [fScore, fAddrs] =
        detectFunctions(data.data(), data.size(), result.ramBase);
    result.functionScore = fScore;
    result.functions = std::move(fAddrs);
  }

  return result;
}

// Scan Entire Disc

std::vector<OverlayCandidate>
OverlayScanner::scanDisc(const DiscReader &reader,
                         const OverlayScanOptions &options) const {
  std::vector<OverlayCandidate> results;
  const auto &files = reader.getFiles();

  fmt::print("[OverlayScanner] Scanning {} files on disc...\n", files.size());

  for (const auto &file : files) {
    // Skip directories
    if (file.isDirectory)
      continue;

    // Skip boot EXE (it's already the main binary)
    if (!options.bootExeName.empty()) {
      std::string cleanName = file.name;
      auto semi = cleanName.find(';');
      if (semi != std::string::npos)
        cleanName = cleanName.substr(0, semi);

      std::string cleanBoot = options.bootExeName;
      auto semi2 = cleanBoot.find(';');
      if (semi2 != std::string::npos)
        cleanBoot = cleanBoot.substr(0, semi2);

      // Case-insensitive comparison
      auto toLower = [](std::string s) {
        for (auto &c : s)
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
      };
      if (toLower(cleanName) == toLower(cleanBoot))
        continue;
    }

    // Skip known non-code extensions (unless scanAllFiles)
    if (!options.scanAllFiles && isNonCodeExtension(file.name))
      continue;

    // Skip very large files
    if (file.size > options.maxFileSize)
      continue;

    // Skip tiny files
    if (file.size < options.minCodeSize)
      continue;

    // Read file data
    auto data = reader.readFile(file);
    if (data.empty())
      continue;

    // Scan
    auto candidate = scanFile(data, file.name, options);
    candidate.discPath = file.path;
    candidate.lba = file.lba;

    if (candidate.isLikelyCode()) {
      fmt::print(
          "[OverlayScanner] Found overlay candidate: {} "
          "(MIPS={:.1f}%, funcs={}, ram=0x{:08X}, size={})\n",
          file.name, candidate.mipsScore * 100.0f, candidate.functions.size(),
          candidate.ramBase, candidate.codeSize);
      results.push_back(std::move(candidate));
    }
  }

  fmt::print("[OverlayScanner] Found {} overlay candidates.\n", results.size());
  return results;
}

// Export to TOML

std::string
OverlayScanner::exportToml(const std::vector<OverlayCandidate> &candidates) {
  std::string toml;
  toml += "# Auto-generated overlay definitions\n";
  toml += "# Review and adjust ram_base values before recompilation\n\n";

  for (const auto &c : candidates) {
    if (!c.isLikelyCode())
      continue;

    // Clean filename for overlay name
    std::string ovlName = c.name;
    auto dot = ovlName.rfind('.');
    if (dot != std::string::npos)
      ovlName = ovlName.substr(0, dot);
    auto semi = ovlName.find(';');
    if (semi != std::string::npos)
      ovlName = ovlName.substr(0, semi);
    // Replace non-alnum with underscore
    for (auto &ch : ovlName) {
      if (!std::isalnum(static_cast<unsigned char>(ch)))
        ch = '_';
    }

    toml += "[[overlays]]\n";
    toml += fmt::format("name = \"{}\"\n", ovlName);
    toml += fmt::format("disc_path = \"{}\"\n", c.discPath);
    toml += fmt::format("rom_offset = {} # LBA: {}\n", c.lba * 2048, c.lba);
    toml += fmt::format("ram_base = {} # 0x{:08X}\n", c.ramBase, c.ramBase);
    toml += fmt::format("size = {}\n", c.codeSize);

    if (!c.functions.empty()) {
      toml += "functions = [\n";
      for (size_t i = 0; i < c.functions.size(); ++i) {
        toml += fmt::format("  {}", c.functions[i]);
        if (i + 1 < c.functions.size())
          toml += ",";
        toml += fmt::format(" # 0x{:08X}\n", c.functions[i]);
      }
      toml += "]\n";
    }

    toml += fmt::format("# PS-X EXE: {}, MIPS score: {:.1f}%, "
                         "function score: {:.1f}%\n",
                         c.hasPsxExeHeader ? "yes" : "no",
                         c.mipsScore * 100.0f, c.functionScore * 100.0f);
    toml += "\n";
  }

  return toml;
}

} // namespace ps1recomp
