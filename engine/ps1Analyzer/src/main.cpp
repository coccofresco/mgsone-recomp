// ps1Analyzer -- PS1 Binary Analyzer
// Accepts: ELF, PS-X EXE, or BIN/CUE disc images
// Extracts functions, symbols, and relocations
// Generates config.toml for the recompiler

#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <ps1recomp/config_generator.h>
#include <ps1recomp/disc_reader.h>
#include <ps1recomp/elf_parser.h>
#include <ps1recomp/function_finder.h>
#include <ps1recomp/overlay_scanner.h>
#include <ps1recomp/psyq_signatures.h>
#include <string>
#include <vector>

static bool endsWith(const std::string &str, const std::string &suffix) {
  if (suffix.size() > str.size())
    return false;
  return std::equal(
      suffix.rbegin(), suffix.rend(), str.rbegin(),
      [](char a, char b) { return std::tolower(a) == std::tolower(b); });
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fmt::print("Usage: ps1Analyzer <ps1_elf_or_bin> [output_config.toml] "
               "[--add-func <addr>...]\n");
    fmt::print("       ps1Analyzer --scan-overlays <disc.bin> "
               "[output_overlays.toml]\n");
    fmt::print("  Accepts: .elf, .exe (PS-X EXE), .bin (BIN/CUE disc image)\n");
    return 1;
  }

  // Overlay scanning mode
  if (std::string(argv[1]) == "--scan-overlays") {
    if (argc < 3) {
      fmt::print(
          stderr,
          "Usage: ps1Analyzer --scan-overlays <disc.bin> [output.toml]\n");
      return 1;
    }

    std::string binPath = argv[2];
    const char *overlayOut = (argc >= 4) ? argv[3] : nullptr;

    ps1recomp::DiscReader disc;
    if (!disc.open(binPath)) {
      fmt::print(stderr, "Error opening disc: {}\n", disc.getError());
      return 1;
    }
    if (!disc.parseFilesystem()) {
      fmt::print(stderr, "Error parsing filesystem: {}\n", disc.getError());
      return 1;
    }

    fmt::print("Disc loaded: {} files found\n", disc.getFiles().size());

    ps1recomp::OverlayScanOptions opts;
    opts.bootExeName = disc.getBootFilename();
    opts.scanAllFiles = false;

    ps1recomp::OverlayScanner scanner;
    auto candidates = scanner.scanDisc(disc, opts);

    // Print summary
    fmt::print("\n=== Overlay Scan Results ===\n");
    for (const auto &c : candidates) {
      fmt::print("  {} -> RAM 0x{:08X}, {} bytes, {} funcs, MIPS {:.0f}%{}\n",
                 c.name, c.ramBase, c.codeSize, c.functions.size(),
                 c.mipsScore * 100.0f, c.hasPsxExeHeader ? " [PS-X EXE]" : "");
    }

    if (overlayOut) {
      auto toml = ps1recomp::OverlayScanner::exportToml(candidates);
      FILE *fp = fopen(overlayOut, "w");
      if (fp) {
        fwrite(toml.c_str(), 1, toml.size(), fp);
        fclose(fp);
        fmt::print("\nOverlay config written to: {}\n", overlayOut);
      } else {
        fmt::print(stderr, "Error: could not write to {}\n", overlayOut);
        return 1;
      }
    } else {
      fmt::print("\n{}", ps1recomp::OverlayScanner::exportToml(candidates));
    }

    return 0;
  }

  std::string input_path = argv[1];
  const char *output_path = nullptr;
  std::vector<uint32_t> extraFuncs;

  for (int i = 2; i < argc; ++i) {
    if (std::string(argv[i]) == "--add-func" && i + 1 < argc) {
      extraFuncs.push_back(std::stoul(argv[++i], nullptr, 16));
    } else if (!output_path) {
      output_path = argv[i];
    }
  }

  // Auto-detect BIN disc images
  std::vector<uint8_t>
      extractedExe; // keeps data alive when extracted from disc

  if (endsWith(input_path, ".bin")) {
    fmt::print("Detected BIN disc image: {}\n", input_path);

    ps1recomp::DiscReader disc;
    if (!disc.open(input_path)) {
      fmt::print(stderr, "Error: {}\n", disc.getError());
      return 1;
    }

    if (!disc.parseFilesystem()) {
      fmt::print(stderr, "Error: {}\n", disc.getError());
      return 1;
    }

    fmt::print("ISO9660 filesystem parsed. Files found: {}\n",
               disc.getFiles().size());
    for (const auto &f : disc.getFiles()) {
      fmt::print("  {} {:>8} bytes  LBA={:6}  {}\n",
                 f.isDirectory ? "[DIR]" : "[FILE]", f.size, f.lba, f.path);
    }

    if (disc.getBootFilename().empty()) {
      fmt::print(
          stderr,
          "\nError: Could not determine boot executable from SYSTEM.CNF\n");
      return 1;
    }

    fmt::print("\nBoot executable: {}\n", disc.getBootFilename());

    const auto *bootFile = disc.findFile(disc.getBootFilename());
    if (!bootFile) {
      fmt::print(stderr, "Error: Boot file '{}' not found on disc\n",
                 disc.getBootFilename());
      return 1;
    }

    fmt::print("Extracting {} ({} bytes from LBA {})...\n", bootFile->name,
               bootFile->size, bootFile->lba);
    extractedExe = disc.readFile(*bootFile);

    if (extractedExe.empty()) {
      fmt::print(stderr, "Error: Failed to read boot executable from disc\n");
      return 1;
    }

    // Write extracted EXE to a temp file for the parser
    std::string tmpPath = input_path + ".boot.exe";
    FILE *tmp = fopen(tmpPath.c_str(), "wb");
    if (!tmp) {
      fmt::print(stderr, "Error: Could not create temp file: {}\n", tmpPath);
      return 1;
    }
    fwrite(extractedExe.data(), 1, extractedExe.size(), tmp);
    fclose(tmp);

    input_path = tmpPath;
    fmt::print("Extracted to: {}\n\n", input_path);
  }

  // Load ELF or PS-EXE

  ps1recomp::ElfParser parser;
  if (!parser.load(input_path)) {
    fmt::print(stderr, "Error: {}\n", parser.getError());
    return 1;
  }

  fmt::print("PS1 binary loaded: {}\n", input_path);
  fmt::print("  Entry point: 0x{:08X}\n", parser.getEntryPoint());
  fmt::print("  Sections:    {}\n", parser.getSections().size());
  fmt::print("  Symbols:     {}\n", parser.getSymbols().size());
  fmt::print("  Code size:   {} bytes\n", parser.getTotalCodeSize());
  fmt::print("  Data size:   {} bytes\n", parser.getTotalDataSize());

  fmt::print("\nSections:\n");
  for (const auto &section : parser.getSections()) {
    const char *type_str;
    switch (section.type) {
    case ps1recomp::SectionType::Text:
      type_str = "TEXT";
      break;
    case ps1recomp::SectionType::Data:
      type_str = "DATA";
      break;
    case ps1recomp::SectionType::Bss:
      type_str = "BSS ";
      break;
    default:
      type_str = "    ";
      break;
    }
    fmt::print("  {:16s}  addr=0x{:08X}  size={:8d}  [{}]\n", section.name,
               section.vaddr, section.size, type_str);
  }

  // 1. Find functions
  ps1recomp::FunctionFinder finder;
  finder.findFunctions(parser);

  for (uint32_t addr : extraFuncs) {
    std::string name = fmt::format("func_added_{:08X}", addr);
    finder.addFunction(addr, name, ps1recomp::FunctionSource::Symbol);
  }
  if (!extraFuncs.empty()) {
    finder.recomputeBoundaries(parser);
  }

  // 2. Classify against PsyQ
  ps1recomp::PsyQMatcher matcher;
  matcher.matchFunctions(parser, finder);

  fmt::print("\nFunction stats:\n");
  fmt::print("  Total functions:   {}\n", finder.getFunctionCount());
  fmt::print("  PsyQ matches:      {}\n", matcher.getMatchCount());
  fmt::print("  Loaded signatures: {}\n", matcher.getSignatureCount());

  const auto lc = matcher.getLibraryCounts();
  fmt::print(stderr,
             "[PSYQ] Detected {} PsyQ functions in {} "
             "(libgpu={}, libetc={}, libapi={}, libcd={}, libgte={}, libgs={}, other={})\n",
             matcher.getMatchCount(), input_path,
             lc.libgpu, lc.libetc, lc.libapi, lc.libcd, lc.libgte, lc.libgs, lc.other);

  // 3. Generate config
  if (output_path) {
    ps1recomp::ConfigGenerator generator;
    if (generator.generate(parser, finder, matcher, input_path.c_str(),
                           output_path)) {
      fmt::print("\nSuccess! Generated config at: {}\n", output_path);
    } else {
      fmt::print(stderr, "\nFailed to generate config: {}\n",
                 generator.getError());
      return 1;
    }
  } else {
    fmt::print("\nNo output_config.toml specified. Skipping generation.\n");
  }

  return 0;
}
