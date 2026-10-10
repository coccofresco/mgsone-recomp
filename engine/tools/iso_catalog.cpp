// Read-only native ISO directory/lookup audit. Built by scripts/mgs_iso_audit.py.
#include "runtime/cdrom/iso9660.h"
#include <iostream>
#include <set>
#include <stdexcept>

int main(int argc, char **argv) {
  try {
    if (argc != 2) throw std::runtime_error("expected raw Mode2 disc path");
    ps1::cdrom::BinReader reader;
    if (!reader.open(argv[1])) throw std::runtime_error("cannot open disc");
    ps1::cdrom::Iso9660Parser parser(reader);
    if (!parser.initialize()) throw std::runtime_error("missing primary descriptor");
    struct Directory { uint32_t lba, size; std::string path; };
    std::vector<Directory> pending{{parser.getRootLba(), parser.getRootLength(), ""}};
    std::set<uint32_t> visited;
    std::set<std::string> paths;
    for (size_t i = 0; i < pending.size(); ++i) {
      const auto directory = pending[i];
      if (!visited.insert(directory.lba).second) throw std::runtime_error("directory cycle");
      const auto entries = parser.readDirectory(directory.lba, directory.size);
      if (entries.empty()) throw std::runtime_error("unreadable directory");
      for (const auto &entry : entries) {
        if (entry.name == "." || entry.name == "..") continue;
        if (entry.name.empty() || entry.name.find_first_of("\t\n\r/\\") != std::string::npos)
          throw std::runtime_error("invalid name");
        const auto path = directory.path + '/' + entry.name;
        if (!paths.insert(path).second) throw std::runtime_error("duplicate path");
        const auto found = parser.findFile(path);
        if (!found || found->extent_lba != entry.extent_lba ||
            found->data_length != entry.data_length || found->is_directory != entry.is_directory)
          throw std::runtime_error("path lookup differs from directory walk");
        const uint64_t sectors = (uint64_t(entry.data_length) + 2047) / 2048;
        if (uint64_t(entry.extent_lba) + sectors > reader.getTotalSectors())
          throw std::runtime_error("extent outside disc");
        if (entry.is_directory)
          pending.push_back({entry.extent_lba, entry.data_length, path});
        else
          std::cout << path << '\t' << entry.extent_lba << '\t' << entry.data_length << '\n';
      }
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "ISO audit: " << error.what() << '\n';
    return 1;
  }
}
