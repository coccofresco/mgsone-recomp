#pragma once

#include "runtime/gpu/modern_replay_vulkan_bake.h"
#include <optional>

namespace ps1::games::mgs {
// MGS US uses two adjacent 320x224 draw buffers. This is a presentation-only
// read of those surfaces, independent of the native GPUREAD/VRAM mirror.
inline constexpr uint32_t SCREEN_TEXTURE_WIDTH = 640;
inline constexpr uint32_t SCREEN_TEXTURE_HEIGHT = 224;
struct ScreenRead {
  gpu::replay::VulkanReplayBakedRect source;
  gpu::replay::VulkanReplayBakedRect destination;
};
std::optional<ScreenRead>
screenRead(const gpu::replay::VulkanReplayBakedDraw &draw,
           const gpu::replay::VulkanReplayBakedBatch &batch);
void prepareScreenReads(gpu::replay::VulkanReplayBakedBatch &batch);
void captureWaterEffects(std::span<const uint8_t> ram);
void bindWaterEffect(gpu::replay::DrawPacket &packet, uint32_t commandAddress);

// A snapshot can serve successive strips only while none of their source
// pixels have been written since the snapshot. This preserves deliberate
// feedback, including overlapping blur passes, without copying per sprite.
class ScreenReadTracker {
public:
  void invalidate();
  bool needsSnapshot(const ScreenRead &read);

private:
  bool valid_ = false;
  std::vector<gpu::replay::VulkanReplayBakedRect> writes_;
};
} // namespace ps1::games::mgs
