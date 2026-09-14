#include "runtime/gpu/modern_replay_vulkan_bake.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ps1::gpu::replay {
namespace {

constexpr uint32_t NATIVE_WIDTH = 1024;
constexpr uint32_t NATIVE_HEIGHT = 512;

struct BakeTotals {
  std::size_t commands = 0;
  std::size_t vertices = 0;
  std::size_t uploadPixels = 0;
  std::size_t visibilityOwners = 0;
};

VulkanReplayBakeResult failure(VulkanReplayBakeReason reason) {
  VulkanReplayBakeResult result;
  result.reason = reason;
  return result;
}

VulkanReplayBakeResult failure(VulkanReplayBakeReason reason,
                               const ReplayExecutionOperation &item) {
  VulkanReplayBakeResult result;
  result.reason = reason;
  result.sourceOrdinal = item.ordinal;
  result.sourceKind = item.kind;
  return result;
}

bool checkedAdd(std::size_t left, std::size_t right,
                std::size_t &result) noexcept {
  if (right > std::numeric_limits<std::size_t>::max() - left)
    return false;
  result = left + right;
  return true;
}

bool checkedMultiply(std::size_t left, std::size_t right,
                     std::size_t &result) noexcept {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left)
    return false;
  result = left * right;
  return true;
}

VulkanReplayBakeReason accountNativeUploadPixels(std::size_t pixelCount,
                                                 BakeTotals &totals) noexcept {
  std::size_t alignedOffset = totals.uploadPixels;
  if ((alignedOffset & 1u) != 0u &&
      !checkedAdd(alignedOffset, 1, alignedOffset)) {
    return VulkanReplayBakeReason::NumericOverflow;
  }
  std::size_t next = 0;
  if (!checkedAdd(alignedOffset, pixelCount, next))
    return VulkanReplayBakeReason::NumericOverflow;
  totals.uploadPixels = next;
  return VulkanReplayBakeReason::None;
}

bool fitsFloat(double value) noexcept {
  constexpr double maximum =
      static_cast<double>(std::numeric_limits<float>::max());
  return std::isfinite(value) && value >= -maximum && value <= maximum;
}

bool validBlendMode(BlendMode mode) noexcept {
  return static_cast<uint8_t>(mode) <=
         static_cast<uint8_t>(BlendMode::BackgroundPlusQuarterForeground);
}

uint32_t wrapNativeCoordinate(int64_t value, uint32_t extent) noexcept {
  int64_t wrapped = value % extent;
  if (wrapped < 0)
    wrapped += extent;
  return static_cast<uint32_t>(wrapped);
}

struct CpuUploadRunSpan {
  std::size_t sourceChunkOffset = 0;
  uint32_t destinationX = 0;
  uint32_t destinationY = 0;
  uint32_t length = 0;
};

bool cpuUploadAcceptedAt(const CpuUpload &upload,
                         std::size_t chunkIndex) noexcept {
  return ((upload.acceptedBits[chunkIndex / 64u] >> (chunkIndex % 64u)) & 1u) !=
         0;
}

NativePoint cpuUploadDestinationAt(const CpuUpload &upload,
                                   std::size_t chunkIndex) noexcept {
  const uint64_t linear = upload.pixelOffset + chunkIndex;
  const uint64_t column = linear % upload.transfer.width;
  const uint64_t row = linear / upload.transfer.width;
  return {
      static_cast<int32_t>(
          wrapNativeCoordinate(static_cast<int64_t>(upload.transfer.x) +
                                   static_cast<int64_t>(column),
                               NATIVE_WIDTH)),
      static_cast<int32_t>(wrapNativeCoordinate(
          static_cast<int64_t>(upload.transfer.y) + static_cast<int64_t>(row),
          NATIVE_HEIGHT))};
}

CpuUploadRunSpan cpuUploadRunAt(const CpuUpload &upload,
                                std::size_t sourceChunkOffset) noexcept {
  const NativePoint first = cpuUploadDestinationAt(upload, sourceChunkOffset);
  CpuUploadRunSpan run{sourceChunkOffset, static_cast<uint32_t>(first.x),
                       static_cast<uint32_t>(first.y), 1};
  const uint64_t transferRow =
      (upload.pixelOffset + sourceChunkOffset) / upload.transfer.width;
  while (sourceChunkOffset + run.length < upload.pixels.size()) {
    const std::size_t nextChunkOffset = sourceChunkOffset + run.length;
    if (!cpuUploadAcceptedAt(upload, nextChunkOffset) ||
        (upload.pixelOffset + nextChunkOffset) / upload.transfer.width !=
            transferRow) {
      break;
    }
    const NativePoint next = cpuUploadDestinationAt(upload, nextChunkOffset);
    if (next.y != static_cast<int32_t>(run.destinationY) ||
        next.x != static_cast<int32_t>(run.destinationX + run.length)) {
      break;
    }
    ++run.length;
  }
  return run;
}

uint32_t wrappedNativeReadTileCount(const CopyRect &copy) noexcept {
  const auto axisTileCount = [](int32_t origin, uint32_t length,
                                uint32_t extent, uint32_t tileExtent) noexcept {
    const uint32_t wrapped = wrapNativeCoordinate(origin, extent);
    const uint32_t tileOffset = wrapped % tileExtent;
    const uint32_t spanned =
        (tileOffset + length + tileExtent - 1u) / tileExtent;
    return std::min(extent / tileExtent, spanned);
  };
  return axisTileCount(copy.source.x, copy.width, NATIVE_WIDTH,
                       CRASH_REPLAY_NATIVE_TILE_WIDTH) *
         axisTileCount(copy.source.y, copy.height, NATIVE_HEIGHT,
                       CRASH_REPLAY_NATIVE_TILE_HEIGHT);
}

struct AxisRegion {
  uint32_t source = 0;
  uint32_t destination = 0;
  uint32_t length = 0;
};

std::array<AxisRegion, 2> splitNativeAxis(int32_t origin, uint32_t length,
                                          uint32_t extent) noexcept {
  std::array<AxisRegion, 2> regions{};
  const uint32_t wrapped = wrapNativeCoordinate(origin, extent);
  const uint32_t first = std::min(length, extent - wrapped);
  regions[0] = {0, wrapped, first};
  if (first < length)
    regions[1] = {first, 0, length - first};
  return regions;
}

uint32_t blendModeIndex(BlendMode mode) noexcept {
  switch (mode) {
  case BlendMode::HalfBackgroundPlusHalfForeground:
    return 0;
  case BlendMode::BackgroundPlusForeground:
    return 1;
  case BlendMode::BackgroundMinusForeground:
    return 2;
  case BlendMode::BackgroundPlusQuarterForeground:
    return 3;
  }
  return 0;
}

VulkanReplayBakedRect fullScaledSurface(uint32_t scale) noexcept {
  return {0, 0, NATIVE_WIDTH * scale, NATIVE_HEIGHT * scale};
}

VulkanReplayBakedRect scaledDrawArea(const DrawArea &area,
                                     uint32_t scale) noexcept {
  const int64_t left = std::max<int64_t>(area.left, 0);
  const int64_t top = std::max<int64_t>(area.top, 0);
  const int64_t right =
      std::min<int64_t>(area.right, static_cast<int64_t>(NATIVE_WIDTH) - 1);
  const int64_t bottom =
      std::min<int64_t>(area.bottom, static_cast<int64_t>(NATIVE_HEIGHT) - 1);
  if (left > right || top > bottom)
    return {};

  return {static_cast<uint32_t>(left) * scale,
          static_cast<uint32_t>(top) * scale,
          static_cast<uint32_t>(right - left + 1) * scale,
          static_cast<uint32_t>(bottom - top + 1) * scale};
}

VulkanReplayBakedRect scaledNativeRect(const NativeRect &area,
                                       uint32_t scale) noexcept {
  return {static_cast<uint32_t>(area.x) * scale,
          static_cast<uint32_t>(area.y) * scale, area.width * scale,
          area.height * scale};
}

VulkanReplayBakeReason validateNativeUpload(const NativeTileUpload &upload,
                                            BakeTotals &totals) noexcept {
  if (upload.destination.width == 0 || upload.destination.height == 0)
    return VulkanReplayBakeReason::InvalidNativeTileUpload;

  if (upload.destination.width > NATIVE_WIDTH ||
      upload.destination.height > NATIVE_HEIGHT ||
      upload.destination.width % CRASH_REPLAY_NATIVE_TILE_WIDTH != 0 ||
      upload.destination.height % CRASH_REPLAY_NATIVE_TILE_HEIGHT != 0 ||
      wrapNativeCoordinate(upload.destination.x, NATIVE_WIDTH) %
              CRASH_REPLAY_NATIVE_TILE_WIDTH !=
          0 ||
      wrapNativeCoordinate(upload.destination.y, NATIVE_HEIGHT) %
              CRASH_REPLAY_NATIVE_TILE_HEIGHT !=
          0) {
    return VulkanReplayBakeReason::InvalidNativeTileUpload;
  }

  const std::size_t width = upload.destination.width;
  const std::size_t height = upload.destination.height;
  std::size_t pixelCount = 0;
  if (!checkedMultiply(width, height, pixelCount))
    return VulkanReplayBakeReason::NumericOverflow;
  if (upload.pixels.size() != pixelCount)
    return VulkanReplayBakeReason::InvalidNativeTileUpload;

  return accountNativeUploadPixels(pixelCount, totals);
}

VulkanReplayBakeReason validatePaintRect(const ReplayExecutionOperation &item,
                                         const PaintRect &paint,
                                         uint64_t establishedDepthEpoch,
                                         BakeTotals &totals) noexcept {
  const NativeRect &destination = paint.destination;
  if (destination.width == 0 || destination.height == 0 || destination.x < 0 ||
      destination.y < 0) {
    return VulkanReplayBakeReason::InvalidPaintRect;
  }

  const uint32_t x = static_cast<uint32_t>(destination.x);
  const uint32_t y = static_cast<uint32_t>(destination.y);
  if (x >= NATIVE_WIDTH || y >= NATIVE_HEIGHT ||
      destination.width > NATIVE_WIDTH - x ||
      destination.height > NATIVE_HEIGHT - y) {
    return VulkanReplayBakeReason::InvalidPaintRect;
  }

  std::size_t pixelCount = 0;
  if (!checkedMultiply(static_cast<std::size_t>(destination.width),
                       static_cast<std::size_t>(destination.height),
                       pixelCount)) {
    return VulkanReplayBakeReason::NumericOverflow;
  }
  if (paint.coalescedCommandCount != static_cast<uint64_t>(pixelCount))
    return VulkanReplayBakeReason::InvalidPaintRect;

  if (paint.mask.checkMaskBit || item.access.nativeRead ||
      !item.access.nativeWrite || item.access.colorRead ||
      !item.access.colorWrite || item.access.maskRead ||
      !item.access.maskWrite || item.access.depthRead ||
      !item.access.depthWrite || item.hazards.destinationFeedbackBoundary ||
      item.hazards.destinationScratchRequired ||
      item.hazards.refreshDestinationBetweenTriangles ||
      item.hazards.copySourceScratchRequired ||
      item.hazards.fragmentAcceptanceControlsDepth ||
      !item.hazards.barrierBefore || !item.hazards.barrierAfter) {
    return VulkanReplayBakeReason::UnsupportedPaintRectState;
  }

  const DepthState &depth = paint.depth;
  if (!depth.invalidateOnAccepted || depth.testEnabled || depth.writeEnabled ||
      depth.compare != DepthCompare::Always || depth.epoch == 0 ||
      !item.depth.invalidateOnAccepted || item.depth.test || item.depth.write ||
      item.depth.clear || item.depth.donorEligible ||
      item.depth.requestedTest || item.depth.requestedWrite ||
      item.depth.compare != DepthCompare::Always ||
      item.depth.epoch != depth.epoch ||
      (establishedDepthEpoch != 0 && depth.epoch != establishedDepthEpoch)) {
    return VulkanReplayBakeReason::InvalidDepthState;
  }

  return accountNativeUploadPixels(pixelCount, totals);
}

