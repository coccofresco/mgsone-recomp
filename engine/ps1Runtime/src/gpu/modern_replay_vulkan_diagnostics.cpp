#include "runtime/gpu/modern_replay_vulkan_diagnostics.h"

#include "runtime/gpu/modern_replay_vulkan_abi.h"
#include "runtime/metrics.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace ps1::gpu::replay {
namespace {

constexpr uint64_t kFnv1a64Offset = UINT64_C(14695981039346656037);
constexpr uint64_t kFnv1a64Prime = UINT64_C(1099511628211);

struct ExpectedSample {
  Color32 color{};
  uint8_t mask = 0;
  float depth = 1.0f;
  bool invalidDepth = false;
};

bool checkedMultiply(std::size_t left, std::size_t right, std::size_t &result) {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left)
    return false;
  result = left * right;
  return true;
}

bool checkedAdd(std::size_t left, std::size_t right, std::size_t &result) {
  if (right > std::numeric_limits<std::size_t>::max() - left)
    return false;
  result = left + right;
  return true;
}

bool planeCanAddress(const VulkanReplayDiagnosticPlaneView &plane,
                     uint32_t width, uint32_t height,
                     std::size_t bytesPerPixel) {
  if (plane.data == nullptr || width == 0 || height == 0)
    return false;
  std::size_t rowBytes = 0;
  if (!checkedMultiply(width, bytesPerPixel, rowBytes) ||
      plane.rowPitchBytes < rowBytes)
    return false;
  std::size_t precedingRows = 0;
  std::size_t requiredBytes = 0;
  return checkedMultiply(static_cast<std::size_t>(height - 1),
                         plane.rowPitchBytes, precedingRows) &&
         checkedAdd(precedingRows, rowBytes, requiredBytes) &&
         plane.sizeBytes >= requiredBytes;
}

bool planesCanAddress(const VulkanReplayDiagnosticPlanes &planes,
                      uint32_t width, uint32_t height) {
  return planeCanAddress(planes.colorRgba8, width, height, sizeof(Color32)) &&
         planeCanAddress(planes.maskR8, width, height, sizeof(uint8_t)) &&
         planeCanAddress(planes.depthD32, width, height, sizeof(float));
}

