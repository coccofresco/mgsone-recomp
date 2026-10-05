#include "runtime/games/crash3_profile.h"
#include "runtime/games/mgs_asset_cache.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/games/mgs_screen_effects.h"
#include "runtime/games/mgs_surface_effects.h"
#include "runtime/gpu/modern_replay_preflight.h"
#include "runtime/gpu/modern_replay_visibility_stack.h"
#include "runtime/gpu/modern_replay_vulkan_abi.h"
#include "runtime/gpu/modern_replay_vulkan_bake.h"
#include "runtime/gpu/modern_replay_vulkan_copy.h"
#include "runtime/gpu/modern_replay_vulkan_diagnostics.h"
#include "runtime/gpu/renderer_options.h"
#include "runtime/gpu/renderer_vulkan.h"
#include "runtime/metrics.h"
#include "runtime/pgxp.h"
#include <cstring>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ps1;
using namespace ps1::gpu;
using namespace ps1::gpu::replay;

namespace {

constexpr VulkanReplayCopyExecutionLimits kLimits{4, 1024, 512, 4096, 2048};

ReplayPolicyState replayPolicy() {
  return {4, true, true, ReplayDepthMode::Hybrid};
}

FrameLatch replayLatch() {
  FrameLatch latch;
  latch.frameNumber = 11;
  latch.contentRevision = 22;
  latch.completedSubmissionId = 33;
  latch.sequenceEnd = 1000;
  latch.policy = replayPolicy();
  latch.display.aperture = NativeRect{0, 0, 320, 240};
  return latch;
}

CopyRect copyRect(NativePoint source, NativePoint destination, uint32_t width,
                  uint32_t height, MaskState mask = {}) {
  CopyRect copy;
  copy.source = source;
  copy.destination = destination;
  copy.width = width;
  copy.height = height;
  copy.mask = mask;
  const auto tiles = conservativeNativeReadTileIndices(copy);
  copy.nativeRead.required = true;
  copy.nativeRead.tileCount = static_cast<uint32_t>(tiles.size());
  copy.nativeRead.versionHash = CRASH_REPLAY_READ_HASH_OFFSET;
  for (const uint32_t tile : tiles) {
    copy.nativeRead.versionHash =
        appendNativeReadDependencyHash(copy.nativeRead.versionHash, tile, 1);
  }
  return copy;
}

VulkanReplayBakeResult bakeCopyResult(CopyRect copy) {
  CrashReplayFrameBuilder builder;
  builder.record(std::move(copy));
  const CrashReplayFramePtr frame = builder.seal(replayLatch());
  ReplayExecutionPlanResult planned =
      planModernReplayExecution(std::array{frame});
  if (!planned.accepted())
    throw std::logic_error("copy fixture did not produce an execution plan");
  VulkanReplayBakeFeatures features;
  features.allowCopyRectScratch = true;
  return bakeModernReplayVulkan(planned.plan, 4, features);
}

VulkanReplayBakedCopyRect bakeCopy(CopyRect copy) {
  VulkanReplayBakeResult baked = bakeCopyResult(std::move(copy));
  if (!baked.accepted() || baked.batch.commands.empty())
    throw std::logic_error("copy fixture did not produce a Vulkan bake");
  return std::get<VulkanReplayBakedCopyRect>(
      baked.batch.commands.front().payload);
}

class ScopedEnvironment {
public:
  ScopedEnvironment(const char *name, const char *value) : name_(name) {
    if (const char *old = std::getenv(name))
      old_ = old;
    setenv(name, value, 1);
  }

  ~ScopedEnvironment() {
    if (old_.has_value())
      setenv(name_.c_str(), old_->c_str(), 1);
    else
      unsetenv(name_.c_str());
  }

private:
  std::string name_;
  std::optional<std::string> old_;
};

constexpr uint32_t packedVertex(int x, int y) {
  return static_cast<uint16_t>(x) |
         (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16);
}

PGXPValue projectedVertex(uint32_t packed, float x, float y,
                          uint64_t projectionId) {
  constexpr double depth = 16384.0;
  PGXPValue value;
  value.x = x;
  value.y = y;
  value.z = depth;
  value.value = packed;
  value.flags = PGXPValue::VALID_XY | PGXPValue::VALID_Z |
                PGXPValue::VALID_SIGNED_PROJECTION;
  value.origin = PGXPOrigin::GteProjection;
  value.qx = static_cast<double>(x) * depth;
  value.qy = static_cast<double>(y) * depth;
  value.nearDepth = 1.0;
  value.projectionId = projectionId;
  value.projectionContext = 0xC0FFEEu;
  return value;
}

PGXPValue projectedVertexAtDepth(uint32_t packed, float x, float y,
                                 double depth, uint64_t projectionId) {
  PGXPValue value = projectedVertex(packed, x, y, projectionId);
  value.z = depth;
  value.qx = static_cast<double>(x) * depth;
  value.qy = static_cast<double>(y) * depth;
  return value;
}

VulkanReplayDiagnosticReport
diagnosticReportForNativeSamples(const std::array<Color32, 16> &colors,
                                 const std::array<uint8_t, 16> &masks) {
  constexpr uint32_t scale = 4;
  constexpr uint32_t nativeWidth = 16;
  constexpr uint32_t nativeHeight = 16;
  constexpr uint32_t width = nativeWidth * scale;
  constexpr uint32_t height = nativeHeight * scale;
  std::vector<Color32> color(static_cast<std::size_t>(width) * height);
  std::vector<uint8_t> mask(color.size());
  std::vector<float> depth(color.size(), 1.0f);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const std::size_t index = static_cast<std::size_t>(y) * width + x;
      color[index] = colors[x / scale];
      mask[index] = masks[x / scale];
    }
  }
  const VulkanReplayDiagnosticPlanes planes{
      {color.data(), color.size() * sizeof(Color32), width * sizeof(Color32)},
      {mask.data(), mask.size(), width},
      {depth.data(), depth.size() * sizeof(float), width * sizeof(float)}};
  return captureVulkanReplayReadback({32, 0, 16, 16, scale}, planes);
}

VulkanReplayDiagnosticReport
diagnosticReportForPainterBoundarySamples(const std::array<Color32, 2> &colors,
                                          const std::array<uint8_t, 2> &masks,
                                          const std::array<float, 2> &depths) {
  constexpr uint32_t scale = 4;
  constexpr uint32_t nativeWidth = 2;
  constexpr uint32_t nativeHeight = 1;
  constexpr uint32_t width = nativeWidth * scale;
  constexpr uint32_t height = nativeHeight * scale;
  std::array<Color32, width * height> color{};
  std::array<uint8_t, width * height> mask{};
  std::array<float, width * height> depth{};
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const std::size_t index = static_cast<std::size_t>(y) * width + x;
      color[index] = colors[x / scale];
      mask[index] = masks[x / scale];
      depth[index] = depths[x / scale];
    }
  }
  const VulkanReplayDiagnosticPlanes planes{
      {color.data(), color.size() * sizeof(Color32), width * sizeof(Color32)},
      {mask.data(), mask.size(), width},
      {depth.data(), depth.size() * sizeof(float), width * sizeof(float)}};
  return captureVulkanReplayReadback({12, 12, nativeWidth, nativeHeight, scale},
                                     planes);
}

VulkanReplayDiagnosticReport
diagnosticReportForPainterWriteSamples(const std::array<Color32, 4> &colors,
                                       const std::array<uint8_t, 4> &masks,
                                       const std::array<float, 4> &depths) {
  constexpr uint32_t scale = 4;
  constexpr uint32_t nativeWidth = 4;
  constexpr uint32_t nativeHeight = 1;
  constexpr uint32_t width = nativeWidth * scale;
  constexpr uint32_t height = nativeHeight * scale;
  std::array<Color32, width * height> color{};
  std::array<uint8_t, width * height> mask{};
  std::array<float, width * height> depth{};
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const std::size_t index = static_cast<std::size_t>(y) * width + x;
      color[index] = colors[x / scale];
      mask[index] = masks[x / scale];
      depth[index] = depths[x / scale];
    }
  }
  const VulkanReplayDiagnosticPlanes planes{
      {color.data(), color.size() * sizeof(Color32), width * sizeof(Color32)},
      {mask.data(), mask.size(), width},
      {depth.data(), depth.size() * sizeof(float), width * sizeof(float)}};
  return captureVulkanReplayReadback({12, 12, nativeWidth, nativeHeight, scale},
                                     planes);
}

int64_t metricHash(uint64_t hash) { return std::bit_cast<int64_t>(hash); }

void submitFill(GPU &gpu, uint8_t r, uint8_t g, uint8_t b, uint32_t x,
                uint32_t y, uint32_t width, uint32_t height) {
  gpu.writeGP0(0x02000000u | r | (static_cast<uint32_t>(g) << 8) |
               (static_cast<uint32_t>(b) << 16));
  gpu.writeGP0(x | (y << 16));
  gpu.writeGP0(width | (height << 16));
}

void submitCpuUpload(GPU &gpu, uint32_t x, uint32_t y, uint32_t width,
                     uint32_t height, const std::vector<uint16_t> &pixels) {
  if (pixels.size() != static_cast<std::size_t>(width) * height)
    throw std::invalid_argument("CPU upload fixture size mismatch");
  gpu.writeGP0(0xA0000000u);
  gpu.writeGP0(x | (y << 16));
  gpu.writeGP0(width | (height << 16));
  for (std::size_t index = 0; index < pixels.size(); index += 2) {
    const uint32_t high = index + 1 < pixels.size() ? pixels[index + 1] : 0;
    gpu.writeGP0(pixels[index] | (high << 16));
  }
}

void submitCopy(GPU &gpu, uint32_t sourceX, uint32_t sourceY,
                uint32_t destinationX, uint32_t destinationY, uint32_t width,
                uint32_t height) {
  gpu.writeGP0(0x80000000u);
  gpu.writeGP0(sourceX | (sourceY << 16));
  gpu.writeGP0(destinationX | (destinationY << 16));
  gpu.writeGP0(width | (height << 16));
}

void submitProjectedQuad(GPU &gpu, uint32_t command, int left, int top,
                         int right, int bottom, double depth,
                         uint64_t firstProjectionId) {
  const std::array<uint32_t, 4> packed{
      packedVertex(left, top), packedVertex(right, top),
      packedVertex(left, bottom), packedVertex(right, bottom)};
  gpu.writeGP0(command);
  for (std::size_t index = 0; index < packed.size(); ++index) {
    const float x = static_cast<float>(static_cast<int16_t>(packed[index]));
    const float y =
        static_cast<float>(static_cast<int16_t>(packed[index] >> 16u));
    const PGXPValue precision = projectedVertexAtDepth(
        packed[index], x, y, depth, firstProjectionId + index);
    gpu.writeGP0(packed[index], UINT32_MAX, &precision);
  }
}

void submitProjectedTexturedQuad(GPU &gpu, int left, int top, int right,
                                 int bottom, uint8_t u, uint8_t v,
                                 uint16_t texturePage, double depth,
                                 uint64_t firstProjectionId,
                                 uint16_t clut = 0) {
  const std::array<uint32_t, 4> packed{
      packedVertex(left, top), packedVertex(right, top),
      packedVertex(left, bottom), packedVertex(right, bottom)};
  gpu.writeGP0(0x2D000000u); // raw, opaque textured quad
  for (std::size_t index = 0; index < packed.size(); ++index) {
    const float x = static_cast<float>(static_cast<int16_t>(packed[index]));
    const float y =
        static_cast<float>(static_cast<int16_t>(packed[index] >> 16u));
    const PGXPValue precision = projectedVertexAtDepth(
        packed[index], x, y, depth, firstProjectionId + index);
    gpu.writeGP0(packed[index], UINT32_MAX, &precision);
    uint32_t textureWord =
        static_cast<uint32_t>(u) | (static_cast<uint32_t>(v) << 8u);
    if (index == 0)
      textureWord |= uint32_t(clut) << 16;
    if (index == 1)
      textureWord |= static_cast<uint32_t>(texturePage) << 16u;
    gpu.writeGP0(textureWord);
  }
}

} // namespace

TEST(GpuModernReplayVulkanCopyTest,
     AcceptsSixRegionWrappedMaskedAtlasOnlySnapshot) {
  const VulkanReplayBakedCopyRect copy =
      bakeCopy(copyRect({1000, 500}, {900, 500}, 200, 100, {true, true}));

  ASSERT_EQ(copy.path, VulkanReplayCopyPath::ScratchSnapshot);
  ASSERT_EQ(copy.regionCount, 6u);
  EXPECT_FALSE(copy.copyNativeR16);
  EXPECT_TRUE(copy.snapshotSourceColorRgba8);
  EXPECT_TRUE(copy.snapshotSourceMaskR8);
  EXPECT_TRUE(copy.snapshotDestinationMaskR8);
  EXPECT_EQ(validateVulkanReplayCopyForExecution(copy, kLimits),
            VulkanReplayCopyValidationReason::None);
}

TEST(GpuModernReplayVulkanCopyTest,
     AcceptsOverlapAndForceMaskWithoutDestinationMaskSnapshot) {
  const VulkanReplayBakedCopyRect overlap =
      bakeCopy(copyRect({100, 100}, {104, 100}, 32, 8));
  ASSERT_EQ(overlap.path, VulkanReplayCopyPath::ScratchSnapshot);
  ASSERT_EQ(overlap.regionCount, 1u);
  EXPECT_EQ(validateVulkanReplayCopyForExecution(overlap, kLimits),
            VulkanReplayCopyValidationReason::None);

  const VulkanReplayBakedCopyRect force =
      bakeCopy(copyRect({0, 264}, {0, 0}, 512, 240, {true, false}));
  ASSERT_EQ(force.path, VulkanReplayCopyPath::ScratchSnapshot);
  EXPECT_TRUE(force.mask.forceMaskBit);
  EXPECT_FALSE(force.snapshotDestinationMaskR8);
  EXPECT_EQ(validateVulkanReplayCopyForExecution(force, kLimits),
            VulkanReplayCopyValidationReason::None);
}

TEST(GpuModernReplayVulkanCopyTest,
     RejectsDonorUnprovenAndCyclicOverlapButKeepsSafeHorizontalOverlap) {
  const VulkanReplayBakeResult vertical =
      bakeCopyResult(copyRect({100, 100}, {100, 101}, 8, 2));
  EXPECT_FALSE(vertical.accepted());
  EXPECT_EQ(vertical.reason,
            VulkanReplayBakeReason::CopyRectOrderedOverlapUnproven);
  EXPECT_TRUE(vertical.batch.empty());

  const VulkanReplayBakeResult wrappedVertical =
      bakeCopyResult(copyRect({100, 511}, {100, 0}, 8, 2));
  EXPECT_FALSE(wrappedVertical.accepted());
  EXPECT_EQ(wrappedVertical.reason,
            VulkanReplayBakeReason::CopyRectOrderedOverlapUnproven);
  EXPECT_TRUE(wrappedVertical.batch.empty());

  const VulkanReplayBakeResult horizontalCycle =
      bakeCopyResult(copyRect({0, 100}, {1, 100}, 1024, 1));
  EXPECT_FALSE(horizontalCycle.accepted());
  EXPECT_EQ(horizontalCycle.reason,
            VulkanReplayBakeReason::CopyRectOrderedOverlapUnproven);
  EXPECT_TRUE(horizontalCycle.batch.empty());

  const VulkanReplayBakeResult wrappedHorizontalCycle =
      bakeCopyResult(copyRect({0, 100}, {1023, 100}, 2, 1));
  EXPECT_FALSE(wrappedHorizontalCycle.accepted());
  EXPECT_EQ(wrappedHorizontalCycle.reason,
            VulkanReplayBakeReason::CopyRectOrderedOverlapUnproven);
  EXPECT_TRUE(wrappedHorizontalCycle.batch.empty());

  const VulkanReplayBakedCopyRect horizontal =
      bakeCopy(copyRect({100, 100}, {101, 100}, 8, 2));
  EXPECT_EQ(horizontal.path, VulkanReplayCopyPath::ScratchSnapshot);
}