VulkanReplayBakeReason validatePaintPixels(
    const ReplayExecutionOperation &item, const PaintPixels &paint,
    uint64_t establishedDepthEpoch, VulkanReplayBakeFeatures features,
    BakeTotals &totals) noexcept {
  if (!features.allowPaintPixels)
    return VulkanReplayBakeReason::PaintPixelsDisabled;
  if (paint.pixels.empty())
    return VulkanReplayBakeReason::InvalidPaintPixels;
  if (!features.allowScreen2D)
    return VulkanReplayBakeReason::Screen2DDrawDisabled;
  if (!features.allowDepthInvalidate)
    return VulkanReplayBakeReason::DepthInvalidateDisabled;
  if (paint.blend.enabled && !features.allowSemiTransparent)
    return VulkanReplayBakeReason::SemiTransparentDrawDisabled;
  if (paint.blend.enabled && !features.allowDestinationColorFeedback)
    return VulkanReplayBakeReason::DestinationColorFeedbackDisabled;
  if (!validBlendMode(paint.blend.mode))
    return VulkanReplayBakeReason::UnsupportedPaintPixelsState;

  if (item.access.nativeRead || !item.access.nativeWrite ||
      item.access.colorRead != paint.blend.enabled ||
      !item.access.colorWrite || item.access.maskRead ||
      !item.access.maskWrite || item.access.depthRead ||
      !item.access.depthWrite ||
      item.hazards.destinationFeedbackBoundary != paint.blend.enabled ||
      item.hazards.destinationScratchRequired != paint.blend.enabled ||
      item.hazards.refreshDestinationBetweenTriangles ||
      item.hazards.copySourceScratchRequired ||
      item.hazards.fragmentAcceptanceControlsDepth ||
      !item.hazards.barrierBefore || !item.hazards.barrierAfter) {
    return VulkanReplayBakeReason::UnsupportedPaintPixelsState;
  }

  const DepthState &depth = paint.depth;
  if (!depth.invalidateOnAccepted || depth.testEnabled || depth.writeEnabled ||
      depth.compare != DepthCompare::Always || depth.epoch == 0 ||
      !item.depth.invalidateOnAccepted || item.depth.test || item.depth.write ||
      item.depth.clear || item.depth.donorEligible ||
      item.depth.requestedTest || item.depth.requestedWrite ||
      item.depth.compare != DepthCompare::Always ||
      item.depth.epoch != depth.epoch ||
      (establishedDepthEpoch != 0 && depth.epoch != establishedDepthEpoch)) {
    return VulkanReplayBakeReason::InvalidDepthState;
  }

  std::size_t expandedCommandCount = 0;
  std::size_t vertexCount = 0;
  if (!checkedMultiply(paint.pixels.size(), 2u, expandedCommandCount) ||
      !checkedMultiply(paint.pixels.size(), 3u, vertexCount)) {
    return VulkanReplayBakeReason::NumericOverflow;
  }
  // The source operation already occupies one entry in totals.commands.
  // Replace it with one native upload and one draw per accepted pixel.
  if (!checkedAdd(totals.commands, expandedCommandCount - 1u,
                  totals.commands) ||
      !checkedAdd(totals.vertices, vertexCount, totals.vertices) ||
      totals.vertices > std::numeric_limits<uint32_t>::max() ||
      !checkedAdd(totals.visibilityOwners, paint.pixels.size(),
                  totals.visibilityOwners) ||
      totals.visibilityOwners > std::numeric_limits<uint32_t>::max()) {
    return VulkanReplayBakeReason::NumericOverflow;
  }

  for (const PaintedPixel &pixel : paint.pixels) {
    if (pixel.destination.x < 0 || pixel.destination.y < 0 ||
        pixel.destination.x >= static_cast<int32_t>(NATIVE_WIDTH) ||
        pixel.destination.y >= static_cast<int32_t>(NATIVE_HEIGHT)) {
      return VulkanReplayBakeReason::InvalidPaintPixels;
    }
    const bool expectedMask = pixel.sourceMaskBit || paint.mask.forceMaskBit;
    if (((pixel.nativeColor & 0x8000u) != 0u) != expectedMask)
      return VulkanReplayBakeReason::InvalidPaintPixels;
    const VulkanReplayBakeReason uploadReason =
        accountNativeUploadPixels(1u, totals);
    if (uploadReason != VulkanReplayBakeReason::None)
      return uploadReason;
  }
  return VulkanReplayBakeReason::None;
}

uint16_t quantizeFillColor(Rgb8 color) noexcept {
  return static_cast<uint16_t>((color.r >> 3u) | ((color.g >> 3u) << 5u) |
                               ((color.b >> 3u) << 10u));
}

VulkanReplayBakeReason validateFillRect(const ReplayExecutionOperation &item,
                                        const FillRect &fill,
                                        BakeTotals &totals) noexcept {
  const NativeRect &destination = fill.destination;
  if (destination.width == 0 || destination.height == 0)
    return VulkanReplayBakeReason::InvalidFillRect;

  std::size_t expectedPixels = 0;
  if (!checkedMultiply(static_cast<std::size_t>(destination.width),
                       static_cast<std::size_t>(destination.height),
                       expectedPixels)) {
    return VulkanReplayBakeReason::NumericOverflow;
  }
  if (destination.width > NATIVE_WIDTH || destination.height > NATIVE_HEIGHT) {
    return VulkanReplayBakeReason::InvalidFillRect;
  }

  if ((fill.nativeColor & 0x8000u) != 0u)
    return VulkanReplayBakeReason::UnsupportedFillRectState;
  if (fill.nativeColor != quantizeFillColor(fill.color))
    return VulkanReplayBakeReason::InvalidFillRect;

  if (item.access.nativeRead || !item.access.nativeWrite ||
      item.access.colorRead || !item.access.colorWrite ||
      item.access.maskRead || !item.access.maskWrite || item.access.depthRead ||
      !item.access.depthWrite || item.hazards.destinationFeedbackBoundary ||
      item.hazards.destinationScratchRequired ||
      item.hazards.refreshDestinationBetweenTriangles ||
      item.hazards.copySourceScratchRequired ||
      item.hazards.fragmentAcceptanceControlsDepth ||
      !item.hazards.barrierBefore || !item.hazards.barrierAfter ||
      !item.depth.invalidateOnAccepted || item.depth.test || item.depth.write ||
      item.depth.clear || item.depth.donorEligible ||
      item.depth.requestedTest || item.depth.requestedWrite ||
      item.depth.compare != DepthCompare::Always || item.depth.epoch != 0) {
    return VulkanReplayBakeReason::UnsupportedFillRectState;
  }

  const auto xRegions =
      splitNativeAxis(destination.x, destination.width, NATIVE_WIDTH);
  const auto yRegions =
      splitNativeAxis(destination.y, destination.height, NATIVE_HEIGHT);
  std::size_t regionPixelsTotal = 0;
  for (const AxisRegion &y : yRegions) {
    if (y.length == 0)
      continue;
    for (const AxisRegion &x : xRegions) {
      if (x.length == 0)
        continue;
      std::size_t regionPixels = 0;
      if (!checkedMultiply(static_cast<std::size_t>(x.length),
                           static_cast<std::size_t>(y.length), regionPixels)) {
        return VulkanReplayBakeReason::NumericOverflow;
      }
      std::size_t nextRegionTotal = 0;
      if (!checkedAdd(regionPixelsTotal, regionPixels, nextRegionTotal))
        return VulkanReplayBakeReason::NumericOverflow;
      regionPixelsTotal = nextRegionTotal;
      const VulkanReplayBakeReason accountReason =
          accountNativeUploadPixels(regionPixels, totals);
      if (accountReason != VulkanReplayBakeReason::None)
        return accountReason;
    }
  }
  return regionPixelsTotal == expectedPixels
             ? VulkanReplayBakeReason::None
             : VulkanReplayBakeReason::NumericOverflow;
}

bool nativeCopyRectsOverlap(const CopyRect &copy) noexcept {
  // validateCopyRect has already proved that these additions remain inside
  // the 1024x512 native surface.
  const uint32_t sourceX = static_cast<uint32_t>(copy.source.x);
  const uint32_t sourceY = static_cast<uint32_t>(copy.source.y);
  const uint32_t destinationX = static_cast<uint32_t>(copy.destination.x);
  const uint32_t destinationY = static_cast<uint32_t>(copy.destination.y);
  return sourceX < destinationX + copy.width &&
         destinationX < sourceX + copy.width &&
         sourceY < destinationY + copy.height &&
         destinationY < sourceY + copy.height;
}

bool nativeCopyRectIsWrapped(const NativePoint &origin,
                             const CopyRect &copy) noexcept {
  if (origin.x < 0 || origin.y < 0)
    return true;
  const uint32_t x = static_cast<uint32_t>(origin.x);
  const uint32_t y = static_cast<uint32_t>(origin.y);
  return x >= NATIVE_WIDTH || y >= NATIVE_HEIGHT ||
         copy.width > NATIVE_WIDTH - x || copy.height > NATIVE_HEIGHT - y;
}

bool wrappedAxisRangesOverlap(int32_t sourceOrigin, int32_t destinationOrigin,
                              uint32_t length, uint32_t extent) noexcept {
  const auto source = splitNativeAxis(sourceOrigin, length, extent);
  const auto destination = splitNativeAxis(destinationOrigin, length, extent);
  for (const AxisRegion &sourceRegion : source) {
    if (sourceRegion.length == 0)
      continue;
    for (const AxisRegion &destinationRegion : destination) {
      if (destinationRegion.length == 0)
        continue;
      if (sourceRegion.destination <
              destinationRegion.destination + destinationRegion.length &&
          destinationRegion.destination <
              sourceRegion.destination + sourceRegion.length) {
        return true;
      }
    }
  }
  return false;
}

bool horizontalCopyReadsPriorWrite(const CopyRect &copy) noexcept {
  std::array<bool, NATIVE_WIDTH> written{};
  const uint32_t sourceX = wrapNativeCoordinate(copy.source.x, NATIVE_WIDTH);
  const uint32_t destinationX =
      wrapNativeCoordinate(copy.destination.x, NATIVE_WIDTH);
  const bool backwards = sourceX < destinationX ||
                         ((sourceX + copy.width - 1u) % NATIVE_WIDTH) <
                             ((destinationX + copy.width - 1u) % NATIVE_WIDTH);
  for (uint32_t step = 0; step < copy.width; ++step) {
    const uint32_t logicalX = backwards ? copy.width - 1u - step : step;
    const uint32_t source = (sourceX + logicalX) % NATIVE_WIDTH;
    if (written[source])
      return true;
    written[(destinationX + logicalX) % NATIVE_WIDTH] = true;
  }
  return false;
}

bool copyRectHasUnprovenOrderedOverlap(const CopyRect &copy) noexcept {
  if (!wrappedAxisRangesOverlap(copy.source.x, copy.destination.x, copy.width,
                                NATIVE_WIDTH)) {
    return false;
  }

  const uint32_t sourceY = wrapNativeCoordinate(copy.source.y, NATIVE_HEIGHT);
  const uint32_t destinationY =
      wrapNativeCoordinate(copy.destination.y, NATIVE_HEIGHT);
  if (sourceY == destinationY)
    return horizontalCopyReadsPriorWrite(copy);

  // Native and SwanStation software execute rows top-to-bottom. Prove the
  // actual wrapped read/write order instead of rejecting every vertical
  // overlap: MGS texture scrolling moves rows upward and never reads a row
  // it has already overwritten. Downward feedback remains rejected.
  std::array<bool, NATIVE_HEIGHT> written{};
  for (uint32_t row = 0; row < copy.height; ++row) {
    if (written[(sourceY + row) % NATIVE_HEIGHT])
      return true;
    written[(destinationY + row) % NATIVE_HEIGHT] = true;
  }
  return false;
}

