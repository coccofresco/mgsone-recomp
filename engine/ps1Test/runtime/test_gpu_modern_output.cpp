#include "runtime/gpu/gpu.h"
#include "runtime/gpu/modern_frame_interpolation.h"
#include "runtime/gpu/modern_replay_vulkan_abi.h"
#include "runtime/gpu/renderer_options.h"
#include "runtime/gpu/renderer_vulkan.h"
#include "runtime/gpu/vulkan_replay_seed.h"
#include "runtime/metrics.h"
#include <cstdlib>
#include <gtest/gtest.h>
#include <vector>

using namespace ps1::gpu;

namespace {

TEST(GpuModernOutputTest, TemporalQuadCannotMixAnInterpolatedAndCurrentHalf) {
  using namespace ps1::gpu::replay::vulkan_abi;
  std::array<SidecarPreviousVertex, 6> previous{};
  constexpr std::array<uint8_t, 6> corners{0, 1, 2, 1, 2, 3};
  for (std::size_t i = 0u; i < 3u; ++i)
    previous[i].packedOriginAndFlags = SIDECAR_PREVIOUS_VALID;
  EXPECT_EQ(interpolation::cohereTemporalPrimitive(previous, corners), 3u);
  for (const auto &vertex : previous)
    EXPECT_EQ(vertex.packedOriginAndFlags & SIDECAR_PREVIOUS_VALID, 0u);
}

TEST(GpuModernOutputTest, TemporalQuadRequiresIdenticalSharedSourceCorners) {
  using namespace ps1::gpu::replay::vulkan_abi;
  std::array<SidecarPreviousVertex, 6> previous{};
  constexpr std::array<uint8_t, 6> corners{0, 1, 2, 1, 2, 3};
  for (std::size_t i = 0u; i < previous.size(); ++i) {
    previous[i].packedOriginAndFlags = SIDECAR_PREVIOUS_VALID | 512u;
    previous[i].positionW = {512.0f + corners[i], 10.0f, 100.0f};
  }
  EXPECT_EQ(interpolation::cohereTemporalPrimitive(previous, corners), 0u);
  previous[3].positionW[0] += 1.0f;
  EXPECT_EQ(interpolation::cohereTemporalPrimitive(previous, corners), 6u);
  for (const auto &vertex : previous)
    EXPECT_EQ(vertex.packedOriginAndFlags & SIDECAR_PREVIOUS_VALID, 0u);
}

TEST(GpuModernOutputTest, TemporalPrimitiveWithoutCornerProvenanceRetainsCurrentGeometry) {
  using namespace ps1::gpu::replay::vulkan_abi;
  std::array<SidecarPreviousVertex, 6> previous{};
  for (auto &vertex : previous)
    vertex.packedOriginAndFlags = SIDECAR_PREVIOUS_VALID | 512u;
  EXPECT_EQ(interpolation::cohereTemporalPrimitive(previous, {}), 6u);
  for (const auto &vertex : previous)
    EXPECT_EQ(vertex.packedOriginAndFlags, 512u);
  EXPECT_EQ(interpolation::cohereTemporalPrimitive(previous, {}), 0u);
}

VulkanReplayImageSupport fullReplayImageSupport(uint32_t width = 8192,
                                                uint32_t height = 8192) {
  return {true, true, true, {width, height}};
}

TEST(GpuModernOutputTest,
     TemporalVertexInterpolationNormalizesAlternatingVramPages) {
  using namespace ps1::gpu::interpolation;
  const TemporalVertexIdentity identity{{11u, 22u, 33u, 44u}};
  TemporalVertexSample previous;
  previous.identity = identity;
  previous.positionNative = {10.0f, 20.0f};
  previous.projectiveW = 100.0f;
  previous.presentationOriginNative = {0.0f, 0.0f};
  previous.presentationDepthBits = {1u, 2u, 3u, 4u};
  previous.eligible = true;
  TemporalVertexSample current = previous;
  current.positionNative = {522.0f, 276.0f};
  current.presentationOriginNative = {512.0f, 256.0f};

  const TemporalVertexInterpolationResult midpoint =
      interpolateTemporalVertex(previous, current, 0.5f);

  ASSERT_TRUE(midpoint.interpolated());
  EXPECT_FLOAT_EQ(midpoint.positionNative[0], 522.0f);
  EXPECT_FLOAT_EQ(midpoint.positionNative[1], 276.0f);
  EXPECT_FLOAT_EQ(midpoint.projectiveW, 100.0f);
}

TEST(GpuModernOutputTest,
     TemporalVertexInterpolationUsesHomogeneousProjectedPosition) {
  using namespace ps1::gpu::interpolation;
  const TemporalVertexIdentity identity{{1u, 2u, 3u, 4u}};
  TemporalVertexSample previous;
  previous.identity = identity;
  previous.positionNative = {10.0f, 8.0f};
  previous.projectiveW = 1.0f;
  previous.presentationDepthBits = {5u, 6u, 7u, 8u};
  previous.eligible = true;
  TemporalVertexSample current = previous;
  current.positionNative = {30.0f, 16.0f};
  current.projectiveW = 3.0f;

  const TemporalVertexInterpolationResult midpoint =
      interpolateTemporalVertex(previous, current, 0.5f);

  ASSERT_TRUE(midpoint.interpolated());
  EXPECT_FLOAT_EQ(midpoint.positionNative[0], 25.0f);
  EXPECT_FLOAT_EQ(midpoint.positionNative[1], 14.0f);
  EXPECT_FLOAT_EQ(midpoint.projectiveW, 2.0f);
}

TEST(GpuModernOutputTest, TemporalVertexInterpolationFailsClosedOnDepthChange) {
  using namespace ps1::gpu::interpolation;
  TemporalVertexSample previous;
  previous.identity = {{1u, 2u, 3u, 4u}};
  previous.positionNative = {10.0f, 20.0f};
  previous.projectiveW = 100.0f;
  previous.presentationDepthBits = {1u, 2u, 3u, 4u};
  previous.eligible = true;
  TemporalVertexSample current = previous;
  current.positionNative = {30.0f, 40.0f};
  current.presentationDepthBits[1] = 99u;

  const TemporalVertexInterpolationResult midpoint =
      interpolateTemporalVertex(previous, current, 0.5f);

  EXPECT_FALSE(midpoint.interpolated());
  EXPECT_EQ(midpoint.status,
            TemporalVertexInterpolationStatus::DepthFamilyMismatch);
  EXPECT_EQ(midpoint.positionNative, current.positionNative);
  EXPECT_FLOAT_EQ(midpoint.projectiveW, current.projectiveW);
}

VulkanReplayDeviceSupport fullReplayDeviceSupport() {
  VulkanReplayDeviceSupport support;
  support.maxImageDimension2D = 8192;
  support.maxFramebufferWidth = 8192;
  support.maxFramebufferHeight = 8192;
  support.nativeShadow = fullReplayImageSupport();
  support.color = fullReplayImageSupport();
  support.mask = fullReplayImageSupport();
  support.surfaceDepth = fullReplayImageSupport();
  support.depth = fullReplayImageSupport();
  return support;
}

} // namespace