TEST(GpuModernReplayVulkanCopyTest,
     RejectsNativeR16AndIncompleteScratchPlaneContracts) {
  const VulkanReplayBakedCopyRect valid =
      bakeCopy(copyRect({1000, 500}, {900, 500}, 200, 100, {true, true}));

  VulkanReplayBakedCopyRect copy = valid;
  copy.copyNativeR16 = true;
  EXPECT_EQ(validateVulkanReplayCopyForExecution(copy, kLimits),
            VulkanReplayCopyValidationReason::InvalidPlaneContract);

  copy = valid;
  copy.snapshotSourceMaskR8 = false;
  EXPECT_EQ(validateVulkanReplayCopyForExecution(copy, kLimits),
            VulkanReplayCopyValidationReason::InvalidPlaneContract);

  copy = valid;
  copy.snapshotDestinationMaskR8 = false;
  EXPECT_EQ(validateVulkanReplayCopyForExecution(copy, kLimits),
            VulkanReplayCopyValidationReason::InvalidPlaneContract);

  copy = valid;
  copy.nativeRead.required = false;
  EXPECT_EQ(validateVulkanReplayCopyForExecution(copy, kLimits),
            VulkanReplayCopyValidationReason::InvalidNativeRead);

  copy = valid;
  copy.regionCount = static_cast<uint8_t>(copy.regions.size() + 1u);
  EXPECT_EQ(validateVulkanReplayCopyForExecution(copy, kLimits),
            VulkanReplayCopyValidationReason::InvalidScratchRegionCount);
}

TEST(GpuModernReplayVulkanCopyTest,
     RejectsScalePartitionAndWrappedMappingCorruption) {
  const VulkanReplayBakedCopyRect valid =
      bakeCopy(copyRect({1000, 500}, {900, 500}, 200, 100, {true, true}));

  VulkanReplayBakedCopyRect copy = valid;
  ++copy.regions[1].sourceAtlas.x;
  EXPECT_EQ(validateVulkanReplayCopyForExecution(copy, kLimits),
            VulkanReplayCopyValidationReason::ScratchScaleMismatch);

  copy = valid;
  copy.regions[1] = copy.regions[0];
  EXPECT_EQ(validateVulkanReplayCopyForExecution(copy, kLimits),
            VulkanReplayCopyValidationReason::ScratchPartition);

  copy = valid;
  ++copy.regions[1].sourceNative.x;
  copy.regions[1].sourceAtlas.x += kLimits.internalScale;
  EXPECT_EQ(validateVulkanReplayCopyForExecution(copy, kLimits),
            VulkanReplayCopyValidationReason::ScratchMapping);
}

TEST(GpuModernReplayVulkanCopyTest,
     KeepsDirectAndScratchExecutionContractsExplicitlySeparate) {
  const VulkanReplayBakedCopyRect direct =
      bakeCopy(copyRect({0, 264}, {0, 0}, 512, 240));
  ASSERT_EQ(direct.path, VulkanReplayCopyPath::DirectNonOverlap);
  EXPECT_EQ(validateVulkanReplayCopyForExecution(direct, kLimits),
            VulkanReplayCopyValidationReason::None);

  VulkanReplayBakedCopyRect overlap = direct;
  overlap.destinationNative = {4, 264, 512, 240};
  overlap.destinationAtlas = {16, 1056, 2048, 960};
  EXPECT_EQ(validateVulkanReplayCopyForExecution(overlap, kLimits),
            VulkanReplayCopyValidationReason::DirectOverlap);

  overlap.path = VulkanReplayCopyPath::ScratchSnapshot;
  EXPECT_EQ(validateVulkanReplayCopyForExecution(overlap, kLimits),
            VulkanReplayCopyValidationReason::InvalidPlaneContract);
}

TEST(GpuModernReplayVulkanCopyTest,
     PushConstantUsesLogicalScratchForBothSourceAndDestinationMask) {
  const VulkanReplayBakedCopyRect copy =
      bakeCopy(copyRect({1000, 500}, {900, 500}, 200, 100, {true, true}));
  const vulkan_abi::ScratchCopyPushConstants push =
      makeVulkanReplayScratchCopyPushConstants(copy.regions[1], copy.mask);
  EXPECT_EQ(push.sourceDestination,
            (std::array<uint32_t, 4>{96, 0, 3696, 2000}));
  EXPECT_EQ(push.extentFlags, (std::array<uint32_t, 4>{400, 48, 1, 1}));
  EXPECT_EQ(sizeof(push), 32u);
}