bool copyRectNeedsScratch(const CopyRect &copy) noexcept {
  return copy.mask.checkMaskBit || copy.mask.forceMaskBit ||
         nativeCopyRectIsWrapped(copy.source, copy) ||
         nativeCopyRectIsWrapped(copy.destination, copy) ||
         nativeCopyRectsOverlap(copy);
}

VulkanReplayBakeReason
validateCopyRect(const ReplayExecutionOperation &item, const CopyRect &copy,
                 const VulkanReplayBakeFeatures &features) noexcept {
  if (copy.width == 0 || copy.height == 0 || copy.width > NATIVE_WIDTH ||
      copy.height > NATIVE_HEIGHT) {
    return VulkanReplayBakeReason::InvalidCopyRect;
  }

  if (!copy.nativeRead.required || copy.nativeRead.tileCount == 0 ||
      copy.nativeRead.tileWidth != CRASH_REPLAY_NATIVE_TILE_WIDTH ||
      copy.nativeRead.tileHeight != CRASH_REPLAY_NATIVE_TILE_HEIGHT ||
      copy.nativeRead.tileCount != wrappedNativeReadTileCount(copy) ||
      !item.access.nativeRead || !item.access.nativeWrite ||
      !item.access.colorRead || !item.access.colorWrite ||
      !item.access.maskRead || !item.access.maskWrite ||
      item.access.depthRead || !item.access.depthWrite ||
      item.hazards.destinationFeedbackBoundary != copy.mask.checkMaskBit ||
      item.hazards.destinationScratchRequired != copy.mask.checkMaskBit ||
      item.hazards.refreshDestinationBetweenTriangles ||
      item.hazards.fragmentAcceptanceControlsDepth != copy.mask.checkMaskBit ||
      !item.hazards.barrierBefore || !item.hazards.barrierAfter ||
      !item.depth.invalidateOnAccepted || item.depth.test || item.depth.write ||
      item.depth.clear || item.depth.donorEligible ||
      item.depth.requestedTest || item.depth.requestedWrite ||
      item.depth.compare != DepthCompare::Always || item.depth.epoch != 0) {
    return VulkanReplayBakeReason::UnsupportedCopyRectState;
  }

  if (copyRectHasUnprovenOrderedOverlap(copy))
    return VulkanReplayBakeReason::CopyRectOrderedOverlapUnproven;

  // The execution plan conservatively marks every CopyRect as needing a
  // source scratch. The bake may ignore that bit only after proving the
  // narrower legacy direct path; every general path requires it explicitly.
  if (copyRectNeedsScratch(copy)) {
    if (!item.hazards.copySourceScratchRequired)
      return VulkanReplayBakeReason::UnsupportedCopyRectState;
    if (!features.allowCopyRectScratch)
      return VulkanReplayBakeReason::CopyRectScratchRequired;
  }

  return VulkanReplayBakeReason::None;
}

struct CopyAxisRegion {
  uint32_t logical = 0;
  uint32_t source = 0;
  uint32_t destination = 0;
  uint32_t length = 0;
};

struct CopyAxisRegions {
  std::array<CopyAxisRegion, 3> regions{};
  uint8_t count = 0;
};

CopyAxisRegions splitCopyAxis(int32_t sourceOrigin, int32_t destinationOrigin,
                              uint32_t length, uint32_t extent) noexcept {
  const uint32_t source = wrapNativeCoordinate(sourceOrigin, extent);
  const uint32_t destination = wrapNativeCoordinate(destinationOrigin, extent);

  std::array<uint32_t, 4> breakpoints{0, length, length, length};
  std::size_t breakpointCount = 2;
  const uint32_t sourceWrap = extent - source;
  const uint32_t destinationWrap = extent - destination;
  if (sourceWrap < length)
    breakpoints[breakpointCount++] = sourceWrap;
  if (destinationWrap < length)
    breakpoints[breakpointCount++] = destinationWrap;
  std::sort(breakpoints.begin(), breakpoints.begin() + breakpointCount);
  breakpointCount = static_cast<std::size_t>(
      std::unique(breakpoints.begin(), breakpoints.begin() + breakpointCount) -
      breakpoints.begin());

  CopyAxisRegions result;
  for (std::size_t index = 1; index < breakpointCount; ++index) {
    const uint32_t begin = breakpoints[index - 1];
    const uint32_t end = breakpoints[index];
    if (begin == end)
      continue;
    CopyAxisRegion &region = result.regions[result.count++];
    region.logical = begin;
    region.source = (source + begin) % extent;
    region.destination = (destination + begin) % extent;
    region.length = end - begin;
  }
  return result;
}

VulkanReplayBakedRect scaleCopyRect(VulkanReplayBakedRect rect,
                                    uint32_t internalScale) noexcept {
  rect.x *= internalScale;
  rect.y *= internalScale;
  rect.width *= internalScale;
  rect.height *= internalScale;
  return rect;
}

VulkanReplayBakedCopyRect bakeScratchCopyRect(const CopyRect &copy,
                                              uint32_t internalScale) noexcept {
  VulkanReplayBakedCopyRect baked;
  baked.path = VulkanReplayCopyPath::ScratchSnapshot;
  baked.nativeRead = copy.nativeRead;
  baked.mask = copy.mask;
  baked.scratchNativeExtent = {0, 0, copy.width, copy.height};
  baked.scratchAtlasExtent =
      scaleCopyRect(baked.scratchNativeExtent, internalScale);
  // Native R16 is a read-through cache, not the presented modern surface.
  // Every later native reader is preceded by a producer-owned tile upload,
  // so a masked/forced scratch copy must not consult a potentially stale
  // destination R16 merely to keep this cache eager.
  baked.snapshotSourceNativeR16 = false;
  baked.snapshotSourceColorRgba8 = true;
  baked.snapshotSourceMaskR8 = true;
  baked.snapshotDestinationMaskR8 = copy.mask.checkMaskBit;
  baked.copyNativeR16 = false;

  const CopyAxisRegions xRegions = splitCopyAxis(
      copy.source.x, copy.destination.x, copy.width, NATIVE_WIDTH);
  const CopyAxisRegions yRegions = splitCopyAxis(
      copy.source.y, copy.destination.y, copy.height, NATIVE_HEIGHT);
  for (uint8_t yIndex = 0; yIndex < yRegions.count; ++yIndex) {
    const CopyAxisRegion &y = yRegions.regions[yIndex];
    for (uint8_t xIndex = 0; xIndex < xRegions.count; ++xIndex) {
      const CopyAxisRegion &x = xRegions.regions[xIndex];
      VulkanReplayBakedCopyRegion &region = baked.regions[baked.regionCount++];
      region.scratchNative = {x.logical, y.logical, x.length, y.length};
      region.sourceNative = {x.source, y.source, x.length, y.length};
      region.destinationNative = {x.destination, y.destination, x.length,
                                  y.length};
      region.scratchAtlas = scaleCopyRect(region.scratchNative, internalScale);
      region.sourceAtlas = scaleCopyRect(region.sourceNative, internalScale);
      region.destinationAtlas =
          scaleCopyRect(region.destinationNative, internalScale);
    }
  }

  return baked;
}

VulkanReplayBakedCopyRect bakeCopyRect(const CopyRect &copy,
                                       uint32_t internalScale) noexcept {
  if (copyRectNeedsScratch(copy))
    return bakeScratchCopyRect(copy, internalScale);

  VulkanReplayBakedCopyRect baked;
  baked.sourceNative = {static_cast<uint32_t>(copy.source.x),
                        static_cast<uint32_t>(copy.source.y), copy.width,
                        copy.height};
  baked.destinationNative = {static_cast<uint32_t>(copy.destination.x),
                             static_cast<uint32_t>(copy.destination.y),
                             copy.width, copy.height};
  baked.sourceAtlas = scaleCopyRect(baked.sourceNative, internalScale);
  baked.destinationAtlas =
      scaleCopyRect(baked.destinationNative, internalScale);
  return baked;
}

VulkanReplayBakeReason validateCpuUpload(const ReplayExecutionOperation &item,
                                         const CpuUpload &upload,
                                         BakeTotals &totals) noexcept {
  if (upload.transfer.width == 0 || upload.transfer.height == 0 ||
      upload.transfer.width > NATIVE_WIDTH ||
      upload.transfer.height > NATIVE_HEIGHT ||
      upload.transactionId == UNASSIGNED_PACKET_SEQUENCE) {
    return VulkanReplayBakeReason::InvalidCpuUpload;
  }

  std::size_t transferPixelCount = 0;
  if (!checkedMultiply(static_cast<std::size_t>(upload.transfer.width),
                       static_cast<std::size_t>(upload.transfer.height),
                       transferPixelCount)) {
    return VulkanReplayBakeReason::NumericOverflow;
  }

  ReplayExecutionKind expectedKind = ReplayExecutionKind::CpuUploadBegin;
  switch (upload.phase) {
  case CpuUploadPhase::Begin:
    expectedKind = ReplayExecutionKind::CpuUploadBegin;
    if (upload.pixelOffset != 0 || !upload.pixels.empty() ||
        !upload.acceptedBits.empty()) {
      return VulkanReplayBakeReason::InvalidCpuUpload;
    }
    break;
  case CpuUploadPhase::Data:
    expectedKind = ReplayExecutionKind::CpuUploadData;
    if (upload.pixels.empty() || upload.pixelOffset > transferPixelCount ||
        upload.pixels.size() > transferPixelCount - upload.pixelOffset) {
      return VulkanReplayBakeReason::InvalidCpuUpload;
    }
    if (upload.acceptedBits.size() !=
        CpuUpload::acceptedWordCount(upload.pixels.size())) {
      return VulkanReplayBakeReason::InvalidCpuUpload;
    }
    if (const std::size_t tailBits = upload.pixels.size() % 64u;
        tailBits != 0 &&
        (upload.acceptedBits.back() & ~((uint64_t{1} << tailBits) - 1u)) != 0) {
      return VulkanReplayBakeReason::InvalidCpuUpload;
    }
    break;
  case CpuUploadPhase::Complete:
    expectedKind = ReplayExecutionKind::CpuUploadComplete;
    if (upload.pixelOffset != transferPixelCount || !upload.pixels.empty() ||
        !upload.acceptedBits.empty()) {
      return VulkanReplayBakeReason::InvalidCpuUpload;
    }
    break;
  case CpuUploadPhase::Abort:
    expectedKind = ReplayExecutionKind::CpuUploadAbort;
    if (upload.pixelOffset > transferPixelCount || !upload.pixels.empty() ||
        !upload.acceptedBits.empty()) {
      return VulkanReplayBakeReason::InvalidCpuUpload;
    }
    break;
  default:
    return VulkanReplayBakeReason::InvalidCpuUpload;
  }

  uint64_t acceptedPixelCount = 0;
  if (upload.phase == CpuUploadPhase::Data) {
    for (const uint64_t word : upload.acceptedBits)
      acceptedPixelCount += std::popcount(word);
  }
  const uint64_t consumedPixelCount = upload.pixels.size();
  if (acceptedPixelCount > consumedPixelCount)
    return VulkanReplayBakeReason::InvalidCpuUpload;
  const uint64_t rejectedPixelCount = consumedPixelCount - acceptedPixelCount;

  ReplayExecutionAccess expectedAccess;
  ReplayExecutionHazards expectedHazards;
  ReplayDepthExecution expectedDepth;
  ReplayA0Execution expectedA0;
  if (upload.phase == CpuUploadPhase::Data) {
    expectedA0.data = true;
    expectedA0.consumedPixels = consumedPixelCount;
    expectedA0.acceptedPixels = acceptedPixelCount;
    expectedA0.rejectedPixels = rejectedPixelCount;
    if (acceptedPixelCount != 0) {
      expectedAccess.nativeWrite = true;
      expectedAccess.colorWrite = true;
      expectedAccess.maskWrite = true;
      expectedAccess.depthWrite = true;
      expectedDepth.invalidateOnAccepted = true;
      expectedHazards.barrierBefore = true;
      expectedHazards.barrierAfter = true;
    }
  }
  if (item.kind != expectedKind || item.access != expectedAccess ||
      item.hazards != expectedHazards || item.depth != expectedDepth ||
      item.a0 != expectedA0 || item.draw != ReplayDrawExecution{}) {
    return VulkanReplayBakeReason::UnsupportedCpuUploadState;
  }

  std::size_t acceptedPixelsInRuns = 0;
  for (std::size_t sourceOffset = 0; sourceOffset < upload.pixels.size();) {
    if (!cpuUploadAcceptedAt(upload, sourceOffset)) {
      ++sourceOffset;
      continue;
    }
    const CpuUploadRunSpan run = cpuUploadRunAt(upload, sourceOffset);
    std::size_t nextAcceptedPixels = 0;
    if (run.length == 0 ||
        !checkedAdd(acceptedPixelsInRuns, run.length, nextAcceptedPixels)) {
      return VulkanReplayBakeReason::NumericOverflow;
    }
    acceptedPixelsInRuns = nextAcceptedPixels;
    const VulkanReplayBakeReason accountReason =
        accountNativeUploadPixels(run.length, totals);
    if (accountReason != VulkanReplayBakeReason::None)
      return accountReason;
    sourceOffset += run.length;
  }
  return acceptedPixelsInRuns == acceptedPixelCount
             ? VulkanReplayBakeReason::None
             : VulkanReplayBakeReason::InvalidCpuUpload;
}

