#include "runtime/gpu/modern_replay_vulkan_abi.h"
#include "runtime/gpu/modern_replay_vulkan_diagnostics.h"
#include "runtime/metrics.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <vector>

using namespace ps1::gpu;
using namespace ps1::gpu::replay;

namespace {

struct OwnedDiagnosticPlanes {
  OwnedDiagnosticPlanes(uint32_t width, uint32_t height,
                        std::size_t rowPadding = 0)
      : width(width), height(height),
        colorPitch(width * sizeof(Color32) + rowPadding),
        maskPitch(width + rowPadding),
        depthPitch(width * sizeof(float) + rowPadding),
        color(colorPitch * height, std::byte{0x5a}),
        mask(maskPitch * height, std::byte{0x6b}),
        depth(depthPitch * height, std::byte{0x7c}) {}

  void set(uint32_t x, uint32_t y, Color32 value, uint8_t maskValue,
           float depthValue) {
    ASSERT_LT(x, width);
    ASSERT_LT(y, height);
    std::memcpy(color.data() + y * colorPitch + x * sizeof(Color32), &value,
                sizeof(value));
    std::memcpy(mask.data() + y * maskPitch + x, &maskValue, sizeof(maskValue));
    std::memcpy(depth.data() + y * depthPitch + x * sizeof(float), &depthValue,
                sizeof(depthValue));
  }

  VulkanReplayDiagnosticPlanes view() const {
    return {{color.data(), color.size(), colorPitch},
            {mask.data(), mask.size(), maskPitch},
            {depth.data(), depth.size(), depthPitch}};
  }

  uint32_t width;
  uint32_t height;
  std::size_t colorPitch;
  std::size_t maskPitch;
  std::size_t depthPitch;
  std::vector<std::byte> color;
  std::vector<std::byte> mask;
  std::vector<std::byte> depth;
};

void fillNativeSeedPlanes(OwnedDiagnosticPlanes &planes,
                          const std::vector<Color16> &native,
                          uint32_t nativeWidth, uint32_t nativeHeight,
                          uint32_t scale) {
  ASSERT_EQ(planes.width, nativeWidth * scale);
  ASSERT_EQ(planes.height, nativeHeight * scale);
  for (uint32_t y = 0; y < planes.height; ++y) {
    for (uint32_t x = 0; x < planes.width; ++x) {
      const uint16_t raw = native[(y / scale) * nativeWidth + (x / scale)].raw;
      planes.set(x, y, EnhancedSurface::expandNativeColor(raw),
                 (raw & 0x8000u) != 0 ? 1u : 0u, 1.0f);
    }
  }
}

void fillEnhancedPlanes(OwnedDiagnosticPlanes &planes,
                        const EnhancedSurface &surface, uint32_t nativeX = 0,
                        uint32_t nativeY = 0) {
  const uint32_t originX = nativeX * surface.scale();
  const uint32_t originY = nativeY * surface.scale();
  for (uint32_t y = 0; y < planes.height; ++y) {
    for (uint32_t x = 0; x < planes.width; ++x) {
      const float rawDepth = surface.depthAt(originX + x, originY + y);
      float depth = 1.0f;
      if (std::isfinite(rawDepth)) {
        const auto normalized = vulkan_abi::tryNormalizeProjectiveWDepth(
            static_cast<double>(rawDepth));
        ASSERT_TRUE(normalized.has_value());
        depth = *normalized;
      }
      planes.set(x, y, surface.colorAt(originX + x, originY + y),
                 surface.maskAt(originX + x, originY + y) ? 1u : 0u, depth);
    }
  }
}

class VulkanReplayDiagnosticMetricsTest : public ::testing::Test {
protected:
  void SetUp() override {
    ps1::metrics::setEnabledForTesting(true);
    ps1::metrics::reset();
  }

  void TearDown() override {
    ps1::metrics::reset();
    ps1::metrics::setEnabledForTesting(false);
  }
};

} // namespace

