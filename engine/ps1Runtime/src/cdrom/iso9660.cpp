#include "runtime/cdrom/iso9660.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>

namespace ps1::cdrom {

namespace {
uint32_t readLe32(const uint8_t *bytes) {
  return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
         (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}
}

Iso9660Parser::Iso9660Parser(BinReader &reader) : reader_(reader) {}

bool Iso9660Parser::initialize() {
  root_lba_ = root_length_ = 0;
  // Primary Volume Descriptor represents sector 16
  for (uint32_t sector = 16; sector < 32; ++sector) {
    auto sectorOpt = reader_.readSector(sector);
    if (!sectorOpt)
      break;

    auto data = sectorOpt->getDataMode2Form1();
    uint8_t type = data[0];

    // "CD001" identifier
    if (std::memcmp(&data[1], "CD001", 5) != 0) {
      continue;
    }

    // Primary Volume Descriptor
    if (type == 1) {
      // Root directory record is at offset 156, length is 34 bytes
      const uint8_t *root_record = &data[156];
      root_lba_ = readLe32(root_record + 2);
      root_length_ = readLe32(root_record + 10);
      return true;
    }
  }

  return false;
}

std::vector<Iso9660DirEntry> Iso9660Parser::readDirectory(uint32_t dir_lba,
                                                          uint32_t length) {
  std::vector<Iso9660DirEntry> entries;
  const uint64_t sectors_to_read =
      (uint64_t(length) + SECTOR_SIZE_DATA - 1) / SECTOR_SIZE_DATA;
  const uint32_t total = reader_.getTotalSectors();
  if (dir_lba > total || sectors_to_read > total - dir_lba) return entries;

  for (uint32_t i = 0; i < sectors_to_read; ++i) {
    auto sectorOpt = reader_.readSector(dir_lba + i);
    if (!sectorOpt)
      break;

    auto data = sectorOpt->getDataMode2Form1();
    const auto remaining = uint64_t(length) - uint64_t(i) * SECTOR_SIZE_DATA;
    auto sector_entries = parseDirectorySector(data.first(std::min<uint64_t>(data.size(), remaining)));
    entries.insert(entries.end(), sector_entries.begin(), sector_entries.end());
  }

  return entries;
}

std::vector<Iso9660DirEntry>
Iso9660Parser::parseDirectorySector(std::span<const uint8_t> data) {
  std::vector<Iso9660DirEntry> entries;
  size_t offset = 0;

  while (offset < data.size()) {
    uint8_t record_len = data[offset];
    if (record_len == 0)
      break; // Reached end of directory records

    if (record_len < 34 || record_len > data.size() - offset)
      break; // Corrupt record

    const uint8_t name_len = data[offset + 32];
    if (name_len == 0 || name_len > record_len - 33)
      break; // Name must be contained by its record, not just the sector.

    Iso9660DirEntry entry;
    entry.extent_lba = readLe32(&data[offset + 2]);
    entry.data_length = readLe32(&data[offset + 10]);
    entry.is_directory = (data[offset + 25] & 0x02) != 0;

    const char *name_ptr = reinterpret_cast<const char *>(&data[offset + 33]);
    entry.name = std::string(name_ptr, name_len);

    // Remove the form ";1" (file version) at the end of filenames
    size_t semi_idx = entry.name.find(';');
    if (semi_idx != std::string::npos) {
      entry.name = entry.name.substr(0, semi_idx);
    }

    // Directories usually appear as "." or ".." with specific 1-byte IDs
    if (name_len == 1 && name_ptr[0] == 0x00)
      entry.name = ".";
    if (name_len == 1 && name_ptr[0] == 0x01)
      entry.name = "..";

    entries.push_back(std::move(entry));
    offset += record_len;
  }

  return entries;
}

std::optional<Iso9660DirEntry>
Iso9660Parser::findFile(const std::string &filepath) {
  if (root_lba_ == 0)
    return std::nullopt;

  uint32_t current_lba = root_lba_;
  uint32_t current_len = root_length_;

  std::stringstream ss(filepath);
  std::string token;

  // Naively splitting by '\' or '/' and finding iteratively
  Iso9660DirEntry last_found;
  bool found_any = false;

  while (std::getline(ss, token, '\\')) {
    std::stringstream ss2(token);
    std::string subtoken;
    while (std::getline(ss2, subtoken, '/')) {
      if (subtoken.empty())
        continue;

      auto entries = readDirectory(current_lba, current_len);
      bool found_token = false;

      for (const auto &entry : entries) {
        if (entry.name == subtoken) {
          last_found = entry;
          current_lba = entry.extent_lba;
          current_len = entry.data_length;
          found_token = true;
          found_any = true;
          break;
        }
      }

      if (!found_token)
        return std::nullopt; // Part of path not found
    }
  }

  if (found_any)
    return last_found;
  return std::nullopt;
}

} // namespace ps1::cdrom
