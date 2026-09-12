#include "runtime/gpu/modern_replay_preflight.h"
#include "runtime/gpu/modern_replay_vulkan_bake.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

using namespace ps1::gpu::replay;

namespace {

ReplayPolicyState replayPolicy(uint32_t internalScale = 4) {
  ReplayPolicyState policy;
  policy.internalScale = internalScale;
  policy.pgxp = true;
  policy.trueColor = true;
  policy.trueDepthMode = ReplayDepthMode::Hybrid;
  return policy;
}

FrameLatch replayLatch(ReplayPolicyState policy) {
  FrameLatch latch;
  latch.frameNumber = 11;
  latch.contentRevision = 22;
  latch.completedSubmissionId = 33;
  latch.sequenceBegin = 0;
  latch.sequenceEnd = 1000;
  latch.policy = policy;
  latch.display.aperture = NativeRect{0, 0, 320, 240};
  return latch;
}

DrawPacket precisePacket(uint64_t sequence = 1, bool quad = false,
                         bool textured = false) {
  DrawPacket packet;
  packet.opcode = static_cast<uint8_t>(0x20u | 0x10u | (quad ? 0x08u : 0u) |
                                       (textured ? 0x05u : 0u));
  packet.packetSequence = sequence;
  packet.submissionId = 9;
  packet.vertexCount = quad ? 4 : 3;
  packet.indexCount = quad ? 6 : 3;
  packet.validMask = static_cast<uint8_t>((1u << packet.vertexCount) - 1u);
  packet.triangleIndices =
      quad ? std::array<uint8_t, DrawPacket::MAX_INDICES>{0, 1, 2, 0, 2, 3}
           : std::array<uint8_t, DrawPacket::MAX_INDICES>{0, 1, 2, 0, 0, 0};
  packet.textured = textured;
  packet.gouraud = true;
  packet.rawTexture = textured;

  packet.state.drawArea = DrawArea{0, 0, 1023, 511};
  packet.state.texturePage.raw = 0x0123;
  packet.state.clut.raw = 0x0456;
  packet.state.textureWindow = TextureWindowState{0x12345, 8, 16, 24, 32};
  packet.state.textureUvDomain = TextureUvDomainState{
      4.5, 5.5, 200.5, 201.5, 0, 0, textured, false, false};
  packet.state.blend.mode = BlendMode::BackgroundMinusForeground;
  packet.state.mask.forceMaskBit = true;
  packet.state.depth.testEnabled = false;
  packet.state.depth.writeEnabled = true;
  packet.state.depth.compare = DepthCompare::Always;
  packet.state.depth.epoch = 7;

  if (textured) {
    packet.nativeRead.required = true;
    packet.nativeRead.tileCount = 1;
    packet.nativeRead.versionHash = 99;
  }

  for (std::size_t index = 0; index < packet.vertexCount; ++index) {
    ReplayVertex &vertex = packet.vertices[index];
    vertex.nativeX = static_cast<int32_t>(10 + index * 10);
    vertex.nativeY = static_cast<int32_t>(20 + index * 5);
    vertex.x = static_cast<double>(vertex.nativeX) + 0.25;
    vertex.y = static_cast<double>(vertex.nativeY) + 0.5;
    vertex.u = 30.75 + static_cast<double>(index);
    vertex.v = 40.25 + static_cast<double>(index * 2);
    vertex.w = 16384.0 + static_cast<double>(index) * 8192.0;
    vertex.rawDepth = vertex.w;
    vertex.nearDepth = 1.0;
    vertex.color =
        Rgb8{static_cast<uint8_t>(10 + index), static_cast<uint8_t>(20 + index),
             static_cast<uint8_t>(30 + index)};
    vertex.preciseXY = true;
    vertex.preciseX = true;
    vertex.preciseY = true;
    vertex.preciseUV = textured;
    vertex.rawDepthValid = true;
  }
  return packet;
}

void makeSemiTransparent(DrawPacket &packet) {
  packet.opcode = static_cast<uint8_t>(packet.opcode | 0x02u);
  packet.state.blend.enabled = true;
  packet.state.blend.texturedStpOnly = packet.textured;
  packet.state.depth.testEnabled = false;
  packet.state.depth.writeEnabled = false;
  packet.state.depth.compare = DepthCompare::Always;
  packet.state.depth.invalidateOnAccepted = true;
}

void makeScreen2D(DrawPacket &packet) {
  packet.route = PacketRoute::Screen2D;
  packet.semanticReason = PacketSemanticReason::Intrinsic2DFamily;
  packet.validMask = 0;
  packet.fallbackReason = PacketFallbackReason::None;
  packet.state.depth.testEnabled = false;
  packet.state.depth.writeEnabled = false;
  packet.state.depth.compare = DepthCompare::Always;
  packet.state.depth.invalidateOnAccepted = true;
  for (std::size_t index = 0; index < packet.vertexCount; ++index) {
    ReplayVertex &vertex = packet.vertices[index];
    vertex.x = vertex.nativeX;
    vertex.y = vertex.nativeY;
    vertex.w = CRASH_REPLAY_FALLBACK_W;
    vertex.rawDepth = 0.0;
    vertex.nearDepth = 0.0;
    vertex.preciseXY = false;
    vertex.preciseX = false;
    vertex.preciseY = false;
    vertex.preciseUV = false;
    vertex.rawDepthValid = false;
    vertex.inventedW = false;
    vertex.nearSaturated = false;
  }
}

VulkanReplayBakeFeatures feedbackFeatures() {
  VulkanReplayBakeFeatures features;
  features.allowTextured = true;
  features.allowSemiTransparent = true;
  features.allowScreen2D = true;
  features.allowDestinationColorFeedback = true;
  features.allowDestinationMaskRead = true;
  features.allowPainterOrdered = true;
  features.allowPainterWriteRealDepth = true;
  features.allowDepthInvalidate = true;
  return features;
}

VulkanReplayBakeFeatures painterWriteFeatures() {
  VulkanReplayBakeFeatures features;
  features.allowPainterWriteRealDepth = true;
  return features;
}

VulkanReplayBakeFeatures cpuUploadFeatures() {
  VulkanReplayBakeFeatures features;
  features.allowCpuUpload = true;
  return features;
}

VulkanReplayBakeFeatures copyRectScratchFeatures() {
  VulkanReplayBakeFeatures features;
  features.allowCopyRectScratch = true;
  return features;
}

NativeTileUpload tileUpload(NativeRect destination = {0, 0, 8, 8}) {
  NativeTileUpload upload;
  upload.destination = destination;
  upload.nativeRevision = 17;
  const std::size_t count =
      static_cast<std::size_t>(destination.width) * destination.height;
  upload.pixels.resize(count);
  for (std::size_t index = 0; index < count; ++index)
    upload.pixels[index] = static_cast<uint16_t>(index);
  return upload;
}

PaintRect paintRect(NativeRect destination = {10, 20, 1, 1},
                    uint64_t depthEpoch = 7) {
  PaintRect paint;
  paint.destination = destination;
  paint.nativeColor = 0x1234;
  paint.color = {17, 34, 251};
  paint.depth.epoch = depthEpoch;
  paint.depth.invalidateOnAccepted = true;
  paint.coalescedCommandCount =
      static_cast<uint64_t>(destination.width) * destination.height;
  return paint;
}

PaintPixels paintPixels(uint64_t depthEpoch = 7) {
  PaintPixels paint;
  paint.pixels = {
      PaintedPixel{NativePoint{4, 5}, 0x801fu, Rgb8{250, 20, 10}, true},
      PaintedPixel{NativePoint{9, 11}, 0x03e0u, Rgb8{10, 240, 20}, false}};
  paint.blend.mode = BlendMode::BackgroundMinusForeground;
  paint.blend.enabled = true;
  paint.blend.texturedStpOnly = true;
  paint.mask.checkMaskBit = true;
  paint.depth.epoch = depthEpoch;
  paint.depth.invalidateOnAccepted = true;
  paint.dither = true;
  return paint;
}

FillRect fillRect(NativeRect destination = {32, 40, 16, 2}) {
  FillRect fill;
  fill.destination = destination;
  fill.color = {18, 52, 86};
  fill.nativeColor = 0x28c2;
  return fill;
}

CopyRect copyRect(NativePoint source = {0, 264},
                  NativePoint destination = {0, 0}, uint32_t width = 512,
                  uint32_t height = 240) {
  CopyRect copy;
  copy.source = source;
  copy.destination = destination;
  copy.width = width;
  copy.height = height;
  const std::vector<uint32_t> sourceTiles =
      conservativeNativeReadTileIndices(copy);
  copy.nativeRead.required = true;
  copy.nativeRead.tileCount = static_cast<uint32_t>(sourceTiles.size());
  copy.nativeRead.versionHash = CRASH_REPLAY_READ_HASH_OFFSET;
  for (const uint32_t tile : sourceTiles) {
    copy.nativeRead.versionHash =
        appendNativeReadDependencyHash(copy.nativeRead.versionHash, tile, 1);
  }
  return copy;
}

CpuUpload cpuUpload(uint64_t transactionId, CpuUploadPhase phase,
                    NativeRect transfer, uint64_t pixelOffset = 0,
                    std::vector<uint16_t> pixels = {},
                    std::vector<uint64_t> acceptedBits = {},
                    MaskState mask = {}) {
  CpuUpload upload;
  upload.transactionId = transactionId;
  upload.phase = phase;
  upload.transfer = transfer;
  upload.pixelOffset = pixelOffset;
  upload.pixels = std::move(pixels);
  upload.acceptedBits = std::move(acceptedBits);
  upload.mask = mask;
  return upload;
}

template <typename... Operations>
ModernReplayExecutionPlan makePlanWithPolicy(ReplayPolicyState policy,
                                             Operations &&...operations) {
  CrashReplayFrameBuilder builder;
  (builder.record(std::forward<Operations>(operations)), ...);
  const CrashReplayFramePtr frame = builder.seal(replayLatch(policy));
  ReplayExecutionPlanResult planned =
      planModernReplayExecution(std::array{frame});
  if (!planned.accepted())
    throw std::logic_error("test fixture did not produce an execution plan");
  return std::move(planned.plan);
}

template <typename... Operations>
ModernReplayExecutionPlan makePlan(Operations &&...operations) {
  return makePlanWithPolicy(replayPolicy(),
                            std::forward<Operations>(operations)...);
}

DrawPacket &firstMutableDraw(ModernReplayExecutionPlan &plan) {
  for (const ReplayExecutionOperation &item : plan.operations()) {
    const Operation &operation = plan.sourceOperation(item);
    if (std::holds_alternative<DrawPacket>(operation)) {
      // Deliberately corrupt immutable source storage to exercise the bake's
      // defensive boundary independently from the producer validation.
      return const_cast<DrawPacket &>(std::get<DrawPacket>(operation));
    }
  }
  throw std::logic_error("test plan has no draw");
}

PaintRect &firstMutablePaintRect(ModernReplayExecutionPlan &plan) {
  for (const ReplayExecutionOperation &item : plan.operations()) {
    const Operation &operation = plan.sourceOperation(item);
    if (std::holds_alternative<PaintRect>(operation)) {
      return const_cast<PaintRect &>(std::get<PaintRect>(operation));
    }
  }
  throw std::logic_error("test plan has no PaintRect");
}

PaintPixels &firstMutablePaintPixels(ModernReplayExecutionPlan &plan) {
  for (const ReplayExecutionOperation &item : plan.operations()) {
    const Operation &operation = plan.sourceOperation(item);
    if (std::holds_alternative<PaintPixels>(operation)) {
      return const_cast<PaintPixels &>(std::get<PaintPixels>(operation));
    }
  }
  throw std::logic_error("test plan has no PaintPixels");
}

FillRect &firstMutableFillRect(ModernReplayExecutionPlan &plan) {
  for (const ReplayExecutionOperation &item : plan.operations()) {
    const Operation &operation = plan.sourceOperation(item);
    if (std::holds_alternative<FillRect>(operation))
      return const_cast<FillRect &>(std::get<FillRect>(operation));
  }
  throw std::logic_error("test plan has no FillRect");
}

CopyRect &firstMutableCopyRect(ModernReplayExecutionPlan &plan) {
  for (const ReplayExecutionOperation &item : plan.operations()) {
    const Operation &operation = plan.sourceOperation(item);
    if (std::holds_alternative<CopyRect>(operation))
      return const_cast<CopyRect &>(std::get<CopyRect>(operation));
  }
  throw std::logic_error("test plan has no CopyRect");
}

CpuUpload &firstMutableCpuUpload(ModernReplayExecutionPlan &plan,
                                 CpuUploadPhase phase) {
  for (const ReplayExecutionOperation &item : plan.operations()) {
    const Operation &operation = plan.sourceOperation(item);
    if (const auto *upload = std::get_if<CpuUpload>(&operation);
        upload != nullptr && upload->phase == phase) {
      return const_cast<CpuUpload &>(*upload);
    }
  }
  throw std::logic_error("test plan has no requested CpuUpload phase");
}

void expectAtomicFailure(
    const VulkanReplayBakeResult &result, VulkanReplayBakeReason reason,
    std::size_t ordinal = VulkanReplayBakeResult::NO_SOURCE_ORDINAL) {
  EXPECT_FALSE(result.accepted());
  EXPECT_EQ(result.reason, reason);
  EXPECT_EQ(result.sourceOrdinal, ordinal);
  EXPECT_TRUE(result.batch.empty());
  EXPECT_EQ(result.batch.internalScale, 0u);
}

} // namespace

