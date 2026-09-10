#include "runtime/gpu/modern_replay_execution_plan.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <variant>
#include <vector>

using namespace ps1::gpu::replay;

namespace {

ReplayPolicyState hybridPolicy() {
  ReplayPolicyState policy;
  policy.internalScale = 4;
  policy.pgxp = true;
  policy.trueColor = true;
  policy.trueDepthMode = ReplayDepthMode::Hybrid;
  return policy;
}

FrameLatch latch(ReplayPolicyState policy = hybridPolicy(),
                 uint64_t sequenceEnd = 128) {
  FrameLatch value;
  value.frameNumber = 1;
  value.contentRevision = 1;
  value.completedSubmissionId = 1;
  value.sequenceBegin = 0;
  value.sequenceEnd = sequenceEnd;
  value.policy = policy;
  value.display.aperture = NativeRect{0, 0, 320, 240};
  return value;
}

DrawPacket opaquePacket(uint64_t sequence = 1) {
  DrawPacket packet;
  packet.opcode = 0x20;
  packet.packetSequence = sequence;
  packet.submissionId = 1;
  packet.validMask = 0x7;
  packet.vertexCount = 3;
  packet.indexCount = 3;
  packet.triangleIndices = {0, 1, 2, 0, 0, 0};
  packet.state.depth.testEnabled = false;
  packet.state.depth.writeEnabled = true;
  packet.state.depth.compare = DepthCompare::Always;
  packet.state.depth.epoch = 1;
  return packet;
}

void makeTextured(DrawPacket &packet) {
  packet.opcode = static_cast<uint8_t>(packet.opcode | 0x04u);
  packet.textured = true;
  packet.state.textureUvDomain.enabled = true;
  packet.nativeRead.required = true;
  packet.nativeRead.tileCount = 1;
  packet.nativeRead.versionHash = 1;
}

void makeTranslucent(DrawPacket &packet) {
  packet.opcode = static_cast<uint8_t>(packet.opcode | 0x02u);
  packet.state.blend.enabled = true;
  packet.state.blend.texturedStpOnly = packet.textured;
  packet.state.depth.testEnabled = false;
  packet.state.depth.writeEnabled = false;
  packet.state.depth.compare = DepthCompare::Always;
  packet.state.depth.invalidateOnAccepted = true;
}

void makeNativeReadable(CopyRect &copy) {
  copy.nativeRead.required = true;
  copy.nativeRead.tileCount = 1;
  copy.nativeRead.versionHash =
      appendNativeReadDependencyHash(CRASH_REPLAY_READ_HASH_OFFSET, 0, 1);
}

void makeInvalidW(DrawPacket &packet) {
  packet.validMask = 0;
  packet.fallbackReason = PacketFallbackReason::InvalidWPacketAtomic;
  for (std::size_t index = 0; index < packet.vertexCount; ++index)
    packet.vertices[index].w = CRASH_REPLAY_FALLBACK_W;
  packet.state.depth.testEnabled = false;
  packet.state.depth.writeEnabled = false;
  packet.state.depth.compare = DepthCompare::Always;
}

void makeFullyPrecise(DrawPacket &packet) {
  for (std::size_t index = 0; index < packet.vertexCount; ++index) {
    ReplayVertex &vertex = packet.vertices[index];
    vertex.nativeX = static_cast<int32_t>(10 + index);
    vertex.nativeY = static_cast<int32_t>(20 + index);
    vertex.x = static_cast<double>(vertex.nativeX) + 0.25;
    vertex.y = static_cast<double>(vertex.nativeY) + 0.5;
    vertex.w = 4096.0 + static_cast<double>(index);
    vertex.rawDepth = vertex.w;
    vertex.nearDepth = 1.0;
    vertex.preciseXY = true;
    vertex.preciseX = true;
    vertex.preciseY = true;
    vertex.rawDepthValid = true;
    vertex.preciseUV = packet.textured;
  }
}

DrawPacket screenPacket(uint64_t sequence = 1) {
  DrawPacket packet = opaquePacket(sequence);
  packet.route = PacketRoute::Screen2D;
  packet.semanticReason = PacketSemanticReason::Intrinsic2DFamily;
  packet.validMask = 0;
  packet.state.depth.testEnabled = false;
  packet.state.depth.writeEnabled = false;
  packet.state.depth.compare = DepthCompare::Always;
  packet.state.depth.invalidateOnAccepted = true;
  return packet;
}

template <typename... Operations>
CrashReplayFramePtr frameWithPolicy(ReplayPolicyState policy,
                                    Operations &&...operations) {
  CrashReplayFrameBuilder builder;
  (builder.record(std::forward<Operations>(operations)), ...);
  return builder.seal(latch(policy));
}

template <typename... Operations>
CrashReplayFramePtr frame(Operations &&...operations) {
  return frameWithPolicy(hybridPolicy(),
                         std::forward<Operations>(operations)...);
}

const ReplayExecutionOperation &
onlyMutation(const ReplayExecutionPlanResult &result) {
  EXPECT_TRUE(result.accepted());
  EXPECT_EQ(result.plan.operations().size(), 2u);
  return result.plan.operations().front();
}

} // namespace