TEST(GpuModernOutputTest, DepthPresentationViewCyclesPrePostAndComparison) {
  VulkanPresentationView view = VulkanPresentationView::Color;
  EXPECT_FALSE(vulkanPresentationViewUsesDepth(view));

  view = nextVulkanPresentationView(view);
  EXPECT_EQ(view, VulkanPresentationView::DepthPostCorrection);
  EXPECT_TRUE(vulkanPresentationViewUsesDepth(view));
  EXPECT_FALSE(vulkanPresentationViewUsesPreCorrectionDepth(view));

  view = nextVulkanPresentationView(view);
  EXPECT_EQ(view, VulkanPresentationView::DepthPreCorrection);
  EXPECT_TRUE(vulkanPresentationViewUsesDepth(view));
  EXPECT_TRUE(vulkanPresentationViewUsesPreCorrectionDepth(view));

  view = nextVulkanPresentationView(view);
  EXPECT_EQ(view, VulkanPresentationView::DepthComparison);
  EXPECT_TRUE(vulkanPresentationViewUsesDepth(view));
  EXPECT_TRUE(vulkanPresentationViewUsesPreCorrectionDepth(view));

  view = nextVulkanPresentationView(view);
  EXPECT_EQ(view, VulkanPresentationView::Color);
  EXPECT_FALSE(vulkanPresentationViewUsesDepth(view));
}

TEST(GpuModernOutputTest, ScanoutPushAbiMapsCrashPalSwanCanvasAtFourX) {
  RawDisplayCrtcInput input;
  input.sourceWordX = 512;
  input.sourceY = 0;
  input.horizontalStart = 608;
  input.horizontalEnd = 3168;
  input.verticalStart = 37;
  input.verticalEnd = 293;
  input.gpuStat = (2u << 17) | (1u << 20); // 512-wide PAL
  input.modeSet = true;
  input.areaSet = true;
  input.horizontalRangeSet = true;
  input.verticalRangeSet = true;
  input.signalProfile = DisplaySignalProfile::SwanStationNoCrop;

  const DisplayScanoutPlan plan =
      deriveDisplayScanoutPlan(input, DisplayAreaPolicy::DonorCanvas);
  ASSERT_EQ(plan.canvas.width, 559u);
  ASSERT_EQ(plan.canvas.height, 288u);
  ASSERT_EQ(plan.destinationOrigin.x, 23u);
  ASSERT_EQ(plan.destinationOrigin.y, 17u);

  const VulkanScanoutPushConstants push =
      makeVulkanScanoutPushConstants(plan, 4);
  EXPECT_EQ(push.source, (std::array<uint32_t, 4>{1024, 0, 1, 4}));
  EXPECT_EQ(push.active, (std::array<uint32_t, 4>{512, 256, 0, 0}));
  EXPECT_TRUE(isVulkanScanoutPushConstantsValid(push));
  EXPECT_EQ(mapVulkanScanoutRgb555Source(push, 0, 0),
            (VulkanScanoutSourceCoordinate{2048, 0}));
  EXPECT_EQ(mapVulkanScanoutRgb555Source(push, 2047, 1023),
            (VulkanScanoutSourceCoordinate{4095, 1023}));
}