TEST(GpuModernReplayVulkanBakeTest,
     BakesTexturedTriangleIntoExactShaderAbiWhenExplicitlyEnabled) {
  const NativeTileUpload upload = tileUpload();
  const DrawPacket packet = precisePacket(1, false, true);
  const ModernReplayExecutionPlan plan = makePlan(upload, packet);

  VulkanReplayBakeFeatures features;
  features.allowTextured = true;
  features.allowPainterWriteRealDepth = true;
  const VulkanReplayBakeResult result =
      bakeModernReplayVulkan(plan, 4, features);
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.commands.size(), 3u);
  ASSERT_EQ(result.batch.vertices.size(), 3u);
  ASSERT_EQ(result.batch.visibilityOwners.size(), 1u);
  EXPECT_EQ(result.batch.internalScale, 4u);
  EXPECT_EQ(result.batch.commands[0].sourceOrdinal, 0u);
  EXPECT_EQ(result.batch.commands[1].sourceOrdinal, 1u);
  EXPECT_EQ(result.batch.commands[2].sourceOrdinal, 2u);

  const auto &bakedUpload =
      std::get<VulkanReplayBakedNativeUpload>(result.batch.commands[0].payload);
  EXPECT_EQ(bakedUpload.firstPixel, 0u);
  EXPECT_EQ(bakedUpload.pixelCount, upload.pixels.size());
  EXPECT_EQ(bakedUpload.regionCount, 1u);
  EXPECT_EQ(result.batch.nativeUploadPixels, upload.pixels);

  const auto &draw =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[1].payload);
  EXPECT_EQ(draw.firstVertex, 0u);
  EXPECT_EQ(draw.vertexCount, 3u);
  EXPECT_EQ(draw.scissor, (VulkanReplayBakedRect{0, 0, 4096, 2048}));
  EXPECT_FLOAT_EQ(draw.uniforms.positionScaleBias[0], 2.0f / 1024.0f);
  EXPECT_FLOAT_EQ(draw.uniforms.positionScaleBias[1], 2.0f / 512.0f);
  EXPECT_FLOAT_EQ(draw.uniforms.positionScaleBias[2], -1.0f);
  EXPECT_FLOAT_EQ(draw.uniforms.positionScaleBias[3], -1.0f);
  EXPECT_FLOAT_EQ(draw.uniforms.depthScaleBias[0],
                  vulkan_abi::PROJECTIVE_W_TO_DEPTH_SCALE);
  EXPECT_EQ(draw.uniforms.uvDomain,
            (std::array<float, 4>{4.5f, 5.5f, 200.5f, 201.5f}));
  EXPECT_EQ(draw.uniforms.textureState[0], packet.state.texturePage.raw);
  EXPECT_EQ(draw.uniforms.textureState[1], packet.state.clut.raw);
  EXPECT_EQ(draw.uniforms.textureState[2], shaderDrawFlags(packet));
  EXPECT_EQ(draw.uniforms.textureState[3], 2u);
  EXPECT_EQ(draw.uniforms.textureWindow,
            (std::array<uint32_t, 4>{8, 16, 24, 32}));
  EXPECT_EQ(draw.uniforms.destinationState,
            (std::array<uint32_t, 4>{0, 0, 0, 0}));
  EXPECT_EQ(draw.depthEpoch, 7u);
  EXPECT_EQ(draw.depthPath, VulkanReplayDrawDepthPath::PainterWriteRealDepth);
  EXPECT_EQ(draw.feedbackPlanes, 0u);
  EXPECT_EQ(draw.visibilityOwnerCount, 1u);
  EXPECT_EQ(draw.visibilityOwnerTokens[0], 1u);
  ASSERT_EQ(draw.segmentCount, 1u);
  EXPECT_EQ(draw.segments[0].firstVertex, draw.firstVertex);
  EXPECT_EQ(draw.segments[0].vertexCount, draw.vertexCount);
  EXPECT_EQ(draw.segments[0].scissor, draw.scissor);
  EXPECT_EQ(draw.segments[0].destinationSnapshot, VulkanReplayBakedRect{});
  EXPECT_EQ(draw.segments[0].snapshotAction,
            VulkanReplayDestinationSnapshotAction::None);

  for (std::size_t index = 0; index < 3; ++index) {
    const vulkan_abi::Vertex &vertex = result.batch.vertices[index];
    EXPECT_FLOAT_EQ(vertex.positionNative[0],
                    static_cast<float>(packet.vertices[index].x));
    EXPECT_FLOAT_EQ(vertex.positionNative[1],
                    static_cast<float>(packet.vertices[index].y));
    EXPECT_FLOAT_EQ(vertex.projectiveW,
                    static_cast<float>(packet.vertices[index].w));
    EXPECT_FLOAT_EQ(vertex.uv[0], static_cast<float>(packet.vertices[index].u));
    EXPECT_FLOAT_EQ(vertex.uv[1], static_cast<float>(packet.vertices[index].v));
  }
  const ReplayVisibilityAuditOwner &owner =
      result.batch.visibilityOwners.front();
  EXPECT_EQ(owner.packetSequence, packet.packetSequence);
  EXPECT_EQ(owner.submissionId, packet.submissionId);
  EXPECT_EQ(owner.sourceOrdinal, 1u);
  EXPECT_EQ(owner.triangleIndex, 0u);
  EXPECT_TRUE(owner.comparable());
  EXPECT_TRUE(owner.provenance.validTopology);
  EXPECT_EQ(owner.provenance.completeEvidence(), 0u)
      << "render eligibility must not fabricate intrinsic identity evidence";
}

TEST(GpuModernReplayVulkanBakeTest,
     VisibilityBoundsFollowSubmittedGeometryOutsideDisplayPage) {
  DrawPacket first = precisePacket(1);
  DrawPacket second = precisePacket(2);
  for (ReplayVertex &vertex : second.vertices) {
    vertex.nativeX += 512;
    vertex.x += 512.0;
  }
  const ModernReplayExecutionPlan plan = makePlan(first, second);
  const VulkanReplayBakeResult baked =
      bakeModernReplayVulkan(plan, 4, painterWriteFeatures());
  ASSERT_TRUE(baked.accepted());

  const VulkanReplayVisibilityBoundsResult bounds =
      measureVulkanReplayVisibilityBounds(baked.batch);
  ASSERT_TRUE(bounds.complete());
  // The exact conservative union spans both submitted pages. It is derived
  // from the 4x vertices and one-sample padding, not from the latch aperture.
  EXPECT_EQ(bounds.atlasArea, (VulkanReplayBakedRect{40, 81, 2130, 42}));
}

TEST(GpuModernReplayVulkanBakeTest,
     VisibilityBoundsFailClosedForMalformedBakedSegment) {
  const ModernReplayExecutionPlan plan = makePlan(precisePacket());
  VulkanReplayBakeResult baked =
      bakeModernReplayVulkan(plan, 4, painterWriteFeatures());
  ASSERT_TRUE(baked.accepted());
  auto &draw =
      std::get<VulkanReplayBakedDraw>(baked.batch.commands.front().payload);
  draw.segments[0].firstVertex =
      static_cast<uint32_t>(baked.batch.vertices.size());

  const VulkanReplayVisibilityBoundsResult bounds =
      measureVulkanReplayVisibilityBounds(baked.batch);
  EXPECT_EQ(bounds.status, VulkanReplayVisibilityBoundsStatus::InvalidBatch);
  EXPECT_EQ(bounds.atlasArea, VulkanReplayBakedRect{});
}

TEST(GpuModernReplayVulkanBakeTest, VisibilityBoundsReportEmptyWithoutDraws) {
  VulkanReplayBakedBatch batch;
  batch.internalScale = 4;
  const VulkanReplayVisibilityBoundsResult bounds =
      measureVulkanReplayVisibilityBounds(batch);
  EXPECT_EQ(bounds.status, VulkanReplayVisibilityBoundsStatus::Empty);
  EXPECT_EQ(bounds.atlasArea, VulkanReplayBakedRect{});
}

TEST(GpuModernReplayVulkanBakeTest,
     BuildsDeterministicTriangleOwnerCatalogWithExactProvenance) {
  DrawPacket packet = precisePacket(41, true);
  for (std::size_t index = 0; index < packet.vertexCount; ++index) {
    ReplayVertex &vertex = packet.vertices[index];
    vertex.homogeneousQ = true;
    vertex.provenance.projectionId = 500u + index;
    vertex.provenance.projectionContext = 0x11u;
    vertex.provenance.transformLineage = 0x22334455u;
    vertex.provenance.modelVertexSource = 0x80030001u + index * 8u;
    vertex.provenance.modelValueFlowSourceFingerprint = 0x9000u + index;
    vertex.provenance.modelValueFlowProgramFingerprint = 0x55667788u;
    vertex.provenance.gp0WordSource = 0x80040001u + index * 4u;
    vertex.provenance.dmaListSource = 0x1001u;
    vertex.provenance.dmaNodeSource = 0x2001u;
    vertex.provenance.dmaOrderingTableAnchorSource = 0x1801u;
    vertex.provenance.dmaTraversalOrdinal = 7u;
    vertex.provenance.evidence = REPLAY_VERTEX_PROVENANCE_KNOWN_EVIDENCE;
  }

  const VulkanReplayBakeResult result =
      bakeModernReplayVulkan(makePlan(packet), 4, painterWriteFeatures());
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.visibilityOwners.size(), 2u);
  const VulkanReplayBakedDraw &draw =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[0].payload);
  EXPECT_EQ(draw.visibilityOwnerCount, 2u);
  EXPECT_EQ(draw.visibilityOwnerTokens, (std::array<uint32_t, 2>{1, 2}));

  const ReplayVisibilityAuditOwner &first = result.batch.visibilityOwners[0];
  const ReplayVisibilityAuditOwner &second = result.batch.visibilityOwners[1];
  EXPECT_EQ(first.packetSequence, 41u);
  EXPECT_EQ(first.sourceOrdinal, 0u);
  EXPECT_EQ(first.frameIndex, 0u);
  EXPECT_EQ(first.operationIndex, 0u);
  EXPECT_EQ(first.triangleIndex, 0u);
  EXPECT_EQ(second.triangleIndex, 1u);
  EXPECT_TRUE(first.provenance.coherentProjectionContext());
  EXPECT_TRUE(first.provenance.coherentTransformLineage());
  EXPECT_TRUE(first.provenance.coherentDmaTraversal());
  EXPECT_TRUE(first.provenance.coherentOrderingTableAnchor());
  EXPECT_TRUE(second.provenance.coherentProjectionContext());
  EXPECT_TRUE(second.provenance.coherentTransformLineage());
  EXPECT_TRUE(second.provenance.coherentDmaTraversal());
  EXPECT_TRUE(second.provenance.coherentOrderingTableAnchor());
  EXPECT_EQ(first.provenance.vertexIndices, (std::array<uint8_t, 3>{0, 1, 2}));
  EXPECT_EQ(second.provenance.vertexIndices, (std::array<uint8_t, 3>{0, 2, 3}));
  EXPECT_EQ(first.provenance.completeEvidence(),
            REPLAY_VERTEX_PROVENANCE_KNOWN_EVIDENCE);
  EXPECT_EQ(second.provenance.completeEvidence(),
            REPLAY_VERTEX_PROVENANCE_KNOWN_EVIDENCE);
}

TEST(GpuModernReplayVulkanBakeTest,
     BakesProjectedSemiTransparentTriangleWithBoundedColorSnapshot) {
  DrawPacket packet = precisePacket(1, false, true);
  packet.vertices[0].x = 10.25;
  packet.vertices[0].y = 20.25;
  packet.vertices[1].x = 20.5;
  packet.vertices[1].y = 20.25;
  packet.vertices[2].x = 10.25;
  packet.vertices[2].y = 30.75;
  makeSemiTransparent(packet);

  const VulkanReplayBakeResult result = bakeModernReplayVulkan(
      makePlan(tileUpload(), packet), 4, feedbackFeatures());
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.commands.size(), 3u);
  const auto &draw =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[1].payload);
  EXPECT_EQ(draw.route, PacketRoute::Projected3D);
  EXPECT_EQ(draw.depthPath, VulkanReplayDrawDepthPath::InvalidateAcceptedFar);
  EXPECT_EQ(draw.feedbackPlanes,
            vulkanReplayFeedbackPlaneBit(VulkanReplayFeedbackPlane::Color));
  ASSERT_EQ(draw.segmentCount, 1u);
  const VulkanReplayBakedDrawSegment &segment = draw.segments[0];
  EXPECT_EQ(segment.firstVertex, 0u);
  EXPECT_EQ(segment.vertexCount, 3u);
  EXPECT_EQ(segment.destinationSnapshot,
            (VulkanReplayBakedRect{40, 80, 43, 44}));
  EXPECT_EQ(segment.uniforms.destinationState,
            (std::array<uint32_t, 4>{40, 80, 43, 44}));
  EXPECT_EQ(segment.snapshotAction,
            VulkanReplayDestinationSnapshotAction::Initial);
  EXPECT_EQ(draw.uniforms.destinationState, segment.uniforms.destinationState);
  EXPECT_EQ(draw.uniforms.textureState, segment.uniforms.textureState);
  EXPECT_EQ(draw.uniforms.depthScaleBias,
            (std::array<float, 4>{0.0f, 1.0f, 0.0f, 0.0f}));
}