VulkanReplayBakeReason validateDraw(const ReplayExecutionOperation &item,
                                    const DrawPacket &packet,
                                    const VulkanReplayBakeFeatures &features,
                                    BakeTotals &totals) noexcept {
  if (packet.vertexCount < 3 || packet.vertexCount > DrawPacket::MAX_VERTICES ||
      packet.indexCount == 0 || packet.indexCount > DrawPacket::MAX_INDICES ||
      (packet.indexCount % 3u) != 0 ||
      (packet.vertexCount == 3 && packet.indexCount != 3) ||
      (packet.vertexCount == 4 && packet.indexCount != 6)) {
    return VulkanReplayBakeReason::InvalidTriangleIndex;
  }
  for (std::size_t index = 0; index < packet.indexCount; ++index) {
    if (packet.triangleIndices[index] >= packet.vertexCount)
      return VulkanReplayBakeReason::InvalidTriangleIndex;
  }

  switch (packet.route) {
  case PacketRoute::Projected3D:
    if (packet.semanticReason != PacketSemanticReason::PolygonDefault)
      return VulkanReplayBakeReason::UnsupportedDrawRoute;
    break;
  case PacketRoute::Screen2D:
    if (packet.semanticReason == PacketSemanticReason::PolygonDefault)
      return VulkanReplayBakeReason::UnsupportedDrawRoute;
    if (!features.allowScreen2D)
      return VulkanReplayBakeReason::Screen2DDrawDisabled;
    break;
  default:
    return VulkanReplayBakeReason::UnsupportedDrawRoute;
  }

  if (packet.textured && !features.allowTextured)
    return VulkanReplayBakeReason::TexturedDrawDisabled;
  const bool semiTransparent = packet.state.blend.enabled;
  if (semiTransparent && !features.allowSemiTransparent)
    return VulkanReplayBakeReason::SemiTransparentDrawDisabled;
  if (semiTransparent && !features.allowDestinationColorFeedback)
    return VulkanReplayBakeReason::DestinationColorFeedbackDisabled;
  if (packet.state.mask.checkMaskBit && !features.allowDestinationMaskRead) {
    return VulkanReplayBakeReason::DestinationMaskReadDisabled;
  }

  const bool nativeFallback =
      features.allowNativeProjectionFallback &&
      packet.route == PacketRoute::Projected3D && packet.validMask == 0 &&
      packet.fallbackReason ==
          PacketFallbackReason::MissingPrecisionPacketAtomic &&
      !packet.originalMesh && !packet.state.depth.testEnabled &&
      !packet.state.depth.writeEnabled;
  if ((!nativeFallback &&
       packet.fallbackReason != PacketFallbackReason::None) ||
      (packet.textured && packet.state.texturePage.textureDisable) ||
      (!packet.textured && packet.rawTexture) ||
      packet.state.blend.texturedStpOnly !=
          (semiTransparent && packet.textured) ||
      packet.nativeRead.required != packet.textured) {
    return VulkanReplayBakeReason::UnsupportedDrawState;
  }

  const DepthState &depth = packet.state.depth;
  VulkanReplayDrawDepthPath depthPath =
      VulkanReplayDrawDepthPath::LessEqualWrite;
  if (depth.testEnabled && depth.writeEnabled &&
      depth.compare == DepthCompare::LessOrEqual &&
      !depth.invalidateOnAccepted) {
    depthPath = VulkanReplayDrawDepthPath::LessEqualWrite;
  } else if (!depth.testEnabled && depth.writeEnabled &&
             depth.compare == DepthCompare::Always &&
             !depth.invalidateOnAccepted) {
    depthPath = VulkanReplayDrawDepthPath::PainterWriteRealDepth;
  } else if (!depth.testEnabled && !depth.writeEnabled &&
             depth.compare == DepthCompare::Always &&
             !depth.invalidateOnAccepted) {
    depthPath = VulkanReplayDrawDepthPath::PainterOrdered;
  } else if (!depth.testEnabled && !depth.writeEnabled &&
             depth.compare == DepthCompare::Always &&
             depth.invalidateOnAccepted) {
    depthPath = VulkanReplayDrawDepthPath::InvalidateAcceptedFar;
  } else {
    return VulkanReplayBakeReason::InvalidDepthState;
  }
  if (depth.epoch == 0 || item.depth.epoch != depth.epoch ||
      item.depth.requestedTest != depth.testEnabled ||
      item.depth.requestedWrite != depth.writeEnabled || item.depth.clear ||
      item.depth.compare != depth.compare ||
      item.depth.donorEligible != donorTrueDepthEligible(packet)) {
    return VulkanReplayBakeReason::InvalidDepthState;
  }

  switch (depthPath) {
  case VulkanReplayDrawDepthPath::LessEqualWrite:
    if (packet.route != PacketRoute::Projected3D || semiTransparent ||
        !item.depth.donorEligible || !item.depth.test || !item.depth.write ||
        item.depth.invalidateOnAccepted || !item.access.depthRead ||
        !item.access.depthWrite) {
      return VulkanReplayBakeReason::InvalidDepthState;
    }
    break;
  case VulkanReplayDrawDepthPath::PainterWriteRealDepth:
    if (packet.route != PacketRoute::Projected3D || semiTransparent ||
        !item.depth.donorEligible || item.depth.test || !item.depth.write ||
        item.depth.invalidateOnAccepted || item.access.depthRead ||
        !item.access.depthWrite) {
      return VulkanReplayBakeReason::InvalidDepthState;
    }
    if (!features.allowPainterWriteRealDepth)
      return VulkanReplayBakeReason::PainterWriteRealDepthDisabled;
    break;
  case VulkanReplayDrawDepthPath::PainterOrdered:
    if (packet.route != PacketRoute::Projected3D)
      return VulkanReplayBakeReason::InvalidDepthState;
    if (!features.allowPainterOrdered)
      return VulkanReplayBakeReason::PainterOrderedDepthDisabled;
    if (item.depth.test || item.depth.write ||
        item.depth.invalidateOnAccepted || item.access.depthRead ||
        item.access.depthWrite) {
      return VulkanReplayBakeReason::InvalidDepthState;
    }
    break;
  case VulkanReplayDrawDepthPath::InvalidateAcceptedFar:
    if (packet.route != PacketRoute::Screen2D &&
        !(packet.route == PacketRoute::Projected3D && semiTransparent)) {
      return VulkanReplayBakeReason::InvalidDepthState;
    }
    if (!features.allowDepthInvalidate)
      return VulkanReplayBakeReason::DepthInvalidateDisabled;
    if (item.depth.donorEligible || item.depth.test || item.depth.write ||
        !item.depth.invalidateOnAccepted || item.access.depthRead ||
        !item.access.depthWrite) {
      return VulkanReplayBakeReason::InvalidDepthState;
    }
    break;
  }

  const ReplayBlendSelection expectedBlend =
      !semiTransparent  ? ReplayBlendSelection::None
      : packet.textured ? ReplayBlendSelection::SourceStp
                        : ReplayBlendSelection::WholePrimitive;
  const bool destinationFeedback =
      semiTransparent || packet.state.mask.checkMaskBit;
  const bool acceptanceControlsDepth =
      depthPath != VulkanReplayDrawDepthPath::PainterOrdered &&
      (packet.textured || packet.state.mask.checkMaskBit);
  const bool expectedBarrierBefore =
      packet.nativeRead.required || destinationFeedback;
  const bool expectedRefresh = destinationFeedback && packet.indexCount > 3;
  if (item.kind != (semiTransparent ? ReplayExecutionKind::DrawTranslucent
                                    : ReplayExecutionKind::DrawOpaque) ||
      item.draw.blendSelection != expectedBlend ||
      item.draw.textured != packet.textured ||
      item.draw.checkMask != packet.state.mask.checkMaskBit ||
      item.draw.forceMask != packet.state.mask.forceMaskBit ||
      item.draw.textureZeroCanDiscard != packet.textured ||
      item.draw.sourceStpWritesMask != packet.textured ||
      item.access.nativeRead != packet.nativeRead.required ||
      item.access.nativeWrite || item.access.colorRead != semiTransparent ||
      !item.access.colorWrite ||
      item.access.maskRead != packet.state.mask.checkMaskBit ||
      !item.access.maskWrite ||
      item.hazards.destinationFeedbackBoundary != destinationFeedback ||
      item.hazards.destinationScratchRequired != destinationFeedback ||
      item.hazards.refreshDestinationBetweenTriangles != expectedRefresh ||
      item.hazards.copySourceScratchRequired ||
      item.hazards.barrierBefore != expectedBarrierBefore ||
      item.hazards.barrierAfter != destinationFeedback ||
      item.hazards.fragmentAcceptanceControlsDepth != acceptanceControlsDepth) {
    return VulkanReplayBakeReason::UnsupportedDrawState;
  }

  const uint8_t fullMask =
      static_cast<uint8_t>((1u << packet.vertexCount) - 1u);
  if (packet.route == PacketRoute::Projected3D) {
    if (!nativeFallback && packet.validMask != fullMask)
      return VulkanReplayBakeReason::IncompletePrecisionProvenance;
  } else if (packet.validMask != 0) {
    return VulkanReplayBakeReason::IncompletePrecisionProvenance;
  }

  const TextureUvDomainState &uvDomain = packet.state.textureUvDomain;
  if (!std::isfinite(uvDomain.minU) || !std::isfinite(uvDomain.minV) ||
      !std::isfinite(uvDomain.maxU) || !std::isfinite(uvDomain.maxV)) {
    return VulkanReplayBakeReason::NonFiniteVertexAttribute;
  }
  if (!fitsFloat(uvDomain.minU) || !fitsFloat(uvDomain.minV) ||
      !fitsFloat(uvDomain.maxU) || !fitsFloat(uvDomain.maxV)) {
    return VulkanReplayBakeReason::NumericOverflow;
  }
  if (uvDomain.minU > uvDomain.maxU || uvDomain.minV > uvDomain.maxV ||
      uvDomain.enabled != packet.textured ||
      !validBlendMode(packet.state.blend.mode)) {
    return VulkanReplayBakeReason::UnsupportedDrawState;
  }

  for (std::size_t vertexIndex = 0; vertexIndex < packet.vertexCount;
       ++vertexIndex) {
    const ReplayVertex &vertex = packet.vertices[vertexIndex];
    if (vertex.coordinateSpace !=
            ReplayVertex::CoordinateSpace::PostDrawOffsetNativePixels ||
        (!nativeFallback && (vertex.reconciledX || vertex.reconciledY))) {
      return VulkanReplayBakeReason::IncompletePrecisionProvenance;
    }
    if (!vulkan_abi::tryNormalizeProjectiveWDepth(vertex.w).has_value())
      return VulkanReplayBakeReason::ProjectiveWOutOfDomain;
    if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
        !std::isfinite(vertex.u) || !std::isfinite(vertex.v) ||
        !std::isfinite(vertex.rawDepth) || !std::isfinite(vertex.nearDepth)) {
      return VulkanReplayBakeReason::NonFiniteVertexAttribute;
    }
    if (nativeFallback) {
      if (vertex.x != vertex.nativeX || vertex.y != vertex.nativeY ||
          vertex.w != CRASH_REPLAY_FALLBACK_W || vertex.preciseXY ||
          vertex.preciseX || vertex.preciseY)
        return VulkanReplayBakeReason::IncompletePrecisionProvenance;
    } else if (packet.route == PacketRoute::Projected3D) {
      if (!vertex.preciseXY || !vertex.preciseX || !vertex.preciseY ||
          !vertex.rawDepthValid || vertex.inventedW ||
          (packet.textured && !vertex.preciseUV)) {
        return VulkanReplayBakeReason::IncompletePrecisionProvenance;
      }
      const double effectiveW = std::max(vertex.rawDepth, vertex.nearDepth);
      if (vertex.w != effectiveW ||
          vertex.nearSaturated != (vertex.rawDepth < vertex.nearDepth)) {
        return VulkanReplayBakeReason::IncompletePrecisionProvenance;
      }
    } else if (vertex.x != static_cast<double>(vertex.nativeX) ||
               vertex.y != static_cast<double>(vertex.nativeY) ||
               vertex.w != CRASH_REPLAY_FALLBACK_W || vertex.preciseXY ||
               vertex.preciseX || vertex.preciseY || vertex.rawDepthValid ||
               vertex.inventedW || vertex.rawDepth != 0.0 ||
               vertex.nearDepth != 0.0 || vertex.nearSaturated) {
      return VulkanReplayBakeReason::IncompletePrecisionProvenance;
    }
    if (!fitsFloat(vertex.x) || !fitsFloat(vertex.y) || !fitsFloat(vertex.u) ||
        !fitsFloat(vertex.v)) {
      return VulkanReplayBakeReason::NumericOverflow;
    }
  }

  if (totals.vertices > std::numeric_limits<uint32_t>::max() ||
      packet.indexCount >
          std::numeric_limits<uint32_t>::max() - totals.vertices) {
    return VulkanReplayBakeReason::NumericOverflow;
  }
  std::size_t next = 0;
  if (!checkedAdd(totals.vertices, packet.indexCount, next))
    return VulkanReplayBakeReason::NumericOverflow;
  totals.vertices = next;
  if (!checkedAdd(totals.visibilityOwners, packet.indexCount / 3u, next) ||
      next > std::numeric_limits<uint32_t>::max()) {
    return VulkanReplayBakeReason::NumericOverflow;
  }
  totals.visibilityOwners = next;
  return VulkanReplayBakeReason::None;
}

