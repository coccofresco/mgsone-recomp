#pragma once

#include "runtime/cdrom/bin_reader.h"
#include "runtime/cdrom/iso9660.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ps1::cdrom {

class ExeExtractor {
public:
  ExeExtractor(BinReader &reader, Iso9660Parser &isoParser);

  // Reads SYSTEM.CNF and extracts the BOOT path (e.g. "cdrom:\SLUS_123.45;1")
  std::optional<std::string> getBootExecutablePath();

  // Locates the executable file via ISO9660 and reads its raw bytes
  std::optional<std::vector<uint8_t>> extractExecutable();

private:
  BinReader &reader_;
  Iso9660Parser &isoParser_;
};

} // namespace ps1::cdrom
