#pragma once
#include "runtime/gpu/modern_replay_vulkan_bake.h"
#include <map>
#include <optional>

namespace ps1::games::mgs {
// The host joins authenticated source faces; Vulkan still pulls their original
// vertices, projects/rasterizes them, lights them per pixel and composes effects.
// No pixel blending, extrapolation, guest writes or additional simulation ticks.
struct TemporalPair {
  uint32_t steps = 0, matchedFaces = 0, movingFaces = 0;
  std::array<std::vector<gpu::replay::vulkan_abi::DrawUniforms>, 2> phases;
  uint32_t pageX = 0, pageY = 0;
};
class FrameInterpolator {
public:
  std::optional<TemporalPair> advance(const gpu::replay::VulkanReplayBakedBatch &,
                                     uint64_t presentationFrame);
  void reset();
private:
  using Key = std::array<uint64_t, 4>;
  using Uniforms = gpu::replay::vulkan_abi::DrawUniforms;
  std::map<Key, Uniforms> previous_;
  std::array<float, 16> camera_{};
  uint64_t presentationFrame_ = 0, epoch_ = 0;
  uint32_t sourceFrame_ = 0;
};
}
