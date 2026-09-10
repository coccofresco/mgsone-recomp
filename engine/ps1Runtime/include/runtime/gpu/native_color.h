#pragma once

#include <cstdint>

namespace ps1::gpu {

// Guest-authoritative PS1 ABGR1555 word. Keeping this type in a standalone
// header lets capture code consume the actual VRAM element type without
// aliasing an array of Color16 objects as an unrelated uint16_t array.
struct Color16 {
  uint16_t raw = 0;

  bool operator==(const Color16 &) const = default;
};

static_assert(sizeof(Color16) == sizeof(uint16_t));

} // namespace ps1::gpu