template <typename T>
T loadPlane(const VulkanReplayDiagnosticPlaneView &plane, uint32_t x,
            uint32_t y) {
  const auto *bytes = static_cast<const std::byte *>(plane.data) +
                      static_cast<std::size_t>(y) * plane.rowPitchBytes +
                      static_cast<std::size_t>(x) * sizeof(T);
  T value{};
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

void appendHashByte(uint64_t &hash, uint8_t value) {
  hash = (hash ^ value) * kFnv1a64Prime;
}

void appendHashU32(uint64_t &hash, uint32_t value) {
  appendHashByte(hash, static_cast<uint8_t>(value));
  appendHashByte(hash, static_cast<uint8_t>(value >> 8));
  appendHashByte(hash, static_cast<uint8_t>(value >> 16));
  appendHashByte(hash, static_cast<uint8_t>(value >> 24));
}

void appendSampleHashes(VulkanReplayDiagnosticHashes &hashes,
                        const ExpectedSample &sample) {
  appendHashByte(hashes.rgba8, sample.color.r);
  appendHashByte(hashes.rgba8, sample.color.g);
  appendHashByte(hashes.rgba8, sample.color.b);
  appendHashByte(hashes.rgba8, sample.color.a);
  appendHashByte(hashes.maskR8, sample.mask);
  appendHashU32(hashes.depthD32, std::bit_cast<uint32_t>(sample.depth));
  appendHashByte(hashes.donorRgb5, sample.color.r >> 3);
  appendHashByte(hashes.donorRgb5, sample.color.g >> 3);
  appendHashByte(hashes.donorRgb5, sample.color.b >> 3);
  const uint8_t covered =
      sample.color.r != 0 || sample.color.g != 0 || sample.color.b != 0 ? 1u
                                                                        : 0u;
  appendHashByte(hashes.coverage, covered);
  hashes.nonBlackPixels += covered;
}

void recordActualSurfaceDepthCoverage(VulkanReplayDiagnosticReport &report,
                                      const Color32 &color, float depth) {
  if (!std::isfinite(depth) || depth < 0.0f || depth > 1.0f)
    return;
  const bool nonBlack = color.r != 0 || color.g != 0 || color.b != 0;
  if (depth < 1.0f) {
    ++report.actualSurfaceDepthPixels;
    report.actualNonBlackSurfaceDepthPixels += nonBlack ? 1u : 0u;
    return;
  }
  ++report.actualSurfaceDepthSentinelPixels;
  report.actualNonBlackSurfaceDepthSentinelPixels += nonBlack ? 1u : 0u;
}

float normalizeOracleDepth(float depth, bool &invalid) {
  invalid = false;
  if (std::isinf(depth) && depth > 0.0f)
    return 1.0f;
  const auto normalized =
      vulkan_abi::tryNormalizeProjectiveWDepth(static_cast<double>(depth));
  if (normalized.has_value())
    return *normalized;
  invalid = true;
  return 1.0f;
}

ExpectedSample sampleOracle(const VulkanReplayDiagnosticOracleView &oracle,
                            const VulkanReplayDiagnosticRegion &region,
                            uint32_t localX, uint32_t localY) {
  const uint32_t atlasX = region.nativeX * region.internalScale + localX;
  const uint32_t atlasY = region.nativeY * region.internalScale + localY;
  switch (oracle.kind) {
  case VulkanReplayDiagnosticOracleKind::EnhancedSurface: {
    const float rawDepth = oracle.enhancedSurface->depthAt(atlasX, atlasY);
    ExpectedSample sample{
        oracle.enhancedSurface->colorAt(atlasX, atlasY),
        static_cast<uint8_t>(
            oracle.enhancedSurface->maskAt(atlasX, atlasY) ? 1u : 0u)};
    sample.depth = normalizeOracleDepth(rawDepth, sample.invalidDepth);
    return sample;
  }
  case VulkanReplayDiagnosticOracleKind::NativeSeed: {
    const uint32_t nativeX = atlasX / region.internalScale;
    const uint32_t nativeY = atlasY / region.internalScale;
    const uint16_t raw = oracle
                             .nativePixels[static_cast<std::size_t>(nativeY) *
                                               oracle.nativeWidth +
                                           nativeX]
                             .raw;
    return {EnhancedSurface::expandNativeColor(raw),
            static_cast<uint8_t>((raw & 0x8000u) != 0 ? 1u : 0u), 1.0f, false};
  }
  case VulkanReplayDiagnosticOracleKind::AtlasPlanes: {
    ExpectedSample sample{
        loadPlane<Color32>(oracle.atlasPlanes.colorRgba8, localX, localY),
        loadPlane<uint8_t>(oracle.atlasPlanes.maskR8, localX, localY)};
    const float rawDepth =
        loadPlane<float>(oracle.atlasPlanes.depthD32, localX, localY);
    if (oracle.atlasDepthEncoding ==
        VulkanReplayDiagnosticDepthEncoding::ProjectiveWInfinityFar) {
      sample.depth = normalizeOracleDepth(rawDepth, sample.invalidDepth);
    } else {
      sample.depth = rawDepth;
      sample.invalidDepth =
          !std::isfinite(rawDepth) || rawDepth < 0.0f || rawDepth > 1.0f;
    }
    return sample;
  }
  }
  return {};
}

bool validateOracle(const VulkanReplayDiagnosticOracleView &oracle,
                    const VulkanReplayDiagnosticRegion &region,
                    uint32_t scaledWidth, uint32_t scaledHeight,
                    VulkanReplayDiagnosticStatus &failure) {
  if (oracle.internalScale != region.internalScale) {
    failure = VulkanReplayDiagnosticStatus::OracleScaleMismatch;
    return false;
  }
  const auto regionFitsNativeExtent = [&]() {
    return region.nativeX <= oracle.nativeWidth &&
           region.nativeWidth <= oracle.nativeWidth - region.nativeX &&
           region.nativeY <= oracle.nativeHeight &&
           region.nativeHeight <= oracle.nativeHeight - region.nativeY;
  };
  switch (oracle.kind) {
  case VulkanReplayDiagnosticOracleKind::EnhancedSurface:
    if (!regionFitsNativeExtent()) {
      failure = VulkanReplayDiagnosticStatus::RegionOutOfBounds;
      return false;
    }
    if (oracle.enhancedSurface == nullptr ||
        !oracle.enhancedSurface->enabled() ||
        oracle.enhancedSurface->nativeWidth() != oracle.nativeWidth ||
        oracle.enhancedSurface->nativeHeight() != oracle.nativeHeight ||
        oracle.enhancedSurface->scale() != oracle.internalScale) {
      failure = VulkanReplayDiagnosticStatus::InvalidOracle;
      return false;
    }
    return true;
  case VulkanReplayDiagnosticOracleKind::NativeSeed: {
    std::size_t required = 0;
    if (!regionFitsNativeExtent()) {
      failure = VulkanReplayDiagnosticStatus::RegionOutOfBounds;
      return false;
    }
    if (oracle.nativePixels == nullptr ||
        !checkedMultiply(oracle.nativeWidth, oracle.nativeHeight, required) ||
        oracle.nativePixelCount < required) {
      failure = VulkanReplayDiagnosticStatus::InvalidOracle;
      return false;
    }
    return true;
  }
  case VulkanReplayDiagnosticOracleKind::AtlasPlanes: {
    const VulkanReplayDiagnosticRegion &captured = oracle.atlasRegion;
    if (captured.nativeX != region.nativeX ||
        captured.nativeY != region.nativeY ||
        captured.nativeWidth != region.nativeWidth ||
        captured.nativeHeight != region.nativeHeight ||
        captured.internalScale != region.internalScale) {
      failure = VulkanReplayDiagnosticStatus::RegionOutOfBounds;
      return false;
    }
    if (!planesCanAddress(oracle.atlasPlanes, scaledWidth, scaledHeight)) {
      failure = VulkanReplayDiagnosticStatus::InvalidOraclePlane;
      return false;
    }
    return true;
  }
  }
  failure = VulkanReplayDiagnosticStatus::InvalidOracle;
  return false;
}

uint32_t depthUlpDistance(float actual, float expected) {
  if (!std::isfinite(actual) || !std::isfinite(expected) || actual < 0.0f ||
      expected < 0.0f)
    return std::numeric_limits<uint32_t>::max();
  const uint32_t actualBits = std::bit_cast<uint32_t>(actual);
  const uint32_t expectedBits = std::bit_cast<uint32_t>(expected);
  return actualBits > expectedBits ? actualBits - expectedBits
                                   : expectedBits - actualBits;
}

bool depthMatches(float actual, float expected, uint32_t maximumUlps,
                  uint32_t &ulpDistance, bool &invalidActual) {
  invalidActual = !std::isfinite(actual) || actual < 0.0f || actual > 1.0f;
  ulpDistance = depthUlpDistance(actual, expected);
  if (invalidActual || !std::isfinite(expected) || expected < 0.0f ||
      expected > 1.0f)
    return false;
  if (actual == 1.0f || expected == 1.0f)
    return actual == expected;
  return ulpDistance <= maximumUlps;
}

int64_t metricBits(uint64_t value) { return std::bit_cast<int64_t>(value); }

std::string metricName(std::string_view prefix, std::string_view suffix) {
  std::string result(prefix);
  result.append(suffix);
  return result;
}

} // namespace