TEST(GpuModernReplayVulkanBakeTest,
     ClassifiesOptInOpaqueProjectedDrawWithoutScheduledD32AsPainterOrdered) {
  ReplayPolicyState policy = replayPolicy();
  policy.trueDepthMode = ReplayDepthMode::Off;
  DrawPacket packet = precisePacket();
  packet.state.depth.testEnabled = false;
  packet.state.depth.writeEnabled = false;
  packet.state.depth.compare = DepthCompare::Always;

  VulkanReplayBakeFeatures features;
  features.allowPainterOrdered = true;
  const VulkanReplayBakeResult result =
      bakeModernReplayVulkan(makePlanWithPolicy(policy, packet), 4, features);
  ASSERT_TRUE(result.accepted());
  const auto &draw =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[0].payload);
  EXPECT_EQ(draw.depthPath, VulkanReplayDrawDepthPath::PainterOrdered);
  EXPECT_EQ(draw.feedbackPlanes, 0u);
  ASSERT_EQ(draw.segmentCount, 1u);
  EXPECT_EQ(draw.segments[0].snapshotAction,
            VulkanReplayDestinationSnapshotAction::None);
  EXPECT_EQ(draw.segments[0].destinationSnapshot, VulkanReplayBakedRect{});
}

TEST(GpuModernReplayVulkanBakeTest,
     SplitsFeedbackQuadAndRefreshesPerTriangleColorAndMaskSnapshots) {
  DrawPacket packet = precisePacket(1, true, false);
  const std::array<std::array<double, 2>, 4> positions{
      std::array<double, 2>{10.0, 10.0}, std::array<double, 2>{20.0, 10.0},
      std::array<double, 2>{10.0, 20.0}, std::array<double, 2>{50.0, 50.0}};
  for (std::size_t index = 0; index < positions.size(); ++index) {
    packet.vertices[index].x = positions[index][0];
    packet.vertices[index].y = positions[index][1];
  }
  makeSemiTransparent(packet);
  packet.state.mask.checkMaskBit = true;

  const VulkanReplayBakeResult result =
      bakeModernReplayVulkan(makePlan(packet), 4, feedbackFeatures());
  ASSERT_TRUE(result.accepted());
  const auto &draw =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[0].payload);
  EXPECT_EQ(draw.depthPath, VulkanReplayDrawDepthPath::InvalidateAcceptedFar);
  EXPECT_EQ(draw.feedbackPlanes,
            vulkanReplayFeedbackPlaneBit(VulkanReplayFeedbackPlane::Color) |
                vulkanReplayFeedbackPlaneBit(VulkanReplayFeedbackPlane::Mask));
  ASSERT_EQ(draw.segmentCount, 2u);
  EXPECT_EQ(draw.segments[0].firstVertex, 0u);
  EXPECT_EQ(draw.segments[0].vertexCount, 3u);
  EXPECT_EQ(draw.segments[0].destinationSnapshot,
            (VulkanReplayBakedRect{39, 39, 42, 42}));
  EXPECT_EQ(draw.segments[0].snapshotAction,
            VulkanReplayDestinationSnapshotAction::Initial);
  EXPECT_EQ(draw.segments[1].firstVertex, 3u);
  EXPECT_EQ(draw.segments[1].vertexCount, 3u);
  EXPECT_EQ(draw.segments[1].destinationSnapshot,
            (VulkanReplayBakedRect{39, 39, 162, 162}));
  EXPECT_EQ(draw.segments[1].uniforms.destinationState,
            (std::array<uint32_t, 4>{39, 39, 162, 162}));
  EXPECT_EQ(draw.segments[1].snapshotAction,
            VulkanReplayDestinationSnapshotAction::Refresh);
}

TEST(GpuModernReplayVulkanBakeTest,
     BakesSemiTransparentScreenQuadAsAcceptedFarDepthSegments) {
  DrawPacket packet = precisePacket(1, true, false);
  const std::array<NativePoint, 4> positions{
      NativePoint{100, 50}, NativePoint{200, 50}, NativePoint{100, 100},
      NativePoint{200, 100}};
  for (std::size_t index = 0; index < positions.size(); ++index) {
    packet.vertices[index].nativeX = positions[index].x;
    packet.vertices[index].nativeY = positions[index].y;
  }
  makeSemiTransparent(packet);
  makeScreen2D(packet);

  const VulkanReplayBakeResult result =
      bakeModernReplayVulkan(makePlan(packet), 4, feedbackFeatures());
  ASSERT_TRUE(result.accepted());
  const auto &draw =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[0].payload);
  EXPECT_EQ(draw.route, PacketRoute::Screen2D);
  EXPECT_EQ(draw.depthPath, VulkanReplayDrawDepthPath::InvalidateAcceptedFar);
  EXPECT_EQ(draw.feedbackPlanes,
            vulkanReplayFeedbackPlaneBit(VulkanReplayFeedbackPlane::Color));
  ASSERT_EQ(draw.segmentCount, 2u);
  EXPECT_EQ(draw.segments[0].destinationSnapshot,
            (VulkanReplayBakedRect{399, 199, 402, 202}));
  EXPECT_EQ(draw.segments[0].snapshotAction,
            VulkanReplayDestinationSnapshotAction::Initial);
  EXPECT_EQ(draw.segments[1].destinationSnapshot,
            (VulkanReplayBakedRect{399, 199, 402, 202}));
  EXPECT_EQ(draw.segments[1].snapshotAction,
            VulkanReplayDestinationSnapshotAction::Refresh);
  EXPECT_EQ(draw.uniforms.depthScaleBias,
            (std::array<float, 4>{0.0f, 1.0f, 0.0f, 0.0f}));
  for (const vulkan_abi::Vertex &vertex : result.batch.vertices)
    EXPECT_FLOAT_EQ(vertex.projectiveW,
                    static_cast<float>(CRASH_REPLAY_FALLBACK_W));
}

TEST(GpuModernReplayVulkanBakeTest,
     FeedbackBoundsArePaddedClippedAndSafeForExtremeFiniteCoordinates) {
  DrawPacket clipped = precisePacket(1);
  clipped.state.drawArea = {10, 20, 19, 29};
  clipped.vertices[0].x = -100.0;
  clipped.vertices[0].y = 0.0;
  clipped.vertices[1].x = 100.0;
  clipped.vertices[1].y = 0.0;
  clipped.vertices[2].x = 0.0;
  clipped.vertices[2].y = 100.0;
  makeSemiTransparent(clipped);

  DrawPacket outside = precisePacket(2);
  for (std::size_t index = 0; index < outside.vertexCount; ++index) {
    outside.vertices[index].x = 1.0e30;
    outside.vertices[index].y = 1.0e30;
  }
  makeSemiTransparent(outside);

  const VulkanReplayBakeResult result =
      bakeModernReplayVulkan(makePlan(clipped, outside), 4, feedbackFeatures());
  ASSERT_TRUE(result.accepted());
  const auto &first =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[0].payload);
  EXPECT_EQ(first.scissor, (VulkanReplayBakedRect{40, 80, 40, 40}));
  EXPECT_EQ(first.segments[0].destinationSnapshot, first.scissor);
  EXPECT_EQ(first.segments[0].uniforms.destinationState,
            (std::array<uint32_t, 4>{40, 80, 40, 40}));

  const auto &second =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[1].payload);
  EXPECT_EQ(second.segments[0].destinationSnapshot, VulkanReplayBakedRect{});
  EXPECT_EQ(second.segments[0].uniforms.destinationState,
            (std::array<uint32_t, 4>{0, 0, 0, 0}));
}

TEST(GpuModernReplayVulkanBakeTest,
     FeedbackAndDepthClassesRemainIndependentFailClosedOptIns) {
  const NativeTileUpload prefix = tileUpload();
  expectAtomicFailure(
      bakeModernReplayVulkan(makePlan(prefix, precisePacket()), 4),
      VulkanReplayBakeReason::PainterWriteRealDepthDisabled, 1);

  DrawPacket semi = precisePacket();
  makeSemiTransparent(semi);
  expectAtomicFailure(bakeModernReplayVulkan(makePlan(prefix, semi), 4),
                      VulkanReplayBakeReason::SemiTransparentDrawDisabled, 1);

  VulkanReplayBakeFeatures features;
  features.allowSemiTransparent = true;
  expectAtomicFailure(
      bakeModernReplayVulkan(makePlan(prefix, semi), 4, features),
      VulkanReplayBakeReason::DestinationColorFeedbackDisabled, 1);
  features.allowDestinationColorFeedback = true;
  expectAtomicFailure(
      bakeModernReplayVulkan(makePlan(prefix, semi), 4, features),
      VulkanReplayBakeReason::DepthInvalidateDisabled, 1);

  DrawPacket screen = precisePacket();
  makeScreen2D(screen);
  expectAtomicFailure(bakeModernReplayVulkan(makePlan(prefix, screen), 4),
                      VulkanReplayBakeReason::Screen2DDrawDisabled, 1);
  features = {};
  features.allowScreen2D = true;
  expectAtomicFailure(
      bakeModernReplayVulkan(makePlan(prefix, screen), 4, features),
      VulkanReplayBakeReason::DepthInvalidateDisabled, 1);

  DrawPacket checkMask = precisePacket();
  checkMask.state.mask.checkMaskBit = true;
  expectAtomicFailure(bakeModernReplayVulkan(makePlan(prefix, checkMask), 4),
                      VulkanReplayBakeReason::DestinationMaskReadDisabled, 1);
}

TEST(GpuModernReplayVulkanBakeTest,
     RejectsFeedbackPlanDriftTransactionallyBeforePublishingPayload) {
  DrawPacket packet = precisePacket();
  makeSemiTransparent(packet);

  ModernReplayExecutionPlan refreshPlan = makePlan(tileUpload(), packet);
  ReplayExecutionOperation &refreshItem =
      const_cast<ReplayExecutionOperation &>(refreshPlan.operations()[1]);
  ASSERT_FALSE(refreshItem.hazards.refreshDestinationBetweenTriangles);
  refreshItem.hazards.refreshDestinationBetweenTriangles = true;
  expectAtomicFailure(
      bakeModernReplayVulkan(refreshPlan, 4, feedbackFeatures()),
      VulkanReplayBakeReason::UnsupportedDrawState, 1);

  ModernReplayExecutionPlan accessPlan = makePlan(tileUpload(), packet);
  ReplayExecutionOperation &accessItem =
      const_cast<ReplayExecutionOperation &>(accessPlan.operations()[1]);
  ASSERT_TRUE(accessItem.access.colorRead);
  accessItem.access.colorRead = false;
  expectAtomicFailure(bakeModernReplayVulkan(accessPlan, 4, feedbackFeatures()),
                      VulkanReplayBakeReason::UnsupportedDrawState, 1);
}

TEST(GpuModernReplayVulkanBakeTest,
     SplitsWrappedNativeUploadIntoFourRegionsWithoutRepacking) {
  const NativeTileUpload prefix = tileUpload();
  const NativeTileUpload upload = tileUpload(NativeRect{1016, 504, 16, 16});
  VulkanReplayBakeResult result;
  {
    const ModernReplayExecutionPlan plan = makePlan(prefix, upload);
    result = bakeModernReplayVulkan(plan, 4);
  }
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.commands.size(), 3u);
  ASSERT_EQ(result.batch.nativeUploadPixels.size(),
            prefix.pixels.size() + upload.pixels.size());
  EXPECT_TRUE(std::equal(prefix.pixels.begin(), prefix.pixels.end(),
                         result.batch.nativeUploadPixels.begin()));
  EXPECT_TRUE(
      std::equal(upload.pixels.begin(), upload.pixels.end(),
                 result.batch.nativeUploadPixels.begin() +
                     static_cast<std::ptrdiff_t>(prefix.pixels.size())));
  const auto &baked =
      std::get<VulkanReplayBakedNativeUpload>(result.batch.commands[1].payload);
  ASSERT_EQ(baked.regionCount, 4u);
  EXPECT_EQ(baked.firstPixel, prefix.pixels.size());
  EXPECT_EQ(baked.regions[0].sourceWordOffset, 64u);
  EXPECT_EQ(baked.regions[0].destination,
            (VulkanReplayBakedRect{1016, 504, 8, 8}));
  EXPECT_EQ(baked.regions[1].sourceWordOffset, 72u);
  EXPECT_EQ(baked.regions[1].destination,
            (VulkanReplayBakedRect{0, 504, 8, 8}));
  EXPECT_EQ(baked.regions[2].sourceWordOffset, 192u);
  EXPECT_EQ(baked.regions[2].destination,
            (VulkanReplayBakedRect{1016, 0, 8, 8}));
  EXPECT_EQ(baked.regions[3].sourceWordOffset, 200u);
  EXPECT_EQ(baked.regions[3].destination, (VulkanReplayBakedRect{0, 0, 8, 8}));
  for (std::size_t index = 0; index < baked.regionCount; ++index) {
    EXPECT_EQ(baked.regions[index].bufferRowLength, 16u);
    EXPECT_EQ(baked.regions[index].bufferImageHeight, 16u);
  }
}

