#include "runtime/cdrom/xa_adpcm.h"
#include <algorithm>

namespace ps1::cdrom {
// Hardware interpolation coefficients documented by PSX-SPX (CDROM format),
// also used by SwanStation's CDROM::ResampleXAADPCM. Preserve integer rounding.
static constexpr std::array<std::array<int16_t, 29>, 7> kInterpolation = {
  {{0,      0x0,     0x0,     0x0,    0x0,     -0x0002, 0x000A,  -0x0022, 0x0041, -0x0054,
    0x0034, 0x0009,  -0x010A, 0x0400, -0x0A78, 0x234C,  0x6794,  -0x1780, 0x0BCD, -0x0623,
    0x0350, -0x016D, 0x006B,  0x000A, -0x0010, 0x0011,  -0x0008, 0x0003,  -0x0001},
   {0,       0x0,    0x0,     -0x0002, 0x0,    0x0003,  -0x0013, 0x003C,  -0x004B, 0x00A2,
    -0x00E3, 0x0132, -0x0043, -0x0267, 0x0C9D, 0x74BB,  -0x11B4, 0x09B8,  -0x05BF, 0x0372,
    -0x01A8, 0x00A6, -0x001B, 0x0005,  0x0006, -0x0008, 0x0003,  -0x0001, 0x0},
   {0,      0x0,     -0x0001, 0x0003,  -0x0002, -0x0005, 0x001F,  -0x004A, 0x00B3, -0x0192,
    0x02B1, -0x039E, 0x04F8,  -0x05A6, 0x7939,  -0x05A6, 0x04F8,  -0x039E, 0x02B1, -0x0192,
    0x00B3, -0x004A, 0x001F,  -0x0005, -0x0002, 0x0003,  -0x0001, 0x0,     0x0},
   {0,       -0x0001, 0x0003,  -0x0008, 0x0006, 0x0005,  -0x001B, 0x00A6, -0x01A8, 0x0372,
    -0x05BF, 0x09B8,  -0x11B4, 0x74BB,  0x0C9D, -0x0267, -0x0043, 0x0132, -0x00E3, 0x00A2,
    -0x004B, 0x003C,  -0x0013, 0x0003,  0x0,    -0x0002, 0x0,     0x0,    0x0},
   {-0x0001, 0x0003,  -0x0008, 0x0011,  -0x0010, 0x000A, 0x006B,  -0x016D, 0x0350, -0x0623,
    0x0BCD,  -0x1780, 0x6794,  0x234C,  -0x0A78, 0x0400, -0x010A, 0x0009,  0x0034, -0x0054,
    0x0041,  -0x0022, 0x000A,  -0x0001, 0x0,     0x0001, 0x0,     0x0,     0x0},
   {0x0002,  -0x0008, 0x0010,  -0x0023, 0x002B, 0x001A,  -0x00EB, 0x027B,  -0x0548, 0x0AFA,
    -0x16FA, 0x53E0,  0x3C07,  -0x1249, 0x080E, -0x0347, 0x015B,  -0x0044, -0x0017, 0x0046,
    -0x0023, 0x0011,  -0x0005, 0x0,     0x0,    0x0,     0x0,     0x0,     0x0},
   {-0x0005, 0x0011,  -0x0023, 0x0046, -0x0017, -0x0044, 0x015B,  -0x0347, 0x080E, -0x1249,
    0x3C07,  0x53E0,  -0x16FA, 0x0AFA, -0x0548, 0x027B,  -0x00EB, 0x001A,  0x002B, -0x0023,
    0x0010,  -0x0008, 0x0002,  0x0,    0x0,     0x0,     0x0,     0x0,     0x0}}};


std::vector<int16_t> XaAdpcm::decode(const uint8_t *raw, bool resample) {
  const uint8_t coding = raw[19];
  const bool stereo = (coding & 3) == 1;
  const bool halfRate = ((coding >> 2) & 3) == 1;
  const bool eightBit = ((coding >> 4) & 3) == 1;
  const unsigned blocks = eightBit ? 4 : 8;
  const unsigned channels = stereo ? 2 : 1;
  constexpr int32_t positive[] = {0, 60, 115, 98};
  constexpr int32_t negative[] = {0, 0, -52, -55};
  std::array<int16_t, 4032> decoded{};
  for (unsigned group = 0; group < 18; ++group) {
    const auto *data = raw + 24 + group * 128;
    for (unsigned block = 0; block < blocks; ++block) {
      const unsigned filter = (data[4 + block] >> 4) & 3;
      unsigned shift = data[4 + block] & 15;
      if (shift > 12) shift = 9;
      auto *history = state_.history.data() + (stereo ? (block & 1) * 2 : 0);
      const unsigned first = group * blocks * 28 +
          (stereo ? (block / 2) * 56 + (block & 1) : block * 28);
      for (unsigned i = 0; i < 28; ++i) {
        const uint8_t packed = data[16 + i * 4 + (eightBit ? block : block / 2)];
        const int32_t signedCode = eightBit ? (packed < 128 ? packed : int(packed) - 256)
            : ((packed >> ((block & 1) * 4)) & 15);
        const int32_t input = eightBit ? signedCode * 256
            : (signedCode < 8 ? signedCode : signedCode - 16) * 4096;
        // Match the donor's predictor history (unclipped), with wide math to
        // avoid signed overflow on malformed sectors. Eight-bit uses <<8,
        // as specified by PSX-SPX, rather than the donor's four-bit-only shift.
        const int64_t prediction = (int64_t(history[0]) * positive[filter] +
                                   int64_t(history[1]) * negative[filter] + 32) / 64;
        const int32_t sample = static_cast<int32_t>(std::clamp<int64_t>(
            (input >> shift) + prediction, INT32_MIN, INT32_MAX));
        history[1] = history[0];
        history[0] = sample;
        decoded[first + i * channels] = static_cast<int16_t>(std::clamp(sample, -32768, 32767));
      }
    }
  }
  if (!resample) return {}; // muted drive still updates ADPCM prediction
  std::vector<int16_t> output;
  const unsigned frames = 18 * blocks * 28 / channels;
  output.reserve(frames * (halfRate ? 2 : 1) * 7 / 6 * 2);
  for (unsigned i = 0; i < frames; ++i) {
    for (unsigned repeat = 0; repeat < (halfRate ? 2u : 1u); ++repeat) {
      state_.ring[0][state_.position] = decoded[i * channels];
      state_.ring[1][state_.position] = decoded[i * channels + (stereo ? 1 : 0)];
      state_.position = (state_.position + 1) & 31;
      if (--state_.sixstep != 0) continue;
      state_.sixstep = 6;
      for (const auto &coefficients : kInterpolation) {
        for (unsigned channel = 0; channel < 2; ++channel) {
          int32_t sum = 0;
          for (unsigned tap = 0; tap < 29; ++tap)
            sum += int32_t(state_.ring[channel][(state_.position - tap) & 31]) *
                   coefficients[tap] / 32768;
          output.push_back(static_cast<int16_t>(std::clamp(sum, -32768, 32767)));
        }
      }
    }
  }
  return output;
}
} // namespace ps1::cdrom
