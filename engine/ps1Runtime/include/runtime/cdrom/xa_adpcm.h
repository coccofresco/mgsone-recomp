#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace ps1::cdrom {
// Stateful XA predictor and 6:7 interpolation. Input is a complete raw sector;
// output is interleaved stereo PCM at 44100 Hz, before the CD volume matrix.
class XaAdpcm {
public:
  struct Snapshot {
    std::array<int32_t, 4> history{};
    std::array<std::array<int16_t, 32>, 2> ring{};
    uint8_t position = 0;
    uint8_t sixstep = 6;
  };
  void reset() { state_ = Snapshot{}; }
  Snapshot snapshot() const { return state_; }
  void restore(const Snapshot &state) { state_ = state; }
  std::vector<int16_t> decode(const uint8_t *raw, bool resample = true);
private:
  Snapshot state_;
};
} // namespace ps1::cdrom