TEST(GpuModernReplayVulkanCopyTest,
     OptInGpuDepthPresentationReconstructsPreAndSplitViews) {
  if (std::getenv("PS1_RUN_VULKAN_INTEGRATION") == nullptr)
    GTEST_SKIP() << "set PS1_RUN_VULKAN_INTEGRATION=1 for the GPU oracle";

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);
  GPU gpu;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.internalScale = 4;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueDepthMode = "hybrid";
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(renderer.init("Vulkan RGB depth comparison GPU oracle", 320, 240,
                            false, false));

  gpu.writeGP1(0x03000000u); // display enable
  gpu.writeGP1(0x05000000u); // display origin (0,0)
  gpu.writeGP0(0xE3000000u); // draw area top-left (0,0)
  gpu.writeGP0(0xE4000000u | 1023u | (511u << 10));
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();

  renderer.toggleDepthVisualization(); // post-correction
  renderer.toggleDepthVisualization(); // pre-correction
  submitProjectedQuad(gpu, 0x280000FFu, 10, 10, 80, 80, 8192.0, 100);
  gpu.snapshotDisplayBuffer(2);
  renderer.renderFrame();

  ASSERT_TRUE(renderer.processEvents());
  EXPECT_EQ(renderer.presentationView(),
            VulkanPresentationView::DepthPreCorrection);
  EXPECT_EQ(
      ps1::metrics::getState("host.renderer.depth_visualization.active_mode"),
      static_cast<int64_t>(VulkanPresentationView::DepthPreCorrection));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.depth_visualization.pre_correction_available"),
            1);
  EXPECT_GT(
      ps1::metrics::get("host.renderer.depth_visualization.pre_reconstruction"),
      0u);
  EXPECT_GT(ps1::metrics::get("host.renderer.vulkan_visibility.submitted"), 0u);

  renderer.toggleDepthVisualization(); // aligned pre | post split
  submitProjectedQuad(gpu, 0x2800FF00u, 30, 30, 100, 100, 12288.0, 200);
  gpu.snapshotDisplayBuffer(3);
  renderer.renderFrame();

  ASSERT_TRUE(renderer.processEvents());
  EXPECT_EQ(renderer.presentationView(),
            VulkanPresentationView::DepthComparison);
  EXPECT_EQ(
      ps1::metrics::getState("host.renderer.depth_visualization.active_mode"),
      static_cast<int64_t>(VulkanPresentationView::DepthComparison));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.depth_visualization.pre_correction_available"),
            1);

  renderer.toggleDepthVisualization();
  EXPECT_EQ(renderer.presentationView(), VulkanPresentationView::Color);
  renderer.destroy();
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST(GpuModernReplayVulkanCopyTest,
     OptInGpuReadbackProvesTrueColorMaskOverlapAndDepthPlanes) {
  if (std::getenv("PS1_RUN_VULKAN_INTEGRATION") == nullptr)
    GTEST_SKIP() << "set PS1_RUN_VULKAN_INTEGRATION=1 for the GPU oracle";

  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK", "all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM", "2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "4");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION", "32,0,16,16");
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  GPU gpu;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.internalScale = 4;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueDepthMode = "hybrid";
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(
      renderer.init("Vulkan CopyRect GPU oracle", 320, 240, false, false));

  gpu.writeGP1(0x03000000u); // display enable
  gpu.writeGP1(0x05000000u); // display origin (0,0)
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();

  // Establish genuine D32 content in the future CopyRect destination. The
  // next readback must differ from an all-far plane, proving that the copy's
  // accepted-pixel invalidation is not a vacuous clear of already-far data.
  gpu.writeGP0(0xE3000000u);
  gpu.writeGP0(0xE4000000u | 1023u | (511u << 10));
  constexpr std::array<uint32_t, 4> packed{
      packedVertex(32, 0), packedVertex(48, 0), packedVertex(32, 16),
      packedVertex(48, 16)};
  const std::array<PGXPValue, 4> precision{
      projectedVertex(packed[0], 32.0f, 0.0f, 1),
      projectedVertex(packed[1], 48.0f, 0.0f, 2),
      projectedVertex(packed[2], 32.0f, 16.0f, 3),
      projectedVertex(packed[3], 48.0f, 16.0f, 4)};
  gpu.writeGP0(0x28C06020u);
  for (std::size_t index = 0; index < packed.size(); ++index)
    gpu.writeGP0(packed[index], UINT32_MAX, &precision[index]);
  gpu.snapshotDisplayBuffer(2);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();

  std::array<Color32, 16> black{};
  for (Color32 &sample : black)
    sample = {0, 0, 0, 255};
  const std::array<uint8_t, 16> clearMask{};
  const VulkanReplayDiagnosticReport allFar =
      diagnosticReportForNativeSamples(black, clearMask);
  ASSERT_TRUE(allFar.captured());
  EXPECT_NE(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_depth_hash"),
            metricHash(allFar.actual.depthD32));

  // A non-overlapping copy still selects ScratchSnapshot when force-mask is
  // active. It must preserve the fill's 24-bit color, set R8, and replace the
  // previously written D32 values with exact far depth.
  constexpr Color32 trueColor{13, 37, 91, 255};
  submitFill(gpu, trueColor.r, trueColor.g, trueColor.b, 0, 0, 16, 16);
  gpu.writeGP0(0xE6000001u); // force mask, no destination check
  submitCopy(gpu, 0, 0, 32, 0, 16, 16);
  gpu.snapshotDisplayBuffer(3);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();

  std::array<Color32, 16> forcedColors{};
  forcedColors.fill(trueColor);
  std::array<uint8_t, 16> forcedMasks{};
  forcedMasks.fill(1);
  const VulkanReplayDiagnosticReport forcedExpected =
      diagnosticReportForNativeSamples(forcedColors, forcedMasks);
  ASSERT_TRUE(forcedExpected.captured());
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
            metricHash(forcedExpected.actual.rgba8));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_mask_hash"),
            metricHash(forcedExpected.actual.maskR8));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_depth_hash"),
            metricHash(forcedExpected.actual.depthD32));

  // Upload a per-column RGB555/mask pattern, then move it four pixels to the
  // right through an overlapping check-mask copy. Expected output is derived
  // from the immutable pre-write source and destination masks; an in-place or
  // live-mask implementation produces a different hash.
  gpu.writeGP0(0xE6000000u);
  std::vector<uint16_t> upload(16u * 16u);
  std::array<uint16_t, 16> rawColumns{};
  for (uint32_t x = 0; x < rawColumns.size(); ++x) {
    const uint16_t r = static_cast<uint16_t>((x * 2u + 1u) & 31u);
    const uint16_t g = static_cast<uint16_t>((x * 3u + 2u) & 31u);
    const uint16_t b = static_cast<uint16_t>((x * 5u + 3u) & 31u);
    rawColumns[x] = static_cast<uint16_t>(r | (g << 5) | (b << 10) |
                                          (x % 3u == 1u ? 0x8000u : 0u));
  }
  for (uint32_t y = 0; y < 16; ++y)
    std::copy(rawColumns.begin(), rawColumns.end(), upload.begin() + y * 16u);
  submitCpuUpload(gpu, 32, 0, 16, 16, upload);
  gpu.writeGP0(0xE6000002u); // check destination mask, do not force
  submitCopy(gpu, 32, 0, 36, 0, 16, 16);
  gpu.snapshotDisplayBuffer(4);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();

  std::array<Color32, 16> overlapColors{};
  std::array<uint8_t, 16> overlapMasks{};
  for (uint32_t x = 0; x < overlapColors.size(); ++x) {
    uint32_t selected = x;
    if (x >= 4 && (rawColumns[x] & 0x8000u) == 0)
      selected = x - 4;
    overlapColors[x] = EnhancedSurface::expandNativeColor(rawColumns[selected]);
    overlapMasks[x] =
        static_cast<uint8_t>((rawColumns[selected] & 0x8000u) != 0u);
  }
  const VulkanReplayDiagnosticReport overlapExpected =
      diagnosticReportForNativeSamples(overlapColors, overlapMasks);
  ASSERT_TRUE(overlapExpected.captured());
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
            metricHash(overlapExpected.actual.rgba8));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_mask_hash"),
            metricHash(overlapExpected.actual.maskR8));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_depth_hash"),
            metricHash(overlapExpected.actual.depthD32));
  EXPECT_EQ(ps1::metrics::get(
                "host.renderer.vulkan_replay_copy_path.scratch_snapshot"),
            2u);
  EXPECT_EQ(ps1::metrics::get(
                "host.renderer.vulkan_replay_copy_scratch_apply_regions"),
            2u);
  EXPECT_EQ(ps1::metrics::get("host.renderer.vulkan_replay_executor_downgrade"),
            0u);
  constexpr std::string_view coverage =
      "host.renderer.replay_coverage.m1_submitted.";
  const auto submittedCoverage = [coverage](std::string_view suffix) {
    return ps1::metrics::get(std::string(coverage) + std::string(suffix));
  };
  EXPECT_EQ(submittedCoverage("prefixes"), 3u);
  EXPECT_EQ(submittedCoverage("frames"), 3u);
  EXPECT_EQ(submittedCoverage("capture_complete_prefixes"), 3u);
  EXPECT_EQ(submittedCoverage("capture_incomplete_prefixes"), 0u);
  EXPECT_EQ(submittedCoverage("high_precision_complete_prefixes"), 3u);
  EXPECT_EQ(submittedCoverage("high_precision_incomplete_prefixes"), 0u);
  EXPECT_EQ(submittedCoverage("draw_packets"), 1u);
  EXPECT_EQ(submittedCoverage("projected_packets"), 1u);
  EXPECT_EQ(submittedCoverage("projected_vertices"), 4u);
  EXPECT_EQ(submittedCoverage("projected_xy_precise_vertices"), 4u);
  EXPECT_EQ(submittedCoverage("projected_w_valid_vertices"), 4u);
  EXPECT_EQ(submittedCoverage("projected_q_homogeneous_vertices"), 4u);
  EXPECT_EQ(submittedCoverage("projected_x_reconciled_vertices"), 0u);
  EXPECT_EQ(submittedCoverage("projected_y_reconciled_vertices"), 0u);
  EXPECT_EQ(submittedCoverage("projected_w_invented_vertices"), 0u);
  EXPECT_EQ(submittedCoverage("fallback_packets"), 0u);
  EXPECT_EQ(submittedCoverage("depth_eligible_packets"), 1u);
  EXPECT_EQ(submittedCoverage("depth_eligible_vertices"), 4u);
  EXPECT_EQ(submittedCoverage("depth_state_matched_packets"), 1u);
  EXPECT_EQ(submittedCoverage("depth_state_matched_vertices"), 4u);
  EXPECT_EQ(submittedCoverage("painter_projected_packets"), 0u);
  EXPECT_EQ(submittedCoverage("painter_projected_vertices"), 0u);

  constexpr std::string_view legacyInventory = "host.renderer.replay_ir.";
  constexpr std::string_view m1Inventory =
      "host.renderer.replay_ir.m1_submitted.";
  constexpr std::string_view m0Inventory =
      "host.renderer.replay_ir.m0_reanchor_submitted.";
  const auto inventory = [](std::string_view prefix, std::string_view suffix) {
    return ps1::metrics::get(std::string(prefix) + std::string(suffix));
  };
  constexpr std::array<std::string_view, 20> inventorySuffixes{
      "native_tile_upload",
      "surface_seed",
      "draw_packet",
      "draw_projected",
      "draw_screen_2d",
      "draw_textured",
      "draw_translucent",
      "draw_check_mask",
      "draw_vertices",
      "fill_rect",
      "copy_rect",
      "cpu_upload",
      "cpu_upload_words",
      "paint_pixels",
      "painted_pixels",
      "paint_rect",
      "paint_rect_commands",
      "clear_depth",
      "reset",
      "frame_latch"};
  for (const std::string_view suffix : inventorySuffixes) {
    EXPECT_EQ(inventory(legacyInventory, suffix),
              inventory(m1Inventory, suffix) + inventory(m0Inventory, suffix))
        << suffix;
  }
  EXPECT_EQ(inventory(legacyInventory, "surface_seed"), 1u);
  EXPECT_EQ(inventory(m0Inventory, "surface_seed"), 1u);
  EXPECT_EQ(inventory(m1Inventory, "surface_seed"), 0u);
  EXPECT_EQ(inventory(m1Inventory, "draw_packet"), 1u);
  EXPECT_EQ(inventory(m0Inventory, "draw_packet"), 0u);
  EXPECT_EQ(inventory(m1Inventory, "copy_rect"), 2u);
  EXPECT_EQ(inventory(m0Inventory, "copy_rect"), 0u);
  EXPECT_EQ(inventory(m1Inventory, "frame_latch"), 3u);
  EXPECT_EQ(inventory(m0Inventory, "frame_latch"), 1u);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.precision_replay"), 1);
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_submitted_queue_serial"),
            4);
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_confirmed_queue_serial"),
            2);

  renderer.destroy();
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST(GpuModernReplayVulkanDepthTest,
     OptInGpuPerspectiveDepthIsIndependentOfPlanarQuadDiagonal) {
  if (std::getenv("PS1_RUN_VULKAN_INTEGRATION") == nullptr)
    GTEST_SKIP() << "set PS1_RUN_VULKAN_INTEGRATION=1 for the GPU oracle";

  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK", "all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM", "2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "5");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION", "60,60,2,1");
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);
  GPU gpu;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.internalScale = 4;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueDepthMode = "strict";
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(renderer.init("Projective depth diagonal GPU oracle", 320, 240,
                            false, false));
  gpu.writeGP1(0x03000000u);
  gpu.writeGP1(0x05000000u);
  gpu.writeGP0(0xE3000000u);
  gpu.writeGP0(0xE4000000u | 1023u | (511u << 10));
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();

  // These screen corners are coplanar in 3D: reciprocal W is affine and
  // 1/W00 + 1/W11 == 1/W10 + 1/W01. At the centre W=6144. Screen-affine W
  // incorrectly gives ~8192 on one diagonal but ~6144 on the other.
  constexpr std::array<float, 4> xs{10, 110, 10, 110};
  constexpr std::array<float, 4> ys{10, 10, 110, 110};
  constexpr std::array<double, 4> ws{4096, 6144, 6144, 12288};
  constexpr std::array<std::array<uint8_t, 6>, 2> diagonals{
      {{0, 1, 2, 1, 3, 2}, {0, 1, 3, 0, 3, 2}}};
  for (uint32_t variant = 0; variant != 4u; ++variant) {
    submitFill(gpu, 0, 0, 0, 0, 0, 128, 128);
    const bool foregroundPlane = variant >= 2u;
    submitProjectedQuad(gpu, 0x28FF0000u, 0, 0, 128, 128,
                         foregroundPlane ? 5000.0 : 7000.0, 1000 + variant * 16);
    const auto &indices = diagonals[variant & 1u];
    for (uint32_t triangle = 0; triangle != 2u; ++triangle) {
      gpu.writeGP0(0x200000FFu);
      for (uint32_t corner = 0; corner != 3u; ++corner) {
        const auto index = indices[triangle * 3u + corner];
        const uint32_t packed = packedVertex(xs[index], ys[index]);
        const auto precision = projectedVertexAtDepth(
            packed, xs[index], ys[index], ws[index],
            2000 + variant * 16 + triangle * 3 + corner);
        gpu.writeGP0(packed, UINT32_MAX, &precision);
      }
    }
    gpu.snapshotDisplayBuffer(2 + variant);
    renderer.renderFrame();
    renderer.drainReplayDiagnostics();
    const Color32 expectedColor = foregroundPlane ? Color32{0, 0, 255, 255}
                                                  : Color32{255, 0, 0, 255};
    const auto expected = diagnosticReportForPainterBoundarySamples(
        {expectedColor, expectedColor}, {0, 0}, {1, 1});
    EXPECT_EQ(ps1::metrics::getState(
                  "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
              metricHash(expected.actual.rgba8)) << variant;
  }
  EXPECT_GT(ps1::metrics::get("host.renderer.replay_coverage.m1_submitted.vertices"),
            0u);
  renderer.destroy();
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST(GpuModernReplayVulkanDepthTest,
     OptInGpuPainterWriteKeepsOtLaterOpaqueVisibleAcrossRejectedHole) {
  if (std::getenv("PS1_RUN_VULKAN_INTEGRATION") == nullptr)
    GTEST_SKIP() << "set PS1_RUN_VULKAN_INTEGRATION=1 for the GPU oracle";

  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK", "all");
  ScopedEnvironment visibility("PS1_VULKAN_VISIBILITY_AUDIT", "1");
  ScopedEnvironment candidate("PS1_VULKAN_VISIBILITY_CANDIDATE_IDENTITY", "1");
  ScopedEnvironment catalog("PS1_VULKAN_VISIBILITY_TRACE_CATALOG", "1");
  ScopedEnvironment stack("PS1_VULKAN_VISIBILITY_STACK", "1");
  ScopedEnvironment stackRecords("PS1_VULKAN_VISIBILITY_STACK_RECORDS", "1024");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM", "2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "2");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION", "12,12,2,1");
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  GPU gpu;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.internalScale = 4;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueDepthMode = "hybrid";
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(
      renderer.init("Vulkan painter-depth GPU oracle", 320, 240, false, false));

  gpu.writeGP1(0x03000000u); // display enable
  gpu.writeGP1(0x05000000u); // display origin (0,0)
  gpu.writeGP0(0xE3000000u); // draw area top-left (0,0)
  gpu.writeGP0(0xE4000000u | 1023u | (511u << 10));
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();

  constexpr double nearDepth = 8192.0;
  constexpr double painterDepth = 4096.0;
  constexpr double middleDepth = 9216.0;
  // A owns D32 over both observed pixels. A2 marks only the control pixel with
  // E6 force-mask. B geometrically covers both pixels, but E6 check-mask lets
  // it invalidates depth only at the unmasked sample. C is farther than A and
  // must nevertheless win at both samples because GP0/OT order, not an
  // unproven cross-packet LEQUAL relation, owns Hybrid visibility. C stores
  // its genuine W in D32 at both accepted samples.
  submitProjectedQuad(gpu, 0x280000FFu, 10, 10, 30, 20, nearDepth, 100);
  gpu.writeGP0(0xE6000001u); // force mask, no destination check
  submitProjectedQuad(gpu, 0x280000FFu, 13, 12, 14, 13, nearDepth, 200);
  gpu.writeGP0(0xE6000002u); // check destination mask, do not force
  submitProjectedQuad(gpu, 0x2A00FF00u, 12, 12, 14, 13, painterDepth, 300);
  gpu.writeGP0(0xE6000000u); // C observes depth, not the E6 destination mask
  submitProjectedQuad(gpu, 0x28FF0000u, 10, 10, 30, 20, middleDepth, 400);
  gpu.snapshotDisplayBuffer(2);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();

  ASSERT_TRUE(renderer.processEvents());
  EXPECT_EQ(
      ps1::metrics::getState("host.renderer.vulkan_visibility.last_status"),
      static_cast<int64_t>(ReplayVisibilityAuditStatus::Compared));
  EXPECT_GT(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.last_conflict_samples"),
            0);
  EXPECT_GT(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.last_conflict_pairs"),
            0);
  EXPECT_GT(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.last_owned_agreement_samples"),
            0);
  EXPECT_EQ(
      ps1::metrics::getState("host.renderer.vulkan_visibility.catalog_active"),
      1);
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.last_catalog_eligible_owners"),
            6);
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.last_catalog_emitted_owners"),
            6);
  EXPECT_GT(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.last_catalog_conflict_owners"),
            0);
  EXPECT_EQ(
      ps1::metrics::get("host.renderer.vulkan_visibility.catalog_completed"),
      1u);
  EXPECT_EQ(
      ps1::metrics::getState("host.renderer.vulkan_visibility.stack_supported"),
      1);
  EXPECT_EQ(
      ps1::metrics::getState("host.renderer.vulkan_visibility.stack_active"),
      1);
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.stack_last_status"),
            static_cast<int64_t>(ReplayVisibilityStackStatus::Compared));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.stack_last_complete"),
            1);
  EXPECT_GT(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.stack_last_attempted_records"),
            0);
  EXPECT_GT(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.stack_last_occluder_pairs"),
            0);
  EXPECT_GE(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.stack_last_maximum_distinct_"
                "owners_per_sample"),
            2);
  EXPECT_EQ(
      ps1::metrics::get("host.renderer.vulkan_visibility.stack_completed"), 1u);
  EXPECT_EQ(ps1::metrics::get("host.renderer.vulkan_visibility.stack_overflow"),
            0u);
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.candidate_active"),
            1);
  EXPECT_EQ(
      ps1::metrics::get("host.renderer.vulkan_visibility.candidate_completed"),
      1u);
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.candidate_last_status"),
            static_cast<int64_t>(ReplayVisibilityAuditStatus::Compared));
  EXPECT_EQ(ps1::metrics::getState("host.renderer.vulkan_visibility."
                                   "candidate_last_baseline_agreement"),
            ps1::metrics::getState(
                "host.renderer.vulkan_visibility.last_agreement_samples"));
  EXPECT_EQ(ps1::metrics::getState("host.renderer.vulkan_visibility."
                                   "candidate_last_preserved_agreement"),
            ps1::metrics::getState(
                "host.renderer.vulkan_visibility.last_agreement_samples"));
  EXPECT_EQ(ps1::metrics::getState("host.renderer.vulkan_visibility."
                                   "candidate_last_baseline_conflicts"),
            ps1::metrics::getState(
                "host.renderer.vulkan_visibility.last_conflict_samples"));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.candidate_last_resolved"),
            0);
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.candidate_last_retained"),
            ps1::metrics::getState(
                "host.renderer.vulkan_visibility.last_conflict_samples"));
  EXPECT_EQ(ps1::metrics::getState("host.renderer.vulkan_visibility."
                                   "candidate_last_changed_winner"),
            0);
  EXPECT_EQ(ps1::metrics::getState("host.renderer.vulkan_visibility."
                                   "candidate_last_coverage_divergence"),
            0);
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.candidate_last_regressions"),
            0);
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.candidate_last_gate_pass"),
            0);
  const auto normalizedMiddle =
      vulkan_abi::tryNormalizeProjectiveWDepth(middleDepth);
  ASSERT_TRUE(normalizedMiddle.has_value());
  const VulkanReplayDiagnosticReport expected =
      diagnosticReportForPainterBoundarySamples(
          {Color32{0, 0, 255, 255}, Color32{0, 0, 255, 255}}, {0u, 0u},
          {*normalizedMiddle, *normalizedMiddle});
  ASSERT_TRUE(expected.captured());
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
            metricHash(expected.actual.rgba8));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_mask_hash"),
            metricHash(expected.actual.maskR8));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_depth_hash"),
            metricHash(expected.actual.depthD32));

  EXPECT_EQ(ps1::metrics::getState("host.renderer.precision_replay"), 1);
  EXPECT_EQ(ps1::metrics::get("host.renderer.vulkan_replay_executor_downgrade"),
            0u);
  EXPECT_EQ(
      ps1::metrics::get("host.renderer.replay_ir.m1_submitted.clear_depth"),
      0u);
  EXPECT_EQ(
      ps1::metrics::get("host.renderer.replay_ir.m1_submitted.draw_packet"),
      4u);
  EXPECT_EQ(
      ps1::metrics::get("host.renderer.replay_ir.m1_submitted.draw_check_mask"),
      1u);
  EXPECT_EQ(
      ps1::metrics::get("host.renderer.vulkan_replay_feedback_mask_snapshot"),
      2u);
  EXPECT_EQ(ps1::metrics::get("host.renderer.replay_coverage.m1_submitted."
                              "depth_eligible_packets"),
            3u);
  EXPECT_EQ(ps1::metrics::get("host.renderer.replay_coverage.m1_submitted."
                              "painter_precise_translucent_packets"),
            1u);
  EXPECT_EQ(ps1::metrics::get("host.renderer.replay_coverage.m1_submitted."
                              "ot_visibility_boundary_packets"),
            1u);
  EXPECT_EQ(ps1::metrics::get("host.renderer.replay_coverage.m1_submitted."
                              "ot_visibility_boundary_vertices"),
            4u);
  EXPECT_EQ(ps1::metrics::get("host.renderer.vulkan_replay_pipeline."
                              "precise_ot_visibility_boundary_draw"),
            2u);
  EXPECT_EQ(ps1::metrics::get("host.renderer.vulkan_replay_pipeline."
                              "opaque_untextured_painter_write_draw"),
            3u);
  EXPECT_EQ(ps1::metrics::get(
                "host.renderer.vulkan_replay_pipeline.precise_painter_draw"),
            0u);

  renderer.destroy();
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST(GpuModernReplayVulkanDepthTest,
     OptInGpuCrashSemiTransparencyWritesPresentedSurfaceOnlyWhereAccepted) {
  if (std::getenv("PS1_RUN_VULKAN_INTEGRATION") == nullptr)
    GTEST_SKIP() << "set PS1_RUN_VULKAN_INTEGRATION=1 for the GPU oracle";

  struct ProfileScope {
    ProfileScope() {
      games::crash3::configure(
          {.enabled = true, .presentationDepthCorrections = true});
    }
    ~ProfileScope() { games::crash3::configure({}); }
  } profileScope;
  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK", "all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM", "2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "2");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION", "12,12,2,1");
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  GPU gpu;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.internalScale = 4;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueDepthMode = "hybrid";
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(renderer.init("Crash presented-surface depth GPU oracle", 320,
                            240, false, false));

  gpu.writeGP1(0x03000000u);
  gpu.writeGP1(0x05000000u);
  gpu.writeGP0(0xE3000000u);
  gpu.writeGP0(0xE4000000u | 1023u | (511u << 10));
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();

  constexpr double opaqueDepth = 8192.0;
  constexpr double translucentDepth = 4096.0;
  submitProjectedQuad(gpu, 0x28FF0000u, 10, 10, 20, 20, opaqueDepth, 100);
  gpu.writeGP0(0xE6000000u);
  submitProjectedQuad(gpu, 0x2A0000FFu, 12, 12, 13, 13, translucentDepth, 200);
  gpu.writeGP0(0xE6000001u); // protect only the right sample
  submitProjectedQuad(gpu, 0x28FF0000u, 13, 12, 14, 13, opaqueDepth, 300);
  gpu.writeGP0(0xE6000002u);
  submitProjectedQuad(gpu, 0x2A0000FFu, 13, 12, 14, 13, translucentDepth, 400);
  gpu.snapshotDisplayBuffer(2);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();

  const auto normalizedOpaque =
      vulkan_abi::tryNormalizeProjectiveWDepth(opaqueDepth);
  const auto normalizedTranslucent =
      vulkan_abi::tryNormalizeProjectiveWDepth(translucentDepth);
  ASSERT_TRUE(normalizedOpaque.has_value());
  ASSERT_TRUE(normalizedTranslucent.has_value());
  const VulkanReplayDiagnosticReport expected =
      diagnosticReportForPainterBoundarySamples(
          {Color32{127, 0, 127, 255}, Color32{0, 0, 255, 255}}, {0u, 1u},
          {*normalizedTranslucent, *normalizedOpaque});
  ASSERT_TRUE(expected.captured());
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
            metricHash(expected.actual.rgba8));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_mask_hash"),
            metricHash(expected.actual.maskR8));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_depth_hash"),
            metricHash(expected.actual.depthD32));
  EXPECT_GT(ps1::metrics::get("host.renderer.crash3_profile.semi_fast_packet"),
            0u);
  EXPECT_GT(
      ps1::metrics::get("host.renderer.crash3_profile.depth_triangle_unmapped"),
      0u);

  renderer.destroy();
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST(GpuModernReplayVulkanDepthTest,
     OptInGpuFragmentStackOverflowReportsRequiredRecordsAndFailsClosed) {
  if (std::getenv("PS1_RUN_VULKAN_INTEGRATION") == nullptr)
    GTEST_SKIP() << "set PS1_RUN_VULKAN_INTEGRATION=1 for the GPU oracle";

  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK", "all");
  ScopedEnvironment visibility("PS1_VULKAN_VISIBILITY_AUDIT", "1");
  ScopedEnvironment catalog("PS1_VULKAN_VISIBILITY_TRACE_CATALOG", "1");
  ScopedEnvironment stack("PS1_VULKAN_VISIBILITY_STACK", "1");
  ScopedEnvironment stackRecords("PS1_VULKAN_VISIBILITY_STACK_RECORDS", "1");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM", "2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "2");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION", "12,12,1,1");
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  GPU gpu;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.internalScale = 4;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueDepthMode = "hybrid";
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(renderer.init("Vulkan fragment-stack overflow GPU oracle", 320,
                            240, false, false));

  gpu.writeGP1(0x03000000u);
  gpu.writeGP1(0x05000000u);
  gpu.writeGP0(0xE3000000u);
  gpu.writeGP0(0xE4000000u | 1023u | (511u << 10));
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();

  submitProjectedQuad(gpu, 0x280000FFu, 10, 10, 30, 20, 8192.0, 100);
  gpu.snapshotDisplayBuffer(2);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();

  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.stack_last_status"),
            static_cast<int64_t>(ReplayVisibilityStackStatus::Overflow));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.stack_last_complete"),
            0);
  EXPECT_GT(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.stack_last_attempted_records"),
            1);
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_visibility.stack_last_retained_records"),
            1);
  EXPECT_EQ(ps1::metrics::get("host.renderer.vulkan_visibility.stack_overflow"),
            1u);

  renderer.destroy();
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST(GpuModernReplayVulkanDepthTest,
     OptInGpuPainterWriteCoversTexturedDiscardAndMaskFeedback) {
  if (std::getenv("PS1_RUN_VULKAN_INTEGRATION") == nullptr)
    GTEST_SKIP() << "set PS1_RUN_VULKAN_INTEGRATION=1 for the GPU oracle";

  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK", "all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM", "2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "2");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION", "12,12,4,1");
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  GPU gpu;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.internalScale = 4;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueDepthMode = "hybrid";
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(renderer.init("Vulkan painter-write path oracle", 320, 240, false,
                            false));

  gpu.writeGP1(0x03000000u); // display enable
  gpu.writeGP1(0x05000000u); // display origin (0,0)
  gpu.writeGP0(0xE3000000u); // draw area top-left (0,0)
  gpu.writeGP0(0xE4000000u | 1023u | (511u << 10));
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();

  constexpr uint16_t direct15Page = 0x0101u; // X=64, Y=0, direct 15-bit
  constexpr double baseDepth = 8192.0;
  constexpr double texturedDepth = 4096.0;
  constexpr double feedbackDepth = 12288.0;
  // The two direct-color texels distinguish an accepted texture sample from
  // the PS1 zero-texel discard. Both packets use the opaque textured Hybrid
  // fast path; only the visible red texel may replace the blue color and D32.
  submitCpuUpload(gpu, 64, 0, 2, 1, {0x001Fu, 0x0000u});
  submitProjectedQuad(gpu, 0x28FF0000u, 12, 12, 16, 13, baseDepth, 100);
  submitProjectedTexturedQuad(gpu, 12, 12, 13, 13, 0, 0, direct15Page,
                              texturedDepth, 200);
  submitProjectedTexturedQuad(gpu, 13, 12, 14, 13, 1, 0, direct15Page,
                              texturedDepth, 300);

  // Force-mask the final control pixel, then cover two pixels with one opaque
  // projected quad under E6 destination checking. The unmasked sample must use
  // the generic precise painter-write pipeline and store real W; the masked
  // sample must reject color, mask, and depth together.
  gpu.writeGP0(0xE6000001u); // force mask, no destination check
  submitProjectedQuad(gpu, 0x28FF0000u, 15, 12, 16, 13, baseDepth, 400);
  gpu.writeGP0(0xE6000002u); // check destination mask, do not force
  submitProjectedQuad(gpu, 0x2800FF00u, 14, 12, 16, 13, feedbackDepth, 500);
  gpu.snapshotDisplayBuffer(2);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();

  ASSERT_TRUE(renderer.processEvents());
  const auto normalizedBase =
      vulkan_abi::tryNormalizeProjectiveWDepth(baseDepth);
  const auto normalizedTextured =
      vulkan_abi::tryNormalizeProjectiveWDepth(texturedDepth);
  const auto normalizedFeedback =
      vulkan_abi::tryNormalizeProjectiveWDepth(feedbackDepth);
  ASSERT_TRUE(normalizedBase.has_value());
  ASSERT_TRUE(normalizedTextured.has_value());
  ASSERT_TRUE(normalizedFeedback.has_value());
  const VulkanReplayDiagnosticReport expected =
      diagnosticReportForPainterWriteSamples(
          {Color32{255, 0, 0, 255}, Color32{0, 0, 255, 255},
           Color32{0, 255, 0, 255}, Color32{0, 0, 255, 255}},
          {0u, 0u, 0u, 1u},
          {*normalizedTextured, *normalizedBase, *normalizedFeedback,
           *normalizedBase});
  ASSERT_TRUE(expected.captured());
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
            metricHash(expected.actual.rgba8));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_mask_hash"),
            metricHash(expected.actual.maskR8));
  EXPECT_EQ(ps1::metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_depth_hash"),
            metricHash(expected.actual.depthD32));

  EXPECT_EQ(ps1::metrics::getState("host.renderer.precision_replay"), 1);
  EXPECT_EQ(ps1::metrics::get("host.renderer.vulkan_replay_executor_downgrade"),
            0u);
  EXPECT_EQ(
      ps1::metrics::get("host.renderer.replay_ir.m1_submitted.draw_packet"),
      5u);
  EXPECT_EQ(
      ps1::metrics::get("host.renderer.replay_ir.m1_submitted.draw_textured"),
      2u);
  EXPECT_EQ(
      ps1::metrics::get("host.renderer.replay_ir.m1_submitted.draw_check_mask"),
      1u);
  EXPECT_EQ(ps1::metrics::get("host.renderer.replay_coverage.m1_submitted."
                              "depth_eligible_packets"),
            5u);
  EXPECT_EQ(ps1::metrics::get("host.renderer.replay_coverage.m1_submitted."
                              "depth_state_matched_packets"),
            5u);
  EXPECT_EQ(ps1::metrics::get("host.renderer.vulkan_replay_pipeline."
                              "opaque_textured_painter_write_draw"),
            2u);
  EXPECT_EQ(ps1::metrics::get("host.renderer.vulkan_replay_pipeline."
                              "opaque_untextured_painter_write_draw"),
            2u);
  EXPECT_EQ(ps1::metrics::get("host.renderer.vulkan_replay_pipeline."
                              "precise_painter_write_draw"),
            2u);
  EXPECT_EQ(
      ps1::metrics::get("host.renderer.vulkan_replay_feedback_mask_snapshot"),
      2u);

  renderer.destroy();
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST(GpuModernReplayVulkanDepthTest,
     OptInGpuPaintRunsPreserveOpaqueBlendedPixelsAndHoles) {
  if (!std::getenv("PS1_RUN_VULKAN_INTEGRATION"))
    GTEST_SKIP();
  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK", "all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM", "2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "3");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION", "32,0,16,16");
  metrics::reset();
  metrics::setEnabledForTesting(true);
  GPU gpu;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.internalScale = 4;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueDepthMode = "hybrid";
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(
      renderer.init("Opaque paint runs GPU oracle", 320, 240, false, false));
  gpu.writeGP1(0x03000000u);
  gpu.writeGP1(0x05000000u);
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  gpu.writeGP0(0xE3000000u);
  gpu.writeGP0(0xE4000000u | 1023u | (511u << 10));
  gpu.writeGP0(0xE5000000u);
  gpu.writeGP0(0xE6000000u);
  for (const auto [x, width] :
       std::array<std::pair<uint32_t, uint32_t>, 2>{{{32, 5}, {39, 9}}}) {
    gpu.writeGP0(0x600000ffu);
    gpu.writeGP0(x);
    gpu.writeGP0(width | (16u << 16));
  }
  gpu.snapshotDisplayBuffer(2);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  std::array<Color32, 16> colors{};
  colors.fill({255, 0, 0, 255});
  colors[5] = colors[6] = {0, 0, 0, 255};
  const std::array<uint8_t, 16> masks{};
  const auto expected = diagnosticReportForNativeSamples(colors, masks);
  ASSERT_TRUE(expected.captured());
  EXPECT_EQ(metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
            metricHash(expected.actual.rgba8));
  EXPECT_EQ(metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_mask_hash"),
            metricHash(expected.actual.maskR8));
  EXPECT_EQ(metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_depth_hash"),
            metricHash(expected.actual.depthD32));
  // A uniform translucent source has different final native words over
  // the red bands and black hole, but may still be grouped horizontally.
  gpu.writeGP0(0xE1000000u);
  gpu.writeGP0(0x62006400u);
  gpu.writeGP0(32u);
  gpu.writeGP0(16u | (16u << 16));
  gpu.snapshotDisplayBuffer(3);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  colors.fill({127, 50, 0, 255});
  colors[5] = colors[6] = {0, 50, 0, 255};
  const auto blended = diagnosticReportForNativeSamples(colors, masks);
  ASSERT_TRUE(blended.captured());
  EXPECT_EQ(metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
            metricHash(blended.actual.rgba8));
  EXPECT_EQ(metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_mask_hash"),
            metricHash(blended.actual.maskR8));
  EXPECT_EQ(metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_depth_hash"),
            metricHash(blended.actual.depthD32));
  renderer.destroy();
  metrics::reset();
  metrics::setEnabledForTesting(false);
}