VulkanReplayDiagnosticOracleView
VulkanReplayDiagnosticOracleView::fromEnhancedSurface(
    const EnhancedSurface &surface) {
  VulkanReplayDiagnosticOracleView view;
  view.kind = VulkanReplayDiagnosticOracleKind::EnhancedSurface;
  view.enhancedSurface = &surface;
  view.nativeWidth = surface.nativeWidth();
  view.nativeHeight = surface.nativeHeight();
  view.internalScale = surface.scale();
  return view;
}

VulkanReplayDiagnosticOracleView
VulkanReplayDiagnosticOracleView::fromNativeSeed(const Color16 *pixels,
                                                 std::size_t pixelCount,
                                                 uint32_t width,
                                                 uint32_t height,
                                                 uint32_t scale) {
  VulkanReplayDiagnosticOracleView view;
  view.kind = VulkanReplayDiagnosticOracleKind::NativeSeed;
  view.nativePixels = pixels;
  view.nativePixelCount = pixelCount;
  view.nativeWidth = width;
  view.nativeHeight = height;
  view.internalScale = scale;
  return view;
}

VulkanReplayDiagnosticOracleView
VulkanReplayDiagnosticOracleView::fromAtlasPlanes(
    VulkanReplayDiagnosticPlanes planes, uint32_t nativeWidth,
    uint32_t nativeHeight, uint32_t scale,
    VulkanReplayDiagnosticDepthEncoding depthEncoding) {
  return fromAtlasPlanes(
      planes, {0, 0, nativeWidth, nativeHeight, scale}, depthEncoding);
}

