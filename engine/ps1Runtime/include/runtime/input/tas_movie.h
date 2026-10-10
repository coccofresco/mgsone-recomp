#pragma once

#include "runtime/input/input.h"
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace ps1::input {

// A movie records the native cooperative boundary as well as the pad sample.
// VBlank alone is insufficient: host timing can otherwise interrupt different
// iterations of the translated idle/CD polling loops on each run.
class TasMovie {
public:
  struct Frame {
    uint64_t boundary = 0;
    uint64_t vblank = 0;
    uint64_t ramHash = 0;
    uint32_t resumePc = 0;
    std::array<InputController::PortSnapshot, 2> pads{};
  };

  bool open(const std::filesystem::path &path, bool replay, std::string &error);
  // Diagnostic rendezvous without a movie/hash per frame. Timelines may be
  // restarted ONLY in this mode, after suspendProbe releases the old gate.
  void openProbe();
  void suspendProbe();
  void start(uint64_t vblank); // after the initial savestate has been restored
  void requestTick();          // host: request the next native safe boundary
  void checkpoint(uint32_t resumePc, bool resumable = true); // guest rendezvous
  bool parked() const;
  bool canSave() const;
  bool frame(uint64_t vblank, const uint8_t *ram, size_t size,
             InputController &input); // host, with guest parked
  void release();
  bool end(uint64_t vblank, const uint8_t *ram, size_t size);
  void finish(); // flush footer before runtime's possible _Exit shutdown
  bool enabled() const { return enabled_; }
  bool replaying() const { return replay_; }
  bool active() const;
  bool done() const;
  bool endDue() const;
  bool failed() const;
  std::string error() const;
  uint64_t frameCount() const;
  uint64_t boundary() const;
  static uint64_t hash(const uint8_t *data, size_t size);

private:
  void failLocked(std::string message);
  mutable std::mutex mutex_;
  std::condition_variable resumed_;
  bool enabled_ = false, replay_ = false, active_ = false;
  bool probe_ = false;
  bool requested_ = false, parked_ = false, stopped_ = false;
  bool sealed_ = false;
  bool resumable_ = false;
  uint64_t boundary_ = 0, count_ = 0, lastVblank_ = 0;
  uint32_t resumePc_ = 0;
  std::string error_;
  std::ofstream output_;
  std::vector<Frame> frames_;
  Frame terminal_{};
};

} // namespace ps1::input