TEST(GpuModernReplayVulkanDiagnosticsTest,
     NativeSeedMatchesAllThreeFourXPlanesWithPaddedRows) {
  constexpr uint32_t nativeWidth = 2;
  constexpr uint32_t nativeHeight = 2;
  constexpr uint32_t scale = 4;
  const std::vector<Color16> native{{0x0000u}, {0x801fu}, {0x03e0u}, {0x7c00u}};
  OwnedDiagnosticPlanes actual(nativeWidth * scale, nativeHeight * scale, 13);
  fillNativeSeedPlanes(actual, native, nativeWidth, nativeHeight, scale);

  const auto report = diagnoseVulkanReplayReadback(
      {0, 0, nativeWidth, nativeHeight, scale}, actual.view(),
      VulkanReplayDiagnosticOracleView::fromNativeSeed(
          native.data(), native.size(), nativeWidth, nativeHeight, scale));

  ASSERT_TRUE(report.compared());
  EXPECT_TRUE(report.matches());
  EXPECT_EQ(report.comparedPixels, 64u);
  EXPECT_EQ(report.actual.rgba8, report.expected.rgba8);
  EXPECT_EQ(report.actual.maskR8, report.expected.maskR8);
  EXPECT_EQ(report.actual.depthD32, report.expected.depthD32);
  EXPECT_EQ(report.actual.donorRgb5, report.expected.donorRgb5);
  EXPECT_EQ(report.actual.coverage, report.expected.coverage);
  EXPECT_EQ(report.actual.nonBlackPixels, 48u);
  EXPECT_TRUE(report.mismatches.empty());
}

TEST(GpuModernReplayVulkanDiagnosticsTest,
     HashesIgnoreDeviceSpecificReadbackRowPadding) {
  constexpr uint32_t scale = 2;
  const std::vector<Color16> native{{0x001fu}, {0x83e0u}};
  OwnedDiagnosticPlanes dense(4, 2);
  OwnedDiagnosticPlanes padded(4, 2, 31);
  fillNativeSeedPlanes(dense, native, 2, 1, scale);
  fillNativeSeedPlanes(padded, native, 2, 1, scale);

  const auto oracle = VulkanReplayDiagnosticOracleView::fromNativeSeed(
      native.data(), native.size(), 2, 1, scale);
  const auto denseReport =
      diagnoseVulkanReplayReadback({0, 0, 2, 1, scale}, dense.view(), oracle);
  const auto paddedReport =
      diagnoseVulkanReplayReadback({0, 0, 2, 1, scale}, padded.view(), oracle);

  ASSERT_TRUE(denseReport.matches());
  ASSERT_TRUE(paddedReport.matches());
  EXPECT_EQ(denseReport.actual.rgba8, paddedReport.actual.rgba8);
  EXPECT_EQ(denseReport.actual.maskR8, paddedReport.actual.maskR8);
  EXPECT_EQ(denseReport.actual.depthD32, paddedReport.actual.depthD32);
  EXPECT_EQ(denseReport.actual.donorRgb5, paddedReport.actual.donorRgb5);
  EXPECT_EQ(denseReport.actual.coverage, paddedReport.actual.coverage);
}

TEST(GpuModernReplayVulkanDiagnosticsTest,
     AttributesColorMaskAndDepthAtAbsoluteAtlasCoordinate) {
  EnhancedSurface oracleSurface;
  oracleSurface.configure(4, 3, 2);
  oracleSurface.clear({1, 2, 3, 255});
  ASSERT_TRUE(oracleSurface.writeSample(5, 3, {90, 80, 70, 255}, true, false,
                                        false, 32768.0f, true));
  OwnedDiagnosticPlanes actual(4, 2, 7);
  fillEnhancedPlanes(actual, oracleSurface, 1, 1);

  actual.set(1, 0, {9, 8, 7, 255}, 0, std::nextafter(1.0f, 0.0f));
  const auto report = diagnoseVulkanReplayReadback(
      {1, 1, 2, 1, 2}, actual.view(),
      VulkanReplayDiagnosticOracleView::fromEnhancedSurface(oracleSurface),
      {.maximumDepthUlps = 16, .maximumRecordedMismatches = 8});

  ASSERT_TRUE(report.compared());
  EXPECT_FALSE(report.matches());
  EXPECT_EQ(report.comparedPixels, 8u);
  EXPECT_EQ(report.colorMismatchPixels, 1u);
  EXPECT_EQ(report.maskMismatchPixels, 0u);
  // Far is strict and may not pass through the ULP tolerance.
  EXPECT_EQ(report.depthMismatchPixels, 1u);
  EXPECT_EQ(report.anyMismatchPixels, 1u);
  ASSERT_EQ(report.mismatches.size(), 1u);
  EXPECT_EQ(report.mismatches[0].atlasX, 3u);
  EXPECT_EQ(report.mismatches[0].atlasY, 2u);
  EXPECT_EQ(report.mismatches[0].planes,
            VulkanReplayMismatchColor | VulkanReplayMismatchDepth);
}

