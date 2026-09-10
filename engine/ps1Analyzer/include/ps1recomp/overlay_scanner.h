#pragma once

// ps1Analyzer -- Overlay Scanner
// Scans PS1 disc images for dynamically-loaded code overlays.
// Uses MIPS instruction heuristics to detect executable code in disc files
// and infers RAM load addresses from PS-X EXE headers or LUI/ORI patterns.

#include "ps1recomp/disc_reader.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ps1recomp {

// Overlay Candidate

/// Result of scanning a single disc file for executable content
struct OverlayCandidate {
  std::string name;     // Disc file name (e.g. "S0000003.NSF")
  std::string discPath; // Full disc path (e.g. "/S0000003.NSF")
  uint32_t lba;         // LBA on disc
  uint32_t fileSize;    // Size in bytes

  // Detection results
  bool hasPsxExeHeader;     // File starts with PS-X EXE magic
  uint32_t ramBase;          // Detected RAM load address (0 if unknown)
  uint32_t codeOffset;       // Offset within file where code starts
  uint32_t codeSize;         // Size of detected code region

  // Heuristic scores (0.0 - 1.0)
  float mipsScore;           // Ratio of valid MIPS instructions
  float functionScore;       // Ratio of function prologues/epilogues detected

  // Detected function entry points (addresses)
  std::vector<uint32_t> functions;

  /// Is this likely an executable overlay?
  bool isLikelyCode() const {
    return hasPsxExeHeader || (mipsScore >= 0.5f && functionScore >= 0.1f);
  }
};

// Scanner Options

struct OverlayScanOptions {
  float minMipsScore = 0.5f;          // Minimum valid MIPS ratio
  float minFunctionScore = 0.05f;     // Minimum function detection ratio
  uint32_t minCodeSize = 256;          // Minimum bytes to consider
  uint32_t maxFileSize = 4 * 1024 * 1024; // Skip files larger than 4MB
  bool scanAllFiles = false;           // If false, skip known non-code extensions
  std::string bootExeName;             // Boot EXE to exclude from scan
};

// PS-X EXE Header

/// PS-X EXE file header (2048 bytes, first sector of executable)
struct PsxExeHeader {
  char magic[8];       // "PS-X EXE" (null-padded)
  uint32_t _pad1[2];   // Unused
  uint32_t pc0;        // Initial PC
  uint32_t gp0;        // Initial GP
  uint32_t tAddr;      // Text section destination in RAM
  uint32_t tSize;      // Text section size
  uint32_t _pad2[2];   // Data section (unused)
  uint32_t bssAddr;    // BSS address
  uint32_t bssSize;    // BSS size
  uint32_t spBase;     // Initial SP base
  uint32_t spOffset;   // SP offset
};

// Overlay Scanner

class OverlayScanner {
public:
  OverlayScanner() = default;

  /// Scan all files on a disc image for overlay candidates
  /// @param reader An opened DiscReader with parsed filesystem
  /// @param options Scanning options/thresholds
  /// @return Vector of detected overlay candidates
  std::vector<OverlayCandidate>
  scanDisc(const DiscReader &reader,
           const OverlayScanOptions &options = {}) const;

  /// Scan a single file's data for MIPS code
  /// @param data Raw file bytes
  /// @param filename Name for identification
  /// @param options Scanning options
  /// @return Candidate result (check isLikelyCode())
  OverlayCandidate scanFile(const std::vector<uint8_t> &data,
                            const std::string &filename,
                            const OverlayScanOptions &options = {}) const;

  /// Try to parse a PS-X EXE header from data
  /// @return Header if valid magic found
  static std::optional<PsxExeHeader>
  parsePsxExeHeader(const std::vector<uint8_t> &data);

  /// Compute MIPS instruction validity score for a data region
  /// @return Score from 0.0 (no valid MIPS) to 1.0 (all valid)
  static float computeMipsScore(const uint8_t *data, size_t size);

  /// Detect function prologues/epilogues in MIPS code
  /// @return Pair of (score, function_addresses)
  static std::pair<float, std::vector<uint32_t>>
  detectFunctions(const uint8_t *data, size_t size, uint32_t baseAddr);

  /// Infer RAM base address from LUI/ORI instruction patterns
  /// @return Most likely base address, or 0 if undetermined
  static uint32_t inferRamBase(const uint8_t *data, size_t size);

  /// Export overlay candidates as TOML config sections
  static std::string
  exportToml(const std::vector<OverlayCandidate> &candidates);

private:
  /// Check if a file extension suggests non-code content
  static bool isNonCodeExtension(const std::string &name);
};

} // namespace ps1recomp
