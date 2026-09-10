#include "runtime/cdrom/exe_extractor.h"
#include <iostream>
#include <regex>
#include <sstream>

namespace ps1::cdrom {

ExeExtractor::ExeExtractor(BinReader &reader, Iso9660Parser &isoParser)
    : reader_(reader), isoParser_(isoParser) {}

std::optional<std::string> ExeExtractor::getBootExecutablePath() {
  auto sysCnfEntry = isoParser_.findFile("SYSTEM.CNF");
  if (!sysCnfEntry)
    return std::nullopt;

  uint32_t sectors_to_read =
      (sysCnfEntry->data_length + SECTOR_SIZE_DATA - 1) / SECTOR_SIZE_DATA;
  std::string sysCnfContent;
  sysCnfContent.reserve(sectors_to_read * SECTOR_SIZE_DATA);

  for (uint32_t i = 0; i < sectors_to_read; ++i) {
    auto sector = reader_.readSector(sysCnfEntry->extent_lba + i);
    if (!sector)
      return std::nullopt;

    auto data = sector->getDataMode2Form1();
    sysCnfContent.append(reinterpret_cast<const char *>(data.data()),
                         data.size());
  }

  // Shrink down to actual file length
  sysCnfContent.resize(sysCnfEntry->data_length);

  std::istringstream stream(sysCnfContent);
  std::string line;
  // We look for BOOT = cdrom:\SLUS_xxx.xx;1
  std::regex bootRegex(R"(BOOT\s*=\s*cdrom:\\(.+))", std::regex::icase);
  std::smatch match;

  while (std::getline(stream, line)) {
    if (std::regex_search(line, match, bootRegex)) {
      std::string path = match[1];
      // Clean carriage returns or semicolons
      size_t semi = path.find(';');
      if (semi != std::string::npos)
        path = path.substr(0, semi);
      size_t cr = path.find('\r');
      if (cr != std::string::npos)
        path = path.substr(0, cr);
      return path;
    }
  }

  return std::nullopt;
}

std::optional<std::vector<uint8_t>> ExeExtractor::extractExecutable() {
  auto pathOpt = getBootExecutablePath();
  if (!pathOpt)
    return std::nullopt;

  // Convert DOS-style slashes to string stream splitting
  std::string path = *pathOpt;
  for (char &c : path) {
    if (c == '\\')
      c = '/';
  }

  auto exeEntry = isoParser_.findFile(path);
  if (!exeEntry)
    return std::nullopt;

  std::vector<uint8_t> exeData;
  exeData.reserve(exeEntry->data_length);

  uint32_t sectors_to_read =
      (exeEntry->data_length + SECTOR_SIZE_DATA - 1) / SECTOR_SIZE_DATA;

  for (uint32_t i = 0; i < sectors_to_read; ++i) {
    auto sector = reader_.readSector(exeEntry->extent_lba + i);
    if (!sector)
      return std::nullopt;

    auto data = sector->getDataMode2Form1();
    exeData.insert(exeData.end(), data.begin(), data.end());
  }

  exeData.resize(exeEntry->data_length);
  return exeData;
}

} // namespace ps1::cdrom
