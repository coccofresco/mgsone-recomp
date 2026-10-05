#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ps1::input {
// Input-only adaptive macro runner. Called with the guest parked at a TAS
// VBlank boundary; no RAM writes, device timing overrides or state reloads.
class TasRoute {
public:
  bool parse(std::string_view text, std::string& error);
  bool load(const std::filesystem::path& path, std::string& error);
  std::array<uint16_t, 2> sample(const uint8_t* ram, size_t size, uint64_t vblank);
  bool done() const { return index_ >= steps_.size(); }
  bool failed() const { return !error_.empty(); }
  const std::string& error() const { return error_; }
  std::vector<std::string> takeEvents();
private:
  struct Predicate { std::string field, op, value; uint32_t mask; };
  struct Run { uint64_t frames; std::array<uint16_t, 2> pads; };
  struct Step {
    std::string name;
    uint64_t budget = 0, attempts = 0;
    uint64_t stableFrames = 1;
    std::vector<Predicate> when, until, guard, abort;
    std::vector<Run> runs;
    bool move = false;
    bool gameClock = false;
    int targetX = 0, targetZ = 0;
  };
  std::vector<Step> steps_;
  size_t index_ = 0, run_ = 0;
  uint64_t elapsed_ = 0, used_ = 0, remaining_ = 0;
  uint64_t goalStreak_ = 0;
  bool playing_ = false;
  uint32_t lastGameTick_ = 0;
  std::string error_;
  std::vector<std::string> events_;
};
}