TEST(GpuModernOutputTest,
     TemporalScanoutPushKeepsPreviousAndMidpointPagesDistinct) {
  DisplayScanoutPlan midpoint;
  midpoint.activeExtent = {320, 240};
  midpoint.sourceByteX = 0;
  midpoint.sourceY = 0;
  midpoint.rowStep = 1;
  midpoint.format = DisplayScanoutFormat::RGB555;
  midpoint.progressive = true;
  midpoint.field = DisplayScanoutField::NotApplicable;
  DisplayScanoutPlan previous = midpoint;
  previous.sourceByteX = 512u * sizeof(Color16);

  const VulkanTemporalScanoutPushConstants push =
      makeVulkanTemporalScanoutPushConstants(midpoint, previous, 4u);

  EXPECT_EQ(push.midpoint.source, (std::array<uint32_t, 4>{0u, 0u, 1u, 4u}));
  EXPECT_EQ(push.previousSource, (std::array<uint32_t, 4>{1024u, 0u, 1u, 4u}));
  EXPECT_TRUE(isVulkanTemporalScanoutPushConstantsValid(push));
}

TEST(GpuModernOutputTest,
     TemporalHistoryAllowsPreviousPixelsForContinuousSceneIdentity) {
  const VulkanTemporalHistoryIdentity previous{
      11u, 22u, 33u, 4u, true, true,
  };
  VulkanTemporalHistoryIdentity current = previous;

  EXPECT_EQ(
      vulkanTemporalHistoryDiscontinuityMask(previous, current, false, false),
      VulkanTemporalHistoryContinuous);
}

TEST(GpuModernOutputTest,
     TemporalHistoryRejectsEveryIntrinsicResidentDiscontinuity) {
  const VulkanTemporalHistoryIdentity previous{
      11u, 22u, 33u, 4u, true, true,
  };
  const VulkanTemporalHistoryIdentity current{
      12u, 23u, 34u, 5u, false, true,
  };

  const uint32_t mask =
      vulkanTemporalHistoryDiscontinuityMask(previous, current, false, false);
  EXPECT_NE(mask & VulkanTemporalHistoryResidentPresence, 0u);
  EXPECT_NE(mask & VulkanTemporalHistoryResidentTopology, 0u);
  EXPECT_NE(mask & VulkanTemporalHistoryResidentResources, 0u);
  EXPECT_NE(mask & VulkanTemporalHistoryResidentCohort, 0u);
  EXPECT_EQ(mask & VulkanTemporalHistoryObjectMembership, 0u);
  EXPECT_EQ(mask & VulkanTemporalHistoryObjectGeometry, 0u);
}

TEST(GpuModernOutputTest,
     TemporalHistoryRejectsObjectMembershipAndGeometryIndependently) {
  const VulkanTemporalHistoryIdentity previous{
      0u, 0u, 0u, 0u, false, true,
  };
  const VulkanTemporalHistoryIdentity current = previous;

  EXPECT_EQ(
      vulkanTemporalHistoryDiscontinuityMask(previous, current, true, false),
      VulkanTemporalHistoryObjectMembership);
  EXPECT_EQ(
      vulkanTemporalHistoryDiscontinuityMask(previous, current, false, true),
      VulkanTemporalHistoryObjectGeometry);
  EXPECT_EQ(
      vulkanTemporalHistoryDiscontinuityMask(previous, current, true, true),
      VulkanTemporalHistoryObjectMembership |
          VulkanTemporalHistoryObjectGeometry);
}

TEST(GpuModernOutputTest,
     TemporalHistoryIgnoresEmptyResidentFingerprintDifferences) {
  const VulkanTemporalHistoryIdentity previous{
      11u, 22u, 33u, 4u, false, false,
  };
  const VulkanTemporalHistoryIdentity current{
      12u, 23u, 34u, 5u, false, false,
  };

  EXPECT_EQ(
      vulkanTemporalHistoryDiscontinuityMask(previous, current, false, false),
      VulkanTemporalHistoryContinuous);
}

TEST(GpuModernOutputTest,
     BackgroundUnderlaySharesResidentEpochButNotUnrelatedObjectAnimation) {
  // Every combination of the existing identity-domain changes. A new object
  // or changing animated mesh does not freeze the sky, but any resident scene
  // replacement must invalidate its supplemental projected history as well.
  for (uint32_t mask = 0u; mask < 64u; ++mask) {
    SCOPED_TRACE(mask);
    EXPECT_EQ(vulkanTemporalBackgroundUnderlayHistoryCompatible(mask),
              (mask & 15u) == 0u);
  }
  EXPECT_FALSE(vulkanTemporalBackgroundUnderlayHistoryCompatible(1u << 31u));
}