TEST(GpuModernReplayExecutionPlanTest,
     PreservesOperationOrderAndOwnsExactSourceFrames) {
  FillRect fill;
  fill.destination = NativeRect{4, 5, 8, 2};
  DrawPacket draw = opaquePacket();

  CrashReplayFramePtr source = frame(fill, draw);
  std::weak_ptr<const CrashReplayFrame> weak = source;
  std::array<CrashReplayFramePtr, 1> input{source};
  ReplayExecutionPlanResult result = planModernReplayExecution(input);
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.plan.operations().size(), 3u);
  EXPECT_EQ(result.plan.operations()[0].kind, ReplayExecutionKind::FillRect);
  EXPECT_EQ(result.plan.operations()[1].kind, ReplayExecutionKind::DrawOpaque);
  EXPECT_EQ(result.plan.operations()[2].kind, ReplayExecutionKind::FrameLatch);
  for (std::size_t index = 0; index < result.plan.operations().size();
       ++index) {
    EXPECT_EQ(result.plan.operations()[index].ordinal, index);
    EXPECT_EQ(result.plan.operations()[index].frameIndex, 0u);
    EXPECT_EQ(result.plan.operations()[index].operationIndex, index);
  }
  EXPECT_TRUE(std::holds_alternative<FillRect>(
      result.plan.sourceOperation(result.plan.operations()[0])));
  EXPECT_TRUE(std::holds_alternative<DrawPacket>(
      result.plan.sourceOperation(result.plan.operations()[1])));

  source.reset();
  input[0].reset();
  EXPECT_FALSE(weak.expired());
  result = {};
  EXPECT_TRUE(weak.expired());
}

TEST(GpuModernReplayExecutionPlanTest,
     OpaqueTexturedStpWritesMaskWithoutDestinationFeedback) {
  DrawPacket packet = opaquePacket();
  makeTextured(packet);
  const ReplayExecutionPlanResult result =
      planModernReplayExecution(std::array{frame(packet)});
  const ReplayExecutionOperation &item = onlyMutation(result);

  EXPECT_EQ(item.kind, ReplayExecutionKind::DrawOpaque);
  EXPECT_EQ(item.draw.blendSelection, ReplayBlendSelection::None);
  EXPECT_TRUE(item.draw.sourceStpWritesMask);
  EXPECT_TRUE(item.draw.textureZeroCanDiscard);
  EXPECT_TRUE(item.access.nativeRead);
  EXPECT_FALSE(item.access.colorRead);
  EXPECT_TRUE(item.access.maskWrite);
  EXPECT_FALSE(item.hazards.destinationFeedbackBoundary);
  EXPECT_FALSE(item.hazards.destinationScratchRequired);
  EXPECT_TRUE(item.depth.donorEligible);
  EXPECT_FALSE(item.depth.test);
  EXPECT_TRUE(item.depth.write);
  EXPECT_FALSE(item.access.depthRead);
  EXPECT_TRUE(item.access.depthWrite);
  EXPECT_TRUE(item.hazards.fragmentAcceptanceControlsDepth);
}

TEST(GpuModernReplayExecutionPlanTest,
     TranslucencyUsesFeedbackAndAcceptedSampleDepthBoundaries) {
  DrawPacket textured = opaquePacket(1);
  makeTextured(textured);
  makeTranslucent(textured);
  DrawPacket untextured = opaquePacket(2);
  makeTranslucent(untextured);

  const ReplayExecutionPlanResult result =
      planModernReplayExecution(std::array{frame(textured, untextured)});
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.plan.operations().size(), 3u);
  const ReplayExecutionOperation &stp = result.plan.operations()[0];
  EXPECT_EQ(stp.kind, ReplayExecutionKind::DrawTranslucent);
  EXPECT_EQ(stp.draw.blendSelection, ReplayBlendSelection::SourceStp);
  EXPECT_TRUE(stp.access.colorRead);
  EXPECT_TRUE(stp.hazards.destinationFeedbackBoundary);
  EXPECT_TRUE(stp.hazards.destinationScratchRequired);
  EXPECT_FALSE(stp.depth.donorEligible);
  EXPECT_FALSE(stp.depth.test);
  EXPECT_FALSE(stp.depth.write);
  EXPECT_TRUE(stp.depth.invalidateOnAccepted);
  EXPECT_FALSE(stp.access.depthRead);
  EXPECT_TRUE(stp.access.depthWrite);
  EXPECT_TRUE(stp.hazards.fragmentAcceptanceControlsDepth);

  const ReplayExecutionOperation &whole = result.plan.operations()[1];
  EXPECT_EQ(whole.draw.blendSelection, ReplayBlendSelection::WholePrimitive);
  EXPECT_TRUE(whole.access.colorRead);
  EXPECT_FALSE(whole.depth.test);
  EXPECT_FALSE(whole.depth.write);
  EXPECT_TRUE(whole.depth.invalidateOnAccepted);
  EXPECT_TRUE(whole.access.depthWrite);
}

