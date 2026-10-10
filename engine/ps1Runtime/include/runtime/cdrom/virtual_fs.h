#pragma once

#include "runtime/cdrom/bin_reader.h"
#include "runtime/cdrom/cue_parser.h"
#include "runtime/cdrom/exe_extractor.h"
#include "runtime/cdrom/iso9660.h"

#include <filesystem>
#include <functional>
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

  // Host policy for ordinary asset archives. Resolve against the mounted ISO,
  // and rebuild after every media change; never infer data from LBA constants.
  void setUnpacedDataFiles(std::vector<std::string> paths);
  virtual bool allowsUnpacedData(uint32_t lba) const;
  void setUnpacedDataGuard(std::function<bool(std::string_view)> guard) {
    unpacedDataGuard_ = std::move(guard);
  }

  // Abstract file reading from the mounted disc
  virtual std::optional<std::vector<uint8_t>>
  readFile(const std::string &filepath);

  // Get the boot executable path from SYSTEM.CNF
  virtual std::optional<std::string> getBootPath();

  // Multi-disc support
  bool swapDisc(const std::filesystem::path &newImagePath);
  // Prepare and validate a disc without disturbing the mounted medium.
  bool registerDisc(const std::filesystem::path &imagePath,
                    const std::string &expectedBoot = {});
  bool mountIdentity(const std::string &identity);
  std::string identityForBoot(const std::string &boot) const;
  const std::string &mediaIdentity() const { return mediaIdentity_; }
  uint32_t getDiscCount() const { return discPaths_.size(); }
  uint32_t getCurrentDisc() const { return currentDiscIndex_; }

private:
  std::unique_ptr<BinReader> binReader_;
  std::unique_ptr<Iso9660Parser> isoParser_;
  std::unique_ptr<ExeExtractor> exeExtractor_;
  std::vector<Track> tracks_;
  std::vector<std::filesystem::path> discPaths_;
  std::vector<std::string> discIdentities_;
  std::vector<std::string> discBoots_;
  std::string mediaIdentity_;
  uint32_t currentDiscIndex_ = 0;

  bool loadCue(const std::filesystem::path &cuePath);
  bool openImage(const std::filesystem::path &imagePath);
  void adoptMedia(VirtualFs &prepared);
  bool prepareMedia(const std::filesystem::path &path);
  uint32_t rememberMedia(const std::filesystem::path &path, const VirtualFs &prepared);
  std::string bootName_;
  std::vector<std::string> unpacedDataFiles_;
  struct DataExtent { uint32_t first; uint64_t end; std::string path; };
  std::vector<DataExtent> unpacedDataExtents_;
  std::function<bool(std::string_view)> unpacedDataGuard_;
  void refreshUnpacedDataExtents();
};

} // namespace ps1::cdrom
