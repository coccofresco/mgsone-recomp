// ps1Analyzer -- Disc Reader Implementation
// Reads PS1 BIN/CUE disc images (Mode 2 / 2352-byte sectors)
// Parses ISO9660 filesystem to extract files

#include "ps1recomp/disc_reader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>

namespace ps1recomp {

// Open

bool DiscReader::open(const std::string &binPath) {
  m_binPath = binPath;
  m_files.clear();
  m_bootFilename.clear();
  m_error.clear();

  m_fp = fopen(binPath.c_str(), "rb");
  if (!m_fp) {
    m_error = fmt::format("Could not open BIN file: {}", binPath);
    return false;
  }

  return true;
}

// Read Sector Data

bool DiscReader::readSectorData(uint32_t lba, uint8_t *out) const {
  if (!m_fp)
    return false;

  // Each raw sector is 2352 bytes. Data payload starts at offset 24
  // (12 sync + 4 header + 8 subheader for Mode 2 Form 1)
  long fileOffset =
      static_cast<long>(lba) * SECTOR_SIZE_RAW + SECTOR_HEADER_SIZE;
  if (fseek(m_fp, fileOffset, SEEK_SET) != 0) {
    return false;
  }

  return fread(out, 1, SECTOR_SIZE_DATA, m_fp) == SECTOR_SIZE_DATA;
}

// Read Sectors

std::vector<uint8_t> DiscReader::readSectors(uint32_t lba,
                                             uint32_t count) const {
  std::vector<uint8_t> data(count * SECTOR_SIZE_DATA);
  for (uint32_t i = 0; i < count; i++) {
    if (!readSectorData(lba + i, data.data() + i * SECTOR_SIZE_DATA)) {
      data.resize(i * SECTOR_SIZE_DATA);
      break;
    }
  }
  return data;
}

// Read File

std::vector<uint8_t> DiscReader::readFile(const DiscFile &file) const {
  uint32_t sectorsNeeded =
      (file.size + SECTOR_SIZE_DATA - 1) / SECTOR_SIZE_DATA;
  auto data = readSectors(file.lba, sectorsNeeded);
  // Trim to actual file size
  if (data.size() > file.size) {
    data.resize(file.size);
  }
  return data;
}

// Parse Filesystem

bool DiscReader::parseFilesystem() {
  if (!m_fp) {
    m_error = "No BIN file opened";
    return false;
  }

  // ISO9660 Primary Volume Descriptor is at sector 16
  uint8_t pvd[SECTOR_SIZE_DATA];
  if (!readSectorData(16, pvd)) {
    m_error = "Failed to read Primary Volume Descriptor (sector 16)";
    return false;
  }

  // Check PVD signature: byte 0 = 0x01 (type), bytes 1-5 = "CD001"
  if (pvd[0] != 0x01 || std::memcmp(pvd + 1, "CD001", 5) != 0) {
    m_error =
        "Invalid ISO9660 Primary Volume Descriptor (missing CD001 signature)";
    return false;
  }

  // Root directory record is at offset 156 in the PVD (34 bytes)
  uint8_t *rootRecord = pvd + 156;
  uint32_t rootLBA = static_cast<uint32_t>(rootRecord[2]) |
                     (static_cast<uint32_t>(rootRecord[3]) << 8) |
                     (static_cast<uint32_t>(rootRecord[4]) << 16) |
                     (static_cast<uint32_t>(rootRecord[5]) << 24);
  uint32_t rootSize = static_cast<uint32_t>(rootRecord[10]) |
                      (static_cast<uint32_t>(rootRecord[11]) << 8) |
                      (static_cast<uint32_t>(rootRecord[12]) << 16) |
                      (static_cast<uint32_t>(rootRecord[13]) << 24);

  // Parse root directory
  if (!parseDirectory(rootLBA, rootSize, "/")) {
    return false;
  }

  // Parse SYSTEM.CNF to find boot executable
  parseSystemCnf();

  return true;
}

// Parse Directory

bool DiscReader::parseDirectory(uint32_t lba, uint32_t size,
                                const std::string &parentPath) {
  uint32_t sectorsNeeded = (size + SECTOR_SIZE_DATA - 1) / SECTOR_SIZE_DATA;
  auto dirData = readSectors(lba, sectorsNeeded);
  if (dirData.empty()) {
    m_error = fmt::format("Failed to read directory at LBA {}", lba);
    return false;
  }

  uint32_t offset = 0;
  while (offset < size) {
    uint8_t recordLen = dirData[offset];
    if (recordLen == 0) {
      // Padding at end of sector -- skip to next sector boundary
      uint32_t nextSector =
          ((offset / SECTOR_SIZE_DATA) + 1) * SECTOR_SIZE_DATA;
      if (nextSector >= size)
        break;
      offset = nextSector;
      continue;
    }

    if (offset + recordLen > dirData.size())
      break;

    uint8_t *rec = dirData.data() + offset;

    // Extract LBA (little-endian at offset 2)
    uint32_t fileLBA = static_cast<uint32_t>(rec[2]) |
                       (static_cast<uint32_t>(rec[3]) << 8) |
                       (static_cast<uint32_t>(rec[4]) << 16) |
                       (static_cast<uint32_t>(rec[5]) << 24);

    // Extract size (little-endian at offset 10)
    uint32_t fileSize = static_cast<uint32_t>(rec[10]) |
                        (static_cast<uint32_t>(rec[11]) << 8) |
                        (static_cast<uint32_t>(rec[12]) << 16) |
                        (static_cast<uint32_t>(rec[13]) << 24);

    // Flags: bit 1 = directory
    uint8_t flags = rec[25];
    bool isDir = (flags & 0x02) != 0;

    // Filename length and data
    uint8_t nameLen = rec[32];
    std::string rawName(reinterpret_cast<char *>(rec + 33), nameLen);

    // Skip "." (0x00) and ".." (0x01) entries
    if (nameLen == 1 && (rec[33] == 0x00 || rec[33] == 0x01)) {
      offset += recordLen;
      continue;
    }

    std::string cleanName = cleanFilename(rawName);
    std::string fullPath = parentPath + cleanName;

    DiscFile entry;
    entry.name = cleanName;
    entry.path = fullPath;
    entry.lba = fileLBA;
    entry.size = fileSize;
    entry.isDirectory = isDir;
    m_files.push_back(entry);

    // Recurse into subdirectories
    if (isDir) {
      parseDirectory(fileLBA, fileSize, fullPath + "/");
    }

    offset += recordLen;
  }

  return true;
}

// Parse SYSTEM.CNF

bool DiscReader::parseSystemCnf() {
  const DiscFile *cnf = findFile("SYSTEM.CNF");
  if (!cnf) {
    // Some games use PSX.EXE directly without SYSTEM.CNF
    const DiscFile *psxExe = findFile("PSX.EXE");
    if (psxExe) {
      m_bootFilename = "PSX.EXE";
      return true;
    }
    return false;
  }

  auto data = readFile(*cnf);
  std::string content(data.begin(), data.end());

  // Look for "BOOT = cdrom:\..." or "BOOT=cdrom:\"
  // Format: BOOT = cdrom:\SCUS_949.00;1
  auto pos = content.find("BOOT");
  if (pos == std::string::npos)
    return false;

  pos = content.find('\\', pos);
  if (pos == std::string::npos) {
    pos = content.find(':', pos);
    if (pos == std::string::npos)
      return false;
  }
  pos++; // skip backslash or colon

  // Extract filename until semicolon, newline, or end
  std::string filename;
  while (pos < content.size()) {
    char c = content[pos];
    if (c == '\r' || c == '\n' || c == '\0')
      break;
    filename += c;
    pos++;
  }

  // Strip version suffix (";1") if present for the stored name
  m_bootFilename = cleanFilename(filename);
  return !m_bootFilename.empty();
}

// Find File

const DiscFile *DiscReader::findFile(const std::string &name) const {
  std::string upperName = toUpper(cleanFilename(name));
  for (const auto &f : m_files) {
    if (toUpper(f.name) == upperName) {
      return &f;
    }
  }
  return nullptr;
}

// Helpers

std::string DiscReader::cleanFilename(const std::string &raw) {
  std::string result = raw;
  // Remove ISO9660 version suffix (";1", ";2", etc.)
  auto semi = result.find(';');
  if (semi != std::string::npos) {
    result = result.substr(0, semi);
  }
  // Trim trailing spaces
  while (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }
  return result;
}

std::string DiscReader::toUpper(const std::string &s) {
  std::string result = s;
  for (auto &c : result) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

} // namespace ps1recomp
