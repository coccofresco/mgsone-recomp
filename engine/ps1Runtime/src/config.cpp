#include "runtime/config.h"
#include <fmt/format.h>
#include <fstream>
#include <sstream>

namespace ps1 {

// Simple TOML-like parser
// Supports: [section], key = "value", key = number, key = true/false
// This avoids requiring the toml11 dependency at runtime.

static std::string trim(const std::string &s) {
  auto start = s.find_first_not_of(" \t\r\n");
  auto end = s.find_last_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  return s.substr(start, end - start + 1);
}

static std::string unquote(const std::string &s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

bool ConfigReader::load(const std::string &path) {
  std::ifstream file(path);
  if (!file) {
    error_ = "Cannot open config file: " + path;
    return false;
  }

  std::string line, section;
  int lineNum = 0;
  while (std::getline(file, line)) {
    lineNum++;
    line = trim(line);

    // Skip empty lines and comments
    if (line.empty() || line[0] == '#')
      continue;

    // Section header
    if (line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }

    // Key = value
    auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;

    std::string key = trim(line.substr(0, eq));
    std::string val = trim(line.substr(eq + 1));

    if (section == "game") {
      if (key == "profile")
        config_.game.profile = unquote(val);
      else if (key == "widescreen_16_9")
        config_.game.widescreen16x9 = (val == "true");
      else if (key == "fast_loading")
        config_.game.fastLoading = (val == "true");
      else if (key == "presentation_height")
        config_.game.presentationHeight = std::stoi(val);
      else if (key == "ambient_occlusion")
        config_.game.ambientOcclusion = (val == "true");
      else if (key == "presentation_depth_corrections")
        config_.game.presentationDepthCorrections = (val == "true");
      else if (key == "scenery_cull_guard_pixels")
        config_.game.sceneryCullGuardPixels =
            static_cast<uint32_t>(std::stoul(val));
    } else if (section == "video") {
      if (key == "backend")
        config_.video.backend = unquote(val);
      else if (key == "width")
        config_.video.windowWidth = std::stoi(val);
      else if (key == "height")
        config_.video.windowHeight = std::stoi(val);
      else if (key == "fullscreen")
        config_.video.fullscreen = (val == "true");
      else if (key == "vsync")
        config_.video.vsync = (val == "true");
      else if (key == "res_scale")
        config_.video.internalResScale = std::stoi(val);
      else if (key == "pgxp")
        config_.video.pgxp = (val == "true");
      else if (key == "pgxp_near_clip")
        config_.video.pgxpNearClip = (val == "true");
      else if (key == "true_depth")
        config_.video.trueDepth = (val == "true");
      else if (key == "true_depth_mode")
        config_.video.trueDepthMode = unquote(val);
      else if (key == "wip_depth_manifest")
        config_.video.wipDepthManifest = unquote(val);
      else if (key == "true_color")
        config_.video.trueColor = (val == "true");
      else if (key == "display_area")
        config_.video.displayArea = unquote(val);
      else if (key == "display_signal_profile")
        config_.video.displaySignalProfile = unquote(val);
      else if (key == "preserve_aspect")
        config_.video.preserveAspect = (val == "true");
      else if (key == "max_vblanks_per_present")
        config_.video.maxVblanksPerPresent =
            static_cast<uint32_t>(std::stoul(val));
    } else if (section == "audio") {
      if (key == "sample_rate")
        config_.audio.sampleRate = std::stoi(val);
      else if (key == "buffer_size")
        config_.audio.bufferSize = std::stoi(val);
      else if (key == "volume")
        config_.audio.volume = std::stoi(val);
      else if (key == "mute")
        config_.audio.mute = (val == "true");
    } else if (section == "input") {
      if (key == "analog")
        config_.input.enableAnalog = (val == "true");
      else {
        // Key binding: key_name = "ps1_button"
        config_.input.keyMap[key] = unquote(val);
      }
    } else if (section == "paths") {
      if (key == "disc")
        config_.paths.discImage = unquote(val);
      else if (key == "bios")
        config_.paths.biosPath = unquote(val);
      else if (key == "memcard_dir")
        config_.paths.memcardDir = unquote(val);
      else if (key == "savestate_dir")
        config_.paths.saveStateDir = unquote(val);
    }
  }

  return true;
}

bool ConfigReader::save(const std::string &path) const {
  std::ofstream f(path);
  if (!f)
    return false;

  f << "# ps1Recomp Configuration\n\n";

  if (!config_.game.profile.empty()) {
    f << "[game]\n";
    f << "profile = \"" << config_.game.profile << "\"\n";
    f << "widescreen_16_9 = "
      << (config_.game.widescreen16x9 ? "true" : "false") << "\n";
    f << "fast_loading = "
      << (config_.game.fastLoading ? "true" : "false") << "\n";
    f << "presentation_height = " << config_.game.presentationHeight << "\n";
    f << "ambient_occlusion = " << (config_.game.ambientOcclusion ? "true" : "false") << "\n";
    f << "presentation_depth_corrections = "
      << (config_.game.presentationDepthCorrections ? "true" : "false")
      << "\n";
    f << "scenery_cull_guard_pixels = "
      << config_.game.sceneryCullGuardPixels << "\n\n";
  }

  f << "[video]\n";
  f << "backend = \"" << config_.video.backend << "\"\n";
  f << "width = " << config_.video.windowWidth << "\n";
  f << "height = " << config_.video.windowHeight << "\n";
  f << "fullscreen = " << (config_.video.fullscreen ? "true" : "false") << "\n";
  f << "vsync = " << (config_.video.vsync ? "true" : "false") << "\n";
  f << "res_scale = " << config_.video.internalResScale << "\n";
  f << "pgxp = " << (config_.video.pgxp ? "true" : "false") << "\n";
  f << "pgxp_near_clip = " << (config_.video.pgxpNearClip ? "true" : "false")
    << "\n";
  f << "true_depth = " << (config_.video.trueDepth ? "true" : "false") << "\n";
  f << "true_depth_mode = \"" << config_.video.trueDepthMode << "\"\n";
  if (!config_.video.wipDepthManifest.empty())
    f << "wip_depth_manifest = \"" << config_.video.wipDepthManifest
      << "\"\n";
  f << "true_color = " << (config_.video.trueColor ? "true" : "false") << "\n";
  f << "display_area = \"" << config_.video.displayArea << "\"\n";
  f << "display_signal_profile = \"" << config_.video.displaySignalProfile
    << "\"\n";
  f << "preserve_aspect = " << (config_.video.preserveAspect ? "true" : "false")
    << "\n";
  f << "max_vblanks_per_present = " << config_.video.maxVblanksPerPresent
    << "\n\n";

  f << "[audio]\n";
  f << "sample_rate = " << config_.audio.sampleRate << "\n";
  f << "buffer_size = " << config_.audio.bufferSize << "\n";
  f << "volume = " << config_.audio.volume << "\n";
  f << "mute = " << (config_.audio.mute ? "true" : "false") << "\n\n";

  f << "[input]\n";
  f << "analog = " << (config_.input.enableAnalog ? "true" : "false") << "\n";
  for (const auto &[key, btn] : config_.input.keyMap) {
    f << key << " = \"" << btn << "\"\n";
  }
  f << "\n";

  f << "[paths]\n";
  if (!config_.paths.discImage.empty())
    f << "disc = \"" << config_.paths.discImage << "\"\n";
  if (!config_.paths.biosPath.empty())
    f << "bios = \"" << config_.paths.biosPath << "\"\n";
  if (!config_.paths.memcardDir.empty())
    f << "memcard_dir = \"" << config_.paths.memcardDir << "\"\n";
  if (!config_.paths.saveStateDir.empty())
    f << "savestate_dir = \"" << config_.paths.saveStateDir << "\"\n";

  return true;
}

bool ConfigReader::createDefault(const std::string &path) {
  ConfigReader reader;

  // Default key bindings
  reader.config_.input.keyMap["up"] = "up";
  reader.config_.input.keyMap["down"] = "down";
  reader.config_.input.keyMap["left"] = "left";
  reader.config_.input.keyMap["right"] = "right";
  reader.config_.input.keyMap["z"] = "cross";
  reader.config_.input.keyMap["x"] = "circle";
  reader.config_.input.keyMap["a"] = "square";
  reader.config_.input.keyMap["s"] = "triangle";
  reader.config_.input.keyMap["q"] = "l1";
  reader.config_.input.keyMap["w"] = "r1";
  reader.config_.input.keyMap["e"] = "l2";
  reader.config_.input.keyMap["r"] = "r2";
  reader.config_.input.keyMap["return"] = "start";
  reader.config_.input.keyMap["backspace"] = "select";
  reader.config_.input.keyMap["right_shift"] = "select";

  reader.config_.paths.memcardDir = "./memcards";
  reader.config_.paths.saveStateDir = "./savestates";

  return reader.save(path);
}

} // namespace ps1