static void checkOriginalPcxGpuPalette(uint32_t requestedDepth) {
  const char *path = std::getenv("PS1_TEST_MGS_ASSET_CACHE");
  if (!std::getenv("PS1_RUN_VULKAN_INTEGRATION") || !path)
    GTEST_SKIP();
  const auto cache = games::mgs::AssetCache::load(path);
  using Cache = games::mgs::AssetCache;
  uint32_t chosen = ~0u, chosenPixel = 0;
  const auto word16 = [](const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8;
  };
  for (uint32_t t = 0;
       t < cache.section(Cache::Textures).count && chosen == ~0u; ++t) {
    const uint32_t a = cache.word(Cache::Textures, t, 7);
    const auto *pcx = cache.bytes().data() +
                      cache.section(Cache::Source).offset +
                      cache.word(Cache::Assets, a, 12);
    if ((cache.word(Cache::Textures, t, 6) & 1) != requestedDepth ||
        word16(pcx + 78) < 512 || cache.word(Cache::Textures, t, 0) > 128 ||
        cache.word(Cache::Textures, t, 1) > 128)
      continue;
    for (uint32_t i = 0; i < cache.word(Cache::Textures, t, 3); ++i) {
      const uint32_t index =
          cache.bytes()[cache.section(Cache::Indices).offset +
                        cache.word(Cache::Textures, t, 2) + i];
      const auto color = cache.word(
          Cache::Palettes, cache.word(Cache::Textures, t, 4) + index, 0);
      if (color && !(color & 0x8000)) {
        chosen = t;
        chosenPixel = i;
        break;
      }
    }
  }
  ASSERT_NE(chosen, ~0u);
  const uint32_t t = chosen, a = cache.word(Cache::Textures, t, 7);
  const auto *pcx = cache.bytes().data() + cache.section(Cache::Source).offset +
                    cache.word(Cache::Assets, a, 12);
  const uint32_t px = word16(pcx + 78), py = word16(pcx + 80),
                 cx = word16(pcx + 82), cy = word16(pcx + 84);
  const uint32_t width = cache.word(Cache::Textures, t, 0),
                 height = cache.word(Cache::Textures, t, 1);
  const uint32_t depth = cache.word(Cache::Textures, t, 6) & 1,
                 ppw = depth ? 2 : 4;
  const auto *indices = cache.bytes().data() +
                        cache.section(Cache::Indices).offset +
                        cache.word(Cache::Textures, t, 2);
  std::vector<uint16_t> pixels(width * height / ppw), palette;
  for (uint32_t i = 0; i < width * height; ++i)
    pixels[i / ppw] |= uint16_t(indices[i]) << ((i % ppw) * (depth ? 8 : 4));
  for (uint32_t i = 0; i < cache.word(Cache::Textures, t, 5); ++i)
    palette.push_back(
        cache.word(Cache::Palettes, cache.word(Cache::Textures, t, 4) + i, 0));
  const uint16_t source = palette[indices[chosenPixel]];
  const auto expand = [](uint32_t x) { return uint8_t((x << 3) | (x >> 2)); };
  const Color32 color{expand(source & 31), expand((source >> 5) & 31),
                      expand((source >> 10) & 31), 255};
  ScopedEnvironment mode("PS1_MGS_MESH_REPLAY", "1"),
      assets("PS1_MGS_ASSET_CACHE", path);
  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK", "all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM", "2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "3");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION", "32,0,16,16");
  struct Profile {
    Profile() { games::mgs::configure(true, false); }
    ~Profile() { games::mgs::configure(false, false); }
  } profile;
  metrics::reset();
  metrics::setEnabledForTesting(true);
  GPU gpu;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.internalScale = 4;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueDepthMode = "hybrid";
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(
      renderer.init("MGS original PCX GPU oracle", 320, 240, false, false));
  gpu.writeGP1(0x03000000);
  gpu.writeGP1(0x05000000);
  gpu.writeGP0(0xe3000000);
  gpu.writeGP0(0xe4000000 | 1023 | (511 << 10));
  submitCpuUpload(gpu, px, py, width / ppw, height, pixels);
  submitCpuUpload(gpu, cx, cy, palette.size(), 1, palette);
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  const uint16_t page = (px / 64) | ((py / 256) << 4) | (depth << 7),
                 clut = (cy * 64 + cx / 16);
  const uint8_t u = (px % 64) * ppw + chosenPixel % width,
                v = (py % 256) + chosenPixel / width;
  submitProjectedTexturedQuad(gpu, 32, 0, 48, 16, u, v, page, 4096, 100, clut);
  gpu.snapshotDisplayBuffer(2);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  std::array<Color32, 16> colors;
  colors.fill(color);
  const std::array<uint8_t, 16> masks{};
  auto expected = diagnosticReportForNativeSamples(colors, masks);
  EXPECT_EQ(metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
            metricHash(expected.actual.rgba8));
  EXPECT_EQ(metrics::get("mgs.texture.gpu_submitted_packets"), 1u);
  // A runtime palette edit must be snapshotted, never frozen at its cached
  // color.
  submitCpuUpload(gpu, cx + indices[chosenPixel], cy, 1, 1, {0x7c1f});
  submitProjectedTexturedQuad(gpu, 32, 0, 48, 16, u, v, page, 4096, 200, clut);
  gpu.snapshotDisplayBuffer(3);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  colors.fill({255, 0, 255, 255});
  expected = diagnosticReportForNativeSamples(colors, masks);
  EXPECT_EQ(metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
            metricHash(expected.actual.rgba8));
  EXPECT_EQ(metrics::get("mgs.texture.gpu_submitted_packets"), 2u);
  EXPECT_EQ(metrics::get("mgs.texture.dynamic_palette_packets"), 1u);
  EXPECT_EQ(metrics::get("mgs.texture.dynamic_palette_8bit_packets"),
            requestedDepth);
  renderer.destroy();
  metrics::reset();
  metrics::setEnabledForTesting(false);
}

TEST(GpuModernReplayVulkanDepthTest,
     OptInMgsOriginalPcxMatchesGpuAndTracksRuntimePalette) {
  checkOriginalPcxGpuPalette(0);
}

TEST(GpuModernReplayVulkanDepthTest,
     OptInMgsOriginal8BitPcxKeepsIndicesAndTracksRuntimePalette) {
  checkOriginalPcxGpuPalette(1);
}

