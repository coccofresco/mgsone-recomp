// ps1Recomp -- PS1 Static Recompiler
// Translates MIPS I instructions to C++ code (1:1 literal translation)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <ps1recomp/dispatch_emitter.h>
#include <ps1recomp/elf_parser.h>
#include <ps1recomp/hle_emitter.h>
#include <ps1recomp/instruction_emitter.h>
#include <ps1recomp/mips_decoder.h>
#include <ps1recomp/overlay_handler.h>
#include <ps1recomp/overlay_scanner.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <toml.hpp>
#include <vector>

using namespace ps1recomp;

namespace {

constexpr std::string_view kTranslationDefinitionsBegin =
    "// PS1RECOMP TRANSLATION DEFINITIONS BEGIN\n";
constexpr std::string_view kTranslationDefinitionsEnd =
    "// PS1RECOMP TRANSLATION DEFINITIONS END\n";

void writeGeneratedFile(const std::filesystem::path &path,
                        std::string_view contents) {
  std::ifstream existing(path, std::ios::binary);
  if (existing) {
    existing.seekg(0, std::ios::end);
    const auto size = existing.tellg();
    if (size >= 0 && static_cast<uint64_t>(size) == contents.size()) {
      existing.seekg(0, std::ios::beg);
      std::string previous(contents.size(), '\0');
      if (existing.read(previous.data(), static_cast<std::streamsize>(size)) &&
          previous == contents) {
        return;
      }
    }
  }

  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output ||
        !output.write(contents.data(),
                      static_cast<std::streamsize>(contents.size()))) {
      throw std::runtime_error("failed to write generated source " +
                               temporary.string());
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("failed to replace generated source " +
                             path.string() + ": " + error.message());
  }
}

bool isGeneratedShard(const std::filesystem::path &path,
                      std::string_view stem) {
  const std::string name = path.filename().string();
  const std::string prefix = std::string(stem) + ".part";
  constexpr std::string_view suffix = ".cpp";
  if (!name.starts_with(prefix) || !name.ends_with(suffix) ||
      name.size() <= prefix.size() + suffix.size()) {
    return false;
  }
  const std::string_view index(name.data() + prefix.size(),
                               name.size() - prefix.size() - suffix.size());
  return std::all_of(index.begin(), index.end(),
                     [](char value) { return value >= '0' && value <= '9'; });
}

void removeGeneratedShards(const std::filesystem::path &outputPath) {
  const std::filesystem::path directory = outputPath.has_parent_path()
                                              ? outputPath.parent_path()
                                              : std::filesystem::path(".");
  const std::string stem = outputPath.stem().string();
  std::error_code error;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file() ||
        !isGeneratedShard(entry.path(), stem)) {
      continue;
    }
    std::filesystem::remove(entry.path(), error);
    if (error) {
      throw std::runtime_error("failed to remove stale generated shard " +
                               entry.path().string() + ": " +
                               error.message());
    }
  }
}

uint64_t stableShardHash(std::string_view name) {
  uint64_t value = UINT64_C(14695981039346656037);
  for (const unsigned char byte : name) {
    value ^= byte;
    value *= UINT64_C(1099511628211);
  }
  return value;
}

struct GeneratedDefinition {
  std::string name;
  std::string_view source;
};

std::vector<GeneratedDefinition>
findGeneratedDefinitions(std::string_view body) {
  struct DefinitionStart {
    std::size_t offset;
    std::string name;
  };
  std::vector<DefinitionStart> starts;
  constexpr std::string_view ordinaryPrefix = "void ";
  constexpr std::string_view optimizedPrefix =
      "PS1_RECOMP_HOT_OPTIMIZE void ";
  constexpr std::string_view signature =
      "(uint8_t* rdram, recomp_context* ctx) {";

  for (std::size_t lineStart = 0; lineStart < body.size();) {
    const std::size_t lineEnd = body.find('\n', lineStart);
    const std::size_t end =
        lineEnd == std::string_view::npos ? body.size() : lineEnd;
    const std::string_view line = body.substr(lineStart, end - lineStart);
    std::string_view candidate;
    if (line.starts_with(ordinaryPrefix))
      candidate = line.substr(ordinaryPrefix.size());
    else if (line.starts_with(optimizedPrefix))
      candidate = line.substr(optimizedPrefix.size());

    if (!candidate.empty()) {
      const std::size_t signatureOffset = candidate.find(signature);
      if (signatureOffset != std::string_view::npos) {
        const std::string_view name = candidate.substr(0, signatureOffset);
        const bool validName = !name.empty() &&
            std::all_of(name.begin(), name.end(), [](char value) {
              return (value >= 'a' && value <= 'z') ||
                     (value >= 'A' && value <= 'Z') ||
                     (value >= '0' && value <= '9') || value == '_';
            });
        if (validName)
          starts.push_back({lineStart, std::string(name)});
      }
    }
    if (lineEnd == std::string_view::npos)
      break;
    lineStart = lineEnd + 1;
  }

  if (starts.empty())
    throw std::runtime_error("generated translation contains no definitions");

  std::set<std::string> uniqueNames;
  std::vector<GeneratedDefinition> definitions;
  definitions.reserve(starts.size());
  for (std::size_t index = 0; index < starts.size(); ++index) {
    if (!uniqueNames.insert(starts[index].name).second) {
      throw std::runtime_error("duplicate generated function " +
                               starts[index].name);
    }
    const std::size_t end =
        index + 1 < starts.size() ? starts[index + 1].offset : body.size();
    definitions.push_back(
        {starts[index].name, body.substr(starts[index].offset,
                                        end - starts[index].offset)});
  }
  return definitions;
}

void writeShardedTranslation(const std::filesystem::path &outputPath,
                             const std::string &combined,
                             std::size_t shardCount) {
  const std::size_t begin = combined.find(kTranslationDefinitionsBegin);
  const std::size_t end = combined.find(kTranslationDefinitionsEnd);
  if (begin == std::string::npos || end == std::string::npos || end <= begin) {
    throw std::runtime_error("generated translation shard markers are missing");
  }
  const std::size_t bodyStart = begin + kTranslationDefinitionsBegin.size();
  const std::string_view body(combined.data() + bodyStart, end - bodyStart);
  const auto definitions = findGeneratedDefinitions(body);

  const std::string stem = outputPath.stem().string();
  const std::filesystem::path directory = outputPath.has_parent_path()
                                              ? outputPath.parent_path()
                                              : std::filesystem::path(".");
  const std::filesystem::path headerPath =
      directory / (stem + ".generated.h");

  std::string header;
  header.reserve(begin + definitions.size() * 80u);
  header += "#pragma once\n";
  header.append(combined.data(), begin);
  header += "\n// Declarations shared by independently compiled translation "
            "shards.\n";
  for (const GeneratedDefinition &definition : definitions) {
    header += fmt::format(
        "void {}(uint8_t* rdram, recomp_context* ctx);\n", definition.name);
  }
  writeGeneratedFile(headerPath, header);

  std::vector<std::string> shards(shardCount);
  std::vector<std::size_t> shardBytes(shardCount, 0u);
  const std::string include =
      fmt::format("// Generated by ps1Recomp (translation shard)\n"
                  "#include \"{}\"\n\n",
                  headerPath.filename().string());
  for (std::string &shard : shards)
    shard = include;

  // Largest-first packing prevents one compiler process from inheriting
  // several of the duplicated hand-written renderer islands. Name hashing is
  // the deterministic tie breaker, so an unchanged translation rewrites no
  // shard and therefore keeps incremental renderer builds free of game-code
  // work.
  std::vector<std::size_t> definitionOrder(definitions.size());
  std::iota(definitionOrder.begin(), definitionOrder.end(), 0u);
  std::sort(definitionOrder.begin(), definitionOrder.end(),
            [&definitions](std::size_t left, std::size_t right) {
              const auto &leftDefinition = definitions[left];
              const auto &rightDefinition = definitions[right];
              if (leftDefinition.source.size() !=
                  rightDefinition.source.size()) {
                return leftDefinition.source.size() >
                       rightDefinition.source.size();
              }
              return stableShardHash(leftDefinition.name) <
                     stableShardHash(rightDefinition.name);
            });
  for (const std::size_t definitionIndex : definitionOrder) {
    const GeneratedDefinition &definition = definitions[definitionIndex];
    const std::size_t shard = static_cast<std::size_t>(std::distance(
        shardBytes.begin(),
        std::min_element(shardBytes.begin(), shardBytes.end())));
    shards[shard].append(definition.source.data(), definition.source.size());
    shardBytes[shard] += definition.source.size();
  }

  std::set<std::filesystem::path> expectedShards;
  for (std::size_t index = 0; index < shardCount; ++index) {
    const std::filesystem::path shardPath =
        directory / fmt::format("{}.part{:03}.cpp", stem, index);
    expectedShards.insert(shardPath.lexically_normal());
    writeGeneratedFile(shardPath, shards[index]);
  }

  std::error_code error;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file() ||
        !isGeneratedShard(entry.path(), stem) ||
        expectedShards.count(entry.path().lexically_normal()) != 0) {
      continue;
    }
    std::filesystem::remove(entry.path(), error);
    if (error) {
      throw std::runtime_error("failed to remove stale generated shard " +
                               entry.path().string() + ": " +
                               error.message());
    }
  }

  std::string support = fmt::format(
      "// Generated by ps1Recomp (dispatch/support)\n#include \"{}\"\n\n",
      headerPath.filename().string());
  const std::size_t suffixStart = end + kTranslationDefinitionsEnd.size();
  support.append(combined.data() + suffixStart, combined.size() - suffixStart);
  writeGeneratedFile(outputPath, support);

  const auto [smallest, largest] =
      std::minmax_element(shardBytes.begin(), shardBytes.end());
  fmt::print("  Translation shards: {} files, {} definitions, {}..{} bytes\n",
             shardCount, definitions.size(), *smallest, *largest);
}

} // namespace

// Jump Table Auto-Detection
//
// Detects the canonical MIPS switch table pattern:
//   SLTIU $rchk, $ridx, N     ; bounds check against the entry count
//   BEQZ  $rchk, default      ; out-of-range -> default case
//   SLL   $rscale, $ridx, 2   ; scale index to a word offset (delay slot)
//   LUI   $rbase, hi          ; rbase = upper address
//   ADDIU $rbase, $rbase, lo  ; rbase = table_base
//   ADDU  $rtbl, $rbase, $rscale ; rtbl = table_base + index*4
//   LW    $rtgt, N($rtbl)     ; rtgt = table[index]
//   JR    $rtgt               ; indirect jump
//
// When found, reads the target addresses from the ELF data section
// and stores them in a JumpTableEntry on the RecompFunction.
//
// The entry count comes from the SLTIU/SLTI bounds check when one is present.
// Without it there is nothing in the code stream that marks where the table
// ends, so we fall back to a conservative fixed window: a table is only
// distinguishable from the data that follows it by that bound.
//
// Returns a vector of target addresses (empty if pattern not found).
static constexpr uint32_t kJumpTableFallbackEntries = 64;
static constexpr uint32_t kJumpTableMaxEntries = 4096;

