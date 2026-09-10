#pragma once

#include "runtime/gpu/modern_replay_vulkan_bake.h"

#include <cstdint>

namespace ps1::gpu::replay {

// CPU-side defensive boundary for the Vulkan executor. The bake owns semantic
// construction; this validator proves that a retained payload can be recorded
// without out-of-bounds copies, accidental in-place overlap or a partial
// scratch snapshot.
struct VulkanReplayCopyExecutionLimits {
  uint32_t internalScale = 0;
  uint32_t nativeWidth = 0;
  uint32_t nativeHeight = 0;
  uint32_t atlasWidth = 0;
  uint32_t atlasHeight = 0;
};

enum class VulkanReplayCopyValidationReason : uint8_t {
  None = 0,
  InvalidLimits,
  UnsupportedPath,
  InvalidPlaneContract,
  InvalidNativeRead,
  InvalidDirectRect,
  DirectOverlap,
  InvalidScratchExtent,
  InvalidScratchRegionCount,
  InvalidScratchRegion,
  ScratchScaleMismatch,
  ScratchPartition,
  ScratchMapping,
};

const char *vulkanReplayCopyValidationReasonName(
    VulkanReplayCopyValidationReason reason) noexcept;

VulkanReplayCopyValidationReason validateVulkanReplayCopyForExecution(
    const VulkanReplayBakedCopyRect &copy,
    const VulkanReplayCopyExecutionLimits &limits) noexcept;

vulkan_abi::ScratchCopyPushConstants makeVulkanReplayScratchCopyPushConstants(
    const VulkanReplayBakedCopyRegion &region, const MaskState &mask) noexcept;

} // namespace ps1::gpu::replay