TEST(GpuModernReplayVulkanDepthTest,
     OptInUhdRasterHasTwelveSubpixelsAndNativeOracle) {
  if (!std::getenv("PS1_RUN_VULKAN_INTEGRATION"))
    GTEST_SKIP();
  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK", "all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM", "2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "2");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION", "32,0,1,1");
  struct Profile {
    Profile() { games::mgs::configure(true, true); }
    ~Profile() { games::mgs::configure(false, false); }
  } profile;
  metrics::reset();
  metrics::setEnabledForTesting(true);
  GPU gpu;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.internalScale = 12;
  options.presentationWidth = 3840;
  options.presentationHeight = 2160;
  options.preserveAspect = true;
  options.presentationAspectWidth = 16;
  options.presentationAspectHeight = 9;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(renderer.init("MGS UHD raster oracle", 800, 800, false, false));
  gpu.writeGP1(0x03000000);
  gpu.writeGP1(0x05000000);
  gpu.writeGP0(0xe3000000);
  gpu.writeGP0(0xe4000000 | 1023 | (511 << 10));
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  // A quarter-native-pixel offset must become exactly three UHD columns.
  // Upscaling a native framebuffer would instead color all twelve columns.
  gpu.writeGP0(0x2800ff00);
  const std::array<uint32_t, 4> xy = {packedVertex(32, 0), packedVertex(33, 0),
                                      packedVertex(32, 1), packedVertex(33, 1)};
  for (uint32_t k = 0; k < 4; ++k) {
    const auto p =
        projectedVertexAtDepth(xy[k], float(int16_t(xy[k])) + .25f,
                               float(int16_t(xy[k] >> 16)), 4096, 100 + k);
    gpu.writeGP0(xy[k], UINT32_MAX, &p);
  }
  gpu.snapshotDisplayBuffer(2);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  EXPECT_EQ(metrics::getState("host.vulkan.internal_width"), 3840);
  EXPECT_EQ(metrics::getState("host.vulkan.internal_height"), 2160);
  EXPECT_EQ(metrics::getState("host.renderer.vulkan_replay_scale"), 12);
  EXPECT_EQ(metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_nonblack"),
            108);
  EXPECT_GT(metrics::get("host.renderer.vulkan_replay_route.m1_geometry"), 0u);
  EXPECT_TRUE(isVulkanNativeOraclePolicyActive(gpu));
  const auto w = metrics::getState("host.renderer.present.viewport_width");
  const auto h = metrics::getState("host.renderer.present.viewport_height");
  EXPECT_LE(std::abs(w * 9 - h * 16), 16);
  SDL_Window *window = nullptr;
  for (uint32_t id=1; id<4096 && !window; ++id) {
    auto *candidate=SDL_GetWindowFromID(id);
    if (candidate && std::string(SDL_GetWindowTitle(candidate))=="MGS UHD raster oracle") window=candidate;
  }
  ASSERT_NE(window,nullptr);
  SDL_SetWindowSize(window,1000,500);
  bool resized=false;
  for (unsigned attempt=0; attempt<100 && !resized; ++attempt) {
    SDL_PumpEvents();
    int ww=0,wh=0; SDL_GetWindowSize(window,&ww,&wh);
    resized=ww==1000 && wh==500;
    if (!resized) SDL_Delay(10);
  }
  ASSERT_TRUE(resized);
  renderer.renderFrame(); renderer.drainReplayDiagnostics();
  int dw=0,dh=0; SDL_Vulkan_GetDrawableSize(window,&dw,&dh);
  EXPECT_EQ(metrics::getState("host.renderer.present.window_width"),dw);
  EXPECT_EQ(metrics::getState("host.renderer.present.window_height"),dh);
  EXPECT_EQ(metrics::getState("host.vulkan.internal_width"),3840);
  EXPECT_EQ(metrics::getState("host.vulkan.internal_height"),2160);
  EXPECT_LE(std::abs(metrics::getState("host.renderer.present.viewport_width")*9-
                     metrics::getState("host.renderer.present.viewport_height")*16),16);
  renderer.destroy();
  metrics::reset();
  metrics::setEnabledForTesting(false);
}

TEST(MgsSurfaceEffectsGpuTest, OptInGpuContinuousGlowAccumulatesAndLeavesNativeVramUnchanged) {
  if (!std::getenv("PS1_RUN_VULKAN_INTEGRATION")) GTEST_SKIP();
  const auto directory=std::filesystem::temp_directory_path()/
      ("mgs-surface-test-"+std::to_string(SDL_GetPerformanceCounter()));
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path directory;
    ~Cleanup(){games::mgs::configure(false,false);games::mgs::captureSurfaceEffects({});
      metrics::reset();metrics::setEnabledForTesting(false);std::filesystem::remove_all(directory);}
  } cleanup{directory};
  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK","all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM","2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO","3");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION","32,32,64,64");
  ScopedEnvironment dump("PS1_VULKAN_REPLAY_READBACK_DUMP_DIRECTORY",directory.c_str());
  games::mgs::configure(true,false);metrics::reset();metrics::setEnabledForTesting(true);
  GPU gpu,native;configureVulkanNativeOracle(gpu,true);
  RendererOptions options;options.internalScale=4;options.pgxp=true;
  options.trueDepth=true;options.trueDepthMode="hybrid";options.trueColor=true;
  RendererVulkan renderer(gpu,options);
  ASSERT_TRUE(renderer.init("MGS continuous local effects",320,224,false,false));
  gpu.writeGP1(0x03000000);gpu.writeGP1(0x05000000);
  gpu.snapshotDisplayBuffer(1);renderer.renderFrame();renderer.drainReplayDiagnostics();
  std::vector<uint8_t> ram(2*1024*1024);
  const auto word=[&](uint32_t p,uint32_t v){for(unsigned i=0;i<4;++i)ram[(p&0x1fffff)+i]=v>>(i*8);};
  const auto half=[&](uint32_t p,uint16_t v){ram[p&0x1fffff]=v;ram[(p&0x1fffff)+1]=v>>8;};
  constexpr uint32_t actor=0x80100000,prim=0x80110000,packs=0x80120000,
                     head=0x800af390+5*68;
  word(head+4,actor);word(actor,head);word(actor+4,head+32);word(head+32,actor);
  word(actor+12,0x80070000);word(actor+20,0x80140000);word(actor+32,prim);
  std::memcpy(ram.data()+0x140000,"emitter.c",10);
  word(prim+36,0x412);half(prim+42,2);half(prim+48,40);half(prim+50,4);
  half(prim+52,8);half(prim+54,8);word(prim+56,0x80118000);word(prim+64,packs);
  for(unsigned i=0;i<2;++i){word(packs+40*i+4,0x2e808080);half(packs+40*i+22,0x120);}
  games::mgs::captureSurfaceEffects(ram);
  const auto original=ram;
  const auto send=[&](uint32_t v,uint32_t p=UINT32_MAX){gpu.writeGP0(v,p);native.writeGP0(v,p);};
  send(0xe3000000);send(0xe4000000|319|(223<<10));
  std::vector<uint8_t> previous;
  for(unsigned frame=2;frame<=3;++frame) {
    send(0x02101010);send(0);send(320|(224<<16));
    for(unsigned i=0;i<frame-1;++i) {
      send(0x2e808080,packs+i*40+4);
      send(packedVertex(32,32));send(0);
      send(packedVertex(96,32));send(63|(0x120<<16));
      send(packedVertex(32,96));send(63<<8);
      send(packedVertex(96,96));send(63|(63<<8));
    }
    gpu.snapshotDisplayBuffer(frame);renderer.renderFrame();renderer.drainReplayDiagnostics();
    std::ifstream ppm(directory/("frame-"+std::to_string(frame)+".ppm"),std::ios::binary);
    std::string magic;unsigned width=0,height=0,max=0;
    ppm>>magic>>width>>height>>max;ppm.get();
    ASSERT_EQ(magic,"P6");ASSERT_EQ(width,256u);ASSERT_EQ(height,256u);
    std::vector<uint8_t> pixels(width*height*3);ppm.read(reinterpret_cast<char*>(pixels.data()),pixels.size());
    ASSERT_TRUE(ppm);
    const size_t center=(128*width+128)*3;
    EXPECT_GT(pixels[center],150u);
    EXPECT_LT(pixels[0],20u);
    std::set<uint8_t> levels;
    for(unsigned x=0;x<128;++x) levels.insert(pixels[(128*width+x)*3]);
    EXPECT_GT(levels.size(),32u);
    if(!previous.empty()) EXPECT_GT(pixels[center],previous[center]);
    previous=std::move(pixels);
    for(unsigned i=0;i<GPU::VRAM_WIDTH*GPU::VRAM_HEIGHT;++i)
      ASSERT_EQ(gpu.getVRAM()[i].raw,native.getVRAM()[i].raw)<<i;
  }
  EXPECT_EQ(ram,original);
  EXPECT_EQ(metrics::get("mgs.effects.surface.bound_packets"),3u);
  EXPECT_EQ(metrics::get("host.renderer.mgs_screen.depth_snapshot"),2u);
  EXPECT_EQ(metrics::get("host.renderer.mgs_screen.snapshot_reused"),1u);
  EXPECT_EQ(metrics::get("host.renderer.vulkan_replay_executor_downgrade"),0u);
  renderer.destroy();
}

TEST(MgsSurfaceEffectsGpuTest, OptInBubbleDepthOffsetKeepsAuthoredPlumeVisible) {
  if (!std::getenv("PS1_RUN_VULKAN_INTEGRATION")) GTEST_SKIP();
  const auto directory=std::filesystem::temp_directory_path()/
      ("mgs-bubble-test-"+std::to_string(SDL_GetPerformanceCounter()));
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path directory;
    ~Cleanup(){games::mgs::configure(false,false);games::mgs::captureSurfaceEffects({});
      metrics::reset();metrics::setEnabledForTesting(false);std::filesystem::remove_all(directory);}
  } cleanup{directory};
  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK","all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM","2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO","4");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION","32,32,64,64");
  ScopedEnvironment dump("PS1_VULKAN_REPLAY_READBACK_DUMP_DIRECTORY",directory.c_str());
  games::mgs::configure(true,false);metrics::reset();metrics::setEnabledForTesting(true);
  GPU gpu,native;configureVulkanNativeOracle(gpu,true);
  RendererOptions options;options.internalScale=4;options.pgxp=true;
  options.trueDepth=true;options.trueDepthMode="hybrid";options.trueColor=true;
  RendererVulkan renderer(gpu,options);
  ASSERT_TRUE(renderer.init("MGS continuous local effects",320,224,false,false));
  gpu.writeGP1(0x03000000);gpu.writeGP1(0x05000000);
  gpu.snapshotDisplayBuffer(1);renderer.renderFrame();renderer.drainReplayDiagnostics();
  std::vector<uint8_t> ram(2*1024*1024);
  const auto word=[&](uint32_t p,uint32_t v){for(unsigned i=0;i<4;++i)ram[(p&0x1fffff)+i]=v>>(i*8);};
  const auto half=[&](uint32_t p,uint16_t v){ram[p&0x1fffff]=v;ram[(p&0x1fffff)+1]=v>>8;};
  constexpr uint32_t actor=0x80100000,prim=0x80110000,packs=0x80120000,
                     head=0x800af390+5*68;
  word(head+4,actor);word(actor,head);word(actor+4,head+32);word(head+32,actor);
  word(actor+12,0x80070000);word(actor+20,0x80140000);word(actor+40,prim);
  std::memcpy(ram.data()+0x140000,"bubble_p.c",11);
  word(prim+36,0x412);half(prim+42,2);half(prim+48,40);half(prim+50,4);
  half(prim+52,8);half(prim+54,8);word(prim+56,0x80118000);word(prim+64,packs);
  for(unsigned i=0;i<2;++i){word(packs+40*i+4,0x2e808080);half(packs+40*i+22,0x160);}
  games::mgs::captureSurfaceEffects(ram);
  const auto original=ram;
  const auto send=[&](uint32_t v,uint32_t p=UINT32_MAX){gpu.writeGP0(v,p);native.writeGP0(v,p);};
  send(0xe3000000);send(0xe4000000|319|(223<<10));
  unsigned hiddenPeak=0;
  for(unsigned frame=2;frame<=4;++frame) {
    // A real depth-bearing wall lies in front of the bubble's geometric
    // center. Only the actor's authored raise makes the plume foreground.
    half(prim+46,frame==3 ? 3000 : 0);
    games::mgs::captureSurfaceEffects(ram);
    send(0x02101010);send(0);send(320|(224<<16));
    const auto vertex=[&](unsigned i,double depth) {
      const int x=32+(i&1)*64,y=32+(i>>1)*64;
      const auto xy=packedVertex(x,y);
      auto value=projectedVertexAtDepth(xy,x,y,depth,i+1);
      gpu.writeGP0(xy,UINT32_MAX,&value);native.writeGP0(xy);
    };
    send(0x28101010);
    for(unsigned i=0;i<4;++i) vertex(i,16000);
    send(0x2e808080,packs+4);
    for(unsigned i=0;i<4;++i) {
      vertex(i,16384);
      send((i&1 ? 63u : 0u)|(i>>1 ? 63u<<8 : 0u)|(i==1 ? 0x160u<<16 : 0u));
    }
    gpu.snapshotDisplayBuffer(frame);renderer.renderFrame();renderer.drainReplayDiagnostics();
    std::ifstream ppm(directory/("frame-"+std::to_string(frame)+".ppm"),std::ios::binary);
    std::string magic;unsigned width=0,height=0,max=0;
    ppm>>magic>>width>>height>>max;ppm.get();
    ASSERT_EQ(magic,"P6");ASSERT_EQ(width,256u);ASSERT_EQ(height,256u);
    std::vector<uint8_t> pixels(width*height*3);ppm.read(reinterpret_cast<char*>(pixels.data()),pixels.size());
    ASSERT_TRUE(ppm);
    unsigned peak=0;
    for(unsigned i=2;i<pixels.size();i+=3) peak=std::max(peak,unsigned(pixels[i]));
    if(frame==2) {hiddenPeak=peak;EXPECT_LE(peak,17u);}
    else if(frame==3) {
      EXPECT_GT(peak,60u); // authored quarter-additive energy with bright rims
      EXPECT_LT(peak,100u); // a single bubble must not become an opaque bead
      EXPECT_GT(pixels[(128*width+128)*3+2],35u); // luminous body above background
      EXPECT_LT(pixels[(128*width+128)*3+2],peak); // brighter shell highlights
      EXPECT_LE(pixels[2],17u); // no opaque quad/background overwrite
    }
    else EXPECT_EQ(peak,hiddenPeak); // reset/next actor cannot retain the offset
    for(unsigned i=0;i<GPU::VRAM_WIDTH*GPU::VRAM_HEIGHT;++i)
      ASSERT_EQ(gpu.getVRAM()[i].raw,native.getVRAM()[i].raw)<<i;
  }
  EXPECT_EQ(ram,original);
  EXPECT_EQ(metrics::get("mgs.effects.surface.bound_packets"),3u);
  EXPECT_EQ(metrics::get("host.renderer.mgs_screen.depth_snapshot"),3u);
  EXPECT_EQ(metrics::get("host.renderer.mgs_screen.snapshot_reused"),0u);
  EXPECT_EQ(metrics::get("host.renderer.vulkan_replay_executor_downgrade"),0u);
  renderer.destroy();
}