TEST(GpuModernReplayExecutionPlanTest,
     HybridRejectsLegacySemiTransparentAndInvalidWDepthWritesAtomically) {
  DrawPacket semi = opaquePacket();
  makeTranslucent(semi);
  semi.state.depth.writeEnabled = true;
  ReplayExecutionPlanResult semiResult =
      planModernReplayExecution(std::array{frame(semi)});
  EXPECT_EQ(semiResult.reason,
            ReplayExecutionPlanReason::DonorDepthStateMismatch);
  EXPECT_EQ(semiResult.frameIndex, 0u);
  EXPECT_EQ(semiResult.operationIndex, 0u);
  EXPECT_TRUE(semiResult.plan.operations().empty());
  EXPECT_TRUE(semiResult.plan.frames().empty());

  DrawPacket invalid = opaquePacket();
  makeInvalidW(invalid);
  invalid.state.depth.writeEnabled = true;
  ReplayExecutionPlanResult invalidResult =
      planModernReplayExecution(std::array{frame(invalid)});
  EXPECT_EQ(invalidResult.reason,
            ReplayExecutionPlanReason::DonorDepthStateMismatch);
  EXPECT_TRUE(invalidResult.plan.operations().empty());
}

TEST(GpuModernReplayExecutionPlanTest,
     RejectsNonCanonicalPainterAndScreenDepthCompare) {
  DrawPacket painter = opaquePacket();
  makeTranslucent(painter);
  painter.state.depth.compare = DepthCompare::Less;
  const ReplayExecutionPlanResult painterResult =
      planModernReplayExecution(std::array{frame(painter)});
  EXPECT_EQ(painterResult.reason,
            ReplayExecutionPlanReason::DonorDepthStateMismatch);
  EXPECT_TRUE(painterResult.plan.operations().empty());

  DrawPacket screen = screenPacket();
  screen.state.depth.compare = DepthCompare::Less;
  const ReplayExecutionPlanResult screenResult =
      planModernReplayExecution(std::array{frame(screen)});
  EXPECT_EQ(screenResult.reason,
            ReplayExecutionPlanReason::DonorDepthStateMismatch);
  EXPECT_TRUE(screenResult.plan.operations().empty());
}

TEST(GpuModernReplayExecutionPlanTest,
     InvalidWFallbackAndScreen2DHaveNoTrueDepthSampleWrite) {
  DrawPacket invalid = opaquePacket(1);
  makeInvalidW(invalid);
  DrawPacket screen = screenPacket(2);
  const ReplayExecutionPlanResult result =
      planModernReplayExecution(std::array{frame(invalid, screen)});
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.plan.operations().size(), 3u);

  const ReplayExecutionOperation &fallback = result.plan.operations()[0];
  EXPECT_FALSE(fallback.depth.donorEligible);
  EXPECT_FALSE(fallback.depth.test);
  EXPECT_FALSE(fallback.depth.write);
  EXPECT_FALSE(fallback.depth.invalidateOnAccepted);
  EXPECT_FALSE(fallback.access.depthWrite);

  const ReplayExecutionOperation &screenItem = result.plan.operations()[1];
  EXPECT_FALSE(screenItem.depth.donorEligible);
  EXPECT_FALSE(screenItem.depth.test);
  EXPECT_FALSE(screenItem.depth.write);
  EXPECT_TRUE(screenItem.depth.invalidateOnAccepted);
  EXPECT_TRUE(screenItem.access.depthWrite);
}

TEST(GpuModernReplayExecutionPlanTest,
     Screen2DTextureAndMaskAcceptancePrecedeDepthInvalidation) {
  DrawPacket screen = screenPacket();
  makeTextured(screen);
  screen.state.mask.checkMaskBit = true;

  const ReplayExecutionPlanResult result =
      planModernReplayExecution(std::array{frame(screen)});
  const ReplayExecutionOperation &item = onlyMutation(result);
  EXPECT_TRUE(item.draw.textureZeroCanDiscard);
  EXPECT_TRUE(item.draw.checkMask);
  EXPECT_TRUE(item.depth.invalidateOnAccepted);
  EXPECT_TRUE(item.access.depthWrite);
  EXPECT_TRUE(item.hazards.fragmentAcceptanceControlsDepth);
}

TEST(GpuModernReplayExecutionPlanTest,
     CheckMaskCreatesDestinationScratchAndLateDepthBoundary) {
  DrawPacket packet = opaquePacket();
  packet.state.mask.checkMaskBit = true;
  packet.state.mask.forceMaskBit = true;
  packet.vertexCount = 4;
  packet.indexCount = 6;
  packet.opcode = 0x28;
  packet.validMask = 0xF;
  packet.triangleIndices = {0, 1, 2, 1, 2, 3};

  const ReplayExecutionPlanResult result =
      planModernReplayExecution(std::array{frame(packet)});
  const ReplayExecutionOperation &item = onlyMutation(result);
  EXPECT_TRUE(item.draw.checkMask);
  EXPECT_TRUE(item.draw.forceMask);
  EXPECT_TRUE(item.access.maskRead);
  EXPECT_TRUE(item.hazards.destinationFeedbackBoundary);
  EXPECT_TRUE(item.hazards.destinationScratchRequired);
  EXPECT_TRUE(item.hazards.refreshDestinationBetweenTriangles);
  EXPECT_TRUE(item.hazards.barrierBefore);
  EXPECT_TRUE(item.hazards.barrierAfter);
  EXPECT_FALSE(item.depth.test);
  EXPECT_TRUE(item.depth.write);
  EXPECT_FALSE(item.access.depthRead);
  EXPECT_TRUE(item.access.depthWrite);
  EXPECT_TRUE(item.hazards.fragmentAcceptanceControlsDepth);
}