VulkanReplayDiagnosticOracleView
VulkanReplayDiagnosticOracleView::fromAtlasPlanes(
    VulkanReplayDiagnosticPlanes planes,
    const VulkanReplayDiagnosticRegion &capturedRegion,
    VulkanReplayDiagnosticDepthEncoding depthEncoding) {
  VulkanReplayDiagnosticOracleView view;
  view.kind = VulkanReplayDiagnosticOracleKind::AtlasPlanes;
  view.atlasPlanes = planes;
  view.nativeWidth = capturedRegion.nativeWidth;
  view.nativeHeight = capturedRegion.nativeHeight;
  view.internalScale = capturedRegion.internalScale;
  view.atlasRegion = capturedRegion;
  view.atlasDepthEncoding = depthEncoding;
  return view;
}

const char *
vulkanReplayDiagnosticStatusName(VulkanReplayDiagnosticStatus status) {
  switch (status) {
  case VulkanReplayDiagnosticStatus::Compared:
    return "compared";
  case VulkanReplayDiagnosticStatus::CapturedWithoutOracle:
    return "captured-without-oracle";
  case VulkanReplayDiagnosticStatus::EmptyRegion:
    return "empty-region";
  case VulkanReplayDiagnosticStatus::InvalidScale:
    return "invalid-scale";
  case VulkanReplayDiagnosticStatus::RegionOutOfBounds:
    return "region-out-of-bounds";
  case VulkanReplayDiagnosticStatus::InvalidReadbackPlane:
    return "invalid-readback-plane";
  case VulkanReplayDiagnosticStatus::InvalidOracle:
    return "invalid-oracle";
  case VulkanReplayDiagnosticStatus::OracleScaleMismatch:
    return "oracle-scale-mismatch";
  case VulkanReplayDiagnosticStatus::InvalidOraclePlane:
    return "invalid-oracle-plane";
  }
  return "unknown";
}