TEST(MgsSurfaceEffectsGpuTest, OptInExplosionVolumeOccludesAgainstGeometryWithoutOrderingBypass) {
  if (!std::getenv("PS1_RUN_VULKAN_INTEGRATION")) GTEST_SKIP();
  const auto directory=std::filesystem::temp_directory_path()/
      ("mgs-explosion-test-"+std::to_string(SDL_GetPerformanceCounter()));
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path directory;
    ~Cleanup(){games::mgs::configure(false,false);games::mgs::captureSurfaceEffects({});
      metrics::reset();metrics::setEnabledForTesting(false);std::filesystem::remove_all(directory);}
  } cleanup{directory};
  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK","all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM","2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO","8");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION","32,32,64,64");
  ScopedEnvironment dump("PS1_VULKAN_REPLAY_READBACK_DUMP_DIRECTORY",directory.c_str());
  games::mgs::configure(true,false);metrics::reset();metrics::setEnabledForTesting(true);
  GPU gpu,native;configureVulkanNativeOracle(gpu,true);
  RendererOptions options;options.internalScale=4;options.pgxp=true;
  options.trueDepth=true;options.trueDepthMode="hybrid";options.trueColor=true;
  RendererVulkan renderer(gpu,options);
  ASSERT_TRUE(renderer.init("MGS continuous local effects",320,224,false,false));
  gpu.writeGP1(0x03000000);gpu.writeGP1(0x05000000);
  gpu.snapshotDisplayBuffer(1);renderer.renderFrame();renderer.drainReplayDiagnostics();
  std::vector<uint8_t> ram(2*1024*1024);
  const auto word=[&](uint32_t p,uint32_t v){for(unsigned i=0;i<4;++i)ram[(p&0x1fffff)+i]=v>>(i*8);};
  const auto half=[&](uint32_t p,uint16_t v){ram[p&0x1fffff]=v;ram[(p&0x1fffff)+1]=v>>8;};
  constexpr uint32_t actor=0x80100000,prim=0x80110000,packs=0x80120000,
                     head=0x800af390+5*68;
  word(head+4,actor);word(actor,head);word(actor+4,head+32);word(head+32,actor);
  word(actor+12,0x80070000);word(actor+20,0x80140000);word(actor+36,prim);
  std::memcpy(ram.data()+0x140000,"blastoff.c",11);
  word(prim+36,0x412);half(prim+42,2);half(prim+48,40);half(prim+50,4);
  half(prim+52,8);half(prim+54,8);word(prim+56,0x80118000);word(prim+64,packs);
  for(unsigned i=0;i<2;++i){word(packs+40*i+4,0x2e808080);half(packs+40*i+22,0x120);}
  word(prim+60,0x80118020);
  half(0x80118020,1700);half(0x80118022,1700);
  half(0x80118024,3400);half(0x80118026,3400);
  half(prim+46,2000);
  games::mgs::captureSurfaceEffects(ram);
  const auto original=ram;
  const auto send=[&](uint32_t v,uint32_t p=UINT32_MAX){gpu.writeGP0(v,p);native.writeGP0(v,p);};
  send(0xe3000000);send(0xe4000000|319|(223<<10));
  unsigned partialCenter=0,fullCenter=0;
  for(unsigned frame=2;frame<=8;++frame) {
    // Move the same real surface from in front of the whole blast, through
    // its center, and behind it. A large OT offset must never punch through
    // the foreground panel; a surface inside the blast reveals only part.
    half(prim+46,frame==6 ? 0 : 2000);
    half(0x80118024,frame>=7 ? 0 : 3400);
    const double wallDepth=frame==2 || frame==6 ? 14000 :
                           frame==3 ? 16384 : frame==4 || frame==7 ? 19000 : 15500;
    games::mgs::captureSurfaceEffects(ram);
    send(0x02101010);send(0);send(320|(224<<16));
    const auto vertex=[&](unsigned i,double depth) {
      const int x=32+(i&1)*64,y=32+(i>>1)*64;
      const auto xy=packedVertex(x,y);
      auto value=projectedVertexAtDepth(xy,x,y,depth,i+1);
      gpu.writeGP0(xy,UINT32_MAX,&value);native.writeGP0(xy);
    };
    send(0x28101010);
    for(unsigned i=0;i<4;++i) vertex(i,wallDepth);
    send(0x2e808080,packs+4);
    for(unsigned i=0;i<4;++i) {
      vertex(i,16384);
      send((i&1 ? 63u : 0u)|(i>>1 ? 63u<<8 : 0u)|(i==1 ? 0x120u<<16 : 0u));
    }
    gpu.snapshotDisplayBuffer(frame);renderer.renderFrame();renderer.drainReplayDiagnostics();
    std::ifstream ppm(directory/("frame-"+std::to_string(frame)+".ppm"),std::ios::binary);
    std::string magic;unsigned width=0,height=0,max=0;
    ppm>>magic>>width>>height>>max;ppm.get();
    ASSERT_EQ(magic,"P6");ASSERT_EQ(width,256u);ASSERT_EQ(height,256u);
    std::vector<uint8_t> pixels(width*height*3);ppm.read(reinterpret_cast<char*>(pixels.data()),pixels.size());
    ASSERT_TRUE(ppm);
    unsigned peak=0;
    for(unsigned i=0;i<pixels.size();i+=3) peak=std::max(peak,unsigned(pixels[i]));
    const unsigned center=pixels[(128*width+128)*3];
    if(frame==2 || frame==6 || frame==8) EXPECT_LE(peak,17u);
    else if(frame==3) {partialCenter=center;EXPECT_GT(center,40u);}
    else if(frame==4) {
      fullCenter=center;EXPECT_GT(center,150u);
      EXPECT_LT(partialCenter,fullCenter-30u);
      EXPECT_LE(pixels[2],17u); // transparent artwork corners preserve scene
    } else if(frame==5) {
      EXPECT_GT(center,17u);EXPECT_LT(center,partialCenter-10u);
    } else if(frame==7) EXPECT_EQ(center,fullCenter);
    for(unsigned i=0;i<GPU::VRAM_WIDTH*GPU::VRAM_HEIGHT;++i)
      ASSERT_EQ(gpu.getVRAM()[i].raw,native.getVRAM()[i].raw)<<i;
  }
  // Only fixture-owned size/raise values were changed between submissions.
  half(prim+46,2000);half(0x80118024,3400);
  EXPECT_EQ(ram,original);
  EXPECT_EQ(metrics::get("mgs.effects.surface.bound_packets"),7u);
  EXPECT_EQ(metrics::get("host.renderer.mgs_screen.depth_snapshot"),7u);
  EXPECT_EQ(metrics::get("host.renderer.mgs_screen.snapshot_reused"),0u);
  EXPECT_EQ(metrics::get("host.renderer.vulkan_replay_executor_downgrade"),0u);
  renderer.destroy();
}

TEST(MgsSurfaceEffectsGpuTest, OptInPhotoCapturesModernScanoutWithoutDiagnosticMode) {
  if(!std::getenv("PS1_RUN_VULKAN_INTEGRATION")) GTEST_SKIP();
  const auto directory=std::filesystem::temp_directory_path()/
      ("mgs-photo-test-"+std::to_string(SDL_GetPerformanceCounter()));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup(){games::mgs::configure(false,false);games::mgs::captureSurfaceEffects({});
      metrics::reset();metrics::setEnabledForTesting(false);std::filesystem::remove_all(path);}
  } cleanup{directory};
  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK","off");
  ScopedEnvironment photoDirectory("PS1_MGS_PHOTO_DIRECTORY",directory.c_str());
  games::mgs::configure(true,false);metrics::reset();metrics::setEnabledForTesting(true);
  GPU gpu;configureVulkanNativeOracle(gpu,true);
  RendererOptions options;options.internalScale=4;options.pgxp=true;
  options.trueColor=true;options.trueDepth=true;options.trueDepthMode="hybrid";
  RendererVulkan renderer(gpu,options);
  ASSERT_TRUE(renderer.init("MGS photograph",320,224,false,false));
  gpu.writeGP1(0x03000000);gpu.writeGP1(0x05000000);
  gpu.snapshotDisplayBuffer(1);renderer.renderFrame();renderer.drainReplayDiagnostics();
  std::vector<uint8_t> ram(2*1024*1024);
  const auto word=[&](uint32_t p,uint32_t v){for(unsigned i=0;i<4;++i)ram[(p&0x1fffff)+i]=v>>(i*8);};
  constexpr uint32_t actor=0x80100000,head=0x800af390+6*68;
  word(head+4,actor);word(actor,head);word(actor+4,head+32);word(head+32,actor);
  word(actor+8,0x80070000);word(actor+20,0x80140000);
  std::memcpy(ram.data()+0x140000,"jpegcam.c",10);
  word(actor+32,0x80150000);word(actor+36,0x80160000);
  word(actor+0x64,3);word(actor+0x70,1);word(0x800adb3c,0x400);
  games::mgs::captureSurfaceEffects(ram);
  word(actor+0x64,4);games::mgs::captureSurfaceEffects(ram);
  gpu.writeGP0(0xe3000000);gpu.writeGP0(0xe4000000|319|(223<<10));
  submitFill(gpu,13,37,91,0,0,320,224);
  gpu.snapshotDisplayBuffer(2);
  // The producer advances twice before presentation, including a different
  // image. The export must still contain the exact shutter frame.
  submitFill(gpu,217,19,27,0,0,320,224);gpu.snapshotDisplayBuffer(3);
  gpu.snapshotDisplayBuffer(4);
  renderer.renderFrame();renderer.drainReplayDiagnostics();
  EXPECT_EQ(metrics::get("mgs.effects.photo.saved"),1u);
  ASSERT_TRUE(std::filesystem::is_directory(directory));
  size_t files=0;
  for(const auto &entry:std::filesystem::directory_iterator(directory)) {
    ++files;std::ifstream image(entry.path(),std::ios::binary);
    std::string magic;unsigned width,height,max;image>>magic>>width>>height>>max;
    EXPECT_EQ(magic,"P6");EXPECT_GT(width,320u);EXPECT_GT(height,224u);EXPECT_EQ(max,255u);
    image.get();std::vector<uint8_t> pixels(width*height*3);image.read(reinterpret_cast<char*>(pixels.data()),pixels.size());
    ASSERT_TRUE(image);const size_t center=(height/2*width+width/2)*3;
    EXPECT_EQ(pixels[center],13);EXPECT_EQ(pixels[center+1],37);EXPECT_EQ(pixels[center+2],91);
  }
  EXPECT_EQ(files,1u);
  renderer.renderFrame();renderer.drainReplayDiagnostics();
  EXPECT_EQ(metrics::get("mgs.effects.photo.saved"),1u);
  EXPECT_EQ(metrics::get("host.renderer.vulkan_replay_executor_downgrade"),0u);
  renderer.destroy();
}

TEST(MgsScreenEffectsTest, OptInGpuClippedScreenCopyPreservesSubpixelScene) {
  if (!std::getenv("PS1_RUN_VULKAN_INTEGRATION"))
    GTEST_SKIP();
  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK", "all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM", "2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "2");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION", "0,32,2,1");
  struct Profile {
    Profile() { games::mgs::configure(true, false); }
    ~Profile() { games::mgs::configure(false, false); }
  } profile;
  metrics::reset();
  metrics::setEnabledForTesting(true);
  GPU gpu;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.internalScale = 4;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueDepthMode = "hybrid";
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(
      renderer.init("MGS screen subpixel test", 320, 224, false, false));
  gpu.writeGP1(0x03000000);
  gpu.writeGP1(0x05000000);
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  gpu.writeGP0(0xe3000000);
  gpu.writeGP0(0xe4000000 | 319 | (223 << 10));
  constexpr Color32 blue{13, 37, 91, 255}, red{217, 19, 27, 255};
  submitFill(gpu, blue.r, blue.g, blue.b, 0, 0, 320, 224);
  // A one-native-pixel-wide quad starts a quarter-pixel after the native edge.
  // Its four high-resolution samples cannot be reconstructed from RGB555 VRAM.
  constexpr std::array<uint32_t, 4> packed{
      packedVertex(32, 32), packedVertex(33, 32), packedVertex(32, 33),
      packedVertex(33, 33)};
  gpu.writeGP0(0x28000000 | red.r | (red.g << 8) | (red.b << 16));
  for (uint32_t i = 0; i < 4; ++i) {
    const auto value = projectedVertex(packed[i], 32.25f + float(i & 1),
                                       32.f + float(i >> 1), i + 1);
    gpu.writeGP0(packed[i], UINT32_MAX, &value);
  }
  gpu.writeGP0(0xe1000100); // Direct RGB555 framebuffer addressing.
  gpu.writeGP0(0x65808080);
  gpu.writeGP0(packedVertex(-2, 32));
  gpu.writeGP0(30 | (32 << 8));
  gpu.writeGP0(4 | (1 << 16));
  gpu.snapshotDisplayBuffer(2);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  std::array<Color32, 32> colors;
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 8; ++x)
      colors[y * 8 + x] = x >= 1 && x < 5 ? red : blue;
  std::array<uint8_t, 32> mask{};
  std::array<float, 32> depth;
  depth.fill(1);
  const VulkanReplayDiagnosticPlanes planes{
      {colors.data(), sizeof(colors), 8 * sizeof(Color32)},
      {mask.data(), mask.size(), 8},
      {depth.data(), sizeof(depth), 8 * sizeof(float)}};
  const auto expected = captureVulkanReplayReadback({0, 32, 2, 1, 4}, planes);
  EXPECT_EQ(metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
            metricHash(expected.actual.rgba8));
  EXPECT_EQ(metrics::get("host.renderer.mgs_screen.draw"), 1u);
  EXPECT_EQ(metrics::get("host.renderer.mgs_screen.snapshot"), 1u);
  EXPECT_EQ(metrics::get("host.renderer.vulkan_replay_executor_downgrade"), 0u);
  EXPECT_EQ(gpu.internalResolutionScale(), 1u);
  renderer.destroy();
  metrics::reset();
  metrics::setEnabledForTesting(false);
}

TEST(MgsScreenEffectsTest, OptInGpuScreenTexturePreservesSubpixelSceneAndRgb8) {
  if (!std::getenv("PS1_RUN_VULKAN_INTEGRATION"))
    GTEST_SKIP();
  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK", "all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM", "2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "3");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION", "160,32,2,1");
  struct Profile {
    Profile() { games::mgs::configure(true, false); }
    ~Profile() { games::mgs::configure(false, false); }
  } profile;
  metrics::reset();
  metrics::setEnabledForTesting(true);
  GPU gpu;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.internalScale = 4;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueDepthMode = "hybrid";
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(
      renderer.init("MGS screen subpixel test", 320, 224, false, false));
  gpu.writeGP1(0x03000000);
  gpu.writeGP1(0x05000000);
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  gpu.writeGP0(0xe3000000);
  gpu.writeGP0(0xe4000000 | 319 | (223 << 10));
  constexpr Color32 blue{13, 37, 91, 255}, red{217, 19, 27, 255};
  submitFill(gpu, blue.r, blue.g, blue.b, 0, 0, 320, 224);
  // A one-native-pixel-wide quad starts a quarter-pixel after the native edge.
  // Its four high-resolution samples cannot be reconstructed from RGB555 VRAM.
  constexpr std::array<uint32_t, 4> packed{
      packedVertex(32, 32), packedVertex(33, 32), packedVertex(32, 33),
      packedVertex(33, 33)};
  gpu.writeGP0(0x28000000 | red.r | (red.g << 8) | (red.b << 16));
  for (uint32_t i = 0; i < 4; ++i) {
    const auto value = projectedVertex(packed[i], 32.25f + float(i & 1),
                                       32.f + float(i >> 1), i + 1);
    gpu.writeGP0(packed[i], UINT32_MAX, &value);
  }
  gpu.writeGP0(0xe1000100); // Direct RGB555 framebuffer addressing.
  gpu.writeGP0(0x65808080);
  gpu.writeGP0(packedVertex(160, 32));
  gpu.writeGP0(32 | (32 << 8));
  gpu.writeGP0(2 | (1 << 16));
  gpu.snapshotDisplayBuffer(2);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  std::array<Color32, 32> colors;
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 8; ++x)
      colors[y * 8 + x] = x >= 1 && x < 5 ? red : blue;
  std::array<uint8_t, 32> mask{};
  std::array<float, 32> depth;
  depth.fill(1);
  const VulkanReplayDiagnosticPlanes planes{
      {colors.data(), sizeof(colors), 8 * sizeof(Color32)},
      {mask.data(), mask.size(), 8},
      {depth.data(), sizeof(depth), 8 * sizeof(float)}};
  const auto expected = captureVulkanReplayReadback({160, 32, 2, 1, 4}, planes);
  EXPECT_EQ(metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
            metricHash(expected.actual.rgba8));
  EXPECT_EQ(metrics::get("host.renderer.mgs_screen.draw"), 1u);
  EXPECT_EQ(metrics::get("host.renderer.mgs_screen.snapshot"), 1u);
  EXPECT_EQ(metrics::get("host.renderer.vulkan_replay_executor_downgrade"), 0u);
  EXPECT_EQ(gpu.internalResolutionScale(), 1u);
  // Mirrored U=0 must continue with texel 255, rather than repeating texel 0
  // because a negative fractional coordinate was truncated toward zero.
  submitCpuUpload(gpu, 0, 32, 1, 1, {0x001f});
  submitCpuUpload(gpu, 255, 32, 1, 1, {0x03e0});
  gpu.writeGP0(0xe1001100);
  gpu.writeGP0(0x65808080);
  gpu.writeGP0(packedVertex(160, 32));
  gpu.writeGP0(32u << 8);
  gpu.writeGP0(2 | (1 << 16));
  gpu.snapshotDisplayBuffer(3);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 8; ++x)
      colors[y * 8 + x] =
          x < 4 ? Color32{255, 0, 0, 255} : Color32{0, 255, 0, 255};
  const auto flipped = captureVulkanReplayReadback({160, 32, 2, 1, 4}, planes);
  EXPECT_EQ(metrics::getState(
                "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
            metricHash(flipped.actual.rgba8));
  EXPECT_EQ(metrics::get("host.renderer.vulkan_replay_executor_downgrade"), 0u);
  renderer.destroy();
  metrics::reset();
  metrics::setEnabledForTesting(false);
}