TEST(GpuModernReplayExecutionPlanTest,
     A0DataUsesAcceptedBitmapWithoutRereadingDestinationMask) {
  const NativeRect transfer{1022, 511, 6, 1};
  CpuUpload begin;
  begin.transactionId = 10;
  begin.phase = CpuUploadPhase::Begin;
  begin.transfer = transfer;
  begin.mask.checkMaskBit = true;

  CpuUpload data = begin;
  data.phase = CpuUploadPhase::Data;
  data.pixels = {1, 2, 3, 4, 5, 6};
  data.acceptedBits = {0b100101};

  CpuUpload complete = begin;
  complete.phase = CpuUploadPhase::Complete;
  complete.pixelOffset = 6;

  const ReplayExecutionPlanResult result =
      planModernReplayExecution(std::array{frame(begin, data, complete)});
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.plan.operations().size(), 4u);
  EXPECT_EQ(result.plan.operations()[0].kind,
            ReplayExecutionKind::CpuUploadBegin);
  const ReplayExecutionOperation &item = result.plan.operations()[1];
  EXPECT_EQ(item.kind, ReplayExecutionKind::CpuUploadData);
  EXPECT_TRUE(item.a0.data);
  EXPECT_EQ(item.a0.consumedPixels, 6u);
  EXPECT_EQ(item.a0.acceptedPixels, 3u);
  EXPECT_EQ(item.a0.rejectedPixels, 3u);
  EXPECT_TRUE(item.access.nativeWrite);
  EXPECT_TRUE(item.access.colorWrite);
  EXPECT_TRUE(item.access.maskWrite);
  EXPECT_FALSE(item.access.maskRead);
  EXPECT_FALSE(item.hazards.destinationScratchRequired);
  EXPECT_TRUE(item.depth.invalidateOnAccepted);
  EXPECT_TRUE(item.access.depthWrite);
  EXPECT_TRUE(item.hazards.barrierBefore);
  EXPECT_TRUE(item.hazards.barrierAfter);
  EXPECT_EQ(result.plan.operations()[2].kind,
            ReplayExecutionKind::CpuUploadComplete);
}

TEST(GpuModernReplayExecutionPlanTest,
     FullyRejectedA0ChunkConsumesStreamWithoutCreatingResourceWrites) {
  CpuUpload begin;
  begin.transactionId = 10;
  begin.phase = CpuUploadPhase::Begin;
  begin.transfer = NativeRect{0, 0, 4, 1};
  begin.mask.checkMaskBit = true;

  CpuUpload data = begin;
  data.phase = CpuUploadPhase::Data;
  data.pixels = {1, 2, 3, 4};
  data.acceptedBits = {0};

  CpuUpload complete = begin;
  complete.phase = CpuUploadPhase::Complete;
  complete.pixelOffset = 4;

  const ReplayExecutionPlanResult result =
      planModernReplayExecution(std::array{frame(begin, data, complete)});
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.plan.operations().size(), 4u);
  const ReplayExecutionOperation &item = result.plan.operations()[1];
  EXPECT_EQ(item.a0.consumedPixels, 4u);
  EXPECT_EQ(item.a0.acceptedPixels, 0u);
  EXPECT_EQ(item.a0.rejectedPixels, 4u);
  EXPECT_FALSE(item.access.nativeWrite);
  EXPECT_FALSE(item.access.colorWrite);
  EXPECT_FALSE(item.access.maskWrite);
  EXPECT_FALSE(item.access.depthWrite);
  EXPECT_FALSE(item.hazards.barrierBefore);
  EXPECT_FALSE(item.hazards.barrierAfter);
}

