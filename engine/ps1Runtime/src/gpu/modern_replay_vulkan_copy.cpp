#include "runtime/gpu/modern_replay_vulkan_copy.h"

#include <cstddef>
#include <cstdint>

namespace ps1::gpu::replay {
namespace {

bool isZero(const VulkanReplayBakedRect &rect) noexcept {
  return rect == VulkanReplayBakedRect{};
}

bool isBounded(const VulkanReplayBakedRect &rect, uint32_t width,
               uint32_t height) noexcept {
  return rect.width != 0 && rect.height != 0 && rect.x < width &&
         rect.y < height && rect.width <= width - rect.x &&
         rect.height <= height - rect.y;
}

bool sameExtent(const VulkanReplayBakedRect &left,
                const VulkanReplayBakedRect &right) noexcept {
  return left.width == right.width && left.height == right.height;
}

bool isScaled(const VulkanReplayBakedRect &native,
              const VulkanReplayBakedRect &atlas, uint32_t scale) noexcept {
  return static_cast<uint64_t>(native.x) * scale == atlas.x &&
         static_cast<uint64_t>(native.y) * scale == atlas.y &&
         static_cast<uint64_t>(native.width) * scale == atlas.width &&
         static_cast<uint64_t>(native.height) * scale == atlas.height;
}

bool overlaps(const VulkanReplayBakedRect &left,
              const VulkanReplayBakedRect &right) noexcept {
  return static_cast<uint64_t>(left.x) + left.width > right.x &&
         static_cast<uint64_t>(right.x) + right.width > left.x &&
         static_cast<uint64_t>(left.y) + left.height > right.y &&
         static_cast<uint64_t>(right.y) + right.height > left.y;
}

bool validPlaneContract(const VulkanReplayBakedCopyRect &copy) noexcept {
  return copy.copyColorRgba8 && copy.copyMaskR8 && copy.invalidateDepthD32 &&
         copy.depthClearValue == 1.0f;
}

bool validNativeRead(const NativeReadDependency &read, uint32_t nativeWidth,
                     uint32_t nativeHeight) noexcept {
  const uint64_t maximumTiles =
      static_cast<uint64_t>(nativeWidth / CRASH_REPLAY_NATIVE_TILE_WIDTH) *
      (nativeHeight / CRASH_REPLAY_NATIVE_TILE_HEIGHT);
  return read.required && read.tileCount != 0 &&
         read.tileWidth == CRASH_REPLAY_NATIVE_TILE_WIDTH &&
         read.tileHeight == CRASH_REPLAY_NATIVE_TILE_HEIGHT &&
         read.tileCount <= maximumTiles;
}

uint32_t recoverWrappedOrigin(uint32_t logical, uint32_t mapped,
                              uint32_t extent) noexcept {
  return (mapped + extent - (logical % extent)) % extent;
}

bool mappedAxisMatches(uint32_t logical, uint32_t mapped, uint32_t length,
                       uint32_t origin, uint32_t extent) noexcept {
  return mapped == (origin + logical) % extent && mapped < extent &&
         length != 0 && length <= extent - mapped;
}

VulkanReplayCopyValidationReason
validateDirect(const VulkanReplayBakedCopyRect &copy,
               const VulkanReplayCopyExecutionLimits &limits) noexcept {
  if (!validPlaneContract(copy) || !copy.copyNativeR16 ||
      copy.snapshotSourceNativeR16 || copy.snapshotSourceColorRgba8 ||
      copy.snapshotSourceMaskR8 || copy.snapshotDestinationMaskR8 ||
      copy.regionCount != 0 || !isZero(copy.scratchNativeExtent) ||
      !isZero(copy.scratchAtlasExtent) ||
      copy.nativeRead != NativeReadDependency{} || copy.mask != MaskState{}) {
    return VulkanReplayCopyValidationReason::InvalidPlaneContract;
  }
  if (!isBounded(copy.sourceNative, limits.nativeWidth, limits.nativeHeight) ||
      !isBounded(copy.destinationNative, limits.nativeWidth,
                 limits.nativeHeight) ||
      !isBounded(copy.sourceAtlas, limits.atlasWidth, limits.atlasHeight) ||
      !isBounded(copy.destinationAtlas, limits.atlasWidth,
                 limits.atlasHeight) ||
      !sameExtent(copy.sourceNative, copy.destinationNative) ||
      !sameExtent(copy.sourceAtlas, copy.destinationAtlas) ||
      !isScaled(copy.sourceNative, copy.sourceAtlas, limits.internalScale) ||
      !isScaled(copy.destinationNative, copy.destinationAtlas,
                limits.internalScale)) {
    return VulkanReplayCopyValidationReason::InvalidDirectRect;
  }
  if (overlaps(copy.sourceNative, copy.destinationNative) ||
      overlaps(copy.sourceAtlas, copy.destinationAtlas)) {
    return VulkanReplayCopyValidationReason::DirectOverlap;
  }
  return VulkanReplayCopyValidationReason::None;
}

VulkanReplayCopyValidationReason
validateScratch(const VulkanReplayBakedCopyRect &copy,
                const VulkanReplayCopyExecutionLimits &limits) noexcept {
  if (!validPlaneContract(copy) || copy.copyNativeR16 ||
      copy.snapshotSourceNativeR16 || !copy.snapshotSourceColorRgba8 ||
      !copy.snapshotSourceMaskR8 ||
      copy.snapshotDestinationMaskR8 != copy.mask.checkMaskBit ||
      !isZero(copy.sourceNative) || !isZero(copy.destinationNative) ||
      !isZero(copy.sourceAtlas) || !isZero(copy.destinationAtlas)) {
    return VulkanReplayCopyValidationReason::InvalidPlaneContract;
  }
  if (!validNativeRead(copy.nativeRead, limits.nativeWidth,
                       limits.nativeHeight)) {
    return VulkanReplayCopyValidationReason::InvalidNativeRead;
  }
  if (copy.scratchNativeExtent.x != 0 || copy.scratchNativeExtent.y != 0 ||
      !isBounded(copy.scratchNativeExtent, limits.nativeWidth,
                 limits.nativeHeight) ||
      copy.scratchAtlasExtent.x != 0 || copy.scratchAtlasExtent.y != 0 ||
      !isBounded(copy.scratchAtlasExtent, limits.atlasWidth,
                 limits.atlasHeight) ||
      !isScaled(copy.scratchNativeExtent, copy.scratchAtlasExtent,
                limits.internalScale)) {
    return VulkanReplayCopyValidationReason::InvalidScratchExtent;
  }
  if (copy.regionCount == 0 || copy.regionCount > copy.regions.size())
    return VulkanReplayCopyValidationReason::InvalidScratchRegionCount;

  const VulkanReplayBakedCopyRegion &first = copy.regions[0];
  const uint32_t sourceOriginX = recoverWrappedOrigin(
      first.scratchNative.x, first.sourceNative.x, limits.nativeWidth);
  const uint32_t sourceOriginY = recoverWrappedOrigin(
      first.scratchNative.y, first.sourceNative.y, limits.nativeHeight);
  const uint32_t destinationOriginX = recoverWrappedOrigin(
      first.scratchNative.x, first.destinationNative.x, limits.nativeWidth);
  const uint32_t destinationOriginY = recoverWrappedOrigin(
      first.scratchNative.y, first.destinationNative.y, limits.nativeHeight);

  uint64_t coveredPixels = 0;
  for (uint32_t index = 0; index < copy.regionCount; ++index) {
    const VulkanReplayBakedCopyRegion &region = copy.regions[index];
    if (!isBounded(region.scratchNative, copy.scratchNativeExtent.width,
                   copy.scratchNativeExtent.height) ||
        !isBounded(region.sourceNative, limits.nativeWidth,
                   limits.nativeHeight) ||
        !isBounded(region.destinationNative, limits.nativeWidth,
                   limits.nativeHeight) ||
        !isBounded(region.scratchAtlas, copy.scratchAtlasExtent.width,
                   copy.scratchAtlasExtent.height) ||
        !isBounded(region.sourceAtlas, limits.atlasWidth, limits.atlasHeight) ||
        !isBounded(region.destinationAtlas, limits.atlasWidth,
                   limits.atlasHeight) ||
        !sameExtent(region.scratchNative, region.sourceNative) ||
        !sameExtent(region.scratchNative, region.destinationNative)) {
      return VulkanReplayCopyValidationReason::InvalidScratchRegion;
    }
    if (!isScaled(region.scratchNative, region.scratchAtlas,
                  limits.internalScale) ||
        !isScaled(region.sourceNative, region.sourceAtlas,
                  limits.internalScale) ||
        !isScaled(region.destinationNative, region.destinationAtlas,
                  limits.internalScale) ||
        !sameExtent(region.scratchAtlas, region.sourceAtlas) ||
        !sameExtent(region.scratchAtlas, region.destinationAtlas)) {
      return VulkanReplayCopyValidationReason::ScratchScaleMismatch;
    }
    if (!mappedAxisMatches(region.scratchNative.x, region.sourceNative.x,
                           region.scratchNative.width, sourceOriginX,
                           limits.nativeWidth) ||
        !mappedAxisMatches(region.scratchNative.y, region.sourceNative.y,
                           region.scratchNative.height, sourceOriginY,
                           limits.nativeHeight) ||
        !mappedAxisMatches(region.scratchNative.x, region.destinationNative.x,
                           region.scratchNative.width, destinationOriginX,
                           limits.nativeWidth) ||
        !mappedAxisMatches(region.scratchNative.y, region.destinationNative.y,
                           region.scratchNative.height, destinationOriginY,
                           limits.nativeHeight)) {
      return VulkanReplayCopyValidationReason::ScratchMapping;
    }
    for (uint32_t prior = 0; prior < index; ++prior) {
      if (overlaps(region.scratchNative, copy.regions[prior].scratchNative)) {
        return VulkanReplayCopyValidationReason::ScratchPartition;
      }
    }
    coveredPixels += static_cast<uint64_t>(region.scratchNative.width) *
                     region.scratchNative.height;
  }
  const uint64_t expectedPixels =
      static_cast<uint64_t>(copy.scratchNativeExtent.width) *
      copy.scratchNativeExtent.height;
  if (coveredPixels != expectedPixels)
    return VulkanReplayCopyValidationReason::ScratchPartition;

  for (std::size_t index = copy.regionCount; index < copy.regions.size();
       ++index) {
    if (copy.regions[index] != VulkanReplayBakedCopyRegion{})
      return VulkanReplayCopyValidationReason::InvalidScratchRegionCount;
  }
  return VulkanReplayCopyValidationReason::None;
}

} // namespace

const char *vulkanReplayCopyValidationReasonName(
    VulkanReplayCopyValidationReason reason) noexcept {
  switch (reason) {
  case VulkanReplayCopyValidationReason::None:
    return "none";
  case VulkanReplayCopyValidationReason::InvalidLimits:
    return "invalid-limits";
  case VulkanReplayCopyValidationReason::UnsupportedPath:
    return "unsupported-path";
  case VulkanReplayCopyValidationReason::InvalidPlaneContract:
    return "invalid-plane-contract";
  case VulkanReplayCopyValidationReason::InvalidNativeRead:
    return "invalid-native-read";
  case VulkanReplayCopyValidationReason::InvalidDirectRect:
    return "invalid-direct-rect";
  case VulkanReplayCopyValidationReason::DirectOverlap:
    return "direct-overlap";
  case VulkanReplayCopyValidationReason::InvalidScratchExtent:
    return "invalid-scratch-extent";
  case VulkanReplayCopyValidationReason::InvalidScratchRegionCount:
    return "invalid-scratch-region-count";
  case VulkanReplayCopyValidationReason::InvalidScratchRegion:
    return "invalid-scratch-region";
  case VulkanReplayCopyValidationReason::ScratchScaleMismatch:
    return "scratch-scale-mismatch";
  case VulkanReplayCopyValidationReason::ScratchPartition:
    return "scratch-partition";
  case VulkanReplayCopyValidationReason::ScratchMapping:
    return "scratch-mapping";
  }
  return "unknown";
}

VulkanReplayCopyValidationReason validateVulkanReplayCopyForExecution(
    const VulkanReplayBakedCopyRect &copy,
    const VulkanReplayCopyExecutionLimits &limits) noexcept {
  if (limits.internalScale == 0 || limits.nativeWidth == 0 ||
      limits.nativeHeight == 0 || limits.atlasWidth == 0 ||
      limits.atlasHeight == 0 ||
      static_cast<uint64_t>(limits.nativeWidth) * limits.internalScale !=
          limits.atlasWidth ||
      static_cast<uint64_t>(limits.nativeHeight) * limits.internalScale !=
          limits.atlasHeight) {
    return VulkanReplayCopyValidationReason::InvalidLimits;
  }

  switch (copy.path) {
  case VulkanReplayCopyPath::DirectNonOverlap:
    return validateDirect(copy, limits);
  case VulkanReplayCopyPath::ScratchSnapshot:
    return validateScratch(copy, limits);
  }
  return VulkanReplayCopyValidationReason::UnsupportedPath;
}

vulkan_abi::ScratchCopyPushConstants makeVulkanReplayScratchCopyPushConstants(
    const VulkanReplayBakedCopyRegion &region, const MaskState &mask) noexcept {
  return {{region.scratchAtlas.x, region.scratchAtlas.y,
           region.destinationAtlas.x, region.destinationAtlas.y},
          {region.scratchAtlas.width, region.scratchAtlas.height,
           mask.forceMaskBit ? 1u : 0u, mask.checkMaskBit ? 1u : 0u}};
}

} // namespace ps1::gpu::replay