static std::vector<uint32_t>
detectJumpTable(const std::vector<uint32_t> &instrs, size_t jr_idx,
                const ElfParser &parser, const uint8_t *rawRegion = nullptr,
                uint32_t rawRegionBase = 0, uint32_t rawRegionSize = 0) {
  if (jr_idx == 0)
    return {};

  Instruction jr_inst = MipsDecoder::decode(instrs[jr_idx]);
  uint8_t target_reg = jr_inst.rs; // register that holds the jump target

  // Step 1: Find LW $target_reg, N($base_reg) within 6 instructions back
  int lw_idx = -1;
  uint8_t lw_base_reg = 0;
  int16_t lw_imm = 0;
  for (int j = (int)jr_idx - 1; j >= 0 && j >= (int)jr_idx - 6; j--) {
    Instruction inst = MipsDecoder::decode(instrs[j]);
    if (inst.id == InstrId::LW && inst.rt == target_reg) {
      lw_idx = j;
      lw_base_reg = inst.rs;
      lw_imm = inst.imm16;
      break;
    }
  }
  if (lw_idx < 0)
    return {};

  // Step 2: Find ADDU $lw_base_reg, $rconst, $rscale within 8 instructions back.
  // One of rs/rt is the constant table pointer, the other the scaled index.
  int addu_idx = -1;
  uint8_t addu_rs = 0, addu_rt = 0;
  for (int j = lw_idx - 1; j >= 0 && j >= (int)jr_idx - 12; j--) {
    Instruction inst = MipsDecoder::decode(instrs[j]);
    if (inst.id == InstrId::ADDU && inst.rd == lw_base_reg) {
      addu_idx = j;
      addu_rs = inst.rs;
      addu_rt = inst.rt;
      break;
    }
  }
  if (addu_idx < 0)
    return {};

  // Step 3: Track a candidate register back to LUI (+ optional ADDIU).
  // Whichever ADDU operand resolves to a LUI is the table base; the other
  // one is the scaled index.
  uint32_t lui_val = 0;
  int16_t addiu_imm = 0;
  auto traceToLui = [&](uint8_t reg) {
    lui_val = 0;
    addiu_imm = 0;
    for (int j = lw_idx - 1; j >= 0 && j >= (int)jr_idx - 20; j--) {
      Instruction inst = MipsDecoder::decode(instrs[j]);
      if (inst.id == InstrId::LUI && inst.rt == reg) {
        lui_val = static_cast<uint32_t>(static_cast<uint16_t>(inst.imm16)) << 16;
        return true;
      }
      if ((inst.id == InstrId::ADDIU || inst.id == InstrId::ADDI) &&
          inst.rt == reg && inst.rs == reg) {
        addiu_imm = inst.imm16;
      }
    }
    return false;
  };

  uint8_t scaled_reg = addu_rt;
  if (!traceToLui(addu_rs)) {
    scaled_reg = addu_rs;
    if (!traceToLui(addu_rt))
      return {};
  }

  uint32_t table_addr = lui_val + static_cast<uint32_t>(addiu_imm) +
                        static_cast<uint32_t>(lw_imm);

  // Step 4: Recover the entry count from the bounds check that guards the
  // table. SLL $scaled_reg, $ridx, 2 gives the raw index register, and the
  // SLTIU/SLTI against that register carries the number of entries.
  uint32_t max_entries = kJumpTableFallbackEntries;
  int sll_idx = -1;
  uint8_t index_reg = 0;
  for (int j = addu_idx - 1; j >= 0 && j >= (int)jr_idx - 24; j--) {
    Instruction inst = MipsDecoder::decode(instrs[j]);
    if (inst.id == InstrId::SLL && inst.rd == scaled_reg && inst.shamt == 2) {
      sll_idx = j;
      index_reg = inst.rt;
      break;
    }
  }
  if (sll_idx >= 0) {
    for (int j = sll_idx - 1; j >= 0 && j >= (int)jr_idx - 28; j--) {
      Instruction inst = MipsDecoder::decode(instrs[j]);
      if ((inst.id == InstrId::SLTIU || inst.id == InstrId::SLTI) &&
          inst.rs == index_reg) {
        uint32_t bound = static_cast<uint32_t>(static_cast<uint16_t>(inst.imm16));
        if (bound > 0 && bound <= kJumpTableMaxEntries)
          max_entries = bound;
        break;
      }
    }
  }

  // Step 5: Read table entries from the owning image.  Main-executable
  // functions resolve through ElfParser; overlay functions pass their own
  // raw load region because the overlay is not an ELF section.  Without the
  // latter, valid overlay switch handlers are emitted as data after JR and
  // the state machine silently returns through an unmapped indirect jump.
  const uint8_t *tableData = nullptr;
  uint32_t tableSize = 0;
  uint32_t offset = 0;
  if (rawRegion != nullptr && table_addr >= rawRegionBase &&
      table_addr - rawRegionBase < rawRegionSize) {
    tableData = rawRegion;
    tableSize = rawRegionSize;
    offset = table_addr - rawRegionBase;
  } else {
    const Section *section = parser.findSectionByAddress(table_addr);
    if (!section || section->data == nullptr)
      return {};
    tableData = section->data;
    tableSize = section->size;
    offset = table_addr - section->vaddr;
  }

  std::vector<uint32_t> targets;
  for (uint32_t i = 0; i < max_entries; i++) {
    uint32_t entry_offset = offset + i * 4;
    if (entry_offset + 4 > tableSize)
      break;
    const uint8_t *ptr = tableData + entry_offset;
    uint32_t entry = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
    // Valid PS1 KSEG0 address range: stop at anything outside it
    if (entry < 0x80000000u || entry > 0x80400000u)
      break;
    targets.push_back(entry);
  }
  return targets;
}