TEST(GpuModernReplayExecutionPlanTest,
     FillAndCopyAreNonReorderableAndCopyAlwaysUsesSourceScratch) {
  FillRect fill;
  fill.destination = NativeRect{8, 8, 16, 4};
  CopyRect copy;
  copy.source = NativePoint{0, 0};
  copy.destination = NativePoint{4, 0};
  copy.width = 32;
  copy.height = 2;
  makeNativeReadable(copy);

  const ReplayExecutionPlanResult result =
      planModernReplayExecution(std::array{frame(fill, copy)});
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.plan.operations().size(), 3u);
  const ReplayExecutionOperation &fillItem = result.plan.operations()[0];
  EXPECT_EQ(fillItem.kind, ReplayExecutionKind::FillRect);
  EXPECT_FALSE(fillItem.access.nativeRead);
  EXPECT_TRUE(fillItem.access.nativeWrite);
  EXPECT_TRUE(fillItem.depth.invalidateOnAccepted);
  EXPECT_TRUE(fillItem.hazards.barrierBefore);
  EXPECT_TRUE(fillItem.hazards.barrierAfter);

  const ReplayExecutionOperation &copyItem = result.plan.operations()[1];
  EXPECT_EQ(copyItem.kind, ReplayExecutionKind::CopyRect);
  EXPECT_TRUE(copyItem.access.nativeRead);
  EXPECT_TRUE(copyItem.access.nativeWrite);
  EXPECT_TRUE(copyItem.access.colorRead);
  EXPECT_TRUE(copyItem.access.maskRead);
  EXPECT_TRUE(copyItem.hazards.copySourceScratchRequired);
  EXPECT_FALSE(copyItem.hazards.destinationScratchRequired);
  EXPECT_TRUE(copyItem.depth.invalidateOnAccepted);
  EXPECT_TRUE(copyItem.hazards.barrierBefore);
  EXPECT_TRUE(copyItem.hazards.barrierAfter);
}

TEST(GpuModernReplayExecutionPlanTest,
     CopyMaskCheckAddsDestinationFeedbackToMandatorySourceScratch) {
  CopyRect copy;
  copy.source = NativePoint{100, 20};
  copy.destination = NativePoint{101, 20};
  copy.width = 8;
  copy.height = 8;
  makeNativeReadable(copy);
  copy.mask.checkMaskBit = true;

  const ReplayExecutionPlanResult result =
      planModernReplayExecution(std::array{frame(copy)});
  const ReplayExecutionOperation &item = onlyMutation(result);
  EXPECT_TRUE(item.hazards.copySourceScratchRequired);
  EXPECT_TRUE(item.hazards.destinationFeedbackBoundary);
  EXPECT_TRUE(item.hazards.destinationScratchRequired);
}

TEST(GpuModernReplayExecutionPlanTest,
     AcceptedPaintPixelsDoNotRepeatE6ButRetainTrueColorBlendFeedback) {
  PaintPixels paint;
  paint.pixels.push_back(
      PaintedPixel{NativePoint{4, 5}, 0x801F, Rgb8{255, 0, 0}, true});
  paint.mask = MaskState{true, true};
  paint.blend.enabled = true;
  paint.blend.texturedStpOnly = true;
  paint.depth.epoch = 1;
  paint.depth.invalidateOnAccepted = true;

  const ReplayExecutionPlanResult result =
      planModernReplayExecution(std::array{frame(std::move(paint))});
  const ReplayExecutionOperation &item = onlyMutation(result);
  EXPECT_EQ(item.kind, ReplayExecutionKind::PaintPixels);
  EXPECT_TRUE(item.access.nativeWrite);
  EXPECT_TRUE(item.access.colorRead);
  EXPECT_TRUE(item.access.colorWrite);
  EXPECT_FALSE(item.access.maskRead)
      << "the payload already contains only E6-accepted destinations";
  EXPECT_TRUE(item.access.maskWrite);
  EXPECT_TRUE(item.depth.invalidateOnAccepted);
  EXPECT_TRUE(item.hazards.destinationFeedbackBoundary);
  EXPECT_TRUE(item.hazards.destinationScratchRequired);
  EXPECT_FALSE(item.hazards.fragmentAcceptanceControlsDepth);
}

TEST(GpuModernReplayExecutionPlanTest,
     DepthOffKeepsEligibilityEvidenceButDoesNotRequireOrScheduleD32) {
  ReplayPolicyState policy = hybridPolicy();
  policy.trueDepthMode = ReplayDepthMode::Off;
  DrawPacket packet = opaquePacket();
  packet.state.depth.testEnabled = false;
  packet.state.depth.writeEnabled = false;
  packet.state.depth.compare = DepthCompare::Always;

  const ReplayExecutionPlanResult result =
      planModernReplayExecution(std::array{frameWithPolicy(policy, packet)});
  const ReplayExecutionOperation &item = onlyMutation(result);
  EXPECT_TRUE(item.depth.donorEligible);
  EXPECT_FALSE(item.depth.requestedTest);
  EXPECT_FALSE(item.depth.requestedWrite);
  EXPECT_FALSE(item.depth.test);
  EXPECT_FALSE(item.depth.write);
  EXPECT_FALSE(item.access.depthRead);
  EXPECT_FALSE(item.access.depthWrite);
}

TEST(GpuModernReplayExecutionPlanTest,
     RejectsPolicyDiscontinuityWithoutReturningPartialPlan) {
  ReplayPolicyState off = hybridPolicy();
  off.trueDepthMode = ReplayDepthMode::Off;
  const CrashReplayFramePtr first = frame();
  const CrashReplayFramePtr second = frameWithPolicy(off);
  const ReplayExecutionPlanResult result =
      planModernReplayExecution(std::array{first, second});
  EXPECT_EQ(result.reason, ReplayExecutionPlanReason::PolicyDiscontinuity);
  EXPECT_EQ(result.frameIndex, 1u);
  EXPECT_TRUE(result.plan.frames().empty());
  EXPECT_TRUE(result.plan.operations().empty());
  EXPECT_EQ(replayExecutionPlanReasonName(result.reason),
            "policy_discontinuity");
}

