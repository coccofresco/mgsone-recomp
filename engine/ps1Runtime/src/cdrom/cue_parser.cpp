#include "runtime/cdrom/cue_parser.h"
#include <fstream>
#include <regex>
#include <sstream>

namespace ps1::cdrom {

std::optional<std::vector<Track>>
CueParser::parse(const std::filesystem::path &cuePath) {
  std::ifstream file(cuePath);
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::vector<Track> tracks;
  std::string line;
  std::string currentFile;
  std::regex fileRegex("^FILE\\s+\"([^\"]+)\"\\s+BINARY");
  std::regex trackRegex("^TRACK\\s+(\\d+)\\s+(AUDIO|MODE1/2352|MODE2/2352)");
  std::regex indexRegex("^INDEX\\s+01\\s+(\\d{2}:\\d{2}:\\d{2})");
  std::smatch match;

  Track currentTrack{};
  bool parsingTrack = false;

  while (std::getline(file, line)) {
    // Trim leading whitespace
    line.erase(0, line.find_first_not_of(" \t\r\n"));

    if (std::regex_search(line, match, fileRegex)) {
      currentFile = match[1];
    } else if (std::regex_search(line, match, trackRegex)) {
      if (parsingTrack) {
        tracks.push_back(currentTrack);
      }
      currentTrack = Track{};
      currentTrack.number = std::stoi(match[1]);
      std::string typeStr = match[2];
      currentTrack.type =
          (typeStr == "AUDIO") ? TrackType::Audio : TrackType::Data;
      currentTrack.file = currentFile;
      parsingTrack = true;
    } else if (parsingTrack && std::regex_search(line, match, indexRegex)) {
      currentTrack.msf_start = parseMSF(match[1]);
    }
  }

  if (parsingTrack) {
    tracks.push_back(currentTrack);
  }

  if (tracks.empty()) {
    return std::nullopt;
  }

  return tracks;
}

uint32_t CueParser::parseMSF(const std::string &msfStr) {
  if (msfStr.length() != 8)
    return 0; // expected format MM:SS:FF
  int m = std::stoi(msfStr.substr(0, 2));
  int s = std::stoi(msfStr.substr(3, 2));
  int f = std::stoi(msfStr.substr(6, 2));
  return (m * 60 * 75) + (s * 75) + f;
}

} // namespace ps1::cdrom
