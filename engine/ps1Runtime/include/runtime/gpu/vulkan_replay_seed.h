#pragma once

#include "runtime/gpu/native_color.h"

#include <cstdint>

namespace ps1::gpu {

// CPU-visible oracle for the Vulkan M0Reanchor seed shader. This is a
// single-texel contract only: the runtime never constructs a CPU-side scaled
// atlas. Every native texel is expanded and replicated by Vulkan.
struct VulkanReplaySeedTexel {
  uint8_t red = 0;
  uint8_t green = 0;
  uint8_t blue = 0;
  uint8_t alpha = 255;
  uint8_t mask = 0;

  bool operator==(const VulkanReplaySeedTexel &) const = default;
};

struct VulkanReplaySeedCoordinate {
  uint32_t x = 0;
  uint32_t y = 0;

  bool operator==(const VulkanReplaySeedCoordinate &) const = default;
};

inline constexpr float VULKAN_REPLAY_SEED_DEPTH_CLEAR = 1.0f;

inline constexpr VulkanReplaySeedCoordinate
mapVulkanReplaySeedCoordinate(uint32_t atlasX, uint32_t atlasY,
                              uint32_t internalScale) {
  const uint32_t scale = internalScale == 0 ? 1u : internalScale;
  return {atlasX / scale, atlasY / scale};
}

inline constexpr uint8_t expandReplaySeed5To8(uint16_t value) {
  value &= 31u;
  return static_cast<uint8_t>((value << 3u) | (value >> 2u));
}

inline constexpr VulkanReplaySeedTexel
decodeVulkanReplaySeedTexel(Color16 packed) {
  return {
      expandReplaySeed5To8(packed.raw),
      expandReplaySeed5To8(static_cast<uint16_t>(packed.raw >> 5u)),
      expandReplaySeed5To8(static_cast<uint16_t>(packed.raw >> 10u)),
      255u,
      static_cast<uint8_t>((packed.raw >> 15u) & 1u),
  };
}

} // namespace ps1::gpu