TEST(GpuModernReplayVulkanDiagnosticsTest,
     NormalizesEnhancedOracleWAndAllowsBoundedFiniteDepthUlpDrift) {
  EnhancedSurface oracleSurface;
  oracleSurface.configure(1, 1, 1);
  ASSERT_TRUE(oracleSurface.writeSample(0, 0, {10, 20, 30, 255}, true, false,
                                        false, 32768.0f, true));
  OwnedDiagnosticPlanes actual(1, 1);
  const float normalized = *vulkan_abi::tryNormalizeProjectiveWDepth(32768.0);
  actual.set(0, 0, {10, 20, 30, 255}, 1, std::nextafter(normalized, 1.0f));

  const auto tolerant = diagnoseVulkanReplayReadback(
      {0, 0, 1, 1, 1}, actual.view(),
      VulkanReplayDiagnosticOracleView::fromEnhancedSurface(oracleSurface),
      {.maximumDepthUlps = 1});
  const auto exact = diagnoseVulkanReplayReadback(
      {0, 0, 1, 1, 1}, actual.view(),
      VulkanReplayDiagnosticOracleView::fromEnhancedSurface(oracleSurface),
      {.maximumDepthUlps = 0});

  EXPECT_TRUE(tolerant.matches());
  EXPECT_EQ(tolerant.maximumObservedDepthUlps, 1u);
  EXPECT_FALSE(exact.matches());
  EXPECT_EQ(exact.depthMismatchPixels, 1u);
}

TEST(GpuModernReplayVulkanDiagnosticsTest,
     ComparesOfflineAtlasOracleWithProjectiveDepthEncoding) {
  OwnedDiagnosticPlanes expected(2, 1, 5);
  expected.set(0, 0, {1, 2, 3, 255}, 0, 65536.0f);
  expected.set(1, 0, {4, 5, 6, 255}, 1, std::numeric_limits<float>::infinity());
  OwnedDiagnosticPlanes actual(2, 1, 11);
  actual.set(0, 0, {1, 2, 3, 255}, 0,
             *vulkan_abi::tryNormalizeProjectiveWDepth(65536.0));
  actual.set(1, 0, {4, 5, 6, 255}, 1, 1.0f);

  const auto report = diagnoseVulkanReplayReadback(
      {0, 0, 2, 1, 1}, actual.view(),
      VulkanReplayDiagnosticOracleView::fromAtlasPlanes(
          expected.view(), 2, 1, 1,
          VulkanReplayDiagnosticDepthEncoding::ProjectiveWInfinityFar));

  EXPECT_TRUE(report.matches());
  EXPECT_EQ(report.actualInvalidDepthPixels, 0u);
  EXPECT_EQ(report.oracleInvalidDepthPixels, 0u);
}

TEST(GpuModernReplayVulkanDiagnosticsTest,
     AtlasPlaneOracleRequiresTheExactCapturedCropIdentity) {
  OwnedDiagnosticPlanes artifact(2, 1, 5);
  artifact.set(0, 0, {1, 2, 3, 255}, 0, 0.25f);
  artifact.set(1, 0, {4, 5, 6, 255}, 1, 1.0f);
  OwnedDiagnosticPlanes actual(2, 1, 9);
  actual.set(0, 0, {1, 2, 3, 255}, 0, 0.25f);
  actual.set(1, 0, {4, 5, 6, 255}, 1, 1.0f);

  const VulkanReplayDiagnosticRegion captured{3, 4, 2, 1, 1};
  const auto oracle = VulkanReplayDiagnosticOracleView::fromAtlasPlanes(
      artifact.view(), captured);
  const auto exact =
      diagnoseVulkanReplayReadback(captured, actual.view(), oracle);
  const auto wrongOrigin = diagnoseVulkanReplayReadback(
      {4, 4, 2, 1, 1}, actual.view(), oracle);
  const auto wrongExtent = diagnoseVulkanReplayReadback(
      {3, 4, 1, 1, 1}, actual.view(), oracle);

  EXPECT_TRUE(exact.matches());
  EXPECT_EQ(wrongOrigin.status,
            VulkanReplayDiagnosticStatus::RegionOutOfBounds);
  EXPECT_EQ(wrongExtent.status,
            VulkanReplayDiagnosticStatus::RegionOutOfBounds);
  EXPECT_EQ(wrongOrigin.comparedPixels, 0u);
  EXPECT_EQ(wrongExtent.comparedPixels, 0u);
}