TEST(GpuModernReplayVulkanBakeTest,
     ExpandsAcceptedPaintPixelsIntoExactNativeAndTrueColorDrawPairs) {
  const PaintPixels paint = paintPixels();
  const ModernReplayExecutionPlan plan = makePlan(paint);
  VulkanReplayBakeFeatures features = feedbackFeatures();
  features.allowPaintPixels = true;

  const VulkanReplayBakeResult result =
      bakeModernReplayVulkan(plan, 4, features);
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.commands.size(), 5u);
  ASSERT_EQ(result.batch.vertices.size(), 6u);
  ASSERT_EQ(result.batch.visibilityOwners.size(), 2u);
  ASSERT_EQ(result.batch.nativeUploadPixels.size(), 3u);
  EXPECT_EQ(result.batch.nativeUploadPixels,
            (std::vector<uint16_t>{0x801fu, 0u, 0x03e0u}));

  constexpr uint32_t semi =
      replayDrawFlagBit(ReplayDrawFlag::SemiTransparent);
  constexpr uint32_t forceMask = replayDrawFlagBit(ReplayDrawFlag::ForceMask);
  constexpr uint32_t checkMask = replayDrawFlagBit(ReplayDrawFlag::CheckMask);
  constexpr auto colorFeedback = vulkanReplayFeedbackPlaneBit(
      VulkanReplayFeedbackPlane::Color);

  const auto &firstNative = std::get<VulkanReplayBakedNativeUpload>(
      result.batch.commands[0].payload);
  EXPECT_EQ(result.batch.commands[0].sourceOrdinal, 0u);
  EXPECT_EQ(firstNative.destination, (NativeRect{4, 5, 1, 1}));
  EXPECT_EQ(firstNative.firstPixel, 0u);
  EXPECT_EQ(firstNative.pixelCount, 1u);
  ASSERT_EQ(firstNative.regionCount, 1u);
  EXPECT_EQ(firstNative.regions[0].sourceWordOffset, 0u);
  EXPECT_EQ(firstNative.regions[0].destination,
            (VulkanReplayBakedRect{4, 5, 1, 1}));

  const auto &firstDraw =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[1].payload);
  EXPECT_EQ(result.batch.commands[1].sourceOrdinal, 0u);
  EXPECT_EQ(firstDraw.route, PacketRoute::Screen2D);
  EXPECT_EQ(firstDraw.depthPath,
            VulkanReplayDrawDepthPath::InvalidateAcceptedFar);
  EXPECT_EQ(firstDraw.scissor, (VulkanReplayBakedRect{16, 20, 4, 4}));
  EXPECT_EQ(firstDraw.feedbackPlanes, colorFeedback);
  EXPECT_EQ(firstDraw.segmentCount, 1u);
  EXPECT_EQ(firstDraw.segments[0].destinationSnapshot,
            (VulkanReplayBakedRect{16, 20, 4, 4}));
  EXPECT_EQ(firstDraw.segments[0].snapshotAction,
            VulkanReplayDestinationSnapshotAction::Initial);
  const uint32_t firstFlags = firstDraw.uniforms.textureState[2];
  EXPECT_NE(firstFlags & semi, 0u);
  EXPECT_NE(firstFlags & forceMask, 0u);
  EXPECT_EQ(firstFlags & checkMask, 0u)
      << "E6 acceptance must not be tested again by the async executor";
  EXPECT_EQ(firstDraw.uniforms.textureState[3], 2u);
  EXPECT_EQ(result.batch.vertices[0].positionNative,
            (std::array<float, 2>{4.0f, 5.0f}));
  EXPECT_EQ(result.batch.vertices[1].positionNative,
            (std::array<float, 2>{6.0f, 5.0f}));
  EXPECT_EQ(result.batch.vertices[2].positionNative,
            (std::array<float, 2>{4.0f, 7.0f}));
  EXPECT_EQ(result.batch.vertices[0].color255,
            (std::array<float, 3>{250.0f, 20.0f, 10.0f}));

  const auto &secondNative = std::get<VulkanReplayBakedNativeUpload>(
      result.batch.commands[2].payload);
  EXPECT_EQ(secondNative.firstPixel, 2u);
  EXPECT_EQ(secondNative.regions[0].sourceWordOffset, 2u);
  EXPECT_EQ(secondNative.regions[0].destination,
            (VulkanReplayBakedRect{9, 11, 1, 1}));
  const auto &secondDraw =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[3].payload);
  EXPECT_EQ(secondDraw.route, PacketRoute::Screen2D);
  EXPECT_EQ(secondDraw.scissor, (VulkanReplayBakedRect{36, 44, 4, 4}));
  EXPECT_EQ(secondDraw.feedbackPlanes, 0u)
      << "a non-STP texel in a textured semi primitive remains opaque";
  EXPECT_EQ(secondDraw.uniforms.textureState[2] &
                (semi | forceMask | checkMask),
            0u);
  EXPECT_EQ(result.batch.commands[4].sourceOrdinal, 1u);
  EXPECT_TRUE(std::holds_alternative<VulkanReplayBakedLatch>(
      result.batch.commands[4].payload));

  EXPECT_EQ(firstDraw.visibilityOwnerTokens[0], 1u);
  EXPECT_EQ(secondDraw.visibilityOwnerTokens[0], 2u);
  EXPECT_EQ(result.batch.visibilityOwners[0].sourceOrdinal, 0u);
  EXPECT_EQ(result.batch.visibilityOwners[1].sourceOrdinal, 0u);
  EXPECT_EQ(result.batch.visibilityOwners[0].triangleIndex, 0u);
  EXPECT_EQ(result.batch.visibilityOwners[1].triangleIndex, 0u);
  EXPECT_EQ(result.batch.visibilityOwners[0].route, PacketRoute::Screen2D);
}

TEST(GpuModernReplayVulkanBakeTest,
     PaintPixelsRequireExplicitCompleteBackendAndRejectMalformedPayloads) {
  const ModernReplayExecutionPlan plan = makePlan(paintPixels());
  expectAtomicFailure(bakeModernReplayVulkan(plan, 4, feedbackFeatures()),
                      VulkanReplayBakeReason::PaintPixelsDisabled, 0);

  VulkanReplayBakeFeatures features;
  features.allowPaintPixels = true;
  expectAtomicFailure(bakeModernReplayVulkan(plan, 4, features),
                      VulkanReplayBakeReason::Screen2DDrawDisabled, 0);

  features = feedbackFeatures();
  features.allowPaintPixels = true;
  features.allowDestinationColorFeedback = false;
  expectAtomicFailure(
      bakeModernReplayVulkan(plan, 4, features),
      VulkanReplayBakeReason::DestinationColorFeedbackDisabled, 0);

  ModernReplayExecutionPlan invalidMaskPlan = makePlan(paintPixels());
  firstMutablePaintPixels(invalidMaskPlan).pixels[0].nativeColor &= 0x7fffu;
  features.allowDestinationColorFeedback = true;
  expectAtomicFailure(bakeModernReplayVulkan(invalidMaskPlan, 4, features),
                      VulkanReplayBakeReason::InvalidPaintPixels, 0);

  ModernReplayExecutionPlan invalidCoordinatePlan = makePlan(paintPixels());
  firstMutablePaintPixels(invalidCoordinatePlan).pixels[0].destination.x =
      1024;
  expectAtomicFailure(
      bakeModernReplayVulkan(invalidCoordinatePlan, 4, features),
      VulkanReplayBakeReason::InvalidPaintPixels, 0);
}

TEST(GpuModernReplayVulkanBakeTest,
     PaintPixelsDepthEpochMustMatchTheOrderedSurface) {
  ClearDepth clear;
  clear.nextEpoch = 8;
  const ModernReplayExecutionPlan plan = makePlan(clear, paintPixels());
  VulkanReplayBakeFeatures features = feedbackFeatures();
  features.allowPaintPixels = true;
  expectAtomicFailure(bakeModernReplayVulkan(plan, 4, features),
                      VulkanReplayBakeReason::InvalidDepthState, 1);
}

TEST(GpuModernReplayVulkanBakeTest,
     BakesForcedCoalescedPaintRectsAndAlignsEverySharedNativePayload) {
  PaintRect coalesced = paintRect(NativeRect{5, 7, 3, 1});
  coalesced.mask.forceMaskBit = true;
  PaintRect point = paintRect(NativeRect{20, 9, 1, 1});
  point.nativeColor = 0x001f;
  point.color = {255, 0, 0};
  const NativeTileUpload upload = tileUpload();
  const ModernReplayExecutionPlan plan = makePlan(coalesced, point, upload);

  const VulkanReplayBakeResult result =
      bakeModernReplayVulkan(plan, 4, painterWriteFeatures());
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.commands.size(), 4u);
  ASSERT_EQ(result.batch.nativeUploadPixels.size(), 70u);

  const auto &first =
      std::get<VulkanReplayBakedPaintRect>(result.batch.commands[0].payload);
  EXPECT_EQ(first.atlasArea, (VulkanReplayBakedRect{20, 28, 12, 4}));
  EXPECT_EQ(first.nativeRegion.sourceWordOffset, 0u);
  EXPECT_EQ(first.nativeRegion.bufferRowLength, 3u);
  EXPECT_EQ(first.nativeRegion.bufferImageHeight, 1u);
  EXPECT_EQ(first.nativeRegion.destination,
            (VulkanReplayBakedRect{5, 7, 3, 1}));
  EXPECT_EQ(first.color, coalesced.color);
  EXPECT_EQ(first.maskValue, 1u);
  EXPECT_EQ(first.depthEpoch, 7u);
  EXPECT_FLOAT_EQ(first.depthClearValue, 1.0f);
  EXPECT_EQ(first.coalescedCommandCount, 3u);

  const auto &second =
      std::get<VulkanReplayBakedPaintRect>(result.batch.commands[1].payload);
  EXPECT_EQ(second.atlasArea, (VulkanReplayBakedRect{80, 36, 4, 4}));
  EXPECT_EQ(second.nativeRegion.sourceWordOffset, 4u);
  EXPECT_EQ(second.nativeRegion.destination,
            (VulkanReplayBakedRect{20, 9, 1, 1}));
  EXPECT_EQ(second.color, point.color);
  EXPECT_EQ(second.maskValue, 0u);

  const auto &third =
      std::get<VulkanReplayBakedNativeUpload>(result.batch.commands[2].payload);
  EXPECT_EQ(third.firstPixel, 6u);
  ASSERT_EQ(third.regionCount, 1u);
  EXPECT_EQ(third.regions[0].sourceWordOffset, 6u);

  EXPECT_EQ(result.batch.nativeUploadPixels[0], 0x9234u);
  EXPECT_EQ(result.batch.nativeUploadPixels[1], 0x9234u);
  EXPECT_EQ(result.batch.nativeUploadPixels[2], 0x9234u);
  EXPECT_EQ(result.batch.nativeUploadPixels[3], 0u);
  EXPECT_EQ(result.batch.nativeUploadPixels[4], 0x001fu);
  EXPECT_EQ(result.batch.nativeUploadPixels[5], 0u);
  EXPECT_TRUE(std::equal(upload.pixels.begin(), upload.pixels.end(),
                         result.batch.nativeUploadPixels.begin() + 6));

  EXPECT_EQ((first.nativeRegion.sourceWordOffset * sizeof(uint16_t)) % 4u, 0u);
  EXPECT_EQ((second.nativeRegion.sourceWordOffset * sizeof(uint16_t)) % 4u, 0u);
  EXPECT_EQ((third.regions[0].sourceWordOffset * sizeof(uint16_t)) % 4u, 0u);
}