TEST(MgsScreenEffectsTest, OptInUnderwaterFogUsesSceneDepthAndExcludesFurnace) {
  if (!std::getenv("PS1_RUN_VULKAN_INTEGRATION")) GTEST_SKIP();
  const auto directory=std::filesystem::temp_directory_path()/
      ("mgs-water-depth-test-"+std::to_string(SDL_GetPerformanceCounter()));
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path directory;
    ~Cleanup(){games::mgs::configure(false,false);games::mgs::captureScreenEffects({});
      metrics::reset();metrics::setEnabledForTesting(false);std::filesystem::remove_all(directory);}
  } cleanup{directory};
  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK","all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM","2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO","7");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION","32,32,16,16");
  ScopedEnvironment dump("PS1_VULKAN_REPLAY_READBACK_DUMP_DIRECTORY",directory.c_str());
  games::mgs::configure(true,false);metrics::reset();metrics::setEnabledForTesting(true);
  GPU gpu,native;configureVulkanNativeOracle(gpu,true);
  RendererOptions options;options.internalScale=4;options.pgxp=true;
  options.trueDepth=true;options.trueDepthMode="hybrid";options.trueColor=true;
  RendererVulkan renderer(gpu,options);
  ASSERT_TRUE(renderer.init("MGS underwater depth",320,224,false,false));
  gpu.writeGP1(0x03000000);gpu.writeGP1(0x05000000);
  gpu.snapshotDisplayBuffer(1);renderer.renderFrame();renderer.drainReplayDiagnostics();
  std::vector<uint8_t> ram(2*1024*1024);
  const auto word=[&](uint32_t p,uint32_t v){for(unsigned i=0;i<4;++i)ram[(p&0x1fffff)+i]=v>>(i*8);};
  constexpr uint32_t actor=0x80100000,prims=0x80110000,head=0x800af45c;
  word(head+4,actor);word(actor,head);word(actor+20,0x80120000);
  std::memcpy(ram.data()+0x120000,"wt_view.c",10);
  word(actor+56,prims);word(actor+64,1);word(actor+72,43);
  constexpr uint32_t blur=0x80150000,blurHead=0x800af390+7*68,blurPrims=0x80160000;
  word(blurHead+4,blur);word(blur,blurHead);word(blur+4,blurHead+32);word(blurHead+32,blur);
  word(blur+8,0x80070000);word(blur+20,0x80170000);word(blur+36,blurPrims);
  std::memcpy(ram.data()+0x170000,"blur.c",7);
  word(blur+84,2);word(blur+104,24);word(blur+108,40);
  for(unsigned k=0;k<4;++k) word(blurPrims+k*40+4,0x2e808080);
  const auto send=[&](uint32_t v,uint32_t p=UINT32_MAX){gpu.writeGP0(v,p);native.writeGP0(v,p);};
  std::array<std::array<uint8_t,3>,6> samples{};
  for(unsigned frame=2;frame<=7;++frame) {
    if(frame==5) {
      // Furnace shares wt_view; prove the owner tuple to select hot air.
      constexpr uint32_t furnace=0x80130000,furnaceHead=0x800af390+5*68;
      word(furnaceHead+4,furnace);word(furnace, furnaceHead);
      word(furnace+4,furnaceHead+32);word(furnaceHead+32,furnace);
      word(furnace+8,0x80070000);word(furnace+20,0x80140000);
      std::memcpy(ram.data()+0x140000,"furnace.c",10);
    }
    if(frame==6) word(0x800af390+5*68+4,0); // remove furnace owner
    const auto original=ram;
    games::mgs::captureScreenEffects(ram);
    send(0xe3000000);send(0xe4000000|319|(223<<10));
    send(0x02000000);send(0);send(320|(224<<16));
    send(0x28a0a0a0);
    for(unsigned i=0;i<4;++i) {
      const int x=16+(i&1)*64,y=16+(i>>1)*64;
      const auto xy=packedVertex(x,y);
      auto value=projectedVertexAtDepth(xy,x,y,frame==2 || frame==6 ? 2000 : 32000,i+1);
      // Unpositioned overlay in frame 4 must not inherit frame 3's fog.
      gpu.writeGP0(xy,UINT32_MAX,frame==4 ? nullptr : &value);native.writeGP0(xy);
    }
    send(0xe1000100);
    if(frame>=6) {
      // The intro composes blur.c before wt_view. Its fullscreen draw must
      // preserve scene depth even though its operational depth is invalid.
      for(unsigned k=0;k<4;++k) {
        send(0x2e808080,blurPrims+k*40+4);
        for(unsigned i=0;i<4;++i) {
          send(packedVertex(int(k&1)*160+int(i&1)*160,int(k>>1)*112+int(i>>1)*112));
          send((i&1 ? 159u : 0u)|(i>>1 ? 111u<<8 : 0u)|(i==1 ? 0x100u<<16 : 0u));
        }
      }
    }
    send(0x64808080,prims+60);send(packedVertex(0,6));send(6<<8);send(320|(1<<16));
    gpu.snapshotDisplayBuffer(frame);renderer.renderFrame();renderer.drainReplayDiagnostics();
    std::ifstream ppm(directory/("frame-"+std::to_string(frame)+".ppm"),std::ios::binary);
    std::string magic;unsigned width=0,height=0,max=0;
    ppm>>magic>>width>>height>>max;ppm.get();
    ASSERT_EQ(magic,"P6");ASSERT_EQ(width,64u);ASSERT_EQ(height,64u);
    std::vector<uint8_t> pixels(width*height*3);ppm.read(reinterpret_cast<char*>(pixels.data()),pixels.size());
    ASSERT_TRUE(ppm);
    std::copy_n(pixels.begin()+(32*width+32)*3,3,samples[frame-2].begin());
    EXPECT_EQ(ram,original);
    for(unsigned i=0;i<GPU::VRAM_WIDTH*GPU::VRAM_HEIGHT;++i)
      ASSERT_EQ(gpu.getVRAM()[i].raw,native.getVRAM()[i].raw)<<i;
  }
  EXPECT_GT(samples[0][0],100u);
  for(unsigned channel=0;channel<3;++channel) {
    EXPECT_LT(samples[1][channel],samples[0][channel]*0.55); // distant surface fades
    EXPECT_EQ(samples[2][channel],samples[0][channel]); // no invented overlay depth
    EXPECT_NEAR(samples[3][channel],160,1); // hot air has neither absorption nor fog
    EXPECT_GT(samples[4][channel],35u);
    EXPECT_LT(samples[5][channel],samples[4][channel]*0.55); // fog after temporal zoom
  }
  EXPECT_GT(samples[1][2],samples[1][0]); // red attenuates before blue
  EXPECT_EQ(metrics::get("mgs.screen_effect.pass.zoom_blur"),2u);
  EXPECT_EQ(metrics::get("host.renderer.mgs_screen.depth_snapshot"),7u);
  EXPECT_EQ(metrics::get("host.renderer.vulkan_replay_executor_downgrade"),0u);
  renderer.destroy();
}

