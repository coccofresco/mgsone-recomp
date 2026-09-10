#pragma once

// ps1Analyzer -- Disc Reader
// Reads PS1 BIN/CUE disc images with ISO9660 filesystem support.
// Extracts files from the disc, including the boot executable and overlays.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps1recomp {

// Constants

constexpr uint32_t SECTOR_SIZE_RAW = 2352;  // Raw CD sector
constexpr uint32_t SECTOR_SIZE_DATA = 2048; // ISO9660 data payload
constexpr uint32_t SECTOR_HEADER_SIZE =
    24; // Mode 2 Form 1 header (sync + header + subheader)

// Disc File Entry

struct DiscFile {
  std::string name; // Filename (e.g. "SCUS_949.00;1")
  std::string path; // Full path (e.g. "/SCUS_949.00;1")
  uint32_t lba;     // Logical Block Address (sector number)
  uint32_t size;    // File size in bytes
  bool isDirectory;
};

// Disc Reader

class DiscReader {
public:
  DiscReader() = default;
  ~DiscReader() = default;

  /// Open a BIN file for reading.
  bool open(const std::string &binPath);

  /// Parse the ISO9660 filesystem and build file list.
  bool parseFilesystem();

  /// Get boot executable filename from SYSTEM.CNF.
  std::string getBootFilename() const { return m_bootFilename; }

  /// Get all discovered files.
  const std::vector<DiscFile> &getFiles() const { return m_files; }

  /// Find a file by name (case-insensitive, strips version suffix).
  const DiscFile *findFile(const std::string &name) const;

  /// Read a file's contents from the disc.
  /// Returns raw file data (sector headers stripped).
  std::vector<uint8_t> readFile(const DiscFile &file) const;

  /// Read raw sectors starting at a given LBA.
  std::vector<uint8_t> readSectors(uint32_t lba, uint32_t count) const;

  /// Read a single sector's data payload (2048 bytes).
  bool readSectorData(uint32_t lba, uint8_t *out) const;

  /// Get error message.
  const std::string &getError() const { return m_error; }

private:
  std::string m_binPath;
  std::string m_error;
  std::string m_bootFilename;
  std::vector<DiscFile> m_files;

  // Cached file handle (mutable for const read methods)
  mutable FILE *m_fp = nullptr;

  bool parseDirectory(uint32_t lba, uint32_t size,
                      const std::string &parentPath);
  bool parseSystemCnf();

  static std::string cleanFilename(const std::string &raw);
  static std::string toUpper(const std::string &s);
};

} // namespace ps1recomp
