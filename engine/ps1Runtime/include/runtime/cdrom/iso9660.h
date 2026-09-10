#pragma once

#include "runtime/cdrom/bin_reader.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ps1::cdrom {

struct Iso9660DirEntry {
  uint32_t extent_lba;
  uint32_t data_length;
  bool is_directory;
  std::string name;
};

class Iso9660Parser {
public:
  Iso9660Parser(BinReader &reader);

  // Parses the volume descriptor to find the root directory
  bool initialize();

  // Gets the directory entries for a specific directory, optionally parsing by
  // LBA
  std::vector<Iso9660DirEntry> readDirectory(uint32_t dir_lba, uint32_t length);

  // Look up a file by its exact name, starting from the root
  std::optional<Iso9660DirEntry> findFile(const std::string &filepath);

  // Get root directory's starting sector (LBA)
  uint32_t getRootLba() const { return root_lba_; }
  uint32_t getRootLength() const { return root_length_; }

private:
  BinReader &reader_;
  uint32_t root_lba_ = 0;
  uint32_t root_length_ = 0;

  std::vector<Iso9660DirEntry>
  parseDirectorySector(std::span<const uint8_t> data);
};

} // namespace ps1::cdrom