VulkanReplayDiagnosticReport
diagnoseVulkanReplayReadback(const VulkanReplayDiagnosticRegion &region,
                             const VulkanReplayDiagnosticPlanes &actual,
                             const VulkanReplayDiagnosticOracleView &oracle,
                             VulkanReplayDiagnosticOptions options) {
  VulkanReplayDiagnosticReport report;
  report.region = region;
  if (region.nativeWidth == 0 || region.nativeHeight == 0) {
    report.status = VulkanReplayDiagnosticStatus::EmptyRegion;
    return report;
  }
  if (region.internalScale == 0 ||
      region.internalScale > EnhancedSurface::MAX_SCALE) {
    report.status = VulkanReplayDiagnosticStatus::InvalidScale;
    return report;
  }
  const uint64_t scaledEndX =
      (static_cast<uint64_t>(region.nativeX) + region.nativeWidth) *
      region.internalScale;
  const uint64_t scaledEndY =
      (static_cast<uint64_t>(region.nativeY) + region.nativeHeight) *
      region.internalScale;
  if (scaledEndX > std::numeric_limits<uint32_t>::max() ||
      scaledEndY > std::numeric_limits<uint32_t>::max()) {
    report.status = VulkanReplayDiagnosticStatus::RegionOutOfBounds;
    return report;
  }

  std::size_t scaledWidthValue = 0;
  std::size_t scaledHeightValue = 0;
  if (!checkedMultiply(region.nativeWidth, region.internalScale,
                       scaledWidthValue) ||
      !checkedMultiply(region.nativeHeight, region.internalScale,
                       scaledHeightValue) ||
      scaledWidthValue > std::numeric_limits<uint32_t>::max() ||
      scaledHeightValue > std::numeric_limits<uint32_t>::max()) {
    report.status = VulkanReplayDiagnosticStatus::RegionOutOfBounds;
    return report;
  }
  const uint32_t scaledWidth = static_cast<uint32_t>(scaledWidthValue);
  const uint32_t scaledHeight = static_cast<uint32_t>(scaledHeightValue);
  if (!planesCanAddress(actual, scaledWidth, scaledHeight)) {
    report.status = VulkanReplayDiagnosticStatus::InvalidReadbackPlane;
    return report;
  }
  if (!validateOracle(oracle, region, scaledWidth, scaledHeight, report.status))
    return report;

  report.status = VulkanReplayDiagnosticStatus::Compared;
  report.actual = {kFnv1a64Offset, kFnv1a64Offset, kFnv1a64Offset,
                   kFnv1a64Offset, kFnv1a64Offset, 0};
  report.expected = report.actual;
  report.mismatches.reserve(std::min<std::size_t>(
      options.maximumRecordedMismatches,
      static_cast<std::size_t>(scaledWidth) * scaledHeight));

  for (uint32_t y = 0; y < scaledHeight; ++y) {
    for (uint32_t x = 0; x < scaledWidth; ++x) {
      const Color32 actualColor = loadPlane<Color32>(actual.colorRgba8, x, y);
      const uint8_t actualMask = loadPlane<uint8_t>(actual.maskR8, x, y);
      const float actualDepth = loadPlane<float>(actual.depthD32, x, y);
      const ExpectedSample expected = sampleOracle(oracle, region, x, y);
      const ExpectedSample actualSample{actualColor, actualMask, actualDepth,
                                        false};
      appendSampleHashes(report.actual, actualSample);
      appendSampleHashes(report.expected, expected);
      recordActualSurfaceDepthCoverage(report, actualColor, actualDepth);

      uint8_t mismatchPlanes = 0;
      if (!(actualColor == expected.color)) {
        mismatchPlanes |= VulkanReplayMismatchColor;
        ++report.colorMismatchPixels;
      }
      if (actualMask != expected.mask) {
        mismatchPlanes |= VulkanReplayMismatchMask;
        ++report.maskMismatchPixels;
      }
      uint32_t ulpDistance = 0;
      bool invalidActualDepth = false;
      const bool matchingDepth =
          depthMatches(actualDepth, expected.depth, options.maximumDepthUlps,
                       ulpDistance, invalidActualDepth);
      if (!matchingDepth || expected.invalidDepth) {
        mismatchPlanes |= VulkanReplayMismatchDepth;
        ++report.depthMismatchPixels;
      }
      if (invalidActualDepth)
        ++report.actualInvalidDepthPixels;
      if (expected.invalidDepth)
        ++report.oracleInvalidDepthPixels;
      if (ulpDistance != std::numeric_limits<uint32_t>::max()) {
        report.maximumObservedDepthUlps =
            std::max(report.maximumObservedDepthUlps, ulpDistance);
      }

      if (mismatchPlanes != 0) {
        ++report.anyMismatchPixels;
        if (report.mismatches.size() < options.maximumRecordedMismatches) {
          report.mismatches.push_back(
              {region.nativeX * region.internalScale + x,
               region.nativeY * region.internalScale + y, mismatchPlanes,
               actualColor, expected.color, actualMask, expected.mask,
               std::bit_cast<uint32_t>(actualDepth),
               std::bit_cast<uint32_t>(expected.depth), ulpDistance});
        }
      }
      ++report.comparedPixels;
    }
  }
  return report;
}