// Detect a table which is copied out of the executable before use (commonly
// into the PS1 scratchpad).  At the dispatch site there is no LUI/ADDIU for
// the original table address, so the canonical detector above cannot recover
// it.  We first require an indexed LW+JR shape, then look for a sizeable run
// of nearby executable pointers in the loaded image.  Requiring every entry
// to stay close to the dispatch routine avoids confusing ordinary callback
// arrays with local opcode-handler tables.
static std::vector<uint32_t> detectRelocatedJumpTable(
    const std::vector<uint32_t> &instrs, size_t jr_idx,
    const ElfParser &parser, uint32_t function_addr, uint32_t function_end) {
  if (jr_idx == 0 || jr_idx >= instrs.size())
    return {};

  const Instruction jr = MipsDecoder::decode(instrs[jr_idx]);
  if (jr.id != InstrId::JR || jr.rs == 31)
    return {};

  int lw_idx = -1;
  uint8_t address_reg = 0;
  for (int j = static_cast<int>(jr_idx) - 1;
       j >= 0 && j >= static_cast<int>(jr_idx) - 6; --j) {
    const Instruction inst = MipsDecoder::decode(instrs[j]);
    if (inst.id == InstrId::LW && inst.rt == jr.rs) {
      lw_idx = j;
      address_reg = inst.rs;
      break;
    }
  }
  if (lw_idx < 0)
    return {};

  int add_idx = -1;
  uint8_t add_rs = 0;
  uint8_t add_rt = 0;
  for (int j = lw_idx - 1; j >= 0 && j >= lw_idx - 8; --j) {
    const Instruction inst = MipsDecoder::decode(instrs[j]);
    if (inst.id == InstrId::ADDU && inst.rd == address_reg) {
      add_idx = j;
      add_rs = inst.rs;
      add_rt = inst.rt;
      break;
    }
  }
  if (add_idx < 0)
    return {};

  // One ADDU input must be a word-aligned, bounded index.  Crash 3 uses
  // `srl index, opcode, 22; andi index, 0x3fc`; compilers more often use
  // `sll index, selector, 2`.
  bool indexed = false;
  uint32_t expectedEntries = 0;
  for (int j = add_idx - 1; j >= 0 && j >= add_idx - 16; --j) {
    const Instruction inst = MipsDecoder::decode(instrs[j]);
    const bool addInput = inst.rt == add_rs || inst.rt == add_rt;
    if (inst.id == InstrId::ANDI && addInput) {
      const uint32_t mask = static_cast<uint16_t>(inst.imm16);
      if (mask >= 0x1C && (mask & 3u) == 0) {
        indexed = true;
        break;
      }
    }
    if (inst.id == InstrId::SLL && inst.shamt == 2 &&
        (inst.rd == add_rs || inst.rd == add_rt)) {
      indexed = true;
      for (int k = j - 1; k >= 0 && k >= j - 32; --k) {
        const Instruction bound = MipsDecoder::decode(instrs[k]);
        if ((bound.id == InstrId::SLTIU || bound.id == InstrId::SLTI) &&
            bound.rs == inst.rt) {
          const uint32_t count = static_cast<uint16_t>(bound.imm16);
          if (count > 0 && count <= kJumpTableMaxEntries)
            expectedEntries = count;
          break;
        }
      }
      break;
    }
  }
  if (!indexed)
    return {};

  constexpr size_t minEntries = 8;
  constexpr uint32_t nearbyWindow = 0x10000;
  const uint64_t nearbyLow = function_addr >= nearbyWindow
                                 ? function_addr - nearbyWindow
                                 : 0;
  const uint64_t nearbyHigh =
      static_cast<uint64_t>(function_addr) + nearbyWindow;

  std::vector<uint32_t> best;
  size_t bestLocalTargets = 0;
  for (const Section &section : parser.getSections()) {
    if (section.data == nullptr || section.size < minEntries * 4)
      continue;

    std::vector<uint32_t> run;
    auto considerRun = [&]() {
      if (run.size() < minEntries) {
        run.clear();
        return;
      }

      const size_t candidateSize =
          expectedEntries == 0 ? run.size() : expectedEntries;
      if (run.size() < candidateSize) {
        run.clear();
        return;
      }
      const size_t lastStart =
          expectedEntries == 0 ? 0 : run.size() - candidateSize;
      for (size_t start = 0; start <= lastStart; ++start) {
        const auto first = run.begin() + static_cast<std::ptrdiff_t>(start);
        const auto last = first + static_cast<std::ptrdiff_t>(candidateSize);
        std::set<uint32_t> unique(first, last);
        const size_t local = static_cast<size_t>(std::count_if(
            first, last, [&](uint32_t target) {
              return target >= function_addr && target < function_end;
            }));
        const bool allNearby = std::all_of(
            first, last, [&](uint32_t target) {
              return target >= nearbyLow && target < nearbyHigh;
            });
        const size_t minUnique = expectedEntries == 0 ? 4 : 2;
        const bool better = local > bestLocalTargets ||
                            (local == bestLocalTargets &&
                             candidateSize > best.size());
        if (allNearby && local >= 4 && unique.size() >= minUnique && better) {
          best.assign(first, last);
          bestLocalTargets = local;
        }
      }
      run.clear();
    };

    for (uint32_t offset = 0; offset + 4 <= section.size; offset += 4) {
      const uint8_t *ptr = section.data + offset;
      const uint32_t value = static_cast<uint32_t>(ptr[0]) |
                             (static_cast<uint32_t>(ptr[1]) << 8) |
                             (static_cast<uint32_t>(ptr[2]) << 16) |
                             (static_cast<uint32_t>(ptr[3]) << 24);
      const Section *targetSection = parser.findSectionByAddress(value);
      if ((value & 3u) == 0 && targetSection != nullptr &&
          targetSection->type == SectionType::Text) {
        run.push_back(value);
      } else {
        considerRun();
      }
    }
    considerRun();
  }
  return best;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fmt::print("Usage: ps1Recomp <config.toml> [output_file.cpp]\n");
    return 1;
  }

  std::string config_path = argv[1];
  std::string output_path = (argc >= 3) ? argv[2] : "recompiled_out.cpp";

  try {
    const auto config = toml::parse(config_path);

    // binary path is relative to the config file ideally, but for now we assume
    // it builds from project root
    std::string elf_path = toml::find<std::string>(config, "binary", "path");
    uint32_t entry_point = std::stoul(
        toml::find<std::string>(config, "binary", "entry_point"), nullptr, 16);

    fmt::print("Loading binary: {}\n", elf_path);
    ps1recomp::ElfParser parser;
    if (!parser.load(elf_path)) {
      fmt::print(stderr, "Failed to load ELF: {}\n", parser.getError());
      return 1;
    }

    // HLE overrides: replace a function body with a named HLE stub
    // Config section: [hle_overrides]
    //   "0x801AA484" = "hle_DrawSync"   (calls ps1::psyq::hle_DrawSync)
    //   "0x801B954C" = "hle_VSync"      (calls ps1::psyq::hle_VSync)
    std::map<uint32_t, std::string> hleOverrides;
    if (config.contains("hle_overrides")) {
      const auto &tbl = toml::find(config, "hle_overrides").as_table();
      for (const auto &[key, val] : tbl) {
        uint32_t addr = std::stoul(key, nullptr, 16);
        hleOverrides[addr] = val.as_string();
      }
      fmt::print("HLE overrides: {} address(es)\n", hleOverrides.size());
    }

    // Missing functions: emit a literal C++ body for dead-code stubs
    // Config section: [missing_functions]
    //   "0x8019F848" = "recomp_dispatch(rdram, ctx, 0x8019F85C);"
    std::map<uint32_t, std::string> missingFunctions;
    if (config.contains("missing_functions")) {
      const auto &tbl = toml::find(config, "missing_functions").as_table();
      for (const auto &[key, val] : tbl) {
        uint32_t addr = std::stoul(key, nullptr, 16);
        missingFunctions[addr] = val.as_string();
      }
      fmt::print("Missing functions: {} address(es)\n", missingFunctions.size());
    }

    // Exact instruction replacements are an escape hatch for isolated,
    // game-owned presentation fixes. The original instruction remains the
    // source of CFG and delay-slot structure; only the emitted operation is
    // replaced. This is intentionally separate from generic MIPS semantics.
    // Config section: [instruction_overrides]
    //   "0x8003E168" = "ctx->r4 = game_specific_hook(ctx->r4); ..."
    std::map<uint32_t, std::string> instructionOverrides;
    if (config.contains("instruction_overrides")) {
      const auto &tbl =
          toml::find(config, "instruction_overrides").as_table();
      for (const auto &[key, val] : tbl) {
        const uint32_t addr = std::stoul(key, nullptr, 16);
        if ((addr & 3u) != 0)
          throw std::runtime_error(fmt::format(
              "instruction override 0x{:08X} is not word-aligned", addr));
        instructionOverrides[addr] = val.as_string();
      }
      fmt::print("Instruction overrides: {} address(es)\n",
                 instructionOverrides.size());
    }

    // Diagnostic builds deliberately compile an enormous generated TU at
    // -O0. A profile may still name a small, measured hot set for independent
    // GCC optimization. The mechanism is generic; addresses remain game data.
    std::set<uint32_t> optimizedFunctions;
    std::size_t translationUnits = 1;
    if (config.contains("recompiler")) {
      const auto &recompiler = toml::find(config, "recompiler").as_table();
      const auto found = recompiler.find("optimized_functions");
      if (found != recompiler.end()) {
        for (const std::string &value :
             toml::get<std::vector<std::string>>(found->second)) {
          const uint32_t addr = std::stoul(value, nullptr, 16);
          if ((addr & 3u) != 0)
            throw std::runtime_error(fmt::format(
                "optimized function 0x{:08X} is not word-aligned", addr));
          optimizedFunctions.insert(addr);
        }
      }
      const auto translationUnitsFound =
          recompiler.find("translation_units");
      if (translationUnitsFound != recompiler.end()) {
        const int64_t requested =
            toml::get<int64_t>(translationUnitsFound->second);
        if (requested < 1 || requested > 256) {
          throw std::runtime_error(
              "recompiler.translation_units must be in 1..256");
        }
        translationUnits = static_cast<std::size_t>(requested);
      }
      fmt::print("Optimized generated functions: {} address(es)\n",
                 optimizedFunctions.size());
      fmt::print("Generated translation units: {}\n", translationUnits);
    }

    struct InterpreterRegion {
      uint32_t start;
      uint32_t end;
      std::string name;
    };
    std::vector<InterpreterRegion> interpreterRegions;
    if (config.contains("interpreter_regions")) {
      const auto &regions = toml::find<std::vector<toml::value>>(
          config, "interpreter_regions");
      interpreterRegions.reserve(regions.size());
      for (const auto &region : regions) {
        const uint32_t start = static_cast<uint32_t>(std::stoul(
            toml::find<std::string>(region, "start"), nullptr, 0));
        const uint32_t end = static_cast<uint32_t>(std::stoul(
            toml::find<std::string>(region, "end"), nullptr, 0));
        if (start >= end)
          throw std::runtime_error("interpreter region start must precede end");
        const std::string name = region.contains("name")
                                     ? toml::find<std::string>(region, "name")
                                     : fmt::format("0x{:08X}", start);
        interpreterRegions.push_back({start, end, name});
        fmt::print("Interpreter region [{}]: 0x{:08X}..0x{:08X}\n", name,
                   start, end);
      }
    }

    // HLE functions: hash-detected PsyQ stubs from ps1Analyzer Sessao 0.4
    // Config section: [[hle_functions]]
    //   address = "0x801ABCDE"
    //   hle     = true
    //   name    = "libgpu_PutDispEnv"  (the <library>_<basename> identifier)
    //
    // The recompiler skips MIPS translation for these and emits a stub that
    // calls `psyq_dispatch("<name>", ctx)` -- resolved at runtime by the PsyQ
    // registry in ps1Runtime/src/psyq/psyq_registry.cpp.
    std::map<uint32_t, std::string> hleFunctions;
    if (config.contains("hle_functions")) {
      const auto &arr =
          toml::find<std::vector<toml::value>>(config, "hle_functions");
      for (const auto &h : arr) {
        bool hle = h.contains("hle") ? toml::find<bool>(h, "hle") : false;
        if (!hle)
          continue;
        uint32_t addr =
            std::stoul(toml::find<std::string>(h, "address"), nullptr, 16);
        std::string name = toml::find<std::string>(h, "name");
        hleFunctions[addr] = name;
      }
      fmt::print("HLE functions ([[hle_functions]]): {}\n", hleFunctions.size());
    }

    std::string result_cpp = "// Generated by ps1Recomp\n";
    result_cpp += "#include <cstdint>\n";
    result_cpp += "#include <cstdlib>\n";
    result_cpp += "#include <fmt/format.h>\n";
    result_cpp += "#include <runtime/ps1_runtime_macros.h>\n";
    result_cpp += "#include <runtime/cpu_context.h>\n";
    result_cpp += "#include <runtime/gte.h>\n";
    result_cpp += "#include <runtime/mips_interpreter.h>\n";
    result_cpp +=
        "#if defined(__GNUC__) && !defined(__clang__)\n"
        "#define PS1_RECOMP_HOT_OPTIMIZE "
        "__attribute__((hot, optimize(\"O2\")))\n"
        "#else\n"
        "#define PS1_RECOMP_HOT_OPTIMIZE\n"
        "#endif\n";
    // Generated functions use the stable helpers in ps1_runtime_macros.h;
    // pulling the complete BIOS/Memory/GPU device graph here made every
    // renderer header edit rebuild the multi-megabyte game translation unit.
    result_cpp += "\n";
    result_cpp += "// Forward declaration for OOB dispatch\n";
    result_cpp += "void recomp_dispatch(uint8_t* rdram, recomp_context* ctx, "
                  "uint32_t addr);\n\n";
    result_cpp +=
        "void recomp_resume_dispatch(uint8_t* rdram, recomp_context* ctx, "
        "uint32_t addr);\n\n";
    result_cpp +=
        "static bool recomp_try_static_interpreter(uint8_t* rdram, "
        "recomp_context* ctx, uint32_t addr);\n\n";

    const auto &functions =
        toml::find<std::vector<toml::value>>(config, "functions");

    struct ConfiguredRange {
      uint32_t start;
      uint32_t end;
    };
    struct ConfiguredIndirectProfile {
      uint32_t start;
      uint32_t end;
      std::vector<uint32_t> targets;
      std::vector<uint32_t> sourcePcs;
    };
    struct ConfiguredEntryAliases {
      uint32_t start;
      uint32_t end;
      std::vector<uint32_t> targets;
    };
    std::vector<ConfiguredRange> configuredRanges;
    std::vector<ConfiguredIndirectProfile> configuredIndirectProfiles;
    std::vector<ConfiguredEntryAliases> configuredEntryAliases;
    configuredRanges.reserve(functions.size());
    for (const auto &func : functions) {
      const uint32_t start = std::stoul(
          toml::find<std::string>(func, "address"), nullptr, 16);
      const uint32_t size =
          static_cast<uint32_t>(toml::find<int64_t>(func, "size"));
      configuredRanges.push_back({start, start + size});
      if (func.contains("indirect_targets")) {
        ConfiguredIndirectProfile profile{start, start + size, {}, {}};
        const auto &targets = toml::find<std::vector<std::string>>(
            func, "indirect_targets");
        profile.targets.reserve(targets.size());
        for (const std::string &target : targets)
          profile.targets.push_back(
              static_cast<uint32_t>(std::stoul(target, nullptr, 0)));
        if (func.contains("indirect_source_pcs")) {
          const auto &sources = toml::find<std::vector<std::string>>(
              func, "indirect_source_pcs");
          profile.sourcePcs.reserve(sources.size());
          for (const std::string &source : sources)
            profile.sourcePcs.push_back(
                static_cast<uint32_t>(std::stoul(source, nullptr, 0)));
        }
        configuredIndirectProfiles.push_back(std::move(profile));
      }
      if (func.contains("entry_aliases")) {
        ConfiguredEntryAliases aliases{start, start + size, {}};
        const auto &targets =
            toml::find<std::vector<std::string>>(func, "entry_aliases");
        aliases.targets.reserve(targets.size());
        for (const std::string &targetText : targets) {
          const uint32_t target = static_cast<uint32_t>(
              std::stoul(targetText, nullptr, 0));
          if ((target & 3u) != 0 || target <= start ||
              target >= start + size) {
            throw std::runtime_error(fmt::format(
                "entry alias 0x{:08X} is not an aligned internal address "
                "of 0x{:08X}..0x{:08X}",
                target, start, start + size));
          }
          aliases.targets.push_back(target);
        }
        configuredEntryAliases.push_back(std::move(aliases));
      }
    }
    std::sort(configuredRanges.begin(), configuredRanges.end(),
              [](const ConfiguredRange &a, const ConfiguredRange &b) {
                return a.start < b.start;
              });

    auto containingRangeEnd = [&](uint32_t target) {
      // Hand-written PS1 assembly commonly has several analyzer-created
      // entries whose ranges overlap.  The most recently started range is
      // not necessarily the one that reaches furthest, so retain the maximum
      // end of every configured owner of this address.  Otherwise a branch
      // into a short inner entry can still leave the surrounding shared
      // control-flow region truncated.
      uint32_t end = target + 4;
      for (const ConfiguredRange &range : configuredRanges) {
        if (range.start > target)
          break;
        if (target < range.end)
          end = std::max(end, range.end);
      }
      return end;
    };

    auto containingRangeStart = [&](uint32_t target) {
      // Mirror containingRangeEnd for computed entries which precede the
      // analyzer's chosen function start.  A relocated opcode table describes
      // one control-flow island even when the analyzer split that island into
      // several overlapping callable ranges.  Starting at the earliest owner
      // keeps those transfers as local gotos instead of recursively entering
      // the private-label interpreter.
      uint32_t start = target;
      for (const ConfiguredRange &range : configuredRanges) {
        if (range.start > target)
          break;
        if (target < range.end)
          start = std::min(start, range.start);
      }
      return start;
    };

    auto isInsideConfiguredRange = [&](uint32_t target) {
      return std::any_of(configuredRanges.begin(), configuredRanges.end(),
                         [target](const ConfiguredRange &range) {
                           return target >= range.start && target < range.end;
                         });
    };

    std::map<uint32_t, std::string> addr_to_name;
    std::map<uint32_t, std::string> resume_to_name;
    std::map<uint32_t, std::string> entry_alias_to_name;
    std::vector<uint32_t> known_funcs;
    for (const auto &func : functions) {
      std::string name = toml::find<std::string>(func, "name");
      if (name == "main")
        name = "func_main";
      for (char &c : name)
        if (c == '.')
          c = '_';
      uint32_t addr =
          std::stoul(toml::find<std::string>(func, "address"), nullptr, 16);
      known_funcs.push_back(addr);
      addr_to_name[addr] = name;
      result_cpp +=
          fmt::format("void {}(uint8_t* rdram, recomp_context* ctx);\n", name);
    }

    // Forward-declare HLE overrides and missing functions so they are
    // visible to the dispatch table and to other functions that call them.
    for (const auto &[addr, stub] : hleOverrides) {
      if (!addr_to_name.count(addr)) {
        addr_to_name[addr] = fmt::format("func_{:08X}", addr);
        known_funcs.push_back(addr);
        result_cpp += fmt::format(
            "void func_{:08X}(uint8_t* rdram, recomp_context* ctx);\n", addr);
      }
    }
    for (const auto &[addr, body] : missingFunctions) {
      if (!addr_to_name.count(addr)) {
        addr_to_name[addr] = fmt::format("func_{:08X}", addr);
        known_funcs.push_back(addr);
        result_cpp += fmt::format(
            "void func_{:08X}(uint8_t* rdram, recomp_context* ctx);\n", addr);
      }
    }

    // Forward-declare the runtime PsyQ dispatcher once (resolution by name
    // happens at runtime via psyq_registry), and ensure each HLE'd address
    // has a translation-unit-local symbol name.
    if (!hleFunctions.empty()) {
      result_cpp += ps1recomp::emitHleForwardDecl(ps1recomp::HleStub{});
      for (const auto &[addr, hleName] : hleFunctions) {
        if (!addr_to_name.count(addr)) {
          addr_to_name[addr] = fmt::format("func_{:08X}", addr);
          known_funcs.push_back(addr);
          result_cpp += fmt::format(
              "void func_{:08X}(uint8_t* rdram, recomp_context* ctx);\n", addr);
        }
      }
    }

    // Also include stubs in known functions if they exist.
    // [[hle_functions]] takes precedence: when the same address appears in
    // both, the HLE body is emitted under func_<addr> and we must not let
    // the legacy stub overwrite the dispatch entry.
    if (config.contains("stubs")) {
      const auto &stubs = toml::find<std::vector<toml::value>>(config, "stubs");
      for (const auto &s : stubs) {
        std::string name = toml::find<std::string>(s, "name");
        if (name == "main")
          name = "func_main";
        for (char &c : name)
          if (c == '.')
            c = '_';
        uint32_t addr =
            std::stoul(toml::find<std::string>(s, "address"), nullptr, 16);
        if (hleFunctions.count(addr))
          continue;
        known_funcs.push_back(addr);
        addr_to_name[addr] = name;
        result_cpp += fmt::format(
            "void {}(uint8_t* rdram, recomp_context* ctx);\n", name);
      }
    }

    // `entry_aliases` are data-driven entry PCs inside one already emitted
    // hand-written assembly body. They intentionally do not become duplicate
    // C++ functions: dispatch selects a local label in the owner through the
    // same one-shot PC mechanism used for savestate continuations.
    for (const ConfiguredEntryAliases &aliases : configuredEntryAliases) {
      const auto owner = addr_to_name.find(aliases.start);
      if (owner == addr_to_name.end())
        throw std::runtime_error("entry alias owner was not emitted");
      for (uint32_t target : aliases.targets) {
        if (addr_to_name.count(target)) {
          throw std::runtime_error(fmt::format(
              "entry alias 0x{:08X} duplicates a standalone function",
              target));
        }
        const auto [_, inserted] =
            entry_alias_to_name.emplace(target, owner->second);
        if (!inserted) {
          throw std::runtime_error(fmt::format(
              "entry alias 0x{:08X} has more than one owner", target));
        }
      }
    }

    result_cpp += "\n";
    result_cpp += kTranslationDefinitionsBegin;

    InstructionEmitter emitter;
    emitter.setFuncResolver([&addr_to_name](uint32_t addr) {
      if (addr_to_name.count(addr))
        return addr_to_name[addr];
      return std::string{};
    });
    emitter.setInstructionOverrides(instructionOverrides);

    // Analyzer ranges can contain inline tables after an unconditional jump.
    // Registering every data word which happens to decode as JAL would turn
    // that data back into executable C++ and greatly inflate the translation.
    // Walk only the emitter's reachable CFG when collecting savestate return
    // PCs. Known computed-jump targets restart reachability just like labels.
    auto collectResumeTargets =
        [&resume_to_name](RecompFunction &rfunc, uint32_t ownerStart,
                          uint32_t ownerEnd, const std::string &ownerName) {
          std::vector<bool> labels = rfunc.isLabelTarget;
          labels.resize(rfunc.instructions.size(), false);
          const uint32_t callableEntry =
              rfunc.entryAddress == 0 ? rfunc.address : rfunc.entryAddress;
          if (callableEntry >= rfunc.address) {
            const size_t idx = (callableEntry - rfunc.address) / 4;
            if (idx < labels.size())
              labels[idx] = true;
          }
          const auto markLabel = [&](uint32_t target) {
            if (target < rfunc.address ||
                ((target - rfunc.address) & 3u) != 0)
              return;
            const size_t idx = (target - rfunc.address) / 4;
            if (idx < labels.size())
              labels[idx] = true;
          };
          for (const auto &table : rfunc.jumpTables)
            for (uint32_t target : table.targets)
              markLabel(target);
          for (uint32_t target : rfunc.indirectTargets)
            markLabel(target);

          bool reachable = false;
          size_t stopAfterDelay = rfunc.instructions.size();
          for (size_t i = 0; i < rfunc.instructions.size(); ++i) {
            if (labels[i]) {
              reachable = true;
              stopAfterDelay = rfunc.instructions.size();
            }
            if (!reachable)
              continue;

            const Instruction inst =
                MipsDecoder::decode(rfunc.instructions[i]);
            const uint32_t pc =
                rfunc.address + static_cast<uint32_t>(i * 4);
            if (inst.id == InstrId::JAL || inst.id == InstrId::JALR ||
                inst.id == InstrId::BLTZAL || inst.id == InstrId::BGEZAL) {
              const uint32_t resumePc = pc + 8;
              if (pc >= ownerStart && pc < ownerEnd &&
                  resumePc >= rfunc.address &&
                  resumePc < rfunc.address + rfunc.size) {
                rfunc.resumeTargets.push_back(resumePc);
                resume_to_name.emplace(resumePc, ownerName);
              }
            }

            if (i == stopAfterDelay) {
              reachable = false;
              stopAfterDelay = rfunc.instructions.size();
            }
            if (inst.id == InstrId::J || inst.id == InstrId::JR)
              stopAfterDelay = i + 1;
            else if (inst.id == InstrId::BREAK)
              reachable = false;
          }
        };

    fmt::print("Recompiling {} functions...\n", functions.size());

    // Tracks HLE addresses for which we've already emitted a body, so the
    // post-loop sweep below doesn't double-emit any entry that overlapped
    // with [[functions]].
    std::set<uint32_t> emittedHleAddrs;

    for (const auto &func : functions) {
      std::string name = toml::find<std::string>(func, "name");
      if (name == "main")
        name = "func_main";
      for (char &c : name)
        if (c == '.')
          c = '_';
      uint32_t addr =
          std::stoul(toml::find<std::string>(func, "address"), nullptr, 16);
      uint32_t size = toml::find<int64_t>(func, "size");
      const uint32_t configuredEnd = addr + size;

      const auto interpreterRegion = std::find_if(
          interpreterRegions.begin(), interpreterRegions.end(),
          [addr](const InterpreterRegion &region) {
            return addr >= region.start && addr < region.end;
          });
      if (interpreterRegion != interpreterRegions.end() &&
          !hleFunctions.count(addr) && !hleOverrides.count(addr) &&
          !missingFunctions.count(addr)) {
        result_cpp += fmt::format(
            "// Static interpreter compatibility island [{}]\n"
            "void {}(uint8_t* rdram, recomp_context* ctx) {{\n"
            "    if (!ps1::mips::interpretStaticCall(rdram, ctx, "
            "0x{:08X}u, 0x{:08X}u, 0x{:08X}u)) {{\n"
            "        fmt::print(stderr, \"[STATIC-MIPS] entry 0x{:08X} "
            "failed\\n\");\n"
            "        std::abort();\n"
            "    }}\n"
            "}}\n\n",
            interpreterRegion->name, name, addr, interpreterRegion->start,
            interpreterRegion->end, addr);
        continue;
      }

      // Extract instructions
      auto *section = parser.findSectionByAddress(addr);
      if (!section || section->type != SectionType::Text)
        continue;

      uint32_t regionStart = addr;
      uint32_t offset = regionStart - section->vaddr;
      uint32_t num_instrs = size / 4;

      RecompFunction rfunc;
      rfunc.name = name;
      rfunc.address = regionStart;
      rfunc.entryAddress = addr;
      rfunc.size = size;
      rfunc.instructions.resize(num_instrs);
      rfunc.isLabelTarget.resize(num_instrs, false);

      auto loadInstructions = [&]() {
        offset = regionStart - section->vaddr;
        num_instrs = size / 4;
        rfunc.address = regionStart;
        rfunc.entryAddress = addr;
        rfunc.size = size;
        rfunc.instructions.resize(num_instrs);
        rfunc.isLabelTarget.assign(num_instrs, false);
        for (uint32_t i = 0; i < num_instrs; ++i) {
          const uint8_t *ptr = section->data + offset + (i * 4);
          rfunc.instructions[i] =
              static_cast<uint32_t>(ptr[0]) |
              (static_cast<uint32_t>(ptr[1]) << 8) |
              (static_cast<uint32_t>(ptr[2]) << 16) |
              (static_cast<uint32_t>(ptr[3]) << 24);
        }
      };
      loadInstructions();

      // A relocated opcode table can jump beyond an analyzer-created
      // boundary because helper routines inside the dispatch region are also
      // direct JAL targets.  Duplicate the full region in the dispatch owner
      // (the helpers remain separately callable) so every table entry and the
      // common loop-back label can stay a local C++ goto.
      uint32_t dispatchRegionEnd = addr + size;
      for (uint32_t i = 0; i < num_instrs; ++i) {
        const Instruction inst = MipsDecoder::decode(rfunc.instructions[i]);
        if (inst.id != InstrId::JR || inst.rs == 31)
          continue;
        if (!detectJumpTable(rfunc.instructions, i, parser).empty() ||
            !detectArithmeticJumpTargets(rfunc.instructions, i, addr).empty())
          continue;
        const auto targets = detectRelocatedJumpTable(
            rfunc.instructions, i, parser, addr, addr + size);
        for (uint32_t target : targets)
          dispatchRegionEnd =
              std::max(dispatchRegionEnd, containingRangeEnd(target));
      }
      const uint32_t sectionEnd = section->vaddr + section->size;
      dispatchRegionEnd = std::min(dispatchRegionEnd, sectionEnd);
      if (dispatchRegionEnd > addr + size) {
        const uint32_t originalSize = size;
        size = dispatchRegionEnd - addr;
        loadInstructions();
        fmt::print("  [dispatch region] func 0x{:08X}: {} -> {} bytes\n",
                   addr, originalSize, size);
      }

      // Hand-written assembly frequently exposes several callable entries
      // into one shared control-flow region. An analyzer quite reasonably
      // records each entry as a function, but those entries can branch back
      // into the middle of an earlier configured body. Include every such
      // backward target in this emitted C++ scope even when no jump table
      // caused a forward dispatch-region expansion. `entryAddress` keeps a
      // normal direct call starting at the configured entry.
      //
      // Only follow targets that another configured function already owns.
      // This prevents an instruction-looking data word after a hand-written
      // `jr` from pulling an arbitrary 64 KiB prefix into the function.
      for (;;) {
        uint32_t backwardStart = regionStart;
        for (uint32_t i = 0; i < num_instrs; ++i) {
          const Instruction inst = MipsDecoder::decode(rfunc.instructions[i]);
          if (inst.category != InstrCategory::Branch)
            continue;
          const uint32_t pc = regionStart + i * 4;
          const uint32_t target = inst.branchTarget(pc);
          if (target < backwardStart && target >= section->vaddr &&
              addr - target <= 0x10000 && isInsideConfiguredRange(target)) {
            // A branch can enter the middle of a small helper which is also
            // reached by JAL elsewhere in this shared assembly island.  The
            // helper may tail-branch back into the owner's main loop, so
            // copying only the branch destination leaves its callable entry
            // outside this C++ scope.  That turns the JAL into a nested C++
            // call and lets the helper execute the owner's epilogue against
            // the wrong guest frame.  Include the complete configured owner
            // of the destination so the architectural call and its tail
            // transfer can both remain local gotos.
            backwardStart =
                std::min(backwardStart, containingRangeStart(target));
          }
        }
        if (backwardStart >= regionStart)
          break;

        const uint32_t forwardEnd = regionStart + size;
        regionStart = backwardStart;
        size = forwardEnd - regionStart;
        loadInstructions();
        fmt::print("  [control-flow prefix] func 0x{:08X}: starts at "
                   "0x{:08X} (entry remains 0x{:08X})\n",
                   addr, regionStart, addr);
      }

      // The symmetric case is just as important: a conditional branch can
      // enter a later analyzer-created function without being a computed
      // jump-table transfer.  Treating that destination as an out-of-bounds
      // dispatcher call changes ordinary branch control flow into a fresh
      // function call and loses the shared renderer state.  Crash 3 uses
      // exactly this layout at 0x8003D460 -> 0x8003DDC4.
      //
      // Compute a fixed point because the newly included owner can itself
      // branch into another overlapping owner.  As with the backward pass,
      // require that the target is already recognized by the configuration;
      // this keeps instruction-looking words in inline data from growing the
      // region arbitrarily.
      for (;;) {
        uint32_t forwardEnd = regionStart + size;
        for (uint32_t i = 0; i < num_instrs; ++i) {
          const Instruction inst = MipsDecoder::decode(rfunc.instructions[i]);
          if (inst.category != InstrCategory::Branch)
            continue;
          const uint32_t pc = regionStart + i * 4;
          const uint32_t target = inst.branchTarget(pc);
          if (target >= forwardEnd && target < sectionEnd && target >= addr &&
              target - addr <= 0x10000 && isInsideConfiguredRange(target))
            forwardEnd = std::max(forwardEnd, containingRangeEnd(target));
        }
        forwardEnd = std::min(forwardEnd, sectionEnd);
        if (forwardEnd <= regionStart + size)
          break;

        const uint32_t originalSize = size;
        size = forwardEnd - regionStart;
        loadInstructions();
        fmt::print("  [control-flow suffix] func 0x{:08X}: {} -> {} bytes\n",
                   addr, originalSize, size);
      }

      // A configured range occasionally ends on JR/JALR itself instead of
      // including its architectural delay slot.  This is especially easy to
      // produce for hand-written assembly with several shared epilogues: the
      // analyzer records the last control-flow instruction as the boundary,
      // while the instruction at +4 still belongs to that return path.
      //
      // Silently dropping it is not benign.  Crash 3's renderer has a
      // `jr $ra; addiu $sp, $sp, 0x14` epilogue at 0x8003EA24.  Omitting the
      // delay slot leaks 20 bytes of stack on only one model-rendering branch
      // and corrupts the callback registers much later.  Repair register-jump
      // boundaries here so every config, including donor-profiled shared
      // regions, gets true R3000A semantics without game-specific stubs.
      if (num_instrs != 0 && regionStart + size + 4 <= sectionEnd) {
        const Instruction last =
            MipsDecoder::decode(rfunc.instructions[num_instrs - 1]);
        if (last.id == InstrId::JR || last.id == InstrId::JALR) {
          const uint32_t originalSize = size;
          size += 4;
          loadInstructions();
          fmt::print("  [delay slot boundary] func 0x{:08X}: {} -> {} bytes\n",
                     addr, originalSize, size);
        }
      }

      // The first dispatch-table scan above deliberately starts from the
      // analyzer range.  Branch-based expansion can reveal a computed JR
      // later in the same hand-written island, though.  Re-scan to a fixed
      // point after that expansion and include both sides of every recovered
      // table.  Without the backward half, an entry such as Crash 3's GOOL
      // handler block is emitted as an OOB dispatcher stub; each handler then
      // tail-dispatches the next private label and grows the native stack once
      // per guest opcode (15k+ frames in the title-to-demo transition).
      //
      // Keeping the recovered island in one emitted C++ function is both the
      // faithful model and the fast path: computed transfers become local
      // gotos while independently callable ABI entries remain available as
      // their own generated functions.
      for (;;) {
        uint32_t computedStart = regionStart;
        uint32_t computedEnd = regionStart + size;
        for (uint32_t i = 0; i < num_instrs; ++i) {
          const Instruction inst = MipsDecoder::decode(rfunc.instructions[i]);
          if (inst.id != InstrId::JR || inst.rs == 31)
            continue;

          std::vector<uint32_t> targets =
              detectJumpTable(rfunc.instructions, i, parser);
          if (targets.empty())
            targets = detectArithmeticJumpTargets(rfunc.instructions, i,
                                                   regionStart);
          if (targets.empty()) {
            targets = detectRelocatedJumpTable(
                rfunc.instructions, i, parser, regionStart,
                regionStart + size);
          }

          for (uint32_t target : targets) {
            const uint64_t distance = target >= addr
                                          ? uint64_t{target - addr}
                                          : uint64_t{addr - target};
            if (distance > 0x10000u || target < section->vaddr ||
                target >= sectionEnd || !isInsideConfiguredRange(target)) {
              continue;
            }
            computedStart =
                std::min(computedStart, containingRangeStart(target));
            computedEnd = std::max(computedEnd, containingRangeEnd(target));
          }
        }

        computedStart = std::max(computedStart, section->vaddr);
        computedEnd = std::min(computedEnd, sectionEnd);
        if (computedStart == regionStart &&
            computedEnd == regionStart + size) {
          break;
        }

        const uint32_t originalStart = regionStart;
        const uint32_t originalSize = size;
        regionStart = computedStart;
        size = computedEnd - computedStart;
        loadInstructions();
        fmt::print("  [computed dispatch fixed point] func 0x{:08X}: "
                   "0x{:08X}+{} -> 0x{:08X}+{} bytes\n",
                   addr, originalStart, originalSize, regionStart, size);
      }

      // A newly included owner can itself end at a register jump.  Apply the
      // architectural delay-slot repair once more at the final fixed point.
      if (num_instrs != 0 && regionStart + size + 4 <= sectionEnd) {
        const Instruction last =
            MipsDecoder::decode(rfunc.instructions[num_instrs - 1]);
        if (last.id == InstrId::JR || last.id == InstrId::JALR) {
          const uint32_t originalSize = size;
          size += 4;
          loadInstructions();
          fmt::print("  [computed dispatch delay slot] func 0x{:08X}: "
                     "{} -> {} bytes\n",
                     addr, originalSize, size);
        }
      }

      // Donor-profiled targets belong to the computed-JR sites that select
      // them, not merely to one analyzer-created function boundary. Several
      // callable entries can expand to overlapping copies of the same
      // hand-written renderer. Inherit a profile whenever this emitted copy
      // contains one of its donor-observed source PCs. The profile owner also
      // retains its own targets for backward compatibility with configs that
      // predate `indirect_source_pcs`.
      std::set<uint32_t> inheritedIndirectTargets;
      for (const auto &profile : configuredIndirectProfiles) {
        bool usesProfile = addr == profile.start;
        for (uint32_t sourcePc : profile.sourcePcs) {
          if (sourcePc >= regionStart && sourcePc < regionStart + size) {
            usesProfile = true;
            break;
          }
        }
        if (usesProfile)
          inheritedIndirectTargets.insert(profile.targets.begin(),
                                          profile.targets.end());
      }
      rfunc.indirectTargets.assign(inheritedIndirectTargets.begin(),
                                   inheritedIndirectTargets.end());
      if (!rfunc.indirectTargets.empty() &&
          !func.contains("indirect_targets")) {
        fmt::print("  [indirect profile] func 0x{:08X}: inherited {} "
                   "targets from profiled JR sites\n",
                   addr, rfunc.indirectTargets.size());
      }

      // A profiled target must be real code in the same emitted C++ scope,
      // not an OOB label that merely calls the global dispatcher. Extend the
      // shared copy through the configured owner of every target, matching
      // the expansion already performed for statically recovered relocated
      // jump tables above.
      uint32_t profiledRegionEnd = regionStart + size;
      for (uint32_t target : rfunc.indirectTargets)
        profiledRegionEnd =
            std::max(profiledRegionEnd, containingRangeEnd(target));
      profiledRegionEnd = std::min(profiledRegionEnd, sectionEnd);
      if (profiledRegionEnd > regionStart + size) {
        const uint32_t originalSize = size;
        size = profiledRegionEnd - regionStart;
        loadInstructions();
        fmt::print("  [profiled dispatch region] func 0x{:08X}: {} -> {} "
                   "bytes\n",
                   addr, originalSize, size);
      }

      for (uint32_t i = 0; i < num_instrs; ++i) {
        Instruction inst = MipsDecoder::decode(rfunc.instructions[i]);
        const uint32_t pc = regionStart + (i * 4);
        if (inst.category == InstrCategory::Branch) {
          uint32_t target = inst.branchTarget(pc);
          if (target >= regionStart && target < regionStart + size) {
            uint32_t target_idx = (target - regionStart) / 4;
            if (target_idx < num_instrs)
              rfunc.isLabelTarget[target_idx] = true;
          }
        } else if (inst.category == InstrCategory::Jump &&
                   (inst.id == InstrId::J || inst.id == InstrId::JAL)) {
          uint32_t target = inst.jumpTarget(pc);
          if (target >= regionStart && target < regionStart + size) {
            uint32_t target_idx = (target - regionStart) / 4;
            if (target_idx < num_instrs)
              rfunc.isLabelTarget[target_idx] = true;
          }
          if (inst.id == InstrId::JAL) {
            bool found = false;
            for (auto k : known_funcs) {
              if (k == target) {
                found = true;
                break;
              }
            }
            if (!found) {
              known_funcs.push_back(target);
              result_cpp += fmt::format(
                  "void func_{:08X}(uint8_t* rdram, recomp_context* ctx) {{ /* "
                  "STUB generated by JAL */ }}\n",
                  target);
            }
          }
        } else if (inst.id == InstrId::JR && inst.rs != 31) {
          // Jump table auto-detection
          // Try to detect the LUI+ADDIU+ADDU+LW+JR pattern and resolve
          // the table entries from the ELF binary.
          auto targets = detectJumpTable(rfunc.instructions, i, parser);
          std::string dispatchKind = "memory";
          if (targets.empty()) {
            targets = detectArithmeticJumpTargets(rfunc.instructions, i,
                                                  regionStart);
            dispatchKind = "arithmetic";
          }
          if (targets.empty()) {
            targets = detectRelocatedJumpTable(rfunc.instructions, i, parser,
                                               regionStart,
                                               regionStart + size);
            dispatchKind = "relocated";
          }
          if (!targets.empty()) {
            JumpTableEntry jt;
            jt.jrInstrIdx = i;
            jt.targets = std::move(targets);
            fmt::print("  [jump table] func 0x{:08X}+{}: {} targets ({} "
                       "dispatch detected)\n",
                       addr, i * 4, jt.targets.size(),
                       dispatchKind);
            rfunc.jumpTables.push_back(std::move(jt));
          }
        }
      }

      if (!hleFunctions.count(addr) && !hleOverrides.count(addr) &&
          !missingFunctions.count(addr)) {
        for (const ConfiguredEntryAliases &aliases :
             configuredEntryAliases) {
          if (aliases.start != addr)
            continue;
          for (uint32_t target : aliases.targets) {
            if (target < rfunc.address || target >= rfunc.address + rfunc.size)
              throw std::runtime_error(fmt::format(
                  "entry alias 0x{:08X} is outside emitted owner {}",
                  target, name));
            rfunc.entryAliases.push_back(target);
          }
        }
        collectResumeTargets(rfunc, addr, configuredEnd, name);
      }

      // HLE function (hash-detected): delegate to extern "C" hle_<name>
      if (hleFunctions.count(addr)) {
        ps1recomp::HleStub stub{addr, name, hleFunctions[addr]};
        result_cpp += ps1recomp::emitHleStub(stub);
        result_cpp += "\n";
        emittedHleAddrs.insert(addr);
        continue;
      }

      // HLE override: emit a named stub instead of translating MIPS
      if (hleOverrides.count(addr)) {
        const std::string &stub = hleOverrides[addr];
        result_cpp += fmt::format(
            "// HLE override for 0x{:08X} (replaces MIPS translation)\n"
            "void {}(uint8_t* rdram, recomp_context* ctx) {{\n"
            "    ps1::psyq::{}(ctx);\n"
            "}}\n\n",
            addr, name, stub);
        continue;
      }

      // Missing function: emit literal body
      if (missingFunctions.count(addr)) {
        const std::string &body = missingFunctions[addr];
        result_cpp += fmt::format(
            "// Missing function stub for 0x{:08X}\n"
            "void {}(uint8_t* rdram, recomp_context* ctx) {{\n"
            "    {}\n"
            "}}\n\n",
            addr, name, body);
        continue;
      }

      if (optimizedFunctions.count(addr))
        result_cpp += "PS1_RECOMP_HOT_OPTIMIZE ";
      result_cpp += emitter.emitFunction(rfunc);
      result_cpp += "\n";
    }

    // HLE stub bodies for [[hle_functions]] not covered by [[functions]]
    // The analyzer emits a [[hle_functions]] entry for every hash match but
    // also strips those addresses from [[functions]] (PsyQ functions don't
    // get MIPS bodies). So most HLE entries land here -- bodies are emitted
    // under the `func_<addr>` symbol the dispatch table maps to.
    for (const auto &[addr, hleName] : hleFunctions) {
      if (emittedHleAddrs.count(addr))
        continue;
      // [hle_overrides] / [missing_functions] take precedence: they emit
      // their own body under the same `func_<addr>` symbol below.
      if (hleOverrides.count(addr) || missingFunctions.count(addr))
        continue;
      std::string fname = addr_to_name.count(addr)
                              ? addr_to_name[addr]
                              : fmt::format("func_{:08X}", addr);
      ps1recomp::HleStub stub{addr, fname, hleName};
      result_cpp += ps1recomp::emitHleStub(stub);
      result_cpp += "\n";
      emittedHleAddrs.insert(addr);
    }

    // Emit stub implementations for PsyQ stubs.
    // For well-known PsyQ SDK function names, delegate to the psyq_hle layer
    // so that VSync/DrawSync/etc. have proper behaviour.
    // Unknown names fall back to a safe no-op body.
    if (config.contains("stubs")) {
      const auto &stubs = toml::find<std::vector<toml::value>>(config, "stubs");
      for (const auto &s : stubs) {
        std::string name = toml::find<std::string>(s, "name");
        for (char &c : name)
          if (c == '.')
            c = '_';
        uint32_t stubAddr =
            std::stoul(toml::find<std::string>(s, "address"), nullptr, 16);
        if (hleFunctions.count(stubAddr))
          continue;

        std::string body;
        if (name == "VSync") {
          body = "ps1::psyq::hle_VSync(ctx);";
        } else if (name == "DrawSync") {
          body = "ps1::psyq::hle_DrawSync(ctx);";
        } else if (name == "ResetGraph") {
          body = "ps1::psyq::hle_ResetGraph(ctx);";
        } else if (name == "ClearOTag") {
          body = "ps1::psyq::hle_ClearOTag(ctx);";
        } else if (name == "ClearOTagR") {
          body = "ps1::psyq::hle_ClearOTagR(ctx);";
        } else if (name == "DrawOTag") {
          body = "ps1::psyq::hle_DrawOTag(ctx);";
        } else if (name == "SetDefDispEnv") {
          body = "ps1::psyq::hle_SetDefDispEnv(ctx);";
        } else if (name == "PutDispEnv") {
          body = "ps1::psyq::hle_PutDispEnv(ctx);";
        } else if (name == "SetDefDrawEnv") {
          body = "ps1::psyq::hle_SetDefDrawEnv(ctx);";
        } else if (name == "PutDrawEnv") {
          body = "ps1::psyq::hle_PutDrawEnv(ctx);";
        } else {
          body = "/* PsyQ Stub */";
        }

        result_cpp += fmt::format(
            "void {}(uint8_t* rdram, recomp_context* ctx) {{ {} }}\n",
            name, body);
      }
    }

    // Overlay support
    // Parse [[overlays]] from config. Each overlay is a separate code segment
    // loaded from disc into RAM at a specific address. Functions are prefixed
    // with overlay name to avoid collisions when multiple overlays share RAM.
    OverlayHandler overlayHandler;
    std::string overlayLookupBody;
    std::string overlayResumeBody;
    if (config.contains("overlays")) {
      const auto &overlays =
          toml::find<std::vector<toml::value>>(config, "overlays");

      fmt::print("Processing {} overlay(s)...\n", overlays.size());

      for (const auto &ov : overlays) {
        std::string ovl_name = toml::find<std::string>(ov, "name");
        uint32_t ovl_ramBase =
            static_cast<uint32_t>(toml::find<int64_t>(ov, "ram_base"));
        uint32_t ovl_size =
            static_cast<uint32_t>(toml::find<int64_t>(ov, "size"));

        // Load overlay binary data
        std::string ovl_path;
        if (ov.contains("binary_path")) {
          ovl_path = toml::find<std::string>(ov, "binary_path");
        } else if (ov.contains("disc_path")) {
          ovl_path = toml::find<std::string>(ov, "disc_path");
        }

        std::vector<uint8_t> ovl_data;
        uint32_t ovl_codeOffset = 0;
        uint32_t ovl_codeSize = ovl_size;

        if (!ovl_path.empty()) {
          // Resolve path relative to config file directory
          std::filesystem::path cfgDir =
              std::filesystem::path(config_path).parent_path();
          std::filesystem::path fullPath = cfgDir / ovl_path;
          if (!std::filesystem::exists(fullPath))
            fullPath = ovl_path; // try as absolute

          std::ifstream ovlFile(fullPath, std::ios::binary);
          if (ovlFile) {
            ovlFile.seekg(0, std::ios::end);
            auto fileSize = ovlFile.tellg();
            ovlFile.seekg(0, std::ios::beg);
            ovl_data.resize(fileSize);
            ovlFile.read(reinterpret_cast<char *>(ovl_data.data()), fileSize);

            // Check for PS-X EXE header
            if (ovl_data.size() >= 2048 &&
                std::memcmp(ovl_data.data(), "PS-X EXE", 8) == 0) {
              ovl_codeOffset = 2048;
              // Read tAddr/tSize from header
              uint32_t tAddr, tSize;
              std::memcpy(&tAddr, ovl_data.data() + 24, 4);
              std::memcpy(&tSize, ovl_data.data() + 28, 4);
              ovl_ramBase = tAddr; // Use header's own address
              ovl_codeSize = tSize;
              fmt::print("  [{}] PS-X EXE: ram=0x{:08X}, code={}B\n", ovl_name,
                         ovl_ramBase, ovl_codeSize);
            }
          } else {
            fmt::print(stderr, "  Warning: Could not open overlay binary: {}\n",
                       fullPath.string());
            continue;
          }
        }

        const auto baseNames = addr_to_name;
        const auto baseResumes = resume_to_name;
        const uint32_t identitySize = toml::find_or<int64_t>(ov, "identity_size", 0);
        if (identitySize &&
            (ovl_codeOffset > ovl_data.size() ||
             identitySize > ovl_data.size() - ovl_codeOffset ||
             uint64_t(ovl_ramBase & 0x1fffffffu) + identitySize > 0x200000u))
          throw std::runtime_error("Overlay identity outside RAM or binary");

        // Get function list -- from config or scan for prologues
        std::vector<uint32_t> ovl_funcs;
        if (ov.contains("functions")) {
          auto funcs = toml::find<std::vector<int64_t>>(ov, "functions");
          for (auto f : funcs)
            ovl_funcs.push_back(static_cast<uint32_t>(f));
        } else if (!ovl_data.empty() &&
                   ovl_codeOffset + ovl_codeSize <= ovl_data.size()) {
          // Auto-detect function prologues and direct JAL targets. Leaf
          // functions often have no stack frame, so prologue-only discovery
          // leaves valid calls pointing at C++ symbols that were never emitted.
          const uint8_t *code = ovl_data.data() + ovl_codeOffset;
          size_t codeLen = ovl_codeSize;
          auto [functionScore, prologueFunctions] =
              OverlayScanner::detectFunctions(code, codeLen, ovl_ramBase);
          (void)functionScore;
          ovl_funcs.insert(ovl_funcs.end(), prologueFunctions.begin(),
                           prologueFunctions.end());
          for (size_t i = 0; i + 3 < codeLen; i += 4) {
            uint32_t word = static_cast<uint32_t>(code[i]) |
                            (static_cast<uint32_t>(code[i + 1]) << 8) |
                            (static_cast<uint32_t>(code[i + 2]) << 16) |
                            (static_cast<uint32_t>(code[i + 3]) << 24);

            Instruction inst = MipsDecoder::decode(word);
            if (inst.id == InstrId::JAL) {
              uint32_t pc = ovl_ramBase + static_cast<uint32_t>(i);
              uint32_t target = inst.jumpTarget(pc);
              if (target >= ovl_ramBase &&
                  target < ovl_ramBase + ovl_codeSize) {
                ovl_funcs.push_back(target);
              }
            }
          }
          fmt::print("  [{}] Auto-detected {} functions\n", ovl_name,
                     ovl_funcs.size());
        }

        if (ov.contains("additional_functions")) {
          for (auto value : toml::find<std::vector<int64_t>>(ov, "additional_functions")) {
            if (value < 0 || static_cast<uint64_t>(value) > UINT32_MAX)
              throw std::runtime_error("Additional overlay function is not a 32-bit address");
            const uint32_t address = static_cast<uint32_t>(value);
            if ((address & 3u) || address < ovl_ramBase ||
                static_cast<uint64_t>(address) >= static_cast<uint64_t>(ovl_ramBase) + ovl_codeSize)
              throw std::runtime_error("Additional overlay function outside code range");
            ovl_funcs.push_back(address);
          }
        }

        // Determine function sizes (distance to next function)
        std::sort(ovl_funcs.begin(), ovl_funcs.end());
        ovl_funcs.erase(std::unique(ovl_funcs.begin(), ovl_funcs.end()),
                        ovl_funcs.end());

        // Forward-declare overlay functions
        for (size_t fi = 0; fi < ovl_funcs.size(); ++fi) {
          uint32_t fAddr = ovl_funcs[fi];
          std::string fName =
              fmt::format("overlay_{}__{:08X}", ovl_name, fAddr);
          addr_to_name[fAddr] = fName;
          known_funcs.push_back(fAddr);
          result_cpp += fmt::format(
              "void {}(uint8_t* rdram, recomp_context* ctx);\n", fName);
        }

        // Emit overlay functions
        if (!ovl_data.empty() &&
            ovl_codeOffset + ovl_codeSize <= ovl_data.size()) {
          const uint8_t *code = ovl_data.data() + ovl_codeOffset;

          for (size_t fi = 0; fi < ovl_funcs.size(); ++fi) {
            uint32_t fAddr = ovl_funcs[fi];
            uint32_t fEnd = (fi + 1 < ovl_funcs.size())
                                ? ovl_funcs[fi + 1]
                                : (ovl_ramBase + ovl_codeSize);
            uint32_t fSize = fEnd - fAddr;

            uint32_t localOff = fAddr - ovl_ramBase;
            if (localOff + fSize > ovl_codeSize)
              continue;

            uint32_t numInstrs = fSize / 4;
            RecompFunction rfunc;
            rfunc.name = fmt::format("overlay_{}__{:08X}", ovl_name, fAddr);
            rfunc.address = fAddr;
            rfunc.size = fSize;
            rfunc.instructions.resize(numInstrs);
            rfunc.isLabelTarget.resize(numInstrs, false);

            for (uint32_t ii = 0; ii < numInstrs; ++ii) {
              const uint8_t *ptr = code + localOff + (ii * 4);
              rfunc.instructions[ii] =
                  ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);

              Instruction inst = MipsDecoder::decode(rfunc.instructions[ii]);
              const uint32_t pc = fAddr + (ii * 4);
              if (inst.category == InstrCategory::Branch) {
                uint32_t target = inst.branchTarget(pc);
                if (target >= fAddr && target < fAddr + fSize) {
                  uint32_t idx = (target - fAddr) / 4;
                  if (idx < numInstrs)
                    rfunc.isLabelTarget[idx] = true;
                }
              } else if (inst.category == InstrCategory::Jump &&
                         (inst.id == InstrId::J || inst.id == InstrId::JAL)) {
                uint32_t target = inst.jumpTarget(pc);
                if (target >= fAddr && target < fAddr + fSize) {
                  uint32_t idx = (target - fAddr) / 4;
                  if (idx < numInstrs)
                    rfunc.isLabelTarget[idx] = true;
                }
              }
            }

            // Overlay jump tables live in the overlay's own load image, not
            // in a section known to the main executable's ElfParser.  Resolve
            // canonical indexed LW+JR dispatches from that raw region and
            // keep only handlers local to this emitted function.  OOB targets
            // remain on the ordinary indirect-dispatch path.
            for (uint32_t ii = 0; ii < numInstrs; ++ii) {
              const Instruction inst =
                  MipsDecoder::decode(rfunc.instructions[ii]);
              if (inst.id != InstrId::JR || inst.rs == 31)
                continue;
              auto targets = detectJumpTable(
                  rfunc.instructions, ii, parser, code, ovl_ramBase,
                  static_cast<uint32_t>(ovl_data.size() - ovl_codeOffset));
              targets.erase(
                  std::remove_if(targets.begin(), targets.end(),
                                 [fAddr, fEnd](uint32_t target) {
                                   return target < fAddr || target >= fEnd;
                                 }),
                  targets.end());
              if (targets.empty())
                continue;
              JumpTableEntry jt;
              jt.jrInstrIdx = ii;
              jt.targets = std::move(targets);
              fmt::print("  [overlay jump table] {} 0x{:08X}+{}: {} "
                         "targets\n",
                         ovl_name, fAddr, ii * 4, jt.targets.size());
              rfunc.jumpTables.push_back(std::move(jt));
            }

            collectResumeTargets(rfunc, fAddr, fEnd, rfunc.name);

            if (optimizedFunctions.count(fAddr))
              result_cpp += "PS1_RECOMP_HOT_OPTIMIZE ";
            result_cpp += emitter.emitFunction(rfunc);
            result_cpp += "\n";
          }
        } else if (ovl_data.empty()) {
          // No binary data -- emit stubs for declared functions
          for (auto fAddr : ovl_funcs) {
            std::string fName =
                fmt::format("overlay_{}__{:08X}", ovl_name, fAddr);
            result_cpp +=
                fmt::format("void {}(uint8_t* rdram, recomp_context* ctx) "
                            "{{ /* Overlay stub -- no binary data */ }}\n",
                            fName);
          }
        }

        // Identified overlays share RAM but never share a dispatch map. The
        // immutable identity bytes are checked on each external transfer; a
        // stale translation cannot survive a stage load at the same address.
        if (identitySize) {
          std::string condition = fmt::format("    if (normalized >= 0x{:08X}u && normalized < 0x{:08X}u", ovl_ramBase, ovl_ramBase + ovl_codeSize);
          for (uint32_t i = 0; i < identitySize; ++i)
            condition += fmt::format(" && rdram[0x{:X}] == {}", (ovl_ramBase & 0x1fffffffu) + i, ovl_data[ovl_codeOffset + i]);
          condition += ") {\n        switch (normalized) {\n";
          overlayLookupBody += condition;
          for (auto address : ovl_funcs)
            overlayLookupBody += fmt::format("        case 0x{:08X}u: return {};\n", address, addr_to_name.at(address));
          overlayLookupBody += "        default: return nullptr;\n        }\n    }\n";
          overlayResumeBody += condition;
          for (const auto &[address, name] : resume_to_name)
            if (name.rfind("overlay_" + ovl_name + "__", 0) == 0)
              overlayResumeBody += fmt::format("        case 0x{:08X}u: return {};\n", address, name);
          overlayResumeBody += "        default: return nullptr;\n        }\n    }\n";
          addr_to_name = baseNames;
          resume_to_name = baseResumes;
        }

        // Register overlay section for tracking
        OverlaySection sec;
        sec.name = ovl_name;
        sec.ramBase = ovl_ramBase;
        sec.size = ovl_codeSize;
        sec.functions = ovl_funcs;
        overlayHandler.addOverlay(std::move(sec));

        fmt::print("  Overlay [{}]: ram=0x{:08X}, size={}, {} functions\n",
                   ovl_name, ovl_ramBase, ovl_codeSize, ovl_funcs.size());
      }
    }

    result_cpp += kTranslationDefinitionsEnd;

    // Emit overlay table
    // Runtime uses this to know which overlays exist and their address ranges
    if (overlayHandler.overlayCount() > 0) {
      result_cpp += "\n// Overlay Table\n";
      result_cpp += "#include <vector>\n";
      result_cpp += "#include <string>\n\n";
      result_cpp += "struct OverlayInfo {\n";
      result_cpp += "    std::string name;\n";
      result_cpp += "    uint32_t ramBase;\n";
      result_cpp += "    uint32_t size;\n";
      result_cpp += "    int index;\n";
      result_cpp += "};\n\n";
      result_cpp += "static std::vector<OverlayInfo> recomp_overlay_table = {\n";
      int idx = 0;
      for (const auto &ovl : overlayHandler.overlays()) {
        result_cpp += fmt::format(
            "    {{ \"{}\", 0x{:08X}, {}, {} }},\n", ovl.name, ovl.ramBase,
            ovl.size, idx++);
      }
      result_cpp += "};\n\n";
    }

    result_cpp += "\n// Static interpreter fallbacks for private labels\n";
    result_cpp +=
        "static bool recomp_try_static_interpreter(uint8_t* rdram, "
        "recomp_context* ctx, uint32_t addr) {\n";
    result_cpp +=
        "    const uint32_t normalized = (addr & 0x1FFFFFFFu) | "
        "0x80000000u;\n";
    for (const auto &region : interpreterRegions) {
      result_cpp += fmt::format(
          "    if (normalized >= 0x{:08X}u && normalized < 0x{:08X}u) {{\n"
          "        if (!ps1::mips::interpretStaticCall(rdram, ctx, "
          "normalized, 0x{:08X}u, 0x{:08X}u)) std::abort();\n"
          "        return true;\n"
          "    }}\n",
          region.start, region.end, region.start, region.end);
    }
    result_cpp +=
        "    // Indirect callbacks may target executable labels inside a "
        "known function body. Search from the latest owner backwards so "
        "nested/overlapping analyzer ranges select the most specific body.\n"
        "    struct PrivateRange { uint32_t start; uint32_t end; };\n"
        "    static constexpr PrivateRange privateRanges[] = {\n";
    std::vector<ConfiguredRange> privateRanges;
    for (const ConfiguredRange &range : configuredRanges) {
      if (range.start >= range.end || hleFunctions.count(range.start) ||
          hleOverrides.count(range.start) || missingFunctions.count(range.start))
        continue;
      privateRanges.push_back(range);
      result_cpp +=
          fmt::format("        {{0x{:08X}u, 0x{:08X}u}},\n", range.start,
                      range.end);
    }
    result_cpp += "    };\n";
    if (!privateRanges.empty()) {
      // configuredRanges is start-sorted but may contain nested functions.
      // A prefix maximum lets the generated binary lookup stop only when it
      // has proven that no earlier owner can contain the address. This keeps
      // the old "latest containing range wins" rule exactly while replacing
      // the 968-way reverse scan paid by every Crash 3 callback and heap-code
      // dispatch.
      result_cpp +=
          "    static constexpr uint32_t privatePrefixMaxEnd[] = {\n";
      uint32_t prefixMaxEnd = 0;
      for (const ConfiguredRange &range : privateRanges) {
        prefixMaxEnd = std::max(prefixMaxEnd, range.end);
        result_cpp += fmt::format("        0x{:08X}u,\n", prefixMaxEnd);
      }
      result_cpp +=
          "    };\n"
          "    constexpr size_t privateRangeCount = "
          "sizeof(privateRanges) / sizeof(privateRanges[0]);\n"
          "    size_t low = 0;\n"
          "    size_t high = privateRangeCount;\n"
          "    while (low < high) {\n"
          "        const size_t middle = low + (high - low) / 2;\n"
          "        if (privateRanges[middle].start <= normalized) low = middle + 1;\n"
          "        else high = middle;\n"
          "    }\n"
          "    for (size_t i = low; i-- > 0;) {\n"
          "        const PrivateRange& range = privateRanges[i];\n"
          "        if (normalized < range.end) {\n"
          "            if (!ps1::mips::interpretStaticPrivateCall("
          "rdram, ctx, normalized, range.start, range.end)) std::abort();\n"
          "            return true;\n"
          "        }\n"
          "        if (i == 0 || privatePrefixMaxEnd[i - 1] <= normalized) break;\n"
          "    }\n";
    }
    result_cpp += "    return false;\n}\n\n";

    // Emit dispatch table
    // This allows CALL_INDIRECT / JUMP_INDIRECT to resolve
    // addresses to recompiled functions at runtime.
    // Uses std::unordered_map for O(1) average-case lookup.
    result_cpp += "\n// Dispatch Table\n";
    result_cpp += "// Maps PS1 addresses to recompiled function pointers\n";
    result_cpp += "// Populated once at startup, queried on every indirect call/jump\n\n";
    result_cpp += "#include <unordered_map>\n";
    result_cpp += "#include <cstdlib>\n"; // getenv, abort
    result_cpp += "#include <cstring>\n"; // strcmp
    result_cpp += "#include <cstdio>\n\n"; // fflush
    result_cpp +=
        "typedef void (*recomp_func_t)(uint8_t*, recomp_context*);\n\n";

    result_cpp += "static recomp_func_t recomp_lookup_overlay(uint8_t* rdram, uint32_t addr, bool resume = false) {\n    const uint32_t normalized = (addr & 0x1fffffffu) | 0x80000000u;\n    if (resume) {\n";
    result_cpp += overlayResumeBody;
    result_cpp += "    } else {\n" + overlayLookupBody + "    }\n    return nullptr;\n}\n\n";

    // Global dispatch table (unordered_map)
    result_cpp += "static std::unordered_map<uint32_t, recomp_func_t> recomp_func_table;\n";
    result_cpp += "static std::unordered_map<uint32_t, recomp_func_t> recomp_resume_table;\n";
    result_cpp += "static std::unordered_map<uint32_t, recomp_func_t> recomp_entry_alias_table;\n";
    result_cpp += "static bool recomp_table_ready = false;\n\n";

    // Init function: populate the map once
    result_cpp += "void recomp_init_dispatch_table() {\n";
    result_cpp += fmt::format("    recomp_func_table.reserve({});\n",
                              addr_to_name.size() + 16);
    result_cpp += fmt::format("    recomp_resume_table.reserve({});\n",
                              resume_to_name.size() + 16);
    result_cpp += fmt::format("    recomp_entry_alias_table.reserve({});\n",
                              entry_alias_to_name.size() + 16);
    for (const auto &[addr, name] : addr_to_name) {
      result_cpp += fmt::format("    recomp_func_table[0x{:08X}] = {};\n", addr, name);
    }
    for (const auto &[addr, name] : resume_to_name) {
      result_cpp += fmt::format("    recomp_resume_table[0x{:08X}] = {};\n",
                                addr, name);
    }
    for (const auto &[addr, name] : entry_alias_to_name) {
      result_cpp += fmt::format(
          "    recomp_entry_alias_table[0x{:08X}] = {};\n", addr, name);
    }
    result_cpp += "    recomp_table_ready = true;\n";
    result_cpp += "}\n\n";

    // Lookup function
    result_cpp += "recomp_func_t recomp_lookup(uint32_t addr) {\n";
    result_cpp += "    auto it = recomp_func_table.find(addr);\n";
    result_cpp += "    return (it != recomp_func_table.end()) ? it->second : nullptr;\n";
    result_cpp += "}\n\n";

    result_cpp +=
        "recomp_func_t recomp_lookup_entry_alias(uint32_t addr) {\n"
        "    auto it = recomp_entry_alias_table.find(addr);\n"
        "    return (it != recomp_entry_alias_table.end()) ? it->second : "
        "nullptr;\n"
        "}\n\n";

    result_cpp +=
        "void recomp_resume_dispatch(uint8_t* rdram, recomp_context* ctx, "
        "uint32_t addr) {\n"
        "    if (!recomp_table_ready) recomp_init_dispatch_table();\n"
        "    const uint32_t normalized = (addr & 0x1FFFFFFFu) | "
        "0x80000000u;\n"
        "    if (auto fn = recomp_lookup_overlay(rdram, normalized, true)) {\n"
        "        ctx->nativeResumePc = normalized; fn(rdram, ctx); return;\n"
        "    }\n"
        "    auto it = recomp_resume_table.find(normalized);\n"
        "    if (it == recomp_resume_table.end()) {\n"
        "        fmt::print(stderr, \"[SaveState] FATAL: no native "
        "continuation for 0x{:08X}\\n\", normalized);\n"
        "        std::abort();\n"
        "    }\n"
        "    ctx->nativeResumePc = normalized;\n"
        "    it->second(rdram, ctx);\n"
        "}\n\n";

    // Main dispatch function -- body lives in dispatch_emitter.cpp so the
    // unmapped-target policy it encodes can be pinned by tests.
    result_cpp += ps1recomp::emitDispatchBody();

    const std::filesystem::path generatedOutput(output_path);
    if (translationUnits == 1) {
      writeGeneratedFile(generatedOutput, result_cpp);
      removeGeneratedShards(generatedOutput);
      std::error_code error;
      std::filesystem::remove(
          generatedOutput.parent_path() /
              (generatedOutput.stem().string() + ".generated.h"),
          error);
    } else {
      writeShardedTranslation(generatedOutput, result_cpp, translationUnits);
    }
    fmt::print("Success! Emitted Recompiled source to: {}\n", output_path);
    fmt::print("  Dispatch table: {} entries, {} savestate continuations\n",
               addr_to_name.size(), resume_to_name.size());

  } catch (const std::exception &e) {
    fmt::print(stderr, "Error parsing config: {}\n", e.what());
    return 1;
  }

  return 0;
}