TEST(GpuModernOutputTest, ReplayDepthPagesHaveIndependentValidityBits) {
  EXPECT_EQ(vulkanReplayDepthPageBit(0u, 0u), 1u);
  EXPECT_EQ(vulkanReplayDepthPageBit(512u, 0u), 2u);
  EXPECT_EQ(vulkanReplayDepthPageBit(0u, 256u), 4u);
  EXPECT_EQ(vulkanReplayDepthPageBit(512u, 256u), 8u);
  EXPECT_EQ(vulkanReplayDepthPageBit(1u, 0u), 0u);
  EXPECT_EQ(vulkanReplayDepthPageBit(0u, 1u), 0u);
  EXPECT_EQ(vulkanReplayDepthPageBit(GPU::VRAM_WIDTH, 0u), 0u);
  EXPECT_EQ(vulkanReplayDepthPageBit(0u, GPU::VRAM_HEIGHT), 0u);
}

TEST(GpuModernOutputTest, ScanoutRgb555MappingWrapsWordsRowsAndRowStep) {
  DisplayScanoutPlan plan;
  plan.activeExtent = {2, 2};
  plan.sourceByteX = 2046;
  plan.sourceY = 511;
  plan.rowStep = 2;
  plan.format = DisplayScanoutFormat::RGB555;
  plan.progressive = false;
  plan.field = DisplayScanoutField::Even;
  const VulkanScanoutPushConstants push =
      makeVulkanScanoutPushConstants(plan, 4);

  ASSERT_TRUE(isVulkanScanoutPushConstantsValid(push));
  EXPECT_EQ(mapVulkanScanoutRgb555Source(push, 0, 0),
            (VulkanScanoutSourceCoordinate{4092, 2044}));
  EXPECT_EQ(mapVulkanScanoutRgb555Source(push, 3, 3),
            (VulkanScanoutSourceCoordinate{4095, 2047}));
  EXPECT_EQ(mapVulkanScanoutRgb555Source(push, 4, 4),
            (VulkanScanoutSourceCoordinate{0, 4}));
}

TEST(GpuModernOutputTest,
     ScanoutPackedRgb888MappingPreservesOddBytePhaseAndWrap) {
  DisplayScanoutPlan plan;
  plan.activeExtent = {2, 2};
  plan.sourceByteX = 2047;
  plan.sourceY = 511;
  plan.rowStep = 2;
  plan.format = DisplayScanoutFormat::PackedRGB888;
  plan.progressive = false;
  plan.field = DisplayScanoutField::Odd;
  const VulkanScanoutPushConstants nativePush =
      makeVulkanScanoutPushConstants(plan, 1);

  ASSERT_TRUE(isVulkanScanoutPushConstantsValid(nativePush));
  EXPECT_EQ(mapVulkanScanoutPackedRgb888Source(nativePush, 0, 0),
            (VulkanScanoutPackedRgb888Source{{2047, 0, 1}, 511}));
  EXPECT_EQ(mapVulkanScanoutPackedRgb888Source(nativePush, 1, 1),
            (VulkanScanoutPackedRgb888Source{{2, 3, 4}, 1}));

  const VulkanScanoutPushConstants replayPush =
      makeVulkanScanoutPushConstants(plan, 4);
  EXPECT_FALSE(isVulkanScanoutPushConstantsValid(replayPush));
}