TEST(GpuModernReplayVulkanBakeTest,
     RejectsWrappedMalformedAndCheckMaskPaintRectsAtomically) {
  PaintRect checkMask = paintRect();
  checkMask.mask.checkMaskBit = true;
  expectAtomicFailure(
      bakeModernReplayVulkan(makePlan(tileUpload(), checkMask), 4),
      VulkanReplayBakeReason::UnsupportedPaintRectState, 1);

  ModernReplayExecutionPlan wrappedPlan =
      makePlan(paintRect(NativeRect{1022, 10, 2, 1}));
  PaintRect &wrapped = firstMutablePaintRect(wrappedPlan);
  wrapped.destination.x = 1023;
  expectAtomicFailure(bakeModernReplayVulkan(wrappedPlan, 4),
                      VulkanReplayBakeReason::InvalidPaintRect, 0);

  ModernReplayExecutionPlan countPlan =
      makePlan(paintRect(NativeRect{10, 10, 3, 1}));
  firstMutablePaintRect(countPlan).coalescedCommandCount = 2;
  expectAtomicFailure(bakeModernReplayVulkan(countPlan, 4),
                      VulkanReplayBakeReason::InvalidPaintRect, 0);
}

TEST(GpuModernReplayVulkanBakeTest,
     RejectsPaintRectWhoseDepthEpochDisagreesWithOrderedClear) {
  ClearDepth clear;
  clear.nextEpoch = 8;
  const ModernReplayExecutionPlan plan = makePlan(clear, paintRect());
  expectAtomicFailure(bakeModernReplayVulkan(plan, 4),
                      VulkanReplayBakeReason::InvalidDepthState, 1);
}

TEST(GpuModernReplayVulkanBakeTest,
     SplitsWrappedFillIntoFourNativeAndScaledAtlasRegions) {
  const FillRect fill = fillRect(NativeRect{1008, 510, 32, 4});
  const ModernReplayExecutionPlan plan = makePlan(fill);

  const VulkanReplayBakeResult result =
      bakeModernReplayVulkan(plan, 4, painterWriteFeatures());
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.commands.size(), 2u);
  ASSERT_EQ(result.batch.nativeUploadPixels.size(), 128u);
  EXPECT_TRUE(
      std::all_of(result.batch.nativeUploadPixels.begin(),
                  result.batch.nativeUploadPixels.end(),
                  [&](uint16_t pixel) { return pixel == fill.nativeColor; }));

  const auto &baked =
      std::get<VulkanReplayBakedFillRect>(result.batch.commands[0].payload);
  ASSERT_EQ(baked.regionCount, 4u);
  EXPECT_EQ(baked.color, (Rgb8{18, 52, 86}));
  EXPECT_EQ(baked.maskValue, 0u);
  EXPECT_FLOAT_EQ(baked.depthClearValue, 1.0f);

  const std::array<VulkanReplayBakedRect, 4> nativeAreas{
      VulkanReplayBakedRect{1008, 510, 16, 2},
      VulkanReplayBakedRect{0, 510, 16, 2},
      VulkanReplayBakedRect{1008, 0, 16, 2},
      VulkanReplayBakedRect{0, 0, 16, 2}};
  const std::array<VulkanReplayBakedRect, 4> atlasAreas{
      VulkanReplayBakedRect{4032, 2040, 64, 8},
      VulkanReplayBakedRect{0, 2040, 64, 8},
      VulkanReplayBakedRect{4032, 0, 64, 8},
      VulkanReplayBakedRect{0, 0, 64, 8}};
  for (std::size_t index = 0; index < baked.regionCount; ++index) {
    const VulkanReplayBakedFillRect::Region &region = baked.regions[index];
    EXPECT_EQ(region.nativeRegion.sourceWordOffset, index * 32u);
    EXPECT_EQ(region.nativeRegion.bufferRowLength, 16u);
    EXPECT_EQ(region.nativeRegion.bufferImageHeight, 2u);
    EXPECT_EQ(region.nativeRegion.destination, nativeAreas[index]);
    EXPECT_EQ(region.atlasArea, atlasAreas[index]);
    EXPECT_EQ((region.nativeRegion.sourceWordOffset * sizeof(uint16_t)) % 4u,
              0u);
  }
}

TEST(GpuModernReplayVulkanBakeTest,
     PreservesUploadPaintAndFillOrderWithAlignedOddWrappedSegments) {
  const NativeTileUpload upload = tileUpload();
  const PaintRect paint = paintRect(NativeRect{30, 31, 3, 1});
  const FillRect fill = fillRect(NativeRect{1023, 511, 3, 2});
  const ModernReplayExecutionPlan plan = makePlan(upload, paint, fill);

  const VulkanReplayBakeResult result =
      bakeModernReplayVulkan(plan, 4, painterWriteFeatures());
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.commands.size(), 4u);
  EXPECT_TRUE(std::holds_alternative<VulkanReplayBakedNativeUpload>(
      result.batch.commands[0].payload));
  EXPECT_TRUE(std::holds_alternative<VulkanReplayBakedPaintRect>(
      result.batch.commands[1].payload));
  EXPECT_TRUE(std::holds_alternative<VulkanReplayBakedFillRect>(
      result.batch.commands[2].payload));
  EXPECT_TRUE(std::holds_alternative<VulkanReplayBakedLatch>(
      result.batch.commands[3].payload));
  EXPECT_EQ(result.batch.commands[0].sourceOrdinal, 0u);
  EXPECT_EQ(result.batch.commands[1].sourceOrdinal, 1u);
  EXPECT_EQ(result.batch.commands[2].sourceOrdinal, 2u);

  const auto &bakedPaint =
      std::get<VulkanReplayBakedPaintRect>(result.batch.commands[1].payload);
  EXPECT_EQ(bakedPaint.nativeRegion.sourceWordOffset, 64u);
  const auto &bakedFill =
      std::get<VulkanReplayBakedFillRect>(result.batch.commands[2].payload);
  ASSERT_EQ(bakedFill.regionCount, 4u);
  const std::array<std::size_t, 4> expectedOffsets{68, 70, 72, 74};
  const std::array<VulkanReplayBakedRect, 4> expectedNative{
      VulkanReplayBakedRect{1023, 511, 1, 1},
      VulkanReplayBakedRect{0, 511, 2, 1}, VulkanReplayBakedRect{1023, 0, 1, 1},
      VulkanReplayBakedRect{0, 0, 2, 1}};
  for (std::size_t index = 0; index < bakedFill.regionCount; ++index) {
    EXPECT_EQ(bakedFill.regions[index].nativeRegion.sourceWordOffset,
              expectedOffsets[index]);
    EXPECT_EQ(bakedFill.regions[index].nativeRegion.destination,
              expectedNative[index]);
    EXPECT_EQ((expectedOffsets[index] * sizeof(uint16_t)) % 4u, 0u);
  }
  ASSERT_EQ(result.batch.nativeUploadPixels.size(), 76u);
  EXPECT_EQ(result.batch.nativeUploadPixels[67], 0u);
  EXPECT_EQ(result.batch.nativeUploadPixels[69], 0u);
  EXPECT_EQ(result.batch.nativeUploadPixels[73], 0u);
  for (const std::size_t offset : expectedOffsets)
    EXPECT_EQ(result.batch.nativeUploadPixels[offset], fill.nativeColor);
}

TEST(GpuModernReplayVulkanBakeTest,
     FillExtremeOriginsWrapWithoutOverflowAndMalformedExtentsFailAtomically) {
  const FillRect extreme =
      fillRect(NativeRect{std::numeric_limits<int32_t>::max(),
                          std::numeric_limits<int32_t>::min(), 1, 1});
  const VulkanReplayBakeResult extremeResult =
      bakeModernReplayVulkan(makePlan(extreme), 4);
  ASSERT_TRUE(extremeResult.accepted());
  const auto &extremeFill = std::get<VulkanReplayBakedFillRect>(
      extremeResult.batch.commands[0].payload);
  ASSERT_EQ(extremeFill.regionCount, 1u);
  EXPECT_EQ(extremeFill.regions[0].nativeRegion.destination,
            (VulkanReplayBakedRect{1023, 0, 1, 1}));
  EXPECT_EQ(extremeFill.regions[0].atlasArea,
            (VulkanReplayBakedRect{4092, 0, 4, 4}));

  ModernReplayExecutionPlan oversizedPlan = makePlan(fillRect());
  FillRect &oversized = firstMutableFillRect(oversizedPlan);
  oversized.destination.width = std::numeric_limits<uint32_t>::max();
  oversized.destination.height = std::numeric_limits<uint32_t>::max();
  const VulkanReplayBakeResult oversizedResult =
      bakeModernReplayVulkan(oversizedPlan, 4);
  EXPECT_TRUE(
      oversizedResult.reason == VulkanReplayBakeReason::InvalidFillRect ||
      oversizedResult.reason == VulkanReplayBakeReason::NumericOverflow);
  EXPECT_EQ(oversizedResult.sourceOrdinal, 0u);
  EXPECT_TRUE(oversizedResult.batch.empty());
}

TEST(GpuModernReplayVulkanBakeTest,
     RejectsFillMaskBitAndInconsistentTrueColorAtomically) {
  ModernReplayExecutionPlan maskPlan = makePlan(fillRect());
  firstMutableFillRect(maskPlan).nativeColor |= 0x8000u;
  expectAtomicFailure(bakeModernReplayVulkan(maskPlan, 4),
                      VulkanReplayBakeReason::UnsupportedFillRectState, 0);

  ModernReplayExecutionPlan colorPlan = makePlan(fillRect());
  firstMutableFillRect(colorPlan).nativeColor ^= 1u;
  expectAtomicFailure(bakeModernReplayVulkan(colorPlan, 4),
                      VulkanReplayBakeReason::InvalidFillRect, 0);
}

TEST(GpuModernReplayVulkanBakeTest,
     BakesCrashDoubleBufferCopiesAsExplicitDirectNonOverlapPaths) {
  const CopyRect left = copyRect();
  const CopyRect right = copyRect({0, 264}, {512, 0});
  const ModernReplayExecutionPlan plan = makePlan(left, right);
  ASSERT_EQ(plan.operations().size(), 3u);
  EXPECT_TRUE(plan.operations()[0].hazards.copySourceScratchRequired);
  EXPECT_TRUE(plan.operations()[1].hazards.copySourceScratchRequired);

  const VulkanReplayBakeResult result =
      bakeModernReplayVulkan(plan, 4, painterWriteFeatures());
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.commands.size(), 3u);
  EXPECT_TRUE(result.batch.vertices.empty());
  EXPECT_TRUE(result.batch.nativeUploadPixels.empty());

  const auto &first =
      std::get<VulkanReplayBakedCopyRect>(result.batch.commands[0].payload);
  EXPECT_EQ(first.path, VulkanReplayCopyPath::DirectNonOverlap);
  EXPECT_EQ(first.sourceNative, (VulkanReplayBakedRect{0, 264, 512, 240}));
  EXPECT_EQ(first.destinationNative, (VulkanReplayBakedRect{0, 0, 512, 240}));
  EXPECT_EQ(first.sourceAtlas, (VulkanReplayBakedRect{0, 1056, 2048, 960}));
  EXPECT_EQ(first.destinationAtlas, (VulkanReplayBakedRect{0, 0, 2048, 960}));
  EXPECT_TRUE(first.copyNativeR16);
  EXPECT_TRUE(first.copyColorRgba8);
  EXPECT_TRUE(first.copyMaskR8);
  EXPECT_TRUE(first.invalidateDepthD32);
  EXPECT_FLOAT_EQ(first.depthClearValue, 1.0f);
  EXPECT_EQ(first.regionCount, 0u);
  EXPECT_FALSE(first.snapshotSourceNativeR16);
  EXPECT_FALSE(first.snapshotSourceColorRgba8);
  EXPECT_FALSE(first.snapshotSourceMaskR8);
  EXPECT_FALSE(first.snapshotDestinationMaskR8);
  EXPECT_EQ(first.nativeRead, NativeReadDependency{});
  EXPECT_EQ(first.mask, MaskState{});

  const auto &second =
      std::get<VulkanReplayBakedCopyRect>(result.batch.commands[1].payload);
  EXPECT_EQ(second.path, VulkanReplayCopyPath::DirectNonOverlap);
  EXPECT_EQ(second.sourceNative, first.sourceNative);
  EXPECT_EQ(second.destinationNative,
            (VulkanReplayBakedRect{512, 0, 512, 240}));
  EXPECT_EQ(second.sourceAtlas, first.sourceAtlas);
  EXPECT_EQ(second.destinationAtlas,
            (VulkanReplayBakedRect{2048, 0, 2048, 960}));
  EXPECT_TRUE(std::holds_alternative<VulkanReplayBakedLatch>(
      result.batch.commands[2].payload));
}

