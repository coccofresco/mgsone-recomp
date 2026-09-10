#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ps1::input {

class InputController;

// Deterministic, diagnostic-only controller input keyed to guest VBlank.
// The same compact interval format is understood by the libretro donor
// runner, allowing one script to drive the recomp and reference emulators.
class InputScript {
public:
  struct Interval {
    uint64_t firstVblank = 0;
    uint64_t lastVblank = 0;
    uint16_t button = 0;
    unsigned port = 0;
  };

  bool parse(std::string_view text, std::string *error = nullptr);
  bool loadFile(const std::filesystem::path &path,
                std::string *error = nullptr);
  void apply(uint64_t vblank, InputController &input) const;

  std::size_t intervalCount() const { return intervals_.size(); }

private:
  std::vector<Interval> intervals_;
};

} // namespace ps1::input