TEST(GpuModernOutputTest,
     OptInGpuScanoutPlacesDonorCanvasAndKeepsPacked24Native) {
  if (std::getenv("PS1_RUN_VULKAN_INTEGRATION") == nullptr)
    GTEST_SKIP() << "set PS1_RUN_VULKAN_INTEGRATION=1 for the GPU oracle";

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);
  GPU gpu;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.displayAreaPolicy = DisplayAreaPolicy::DonorCanvas;
  options.displaySignalProfile = DisplaySignalProfile::SwanStationNoCrop;
  options.internalScale = 4;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueDepthMode = "hybrid";
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(
      renderer.init("Vulkan scanout GPU oracle", 320, 240, false, false));

  constexpr uint32_t horizontalStart = 608;
  constexpr uint32_t horizontalEnd = 3168;
  constexpr uint32_t verticalStart = 37;
  constexpr uint32_t verticalEnd = 293;
  gpu.writeGP1(0x03000000u); // display enable
  gpu.writeGP1(0x05000000u | 512u);
  gpu.writeGP1(0x06000000u | horizontalStart | (horizontalEnd << 12u));
  gpu.writeGP1(0x07000000u | verticalStart | (verticalEnd << 10u));
  gpu.writeGP1(0x08000000u | 0x0Au); // 512-wide PAL RGB555
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();

  EXPECT_TRUE(renderer.processEvents());
  EXPECT_EQ(ps1::metrics::getState("host.renderer.scanout.canvas_width"), 559);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.scanout.canvas_height"), 288);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.scanout.active_x"), 23);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.scanout.active_y"), 17);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.scanout.active_width"), 512);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.scanout.active_height"), 256);
  EXPECT_EQ(ps1::metrics::getState("host.vulkan.internal_width"), 2236);
  EXPECT_EQ(ps1::metrics::getState("host.vulkan.internal_height"), 1152);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.vulkan_replay_present_atlas"),
            1);
  EXPECT_EQ(
      ps1::metrics::getState("host.renderer.scanout.precision_compatible"), 1);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.scanout.precision_presented"),
            1);

  renderer.toggleDepthVisualization();
  EXPECT_EQ(renderer.presentationView(),
            VulkanPresentationView::DepthPostCorrection);
  gpu.snapshotDisplayBuffer(2);
  renderer.renderFrame();

  EXPECT_TRUE(renderer.processEvents());
  EXPECT_EQ(
      ps1::metrics::getState("host.renderer.depth_visualization.requested"), 1);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.depth_visualization.active"),
            1);
  EXPECT_EQ(
      ps1::metrics::getState("host.renderer.depth_visualization.active_mode"),
      static_cast<int64_t>(VulkanPresentationView::DepthPostCorrection));

  // A save-state can move the guest frame counter backwards without
  // recreating the Vulkan device. The depth request must survive that
  // discontinuity and become active again on the replacement native seed.
  std::vector<Color16> restoredVram(static_cast<std::size_t>(GPU::VRAM_WIDTH) *
                                    GPU::VRAM_HEIGHT);
  gpu.loadVram(reinterpret_cast<const uint8_t *>(restoredVram.data()));
  renderer.rebaseGuestTimeline();
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();

  EXPECT_TRUE(renderer.processEvents());
  EXPECT_EQ(renderer.presentationView(),
            VulkanPresentationView::DepthPostCorrection);
  EXPECT_EQ(ps1::metrics::get("host.renderer.vulkan_replay_timeline_rebase"),
            1u);
  EXPECT_EQ(
      ps1::metrics::getState("host.renderer.depth_visualization.requested"), 1);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.depth_visualization.active"),
            1);

  renderer.toggleDepthVisualization();
  EXPECT_EQ(renderer.presentationView(),
            VulkanPresentationView::DepthPreCorrection);
  renderer.toggleDepthVisualization();
  EXPECT_EQ(renderer.presentationView(),
            VulkanPresentationView::DepthComparison);
  renderer.toggleDepthVisualization();
  EXPECT_EQ(renderer.presentationView(), VulkanPresentationView::Color);
  gpu.writeGP1(0x08000000u | 0x1Au); // same PAL signal, packed RGB888
  gpu.snapshotDisplayBuffer(3);
  renderer.renderFrame();

  EXPECT_TRUE(renderer.processEvents());
  EXPECT_EQ(ps1::metrics::getState("host.renderer.scanout.format"), 1);
  EXPECT_EQ(
      ps1::metrics::getState("host.renderer.scanout.precision_compatible"), 0);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.scanout.precision_presented"),
            0);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.vulkan_replay_present_atlas"),
            0);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.precision_replay"), 0);
  EXPECT_EQ(
      ps1::metrics::getState("host.renderer.depth_visualization.requested"), 0);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.depth_visualization.active"),
            0);

  gpu.writeGP1(0x08000000u | 0x0Au); // progressive RGB555 again
  gpu.writeGP1(0x03000001u);         // display disable
  gpu.snapshotDisplayBuffer(4);
  renderer.renderFrame();

  EXPECT_EQ(
      ps1::metrics::getState("host.renderer.scanout.precision_compatible"), 1);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.scanout.precision_presented"),
            0);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.vulkan_replay_present_atlas"),
            0);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.precision_replay"), 0);

  gpu.writeGP1(0x03000000u);         // display enable
  gpu.writeGP1(0x08000000u | 0x3Eu); // 512-wide PAL interlaced Packed24
  gpu.snapshotDisplayBuffer(5);
  renderer.renderFrame();

  EXPECT_EQ(ps1::metrics::getState("host.renderer.scanout.interlace_deferred"),
            1);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.scanout.format"), 1);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.scanout.precision_presented"),
            0);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.vulkan_replay_present_atlas"),
            0);

  renderer.destroy();
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST(GpuModernOutputTest, PlansOnePersistentFourXReplayAtlas) {
  const VulkanReplayResourcePlan plan = planVulkanReplayResources(4);

  EXPECT_EQ(plan.internalScale, 4u);
  EXPECT_EQ(plan.nativeShadowExtent.width, 1024u);
  EXPECT_EQ(plan.nativeShadowExtent.height, 512u);
  EXPECT_EQ(plan.atlasExtent.width, 4096u);
  EXPECT_EQ(plan.atlasExtent.height, 2048u);
  EXPECT_EQ(plan.nativeShadowBytes, 1u * 1024u * 1024u);
  EXPECT_EQ(plan.colorBytes, 32u * 1024u * 1024u);
  EXPECT_EQ(plan.maskBytes, 8u * 1024u * 1024u);
  EXPECT_EQ(plan.surfaceDepthBytes, 32u * 1024u * 1024u);
  EXPECT_EQ(plan.depthBytes, 32u * 1024u * 1024u);
  EXPECT_EQ(plan.highPrecisionAtlasBytes(), 104u * 1024u * 1024u);
  EXPECT_EQ(plan.totalBytes(), 105u * 1024u * 1024u);
}