TEST(GpuModernReplayVulkanDiagnosticsTest,
     InvalidActualOrOracleDepthCanNeverPassAsFar) {
  OwnedDiagnosticPlanes invalidOracle(1, 1);
  invalidOracle.set(0, 0, {1, 2, 3, 255}, 0, -1.0f);
  OwnedDiagnosticPlanes actualFar(1, 1);
  actualFar.set(0, 0, {1, 2, 3, 255}, 0, 1.0f);
  OwnedDiagnosticPlanes invalidActual(1, 1);
  invalidActual.set(0, 0, {1, 2, 3, 255}, 0,
                    std::numeric_limits<float>::quiet_NaN());

  const auto badOracle = diagnoseVulkanReplayReadback(
      {0, 0, 1, 1, 1}, actualFar.view(),
      VulkanReplayDiagnosticOracleView::fromAtlasPlanes(
          invalidOracle.view(), 1, 1, 1,
          VulkanReplayDiagnosticDepthEncoding::ProjectiveWInfinityFar));
  const std::vector<Color16> native{{0x0000u}};
  const auto badActual = diagnoseVulkanReplayReadback(
      {0, 0, 1, 1, 1}, invalidActual.view(),
      VulkanReplayDiagnosticOracleView::fromNativeSeed(native.data(),
                                                       native.size(), 1, 1, 1));

  EXPECT_FALSE(badOracle.matches());
  EXPECT_EQ(badOracle.oracleInvalidDepthPixels, 1u);
  EXPECT_EQ(badOracle.depthMismatchPixels, 1u);
  EXPECT_EQ(badOracle.anyMismatchPixels, 1u);
  ASSERT_EQ(badOracle.mismatches.size(), 1u);
  EXPECT_EQ(badOracle.mismatches[0].planes, VulkanReplayMismatchDepth);

  EXPECT_FALSE(badActual.matches());
  EXPECT_EQ(badActual.actualInvalidDepthPixels, 1u);
  EXPECT_EQ(badActual.depthMismatchPixels, 1u);
  EXPECT_EQ(badActual.anyMismatchPixels, 1u);
}

TEST(GpuModernReplayVulkanDiagnosticsTest,
     RejectsTruncatedPlaneBeforeReadingAnyPixel) {
  const std::vector<Color16> native{{0x001fu}};
  OwnedDiagnosticPlanes actual(1, 1);
  fillNativeSeedPlanes(actual, native, 1, 1, 1);
  VulkanReplayDiagnosticPlanes truncated = actual.view();
  truncated.depthD32.sizeBytes = sizeof(float) - 1;

  const auto report = diagnoseVulkanReplayReadback(
      {0, 0, 1, 1, 1}, truncated,
      VulkanReplayDiagnosticOracleView::fromNativeSeed(native.data(),
                                                       native.size(), 1, 1, 1));

  EXPECT_EQ(report.status, VulkanReplayDiagnosticStatus::InvalidReadbackPlane);
  EXPECT_STREQ(vulkanReplayDiagnosticStatusName(report.status),
               "invalid-readback-plane");
  EXPECT_EQ(report.comparedPixels, 0u);
}

TEST(GpuModernReplayVulkanDiagnosticsTest,
     CapturesActualHashesWithoutClaimingAnOracleMatch) {
  OwnedDiagnosticPlanes actual(2, 1, 9);
  actual.set(0, 0, {1, 2, 3, 255}, 0, 0.25f);
  actual.set(1, 0, {4, 5, 6, 255}, 1, 1.0f);

  const auto report =
      captureVulkanReplayReadback({3, 4, 2, 1, 1}, actual.view());

  EXPECT_TRUE(report.captured());
  EXPECT_FALSE(report.compared());
  EXPECT_FALSE(report.matches());
  EXPECT_EQ(report.status, VulkanReplayDiagnosticStatus::CapturedWithoutOracle);
  EXPECT_EQ(report.comparedPixels, 2u);
  EXPECT_NE(report.actual.rgba8, 0u);
  EXPECT_NE(report.actual.maskR8, 0u);
  EXPECT_NE(report.actual.depthD32, 0u);
  EXPECT_NE(report.actual.donorRgb5, 0u);
  EXPECT_EQ(report.actualInvalidDepthPixels, 0u);
  EXPECT_EQ(report.actualSurfaceDepthPixels, 1u);
  EXPECT_EQ(report.actualSurfaceDepthSentinelPixels, 1u);
  EXPECT_EQ(report.actualNonBlackSurfaceDepthPixels, 1u);
  EXPECT_EQ(report.actualNonBlackSurfaceDepthSentinelPixels, 1u);
  EXPECT_TRUE(report.mismatches.empty());
}

