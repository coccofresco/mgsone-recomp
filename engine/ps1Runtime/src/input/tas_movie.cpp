#include "runtime/input/tas_movie.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace ps1::input {

uint64_t TasMovie::hash(const uint8_t *data, size_t size) {
  uint64_t value = 14695981039346656037ull;
  for (size_t i = 0; i < size; ++i) {
    value ^= data[i];
    value *= 1099511628211ull;
  }
  return value;
}

bool TasMovie::open(const std::filesystem::path &path, bool replay,
                    std::string &error) {
  replay_ = replay;
  if (replay) {
    std::ifstream source(path);
    std::string line;
    if (!std::getline(source, line) || line != "MGSTAS 1") {
      error = "missing/unsupported MGSTAS header";
      return false;
    }
    bool footer = false;
    while (std::getline(source, line)) {
      if (line.size() < 3 ||
          !std::all_of(line.begin() + 1, line.end(), [](unsigned char c) {
            return std::isdigit(c) || c == ' ';
          })) {
        error = "invalid TAS numeric field";
        return false;
      }
      std::istringstream fields(line);
      char tag = 0;
      fields >> tag;
      if (tag == 'E') {
        uint64_t count = 0;
        std::string extra;
        if (!(fields >> count >> terminal_.boundary >> terminal_.vblank >>
              terminal_.ramHash >> terminal_.resumePc) ||
            count != frames_.size() || frames_.empty() ||
            terminal_.boundary <= frames_.back().boundary ||
            terminal_.vblank != frames_.back().vblank || (fields >> extra) ||
            std::getline(source, line)) {
          error = "invalid TAS footer";
          return false;
        }
        footer = true;
        break;
      }
      Frame frame;
      if (tag != 'F' || !(fields >> frame.boundary >> frame.vblank >>
                          frame.ramHash >> frame.resumePc)) {
        error = "invalid TAS frame";
        return false;
      }
      for (auto &pad : frame.pads) {
        unsigned buttons, lx, ly, rx, ry, type;
        if (!(fields >> buttons >> lx >> ly >> rx >> ry >> type) ||
            buttons > 65535 || lx > 255 || ly > 255 || rx > 255 || ry > 255 ||
            (type != 0 && type != 0x41 && type != 0x73 && type != 0x79)) {
          error = "invalid TAS controller sample";
          return false;
        }
        pad = {static_cast<uint16_t>(buttons), static_cast<uint8_t>(lx),
               static_cast<uint8_t>(ly),       static_cast<uint8_t>(rx),
               static_cast<uint8_t>(ry),       static_cast<PadType>(type)};
      }
      std::string extra;
      if ((fields >> extra) || frame.boundary == 0 || frame.vblank == 0 ||
          (!frames_.empty() && (frame.boundary <= frames_.back().boundary ||
                                frame.vblank != frames_.back().vblank + 1))) {
        error = "non-contiguous/invalid TAS timeline";
        return false;
      }
      frames_.push_back(frame);
    }
    if (!footer || frames_.empty()) {
      error = "incomplete/empty TAS recording (no successful footer)";
      return false;
    }
  } else {
    if (std::filesystem::exists(path)) {
      error = "refusing to overwrite an existing movie";
      return false;
    }
    output_.open(path);
    output_ << "MGSTAS 1\n";
    output_.flush();
    if (!output_) {
      error = "cannot create TAS recording";
      return false;
    }
  }
  enabled_ = true;
  return true;
}

void TasMovie::start(uint64_t vblank) {
  std::lock_guard lock(mutex_);
  if (active_ || count_ != 0) {
    failLocked(
        "a movie cannot silently cross a savestate load; create a branch");
    return;
  }
  lastVblank_ = vblank;
  boundary_ = 0;
  active_ = true;
  if (replay_ && frames_.front().vblank != vblank + 1)
    failLocked("initial savestate VBlank does not match the movie");
}

void TasMovie::requestTick() {
  std::lock_guard lock(mutex_);
  if (active_ && !stopped_ && !replay_)
    requested_ = true;
}

void TasMovie::checkpoint(uint32_t resumePc, bool resumable) {
  std::unique_lock lock(mutex_);
  if (!active_ || stopped_)
    return;
  ++boundary_;
  const uint64_t target =
      replay_ ? (count_ == frames_.size() ? terminal_.boundary
                                          : frames_[count_].boundary)
              : 0;
  if (replay_ ? boundary_ != target : !requested_)
    return;
  resumePc_ = resumePc;
  resumable_ = resumable;
  parked_ = true;
  resumed_.wait(lock, [&] { return !parked_ || stopped_; });
}

bool TasMovie::parked() const {
  std::lock_guard lock(mutex_);
  return parked_ && !stopped_;
}

