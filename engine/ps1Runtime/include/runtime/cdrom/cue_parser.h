#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ps1::cdrom {

enum class TrackType { Audio, Data };

struct Track {
  uint8_t number;
  TrackType type;
  std::string file;
  uint32_t msf_start; // Minutes, Seconds, Frames combined into sectors (M*60*75
                      // + S*75 + F)
};

class CueParser {
public:
  // Parses a .CUE file and returns a list of tracks or std::nullopt on failure.
  static std::optional<std::vector<Track>>
  parse(const std::filesystem::path &cuePath);

private:
  static uint32_t parseMSF(const std::string &msfStr);
};

} // namespace ps1::cdrom