TEST(GpuModernReplayExecutionPlanTest,
     FrameLatchIsTheFinalColorReadAndBatchBarrier) {
  const ReplayExecutionPlanResult result =
      planModernReplayExecution(std::array{frame()});
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.plan.operations().size(), 1u);
  const ReplayExecutionOperation &item = result.plan.operations().front();
  EXPECT_EQ(item.kind, ReplayExecutionKind::FrameLatch);
  EXPECT_TRUE(item.access.colorRead);
  EXPECT_TRUE(item.hazards.barrierBefore);
  EXPECT_FALSE(item.hazards.barrierAfter);
}

TEST(GpuModernReplayExecutionPlanTest,
     PrecisionCoverageCountsEveryVertexIncludingPainterOrderedGeometry) {
  DrawPacket opaqueTextured = opaquePacket(1);
  makeTextured(opaqueTextured);
  makeFullyPrecise(opaqueTextured);

  DrawPacket translucent = opaquePacket(2);
  makeTranslucent(translucent);
  makeFullyPrecise(translucent);

  DrawPacket screen = screenPacket(3);
  const ReplayPrecisionCoverage coverage = measureReplayPrecisionCoverage(
      std::array{frame(opaqueTextured, translucent, screen)});

  EXPECT_EQ(coverage.frames, 1u);
  EXPECT_EQ(coverage.drawPackets, 3u);
  EXPECT_EQ(coverage.projectedPackets, 2u);
  EXPECT_EQ(coverage.screen2DPackets, 1u);
  EXPECT_EQ(coverage.projectedVertices, 6u);
  EXPECT_EQ(coverage.preciseProjectedXYVertices, 6u);
  EXPECT_EQ(coverage.validProjectedWVertices, 6u);
  EXPECT_EQ(coverage.texturedProjectedVertices, 3u);
  EXPECT_EQ(coverage.preciseProjectedUVVertices, 3u);
  EXPECT_EQ(coverage.screen2DVertices, 3u);
  EXPECT_EQ(coverage.exactScreen2DVertices, 3u);
  EXPECT_EQ(coverage.donorDepthEligiblePackets, 1u);
  EXPECT_EQ(coverage.donorDepthEligibleVertices, 3u);
  EXPECT_EQ(coverage.donorDepthStateMatchedPackets, 1u);
  EXPECT_EQ(coverage.donorDepthStateMatchedVertices, 3u);
  EXPECT_EQ(coverage.painterOrderedProjectedPackets, 1u);
  EXPECT_EQ(coverage.painterOrderedProjectedVertices, 3u);
  EXPECT_EQ(coverage.painterPreciseTranslucentPackets, 1u);
  EXPECT_EQ(coverage.painterPreciseTranslucentVertices, 3u);
  EXPECT_EQ(coverage.otVisibilityBoundaryPackets, 1u);
  EXPECT_EQ(coverage.otVisibilityBoundaryVertices, 3u);
  EXPECT_EQ(coverage.painterFallbackPackets, 0u);
  EXPECT_EQ(coverage.painterDepthPolicyOffPackets, 0u);
  EXPECT_EQ(coverage.painterDepthStateGapPackets, 0u);
  EXPECT_EQ(coverage.painterUnexpectedOpaquePackets, 0u);
  EXPECT_EQ(coverage.screen2DDepthInvalidateMatchedPackets, 1u);
  EXPECT_EQ(coverage.screen2DDepthStateGapPackets, 0u);
  EXPECT_TRUE(coverage.fullyHighPrecision());
}

TEST(GpuModernReplayExecutionPlanTest,
     PrecisionCoverageFailsClosedForOneInvalidProjectedVertex) {
  DrawPacket packet = opaquePacket();
  makeFullyPrecise(packet);
  packet.validMask = 0;
  packet.fallbackReason = PacketFallbackReason::InvalidWPacketAtomic;
  packet.vertices[0].preciseXY = false;
  for (std::size_t index = 0; index < packet.vertexCount; ++index) {
    packet.vertices[index].w = CRASH_REPLAY_FALLBACK_W;
    packet.vertices[index].rawDepth = 0.0;
    packet.vertices[index].nearDepth = 0.0;
    packet.vertices[index].rawDepthValid = false;
    packet.vertices[index].inventedW = true;
  }
  packet.state.depth.testEnabled = false;
  packet.state.depth.writeEnabled = false;
  packet.state.depth.compare = DepthCompare::Always;

  const ReplayPrecisionCoverage coverage =
      measureReplayPrecisionCoverage(std::array{frame(packet)});
  EXPECT_EQ(coverage.projectedVertices, 3u);
  EXPECT_EQ(coverage.preciseProjectedXYVertices, 2u);
  EXPECT_EQ(coverage.validProjectedWVertices, 0u);
  EXPECT_EQ(coverage.fallbackPackets, 1u);
  EXPECT_EQ(coverage.painterFallbackPackets, 1u);
  EXPECT_EQ(coverage.painterFallbackVertices, 3u);
  EXPECT_FALSE(coverage.projectedCoordinatesComplete());
  EXPECT_FALSE(coverage.projectedDepthProvenanceComplete());
  EXPECT_FALSE(coverage.fullyHighPrecision());
}