bool TasMovie::canSave() const {
  std::lock_guard lock(mutex_);
  return parked_ && resumable_ && !stopped_;
}

void TasMovie::failLocked(std::string message) {
  error_ = std::move(message);
  // Keep the guest parked until finish(): diagnostics must see the exact
  // divergent RAM, not a racing snapshot taken after resuming the game.
}

bool TasMovie::frame(uint64_t vblank, const uint8_t *ram, size_t size,
                     InputController &input) {
  std::lock_guard lock(mutex_);
  if (!parked_ || stopped_ || vblank != lastVblank_ + 1 ||
      (replay_ && count_ >= frames_.size())) {
    failLocked("TAS clock advanced without its native rendezvous");
    return false;
  }
  Frame frame;
  frame.boundary = boundary_;
  frame.vblank = vblank;
  frame.ramHash = hash(ram, size);
  frame.resumePc = resumePc_;
  if (replay_) {
    const Frame &expected = frames_[count_];
    if (frame.vblank != expected.vblank ||
        frame.resumePc != expected.resumePc ||
        frame.ramHash != expected.ramHash) {
      failLocked("desync before movie frame " + std::to_string(count_ + 1) +
                 ", VBlank " + std::to_string(vblank) + ", boundary " +
                 std::to_string(boundary_) + ": RAM " +
                 std::to_string(frame.ramHash) + " expected " +
                 std::to_string(expected.ramHash) + ", PC " +
                 std::to_string(frame.resumePc) + " expected " +
                 std::to_string(expected.resumePc));
      return false;
    }
    input.setMovieSample(expected.pads);
  } else {
    frame.pads = input.latchedSample();
    output_ << "F " << frame.boundary << ' ' << frame.vblank << ' '
            << frame.ramHash << ' ' << frame.resumePc;
    for (const auto &pad : frame.pads)
      output_ << ' ' << pad.buttons << ' ' << unsigned(pad.analogLX) << ' '
              << unsigned(pad.analogLY) << ' ' << unsigned(pad.analogRX) << ' '
              << unsigned(pad.analogRY) << ' ' << unsigned(pad.padType);
    output_ << '\n';
    output_.flush(); // preserve the valid prefix even if the game crashes
    if (!output_) {
      failLocked("TAS recording write failed");
      return false;
    }
  }
  lastVblank_ = vblank;
  ++count_;
  return true;
}

void TasMovie::release() {
  std::lock_guard lock(mutex_);
  requested_ = false;
  parked_ = false;
  resumed_.notify_all();
}

bool TasMovie::end(uint64_t vblank, const uint8_t *ram, size_t size) {
  std::lock_guard lock(mutex_);
  if (!parked_ || vblank != lastVblank_) {
    failLocked("terminal TAS witness requested outside its boundary");
    return false;
  }
  const auto ramHash = hash(ram, size);
  if (replay_ && (count_ != frames_.size() || terminal_.boundary != boundary_ ||
                  terminal_.vblank != vblank || terminal_.ramHash != ramHash ||
                  terminal_.resumePc != resumePc_)) {
    failLocked("desync at terminal TAS witness after frame " +
               std::to_string(count_));
    return false;
  }
  if (!replay_) {
    output_ << "E " << count_ << ' ' << boundary_ << ' ' << vblank << ' '
            << ramHash << ' ' << resumePc_ << '\n';
    output_.flush();
    if (!output_) {
      failLocked("TAS terminal witness write failed");
      return false;
    }
  }
  sealed_ = true;
  return true;
}

void TasMovie::finish() {
  std::lock_guard lock(mutex_);
  if (!enabled_)
    return;
  if (output_.is_open()) {
    if (!sealed_ && error_.empty())
      error_ = "incomplete movie: no terminal witness";
    output_.flush();
    if (!output_ && error_.empty())
      error_ = "TAS footer write failed";
    output_.close();
  }
  stopped_ = true;
  parked_ = false;
  resumed_.notify_all();
}

bool TasMovie::active() const {
  std::lock_guard lock(mutex_);
  return active_;
}
bool TasMovie::done() const {
  std::lock_guard lock(mutex_);
  return sealed_;
}
bool TasMovie::endDue() const {
  std::lock_guard lock(mutex_);
  return replay_ && count_ == frames_.size();
}
bool TasMovie::failed() const {
  std::lock_guard lock(mutex_);
  return !error_.empty();
}
std::string TasMovie::error() const {
  std::lock_guard lock(mutex_);
  return error_;
}
uint64_t TasMovie::frameCount() const {
  std::lock_guard lock(mutex_);
  return count_;
}
uint64_t TasMovie::boundary() const {
  std::lock_guard lock(mutex_);
  return boundary_;
}

} // namespace ps1::input