VulkanReplayDiagnosticReport
captureVulkanReplayReadback(const VulkanReplayDiagnosticRegion &region,
                            const VulkanReplayDiagnosticPlanes &actual) {
  VulkanReplayDiagnosticReport report;
  report.region = region;
  if (region.nativeWidth == 0 || region.nativeHeight == 0) {
    report.status = VulkanReplayDiagnosticStatus::EmptyRegion;
    return report;
  }
  if (region.internalScale == 0 ||
      region.internalScale > EnhancedSurface::MAX_SCALE) {
    report.status = VulkanReplayDiagnosticStatus::InvalidScale;
    return report;
  }
  const uint64_t scaledEndX =
      (static_cast<uint64_t>(region.nativeX) + region.nativeWidth) *
      region.internalScale;
  const uint64_t scaledEndY =
      (static_cast<uint64_t>(region.nativeY) + region.nativeHeight) *
      region.internalScale;
  std::size_t scaledWidthValue = 0;
  std::size_t scaledHeightValue = 0;
  if (scaledEndX > std::numeric_limits<uint32_t>::max() ||
      scaledEndY > std::numeric_limits<uint32_t>::max() ||
      !checkedMultiply(region.nativeWidth, region.internalScale,
                       scaledWidthValue) ||
      !checkedMultiply(region.nativeHeight, region.internalScale,
                       scaledHeightValue) ||
      scaledWidthValue > std::numeric_limits<uint32_t>::max() ||
      scaledHeightValue > std::numeric_limits<uint32_t>::max()) {
    report.status = VulkanReplayDiagnosticStatus::RegionOutOfBounds;
    return report;
  }
  const uint32_t scaledWidth = static_cast<uint32_t>(scaledWidthValue);
  const uint32_t scaledHeight = static_cast<uint32_t>(scaledHeightValue);
  if (!planesCanAddress(actual, scaledWidth, scaledHeight)) {
    report.status = VulkanReplayDiagnosticStatus::InvalidReadbackPlane;
    return report;
  }

  report.status = VulkanReplayDiagnosticStatus::CapturedWithoutOracle;
  report.actual = {kFnv1a64Offset, kFnv1a64Offset, kFnv1a64Offset,
                   kFnv1a64Offset, kFnv1a64Offset, 0};
  for (uint32_t y = 0; y < scaledHeight; ++y) {
    for (uint32_t x = 0; x < scaledWidth; ++x) {
      const ExpectedSample sample{loadPlane<Color32>(actual.colorRgba8, x, y),
                                  loadPlane<uint8_t>(actual.maskR8, x, y),
                                  loadPlane<float>(actual.depthD32, x, y),
                                  false};
      appendSampleHashes(report.actual, sample);
      recordActualSurfaceDepthCoverage(report, sample.color, sample.depth);
      if (!std::isfinite(sample.depth) || sample.depth < 0.0f ||
          sample.depth > 1.0f)
        ++report.actualInvalidDepthPixels;
      ++report.comparedPixels;
    }
  }
  return report;
}