TEST(GpuModernOutputTest, VisibilityOraclePlanKeepsTwoIndependentPassesOptIn) {
  const VulkanReplayVisibilityResourcePlan plan =
      planVulkanReplayVisibilityResources(4);

  EXPECT_EQ(plan.internalScale, 4u);
  EXPECT_EQ(plan.atlasExtent.width, 4096u);
  EXPECT_EQ(plan.atlasExtent.height, 2048u);
  EXPECT_EQ(plan.ownerPlaneBytes, 32u * 1024u * 1024u);
  EXPECT_EQ(plan.depthPlaneBytes, 32u * 1024u * 1024u);
  EXPECT_EQ(plan.evidencePlaneBytes, 8u * 1024u * 1024u);
  EXPECT_EQ(plan.passCount, 2u);
  EXPECT_EQ(plan.passBytes(), 72u * 1024u * 1024u);
  EXPECT_EQ(plan.totalBytes(), 144u * 1024u * 1024u);
}

TEST(GpuModernOutputTest,
     VisibilityCandidatePlanAddsOneIndependentPassOnlyWhenRequested) {
  const VulkanReplayVisibilityResourcePlan plan =
      planVulkanReplayVisibilityResources(4, true);

  EXPECT_EQ(plan.passCount, 3u);
  EXPECT_EQ(plan.passBytes(), 72u * 1024u * 1024u);
  EXPECT_EQ(plan.totalBytes(), 216u * 1024u * 1024u);
}

TEST(GpuModernOutputTest, ReplaySeedExpandsRgb555AndMaskIndependently) {
  EXPECT_EQ(decodeVulkanReplaySeedTexel(Color16{0x0000u}),
            (VulkanReplaySeedTexel{0, 0, 0, 255, 0}));
  EXPECT_EQ(decodeVulkanReplaySeedTexel(Color16{0xFFFFu}),
            (VulkanReplaySeedTexel{255, 255, 255, 255, 1}));
  EXPECT_EQ(decodeVulkanReplaySeedTexel(Color16{0x801Fu}),
            (VulkanReplaySeedTexel{255, 0, 0, 255, 1}));
  EXPECT_EQ(decodeVulkanReplaySeedTexel(Color16{0x03E0u}),
            (VulkanReplaySeedTexel{0, 255, 0, 255, 0}));
  EXPECT_EQ(decodeVulkanReplaySeedTexel(Color16{0x7C00u}),
            (VulkanReplaySeedTexel{0, 0, 255, 255, 0}));

  // Bit replication, not a coarse left shift, is the exact M0/M1 bridge.
  EXPECT_EQ(expandReplaySeed5To8(1u), 8u);
  EXPECT_EQ(expandReplaySeed5To8(16u), 132u);
  EXPECT_EQ(expandReplaySeed5To8(31u), 255u);
}

TEST(GpuModernOutputTest, ReplaySeedReplicatesNativePixelsAtFourXAndClearsFar) {
  EXPECT_EQ(mapVulkanReplaySeedCoordinate(0, 0, 4),
            (VulkanReplaySeedCoordinate{0, 0}));
  EXPECT_EQ(mapVulkanReplaySeedCoordinate(3, 3, 4),
            (VulkanReplaySeedCoordinate{0, 0}));
  EXPECT_EQ(mapVulkanReplaySeedCoordinate(4, 4, 4),
            (VulkanReplaySeedCoordinate{1, 1}));
  EXPECT_EQ(mapVulkanReplaySeedCoordinate(4095, 2047, 4),
            (VulkanReplaySeedCoordinate{1023, 511}));
  EXPECT_FLOAT_EQ(VULKAN_REPLAY_SEED_DEPTH_CLEAR, 1.0f);
}

TEST(GpuModernOutputTest,
     VulkanReplayDepthDomainPreservesOrderingAcrossFallbackW) {
  using namespace ps1::gpu::replay::vulkan_abi;

  constexpr auto below = tryNormalizeProjectiveWDepth(16384.0);
  constexpr auto equal = tryNormalizeProjectiveWDepth(32768.0);
  constexpr auto above = tryNormalizeProjectiveWDepth(65536.0);
  static_assert(below.has_value());
  static_assert(equal.has_value());
  static_assert(above.has_value());
  static_assert(*below < *equal && *equal < *above);

  EXPECT_FLOAT_EQ(*below, 0x1p-18f);
  EXPECT_FLOAT_EQ(*equal, 0x1p-17f);
  EXPECT_FLOAT_EQ(*above, 0x1p-16f);
  EXPECT_LT(*below, *equal);
  EXPECT_LT(*equal, *above);
}

TEST(GpuModernOutputTest, VulkanReplayDepthDomainRejectsOutOfRangeW) {
  using namespace ps1::gpu::replay::vulkan_abi;

  constexpr double maximum = static_cast<double>(PROJECTIVE_W_DEPTH_DOMAIN_MAX);
  constexpr auto normalizedMaximum = tryNormalizeProjectiveWDepth(maximum);
  static_assert(normalizedMaximum.has_value());
  static_assert(*normalizedMaximum == 1.0f);
  static_assert(!tryNormalizeProjectiveWDepth(maximum + 1.0).has_value());

  EXPECT_TRUE(isProjectiveWInDepthDomain(maximum));
  EXPECT_FLOAT_EQ(*normalizedMaximum, 1.0f);
  EXPECT_FALSE(isProjectiveWInDepthDomain(0.0));
  EXPECT_FALSE(isProjectiveWInDepthDomain(-1.0));
  EXPECT_FALSE(isProjectiveWInDepthDomain(maximum + 1.0));
  EXPECT_FALSE(tryNormalizeProjectiveWDepth(maximum + 1.0).has_value());
}

