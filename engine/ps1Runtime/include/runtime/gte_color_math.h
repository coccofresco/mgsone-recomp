#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace ps1::gte_math {

struct ColorChannel {
  int32_t mac = 0;
  int32_t ir = 0;
  uint8_t color = 0;
  // Channel-one FLAG positions; shift right by 0/1/2 for R/G/B, and update
  // bit 31 once after the complete instruction. Includes intermediate flags.
  uint32_t flags = 0;
};

// Exact two-stage colour interpolation, including the intermediate signed IR
// saturation (independent of lm), sf at BOTH stages, and low-32-bit MAC writes.
// Input is the command's unshifted colour product: RGB<<16 for DPCS, IR<<12
// for INTPL, or RGB*IR<<4 for DCPL/NCDx/CDP. No CPU or game dependencies.
inline ColorChannel interpolateColorChannel(int64_t input, int32_t farColor,
                                            int16_t factor, bool sf,
                                            bool lm) noexcept {
  ColorChannel result;
  const unsigned shift = sf ? 12u : 0u;
  const auto mac = [&](int64_t value) {
    if (value > (INT64_C(1) << 43) - 1)
      result.flags |= 1u << 30;
    if (value < -(INT64_C(1) << 43))
      result.flags |= 1u << 27;
    return std::bit_cast<int32_t>(static_cast<uint32_t>(value >> shift));
  };
  const int32_t difference = mac(int64_t{farColor} * 4096 - input);
  const int32_t delta = std::clamp(difference, -32768, 32767);
  if (delta != difference)
    result.flags |= 1u << 24;
  result.mac = mac(input + int64_t{factor} * delta);
  result.ir = std::clamp(result.mac, lm ? 0 : -32768, 32767);
  if (result.ir != result.mac)
    result.flags |= 1u << 24;
  const int32_t color = result.mac >> 4;
  result.color = static_cast<uint8_t>(std::clamp(color, 0, 255));
  if (color != result.color)
    result.flags |= 1u << 21;
  return result;
}

// Pure RGB result of DPCS(sf=1). The runtime and host source constructors
// share this arithmetic; only the external donor is an independent oracle.
// rgb contains native integer byte values (0..255) represented as floats;
// apply this before any host interpolation or attribute clipping.
inline std::array<float, 3>
distanceCueColor(const std::array<float, 3> &rgb,
                 const std::array<int32_t, 3> &farColor,
                 int16_t factor) noexcept {
  std::array<float, 3> result{};
  for (uint32_t channel = 0; channel < 3u; ++channel) {
    const int64_t input = static_cast<uint8_t>(rgb[channel]);
    result[channel] = interpolateColorChannel(input * 65536, farColor[channel],
                                              factor, true, false)
                          .color;
  }
  return result;
}

} // namespace ps1::gte_math