TEST(GpuModernReplayVulkanBakeTest,
     CopyOverlapWrapAndMaskSemanticsRequireOptInScratchPathsAtomically) {
  const NativeTileUpload upload = tileUpload();
  expectAtomicFailure(
      bakeModernReplayVulkan(
          makePlan(upload, copyRect({0, 264}, {0, 300}, 512, 200)), 4),
      VulkanReplayBakeReason::CopyRectOrderedOverlapUnproven, 1);
  expectAtomicFailure(
      bakeModernReplayVulkan(
          makePlan(upload, copyRect({1000, 500}, {900, 450}, 200, 100)), 4,
          copyRectScratchFeatures()),
      VulkanReplayBakeReason::CopyRectOrderedOverlapUnproven, 1);
  expectAtomicFailure(
      bakeModernReplayVulkan(
          makePlan(upload, copyRect({800, 264}, {0, 0}, 512, 240)), 4),
      VulkanReplayBakeReason::CopyRectScratchRequired, 1);
  expectAtomicFailure(
      bakeModernReplayVulkan(
          makePlan(upload, copyRect({0, 264}, {800, 0}, 512, 240)), 4),
      VulkanReplayBakeReason::CopyRectScratchRequired, 1);

  CopyRect checkMask = copyRect();
  checkMask.mask.checkMaskBit = true;
  expectAtomicFailure(bakeModernReplayVulkan(makePlan(upload, checkMask), 4),
                      VulkanReplayBakeReason::CopyRectScratchRequired, 1);

  CopyRect forceMask = copyRect();
  forceMask.mask.forceMaskBit = true;
  expectAtomicFailure(bakeModernReplayVulkan(makePlan(upload, forceMask), 4),
                      VulkanReplayBakeReason::CopyRectScratchRequired, 1);
}

TEST(GpuModernReplayVulkanBakeTest,
     BakesGeneralCopyAsDeterministicSixRegionSnapshotWithMaskProvenance) {
  CopyRect copy = copyRect({1000, 500}, {900, 500}, 200, 100);
  copy.mask = MaskState{true, true};
  const VulkanReplayBakeResult result =
      bakeModernReplayVulkan(makePlan(copy), 4, copyRectScratchFeatures());
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.commands.size(), 2u);
  EXPECT_TRUE(result.batch.vertices.empty());
  EXPECT_TRUE(result.batch.nativeUploadPixels.empty());

  const auto &baked =
      std::get<VulkanReplayBakedCopyRect>(result.batch.commands[0].payload);
  EXPECT_EQ(baked.path, VulkanReplayCopyPath::ScratchSnapshot);
  EXPECT_EQ(baked.sourceNative, VulkanReplayBakedRect{});
  EXPECT_EQ(baked.destinationNative, VulkanReplayBakedRect{});
  EXPECT_EQ(baked.sourceAtlas, VulkanReplayBakedRect{});
  EXPECT_EQ(baked.destinationAtlas, VulkanReplayBakedRect{});
  EXPECT_EQ(baked.nativeRead, copy.nativeRead);
  EXPECT_EQ(baked.mask, copy.mask);
  EXPECT_EQ(baked.scratchNativeExtent, (VulkanReplayBakedRect{0, 0, 200, 100}));
  EXPECT_EQ(baked.scratchAtlasExtent, (VulkanReplayBakedRect{0, 0, 800, 400}));
  EXPECT_FALSE(baked.snapshotSourceNativeR16);
  EXPECT_TRUE(baked.snapshotSourceColorRgba8);
  EXPECT_TRUE(baked.snapshotSourceMaskR8);
  EXPECT_TRUE(baked.snapshotDestinationMaskR8);
  EXPECT_FALSE(baked.copyNativeR16);
  EXPECT_TRUE(baked.copyColorRgba8);
  EXPECT_TRUE(baked.copyMaskR8);
  EXPECT_TRUE(baked.invalidateDepthD32);
  EXPECT_FLOAT_EQ(baked.depthClearValue, 1.0f);
  ASSERT_EQ(baked.regionCount, 6u);

  struct ExpectedRegion {
    VulkanReplayBakedRect scratch;
    VulkanReplayBakedRect source;
    VulkanReplayBakedRect destination;
  };
  const std::array<ExpectedRegion, 6> expected{{
      {{0, 0, 24, 12}, {1000, 500, 24, 12}, {900, 500, 24, 12}},
      {{24, 0, 100, 12}, {0, 500, 100, 12}, {924, 500, 100, 12}},
      {{124, 0, 76, 12}, {100, 500, 76, 12}, {0, 500, 76, 12}},
      {{0, 12, 24, 88}, {1000, 0, 24, 88}, {900, 0, 24, 88}},
      {{24, 12, 100, 88}, {0, 0, 100, 88}, {924, 0, 100, 88}},
      {{124, 12, 76, 88}, {100, 0, 76, 88}, {0, 0, 76, 88}},
  }};
  const auto scaled = [](VulkanReplayBakedRect rect) {
    rect.x *= 4;
    rect.y *= 4;
    rect.width *= 4;
    rect.height *= 4;
    return rect;
  };
  uint64_t coveredPixels = 0;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const VulkanReplayBakedCopyRegion &region = baked.regions[index];
    EXPECT_EQ(region.scratchNative, expected[index].scratch);
    EXPECT_EQ(region.sourceNative, expected[index].source);
    EXPECT_EQ(region.destinationNative, expected[index].destination);
    EXPECT_EQ(region.scratchAtlas, scaled(expected[index].scratch));
    EXPECT_EQ(region.sourceAtlas, scaled(expected[index].source));
    EXPECT_EQ(region.destinationAtlas, scaled(expected[index].destination));
    EXPECT_LE(region.sourceNative.x + region.sourceNative.width, 1024u);
    EXPECT_LE(region.sourceNative.y + region.sourceNative.height, 512u);
    EXPECT_LE(region.destinationNative.x + region.destinationNative.width,
              1024u);
    EXPECT_LE(region.destinationNative.y + region.destinationNative.height,
              512u);
    coveredPixels += static_cast<uint64_t>(region.scratchNative.width) *
                     region.scratchNative.height;
  }
  EXPECT_EQ(coveredPixels, UINT64_C(20000));
}

TEST(GpuModernReplayVulkanBakeTest,
     BakesOverlapAndIndividualE6ModesOnlyThroughScratchSnapshot) {
  const auto bake = [](CopyRect copy) {
    return bakeModernReplayVulkan(makePlan(copy), 4, copyRectScratchFeatures());
  };

  const VulkanReplayBakeResult overlapResult =
      bake(copyRect({100, 100}, {104, 100}, 32, 8));
  ASSERT_TRUE(overlapResult.accepted());
  const auto &overlap = std::get<VulkanReplayBakedCopyRect>(
      overlapResult.batch.commands[0].payload);
  EXPECT_EQ(overlap.path, VulkanReplayCopyPath::ScratchSnapshot);
  ASSERT_EQ(overlap.regionCount, 1u);
  EXPECT_EQ(overlap.regions[0].scratchNative,
            (VulkanReplayBakedRect{0, 0, 32, 8}));
  EXPECT_EQ(overlap.regions[0].sourceNative,
            (VulkanReplayBakedRect{100, 100, 32, 8}));
  EXPECT_EQ(overlap.regions[0].destinationNative,
            (VulkanReplayBakedRect{104, 100, 32, 8}));
  EXPECT_FALSE(overlap.snapshotDestinationMaskR8);

  CopyRect force = copyRect();
  force.mask.forceMaskBit = true;
  const VulkanReplayBakeResult forceResult = bake(force);
  ASSERT_TRUE(forceResult.accepted());
  const auto &forceBaked = std::get<VulkanReplayBakedCopyRect>(
      forceResult.batch.commands[0].payload);
  EXPECT_EQ(forceBaked.path, VulkanReplayCopyPath::ScratchSnapshot);
  EXPECT_TRUE(forceBaked.mask.forceMaskBit);
  EXPECT_FALSE(forceBaked.mask.checkMaskBit);
  EXPECT_FALSE(forceBaked.snapshotDestinationMaskR8);

  CopyRect check = copyRect();
  check.mask.checkMaskBit = true;
  const VulkanReplayBakeResult checkResult = bake(check);
  ASSERT_TRUE(checkResult.accepted());
  const auto &checkBaked = std::get<VulkanReplayBakedCopyRect>(
      checkResult.batch.commands[0].payload);
  EXPECT_EQ(checkBaked.path, VulkanReplayCopyPath::ScratchSnapshot);
  EXPECT_FALSE(checkBaked.mask.forceMaskBit);
  EXPECT_TRUE(checkBaked.mask.checkMaskBit);
  EXPECT_TRUE(checkBaked.snapshotDestinationMaskR8);
}

TEST(GpuModernReplayVulkanBakeTest,
     CopyDirectPathRejectsUnexpectedPlanHazardsAndMalformedExtents) {
  ModernReplayExecutionPlan hazardPlan = makePlan(copyRect());
  ReplayExecutionOperation &hazardItem =
      const_cast<ReplayExecutionOperation &>(hazardPlan.operations().front());
  ASSERT_TRUE(hazardItem.hazards.copySourceScratchRequired);
  hazardItem.hazards.destinationScratchRequired = true;
  expectAtomicFailure(bakeModernReplayVulkan(hazardPlan, 4),
                      VulkanReplayBakeReason::UnsupportedCopyRectState, 0);

  ModernReplayExecutionPlan accessPlan = makePlan(copyRect());
  ReplayExecutionOperation &accessItem =
      const_cast<ReplayExecutionOperation &>(accessPlan.operations().front());
  accessItem.access.maskRead = false;
  expectAtomicFailure(bakeModernReplayVulkan(accessPlan, 4),
                      VulkanReplayBakeReason::UnsupportedCopyRectState, 0);

  ModernReplayExecutionPlan missingSourceScratchPlan =
      makePlan(copyRect({100, 100}, {104, 100}, 32, 8));
  ReplayExecutionOperation &missingSourceScratchItem =
      const_cast<ReplayExecutionOperation &>(
          missingSourceScratchPlan.operations().front());
  missingSourceScratchItem.hazards.copySourceScratchRequired = false;
  expectAtomicFailure(bakeModernReplayVulkan(missingSourceScratchPlan, 4,
                                             copyRectScratchFeatures()),
                      VulkanReplayBakeReason::UnsupportedCopyRectState, 0);

  CopyRect checkedCopy = copyRect();
  checkedCopy.mask.checkMaskBit = true;
  ModernReplayExecutionPlan missingDestinationScratchPlan =
      makePlan(checkedCopy);
  ReplayExecutionOperation &missingDestinationScratchItem =
      const_cast<ReplayExecutionOperation &>(
          missingDestinationScratchPlan.operations().front());
  missingDestinationScratchItem.hazards.destinationScratchRequired = false;
  expectAtomicFailure(bakeModernReplayVulkan(missingDestinationScratchPlan, 4,
                                             copyRectScratchFeatures()),
                      VulkanReplayBakeReason::UnsupportedCopyRectState, 0);

  ModernReplayExecutionPlan proofPlan = makePlan(copyRect());
  --firstMutableCopyRect(proofPlan).nativeRead.tileCount;
  expectAtomicFailure(bakeModernReplayVulkan(proofPlan, 4),
                      VulkanReplayBakeReason::UnsupportedCopyRectState, 0);

  ModernReplayExecutionPlan emptyPlan = makePlan(copyRect());
  firstMutableCopyRect(emptyPlan).width = 0;
  expectAtomicFailure(bakeModernReplayVulkan(emptyPlan, 4),
                      VulkanReplayBakeReason::InvalidCopyRect, 0);

  ModernReplayExecutionPlan oversizedPlan = makePlan(copyRect());
  firstMutableCopyRect(oversizedPlan).width =
      std::numeric_limits<uint32_t>::max();
  expectAtomicFailure(bakeModernReplayVulkan(oversizedPlan, 4),
                      VulkanReplayBakeReason::InvalidCopyRect, 0);

  const CopyRect extreme =
      copyRect({std::numeric_limits<int32_t>::max(), 0}, {0, 0}, 1, 1);
  expectAtomicFailure(bakeModernReplayVulkan(makePlan(extreme), 4),
                      VulkanReplayBakeReason::CopyRectScratchRequired, 0);
}