void publishVulkanReplayDiagnosticMetrics(
    const VulkanReplayDiagnosticReport &report, uint64_t guestFrame,
    std::string_view prefix) {
  ps1::metrics::count(metricName(prefix, ".capture"));
  ps1::metrics::setState(metricName(prefix, ".last.status"),
                         static_cast<int64_t>(report.status));
  if (!report.captured()) {
    ps1::metrics::setState(metricName(prefix, ".last.frame"),
                           static_cast<int64_t>(guestFrame));
    ps1::metrics::setState(metricName(prefix, ".last.has_oracle"), 0);
    ps1::metrics::setState(metricName(prefix, ".last.matches"), 0);
    ps1::metrics::setState(metricName(prefix, ".last.actual_rgba8_hash"), 0);
    ps1::metrics::setState(metricName(prefix, ".last.expected_rgba8_hash"), 0);
    ps1::metrics::setState(metricName(prefix, ".last.actual_mask_hash"), 0);
    ps1::metrics::setState(metricName(prefix, ".last.expected_mask_hash"), 0);
    ps1::metrics::setState(metricName(prefix, ".last.actual_depth_hash"), 0);
    ps1::metrics::setState(metricName(prefix, ".last.expected_depth_hash"), 0);
    ps1::metrics::setState(metricName(prefix, ".last.actual_donor_rgb5_hash"),
                           0);
    ps1::metrics::setState(
        metricName(prefix, ".last.expected_donor_rgb5_hash"), 0);
    ps1::metrics::setState(metricName(prefix, ".last.actual_coverage_hash"),
                           0);
    ps1::metrics::setState(
        metricName(prefix, ".last.expected_coverage_hash"), 0);
    ps1::metrics::setState(metricName(prefix, ".last.maximum_depth_ulps"), 0);
    ps1::metrics::setState(metricName(prefix, ".last.actual_nonblack"), 0);
    ps1::metrics::setState(metricName(prefix, ".last.expected_nonblack"), 0);
    ps1::metrics::setState(
        metricName(prefix, ".last.actual_surface_depth_pixels"), 0);
    ps1::metrics::setState(
        metricName(prefix, ".last.actual_surface_depth_sentinel_pixels"), 0);
    ps1::metrics::setState(
        metricName(prefix, ".last.actual_nonblack_surface_depth_pixels"), 0);
    ps1::metrics::setState(
        metricName(prefix,
                   ".last.actual_nonblack_surface_depth_sentinel_pixels"),
        0);
    ps1::metrics::setState(
        metricName(prefix, ".last.mismatch_records_truncated"), 0);
    ps1::metrics::setState(metricName(prefix, ".last.first_mismatch_x"), -1);
    ps1::metrics::setState(metricName(prefix, ".last.first_mismatch_y"), -1);
    ps1::metrics::setState(metricName(prefix, ".last.first_mismatch_planes"),
                           0);
    ps1::metrics::traceAtFrame(
        "host.vulkan_replay_readback", guestFrame,
        {{"status", static_cast<int64_t>(report.status)},
         {"native_x", report.region.nativeX},
         {"native_y", report.region.nativeY},
         {"native_w", report.region.nativeWidth},
         {"native_h", report.region.nativeHeight},
         {"internal_scale", report.region.internalScale}});
    return;
  }

  ps1::metrics::count(metricName(prefix, ".pixels"), report.comparedPixels);
  if (report.compared()) {
    ps1::metrics::count(metricName(prefix, ".color_mismatch_pixels"),
                        report.colorMismatchPixels);
    ps1::metrics::count(metricName(prefix, ".mask_mismatch_pixels"),
                        report.maskMismatchPixels);
    ps1::metrics::count(metricName(prefix, ".depth_mismatch_pixels"),
                        report.depthMismatchPixels);
    ps1::metrics::count(metricName(prefix, ".any_mismatch_pixels"),
                        report.anyMismatchPixels);
    ps1::metrics::count(metricName(prefix, ".invalid_oracle_depth_pixels"),
                        report.oracleInvalidDepthPixels);
    ps1::metrics::count(metricName(prefix, ".recorded_mismatches"),
                        report.mismatches.size());
  }
  ps1::metrics::count(metricName(prefix, ".invalid_actual_depth_pixels"),
                      report.actualInvalidDepthPixels);
  ps1::metrics::count(metricName(prefix, ".surface_depth_pixels"),
                      report.actualSurfaceDepthPixels);
  ps1::metrics::count(metricName(prefix, ".surface_depth_sentinel_pixels"),
                      report.actualSurfaceDepthSentinelPixels);
  ps1::metrics::count(metricName(prefix, ".nonblack_surface_depth_pixels"),
                      report.actualNonBlackSurfaceDepthPixels);
  ps1::metrics::count(
      metricName(prefix, ".nonblack_surface_depth_sentinel_pixels"),
      report.actualNonBlackSurfaceDepthSentinelPixels);
  ps1::metrics::setState(metricName(prefix, ".last.has_oracle"),
                         report.compared() ? 1 : 0);
  ps1::metrics::setState(metricName(prefix, ".last.matches"),
                         report.matches() ? 1 : 0);
  ps1::metrics::setState(metricName(prefix, ".last.frame"),
                         static_cast<int64_t>(guestFrame));
  ps1::metrics::setState(metricName(prefix, ".last.actual_rgba8_hash"),
                         metricBits(report.actual.rgba8));
  ps1::metrics::setState(metricName(prefix, ".last.expected_rgba8_hash"),
                         report.compared() ? metricBits(report.expected.rgba8)
                                           : 0);
  ps1::metrics::setState(metricName(prefix, ".last.actual_mask_hash"),
                         metricBits(report.actual.maskR8));
  ps1::metrics::setState(metricName(prefix, ".last.expected_mask_hash"),
                         report.compared() ? metricBits(report.expected.maskR8)
                                           : 0);
  ps1::metrics::setState(metricName(prefix, ".last.actual_depth_hash"),
                         metricBits(report.actual.depthD32));
  ps1::metrics::setState(
      metricName(prefix, ".last.expected_depth_hash"),
      report.compared() ? metricBits(report.expected.depthD32) : 0);
  ps1::metrics::setState(metricName(prefix, ".last.actual_donor_rgb5_hash"),
                         metricBits(report.actual.donorRgb5));
  ps1::metrics::setState(
      metricName(prefix, ".last.expected_donor_rgb5_hash"),
      report.compared() ? metricBits(report.expected.donorRgb5) : 0);
  ps1::metrics::setState(metricName(prefix, ".last.actual_coverage_hash"),
                         metricBits(report.actual.coverage));
  ps1::metrics::setState(
      metricName(prefix, ".last.expected_coverage_hash"),
      report.compared() ? metricBits(report.expected.coverage) : 0);
  ps1::metrics::setState(metricName(prefix, ".last.maximum_depth_ulps"),
                         report.maximumObservedDepthUlps);
  ps1::metrics::setState(metricName(prefix, ".last.actual_nonblack"),
                         static_cast<int64_t>(report.actual.nonBlackPixels));
  ps1::metrics::setState(
      metricName(prefix, ".last.expected_nonblack"),
      report.compared() ? static_cast<int64_t>(report.expected.nonBlackPixels)
                        : 0);
  ps1::metrics::setState(
      metricName(prefix, ".last.actual_surface_depth_pixels"),
      static_cast<int64_t>(report.actualSurfaceDepthPixels));
  ps1::metrics::setState(
      metricName(prefix, ".last.actual_surface_depth_sentinel_pixels"),
      static_cast<int64_t>(report.actualSurfaceDepthSentinelPixels));
  ps1::metrics::setState(
      metricName(prefix, ".last.actual_nonblack_surface_depth_pixels"),
      static_cast<int64_t>(report.actualNonBlackSurfaceDepthPixels));
  ps1::metrics::setState(
      metricName(prefix,
                 ".last.actual_nonblack_surface_depth_sentinel_pixels"),
      static_cast<int64_t>(
          report.actualNonBlackSurfaceDepthSentinelPixels));
  ps1::metrics::setState(
      metricName(prefix, ".last.mismatch_records_truncated"),
      report.anyMismatchPixels > report.mismatches.size() ? 1 : 0);
  ps1::metrics::setState(metricName(prefix, ".last.first_mismatch_x"), -1);
  ps1::metrics::setState(metricName(prefix, ".last.first_mismatch_y"), -1);
  ps1::metrics::setState(metricName(prefix, ".last.first_mismatch_planes"), 0);
  if (report.compared() && !report.mismatches.empty()) {
    ps1::metrics::setState(metricName(prefix, ".last.first_mismatch_x"),
                           report.mismatches.front().atlasX);
    ps1::metrics::setState(metricName(prefix, ".last.first_mismatch_y"),
                           report.mismatches.front().atlasY);
    ps1::metrics::setState(metricName(prefix, ".last.first_mismatch_planes"),
                           report.mismatches.front().planes);
  }
  ps1::metrics::traceAtFrame(
      "host.vulkan_replay_readback", guestFrame,
      {{"status", static_cast<int64_t>(report.status)},
       {"native_x", report.region.nativeX},
       {"native_y", report.region.nativeY},
       {"native_w", report.region.nativeWidth},
       {"native_h", report.region.nativeHeight},
       {"internal_scale", report.region.internalScale},
       {"pixels", static_cast<int64_t>(report.comparedPixels)},
       {"has_oracle", report.compared() ? 1 : 0},
       {"color_mismatch", static_cast<int64_t>(report.colorMismatchPixels)},
       {"mask_mismatch", static_cast<int64_t>(report.maskMismatchPixels)},
       {"depth_mismatch", static_cast<int64_t>(report.depthMismatchPixels)},
       {"invalid_actual_depth",
        static_cast<int64_t>(report.actualInvalidDepthPixels)},
       {"invalid_oracle_depth",
        static_cast<int64_t>(report.oracleInvalidDepthPixels)},
       {"surface_depth_pixels",
        static_cast<int64_t>(report.actualSurfaceDepthPixels)},
       {"surface_depth_sentinel_pixels",
        static_cast<int64_t>(report.actualSurfaceDepthSentinelPixels)},
       {"nonblack_surface_depth_pixels",
        static_cast<int64_t>(report.actualNonBlackSurfaceDepthPixels)},
       {"nonblack_surface_depth_sentinel_pixels",
        static_cast<int64_t>(
            report.actualNonBlackSurfaceDepthSentinelPixels)},
       {"actual_rgba8_hash", metricBits(report.actual.rgba8)},
       {"expected_rgba8_hash", metricBits(report.expected.rgba8)},
       {"actual_mask_hash", metricBits(report.actual.maskR8)},
       {"expected_mask_hash", metricBits(report.expected.maskR8)},
       {"actual_depth_hash", metricBits(report.actual.depthD32)},
       {"expected_depth_hash", metricBits(report.expected.depthD32)},
       {"actual_donor_rgb5_hash", metricBits(report.actual.donorRgb5)},
       {"expected_donor_rgb5_hash", metricBits(report.expected.donorRgb5)},
       {"actual_coverage_hash", metricBits(report.actual.coverage)},
       {"expected_coverage_hash", metricBits(report.expected.coverage)}});
}

} // namespace ps1::gpu::replay