TEST(GpuModernReplayExecutionPlanTest,
     PrecisionCoverageRejectsIncorrectPainterAndScreenDepthState) {
  DrawPacket translucent = opaquePacket(1);
  makeTranslucent(translucent);
  makeFullyPrecise(translucent);
  translucent.state.depth.writeEnabled = true;

  DrawPacket screen = screenPacket(2);
  screen.state.depth.compare = DepthCompare::Less;
  const ReplayPrecisionCoverage coverage =
      measureReplayPrecisionCoverage(std::array{frame(translucent, screen)});

  EXPECT_EQ(coverage.painterOrderedProjectedPackets, 1u);
  EXPECT_EQ(coverage.painterPreciseTranslucentPackets, 0u);
  EXPECT_EQ(coverage.otVisibilityBoundaryPackets, 0u);
  EXPECT_EQ(coverage.painterDepthStateGapPackets, 1u);
  EXPECT_EQ(coverage.painterDepthStateGapVertices, 3u);
  EXPECT_EQ(coverage.screen2DDepthInvalidateMatchedPackets, 0u);
  EXPECT_EQ(coverage.screen2DDepthStateGapPackets, 1u);
  EXPECT_FALSE(coverage.donorDepthApplicationComplete());
  EXPECT_FALSE(coverage.fullyHighPrecision());
}

TEST(GpuModernReplayExecutionPlanTest,
     PrecisionCoverageNamesIntentionalDepthOffPainterPolicy) {
  ReplayPolicyState off = hybridPolicy();
  off.trueDepthMode = ReplayDepthMode::Off;

  DrawPacket opaque = opaquePacket(1);
  makeFullyPrecise(opaque);
  opaque.state.depth.testEnabled = false;
  opaque.state.depth.writeEnabled = false;
  opaque.state.depth.compare = DepthCompare::Always;
  DrawPacket screen = screenPacket(2);
  const ReplayPrecisionCoverage coverage = measureReplayPrecisionCoverage(
      std::array{frameWithPolicy(off, opaque, screen)});

  EXPECT_EQ(coverage.donorDepthEligiblePackets, 0u);
  EXPECT_EQ(coverage.painterDepthPolicyOffPackets, 1u);
  EXPECT_EQ(coverage.painterDepthPolicyOffVertices, 3u);
  EXPECT_EQ(coverage.screen2DDepthInvalidateMatchedPackets, 1u);
  EXPECT_EQ(coverage.painterDepthStateGapPackets, 0u);
  EXPECT_EQ(coverage.screen2DDepthStateGapPackets, 0u);
  EXPECT_TRUE(coverage.donorDepthApplicationComplete());
  EXPECT_TRUE(coverage.fullyHighPrecision());
}

TEST(GpuModernReplayExecutionPlanTest,
     PrecisionCoverageDoesNotAliasDepthOffTranslucencyAsOtBoundary) {
  ReplayPolicyState off = hybridPolicy();
  off.trueDepthMode = ReplayDepthMode::Off;

  DrawPacket translucent = opaquePacket();
  makeTranslucent(translucent);
  makeFullyPrecise(translucent);
  translucent.state.depth.invalidateOnAccepted = false;

  const ReplayPrecisionCoverage coverage = measureReplayPrecisionCoverage(
      std::array{frameWithPolicy(off, translucent)});
  EXPECT_EQ(coverage.painterPreciseTranslucentPackets, 1u);
  EXPECT_EQ(coverage.painterPreciseTranslucentVertices, 3u);
  EXPECT_EQ(coverage.otVisibilityBoundaryPackets, 0u);
  EXPECT_EQ(coverage.otVisibilityBoundaryVertices, 0u);
  EXPECT_TRUE(coverage.donorDepthApplicationComplete());
}

TEST(GpuModernReplayExecutionPlanTest,
     PrecisionCoverageDoesNotLetPolicyOffHideInvalidPainterState) {
  ReplayPolicyState off = hybridPolicy();
  off.trueDepthMode = ReplayDepthMode::Off;

  DrawPacket opaque = opaquePacket();
  makeFullyPrecise(opaque);
  opaque.state.depth.testEnabled = false;
  opaque.state.depth.writeEnabled = false;
  opaque.state.depth.compare = DepthCompare::Less;
  const ReplayPrecisionCoverage coverage =
      measureReplayPrecisionCoverage(std::array{frameWithPolicy(off, opaque)});

  EXPECT_EQ(coverage.painterOrderedProjectedPackets, 1u);
  EXPECT_EQ(coverage.painterDepthPolicyOffPackets, 0u);
  EXPECT_EQ(coverage.painterDepthStateGapPackets, 1u);
  EXPECT_EQ(coverage.painterDepthStateGapVertices, 3u);
  EXPECT_FALSE(coverage.donorDepthApplicationComplete());
  EXPECT_FALSE(coverage.fullyHighPrecision());
}