TEST(GpuModernOutputTest, BoundsReplayAtlasScaleLikeVulkanPresentation) {
  EXPECT_EQ(planVulkanReplayResources(0).internalScale, 1u);
  EXPECT_EQ(planVulkanReplayResources(99).internalScale, 8u);
}

TEST(GpuModernOutputTest, QualifiesExactFourXReplayImageSet) {
  const VulkanReplayCapabilityAssessment assessment =
      assessVulkanReplayCapability(planVulkanReplayResources(4),
                                   fullReplayDeviceSupport());

  EXPECT_TRUE(assessment.capable());
  EXPECT_EQ(assessment.reason, VulkanReplayCapabilityReason::Supported);
  EXPECT_STREQ(vulkanReplayCapabilityReasonName(assessment.reason),
               "supported");
  EXPECT_EQ(assessment.plan.atlasExtent.width, 4096u);
  EXPECT_EQ(assessment.plan.atlasExtent.height, 2048u);
}

TEST(GpuModernOutputTest, ReportsFirstMissingReplayFormatFeatureSet) {
  VulkanReplayDeviceSupport support = fullReplayDeviceSupport();
  support.mask.requiredFeaturesSupported = false;

  const VulkanReplayCapabilityAssessment assessment =
      assessVulkanReplayCapability(planVulkanReplayResources(4), support);

  EXPECT_FALSE(assessment.capable());
  EXPECT_EQ(assessment.reason,
            VulkanReplayCapabilityReason::MaskFormatFeatures);
  EXPECT_STREQ(vulkanReplayCapabilityReasonName(assessment.reason),
               "mask-format-features");
}

TEST(GpuModernOutputTest, ReportsExactReplayImageUsageFailure) {
  VulkanReplayDeviceSupport support = fullReplayDeviceSupport();
  support.depth.imageUsageSupported = false;

  const VulkanReplayCapabilityAssessment assessment =
      assessVulkanReplayCapability(planVulkanReplayResources(4), support);

  EXPECT_FALSE(assessment.capable());
  EXPECT_EQ(assessment.reason, VulkanReplayCapabilityReason::DepthImageUsage);
}

TEST(GpuModernOutputTest, RequiresPresentedSurfaceDepthColorAttachment) {
  VulkanReplayDeviceSupport support = fullReplayDeviceSupport();
  support.surfaceDepth.requiredFeaturesSupported = false;

  const VulkanReplayCapabilityAssessment assessment =
      assessVulkanReplayCapability(planVulkanReplayResources(4), support);

  EXPECT_FALSE(assessment.capable());
  EXPECT_EQ(assessment.reason,
            VulkanReplayCapabilityReason::SurfaceDepthFormatFeatures);
  EXPECT_STREQ(vulkanReplayCapabilityReasonName(assessment.reason),
               "surface-depth-format-features");
}

TEST(GpuModernOutputTest, RequiresDepthSamplingInReplayFormatFeatureSet) {
  VulkanReplayDeviceSupport support = fullReplayDeviceSupport();
  support.depth.requiredFeaturesSupported = false;

  const VulkanReplayCapabilityAssessment assessment =
      assessVulkanReplayCapability(planVulkanReplayResources(4), support);

  EXPECT_FALSE(assessment.capable());
  EXPECT_EQ(assessment.reason,
            VulkanReplayCapabilityReason::DepthFormatFeatures);
}

TEST(GpuModernOutputTest, RejectsPerFormatAtlasExtentBeforeAllocation) {
  VulkanReplayDeviceSupport support = fullReplayDeviceSupport();
  support.color.maxExtent.width = 2048;

  const VulkanReplayCapabilityAssessment assessment =
      assessVulkanReplayCapability(planVulkanReplayResources(4), support);

  EXPECT_FALSE(assessment.capable());
  EXPECT_EQ(assessment.reason, VulkanReplayCapabilityReason::ColorExtent);
}

TEST(GpuModernOutputTest, RejectsFramebufferLimitBeforeAllocation) {
  VulkanReplayDeviceSupport support = fullReplayDeviceSupport();
  support.maxFramebufferHeight = 1024;

  const VulkanReplayCapabilityAssessment assessment =
      assessVulkanReplayCapability(planVulkanReplayResources(4), support);

  EXPECT_FALSE(assessment.capable());
  EXPECT_EQ(assessment.reason,
            VulkanReplayCapabilityReason::AtlasFramebufferExtent);
}

