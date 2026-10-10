#pragma once
/**
 * @file config.h
 * @brief Runtime configuration structures and TOML config reader.
 *
 * `ps1::ConfigReader` loads a TOML file and populates the `ps1::Config`
 * aggregate (video, audio, input, paths). Per-game PsyQ runtime state lives
 * in `psyq_state()`; the legacy `[psyq_addresses]` block was removed in
 * Phase 2.4 and no longer reaches this reader.
 *
 * Typical usage:
 * @code
 * ps1::ConfigReader reader;
 * if (!reader.load("configs/settings.toml"))
 *     fmt::print(stderr, "Config error: {}\n", reader.error());
 * auto& cfg = reader.config();
 * @endcode
 */

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ps1 {

struct VideoConfig {
  std::string backend = "vulkan";
  int windowWidth = 640;
  int windowHeight = 480;
  bool fullscreen = false;
  bool vsync = true;
  int internalResScale = 1; // 1x = native 320x240
  bool pgxp = false;
  // Original games can rely on the GTE divider's H/2 saturation. The donor-
  // compatible default preserves that visibility; geometric clipping remains
  // available as an explicit research/enhancement mode.
  bool pgxpNearClip = false;
  bool trueDepth = false;
  // "hybrid" applies donor-compatible opaque eligibility/epochs and releases
  // accepted painter samples to far for OT-later geometry; rejected samples
  // and opaque invalid-W fallbacks preserve D32. "strict" retains universal
  // opaque/far-fallback depth for diagnostics.
  std::string trueDepthMode = "hybrid";
  // Optional, explicitly experimental presentation-depth correction. The
  // manifest contains only value-independent runtime identities; absent or
  // rejected entries retain the selected trueDepthMode unchanged.
  std::string wipDepthManifest;
  bool trueColor = false;
  std::string displayArea = "programmed";
  // Named, donor-pinned full-signal convention used only when displayArea
  // selects the donor canvas. Keep it independent from the crop policy.
  std::string displaySignalProfile = "swanstation_no_crop";
  bool preserveAspect = true;
  // Bound one host-pacing slice. Excess wall-clock pulses are coalesced after
  // the slice: they must never become a burst of guest-visible VBlanks.
  uint32_t maxVblanksPerPresent = 1;
};

struct AudioConfig {
  int sampleRate = 44100;
  int bufferSize = 1024;
  int volume = 100; // 0-100
  bool mute = false;
};

struct InputBinding {
  std::string key;    // SDL key name (e.g., "z", "x", "up")
  std::string button; // PS1 button name (e.g., "cross", "circle", "up")
};

struct InputConfig {
  std::unordered_map<std::string, std::string> keyMap; // key -> ps1 button
  bool enableAnalog = false;
};

struct PathConfig {
  std::string discImage;    // Path to .bin/.cue/.iso
  std::string biosPath;     // Path to BIOS ROM (optional for HLE)
  std::string memcardDir;   // Directory for memory card files
  std::string saveStateDir; // Directory for save states
};

struct GameConfig {
  // Exact opt-in. An empty value keeps every game-specific hook inert.
  std::string profile;
  bool widescreen16x9 = false;
  // Applied only by the MGS profile; false restores simulated CD data timing.
  bool fastLoading = true;
  // Presentation only; the authoritative software raster remains native 1x.
  int presentationHeight = 2160;
  bool ambientOcclusion = true;
  bool presentationDepthCorrections = false;
  // Horizontal host-geometry admission margin in presentation pixels. The
  // final Vulkan viewport is unchanged and clips pixels outside the image.
  uint32_t sceneryCullGuardPixels = 0;
};

struct Config {
  GameConfig game;
  VideoConfig video;
  AudioConfig audio;
  InputConfig input;
  PathConfig paths;
};

class ConfigReader {
public:
  ConfigReader() = default;

  // Load config from TOML file
  bool load(const std::string &path);

  // Save current config to TOML file
  bool save(const std::string &path) const;

  // Create a default config file
  static bool createDefault(const std::string &path);

  // Access config
  const Config &config() const { return config_; }
  Config &config() { return config_; }

  // Get last error message
  const std::string &error() const { return error_; }

private:
  Config config_;
  std::string error_;
};

} // namespace ps1