TEST(MgsScreenEffectsTest, OptInGpuVisorRetainsSubpixelEdgesAndNativeVram) {
  if (!std::getenv("PS1_RUN_VULKAN_INTEGRATION"))
    GTEST_SKIP();
  // Optional, reproducible GPU benchmark of this same authenticated ScnMask
  // fixture. Use the shader-directory override for A/B runs; no readback or
  // timing assertions in benchmark mode. Normal invocations verify pixels.
  const char *benchmark = std::getenv("PS1_MGS_CRT_BENCHMARK_JSON");
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("mgs-visor-test-" + std::to_string(SDL_GetPerformanceCounter()));
  struct Cleanup {
    std::filesystem::path directory;
    ~Cleanup() {
      std::filesystem::remove_all(directory);
      games::mgs::configure(false, false);
      games::mgs::captureScreenEffects({});
      metrics::reset();
      metrics::setEnabledForTesting(false);
    }
  } cleanup{directory};
  ScopedEnvironment dump("PS1_VULKAN_REPLAY_READBACK_DUMP_DIRECTORY",
                         directory.c_str());
  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK", benchmark ? "off" : "all");
  ScopedEnvironment timings("PS1_VULKAN_GPU_TIMINGS", benchmark ? "1" : "0");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM", "2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "21");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION", "32,32,16,8");
  games::mgs::configure(true, false);
  metrics::reset();
  metrics::setEnabledForTesting(true);
  GPU gpu, native;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.internalScale = 12;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueDepthMode = "hybrid";
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(renderer.init("MGS visor subpixel test", benchmark ? 3840 : 320,
                            benchmark ? 2160 : 224, false, false));
  gpu.writeGP1(0x03000000);
  gpu.writeGP1(0x05000000);
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  std::vector<uint8_t> ram(2 * 1024 * 1024);
  const auto word = [&](uint32_t a, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
      ram[(a & 0x1fffff) + i] = v >> (8 * i);
  };
  constexpr uint32_t head = 0x800af418, actor = 0x80100000, prims = 0x80110000;
  word(head + 4, actor);
  word(actor, head);
  word(actor + 4, head + 32);
  word(head + 32, actor);
  word(actor + 8, 0x8007af88);
  word(actor + 20, 0x80120000);
  word(actor + 32, prims);
  std::memcpy(ram.data() + 0x120000, "scn_mask.c", 11);
  word(prims + 20, 0x62287838);
  word(prims + 24, 0xff90ff60);
  word(prims + 28, 0x00e00140);
  for (unsigned i = 0; i < 112; ++i) {
    word(prims + 68 + i * 16, 0x62404040);
    word(prims + 72 + i * 16, packedVertex(-160, -112 + int(i) * 2));
    word(prims + 76 + i * 16, 0x00010140);
  }
  games::mgs::captureScreenEffects(ram);
  const auto send = [&](uint32_t value, uint32_t address = UINT32_MAX) {
    gpu.writeGP0(value, address);
    native.writeGP0(value, address);
  };
  const auto renderSensor = [&](uint32_t frame, uint32_t guestTime, bool paused,
                                uint32_t color = 0x62287838,
                                uint32_t gameStatus = 0,
                                uint32_t background = 13 | (37 << 8) | (91 << 16),
                                float offsetX = 0.25f, float width = 1.f,
                                float height = 1.f, float originY = 32.f) {
    word(0x800ada48, guestTime);
    word(0x800ae0a0, paused);
    word(0x800adb3c, gameStatus);
    word(prims + 20, color);
    games::mgs::captureScreenEffects(ram);
    send(0xe5000000);
    send(0xe3000000);
    send(0xe4000000 | 319 | (223 << 10));
    send(0x02000000 | background);
    send(0);
    send(320 | (224 << 16));
    constexpr Color32 red{217, 219, 227, 255};
    send(0x28000000 | red.r | (red.g << 8) | (red.b << 16));
    for (uint32_t i = 0; i < 4; ++i) {
      const auto xy = packedVertex(32 + int(i & 1), 32 + int(i >> 1));
      auto value = projectedVertex(xy, 32.f + offsetX + float(i & 1)*width,
                                   originY + float(i >> 1)*height, i + 1);
      gpu.writeGP0(xy, UINT32_MAX, &value);
      native.writeGP0(xy);
    }
    send(0xe5000000 | 160 | (112 << 11));
    send(0xe1000220);
    send(color, prims + 20);
    send(packedVertex(-160, -112));
    send(320 | (224 << 16));
    send(0xe1000240);
    for (unsigned i = 0; i < 112; ++i) {
      send(0x62404040, prims + 68 + i * 16);
      send(packedVertex(-160, -112 + int(i) * 2));
      send(320 | (1 << 16));
    }
    gpu.snapshotDisplayBuffer(frame);
    renderer.renderFrame();
    renderer.drainReplayDiagnostics();
  };
  if (benchmark) {
    std::ofstream report(benchmark);
    ASSERT_TRUE(report);
    report << "{\"internal_scale\":12,\"window\":[3840,2160],"
              "\"warmup_frames\":24,\"measured_frames\":96,\"profiles\":[";
    struct Profile { const char *name; uint32_t color, status; };
    constexpr std::array profiles{
        Profile{"cinematic_red", 0x62000050, 0x80000028u},
        Profile{"cinematic_green", 0x62287838, 0x80000028u},
        Profile{"night_vision", 0x62287838, 4u},
        Profile{"thermal_vision", 0x62000050, 8u}};
    uint32_t frame = 2;
    for (size_t i = 0; i < profiles.size(); ++i) {
      const auto &p = profiles[i];
      for (unsigned n = 0; n < 24; ++n, ++frame)
        renderSensor(frame, frame, false, p.color, p.status);
      const auto startNs = metrics::get("host.renderer.vulkan_gpu.total_ns");
      const auto startCount = metrics::get("host.renderer.vulkan_gpu.total_samples");
      for (unsigned n = 0; n < 96; ++n, ++frame)
        renderSensor(frame, frame, false, p.color, p.status);
      const auto ns = metrics::get("host.renderer.vulkan_gpu.total_ns") - startNs;
      const auto samples = metrics::get("host.renderer.vulkan_gpu.total_samples") - startCount;
      EXPECT_EQ(samples, 96u);
      report << (i ? "," : "") << "{\"name\":\"" << p.name
             << "\",\"total_ns\":" << ns << ",\"samples\":" << samples << "}";
    }
    EXPECT_EQ(metrics::get("host.renderer.vulkan_replay_executor_downgrade"), 0u);
    report << "],\"downgrades\":"
           << metrics::get("host.renderer.vulkan_replay_executor_downgrade") << "}\n";
    renderer.destroy();
    return;
  }
  renderSensor(2, 17, false);
  EXPECT_EQ(metrics::get("mgs.screen_effect.pass.night_vision"), 1u);
  EXPECT_EQ(metrics::get("mgs.screen_effect.replaced_packets"), 113u);
  EXPECT_EQ(metrics::get("host.renderer.mgs_screen.snapshot"), 1u);
  EXPECT_EQ(metrics::get("host.renderer.vulkan_replay_executor_downgrade"), 0u);
  EXPECT_EQ(gpu.internalResolutionScale(), 1u);
  for (uint32_t i = 0; i < GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT; ++i)
    ASSERT_EQ(gpu.getVRAM()[i].raw, native.getVRAM()[i].raw) << i;
  const auto readRaw = [&](uint32_t frame) {
    std::ifstream ppm(directory / ("frame-" + std::to_string(frame) + ".ppm"),
                      std::ios::binary);
    std::string magic;
    unsigned width = 0, height = 0, max = 0;
    ppm >> magic >> width >> height >> max;
    ppm.get();
    EXPECT_EQ(magic, "P6");
    EXPECT_EQ(width, 192u);
    EXPECT_EQ(height, 96u);
    EXPECT_EQ(max, 255u);
    std::vector<uint8_t> pixels(width * height * 3);
    ppm.read(reinterpret_cast<char *>(pixels.data()), pixels.size());
    EXPECT_TRUE(ppm);
    return pixels;
  };
  // Functional color tests integrate 3x3 UHD samples; phase tests below
  // inspect unfiltered pixels so they resolve the 240-line tube.
  const auto readImage = [&](uint32_t frame) {
    const auto raw=readRaw(frame);std::vector<uint8_t> pixels(64*32*3);
    for(unsigned y=0;y<32;++y) for(unsigned x=0;x<64;++x) for(unsigned c=0;c<3;++c) {
      unsigned sum=0;
      for(unsigned j=0;j<3;++j) for(unsigned i=0;i<3;++i)
        sum+=raw[((y*3+j)*192+x*3+i)*3+c];
      pixels[(y*64+x)*3+c]=(sum+4)/9;
    }
    return pixels;
  };
  const auto pixels = readImage(2);
  ASSERT_EQ(pixels.size(), 64u * 32 * 3);
  // Optical reconstruction may spread the subpixel source across its cell.
  // Its energy must nevertheless remain localized around that source.
  double nearGreen=0,farGreen=0;
  for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
    nearGreen+=pixels[(y*64+x)*3+1]/64.0;
    farGreen+=pixels[(y*64+x+40)*3+1]/64.0;
  }
  EXPECT_GT(nearGreen,farGreen+15);
  // Noise must be perceptible over a uniform source, not +/- one RGB8 level.
  double sum = 0, squared = 0;
  unsigned count = 0;
  for (unsigned y = 12; y < 32; ++y)
    for (unsigned x = 12; x < 64; ++x) {
      double value = pixels[(y * 64 + x) * 3 + 1];
      sum += value;
      squared += value * value;
      ++count;
    }
  EXPECT_GT(std::sqrt(squared / count - (sum / count) * (sum / count)), 5.0);
  renderSensor(3, 18, false);
  const auto next = readImage(3);
  EXPECT_NE(pixels, next);
  renderSensor(4, 17, false);
  EXPECT_EQ(pixels, readImage(4)); // save-state rewind
  renderSensor(5, 19, true);
  const auto pause = readImage(5);
  renderSensor(6, 40, true);
  EXPECT_EQ(pause, readImage(6)); // host ticks cannot animate a paused sensor
  renderSensor(7, 41, false, 0x62000050, 0x80000028u,13|(37<<8)|(91<<16),0.25f,4.f,4.f);
  const auto cinematic = readImage(7);
  ASSERT_EQ(cinematic.size(), 64u * 32 * 3);
  // The bandwidth-limited camera signal now has separate RGB phosphors;
  // neighboring presentation pixels need not have the same channel value.
  double redGrade=0,greenGrade=0;
  for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x) {
    redGrade+=cinematic[(y*64+x)*3]/16.0;
    greenGrade+=cinematic[(y*64+x)*3+1]/16.0;
  }
  EXPECT_GT(redGrade,greenGrade+50); // grade averaged over a complete RGB triad
  renderSensor(8, 61, false, 0x62000050, 0x80000028u,13|(37<<8)|(91<<16),0.25f,4.f,4.f);
  EXPECT_NE(cinematic, readImage(8)); // camera noise follows guest time
  // GoggleIR's retail palette is warm red, not blue/magenta false color.
  renderSensor(9, 61, false, 0x62000050, 8u, 40 | (12<<8) | (12<<16));
  const auto thermal=readImage(9);
  EXPECT_NE(cinematic,thermal); // actual gameplay thermal stays distinct
  const auto phaseContrast=[&](uint32_t frame,unsigned channel,bool horizontal) {
    const auto raw=readRaw(frame);
    double light=0,dark=0;unsigned nlight=0,ndark=0;
    for(unsigned y=36;y<96;++y) for(unsigned x=120;x<192;++x) {
      const double coordinate=32.0+((horizontal ? x : y)+0.5)/12.0;
      double phase=coordinate*(horizontal ? 426.0/320.0 : 240.0/224.0);
      phase-=std::floor(phase);
      const double peak=horizontal ? (channel+0.5)/3.0 : 0.5;
      const double distance=std::abs(std::remainder(phase-peak,1.0));
      const auto value=raw[(y*192+x)*3+channel];
      if(distance<0.12) {light+=value;++nlight;}
      if(distance>0.38) {dark+=value;++ndark;}
    }
    return light/nlight-dark/ndark;
  };
  // Recover the requested 240-row period and three ordered RGB peaks from
  // unfiltered UHD pixels. An old 112-row grid cannot satisfy these phases.
  EXPECT_GT(phaseContrast(2,1,false),8.0);
  EXPECT_GT(phaseContrast(9,0,false),8.0);
  EXPECT_GT(phaseContrast(7,0,false),8.0);
  EXPECT_GT(phaseContrast(2,0,true),5.0);
  EXPECT_GT(phaseContrast(2,1,true),8.0);
  EXPECT_GT(phaseContrast(2,2,true),5.0);
  EXPECT_GT(phaseContrast(7,0,true),8.0);
  EXPECT_GT(phaseContrast(9,0,true),8.0);
  // Average multiple complete triads: an individual blue phosphor must
  // remain blue even when the overall authored thermal palette is red.
  std::array<double,3> thermalMean{};
  for(unsigned y=12;y<32;++y) for(unsigned x=40;x<64;++x)
    for(unsigned c=0;c<3;++c) thermalMean[c]+=thermal[(y*64+x)*3+c]/480.0;
  EXPECT_GT(thermalMean[0],thermalMean[1]+35);
  EXPECT_GT(thermalMean[0],thermalMean[2]+35);
  renderSensor(10,41,false,0x62000050,0x80000028u,13|(37<<8)|(91<<16),0.25f,4.f,4.f);
  EXPECT_EQ(cinematic,readImage(10)); // rewind reproduces the complete CRT signal
  renderSensor(11,10,true,0x62000050,0x80000028u,13|(37<<8)|(91<<16),0.25f,4.f,4.f);
  const auto pausedCrt=readImage(11);
  renderSensor(12,70,true,0x62000050,0x80000028u,13|(37<<8)|(91<<16),0.25f,4.f,4.f);
  EXPECT_EQ(pausedCrt,readImage(12));
  renderSensor(13,17,false,0x62287838,0x80000028u);
  const auto greenCrt=readImage(13);
  EXPECT_NE(greenCrt,pixels); // cinematic CRT is distinct from gameplay goggles
  EXPECT_GT(phaseContrast(13,1,false),8.0);
  EXPECT_GT(phaseContrast(13,1,true),8.0);
  double greenGradeContrast=0;
  for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x)
    greenGradeContrast+=(double(greenCrt[(y*64+x)*3+1])-greenCrt[(y*64+x)*3])/16.0;
  EXPECT_GT(greenGradeContrast,35); // color response averaged across RGB cells
  renderSensor(14,17,false,0x62000050,0x80000028u,0);
  renderSensor(15,17,false,0x62287838,0x80000028u,0);
  const auto blackRed=readImage(14),blackGreen=readImage(15);
  double redFloor=0,greenFloor=0;
  for(unsigned y=0;y<32;++y) for(unsigned x=56;x<64;++x) {
    redFloor+=blackRed[(y*64+x)*3]/256.0;
    greenFloor+=blackGreen[(y*64+x)*3+1]/256.0;
  }
  // ScnMask colors the shadows too. Keeping these black loses the authored
  // silhouettes regardless of how much bloom is added to bright pixels.
  EXPECT_GT(redFloor,35);EXPECT_LT(redFloor,85);
  EXPECT_GT(greenFloor,35);EXPECT_LT(greenFloor,90); // intermediate green pedestal
  EXPECT_EQ(metrics::get("mgs.screen_effect.pass.cinematic_red_tint"), 6u);
  EXPECT_EQ(metrics::get("mgs.screen_effect.pass.thermal_vision"), 1u);
  // The retail NVG palette already doubles intensity. Two bright surfaces
  // must remain distinguishable, rather than both clipping to a white wash.
  renderSensor(16,17,false,0x62287838,4u,0x808080);
  renderSensor(17,17,false,0x62287838,4u,0xe0e0e0);
  const auto midNvg=readImage(16),brightNvg=readImage(17);
  double midGreen=0,brightGreen=0;
  for(unsigned y=12;y<32;++y) for(unsigned x=32;x<64;++x) {
    midGreen+=midNvg[(y*64+x)*3+1]/640.0;
    brightGreen+=brightNvg[(y*64+x)*3+1]/640.0;
  }
  EXPECT_GT(brightGreen,midGreen+20);
  EXPECT_LT(brightGreen,245);
  // A mask over a continuous picture cannot pass this: swap equally bright
  // half-cell source bars. Their integrated cell signal is identical, so
  // their reconstructed RGB emitters must be identical too. Moving the bar
  // to the next cell must change the output. Guest time/noise remain fixed.
  constexpr float pitch=320.f/426.f, cellLeft=43.f*pitch-32.f;
  renderSensor(18,17,false,0x62287838,4u,0,cellLeft,pitch*0.5f,224.f,0.f);
  renderSensor(19,17,false,0x62287838,4u,0,cellLeft+pitch*0.5f,pitch*0.5f,224.f,0.f);
  renderSensor(20,17,false,0x62287838,4u,0,cellLeft+pitch,pitch*0.5f,224.f,0.f);
  EXPECT_EQ(readRaw(18),readRaw(19));
  EXPECT_NE(readRaw(18),readRaw(20));
  // Isolate bloom from phosphor phase and camera grain: compare the exact
  // same pixels/time with the bright source moved beyond the halo radius.
  // The measured patch is outside the source and its local reconstruction.
  renderSensor(21,41,false,0x62000050,0x80000028u,13|(37<<8)|(91<<16),-16.f,4.f,4.f);
  const auto noNearbySource=readImage(21);
  int haloEnergy=0;
  for(unsigned y=0;y<24;++y) for(unsigned x=20;x<28;++x)
    haloEnergy+=int(cinematic[(y*64+x)*3])-noNearbySource[(y*64+x)*3];
  EXPECT_GT(haloEnergy,40);
  for (uint32_t i = 0; i < GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT; ++i)
    ASSERT_EQ(gpu.getVRAM()[i].raw, native.getVRAM()[i].raw) << i;
  EXPECT_EQ(metrics::get("host.renderer.vulkan_replay_executor_downgrade"), 0u);
  renderer.destroy();
}

TEST(MgsScreenEffectsTest, OptInGpuFadeEndpointsAndEnvelopeUseTrueColor) {
  if (!std::getenv("PS1_RUN_VULKAN_INTEGRATION"))
    GTEST_SKIP();
  struct Cleanup {
    ~Cleanup() {
      games::mgs::configure(false, false);
      games::mgs::captureScreenEffects({});
      metrics::reset();
      metrics::setEnabledForTesting(false);
    }
  } cleanup;
  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK", "all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM", "2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "11");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION", "32,32,2,1");
  games::mgs::configure(true, false);
  metrics::reset();
  metrics::setEnabledForTesting(true);
  GPU gpu, native;
  configureVulkanNativeOracle(gpu, true);
  RendererOptions options;
  options.internalScale = 4;
  options.pgxp = true;
  options.trueDepth = true;
  options.trueDepthMode = "hybrid";
  options.trueColor = true;
  RendererVulkan renderer(gpu, options);
  ASSERT_TRUE(renderer.init("MGS fade color test", 320, 224, false, false));
  gpu.writeGP1(0x03000000);
  gpu.writeGP1(0x05000000);
  gpu.snapshotDisplayBuffer(1);
  renderer.renderFrame();
  renderer.drainReplayDiagnostics();
  std::vector<uint8_t> ram(2 * 1024 * 1024);
  const auto word = [&](uint32_t a, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
      ram[(a & 0x1fffff) + i] = v >> (8 * i);
  };
  constexpr uint32_t head = 0x800af45c, actor = 0x80100000, prims = 0x80110000;
  word(head + 4, actor);
  word(actor, head);
  word(actor + 4, head + 32);
  word(head + 32, actor);
  word(actor + 8, 0x80070000);
  word(actor + 20, 0x80120000);
  word(actor + 36, prims);
  std::memcpy(ram.data() + 0x120000, "fadeio.c", 9);
  const auto send = [&](uint32_t v, uint32_t a = UINT32_MAX) {
    gpu.writeGP0(v, a);
    native.writeGP0(v, a);
  };
  send(0xe3000000);
  send(0xe4000000 | 319 | (223 << 10));
  const Color32 base{101, 53, 211, 255};
  uint64_t frame = 2;
  for (bool white : {false, true})
    for (uint32_t shade : {0u, 128u, 255u}) {
      word(actor + 52, white ? 2 : 0);
      games::mgs::captureScreenEffects(ram);
      send(0x02000000 | base.r | (base.g << 8) | (base.b << 16));
      send(0);
      send(320 | (224 << 16));
      send(white ? 0xe1000220 : 0xe1000240);
      send(0x62000000 | shade | (shade << 8) | (shade << 16), prims + 20);
      send(0);
      send(320 | (224 << 16));
      gpu.snapshotDisplayBuffer(frame++);
      renderer.renderFrame();
      renderer.drainReplayDiagnostics();
      const auto channel = [&](uint8_t value) {
        // FadeIO subtracts/adds light: at shade 128 the dark red/green
        // channels must already be black; white mode must saturate blue.
        return uint8_t(std::clamp(int(value)+(white ? int(shade) : -int(shade)),0,255));
      };
      std::array<Color32, 32> color;
      color.fill(
          Color32{channel(base.r), channel(base.g), channel(base.b), 255});
      std::array<uint8_t, 32> mask{};
      std::array<float, 32> depth;
      depth.fill(1);
      const VulkanReplayDiagnosticPlanes planes{
          {color.data(), sizeof(color), 8 * sizeof(Color32)},
          {mask.data(), mask.size(), 8},
          {depth.data(), sizeof(depth), 8 * sizeof(float)}};
      const auto expected =
          captureVulkanReplayReadback({32, 32, 2, 1, 4}, planes);
      EXPECT_EQ(
          metrics::getState(
              "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
          metricHash(expected.actual.rgba8))
          << white << " " << shade;
      EXPECT_EQ(
          metrics::getState(
              "host.renderer.vulkan_replay_readback.last.actual_mask_hash"),
          metricHash(expected.actual.maskR8));
      for (uint32_t i = 0; i < GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT; ++i)
        ASSERT_EQ(gpu.getVRAM()[i].raw, native.getVRAM()[i].raw) << i;
    }
  for (bool stun : {true, false}) {
    std::memcpy(ram.data() + 0x120000, stun ? "stgfd_io.c" : "red_alrt.c", 11);
    word(actor + 32, prims);
    word(prims + 20, 0x62000000);
    word(prims + 24, 0xff90ff60);
    word(prims + 28, 0x00e00140);
    for (uint32_t intensity : {0u, 255u}) {
      games::mgs::captureScreenEffects(ram);
      send(0x02000000 | base.r | (base.g << 8) | (base.b << 16));
      send(0);
      send(320 | (224 << 16));
      send(0xe1000220);
      send(0x62000000 | intensity |
               (stun ? (intensity << 8) | (intensity << 16) : 0),
           prims + 20);
      send(0);
      send(320 | (224 << 16));
      gpu.snapshotDisplayBuffer(frame++);
      renderer.renderFrame();
      renderer.drainReplayDiagnostics();
      Color32 expectedColor = base;
      if (intensity && stun)
        expectedColor = {255, 255, 255, 255};
      if (intensity && !stun)
        expectedColor.r =
            uint8_t(std::lround(255.f - 0.25f*(255.f - base.r) * std::exp(-1.8f)));
      std::array<Color32, 32> color;
      color.fill(expectedColor);
      std::array<uint8_t, 32> mask{};
      std::array<float, 32> depth;
      depth.fill(1);
      const VulkanReplayDiagnosticPlanes planes{
          {color.data(), sizeof(color), 8 * sizeof(Color32)},
          {mask.data(), mask.size(), 8},
          {depth.data(), sizeof(depth), 8 * sizeof(float)}};
      const auto expected =
          captureVulkanReplayReadback({32, 32, 2, 1, 4}, planes);
      EXPECT_EQ(
          metrics::getState(
              "host.renderer.vulkan_replay_readback.last.actual_rgba8_hash"),
          metricHash(expected.actual.rgba8))
          << stun << " " << intensity;
      for (uint32_t i = 0; i < GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT; ++i)
        ASSERT_EQ(gpu.getVRAM()[i].raw, native.getVRAM()[i].raw) << i;
    }
  }
  EXPECT_EQ(metrics::get("mgs.screen_effect.pass.stun_flash"), 2u);
  EXPECT_EQ(metrics::get("mgs.screen_effect.pass.alert_tint"), 2u);
  EXPECT_EQ(metrics::get("mgs.screen_effect.pass.fade_white"), 3u);
  EXPECT_EQ(metrics::get("mgs.screen_effect.pass.fade_black"), 3u);
  EXPECT_EQ(metrics::get("host.renderer.vulkan_replay_executor_downgrade"), 0u);
  renderer.destroy();
}