VulkanReplayBakeReason validateDepthClear(const ReplayExecutionOperation &item,
                                          const ClearDepth &clear) noexcept {
  if (!clear.wholeSurface || clear.nextEpoch == 0 || !item.depth.clear ||
      !item.access.depthWrite || item.depth.epoch != clear.nextEpoch) {
    return VulkanReplayBakeReason::InvalidDepthCommand;
  }
  return VulkanReplayBakeReason::None;
}

VulkanReplayBakeReason validateReset(const ReplayExecutionOperation &item,
                                     const Reset &reset) noexcept {
  if (!reset.clearDepth || reset.nextDepthEpoch == 0 || !item.depth.clear ||
      !item.access.depthWrite || item.depth.epoch != reset.nextDepthEpoch) {
    return VulkanReplayBakeReason::InvalidDepthCommand;
  }
  return VulkanReplayBakeReason::None;
}

vulkan_abi::DrawUniforms drawUniforms(const DrawPacket &packet) noexcept {
  vulkan_abi::DrawUniforms uniforms;
  uniforms.positionScaleBias = {2.0f / static_cast<float>(NATIVE_WIDTH),
                                2.0f / static_cast<float>(NATIVE_HEIGHT), -1.0f,
                                -1.0f};
  uniforms.depthScaleBias =
      packet.state.depth.invalidateOnAccepted
          ? std::array<float, 4>{0.0f, 1.0f, 0.0f, 0.0f}
          : std::array<float, 4>{vulkan_abi::PROJECTIVE_W_TO_DEPTH_SCALE, 0.0f,
                                 0.0f, 0.0f};
  uniforms.uvDomain = {static_cast<float>(packet.state.textureUvDomain.minU),
                       static_cast<float>(packet.state.textureUvDomain.minV),
                       static_cast<float>(packet.state.textureUvDomain.maxU),
                       static_cast<float>(packet.state.textureUvDomain.maxV)};
  uniforms.textureState = {packet.state.texturePage.raw, packet.state.clut.raw,
                           shaderDrawFlags(packet),
                           blendModeIndex(packet.state.blend.mode)};
  uniforms.textureWindow = {
      packet.state.textureWindow.maskX, packet.state.textureWindow.maskY,
      packet.state.textureWindow.offsetX, packet.state.textureWindow.offsetY};
  // Opaque/no-check-mask draws never need a destination snapshot.  Keeping
  // the binding coordinates zeroed makes accidental feedback use detectable.
  uniforms.destinationState = {};
  if (packet.originalMesh) {
    uniforms.mgsViewRows = packet.originalMesh->viewRows;
    uniforms.mgsPointBytes = packet.originalMesh->pointBytes;
    uniforms.mgsProjection = packet.originalMesh->projection;
    uniforms.mgsWeights = packet.originalMesh->weights;
  }
  if (packet.originalTexture) {
    uniforms.mgsTextureData = packet.originalTexture->data;
    uniforms.mgsTextureOrigin = packet.originalTexture->origin;
    uniforms.mgsPalette = packet.originalTexture->palette;
  }
  if (packet.waterEffect[0] != 0) {
    uniforms.mgsProjection = {float(packet.waterEffect[1] & 63),
                              float(packet.waterEffect[0]),
                              float(packet.submissionId & 0xffffffu), 0};
    uniforms.textureState[2] |=
        replayDrawFlagBit(ReplayDrawFlag::WaterRefraction);
  }
  return uniforms;
}

VulkanReplayDrawDepthPath drawDepthPath(const DrawPacket &packet) noexcept {
  const DepthState &depth = packet.state.depth;
  if (depth.invalidateOnAccepted)
    return VulkanReplayDrawDepthPath::InvalidateAcceptedFar;
  if (!depth.testEnabled && depth.writeEnabled &&
      depth.compare == DepthCompare::Always)
    return VulkanReplayDrawDepthPath::PainterWriteRealDepth;
  if (!depth.testEnabled && !depth.writeEnabled)
    return VulkanReplayDrawDepthPath::PainterOrdered;
  return VulkanReplayDrawDepthPath::LessEqualWrite;
}

VulkanReplayFeedbackPlaneMask
drawFeedbackPlanes(const DrawPacket &packet) noexcept {
  VulkanReplayFeedbackPlaneMask result = 0;
  if (packet.state.blend.enabled) {
    result |= vulkanReplayFeedbackPlaneBit(VulkanReplayFeedbackPlane::Color);
  }
  if (packet.state.mask.checkMaskBit)
    result |= vulkanReplayFeedbackPlaneBit(VulkanReplayFeedbackPlane::Mask);
  return result;
}

VulkanReplayBakedRect destinationSnapshotBounds(
    const DrawPacket &packet, std::size_t firstTriangleIndex,
    const VulkanReplayBakedRect &scissor, uint32_t internalScale) noexcept {
  if (scissor.width == 0 || scissor.height == 0)
    return {};

  // A cached triangle crossing the eye plane can cover any part of the draw
  // area after homogeneous clipping; projected endpoint bounds are
  // insufficient.
  if (packet.originalMesh)
    for (std::size_t index = 0; index < 3; ++index)
      if (packet.vertices[packet.triangleIndices[firstTriangleIndex + index]]
              .rawDepth < 1.0)
        return scissor;

  double minimumX = std::numeric_limits<double>::infinity();
  double minimumY = std::numeric_limits<double>::infinity();
  double maximumX = -std::numeric_limits<double>::infinity();
  double maximumY = -std::numeric_limits<double>::infinity();
  for (std::size_t index = 0; index < 3; ++index) {
    const ReplayVertex &vertex =
        packet.vertices[packet.triangleIndices[firstTriangleIndex + index]];
    const double scaledX = vertex.x * static_cast<double>(internalScale);
    const double scaledY = vertex.y * static_cast<double>(internalScale);
    minimumX = std::min(minimumX, scaledX);
    minimumY = std::min(minimumY, scaledY);
    maximumX = std::max(maximumX, scaledX);
    maximumY = std::max(maximumY, scaledY);
  }

  // One high-resolution pixel on every edge is deliberately conservative.
  // All conversion to uint32_t happens after clipping against the small atlas
  // scissor, so extreme but finite source coordinates cannot overflow.
  const double scissorLeft = static_cast<double>(scissor.x);
  const double scissorTop = static_cast<double>(scissor.y);
  const double scissorRight = static_cast<double>(scissor.x) + scissor.width;
  const double scissorBottom = static_cast<double>(scissor.y) + scissor.height;
  const double left = std::max(std::floor(minimumX) - 1.0, scissorLeft);
  const double top = std::max(std::floor(minimumY) - 1.0, scissorTop);
  const double right = std::min(std::ceil(maximumX) + 1.0, scissorRight);
  const double bottom = std::min(std::ceil(maximumY) + 1.0, scissorBottom);
  if (left >= right || top >= bottom)
    return {};
  return {static_cast<uint32_t>(left), static_cast<uint32_t>(top),
          static_cast<uint32_t>(right - left),
          static_cast<uint32_t>(bottom - top)};
}

