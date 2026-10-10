#pragma once
#include "runtime/input/input_script.h"
#include "runtime/input/tas_route.h"
#include "runtime/save_state.h"
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace ps1 {
// Explicit diagnostic only. All RAM inspection happens with the guest parked;
// inputs use the ordinary VBlank latch and saves use the player coordinator.
class SoakProbe {
public:
  bool load(const std::string& manifest, std::string& error);
  bool enabled() const { return !cases_.empty(); }
  bool finished() const { return phase_ == Phase::Done || phase_ == Phase::Failed; }
  bool passed() const { return phase_ == Phase::Done; }
  bool running() const { return phase_ == Phase::Run; }
  void tick(SaveStateCoordinator&, uint64_t vblank, const uint8_t* ram);
  void completed(const SaveStateCoordinator::Request&, bool ok, uint64_t vblank,
                 const uint8_t* ram);
  void input(uint64_t vblank, input::InputController&, const uint8_t* ram);
  void presented(bool vulkan, uint64_t workUs);
private:
  using Clock = std::chrono::steady_clock;
  enum class Phase { Begin, Loading, Hold, Run, Park, Saving, Restoring, Next, Done, Failed };
  struct Case {
    std::string name, state, saved, endpoint, route;
    uint64_t frames;
    input::InputScript inputs;
  };
  void fail(const std::string& reason);
  void sample(const uint8_t* ram, uint64_t tick);
  std::vector<Case> cases_;
  input::TasRoute route_;
  std::filesystem::path output_;
  Phase phase_ = Phase::Begin;
  size_t index_ = 0, cycle_ = 0, cycles_ = 0;
  uint64_t startTick_ = 0, savedTick_ = 0;
  uint32_t savedHash_ = 0;
  Clock::time_point deadline_{}, holdUntil_{}, runStart_{}, previousPresent_{};
  std::vector<uint64_t> workUs_, intervalUs_;
};
}
