#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace ps1::gte_math {

inline constexpr auto unrTable = [] {
  std::array<uint8_t, 257> table{};
  for (uint32_t i = 0; i < table.size(); ++i) {
    const int32_t seed =
        static_cast<int32_t>(((0x40000u / (i + 0x100u)) + 1u) / 2u) - 0x101;
    table[i] = static_cast<uint8_t>(std::clamp(seed, 0, 255));
  }
  return table;
}();

// Pure architectural projection math. Flagging remains the GTE caller's job;
// presentation decoders can reproduce native construction branches without
// executing guest instructions or mutating a global GTE/PGXP history.
inline uint32_t divide(uint16_t h, uint16_t z) noexcept {
  if (z == 0 || uint32_t{h} >= uint32_t{z} * 2u)
    return 0x1ffffu;
  const uint32_t shift = std::countl_zero(uint32_t{z}) - 16u;
  const uint32_t normalized = (uint32_t{z} << shift) & 0x7fffu;
  const uint32_t seed = unrTable[(normalized + 0x40u) >> 7u] + 0x101u;
  const uint32_t correction =
      static_cast<uint32_t>(
          (INT64_C(0x80) - int64_t{seed} * (normalized + 0x8000u)) >> 8u) &
      0x1ffffu;
  const uint32_t reciprocal = (seed * correction + 0x80u) >> 8u;
  return static_cast<uint32_t>(
      std::min((uint64_t{reciprocal} * (uint32_t{h} << shift) + 0x8000u) >> 16u,
               UINT64_C(0x1ffff)));
}

inline int32_t screenCoordinate(int32_t cameraMac, uint32_t quotient,
                                int32_t origin) noexcept {
  // Match doRTPS's MAC -> signed screen quotient -> SXY saturation contract.
  // Do not introduce another IR clamp absent from that construction path.
  const auto screen =
      static_cast<int32_t>((int64_t{cameraMac} * quotient + origin) >> 16u);
  return std::clamp(screen, -1024, 1023);
}

} // namespace ps1::gte_math