TEST(GpuModernReplayExecutionPlanTest,
     PrecisionCoverageDoesNotLetFallbackHideInvalidPainterState) {
  DrawPacket fallback = opaquePacket();
  makeFullyPrecise(fallback);
  makeInvalidW(fallback);
  fallback.state.depth.compare = DepthCompare::Less;
  const ReplayPrecisionCoverage coverage =
      measureReplayPrecisionCoverage(std::array{frame(fallback)});

  EXPECT_EQ(coverage.fallbackPackets, 1u);
  EXPECT_EQ(coverage.painterOrderedProjectedPackets, 1u);
  EXPECT_EQ(coverage.painterFallbackPackets, 0u);
  EXPECT_EQ(coverage.painterDepthStateGapPackets, 1u);
  EXPECT_EQ(coverage.painterDepthStateGapVertices, 3u);
  EXPECT_FALSE(coverage.donorDepthApplicationComplete());
  EXPECT_FALSE(coverage.fullyHighPrecision());
}

TEST(GpuModernReplayExecutionPlanTest,
     DepthCoveragePartitionChecksCannotWrapOrIgnoreVertexDrift) {
  ReplayPrecisionCoverage packetOverflow;
  packetOverflow.painterPreciseTranslucentPackets =
      std::numeric_limits<uint64_t>::max();
  packetOverflow.painterFallbackPackets = 1;
  EXPECT_FALSE(packetOverflow.donorDepthApplicationComplete());

  ReplayPrecisionCoverage vertexOverflow;
  vertexOverflow.painterPreciseTranslucentVertices =
      std::numeric_limits<uint64_t>::max();
  vertexOverflow.painterFallbackVertices = 1;
  EXPECT_FALSE(vertexOverflow.donorDepthApplicationComplete());

  ReplayPrecisionCoverage vertexDrift;
  vertexDrift.painterOrderedProjectedPackets = 1;
  vertexDrift.painterOrderedProjectedVertices = 4;
  vertexDrift.painterPreciseTranslucentPackets = 1;
  vertexDrift.painterPreciseTranslucentVertices = 3;
  EXPECT_FALSE(vertexDrift.donorDepthApplicationComplete());

  ReplayPrecisionCoverage projectedPacketDrift;
  projectedPacketDrift.projectedPackets = 2;
  projectedPacketDrift.donorDepthEligiblePackets = 1;
  projectedPacketDrift.donorDepthStateMatchedPackets = 1;
  EXPECT_FALSE(projectedPacketDrift.donorDepthApplicationComplete());

  ReplayPrecisionCoverage projectedVertexDrift;
  projectedVertexDrift.projectedPackets = 1;
  projectedVertexDrift.projectedVertices = 4;
  projectedVertexDrift.donorDepthEligiblePackets = 1;
  projectedVertexDrift.donorDepthEligibleVertices = 3;
  projectedVertexDrift.donorDepthStateMatchedPackets = 1;
  projectedVertexDrift.donorDepthStateMatchedVertices = 3;
  EXPECT_FALSE(projectedVertexDrift.donorDepthApplicationComplete());

  ReplayPrecisionCoverage projectedPacketOverflow;
  projectedPacketOverflow.projectedPackets = 0;
  projectedPacketOverflow.donorDepthEligiblePackets =
      std::numeric_limits<uint64_t>::max();
  projectedPacketOverflow.painterOrderedProjectedPackets = 1;
  EXPECT_FALSE(projectedPacketOverflow.donorDepthApplicationComplete());

  ReplayPrecisionCoverage projectedVertexOverflow;
  projectedVertexOverflow.projectedVertices = 0;
  projectedVertexOverflow.donorDepthEligibleVertices =
      std::numeric_limits<uint64_t>::max();
  projectedVertexOverflow.painterOrderedProjectedVertices = 1;
  EXPECT_FALSE(projectedVertexOverflow.donorDepthApplicationComplete());

  ReplayPrecisionCoverage donorVertexMismatch;
  donorVertexMismatch.projectedPackets = 1;
  donorVertexMismatch.projectedVertices = 3;
  donorVertexMismatch.donorDepthEligiblePackets = 1;
  donorVertexMismatch.donorDepthEligibleVertices = 3;
  donorVertexMismatch.donorDepthStateMatchedPackets = 1;
  donorVertexMismatch.donorDepthStateMatchedVertices = 2;
  EXPECT_FALSE(donorVertexMismatch.donorDepthApplicationComplete());
}

TEST(GpuModernReplayExecutionPlanTest,
     PrecisionCoverageVerifiesScreen2DIsExactRatherThanAssumingItsRoute) {
  DrawPacket screen = screenPacket();
  screen.vertices[1].x = 0.25;
  const ReplayPrecisionCoverage coverage =
      measureReplayPrecisionCoverage(std::array{frame(screen)});
  EXPECT_EQ(coverage.screen2DVertices, 3u);
  EXPECT_EQ(coverage.exactScreen2DVertices, 2u);
  EXPECT_FALSE(coverage.screen2DCoordinatesComplete());
  EXPECT_FALSE(coverage.fullyHighPrecision());
}