TEST(GpuModernOutputTest, VulkanFourXNeverAllocatesCpuFourXCompanion) {
  GPU gpu;
  RendererOptions vulkan;
  vulkan.internalScale = 4;
  vulkan.pgxp = true;
  vulkan.trueDepth = true;
  vulkan.trueColor = true;

  configureVulkanNativeOracle(gpu, true);
  EXPECT_EQ(vulkan.internalScale, 4);
  EXPECT_TRUE(isVulkanNativeOraclePolicyActive(gpu));
  EXPECT_EQ(gpu.internalResolutionScale(), 1u);
  EXPECT_FALSE(gpu.isEnhancedRasterEnabled());
  EXPECT_FALSE(gpu.isTrueDepthEnabled());
  EXPECT_FALSE(gpu.isTrueColorEnabled());
}

TEST(GpuModernOutputTest, PresentsProgrammedPalApertureAtFourTimesTrueColor) {
  GPU gpu;
  gpu.configureEnhancedPresentation(4, true);

  // Crash 3 PAL: 512-wide mode, 512x256 programmed scanout, right-hand
  // framebuffer. These are the values observed in the transition capture.
  gpu.writeGP1(0x0800000Au);
  gpu.writeGP1(0x06000000u | 608u | (3168u << 12));
  gpu.writeGP1(0x07000000u | 37u | (293u << 10));
  gpu.writeGP1(0x05000000u | 512u);

  // A 1x1 untextured rectangle with values that quantize to black in RGB555.
  // The guest-visible target must remain black while RGBA8 retains 1,2,3.
  gpu.writeGP0(0x68030201u);
  gpu.writeGP0(512u);
  EXPECT_EQ(gpu.getVRAM()[512].raw & 0x7FFFu, 0u);

  gpu.snapshotDisplayBuffer();
  std::vector<Color32> display;
  uint32_t width = 0;
  uint32_t height = 0;
  GPU::DisplayState state;
  ASSERT_TRUE(gpu.copyDisplayRGBA8(display, width, height, &state));
  EXPECT_EQ(width, 2048u);
  EXPECT_EQ(height, 1024u);
  ASSERT_EQ(display.size(), static_cast<std::size_t>(width) * height);
  EXPECT_EQ(display.front(), (Color32{1, 2, 3, 255}));
  EXPECT_EQ(state.xStart, 512u);
  EXPECT_TRUE(state.horizontalRangeSet);
  EXPECT_TRUE(state.verticalRangeSet);
}

TEST(GpuModernOutputTest, CanBeDisabledWithoutChangingNativeSnapshot) {
  GPU gpu;
  gpu.configureEnhancedPresentation(1, false);
  gpu.writeGP0(0x680000FFu);
  gpu.writeGP0(0u);
  gpu.snapshotDisplayBuffer();

  std::vector<Color32> display;
  uint32_t width = 123;
  uint32_t height = 456;
  EXPECT_FALSE(gpu.copyDisplayRGBA8(display, width, height));
  EXPECT_TRUE(display.empty());
  EXPECT_EQ(width, 0u);
  EXPECT_EQ(height, 0u);

  std::vector<Color16> native(GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT);
  EXPECT_EQ(gpu.copyDisplayVRAM(native.data(), native.size()), native.size());
  EXPECT_EQ(native.front().raw & 0x1Fu, 31u);
}

TEST(GpuModernOutputTest, ReusedLatchUpdatesStateWithoutRecopyingModernPixels) {
  GPU gpu;
  gpu.configureEnhancedPresentation(2, true);
  gpu.writeGP0(0x68030201u);
  gpu.writeGP0(0u);
  gpu.snapshotDisplayBuffer(10u);

  std::vector<Color32> display;
  uint32_t width = 0;
  uint32_t height = 0;
  GPU::DisplayState state;
  ASSERT_EQ(
      gpu.copyDisplayRGBA8IfChanged(display, width, height, UINT64_MAX, &state),
      GPU::DisplayCopyStatus::Copied);
  const uint64_t firstRevision = state.contentRevision;
  ASSERT_FALSE(display.empty());
  EXPECT_EQ(display.front(), (Color32{1, 2, 3, 255}));

  const Color32 sentinel{17, 33, 65, 255};
  display.front() = sentinel;
  width = 0;
  height = 0;
  gpu.snapshotDisplayBuffer(11u);
  EXPECT_EQ(gpu.copyDisplayRGBA8IfChanged(display, width, height, firstRevision,
                                          &state),
            GPU::DisplayCopyStatus::Unchanged);
  EXPECT_EQ(display.front(), sentinel)
      << "an unchanged copy must leave the caller's pixels untouched";
  EXPECT_EQ(width, 640u);
  EXPECT_EQ(height, 480u);
  EXPECT_EQ(state.latchFrame, 11u);
  EXPECT_EQ(state.contentRevision, firstRevision);

  gpu.writeGP0(0x6800FF00u);
  gpu.writeGP0(0u);
  gpu.snapshotDisplayBuffer(12u);
  ASSERT_EQ(gpu.copyDisplayRGBA8IfChanged(display, width, height, firstRevision,
                                          &state),
            GPU::DisplayCopyStatus::Copied);
  EXPECT_NE(state.contentRevision, firstRevision);
  EXPECT_EQ(state.latchFrame, 12u);
  EXPECT_EQ(display.front(), (Color32{0, 255, 0, 255}));
}