TEST(GpuModernReplayVulkanDiagnosticsTest,
     SeparatesSurfaceDepthCoverageFromFarSentinelAndInvalidValues) {
  OwnedDiagnosticPlanes actual(4, 1);
  actual.set(0, 0, {1, 2, 3, 255}, 0, 0.25f);
  actual.set(1, 0, {0, 0, 0, 255}, 0, 0.5f);
  actual.set(2, 0, {4, 5, 6, 255}, 0, 1.0f);
  actual.set(3, 0, {7, 8, 9, 255}, 0,
             std::numeric_limits<float>::quiet_NaN());

  const auto report =
      captureVulkanReplayReadback({0, 0, 4, 1, 1}, actual.view());

  EXPECT_EQ(report.actualSurfaceDepthPixels, 2u);
  EXPECT_EQ(report.actualSurfaceDepthSentinelPixels, 1u);
  EXPECT_EQ(report.actualNonBlackSurfaceDepthPixels, 1u);
  EXPECT_EQ(report.actualNonBlackSurfaceDepthSentinelPixels, 1u);
  EXPECT_EQ(report.actualInvalidDepthPixels, 1u);
}

TEST(GpuModernReplayVulkanDiagnosticsTest,
     PublishedStatusValuesRemainAppendOnly) {
  EXPECT_EQ(static_cast<uint8_t>(VulkanReplayDiagnosticStatus::Compared), 0u);
  EXPECT_EQ(static_cast<uint8_t>(VulkanReplayDiagnosticStatus::EmptyRegion),
            1u);
  EXPECT_EQ(
      static_cast<uint8_t>(VulkanReplayDiagnosticStatus::InvalidOraclePlane),
      7u);
  EXPECT_EQ(static_cast<uint8_t>(
                VulkanReplayDiagnosticStatus::CapturedWithoutOracle),
            8u);
}

TEST(GpuModernReplayVulkanDiagnosticsTest,
     RejectsOracleScaleMismatchAndOutOfBoundsRegion) {
  EnhancedSurface oracleSurface;
  oracleSurface.configure(2, 2, 4);
  OwnedDiagnosticPlanes actual(2, 2);
  OwnedDiagnosticPlanes largeActual(4, 4);

  const auto scaleMismatch = diagnoseVulkanReplayReadback(
      {0, 0, 1, 1, 2}, actual.view(),
      VulkanReplayDiagnosticOracleView::fromEnhancedSurface(oracleSurface));
  const auto outOfBounds = diagnoseVulkanReplayReadback(
      {2, 0, 1, 1, 4}, largeActual.view(),
      VulkanReplayDiagnosticOracleView::fromEnhancedSurface(oracleSurface));

  EXPECT_EQ(scaleMismatch.status,
            VulkanReplayDiagnosticStatus::OracleScaleMismatch);
  EXPECT_EQ(outOfBounds.status,
            VulkanReplayDiagnosticStatus::RegionOutOfBounds);
}

TEST_F(VulkanReplayDiagnosticMetricsTest,
       PublishesAggregateMismatchCountersAndPortableHashes) {
  const std::vector<Color16> native{{0x001fu}};
  OwnedDiagnosticPlanes actual(1, 1);
  actual.set(0, 0, {0, 0, 0, 255}, 0, 1.0f);
  const auto report = diagnoseVulkanReplayReadback(
      {0, 0, 1, 1, 1}, actual.view(),
      VulkanReplayDiagnosticOracleView::fromNativeSeed(native.data(),
                                                       native.size(), 1, 1, 1));
  ASSERT_EQ(report.colorMismatchPixels, 1u);

  publishVulkanReplayDiagnosticMetrics(report, 123, "test.vulkan_readback");

  EXPECT_EQ(ps1::metrics::get("test.vulkan_readback.capture"), 1u);
  EXPECT_EQ(ps1::metrics::get("test.vulkan_readback.pixels"), 1u);
  EXPECT_EQ(ps1::metrics::get("test.vulkan_readback.color_mismatch_pixels"),
            1u);
  EXPECT_EQ(ps1::metrics::getState("test.vulkan_readback.last.matches"), 0);
  EXPECT_EQ(ps1::metrics::getState("test.vulkan_readback.last.frame"), 123);
  EXPECT_EQ(ps1::metrics::getState(
                "test.vulkan_readback.last.actual_donor_rgb5_hash"),
            std::bit_cast<int64_t>(report.actual.donorRgb5));
  EXPECT_EQ(ps1::metrics::getState(
                "test.vulkan_readback.last.expected_coverage_hash"),
            std::bit_cast<int64_t>(report.expected.coverage));
}