TEST(GpuModernReplayVulkanBakeTest,
     BakesAcceptedA0PixelsAsOrderedMaximalHorizontalRuns) {
  const NativeRect transfer{1022, 511, 6, 2};
  const MaskState mask{true, true};
  const CpuUpload begin =
      cpuUpload(88, CpuUploadPhase::Begin, transfer, 0, {}, {}, mask);
  const CpuUpload rejectedHead =
      cpuUpload(88, CpuUploadPhase::Data, transfer, 0, {0x1111}, {0}, mask);
  const CpuUpload data = cpuUpload(88, CpuUploadPhase::Data, transfer, 1,
                                   {0x0001, 0x0002, 0x0003, 0x0004, 0x0005,
                                    0x0006, 0x0007, 0x0008, 0x0009, 0x000A},
                                   {0x1F7}, mask);
  const CpuUpload rejectedTail =
      cpuUpload(88, CpuUploadPhase::Data, transfer, 11, {0x2222}, {0}, mask);
  const CpuUpload complete =
      cpuUpload(88, CpuUploadPhase::Complete, transfer, 12, {}, {}, mask);

  const VulkanReplayBakeResult result = bakeModernReplayVulkan(
      makePlan(paintRect(), begin, rejectedHead, data, rejectedTail, complete),
      4, cpuUploadFeatures());
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.commands.size(), 7u);
  EXPECT_EQ(result.batch.commands[1].sourceOrdinal, 1u);
  EXPECT_EQ(result.batch.commands[2].sourceOrdinal, 2u);
  EXPECT_EQ(result.batch.commands[3].sourceOrdinal, 3u);
  EXPECT_EQ(result.batch.commands[4].sourceOrdinal, 4u);
  EXPECT_EQ(result.batch.commands[5].sourceOrdinal, 5u);

  const auto &bakedBegin =
      std::get<VulkanReplayBakedCpuUpload>(result.batch.commands[1].payload);
  EXPECT_EQ(bakedBegin.transactionId, 88u);
  EXPECT_EQ(bakedBegin.phase, CpuUploadPhase::Begin);
  EXPECT_EQ(bakedBegin.transfer, transfer);
  EXPECT_TRUE(bakedBegin.runs.empty());

  const auto &bakedRejectedHead =
      std::get<VulkanReplayBakedCpuUpload>(result.batch.commands[2].payload);
  EXPECT_EQ(bakedRejectedHead.phase, CpuUploadPhase::Data);
  EXPECT_EQ(bakedRejectedHead.pixelOffset, 0u);
  EXPECT_EQ(bakedRejectedHead.consumedPixelCount, 1u);
  EXPECT_EQ(bakedRejectedHead.acceptedPixelCount, 0u);
  EXPECT_EQ(bakedRejectedHead.rejectedPixelCount, 1u);
  EXPECT_TRUE(bakedRejectedHead.runs.empty());

  const auto &bakedData =
      std::get<VulkanReplayBakedCpuUpload>(result.batch.commands[3].payload);
  EXPECT_EQ(bakedData.phase, CpuUploadPhase::Data);
  EXPECT_EQ(bakedData.pixelOffset, 1u);
  EXPECT_EQ(bakedData.consumedPixelCount, 10u);
  EXPECT_EQ(bakedData.acceptedPixelCount, 8u);
  EXPECT_EQ(bakedData.rejectedPixelCount, 2u);
  EXPECT_EQ(bakedData.sourceMask, mask);
  ASSERT_EQ(bakedData.runs.size(), 5u);

  const std::array<std::size_t, 5> expectedSourceOffsets{0, 1, 4, 5, 7};
  const std::array<VulkanReplayBakedRect, 5> expectedNative{
      VulkanReplayBakedRect{1023, 511, 1, 1},
      VulkanReplayBakedRect{0, 511, 2, 1}, VulkanReplayBakedRect{3, 511, 1, 1},
      VulkanReplayBakedRect{1022, 0, 2, 1}, VulkanReplayBakedRect{0, 0, 2, 1}};
  const std::array<VulkanReplayBakedRect, 5> expectedAtlas{
      VulkanReplayBakedRect{4092, 2044, 4, 4},
      VulkanReplayBakedRect{0, 2044, 8, 4},
      VulkanReplayBakedRect{12, 2044, 4, 4},
      VulkanReplayBakedRect{4088, 0, 8, 4}, VulkanReplayBakedRect{0, 0, 8, 4}};
  const std::array<std::size_t, 5> expectedArenaOffsets{2, 4, 6, 8, 10};
  uint64_t acceptedWidth = 0;
  for (std::size_t index = 0; index < bakedData.runs.size(); ++index) {
    const VulkanReplayBakedCpuUploadRun &run = bakedData.runs[index];
    EXPECT_EQ(run.sourceChunkOffset, expectedSourceOffsets[index]);
    EXPECT_EQ(run.nativeRegion.sourceWordOffset, expectedArenaOffsets[index]);
    EXPECT_EQ(run.nativeRegion.sourceWordOffset % 2u, 0u);
    EXPECT_EQ(run.nativeRegion.bufferRowLength, expectedNative[index].width);
    EXPECT_EQ(run.nativeRegion.bufferImageHeight, 1u);
    EXPECT_EQ(run.nativeRegion.destination, expectedNative[index]);
    EXPECT_EQ(run.atlasArea, expectedAtlas[index]);
    EXPECT_TRUE(run.expandColorMask);
    EXPECT_TRUE(run.invalidateDepthD32);
    EXPECT_FLOAT_EQ(run.depthClearValue, 1.0f);
    acceptedWidth += run.nativeRegion.destination.width;
  }
  EXPECT_EQ(acceptedWidth, bakedData.acceptedPixelCount);

  const auto &bakedRejectedTail =
      std::get<VulkanReplayBakedCpuUpload>(result.batch.commands[4].payload);
  EXPECT_EQ(bakedRejectedTail.pixelOffset, 11u);
  EXPECT_TRUE(bakedRejectedTail.runs.empty());
  const auto &bakedComplete =
      std::get<VulkanReplayBakedCpuUpload>(result.batch.commands[5].payload);
  EXPECT_EQ(bakedComplete.phase, CpuUploadPhase::Complete);
  EXPECT_EQ(bakedComplete.pixelOffset, 12u);
  EXPECT_TRUE(bakedComplete.runs.empty());

  EXPECT_EQ(result.batch.nativeUploadPixels,
            (std::vector<uint16_t>{0x1234, 0, 0x8001, 0, 0x8002, 0x8003, 0x8005,
                                   0, 0x8006, 0x8007, 0x8008, 0x8009}));
}

TEST(GpuModernReplayVulkanBakeTest,
     PreservesA0AbortAndWrapsExtremeNativeOriginsWithoutOverflow) {
  const NativeRect transfer{std::numeric_limits<int32_t>::max(),
                            std::numeric_limits<int32_t>::min(), 2, 1};
  const CpuUpload begin = cpuUpload(99, CpuUploadPhase::Begin, transfer);
  const CpuUpload data =
      cpuUpload(99, CpuUploadPhase::Data, transfer, 0, {1, 2}, {0b11});
  const CpuUpload abort = cpuUpload(99, CpuUploadPhase::Abort, transfer, 2);

  const VulkanReplayBakeResult result = bakeModernReplayVulkan(
      makePlan(begin, data, abort), 4, cpuUploadFeatures());
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.commands.size(), 4u);
  const auto &bakedData =
      std::get<VulkanReplayBakedCpuUpload>(result.batch.commands[1].payload);
  ASSERT_EQ(bakedData.runs.size(), 2u);
  EXPECT_EQ(bakedData.runs[0].nativeRegion.destination,
            (VulkanReplayBakedRect{1023, 0, 1, 1}));
  EXPECT_EQ(bakedData.runs[1].nativeRegion.destination,
            (VulkanReplayBakedRect{0, 0, 1, 1}));
  EXPECT_EQ(bakedData.runs[0].atlasArea,
            (VulkanReplayBakedRect{4092, 0, 4, 4}));
  EXPECT_EQ(bakedData.runs[1].atlasArea, (VulkanReplayBakedRect{0, 0, 4, 4}));
  EXPECT_EQ(bakedData.runs[0].nativeRegion.sourceWordOffset, 0u);
  EXPECT_EQ(bakedData.runs[1].nativeRegion.sourceWordOffset, 2u);
  EXPECT_EQ(result.batch.nativeUploadPixels, (std::vector<uint16_t>{1, 0, 2}));
  const auto &bakedAbort =
      std::get<VulkanReplayBakedCpuUpload>(result.batch.commands[2].payload);
  EXPECT_EQ(bakedAbort.phase, CpuUploadPhase::Abort);
  EXPECT_EQ(bakedAbort.pixelOffset, 2u);
  EXPECT_TRUE(bakedAbort.runs.empty());
}

TEST(GpuModernReplayVulkanBakeTest,
     CpuUploadCapabilityAndDefensiveBoundaryFailTransactionally) {
  const NativeRect transfer{0, 0, 4, 1};
  const CpuUpload begin = cpuUpload(77, CpuUploadPhase::Begin, transfer);
  const CpuUpload data =
      cpuUpload(77, CpuUploadPhase::Data, transfer, 0, {1, 2, 3, 4}, {0b0101},
                MaskState{false, true});
  const CpuUpload complete =
      cpuUpload(77, CpuUploadPhase::Complete, transfer, 4);

  expectAtomicFailure(
      bakeModernReplayVulkan(makePlan(tileUpload(), begin, data, complete), 4),
      VulkanReplayBakeReason::CpuUploadDisabled, 1);

  ModernReplayExecutionPlan strayBitsPlan = makePlan(begin, data, complete);
  firstMutableCpuUpload(strayBitsPlan, CpuUploadPhase::Data).acceptedBits[0] |=
      uint64_t{1} << 63u;
  expectAtomicFailure(
      bakeModernReplayVulkan(strayBitsPlan, 4, cpuUploadFeatures()),
      VulkanReplayBakeReason::InvalidCpuUpload, 1);

  ModernReplayExecutionPlan planDrift = makePlan(begin, data, complete);
  ReplayExecutionOperation &dataItem =
      const_cast<ReplayExecutionOperation &>(planDrift.operations()[1]);
  ASSERT_FALSE(dataItem.access.maskRead);
  dataItem.access.maskRead = true;
  expectAtomicFailure(bakeModernReplayVulkan(planDrift, 4, cpuUploadFeatures()),
                      VulkanReplayBakeReason::UnsupportedCpuUploadState, 1);

  ModernReplayExecutionPlan countDrift = makePlan(begin, data, complete);
  firstMutableCpuUpload(countDrift, CpuUploadPhase::Data).acceptedBits[0] =
      0b0111;
  expectAtomicFailure(
      bakeModernReplayVulkan(countDrift, 4, cpuUploadFeatures()),
      VulkanReplayBakeReason::UnsupportedCpuUploadState, 1);

  ModernReplayExecutionPlan offsetOverflow = makePlan(begin, data, complete);
  firstMutableCpuUpload(offsetOverflow, CpuUploadPhase::Data).pixelOffset =
      std::numeric_limits<uint64_t>::max();
  expectAtomicFailure(
      bakeModernReplayVulkan(offsetOverflow, 4, cpuUploadFeatures()),
      VulkanReplayBakeReason::InvalidCpuUpload, 1);
}

TEST(GpuModernReplayVulkanBakeTest,
     FlattensQuadIndicesInSourceOrderAndScalesClampedInclusiveScissor) {
  DrawPacket packet = precisePacket(1, true, false);
  packet.triangleIndices = {0, 2, 1, 2, 3, 1};
  packet.state.drawArea = DrawArea{-2, 3, 1025, 9};
  const ModernReplayExecutionPlan plan = makePlan(packet);

  const VulkanReplayBakeResult result =
      bakeModernReplayVulkan(plan, 4, painterWriteFeatures());
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.vertices.size(), 6u);
  const auto &draw =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[0].payload);
  EXPECT_EQ(draw.scissor, (VulkanReplayBakedRect{0, 12, 4096, 28}));
  const std::array<uint8_t, 6> expected{0, 2, 1, 2, 3, 1};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    EXPECT_FLOAT_EQ(result.batch.vertices[index].positionNative[0],
                    static_cast<float>(packet.vertices[expected[index]].x));
    EXPECT_FLOAT_EQ(result.batch.vertices[index].projectiveW,
                    static_cast<float>(packet.vertices[expected[index]].w));
  }
}

TEST(GpuModernReplayVulkanBakeTest,
     UsesOneGlobalDepthDomainAcrossPacketsWithoutClampingRawW) {
  DrawPacket near = precisePacket(1);
  DrawPacket far = precisePacket(2);
  for (std::size_t index = 0; index < near.vertexCount; ++index) {
    near.vertices[index].w = 32768.0;
    near.vertices[index].rawDepth = 32768.0;
    far.vertices[index].w = 65536.0;
    far.vertices[index].rawDepth = 65536.0;
  }
  const ModernReplayExecutionPlan plan = makePlan(near, far);

  const VulkanReplayBakeResult result =
      bakeModernReplayVulkan(plan, 4, painterWriteFeatures());
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.vertices.size(), 6u);
  EXPECT_FLOAT_EQ(result.batch.vertices[0].projectiveW, 32768.0f);
  EXPECT_FLOAT_EQ(result.batch.vertices[3].projectiveW, 65536.0f);
  EXPECT_LT(result.batch.vertices[0].projectiveW,
            result.batch.vertices[3].projectiveW);
  const auto &first =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[0].payload);
  const auto &second =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[1].payload);
  EXPECT_FLOAT_EQ(first.uniforms.depthScaleBias[0],
                  vulkan_abi::PROJECTIVE_W_TO_DEPTH_SCALE);
  EXPECT_EQ(first.uniforms.depthScaleBias, second.uniforms.depthScaleBias);
}