VulkanReplayBakedDraw bakeDraw(const DrawPacket &packet, uint32_t internalScale,
                               uint32_t firstVertex,
                               uint32_t firstVisibilityOwnerToken) noexcept {
  VulkanReplayBakedDraw baked;
  baked.firstVertex = firstVertex;
  baked.vertexCount = packet.indexCount;
  baked.scissor = scaledDrawArea(packet.state.drawArea, internalScale);
  baked.depthEpoch = packet.state.depth.epoch;
  baked.route = packet.route;
  baked.sourceRecovery =
      packet.originalMesh && packet.originalMesh->sourceRecovery;
  baked.depthPath = drawDepthPath(packet);
  baked.feedbackPlanes = drawFeedbackPlanes(packet);
  baked.visibilityOwnerCount = packet.indexCount / 3u;
  for (uint32_t triangleIndex = 0;
       triangleIndex < baked.visibilityOwnerCount; ++triangleIndex) {
    baked.visibilityOwnerTokens[triangleIndex] =
        firstVisibilityOwnerToken + triangleIndex;
  }

  const bool needsFeedback = baked.feedbackPlanes != 0;
  const bool splitQuad = needsFeedback && packet.indexCount == 6;
  baked.segmentCount = splitQuad ? 2u : 1u;
  for (uint32_t segmentIndex = 0; segmentIndex < baked.segmentCount;
       ++segmentIndex) {
    VulkanReplayBakedDrawSegment &segment = baked.segments[segmentIndex];
    const uint32_t vertexOffset = splitQuad ? segmentIndex * 3u : 0u;
    segment.firstVertex = firstVertex + vertexOffset;
    segment.vertexCount = splitQuad ? 3u : packet.indexCount;
    segment.scissor = baked.scissor;
    segment.uniforms = drawUniforms(packet);
    if (needsFeedback) {
      segment.destinationSnapshot = destinationSnapshotBounds(
          packet, vertexOffset, baked.scissor, internalScale);
      const VulkanReplayBakedRect &area = segment.destinationSnapshot;
      segment.uniforms.destinationState = {area.x, area.y, area.width,
                                           area.height};
      segment.snapshotAction =
          segmentIndex == 0 ? VulkanReplayDestinationSnapshotAction::Initial
                            : VulkanReplayDestinationSnapshotAction::Refresh;
    }
  }
  baked.uniforms = baked.segments[0].uniforms;
  return baked;
}

ReplayVisibilityAuditOwner
bakeVisibilityOwner(const DrawPacket &packet,
                    const ReplayExecutionOperation &item,
                    uint8_t triangleIndex) noexcept {
  ReplayVisibilityAuditOwner owner;
  owner.packetSequence = packet.packetSequence;
  owner.submissionId = packet.submissionId;
  owner.sourceOrdinal = item.ordinal;
  owner.frameIndex = item.frameIndex;
  owner.operationIndex = item.operationIndex;
  owner.triangleIndex = triangleIndex;
  owner.provenance = captureReplayTriangleProvenance(packet, triangleIndex);
  owner.route = packet.route;
  owner.opaque = !packet.state.blend.enabled;
  owner.fullyPrecise = owner.provenance.validTopology;
  owner.authenticDepth = owner.provenance.validTopology;
  for (std::size_t corner = 0; corner < owner.provenance.vertexIndices.size();
       ++corner) {
    const uint8_t vertexIndex = owner.provenance.vertexIndices[corner];
    const ReplayVertex &vertex = packet.vertices[vertexIndex];
    owner.effectiveProjectiveW[corner] = static_cast<float>(vertex.w);
    owner.nativeX[corner] = vertex.nativeX;
    owner.nativeY[corner] = vertex.nativeY;
    owner.preciseX[corner] = vertex.x;
    owner.preciseY[corner] = vertex.y;
    owner.fullyPrecise &= vertex.preciseXY && vertex.preciseX &&
                            vertex.preciseY && !vertex.reconciledX &&
                            !vertex.reconciledY;
    owner.authenticDepth &= vertex.rawDepthValid && !vertex.inventedW &&
                              std::isfinite(vertex.rawDepth) &&
                              vertex.rawDepth > 0.0;
  }
  return owner;
}

vulkan_abi::Vertex bakeVertex(const ReplayVertex &source) noexcept {
  vulkan_abi::Vertex vertex;
  vertex.positionNative = {static_cast<float>(source.x),
                           static_cast<float>(source.y)};
  vertex.projectiveW = static_cast<float>(source.w);
  vertex.uv = {static_cast<float>(source.u), static_cast<float>(source.v)};
  vertex.color255 = {static_cast<float>(source.color.r),
                     static_cast<float>(source.color.g),
                     static_cast<float>(source.color.b)};
  return vertex;
}

VulkanReplayBakedNativeUpload bakeNativeUpload(const NativeTileUpload &upload,
                                               std::size_t firstPixel) {
  VulkanReplayBakedNativeUpload baked;
  baked.destination = upload.destination;
  baked.firstPixel = firstPixel;
  baked.pixelCount = upload.pixels.size();
  baked.nativeRevision = upload.nativeRevision;
  const auto xRegions = splitNativeAxis(upload.destination.x,
                                        upload.destination.width, NATIVE_WIDTH);
  const auto yRegions = splitNativeAxis(
      upload.destination.y, upload.destination.height, NATIVE_HEIGHT);
  for (const AxisRegion &y : yRegions) {
    if (y.length == 0)
      continue;
    for (const AxisRegion &x : xRegions) {
      if (x.length == 0)
        continue;
      VulkanReplayBakedNativeUpload::Region &region =
          baked.regions[baked.regionCount++];
      region.sourceWordOffset =
          firstPixel +
          static_cast<std::size_t>(y.source) * upload.destination.width +
          x.source;
      region.bufferRowLength = upload.destination.width;
      region.bufferImageHeight = upload.destination.height;
      region.destination = {x.destination, y.destination, x.length, y.length};
    }
  }
  return baked;
}

VulkanReplayBakedNativeUpload
bakePaintPixelNativeUpload(const PaintedPixel &pixel, std::size_t firstPixel,
                           uint32_t width = 1) noexcept {
  VulkanReplayBakedNativeUpload baked;
  baked.destination =
      NativeRect{pixel.destination.x, pixel.destination.y, width, 1u};
  baked.firstPixel = firstPixel;
  baked.pixelCount = width;
  baked.nativeRevision = 0;
  baked.regionCount = 1u;
  baked.regions[0].sourceWordOffset = firstPixel;
  baked.regions[0].bufferRowLength = width;
  baked.regions[0].bufferImageHeight = 1u;
  baked.regions[0].destination = {static_cast<uint32_t>(pixel.destination.x),
                                  static_cast<uint32_t>(pixel.destination.y),
                                  width, 1u};
  return baked;
}

DrawPacket makePaintPixelDraw(const PaintPixels &paint,
                              const PaintedPixel &pixel,
                              uint32_t width = 1) noexcept {
  DrawPacket packet;
  packet.opcode = 0x40u;
  packet.route = PacketRoute::Screen2D;
  packet.semanticReason = PacketSemanticReason::Intrinsic2DFamily;
  packet.fallbackReason = PacketFallbackReason::None;
  packet.validMask = 0;
  packet.vertexCount = 3u;
  packet.indexCount = 3u;
  packet.triangleIndices = {0u, 1u, 2u, 0u, 0u, 0u};

  const int32_t x = pixel.destination.x;
  const int32_t y = pixel.destination.y;
  packet.state.drawArea = DrawArea{x, y, x + int32_t(width) - 1, y};
  packet.state.blend = paint.blend;
  packet.state.blend.enabled =
      paint.blend.enabled &&
      (!paint.blend.texturedStpOnly || pixel.sourceMaskBit);
  // The synthetic draw is untextured; the source-STP decision was captured
  // by the authoritative native raster and is now an exact per-pixel fact.
  packet.state.blend.texturedStpOnly = false;
  packet.state.mask.forceMaskBit =
      pixel.sourceMaskBit || paint.mask.forceMaskBit;
  packet.state.mask.checkMaskBit = false;
  packet.state.depth = paint.depth;
  packet.state.dither = false;

  constexpr std::array<std::array<int32_t, 2>, 3> offsets{
      std::array<int32_t, 2>{0, 0}, std::array<int32_t, 2>{2, 0},
      std::array<int32_t, 2>{0, 2}};
  for (std::size_t index = 0; index < offsets.size(); ++index) {
    ReplayVertex &vertex = packet.vertices[index];
    vertex.nativeX = x + offsets[index][0] * int32_t(width);
    vertex.nativeY = y + offsets[index][1];
    vertex.x = static_cast<double>(vertex.nativeX);
    vertex.y = static_cast<double>(vertex.nativeY);
    vertex.w = CRASH_REPLAY_FALLBACK_W;
    vertex.color = pixel.color;
  }
  return packet;
}

void alignNativeUploadArena(std::vector<uint16_t> &pixels) {
  if ((pixels.size() & 1u) != 0u)
    pixels.push_back(0);
}

VulkanReplayBakedPaintRect bakePaintRect(const PaintRect &paint,
                                         uint32_t internalScale,
                                         std::size_t firstPixel) noexcept {
  VulkanReplayBakedPaintRect baked;
  baked.atlasArea = scaledNativeRect(paint.destination, internalScale);
  baked.nativeRegion.sourceWordOffset = firstPixel;
  baked.nativeRegion.bufferRowLength = paint.destination.width;
  baked.nativeRegion.bufferImageHeight = paint.destination.height;
  baked.nativeRegion.destination = {static_cast<uint32_t>(paint.destination.x),
                                    static_cast<uint32_t>(paint.destination.y),
                                    paint.destination.width,
                                    paint.destination.height};
  baked.color = paint.color;
  const uint16_t finalNativeColor = static_cast<uint16_t>(
      paint.nativeColor | (paint.mask.forceMaskBit ? 0x8000u : 0u));
  baked.maskValue = (finalNativeColor & 0x8000u) != 0u ? 1u : 0u;
  baked.depthEpoch = paint.depth.epoch;
  baked.depthClearValue = 1.0f;
  baked.coalescedCommandCount = paint.coalescedCommandCount;
  return baked;
}

VulkanReplayBakedFillRect bakeFillRect(const FillRect &fill,
                                       uint32_t internalScale,
                                       std::vector<uint16_t> &uploadPixels) {
  VulkanReplayBakedFillRect baked;
  baked.color = fill.color;
  // GP0(02h) bypasses E6 entirely and its 24-to-15-bit conversion always
  // writes a clear PS1 mask bit.
  baked.maskValue = 0;
  baked.depthClearValue = 1.0f;

  const auto xRegions =
      splitNativeAxis(fill.destination.x, fill.destination.width, NATIVE_WIDTH);
  const auto yRegions = splitNativeAxis(fill.destination.y,
                                        fill.destination.height, NATIVE_HEIGHT);
  for (const AxisRegion &y : yRegions) {
    if (y.length == 0)
      continue;
    for (const AxisRegion &x : xRegions) {
      if (x.length == 0)
        continue;
      alignNativeUploadArena(uploadPixels);
      VulkanReplayBakedFillRect::Region &region =
          baked.regions[baked.regionCount++];
      region.nativeRegion.sourceWordOffset = uploadPixels.size();
      region.nativeRegion.bufferRowLength = x.length;
      region.nativeRegion.bufferImageHeight = y.length;
      region.nativeRegion.destination = {x.destination, y.destination, x.length,
                                         y.length};
      region.atlasArea = {x.destination * internalScale,
                          y.destination * internalScale,
                          x.length * internalScale, y.length * internalScale};
      uploadPixels.insert(uploadPixels.end(),
                          static_cast<std::size_t>(x.length) * y.length,
                          fill.nativeColor);
    }
  }
  return baked;
}

