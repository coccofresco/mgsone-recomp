#pragma once

#include "runtime/cdrom/bin_reader.h"
#include "runtime/cdrom/cue_parser.h"
#include "runtime/cdrom/exe_extractor.h"
#include "runtime/cdrom/iso9660.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ps1::cdrom {

class VirtualFs {
public:
  VirtualFs();
  virtual ~VirtualFs() = default;

  // Load a disc image (currently supports .cue or .bin directly)
  virtual bool loadDisc(const std::filesystem::path &imagePath);

  // Read a specific sector (raw or data)
  virtual std::optional<Sector> readSector(uint32_t lba);

  // Abstract file reading from the mounted disc
  virtual std::optional<std::vector<uint8_t>>
  readFile(const std::string &filepath);

  // Get the boot executable path from SYSTEM.CNF
  virtual std::optional<std::string> getBootPath();

  // Multi-disc support
  bool swapDisc(const std::filesystem::path &newImagePath);
  uint32_t getDiscCount() const { return discPaths_.size(); }
  uint32_t getCurrentDisc() const { return currentDiscIndex_; }

private:
  std::unique_ptr<BinReader> binReader_;
  std::unique_ptr<Iso9660Parser> isoParser_;
  std::unique_ptr<ExeExtractor> exeExtractor_;
  std::vector<Track> tracks_;
  std::vector<std::filesystem::path> discPaths_;
  uint32_t currentDiscIndex_ = 0;

  bool loadCue(const std::filesystem::path &cuePath);
};

} // namespace ps1::cdrom