TEST(GpuModernReplayVulkanBakeTest,
     PreservesOrderedDepthClearsAndLatchWithoutOtherSurfaceWrites) {
  ClearDepth clear;
  clear.nextEpoch = 8;
  Reset reset;
  reset.nextDepthEpoch = 9;
  const ModernReplayExecutionPlan plan = makePlan(clear, reset);

  const VulkanReplayBakeResult result = bakeModernReplayVulkan(plan, 4);
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.batch.commands.size(), 3u);
  EXPECT_TRUE(result.batch.vertices.empty());
  EXPECT_TRUE(result.batch.nativeUploadPixels.empty());
  const auto &first =
      std::get<VulkanReplayBakedDepthClear>(result.batch.commands[0].payload);
  const auto &second =
      std::get<VulkanReplayBakedDepthClear>(result.batch.commands[1].payload);
  EXPECT_EQ(first.area, (VulkanReplayBakedRect{0, 0, 4096, 2048}));
  EXPECT_EQ(first.nextEpoch, 8u);
  EXPECT_FALSE(first.fromReset);
  EXPECT_FLOAT_EQ(first.clearValue, 1.0f);
  EXPECT_EQ(second.area, first.area);
  EXPECT_EQ(second.nextEpoch, 9u);
  EXPECT_TRUE(second.fromReset);
  EXPECT_FLOAT_EQ(second.clearValue, 1.0f);
  EXPECT_TRUE(std::holds_alternative<VulkanReplayBakedLatch>(
      result.batch.commands[2].payload));
}

TEST(GpuModernReplayVulkanBakeTest,
     TexturedCapabilityIsExplicitAndFailureIsBatchAtomic) {
  const ModernReplayExecutionPlan plan =
      makePlan(tileUpload(), precisePacket(1, false, true));
  const VulkanReplayBakeResult result = bakeModernReplayVulkan(plan, 4);
  expectAtomicFailure(result, VulkanReplayBakeReason::TexturedDrawDisabled, 1);
}

TEST(GpuModernReplayVulkanBakeTest,
     SurfaceSeedAndDisabledTranslucencyFailAfterNoPartialPublication) {
  SurfaceSeed seed;
  seed.destination = NativeRect{0, 0, 1, 1};
  seed.pixels = {0x1234};
  seed.depthEpoch = 2;
  const VulkanReplayBakeResult seedResult =
      bakeModernReplayVulkan(makePlan(tileUpload(), seed), 4);
  expectAtomicFailure(seedResult,
                      VulkanReplayBakeReason::UnsupportedSurfaceSeed, 1);

  DrawPacket translucent = precisePacket();
  makeSemiTransparent(translucent);
  const VulkanReplayBakeResult translucentResult =
      bakeModernReplayVulkan(makePlan(tileUpload(), translucent), 4);
  expectAtomicFailure(translucentResult,
                      VulkanReplayBakeReason::SemiTransparentDrawDisabled, 1);
  EXPECT_EQ(translucentResult.sourceKind, ReplayExecutionKind::DrawTranslucent);
}

TEST(GpuModernReplayVulkanBakeTest,
     RejectsUnsupportedRouteStateDepthAndIncompleteProvenanceAtomically) {
  DrawPacket screen = precisePacket();
  makeScreen2D(screen);
  expectAtomicFailure(bakeModernReplayVulkan(makePlan(screen), 4),
                      VulkanReplayBakeReason::Screen2DDrawDisabled, 0);

  DrawPacket checkMask = precisePacket();
  checkMask.state.mask.checkMaskBit = true;
  expectAtomicFailure(bakeModernReplayVulkan(makePlan(checkMask), 4),
                      VulkanReplayBakeReason::DestinationMaskReadDisabled, 0);

  ReplayPolicyState depthOff = replayPolicy();
  depthOff.trueDepthMode = ReplayDepthMode::Off;
  DrawPacket noDepth = precisePacket();
  noDepth.state.depth = {};
  expectAtomicFailure(
      bakeModernReplayVulkan(makePlanWithPolicy(depthOff, noDepth), 4),
      VulkanReplayBakeReason::InvalidDepthState, 0);

  DrawPacket incomplete = precisePacket();
  incomplete.vertices[1].preciseX = false;
  expectAtomicFailure(
      bakeModernReplayVulkan(makePlan(incomplete), 4, painterWriteFeatures()),
      VulkanReplayBakeReason::IncompletePrecisionProvenance, 0);

  DrawPacket inconsistentDepth = precisePacket();
  inconsistentDepth.vertices[1].w += 1.0;
  expectAtomicFailure(bakeModernReplayVulkan(makePlan(inconsistentDepth), 4,
                                             painterWriteFeatures()),
                      VulkanReplayBakeReason::IncompletePrecisionProvenance, 0);

  DrawPacket saturated = precisePacket();
  saturated.vertices[0].rawDepth = 0.5;
  saturated.vertices[0].nearDepth = 1.0;
  saturated.vertices[0].w = 1.0;
  saturated.vertices[0].nearSaturated = true;
  EXPECT_TRUE(
      bakeModernReplayVulkan(makePlan(saturated), 4, painterWriteFeatures())
          .accepted());
}

TEST(GpuModernReplayVulkanBakeTest,
     RejectsInvalidWIndicesAndNumericOverflowBeforePublishingAnyCommand) {
  DrawPacket outside = precisePacket();
  outside.vertices[2].w =
      static_cast<double>(vulkan_abi::PROJECTIVE_W_DEPTH_DOMAIN_MAX) + 1.0;
  outside.vertices[2].rawDepth = outside.vertices[2].w;
  expectAtomicFailure(bakeModernReplayVulkan(makePlan(tileUpload(), outside), 4,
                                             painterWriteFeatures()),
                      VulkanReplayBakeReason::ProjectiveWOutOfDomain, 1);

  ModernReplayExecutionPlan nanPlan = makePlan(precisePacket());
  firstMutableDraw(nanPlan).vertices[0].w =
      std::numeric_limits<double>::quiet_NaN();
  expectAtomicFailure(
      bakeModernReplayVulkan(nanPlan, 4, painterWriteFeatures()),
      VulkanReplayBakeReason::ProjectiveWOutOfDomain, 0);

  ModernReplayExecutionPlan indexPlan = makePlan(precisePacket());
  firstMutableDraw(indexPlan).triangleIndices[2] = 3;
  expectAtomicFailure(
      bakeModernReplayVulkan(indexPlan, 4, painterWriteFeatures()),
      VulkanReplayBakeReason::InvalidTriangleIndex, 0);

  DrawPacket overflow = precisePacket();
  overflow.vertices[0].x = std::numeric_limits<double>::max();
  expectAtomicFailure(
      bakeModernReplayVulkan(makePlan(overflow), 4, painterWriteFeatures()),
      VulkanReplayBakeReason::NumericOverflow, 0);
}

TEST(GpuModernReplayVulkanBakeTest,
     RejectsInvalidOrPolicyMismatchedScaleWithEmptyBatch) {
  const ModernReplayExecutionPlan plan = makePlan(precisePacket());
  expectAtomicFailure(bakeModernReplayVulkan(plan, 0, painterWriteFeatures()),
                      VulkanReplayBakeReason::InvalidScale);
  expectAtomicFailure(bakeModernReplayVulkan(plan, 13, painterWriteFeatures()),
                      VulkanReplayBakeReason::InvalidScale);
  expectAtomicFailure(bakeModernReplayVulkan(plan, 2, painterWriteFeatures()),
                      VulkanReplayBakeReason::PolicyScaleMismatch, 1);
}

TEST(GpuModernReplayVulkanBakeTest,
     RejectsMalformedNativeUploadAndResetAtDefensiveBoundary) {
  NativeTileUpload unaligned = tileUpload(NativeRect{1, 0, 8, 8});
  expectAtomicFailure(bakeModernReplayVulkan(makePlan(unaligned), 4),
                      VulkanReplayBakeReason::InvalidNativeTileUpload, 0);

  ModernReplayExecutionPlan resetPlan = makePlan(Reset{});
  const ReplayExecutionOperation &resetItem = resetPlan.operations().front();
  Operation &resetOperation =
      const_cast<Operation &>(resetPlan.sourceOperation(resetItem));
  std::get<Reset>(resetOperation).clearDepth = false;
  expectAtomicFailure(bakeModernReplayVulkan(resetPlan, 4),
                      VulkanReplayBakeReason::InvalidDepthCommand, 0);
}

TEST(GpuModernReplayVulkanBakeTest,
     NativeProjectionFallbackRequiresExplicitOptInAndConstantW) {
  auto packet = precisePacket();
  makeScreen2D(packet);
  packet.route = PacketRoute::Projected3D;
  packet.semanticReason = PacketSemanticReason::PolygonDefault;
  packet.fallbackReason = PacketFallbackReason::MissingPrecisionPacketAtomic;
  packet.state.depth.invalidateOnAccepted = false;
  for (unsigned i = 0; i < packet.vertexCount; ++i)
    packet.vertices[i].inventedW = true;
  auto features = feedbackFeatures();
  auto plan = makePlan(packet);
  EXPECT_FALSE(bakeModernReplayVulkan(plan, 4, features).accepted());
  features.allowNativeProjectionFallback = true;
  EXPECT_TRUE(bakeModernReplayVulkan(plan, 4, features).accepted());
  firstMutableDraw(plan).vertices[1].w = 123.0;
  EXPECT_FALSE(bakeModernReplayVulkan(plan, 4, features).accepted());
}

TEST(GpuModernReplayVulkanBakeTest,
     OriginalMeshBakesGpuReferencesWithoutChangingCompactVertexAbi) {
  auto packet = precisePacket(1, true);
  packet.originalMesh.emplace();
  packet.originalMesh->pointBytes = {128, 176, 224, 272};
  packet.originalMesh->sourceRecovery = true;
  packet.originalMesh->projection = {320, 160, 112, 1};
  packet.originalMesh->viewRows[0] = {1, 0, 0, 30};
  const auto result =
      bakeModernReplayVulkan(makePlan(packet), 4, feedbackFeatures());
  ASSERT_TRUE(result.accepted());
  const auto &draw =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[0].payload);
  EXPECT_EQ(draw.segments[0].uniforms.mgsPointBytes,
            packet.originalMesh->pointBytes);
  EXPECT_TRUE(draw.sourceRecovery);
  EXPECT_EQ(draw.segments[0].uniforms.mgsViewRows,
            packet.originalMesh->viewRows);
  for (unsigned i = 0; i < 6; ++i)
    EXPECT_FLOAT_EQ(result.batch.vertices[i].positionNative[0],
                    float(packet.triangleIndices[i]));
  EXPECT_EQ(sizeof(vulkan_abi::Vertex), 32u);
}

TEST(GpuModernReplayVulkanBakeTest,
     CachedEyePlaneCrossingKeepsFullFeedbackScissor) {
  auto packet = precisePacket(1, true, true);
  packet.originalMesh.emplace();
  packet.originalMesh->projection = {320, 160, 112, 1};
  makeSemiTransparent(packet);
  auto &vertex = packet.vertices[0];
  vertex.w = vertex.nearDepth = 1;
  vertex.rawDepth = -100;
  vertex.nearSaturated = true;
  const auto result =
      bakeModernReplayVulkan(makePlan(packet), 4, feedbackFeatures());
  ASSERT_TRUE(result.accepted()) << int(result.reason);
  const auto &draw =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[0].payload);
  EXPECT_EQ(draw.segments[0].destinationSnapshot, draw.scissor);
  EXPECT_EQ(draw.segments[1].destinationSnapshot, draw.scissor);
}

TEST(GpuModernReplayVulkanBakeTest, PaintRunsKeepHolesRowsAndNativeWords) {
  auto paint = paintPixels();
  paint.blend.enabled = false;
  paint.blend.texturedStpOnly = false;
  paint.mask.forceMaskBit = false;
  auto pixel = paint.pixels[0];
  pixel.nativeColor = 0x1f;
  pixel.sourceMaskBit = false;
  pixel.destination = {10, 20};
  paint.pixels = {pixel};
  pixel.destination.x = 11;
  paint.pixels.push_back(pixel);
  pixel.destination.x = 13;
  paint.pixels.push_back(pixel);
  pixel.destination = {10, 21};
  paint.pixels.push_back(pixel);
  auto features = feedbackFeatures();
  features.allowPaintPixels = true;
  auto result = bakeModernReplayVulkan(makePlan(paint), 4, features);
  ASSERT_TRUE(result.accepted());
  // Three runs: [10,11], [13], then a new row at [10].
  EXPECT_EQ(result.batch.vertices.size(), 9u);
  const auto &upload =
      std::get<VulkanReplayBakedNativeUpload>(result.batch.commands[0].payload);
  EXPECT_EQ(upload.destination.width, 2u);
  EXPECT_EQ(upload.pixelCount, 2u);
  const auto &draw =
      std::get<VulkanReplayBakedDraw>(result.batch.commands[1].payload);
  EXPECT_EQ(draw.scissor.width, 8u);
  EXPECT_EQ(draw.scissor.height, 4u);
  EXPECT_EQ(result.batch.nativeUploadPixels[0], 0x1fu);
  EXPECT_EQ(result.batch.nativeUploadPixels[1], 0x1fu);
  paint.blend.enabled = true;
  paint.pixels[1].nativeColor = 0x03e0;
  result = bakeModernReplayVulkan(makePlan(paint), 4, features);
  ASSERT_TRUE(result.accepted());
  EXPECT_EQ(result.batch.vertices.size(), 9u);
  EXPECT_EQ(result.batch.nativeUploadPixels[0], 0x1fu);
  EXPECT_EQ(result.batch.nativeUploadPixels[1], 0x03e0u);
}