VulkanReplayBakedCpuUpload bakeCpuUpload(const CpuUpload &upload,
                                         uint32_t internalScale,
                                         std::vector<uint16_t> &uploadPixels) {
  VulkanReplayBakedCpuUpload baked;
  baked.transactionId = upload.transactionId;
  baked.phase = upload.phase;
  baked.transfer = upload.transfer;
  baked.pixelOffset = upload.pixelOffset;
  baked.sourceMask = upload.mask;
  if (upload.phase != CpuUploadPhase::Data)
    return baked;

  baked.consumedPixelCount = upload.pixels.size();
  for (const uint64_t word : upload.acceptedBits)
    baked.acceptedPixelCount += std::popcount(word);
  baked.rejectedPixelCount =
      baked.consumedPixelCount - baked.acceptedPixelCount;
  std::size_t runCount = 0;
  for (std::size_t sourceOffset = 0; sourceOffset < upload.pixels.size();) {
    if (!cpuUploadAcceptedAt(upload, sourceOffset)) {
      ++sourceOffset;
      continue;
    }
    sourceOffset += cpuUploadRunAt(upload, sourceOffset).length;
    ++runCount;
  }
  baked.runs.reserve(runCount);

  const uint16_t forcedMask = upload.mask.forceMaskBit ? 0x8000u : 0u;
  for (std::size_t sourceOffset = 0; sourceOffset < upload.pixels.size();) {
    if (!cpuUploadAcceptedAt(upload, sourceOffset)) {
      ++sourceOffset;
      continue;
    }
    const CpuUploadRunSpan span = cpuUploadRunAt(upload, sourceOffset);
    alignNativeUploadArena(uploadPixels);

    VulkanReplayBakedCpuUploadRun run;
    run.sourceChunkOffset = span.sourceChunkOffset;
    run.nativeRegion.sourceWordOffset = uploadPixels.size();
    run.nativeRegion.bufferRowLength = span.length;
    run.nativeRegion.bufferImageHeight = 1;
    run.nativeRegion.destination = {span.destinationX, span.destinationY,
                                    span.length, 1};
    run.atlasArea = {span.destinationX * internalScale,
                     span.destinationY * internalScale,
                     span.length * internalScale, internalScale};
    for (std::size_t index = 0; index < span.length; ++index) {
      uploadPixels.push_back(static_cast<uint16_t>(
          upload.pixels[sourceOffset + index] | forcedMask));
    }
    baked.runs.push_back(run);
    sourceOffset += span.length;
  }
  return baked;
}

} // namespace

VulkanReplayBakeResult
bakeModernReplayVulkan(const ModernReplayExecutionPlan &plan,
                       uint32_t internalScale,
                       VulkanReplayBakeFeatures features) {
  if (internalScale == 0 || internalScale > MAX_INTERNAL_SCALE)
    return failure(VulkanReplayBakeReason::InvalidScale);

  const std::span<const ReplayExecutionOperation> operations =
      plan.operations();
  if (operations.empty())
    return failure(VulkanReplayBakeReason::EmptyPlan);

  BakeTotals totals;
  totals.commands = operations.size();
  uint64_t establishedDepthEpoch = 0;
  std::vector<const Operation *> sources;
  sources.reserve(operations.size());
  for (std::size_t itemIndex = 0; itemIndex < operations.size(); ++itemIndex) {
    const ReplayExecutionOperation &item = operations[itemIndex];
    if (item.ordinal != itemIndex)
      return failure(VulkanReplayBakeReason::SourceOperationMismatch, item);
    const Operation *source = nullptr;
    try {
      source = &plan.sourceOperation(item);
    } catch (const std::out_of_range &) {
      return failure(VulkanReplayBakeReason::SourceOperationMismatch, item);
    }
    sources.push_back(source);

    VulkanReplayBakeReason reason = VulkanReplayBakeReason::None;
    switch (item.kind) {
    case ReplayExecutionKind::NativeTileUpload: {
      const auto *upload = std::get_if<NativeTileUpload>(source);
      if (upload == nullptr)
        reason = VulkanReplayBakeReason::SourceOperationMismatch;
      else
        reason = validateNativeUpload(*upload, totals);
      break;
    }
    case ReplayExecutionKind::SurfaceSeed:
      reason = std::holds_alternative<SurfaceSeed>(*source)
                   ? VulkanReplayBakeReason::UnsupportedSurfaceSeed
                   : VulkanReplayBakeReason::SourceOperationMismatch;
      break;
    case ReplayExecutionKind::DrawOpaque:
    case ReplayExecutionKind::DrawTranslucent: {
      const auto *packet = std::get_if<DrawPacket>(source);
      if (packet == nullptr)
        reason = VulkanReplayBakeReason::SourceOperationMismatch;
      else
        reason = validateDraw(item, *packet, features, totals);
      if (reason == VulkanReplayBakeReason::None) {
        if (establishedDepthEpoch != 0 &&
            packet->state.depth.epoch != establishedDepthEpoch) {
          reason = VulkanReplayBakeReason::InvalidDepthState;
        } else {
          establishedDepthEpoch = packet->state.depth.epoch;
        }
      }
      break;
    }
    case ReplayExecutionKind::PaintRect: {
      const auto *paint = std::get_if<PaintRect>(source);
      if (paint == nullptr)
        reason = VulkanReplayBakeReason::SourceOperationMismatch;
      else
        reason = validatePaintRect(item, *paint, establishedDepthEpoch, totals);
      if (reason == VulkanReplayBakeReason::None)
        establishedDepthEpoch = paint->depth.epoch;
      break;
    }
    case ReplayExecutionKind::PaintPixels: {
      const auto *paint = std::get_if<PaintPixels>(source);
      if (paint == nullptr) {
        reason = VulkanReplayBakeReason::SourceOperationMismatch;
      } else {
        reason = validatePaintPixels(item, *paint, establishedDepthEpoch,
                                     features, totals);
      }
      if (reason == VulkanReplayBakeReason::None)
        establishedDepthEpoch = paint->depth.epoch;
      break;
    }
    case ReplayExecutionKind::FillRect: {
      const auto *fill = std::get_if<FillRect>(source);
      if (fill == nullptr)
        reason = VulkanReplayBakeReason::SourceOperationMismatch;
      else
        reason = validateFillRect(item, *fill, totals);
      break;
    }
    case ReplayExecutionKind::CopyRect: {
      const auto *copy = std::get_if<CopyRect>(source);
      if (copy == nullptr)
        reason = VulkanReplayBakeReason::SourceOperationMismatch;
      else
        reason = validateCopyRect(item, *copy, features);
      break;
    }
    case ReplayExecutionKind::CpuUploadBegin:
    case ReplayExecutionKind::CpuUploadData:
    case ReplayExecutionKind::CpuUploadComplete:
    case ReplayExecutionKind::CpuUploadAbort: {
      const auto *upload = std::get_if<CpuUpload>(source);
      if (upload == nullptr) {
        reason = VulkanReplayBakeReason::SourceOperationMismatch;
      } else if (!features.allowCpuUpload) {
        reason = VulkanReplayBakeReason::CpuUploadDisabled;
      } else {
        reason = validateCpuUpload(item, *upload, totals);
      }
      break;
    }
    case ReplayExecutionKind::ClearDepth: {
      const auto *clear = std::get_if<ClearDepth>(source);
      if (clear == nullptr)
        reason = VulkanReplayBakeReason::SourceOperationMismatch;
      else
        reason = validateDepthClear(item, *clear);
      if (reason == VulkanReplayBakeReason::None)
        establishedDepthEpoch = clear->nextEpoch;
      break;
    }
    case ReplayExecutionKind::Reset: {
      const auto *reset = std::get_if<Reset>(source);
      if (reset == nullptr)
        reason = VulkanReplayBakeReason::SourceOperationMismatch;
      else
        reason = validateReset(item, *reset);
      if (reason == VulkanReplayBakeReason::None)
        establishedDepthEpoch = reset->nextDepthEpoch;
      break;
    }
    case ReplayExecutionKind::FrameLatch:
      if (!std::holds_alternative<FrameLatch>(*source)) {
        reason = VulkanReplayBakeReason::SourceOperationMismatch;
      } else if (std::get<FrameLatch>(*source).policy.internalScale !=
                 internalScale) {
        reason = VulkanReplayBakeReason::PolicyScaleMismatch;
      }
      break;
    default:
      reason = VulkanReplayBakeReason::UnsupportedExecutionKind;
      break;
    }

    if (reason != VulkanReplayBakeReason::None)
      return failure(reason, item);
  }

  VulkanReplayBakedBatch candidate;
  candidate.internalScale = internalScale;
  if (totals.commands > candidate.commands.max_size() ||
      totals.vertices > candidate.vertices.max_size() ||
      totals.visibilityOwners > candidate.visibilityOwners.max_size() ||
      totals.uploadPixels > candidate.nativeUploadPixels.max_size()) {
    return failure(VulkanReplayBakeReason::NumericOverflow);
  }
  candidate.commands.reserve(totals.commands);
  candidate.vertices.reserve(totals.vertices);
  candidate.visibilityOwners.reserve(totals.visibilityOwners);
  candidate.nativeUploadPixels.reserve(totals.uploadPixels);

  for (std::size_t itemIndex = 0; itemIndex < operations.size(); ++itemIndex) {
    const ReplayExecutionOperation &item = operations[itemIndex];
    const Operation &source = *sources[itemIndex];
    VulkanReplayBakedCommand command;
    command.sourceOrdinal = item.ordinal;

    switch (item.kind) {
    case ReplayExecutionKind::NativeTileUpload: {
      const NativeTileUpload &upload = std::get<NativeTileUpload>(source);
      alignNativeUploadArena(candidate.nativeUploadPixels);
      const VulkanReplayBakedNativeUpload baked =
          bakeNativeUpload(upload, candidate.nativeUploadPixels.size());
      candidate.nativeUploadPixels.insert(candidate.nativeUploadPixels.end(),
                                          upload.pixels.begin(),
                                          upload.pixels.end());
      command.payload = baked;
      break;
    }
    case ReplayExecutionKind::DrawOpaque:
    case ReplayExecutionKind::DrawTranslucent: {
      const DrawPacket &packet = std::get<DrawPacket>(source);
      const uint32_t firstVisibilityOwnerToken =
          static_cast<uint32_t>(candidate.visibilityOwners.size() + 1u);
      const VulkanReplayBakedDraw draw =
          bakeDraw(packet, internalScale,
                   static_cast<uint32_t>(candidate.vertices.size()),
                   firstVisibilityOwnerToken);
      for (std::size_t index = 0; index < packet.indexCount; ++index) {
        candidate.vertices.push_back([&] {
          auto v = bakeVertex(packet.vertices[packet.triangleIndices[index]]);
          if (packet.originalMesh)
            v.positionNative[0] = packet.triangleIndices[index];
          return v;
        }());
      }
      for (uint8_t triangleIndex = 0; triangleIndex < packet.indexCount / 3u;
           ++triangleIndex) {
        candidate.visibilityOwners.push_back(
            bakeVisibilityOwner(packet, item, triangleIndex));
      }
      command.payload = draw;
      break;
    }
    case ReplayExecutionKind::PaintRect: {
      const PaintRect &paint = std::get<PaintRect>(source);
      alignNativeUploadArena(candidate.nativeUploadPixels);
      const std::size_t firstPixel = candidate.nativeUploadPixels.size();
      const uint16_t finalNativeColor = static_cast<uint16_t>(
          paint.nativeColor | (paint.mask.forceMaskBit ? 0x8000u : 0u));
      candidate.nativeUploadPixels.insert(
          candidate.nativeUploadPixels.end(),
          static_cast<std::size_t>(paint.coalescedCommandCount),
          finalNativeColor);
      command.payload = bakePaintRect(paint, internalScale, firstPixel);
      break;
    }
    case ReplayExecutionKind::PaintPixels: {
      const PaintPixels &paint = std::get<PaintPixels>(source);
      for (std::size_t start = 0; start < paint.pixels.size();) {
        const PaintedPixel &pixel = paint.pixels[start];
        uint32_t width = 1;
        // A run contains disjoint consecutive accepted destination pixels.
        // Identical source color/STP may share one draw even when blending;
        // preserve each final native word separately in the mirror upload.
        // Holes, repeated destinations, row and source changes split runs.
        while (start + width < paint.pixels.size()) {
          const auto &next = paint.pixels[start + width];
          if (next.destination.y != pixel.destination.y ||
              next.destination.x != pixel.destination.x + int32_t(width) ||
              next.color != pixel.color ||
              next.sourceMaskBit != pixel.sourceMaskBit)
            break;
          ++width;
        }
        start += width;
        alignNativeUploadArena(candidate.nativeUploadPixels);
        const std::size_t firstPixel = candidate.nativeUploadPixels.size();
        for (uint32_t i = 0; i < width; ++i)
          candidate.nativeUploadPixels.push_back(
              paint.pixels[start - width + i].nativeColor);

        VulkanReplayBakedCommand nativeCommand;
        nativeCommand.sourceOrdinal = item.ordinal;
        nativeCommand.payload =
            bakePaintPixelNativeUpload(pixel, firstPixel, width);
        candidate.commands.push_back(std::move(nativeCommand));

        const DrawPacket packet = makePaintPixelDraw(paint, pixel, width);
        const uint32_t firstVisibilityOwnerToken =
            static_cast<uint32_t>(candidate.visibilityOwners.size() + 1u);
        VulkanReplayBakedCommand drawCommand;
        drawCommand.sourceOrdinal = item.ordinal;
        drawCommand.payload =
            bakeDraw(packet, internalScale,
                     static_cast<uint32_t>(candidate.vertices.size()),
                     firstVisibilityOwnerToken);
        for (std::size_t index = 0; index < packet.indexCount; ++index) {
          candidate.vertices.push_back(
              bakeVertex(packet.vertices[packet.triangleIndices[index]]));
        }
        candidate.visibilityOwners.push_back(
            bakeVisibilityOwner(packet, item, 0u));
        candidate.commands.push_back(std::move(drawCommand));
      }
      continue;
    }
    case ReplayExecutionKind::FillRect:
      command.payload = bakeFillRect(std::get<FillRect>(source), internalScale,
                                     candidate.nativeUploadPixels);
      break;
    case ReplayExecutionKind::CopyRect:
      command.payload = bakeCopyRect(std::get<CopyRect>(source), internalScale);
      break;
    case ReplayExecutionKind::CpuUploadBegin:
    case ReplayExecutionKind::CpuUploadData:
    case ReplayExecutionKind::CpuUploadComplete:
    case ReplayExecutionKind::CpuUploadAbort:
      command.payload =
          bakeCpuUpload(std::get<CpuUpload>(source), internalScale,
                        candidate.nativeUploadPixels);
      break;
    case ReplayExecutionKind::ClearDepth: {
      const ClearDepth &clear = std::get<ClearDepth>(source);
      command.payload = VulkanReplayBakedDepthClear{
          fullScaledSurface(internalScale), clear.nextEpoch, false};
      break;
    }
    case ReplayExecutionKind::Reset: {
      const Reset &reset = std::get<Reset>(source);
      command.payload = VulkanReplayBakedDepthClear{
          fullScaledSurface(internalScale), reset.nextDepthEpoch, true};
      break;
    }
    case ReplayExecutionKind::FrameLatch:
      command.payload = VulkanReplayBakedLatch{std::get<FrameLatch>(source)};
      break;
    default:
      // The complete validation pass above makes this unreachable without
      // concurrent mutation of immutable plan storage.
      return failure(VulkanReplayBakeReason::SourceOperationMismatch, item);
    }
    candidate.commands.push_back(std::move(command));
  }

  VulkanReplayBakeResult result;
  result.batch = std::move(candidate);
  return result;
}

