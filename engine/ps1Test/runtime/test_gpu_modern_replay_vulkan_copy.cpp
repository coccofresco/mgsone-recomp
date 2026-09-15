#include "runtime/games/crash3_profile.h"
#include "runtime/games/mgs_asset_cache.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/games/mgs_screen_effects.h"
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

TEST(MgsScreenEffectsTest, OptInGpuVisorRetainsSubpixelEdgesAndNativeVram) {
  if (!std::getenv("PS1_RUN_VULKAN_INTEGRATION"))
    GTEST_SKIP();
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
  ScopedEnvironment readback("PS1_VULKAN_REPLAY_READBACK", "all");
  ScopedEnvironment from("PS1_VULKAN_REPLAY_READBACK_FRAME_FROM", "2");
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "6");
  ScopedEnvironment region("PS1_VULKAN_REPLAY_READBACK_REGION", "32,32,16,8");
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
  ASSERT_TRUE(renderer.init("MGS visor subpixel test", 320, 224, false, false));
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
  const auto renderSensor = [&](uint32_t frame, uint32_t guestTime,
                                bool paused) {
    word(0x800ada48, guestTime);
    word(0x800ae0a0, paused);
    games::mgs::captureScreenEffects(ram);
    send(0xe5000000);
    send(0xe3000000);
    send(0xe4000000 | 319 | (223 << 10));
    send(0x02000000 | 13 | (37 << 8) | (91 << 16));
    send(0);
    send(320 | (224 << 16));
    constexpr Color32 red{217, 219, 227, 255};
    send(0x28000000 | red.r | (red.g << 8) | (red.b << 16));
    for (uint32_t i = 0; i < 4; ++i) {
      const auto xy = packedVertex(32 + int(i & 1), 32 + int(i >> 1));
      auto value = projectedVertex(xy, 32.25f + float(i & 1),
                                   32.f + float(i >> 1), i + 1);
      gpu.writeGP0(xy, UINT32_MAX, &value);
      native.writeGP0(xy);
    }
    send(0xe5000000 | 160 | (112 << 11));
    send(0xe1000220);
    send(0x62287838, prims + 20);
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
  renderSensor(2, 17, false);
  EXPECT_EQ(metrics::get("mgs.screen_effect.pass.night_vision"), 1u);
  EXPECT_EQ(metrics::get("mgs.screen_effect.replaced_packets"), 113u);
  EXPECT_EQ(metrics::get("host.renderer.mgs_screen.snapshot"), 1u);
  EXPECT_EQ(metrics::get("host.renderer.vulkan_replay_executor_downgrade"), 0u);
  EXPECT_EQ(gpu.internalResolutionScale(), 1u);
  for (uint32_t i = 0; i < GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT; ++i)
    ASSERT_EQ(gpu.getVRAM()[i].raw, native.getVRAM()[i].raw) << i;
  const auto readImage = [&](uint32_t frame) {
    std::ifstream ppm(directory / ("frame-" + std::to_string(frame) + ".ppm"),
                      std::ios::binary);
    std::string magic;
    unsigned width = 0, height = 0, max = 0;
    ppm >> magic >> width >> height >> max;
    ppm.get();
    EXPECT_EQ(magic, "P6");
    EXPECT_EQ(width, 64u);
    EXPECT_EQ(height, 32u);
    EXPECT_EQ(max, 255u);
    std::vector<uint8_t> pixels(width * height * 3);
    ppm.read(reinterpret_cast<char *>(pixels.data()), pixels.size());
    EXPECT_TRUE(ppm);
    return pixels;
  };
  const auto pixels = readImage(2);
  ASSERT_EQ(pixels.size(), 64u * 32 * 3);
  // Even with pronounced grain and light scatter the bright edge must begin
  // at column one, not at an upscaled native-pixel boundary (zero or four).
  for (unsigned y = 0; y < 4; ++y) {
    const auto green = [&](unsigned x) {
      return int(pixels[(y * 64 + x) * 3 + 1]);
    };
    int background = green(0), foreground = 255;
    for (unsigned x = 1; x < 5; ++x)
      foreground = std::min(foreground, green(x));
    for (unsigned x = 5; x < 8; ++x)
      background = std::max(background, green(x));
    EXPECT_GT(foreground, background + 30);
  }
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
  ScopedEnvironment to("PS1_VULKAN_REPLAY_READBACK_FRAME_TO", "7");
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
        return uint8_t(std::lround(float(value) * (1.f - float(shade) / 255.f) +
                                   (white ? float(shade) : 0.f)));
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
  EXPECT_EQ(metrics::get("mgs.screen_effect.pass.fade_white"), 3u);
  EXPECT_EQ(metrics::get("mgs.screen_effect.pass.fade_black"), 3u);
  EXPECT_EQ(metrics::get("host.renderer.vulkan_replay_executor_downgrade"), 0u);
  renderer.destroy();
}