TEST_F(VulkanReplayDiagnosticMetricsTest,
       PublishesActualOnlyCaptureWithoutClaimingExpectedHashes) {
  OwnedDiagnosticPlanes actual(1, 1);
  actual.set(0, 0, {7, 8, 9, 255}, 1, 0.5f);
  const auto report =
      captureVulkanReplayReadback({0, 0, 1, 1, 1}, actual.view());
  ASSERT_TRUE(report.captured());
  ASSERT_FALSE(report.compared());

  publishVulkanReplayDiagnosticMetrics(report, 456, "test.vulkan_capture");

  EXPECT_EQ(ps1::metrics::get("test.vulkan_capture.capture"), 1u);
  EXPECT_EQ(ps1::metrics::get("test.vulkan_capture.pixels"), 1u);
  EXPECT_EQ(ps1::metrics::getState("test.vulkan_capture.last.has_oracle"), 0);
  EXPECT_EQ(ps1::metrics::getState("test.vulkan_capture.last.matches"), 0);
  EXPECT_EQ(
      ps1::metrics::getState("test.vulkan_capture.last.actual_rgba8_hash"),
      std::bit_cast<int64_t>(report.actual.rgba8));
  EXPECT_EQ(
      ps1::metrics::getState("test.vulkan_capture.last.expected_rgba8_hash"),
      0);
  EXPECT_EQ(
      ps1::metrics::getState("test.vulkan_capture.last.expected_depth_hash"),
      0);
  EXPECT_EQ(ps1::metrics::getState(
                "test.vulkan_capture.last.actual_surface_depth_pixels"),
            1);
  EXPECT_EQ(ps1::metrics::getState(
                "test.vulkan_capture.last."
                "actual_nonblack_surface_depth_sentinel_pixels"),
            0);
}

TEST_F(VulkanReplayDiagnosticMetricsTest,
       InvalidCaptureReplacesEveryPreviouslyPublishedLastState) {
  OwnedDiagnosticPlanes actual(1, 1);
  actual.set(0, 0, {7, 8, 9, 255}, 1, 0.5f);
  const auto captured =
      captureVulkanReplayReadback({0, 0, 1, 1, 1}, actual.view());
  ASSERT_TRUE(captured.captured());
  publishVulkanReplayDiagnosticMetrics(captured, 10, "test.vulkan_reset");
  ASSERT_NE(ps1::metrics::getState(
                "test.vulkan_reset.last.actual_rgba8_hash"),
            0);

  VulkanReplayDiagnosticPlanes truncated = actual.view();
  truncated.depthD32.sizeBytes = sizeof(float) - 1;
  const auto invalid =
      captureVulkanReplayReadback({0, 0, 1, 1, 1}, truncated);
  ASSERT_FALSE(invalid.captured());
  ASSERT_EQ(invalid.status,
            VulkanReplayDiagnosticStatus::InvalidReadbackPlane);
  publishVulkanReplayDiagnosticMetrics(invalid, 11, "test.vulkan_reset");

  EXPECT_EQ(ps1::metrics::getState("test.vulkan_reset.last.frame"), 11);
  EXPECT_EQ(ps1::metrics::getState("test.vulkan_reset.last.status"),
            static_cast<int64_t>(
                VulkanReplayDiagnosticStatus::InvalidReadbackPlane));
  EXPECT_EQ(ps1::metrics::getState("test.vulkan_reset.last.has_oracle"), 0);
  EXPECT_EQ(ps1::metrics::getState("test.vulkan_reset.last.matches"), 0);
  EXPECT_EQ(ps1::metrics::getState(
                "test.vulkan_reset.last.actual_rgba8_hash"),
            0);
  EXPECT_EQ(ps1::metrics::getState(
                "test.vulkan_reset.last.expected_depth_hash"),
            0);
  EXPECT_EQ(ps1::metrics::getState(
                "test.vulkan_reset.last.actual_surface_depth_pixels"),
            0);
  EXPECT_EQ(
      ps1::metrics::getState("test.vulkan_reset.last.first_mismatch_x"), -1);
  EXPECT_EQ(
      ps1::metrics::getState("test.vulkan_reset.last.first_mismatch_y"), -1);
}