VulkanReplayVisibilityBoundsResult measureVulkanReplayVisibilityBounds(
    const VulkanReplayBakedBatch &batch) noexcept {
  VulkanReplayVisibilityBoundsResult result;
  if (batch.internalScale == 0 || batch.internalScale > MAX_INTERNAL_SCALE)
    return result;

  const uint64_t atlasWidth =
      static_cast<uint64_t>(NATIVE_WIDTH) * batch.internalScale;
  const uint64_t atlasHeight =
      static_cast<uint64_t>(NATIVE_HEIGHT) * batch.internalScale;
  bool found = false;
  uint32_t unionLeft = 0;
  uint32_t unionTop = 0;
  uint32_t unionRight = 0;
  uint32_t unionBottom = 0;

  for (const VulkanReplayBakedCommand &command : batch.commands) {
    const auto *draw = std::get_if<VulkanReplayBakedDraw>(&command.payload);
    if (draw == nullptr)
      continue;
    if (draw->segmentCount == 0 || draw->segmentCount > draw->segments.size())
      return result;

    for (uint32_t segmentIndex = 0; segmentIndex < draw->segmentCount;
         ++segmentIndex) {
      const VulkanReplayBakedDrawSegment &segment =
          draw->segments[segmentIndex];
      const uint64_t scissorRight =
          static_cast<uint64_t>(segment.scissor.x) + segment.scissor.width;
      const uint64_t scissorBottom =
          static_cast<uint64_t>(segment.scissor.y) + segment.scissor.height;
      const uint64_t vertexEnd =
          static_cast<uint64_t>(segment.firstVertex) + segment.vertexCount;
      if (segment.vertexCount == 0 || (segment.vertexCount % 3u) != 0u ||
          vertexEnd > batch.vertices.size() || scissorRight > atlasWidth ||
          scissorBottom > atlasHeight) {
        return result;
      }
      if (segment.scissor.width == 0 || segment.scissor.height == 0)
        continue;

      double minimumX = std::numeric_limits<double>::infinity();
      double minimumY = std::numeric_limits<double>::infinity();
      double maximumX = -std::numeric_limits<double>::infinity();
      double maximumY = -std::numeric_limits<double>::infinity();
      for (uint32_t index = 0; index < segment.vertexCount; ++index) {
        const vulkan_abi::Vertex &vertex =
            batch.vertices[segment.firstVertex + index];
        const double scaledX =
            static_cast<double>(vertex.positionNative[0]) * batch.internalScale;
        const double scaledY =
            static_cast<double>(vertex.positionNative[1]) * batch.internalScale;
        if (!std::isfinite(scaledX) || !std::isfinite(scaledY))
          return result;
        minimumX = std::min(minimumX, scaledX);
        minimumY = std::min(minimumY, scaledY);
        maximumX = std::max(maximumX, scaledX);
        maximumY = std::max(maximumY, scaledY);
      }

      const double left = std::max(std::floor(minimumX) - 1.0,
                                   static_cast<double>(segment.scissor.x));
      const double top = std::max(std::floor(minimumY) - 1.0,
                                  static_cast<double>(segment.scissor.y));
      const double right = std::min(std::ceil(maximumX) + 1.0,
                                    static_cast<double>(scissorRight));
      const double bottom = std::min(std::ceil(maximumY) + 1.0,
                                     static_cast<double>(scissorBottom));
      if (left >= right || top >= bottom)
        continue;

      const uint32_t boundedLeft = static_cast<uint32_t>(left);
      const uint32_t boundedTop = static_cast<uint32_t>(top);
      const uint32_t boundedRight = static_cast<uint32_t>(right);
      const uint32_t boundedBottom = static_cast<uint32_t>(bottom);
      if (!found) {
        unionLeft = boundedLeft;
        unionTop = boundedTop;
        unionRight = boundedRight;
        unionBottom = boundedBottom;
        found = true;
      } else {
        unionLeft = std::min(unionLeft, boundedLeft);
        unionTop = std::min(unionTop, boundedTop);
        unionRight = std::max(unionRight, boundedRight);
        unionBottom = std::max(unionBottom, boundedBottom);
      }
    }
  }

  if (!found) {
    result.status = VulkanReplayVisibilityBoundsStatus::Empty;
    return result;
  }
  result.status = VulkanReplayVisibilityBoundsStatus::Complete;
  result.atlasArea = {unionLeft, unionTop, unionRight - unionLeft,
                      unionBottom - unionTop};
  return result;
}

std::string_view
vulkanReplayBakeReasonName(VulkanReplayBakeReason reason) noexcept {
  switch (reason) {
  case VulkanReplayBakeReason::None:
    return "none";
  case VulkanReplayBakeReason::EmptyPlan:
    return "empty_plan";
  case VulkanReplayBakeReason::InvalidScale:
    return "invalid_scale";
  case VulkanReplayBakeReason::PolicyScaleMismatch:
    return "policy_scale_mismatch";
  case VulkanReplayBakeReason::SourceOperationMismatch:
    return "source_operation_mismatch";
  case VulkanReplayBakeReason::UnsupportedSurfaceSeed:
    return "unsupported_surface_seed";
  case VulkanReplayBakeReason::UnsupportedExecutionKind:
    return "unsupported_execution_kind";
  case VulkanReplayBakeReason::TexturedDrawDisabled:
    return "textured_draw_disabled";
  case VulkanReplayBakeReason::SemiTransparentDrawDisabled:
    return "semi_transparent_draw_disabled";
  case VulkanReplayBakeReason::Screen2DDrawDisabled:
    return "screen_2d_draw_disabled";
  case VulkanReplayBakeReason::DestinationColorFeedbackDisabled:
    return "destination_color_feedback_disabled";
  case VulkanReplayBakeReason::DestinationMaskReadDisabled:
    return "destination_mask_read_disabled";
  case VulkanReplayBakeReason::PainterOrderedDepthDisabled:
    return "painter_ordered_depth_disabled";
  case VulkanReplayBakeReason::PainterWriteRealDepthDisabled:
    return "painter_write_real_depth_disabled";
  case VulkanReplayBakeReason::DepthInvalidateDisabled:
    return "depth_invalidate_disabled";
  case VulkanReplayBakeReason::CpuUploadDisabled:
    return "cpu_upload_disabled";
  case VulkanReplayBakeReason::UnsupportedDrawRoute:
    return "unsupported_draw_route";
  case VulkanReplayBakeReason::UnsupportedDrawState:
    return "unsupported_draw_state";
  case VulkanReplayBakeReason::InvalidDepthState:
    return "invalid_depth_state";
  case VulkanReplayBakeReason::IncompletePrecisionProvenance:
    return "incomplete_precision_provenance";
  case VulkanReplayBakeReason::InvalidTriangleIndex:
    return "invalid_triangle_index";
  case VulkanReplayBakeReason::NonFiniteVertexAttribute:
    return "non_finite_vertex_attribute";
  case VulkanReplayBakeReason::ProjectiveWOutOfDomain:
    return "projective_w_out_of_domain";
  case VulkanReplayBakeReason::InvalidNativeTileUpload:
    return "invalid_native_tile_upload";
  case VulkanReplayBakeReason::InvalidPaintRect:
    return "invalid_paint_rect";
  case VulkanReplayBakeReason::UnsupportedPaintRectState:
    return "unsupported_paint_rect_state";
  case VulkanReplayBakeReason::InvalidFillRect:
    return "invalid_fill_rect";
  case VulkanReplayBakeReason::UnsupportedFillRectState:
    return "unsupported_fill_rect_state";
  case VulkanReplayBakeReason::InvalidCopyRect:
    return "invalid_copy_rect";
  case VulkanReplayBakeReason::CopyRectScratchRequired:
    return "copy_rect_scratch_required";
  case VulkanReplayBakeReason::UnsupportedCopyRectState:
    return "unsupported_copy_rect_state";
  case VulkanReplayBakeReason::InvalidCpuUpload:
    return "invalid_cpu_upload";
  case VulkanReplayBakeReason::UnsupportedCpuUploadState:
    return "unsupported_cpu_upload_state";
  case VulkanReplayBakeReason::InvalidDepthCommand:
    return "invalid_depth_command";
  case VulkanReplayBakeReason::NumericOverflow:
    return "numeric_overflow";
  case VulkanReplayBakeReason::CopyRectOrderedOverlapUnproven:
    return "copy_rect_ordered_overlap_unproven";
  case VulkanReplayBakeReason::PaintPixelsDisabled:
    return "paint_pixels_disabled";
  case VulkanReplayBakeReason::InvalidPaintPixels:
    return "invalid_paint_pixels";
  case VulkanReplayBakeReason::UnsupportedPaintPixelsState:
    return "unsupported_paint_pixels_state";
  }
  return "unknown";
}

} // namespace ps1::gpu::replay
