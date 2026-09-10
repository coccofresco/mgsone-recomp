#pragma once

#include "runtime/gpu/enhanced_surface.h"
#include "runtime/gpu/native_color.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ps1::gpu::replay {

// A native-coordinate rectangle whose Vulkan counterpart is an S-times-larger
// rectangle in the persistent replay atlas. Diagnostic captures deliberately
// reject wrapping rectangles: callers split those into bounded rectangles so
// every mismatch retains an unambiguous absolute atlas coordinate.
struct VulkanReplayDiagnosticRegion {
  uint32_t nativeX = 0;
  uint32_t nativeY = 0;
  uint32_t nativeWidth = 0;
  uint32_t nativeHeight = 0;
  uint32_t internalScale = 1;
};

// CPU mapping of one tightly formatted VkBufferImageCopy destination. Vulkan
// permits padded rows; sizeBytes includes that padding. The comparator never
// hashes padding, making captures stable across devices with different buffer
// alignment requirements.
struct VulkanReplayDiagnosticPlaneView {
  const void *data = nullptr;
  std::size_t sizeBytes = 0;
  std::size_t rowPitchBytes = 0;
};

struct VulkanReplayDiagnosticPlanes {
  VulkanReplayDiagnosticPlaneView colorRgba8{};
  VulkanReplayDiagnosticPlaneView maskR8{};
  VulkanReplayDiagnosticPlaneView depthD32{};
};

enum class VulkanReplayDiagnosticOracleKind : uint8_t {
  EnhancedSurface = 0,
  NativeSeed,
  AtlasPlanes,
};

enum class VulkanReplayDiagnosticDepthEncoding : uint8_t {
  // Bytes are already VK_FORMAT_D32_SFLOAT values in the shared [0,1]
  // replay depth domain.
  NormalizedD32 = 0,
  // Values are the software oracle's raw projective W. Positive finite W is
  // normalized by 2^-32; infinity denotes an invalidated/far sample.
  ProjectiveWInfinityFar,
};

// The comparison core accepts all three useful oracle forms:
//
//  * EnhancedSurface for an in-process scalar replay;
//  * authoritative RGB555 native VRAM for M0/A0 expansion checks;
//  * already materialized atlas planes for deterministic offline captures.
//
// The pointed-to storage must outlive diagnoseVulkanReplayReadback(). Use the
// named factories so dimensions and depth encoding are explicit at call sites.
struct VulkanReplayDiagnosticOracleView {
  VulkanReplayDiagnosticOracleKind kind =
      VulkanReplayDiagnosticOracleKind::EnhancedSurface;
  const EnhancedSurface *enhancedSurface = nullptr;
  const Color16 *nativePixels = nullptr;
  std::size_t nativePixelCount = 0;
  uint32_t nativeWidth = 0;
  uint32_t nativeHeight = 0;
  uint32_t internalScale = 1;
  VulkanReplayDiagnosticPlanes atlasPlanes{};
  // Atlas-plane artifacts are tightly cropped. Their absolute origin and
  // extent are therefore part of the oracle identity: accepting an artifact
  // captured for another same-sized crop would produce a false comparison.
  VulkanReplayDiagnosticRegion atlasRegion{};
  VulkanReplayDiagnosticDepthEncoding atlasDepthEncoding =
      VulkanReplayDiagnosticDepthEncoding::NormalizedD32;

  static VulkanReplayDiagnosticOracleView
  fromEnhancedSurface(const EnhancedSurface &surface);

  static VulkanReplayDiagnosticOracleView
  fromNativeSeed(const Color16 *pixels, std::size_t pixelCount, uint32_t width,
                 uint32_t height, uint32_t scale);

  static VulkanReplayDiagnosticOracleView
  fromAtlasPlanes(VulkanReplayDiagnosticPlanes planes, uint32_t nativeWidth,
                  uint32_t nativeHeight, uint32_t scale,
                  VulkanReplayDiagnosticDepthEncoding depthEncoding =
                      VulkanReplayDiagnosticDepthEncoding::NormalizedD32);

  static VulkanReplayDiagnosticOracleView
  fromAtlasPlanes(VulkanReplayDiagnosticPlanes planes,
                  const VulkanReplayDiagnosticRegion &capturedRegion,
                  VulkanReplayDiagnosticDepthEncoding depthEncoding =
                      VulkanReplayDiagnosticDepthEncoding::NormalizedD32);
};

enum class VulkanReplayDiagnosticStatus : uint8_t {
  Compared = 0,
  EmptyRegion = 1,
  InvalidScale = 2,
  RegionOutOfBounds = 3,
  InvalidReadbackPlane = 4,
  InvalidOracle = 5,
  OracleScaleMismatch = 6,
  InvalidOraclePlane = 7,
  // Append-only: status values are published to metrics and JSON traces.
  CapturedWithoutOracle = 8,
};

