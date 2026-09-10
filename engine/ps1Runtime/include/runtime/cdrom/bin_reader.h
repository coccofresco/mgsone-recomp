#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace ps1::cdrom {

// A standard CD-ROM RAW sector is 2352 bytes.
constexpr size_t SECTOR_SIZE_RAW = 2352;
// Common data payload size for Mode 2 Form 1 or Mode 1
constexpr size_t SECTOR_SIZE_DATA = 2048;

struct Sector {
  uint8_t raw[SECTOR_SIZE_RAW];

  // Helper to get the 2048 bytes of user data
  // Assuming Mode 2 Form 1 for standard PS1 data tracks
  std::span<const uint8_t> getDataMode2Form1() const {
    return {raw + 24, SECTOR_SIZE_DATA};
  }
};

class BinReader {
public:
  BinReader() = default;
  ~BinReader() = default;

  // Opens a .bin file for reading. Returns true on success.
  bool open(const std::filesystem::path &binPath);

  // Reads a specific sector (0-indexed). Returns false if out of bounds.
  std::optional<Sector> readSector(uint32_t sectorAddress);

  // Returns the total number of sectors in the file
  uint32_t getTotalSectors() const;

private:
  // CdromController can be advanced by the host/render thread while PsyQ CD
  // calls read sectors on the game thread. std::ifstream has a shared cursor,
  // so every operation on it must be serialized.
  mutable std::mutex fileMutex_;
  std::ifstream file_;
  uint32_t totalSectors_ = 0;
};

} // namespace ps1::cdrom