enum VulkanReplayDiagnosticMismatchPlane : uint8_t {
  VulkanReplayMismatchColor = 1u << 0,
  VulkanReplayMismatchMask = 1u << 1,
  VulkanReplayMismatchDepth = 1u << 2,
};

struct VulkanReplayDiagnosticSampleMismatch {
  uint32_t atlasX = 0;
  uint32_t atlasY = 0;
  uint8_t planes = 0;
  Color32 actualColor{};
  Color32 expectedColor{};
  uint8_t actualMask = 0;
  uint8_t expectedMask = 0;
  uint32_t actualDepthBits = 0;
  uint32_t expectedDepthBits = 0;
  uint32_t depthUlpDistance = 0;
};

struct VulkanReplayDiagnosticHashes {
  // Exact row-major attachment hashes. All hashes use standard 64-bit FNV-1a
  // and exclude row padding.
  uint64_t rgba8 = 0;
  uint64_t maskR8 = 0;
  uint64_t depthD32 = 0;
  // RGB components quantized as RGB8>>3, matching donor_trace's portable
  // SwanStation/Beetle visual hash. Alpha is deliberately excluded.
  uint64_t donorRgb5 = 0;
  // One byte per sample (0 black, 1 non-black), useful for separating missing
  // coverage from color-equation differences.
  uint64_t coverage = 0;
  uint64_t nonBlackPixels = 0;
};

struct VulkanReplayDiagnosticOptions {
  // Raster interpolation can differ by a few representable float values
  // between the scalar oracle and Vulkan. Far (1.0), NaN, infinity and values
  // outside [0,1] are always compared strictly.
  uint32_t maximumDepthUlps = 4;
  std::size_t maximumRecordedMismatches = 64;
};

struct VulkanReplayDiagnosticReport {
  VulkanReplayDiagnosticStatus status =
      VulkanReplayDiagnosticStatus::InvalidReadbackPlane;
  VulkanReplayDiagnosticRegion region{};
  uint64_t comparedPixels = 0;
  uint64_t colorMismatchPixels = 0;
  uint64_t maskMismatchPixels = 0;
  uint64_t depthMismatchPixels = 0;
  uint64_t anyMismatchPixels = 0;
  uint64_t actualInvalidDepthPixels = 0;
  uint64_t oracleInvalidDepthPixels = 0;
  // R32 presented-surface coverage. A value below 1.0 carries a surface
  // coordinate; exactly 1.0 is the explicit unresolved/2D/native sentinel.
  // Invalid values are counted separately and never enter either bucket.
  uint64_t actualSurfaceDepthPixels = 0;
  uint64_t actualSurfaceDepthSentinelPixels = 0;
  uint64_t actualNonBlackSurfaceDepthPixels = 0;
  uint64_t actualNonBlackSurfaceDepthSentinelPixels = 0;
  uint32_t maximumObservedDepthUlps = 0;
  VulkanReplayDiagnosticHashes actual{};
  VulkanReplayDiagnosticHashes expected{};
  std::vector<VulkanReplayDiagnosticSampleMismatch> mismatches;

  bool compared() const {
    return status == VulkanReplayDiagnosticStatus::Compared;
  }

  bool captured() const {
    return compared() ||
           status == VulkanReplayDiagnosticStatus::CapturedWithoutOracle;
  }

  bool matches() const {
    return compared() && anyMismatchPixels == 0 &&
           actualInvalidDepthPixels == 0 && oracleInvalidDepthPixels == 0;
  }
};

const char *
vulkanReplayDiagnosticStatusName(VulkanReplayDiagnosticStatus status);

VulkanReplayDiagnosticReport
diagnoseVulkanReplayReadback(const VulkanReplayDiagnosticRegion &region,
                             const VulkanReplayDiagnosticPlanes &actual,
                             const VulkanReplayDiagnosticOracleView &oracle,
                             VulkanReplayDiagnosticOptions options = {});

// Hash and validate a mapped Vulkan capture when no scalar/offline oracle is
// available for this latch. This never labels the capture as a match; it is
// the actual-side artifact used to compare a later deterministic oracle run.
VulkanReplayDiagnosticReport
captureVulkanReplayReadback(const VulkanReplayDiagnosticRegion &region,
                            const VulkanReplayDiagnosticPlanes &actual);

// Publish aggregate counters, last-capture hashes and one trace event. This is
// intentionally separate from comparison so unit tests and offline tools stay
// side-effect free. `prefix` should be a stable metric namespace.
void publishVulkanReplayDiagnosticMetrics(
    const VulkanReplayDiagnosticReport &report, uint64_t guestFrame,
    std::string_view prefix = "host.renderer.vulkan_replay_readback");

} // namespace ps1::gpu::replay
