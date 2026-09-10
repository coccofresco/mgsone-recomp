#include "runtime/gpu/modern_replay_submission.h"
#include "runtime/metrics.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace ps1::gpu::replay;

namespace {

constexpr NativeRect kAperture{512, 0, 512, 256};
constexpr ReplayPolicyState kPolicy4x{4, true, true, ReplayDepthMode::Hybrid};

FrameLatch latch(uint64_t frameNumber, uint64_t contentRevision,
                 uint64_t submission, uint64_t sequenceBegin,
                 uint64_t sequenceEnd, ReplayPolicyState policy = kPolicy4x) {
  FrameLatch value;
  value.frameNumber = frameNumber;
  value.contentRevision = contentRevision;
  value.completedSubmissionId = submission;
  value.sequenceBegin = sequenceBegin;
  value.sequenceEnd = sequenceEnd;
  value.policy = policy;
  value.display.aperture = kAperture;
  value.display.pal = true;
  return value;
}

CrashReplayFramePtr makeFrame(FrameLatch frameLatch) {
  CrashReplayFrameBuilder builder;
  return builder.seal(std::move(frameLatch));
}

CrashReplayFramePtr makeSeededFrame(FrameLatch frameLatch,
                                    uint64_t nativeRevision = 1,
                                    uint64_t depthEpoch = 1) {
  SurfaceSeed seed;
  seed.destination =
      NativeRect{0, 0, CRASH_REPLAY_NATIVE_WIDTH, CRASH_REPLAY_NATIVE_HEIGHT};
  seed.pixels.resize(static_cast<std::size_t>(CRASH_REPLAY_NATIVE_WIDTH) *
                     CRASH_REPLAY_NATIVE_HEIGHT);
  seed.nativeRevision = nativeRevision;
  seed.depthEpoch = depthEpoch;
  CrashReplayFrameBuilder builder;
  builder.record(std::move(seed));
  return builder.seal(std::move(frameLatch));
}

DrawPacket projectedPacket(uint64_t sequence, bool precise) {
  DrawPacket packet;
  packet.opcode = 0x20;
  packet.packetSequence = sequence;
  packet.submissionId = 10;
  packet.validMask = 0x7;
  packet.vertexCount = 3;
  packet.indexCount = 3;
  packet.triangleIndices = {0, 1, 2, 0, 0, 0};
  packet.state.depth.testEnabled = false;
  packet.state.depth.writeEnabled = true;
  packet.state.depth.compare = DepthCompare::Always;
  packet.state.depth.epoch = 1;
  for (std::size_t index = 0; index < packet.vertexCount; ++index) {
    packet.vertices[index].preciseXY = precise;
    packet.vertices[index].x = 10.25 + static_cast<double>(index);
    packet.vertices[index].y = 20.5 + static_cast<double>(index);
    packet.vertices[index].w = 4096.0 + static_cast<double>(index);
    packet.vertices[index].rawDepth = packet.vertices[index].w;
    packet.vertices[index].nearDepth = 1.0;
    packet.vertices[index].rawDepthValid = true;
    packet.vertices[index].preciseX = precise;
    packet.vertices[index].preciseY = precise;
  }
  return packet;
}

CrashReplayFramePtr makeProjectedFrame(FrameLatch frameLatch, bool precise) {
  CrashReplayFrameBuilder builder;
  builder.record(projectedPacket(frameLatch.sequenceBegin, precise));
  return builder.seal(std::move(frameLatch));
}

ReplayPreflightState anchoredState() {
  ReplayPreflightState state;
  state.timelineInitialized = true;
  state.surfaceInitialized = true;
  state.depthInitialized = true;
  state.depthEpoch = 1;
  state.lastFrameNumber = 9;
  state.contentRevision = 90;
  state.completedSubmissionId = 9;
  state.nextPacketSequence = 100;
  state.nativeTileRevisions.fill(1);
  state.knownNativeTiles.set();
  return state;
}

ModernReplaySubmissionConfig fullConfig() {
  ModernReplaySubmissionConfig config;
  config.expectedPolicy = kPolicy4x;
  config.capabilities = CRASH_REPLAY_V1_ALL_CAPABILITIES;
  return config;
}

ReplayNativeSnapshotIdentity snapshotFor(const CrashReplayFramePtr &frame) {
  ReplayNativeSnapshotIdentity snapshot;
  snapshot.frameNumber = frame->latch().frameNumber;
  snapshot.contentRevision = frame->latch().contentRevision;
  snapshot.completedSubmissionId = frame->latch().completedSubmissionId;
  snapshot.display = frame->latch().display;
  snapshot.sealedFrame = frame;
  return snapshot;
}

} // namespace

static_assert(!std::is_copy_constructible_v<ModernReplaySubmissionPlan>);
static_assert(!std::is_copy_constructible_v<ModernReplaySubmissionCommit>);
static_assert(!std::is_move_assignable_v<ModernReplaySubmissionCommit>);
static_assert(
    noexcept(std::declval<ModernReplaySubmissionScheduler &>().commitSubmitted(
        std::declval<ModernReplaySubmissionPlan &&>())));

TEST(GpuModernReplaySubmissionTest,
     InitialSeedCanReanchorThenFollowingEmptyLatchReplaysAtM1) {
  ModernReplaySubmissionConfig config = fullConfig();
  config.capabilities &= ~replayCapabilityBit(ReplayCapability::SurfaceSeed);
  ModernReplaySubmissionScheduler scheduler(config);

  const auto initial = makeSeededFrame(latch(1, 10, 1, 0, 0));
  scheduler.enqueue(initial);
  ModernReplaySubmissionPlan anchor = scheduler.plan(snapshotFor(initial));
  ASSERT_EQ(anchor.route(), ModernReplaySubmissionRoute::M0Reanchor);
  EXPECT_EQ(anchor.strictReason(), ReplayPreflightReason::MissingCapability);
  EXPECT_EQ(anchor.missingCapabilities(),
            replayCapabilityBit(ReplayCapability::SurfaceSeed));
  EXPECT_EQ(anchor.reason(), ReplayPreflightReason::None);
  ASSERT_EQ(anchor.frames().size(), 1u);
  EXPECT_EQ(anchor.frames().front().get(), initial.get());
  EXPECT_EQ(scheduler.pendingFrameCount(), 1u);
  EXPECT_FALSE(scheduler.scheduledState().timelineInitialized);

  ModernReplaySubmissionCommit anchorCommit =
      scheduler.commitSubmitted(std::move(anchor));
  ASSERT_TRUE(anchorCommit.valid());
  EXPECT_EQ(anchorCommit.route(), ModernReplaySubmissionRoute::M0Reanchor);
  EXPECT_EQ(anchorCommit.queueSerial(), 1u);
  EXPECT_EQ(scheduler.pendingFrameCount(), 0u);
  EXPECT_EQ(scheduler.scheduledState().lastFrameNumber, 1u);
  EXPECT_FALSE(scheduler.confirmedState().timelineInitialized);

  scheduler.confirmQueueOrderedFence(anchorCommit);
  EXPECT_EQ(scheduler.confirmedState().lastFrameNumber, 1u);

  const auto following = makeFrame(latch(2, 10, 1, 0, 0));
  scheduler.enqueue(following);
  ModernReplaySubmissionPlan replay = scheduler.plan(snapshotFor(following));
  ASSERT_EQ(replay.route(), ModernReplaySubmissionRoute::M1Replay);
  EXPECT_EQ(replay.reason(), ReplayPreflightReason::None);
  EXPECT_EQ(replay.missingCapabilities(), 0u);
  EXPECT_EQ(replay.candidateState().lastFrameNumber, 2u);
}

TEST(GpuModernReplaySubmissionTest,
     SaveStateRebaseAcceptsARegressedStrongLatchAndKeepsQueueSerials) {
  ModernReplaySubmissionScheduler scheduler(fullConfig(), anchoredState());

  const auto oldTimeline = makeFrame(latch(10, 100, 10, 100, 100));
  scheduler.enqueue(oldTimeline);
  ModernReplaySubmissionPlan oldPlan = scheduler.plan(snapshotFor(oldTimeline));
  ASSERT_EQ(oldPlan.route(), ModernReplaySubmissionRoute::M1Replay);
  ModernReplaySubmissionCommit oldCommit =
      scheduler.commitSubmitted(std::move(oldPlan));
  ASSERT_TRUE(oldCommit.valid());

  const auto stale = makeFrame(latch(11, 100, 10, 100, 100));
  scheduler.enqueue(stale);
  scheduler.rebaseGuestTimeline();

  EXPECT_EQ(scheduler.pendingFrameCount(), 0u);
  EXPECT_FALSE(scheduler.scheduledState().timelineInitialized);
  EXPECT_TRUE(scheduler.scheduledState().surfaceInitialized);
  EXPECT_EQ(scheduler.confirmedState(), scheduler.scheduledState());
  EXPECT_EQ(scheduler.lastSubmittedQueueSerial(), 1u);
  EXPECT_EQ(scheduler.confirmedQueueSerial(), 1u);

  const auto restored = makeSeededFrame(latch(3, 200, 20, 100, 100), 2, 2);
  scheduler.enqueue(restored);
  ModernReplaySubmissionPlan restoredPlan =
      scheduler.plan(snapshotFor(restored));
  ASSERT_EQ(restoredPlan.route(), ModernReplaySubmissionRoute::M1Replay)
      << replayPreflightReasonName(restoredPlan.reason());
  EXPECT_EQ(restoredPlan.candidateState().lastFrameNumber, 3u);
  ModernReplaySubmissionCommit restoredCommit =
      scheduler.commitSubmitted(std::move(restoredPlan));
  ASSERT_TRUE(restoredCommit.valid());
  EXPECT_EQ(restoredCommit.queueSerial(), 2u);
}

TEST(GpuModernReplaySubmissionTest,
     FullPrecisionPolicyReanchorsInsteadOfAcceptingOneImpreciseVertex) {
  ModernReplaySubmissionConfig config = fullConfig();
  config.requireFullPrecisionCoverage = true;
  ModernReplaySubmissionScheduler scheduler(config, anchoredState());

  const auto imprecise =
      makeProjectedFrame(latch(10, 100, 10, 100, 101), false);
  scheduler.enqueue(imprecise);
  ModernReplaySubmissionPlan fallback = scheduler.plan(snapshotFor(imprecise));
  EXPECT_EQ(fallback.route(), ModernReplaySubmissionRoute::M0Reanchor);
  EXPECT_EQ(fallback.strictReason(),
            ReplayPreflightReason::PrecisionCoverageIncomplete);
  EXPECT_EQ(scheduler.pendingFrameCount(), 1u);
}

TEST(GpuModernReplaySubmissionTest,
     PrecisionRejectTraceUsesImmutableLatchFrameAndSchema) {
  const char *path = "/tmp/ps1_precision_reject_submission_test.jsonl";
  std::remove(path);
  setenv("PS1_TRACE_JSONL", path, 1);
  setenv("PS1_TRACE_MODERN_REJECTS", "1", 1);
  setenv("PS1_TRACE_MODERN_REJECT_FRAME_FROM", "10", 1);
  setenv("PS1_TRACE_MODERN_REJECT_FRAME_TO", "10", 1);
  ps1::metrics::resetTraceForTesting();
  ps1::metrics::setTraceFrame(999);

  ModernReplaySubmissionConfig config = fullConfig();
  config.requireFullPrecisionCoverage = true;
  ModernReplaySubmissionScheduler scheduler(config, anchoredState());
  const auto imprecise =
      makeProjectedFrame(latch(10, 100, 10, 100, 101), false);
  scheduler.enqueue(imprecise);
  const ModernReplaySubmissionPlan fallback =
      scheduler.plan(snapshotFor(imprecise));

  std::ifstream input(path);
  const std::string json((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  input.close();
  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_MODERN_REJECTS");
  unsetenv("PS1_TRACE_MODERN_REJECT_FRAME_FROM");
  unsetenv("PS1_TRACE_MODERN_REJECT_FRAME_TO");
  ps1::metrics::resetTraceForTesting();
  std::remove(path);

  EXPECT_EQ(fallback.route(), ModernReplaySubmissionRoute::M0Reanchor);
  EXPECT_NE(json.find("\"event\":\"replay.precision_coverage_reject\""),
            std::string::npos);
  EXPECT_NE(json.find("\"event\":\"replay.precision_vertex_reject\""),
            std::string::npos);
  EXPECT_NE(json.find("\"frame\":10"), std::string::npos);
  EXPECT_EQ(json.find("\"frame\":999"), std::string::npos);
  EXPECT_NE(json.find("\"reject_schema\":1"), std::string::npos);
  EXPECT_NE(json.find("\"coverage_schema\":2"), std::string::npos);
  EXPECT_NE(json.find("\"homogeneous_q\":0"), std::string::npos);
  EXPECT_NE(json.find("\"depth_matched_vertices\":3"), std::string::npos);
  EXPECT_NE(json.find("\"qx_q16\":"), std::string::npos);
}

TEST(GpuModernReplaySubmissionTest,
     PrecisionRejectTracePublishesPainterAndScreenDepthTaxonomy) {
  const char *path =
      "/tmp/ps1_precision_reject_depth_taxonomy_submission_test.jsonl";
  std::remove(path);
  setenv("PS1_TRACE_JSONL", path, 1);
  setenv("PS1_TRACE_MODERN_REJECTS", "1", 1);
  setenv("PS1_TRACE_MODERN_REJECT_FRAME_FROM", "10", 1);
  setenv("PS1_TRACE_MODERN_REJECT_FRAME_TO", "10", 1);
  ps1::metrics::resetTraceForTesting();

  DrawPacket preciseTranslucent = projectedPacket(100, true);
  preciseTranslucent.opcode = 0x22;
  preciseTranslucent.state.blend.enabled = true;
  preciseTranslucent.state.depth.testEnabled = false;
  preciseTranslucent.state.depth.writeEnabled = false;
  preciseTranslucent.state.depth.compare = DepthCompare::Always;
  preciseTranslucent.state.depth.invalidateOnAccepted = true;

  DrawPacket fallback = projectedPacket(101, false);
  fallback.fallbackReason = PacketFallbackReason::MissingPrecisionPacketAtomic;
  fallback.validMask = 0;
  fallback.state.depth.testEnabled = false;
  fallback.state.depth.writeEnabled = false;
  fallback.state.depth.compare = DepthCompare::Always;
  for (std::size_t index = 0; index < fallback.vertexCount; ++index) {
    ReplayVertex &vertex = fallback.vertices[index];
    vertex.w = CRASH_REPLAY_FALLBACK_W;
    vertex.rawDepth = 0.0;
    vertex.nearDepth = 0.0;
    vertex.rawDepthValid = false;
    vertex.inventedW = true;
  }

  DrawPacket painterDepthGap = projectedPacket(102, true);
  painterDepthGap.opcode = 0x22;
  painterDepthGap.state.blend.enabled = true;
  painterDepthGap.state.depth.testEnabled = false;
  painterDepthGap.state.depth.writeEnabled = true;
  painterDepthGap.state.depth.compare = DepthCompare::Always;

  auto screenPacket = [](uint64_t sequence) {
    DrawPacket packet = projectedPacket(sequence, false);
    packet.route = PacketRoute::Screen2D;
    packet.semanticReason = PacketSemanticReason::Intrinsic2DFamily;
    packet.validMask = 0;
    packet.state.depth.testEnabled = false;
    packet.state.depth.writeEnabled = false;
    packet.state.depth.compare = DepthCompare::Always;
    packet.state.depth.invalidateOnAccepted = true;
    for (std::size_t index = 0; index < packet.vertexCount; ++index) {
      ReplayVertex &vertex = packet.vertices[index];
      vertex.nativeX = 10 + static_cast<int32_t>(index);
      vertex.nativeY = 20 + static_cast<int32_t>(index);
      vertex.x = static_cast<double>(vertex.nativeX);
      vertex.y = static_cast<double>(vertex.nativeY);
      vertex.w = CRASH_REPLAY_FALLBACK_W;
      vertex.rawDepth = 0.0;
      vertex.nearDepth = 0.0;
      vertex.rawDepthValid = false;
      vertex.preciseX = false;
      vertex.preciseY = false;
      vertex.preciseXY = false;
    }
    return packet;
  };

  CrashReplayFrameBuilder builder;
  builder.record(std::move(preciseTranslucent));
  builder.record(std::move(fallback));
  builder.record(std::move(painterDepthGap));
  builder.record(screenPacket(103));
  const CrashReplayFramePtr frame = builder.seal(latch(10, 100, 10, 100, 104));

  ModernReplaySubmissionConfig config = fullConfig();
  config.requireFullPrecisionCoverage = true;
  ModernReplaySubmissionScheduler scheduler(config, anchoredState());
  scheduler.enqueue(frame);
  const ModernReplaySubmissionPlan fallbackPlan =
      scheduler.plan(snapshotFor(frame));

  std::ifstream input(path);
  const std::string json((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  input.close();
  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_MODERN_REJECTS");
  unsetenv("PS1_TRACE_MODERN_REJECT_FRAME_FROM");
  unsetenv("PS1_TRACE_MODERN_REJECT_FRAME_TO");
  ps1::metrics::resetTraceForTesting();
  std::remove(path);

  EXPECT_EQ(fallbackPlan.route(), ModernReplaySubmissionRoute::M0Reanchor);
  EXPECT_EQ(fallbackPlan.strictReason(),
            ReplayPreflightReason::PrecisionCoverageIncomplete);
  const auto expectField = [&](std::string_view name, uint64_t value) {
    const std::string field =
        "\"" + std::string(name) + "\":" + std::to_string(value);
    EXPECT_NE(json.find(field), std::string::npos) << field << '\n' << json;
  };
  expectField("coverage_schema", 2);
  expectField("draw_packets", 4);
  expectField("projected_packets", 3);
  expectField("screen_2d_packets", 1);
  expectField("vertices", 12);
  expectField("painter_projected_packets", 3);
  expectField("painter_projected_vertices", 9);
  expectField("painter_precise_translucent_packets", 1);
  expectField("painter_precise_translucent_vertices", 3);
  expectField("painter_fallback_packets", 1);
  expectField("painter_fallback_vertices", 3);
  expectField("painter_depth_policy_off_packets", 0);
  expectField("painter_depth_policy_off_vertices", 0);
  expectField("painter_depth_state_gap_packets", 1);
  expectField("painter_depth_state_gap_vertices", 3);
  expectField("painter_unexpected_opaque_packets", 0);
  expectField("painter_unexpected_opaque_vertices", 0);
  expectField("screen_2d_depth_invalidate_matched_packets", 1);
  expectField("screen_2d_depth_state_gap_packets", 0);
}

TEST(GpuModernReplaySubmissionTest,
     GuardBandReconciliationCannotCrossFullPrecisionSchedulerGate) {
  ModernReplaySubmissionConfig config = fullConfig();
  config.requireFullPrecisionCoverage = true;
  ModernReplaySubmissionScheduler scheduler(config, anchoredState());

  DrawPacket packet = projectedPacket(100, true);
  packet.vertices[1].reconciledX = true;
  packet.vertices[1].reconciledY = true;
  CrashReplayFrameBuilder builder;
  builder.record(std::move(packet));
  const CrashReplayFramePtr frame = builder.seal(latch(10, 100, 10, 100, 101));
  scheduler.enqueue(frame);

  const ModernReplaySubmissionPlan fallback =
      scheduler.plan(snapshotFor(frame));
  EXPECT_EQ(fallback.route(), ModernReplaySubmissionRoute::M0Reanchor);
  EXPECT_EQ(fallback.strictReason(),
            ReplayPreflightReason::PrecisionCoverageIncomplete);
}

TEST(GpuModernReplaySubmissionTest,
     FullPrecisionPolicyAcceptsEveryVertexOnlyWhenAllProofsMatch) {
  ModernReplaySubmissionConfig config = fullConfig();
  config.requireFullPrecisionCoverage = true;
  ModernReplaySubmissionScheduler scheduler(config, anchoredState());

  const auto precise = makeProjectedFrame(latch(10, 100, 10, 100, 101), true);
  scheduler.enqueue(precise);
  ModernReplaySubmissionPlan replay = scheduler.plan(snapshotFor(precise));
  EXPECT_EQ(replay.route(), ModernReplaySubmissionRoute::M1Replay);
  EXPECT_EQ(replay.strictReason(), ReplayPreflightReason::None);
}

TEST(GpuModernReplaySubmissionTest,
     ExactStrongTargetRetainsFutureAndScalarLookalikeCannotConsume) {
  ModernReplaySubmissionScheduler scheduler(fullConfig(), anchoredState());
  const auto target = makeFrame(latch(10, 100, 10, 100, 100));
  const auto future = makeFrame(latch(11, 101, 11, 100, 100));
  scheduler.enqueue(std::array<CrashReplayFramePtr, 2>{target, future});

  // Every scalar is identical to target, but this object was never queued.
  const auto lookalike = makeFrame(target->latch());
  ModernReplaySubmissionPlan missing = scheduler.plan(snapshotFor(lookalike));
  EXPECT_EQ(missing.route(), ModernReplaySubmissionRoute::M0PresentOnly);
  EXPECT_EQ(missing.reason(), ReplayPreflightReason::TargetLatchUnavailable);
  EXPECT_TRUE(missing.frames().empty());
  EXPECT_EQ(scheduler.pendingFrameCount(), 2u);

  ModernReplaySubmissionPlan selected = scheduler.plan(snapshotFor(target));
  ASSERT_EQ(selected.route(), ModernReplaySubmissionRoute::M1Replay);
  ASSERT_EQ(selected.frames().size(), 1u);
  EXPECT_EQ(selected.frames().front().get(), target.get());
  EXPECT_EQ(scheduler.pendingFrameCount(), 2u);

  const auto commit = scheduler.commitSubmitted(std::move(selected));
  ASSERT_TRUE(commit.valid());
  ASSERT_EQ(scheduler.pendingFrameCount(), 1u);
  EXPECT_EQ(scheduler.pendingFrames().front().get(), future.get());
}

TEST(GpuModernReplaySubmissionTest,
     TailEnqueueBetweenPlanAndSubmitCannotInvalidateExactPrefix) {
  ModernReplaySubmissionScheduler scheduler(fullConfig(), anchoredState());
  const auto target = makeFrame(latch(10, 100, 10, 100, 100));
  scheduler.enqueue(target);
  ModernReplaySubmissionPlan plan = scheduler.plan(snapshotFor(target));
  ASSERT_EQ(plan.route(), ModernReplaySubmissionRoute::M1Replay);

  const auto future = makeFrame(latch(11, 101, 11, 100, 100));
  scheduler.enqueue(future);
  ASSERT_TRUE(scheduler.validatePlanForSubmit(plan));

  const ModernReplaySubmissionCommit commit =
      scheduler.commitSubmitted(std::move(plan));
  ASSERT_TRUE(commit.valid());
  ASSERT_EQ(commit.frames().size(), 1u);
  EXPECT_EQ(commit.frames().front().get(), target.get());
  ASSERT_EQ(scheduler.pendingFrameCount(), 1u);
  EXPECT_EQ(scheduler.pendingFrames().front().get(), future.get());
}

TEST(GpuModernReplaySubmissionTest,
     AcquireRecordAndSubmitFailureLeavePendingAndStatesUnchanged) {
  const ReplayPreflightState initial = anchoredState();
  ModernReplaySubmissionScheduler scheduler(fullConfig(), initial);
  const auto frame = makeFrame(latch(10, 100, 10, 100, 100));
  scheduler.enqueue(frame);

  // Abandoning a plan models acquire failure.
  {
    EXPECT_EQ(scheduler.plan(snapshotFor(frame)).route(),
              ModernReplaySubmissionRoute::M1Replay);
  }
  // A fresh abandoned plan models command-recording failure.
  {
    EXPECT_EQ(scheduler.plan(snapshotFor(frame)).route(),
              ModernReplaySubmissionRoute::M1Replay);
  }
  // vkQueueSubmit failure likewise means commitSubmitted is never called.
  {
    EXPECT_EQ(scheduler.plan(snapshotFor(frame)).route(),
              ModernReplaySubmissionRoute::M1Replay);
  }

  EXPECT_EQ(scheduler.pendingFrameCount(), 1u);
  EXPECT_EQ(scheduler.scheduledState(), initial);
  EXPECT_EQ(scheduler.confirmedState(), initial);
  EXPECT_EQ(scheduler.lastSubmittedQueueSerial(), 0u);

  ModernReplaySubmissionPlan success = scheduler.plan(snapshotFor(frame));
  ASSERT_TRUE(scheduler.validatePlanForSubmit(success));
  const auto commit = scheduler.commitSubmitted(std::move(success));
  ASSERT_TRUE(commit.valid());
  EXPECT_EQ(scheduler.pendingFrameCount(), 0u);
  EXPECT_EQ(scheduler.scheduledState().lastFrameNumber, 10u);

  // A failed/out-of-date present is intentionally not an input to the
  // scheduler: queue submission has already made the replay commit final.
  constexpr bool presentSucceeded = false;
  EXPECT_FALSE(presentSucceeded);
  EXPECT_EQ(scheduler.pendingFrameCount(), 0u);
  EXPECT_EQ(scheduler.scheduledState().lastFrameNumber, 10u);
}

TEST(GpuModernReplaySubmissionTest,
     CommitOwnsConsumedFramesUntilFrameContextReleasesIt) {
  ModernReplaySubmissionScheduler scheduler(fullConfig(), anchoredState());
  CrashReplayFramePtr frame = makeFrame(latch(10, 100, 10, 100, 100));
  std::weak_ptr<const CrashReplayFrame> weak = frame;
  ReplayNativeSnapshotIdentity snapshot = snapshotFor(frame);
  scheduler.enqueue(frame);

  {
    ModernReplaySubmissionPlan plan = scheduler.plan(snapshot);
    ModernReplaySubmissionCommit commit =
        scheduler.commitSubmitted(std::move(plan));
    ASSERT_TRUE(commit.valid());
    ASSERT_EQ(commit.frames().size(), 1u);
    frame.reset();
    snapshot.sealedFrame.reset();
    EXPECT_FALSE(weak.expired());

    scheduler.confirmQueueOrderedFence(commit);
    EXPECT_FALSE(weak.expired());
  }
  EXPECT_TRUE(weak.expired());
}

TEST(GpuModernReplaySubmissionTest,
     QueueOrderedFenceConfirmationNeverRegressesWhenPolledOutOfOrder) {
  ModernReplaySubmissionScheduler scheduler(fullConfig(), anchoredState());
  const auto first = makeFrame(latch(10, 100, 10, 100, 100));
  scheduler.enqueue(first);
  ModernReplaySubmissionPlan firstPlan = scheduler.plan(snapshotFor(first));
  ModernReplaySubmissionCommit firstCommit =
      scheduler.commitSubmitted(std::move(firstPlan));
  ASSERT_TRUE(firstCommit.valid());

  const auto second = makeFrame(latch(11, 101, 11, 100, 100));
  scheduler.enqueue(second);
  ModernReplaySubmissionPlan secondPlan = scheduler.plan(snapshotFor(second));
  ModernReplaySubmissionCommit secondCommit =
      scheduler.commitSubmitted(std::move(secondPlan));
  ASSERT_TRUE(secondCommit.valid());
  ASSERT_EQ(secondCommit.queueSerial(), 2u);

  scheduler.confirmQueueOrderedFence(secondCommit);
  EXPECT_EQ(scheduler.confirmedQueueSerial(), 2u);
  EXPECT_EQ(scheduler.confirmedState().lastFrameNumber, 11u);
  scheduler.confirmQueueOrderedFence(firstCommit);
  EXPECT_EQ(scheduler.confirmedQueueSerial(), 2u);
  EXPECT_EQ(scheduler.confirmedState().lastFrameNumber, 11u);
}

TEST(GpuModernReplaySubmissionTest,
     ForeignCommitCannotAdvanceAnotherSchedulersConfirmedState) {
  const ReplayPreflightState initial = anchoredState();
  ModernReplaySubmissionScheduler owner(fullConfig(), initial);
  ModernReplaySubmissionScheduler foreign(fullConfig(), initial);
  const auto frame = makeFrame(latch(10, 100, 10, 100, 100));
  owner.enqueue(frame);
  ModernReplaySubmissionPlan plan = owner.plan(snapshotFor(frame));
  const ModernReplaySubmissionCommit commit =
      owner.commitSubmitted(std::move(plan));
  ASSERT_TRUE(commit.valid());

  foreign.confirmQueueOrderedFence(commit);
  EXPECT_EQ(foreign.confirmedQueueSerial(), 0u);
  EXPECT_EQ(foreign.confirmedState(), initial);
  owner.confirmQueueOrderedFence(commit);
  EXPECT_EQ(owner.confirmedQueueSerial(), 1u);
  EXPECT_EQ(owner.confirmedState().lastFrameNumber, 10u);
}

TEST(GpuModernReplaySubmissionTest,
     MovingACommitInvalidatesTheSourceBeforeFenceConfirmation) {
  const ReplayPreflightState initial = anchoredState();
  ModernReplaySubmissionScheduler scheduler(fullConfig(), initial);
  const auto frame = makeFrame(latch(10, 100, 10, 100, 100));
  scheduler.enqueue(frame);
  ModernReplaySubmissionPlan plan = scheduler.plan(snapshotFor(frame));
  ModernReplaySubmissionCommit source =
      scheduler.commitSubmitted(std::move(plan));
  ASSERT_TRUE(source.valid());

  ModernReplaySubmissionCommit frameContext = std::move(source);
  EXPECT_FALSE(source.valid());
  EXPECT_TRUE(frameContext.valid());
  scheduler.confirmQueueOrderedFence(source);
  EXPECT_EQ(scheduler.confirmedQueueSerial(), 0u);
  EXPECT_EQ(scheduler.confirmedState(), initial);
  scheduler.confirmQueueOrderedFence(frameContext);
  EXPECT_EQ(scheduler.confirmedQueueSerial(), 1u);
  EXPECT_EQ(scheduler.confirmedState().lastFrameNumber, 10u);
}

TEST(GpuModernReplaySubmissionTest,
     HeadGenerationRejectsAStalePlanEvenWhenFramePointersCollide) {
  ModernReplaySubmissionScheduler scheduler(fullConfig(), anchoredState());
  const auto repeated = makeFrame(latch(10, 100, 10, 100, 100));
  scheduler.enqueue(std::array<CrashReplayFramePtr, 2>{repeated, repeated});

  ModernReplaySubmissionPlan first = scheduler.plan(snapshotFor(repeated));
  ModernReplaySubmissionPlan stale = scheduler.plan(snapshotFor(repeated));
  ASSERT_EQ(first.frames().size(), 1u);
  ASSERT_EQ(stale.frames().size(), 1u);
  const auto firstCommit = scheduler.commitSubmitted(std::move(first));
  ASSERT_TRUE(firstCommit.valid());
  ASSERT_EQ(scheduler.pendingFrameCount(), 1u);

  const ReplayPreflightState afterFirst = scheduler.scheduledState();
  EXPECT_FALSE(scheduler.validatePlanForSubmit(stale));
  const auto staleCommit = scheduler.commitSubmitted(std::move(stale));
  EXPECT_FALSE(staleCommit.valid());
  EXPECT_EQ(scheduler.pendingFrameCount(), 1u);
  EXPECT_EQ(scheduler.scheduledState(), afterFirst);
  EXPECT_EQ(scheduler.lastSubmittedQueueSerial(), 1u);
}

TEST(GpuModernReplaySubmissionTest,
     ExecutorDowngradeKeepsExactPrefixPendingUntilNativeSubmitCommits) {
  const ReplayPreflightState initial = anchoredState();
  ModernReplaySubmissionScheduler scheduler(fullConfig(), initial);
  const auto target = makeFrame(latch(10, 100, 10, 100, 100));
  const auto future = makeFrame(latch(11, 101, 11, 100, 100));
  scheduler.enqueue(target);
  ModernReplaySubmissionPlan geometric = scheduler.plan(snapshotFor(target));
  ASSERT_EQ(geometric.route(), ModernReplaySubmissionRoute::M1Replay);

  // A tail enqueue is legal and must not widen the exact prefix selected by
  // the strong target token.
  scheduler.enqueue(future);
  ModernReplaySubmissionPlan native = scheduler.downgradeToNativeReanchor(
      std::move(geometric), snapshotFor(target));
  ASSERT_EQ(native.route(), ModernReplaySubmissionRoute::M0Reanchor);
  EXPECT_EQ(native.strictReason(), ReplayPreflightReason::MissingCapability);
  EXPECT_EQ(native.reason(), ReplayPreflightReason::None);
  ASSERT_EQ(native.frames().size(), 1u);
  EXPECT_EQ(native.frames().front().get(), target.get());
  EXPECT_EQ(scheduler.pendingFrameCount(), 2u);
  EXPECT_EQ(scheduler.scheduledState(), initial);
  EXPECT_FALSE(scheduler.validatePlanForSubmit(geometric));
  ASSERT_TRUE(scheduler.validatePlanForSubmit(native));

  ModernReplaySubmissionCommit commit =
      scheduler.commitSubmitted(std::move(native));
  ASSERT_TRUE(commit.valid());
  EXPECT_EQ(commit.route(), ModernReplaySubmissionRoute::M0Reanchor);
  ASSERT_EQ(scheduler.pendingFrameCount(), 1u);
  EXPECT_EQ(scheduler.pendingFrames().front().get(), future.get());
  EXPECT_EQ(scheduler.scheduledState().lastFrameNumber, 10u);
}

TEST(GpuModernReplaySubmissionTest,
     ExecutorDowngradeRejectsForeignStaleAndLookalikeTargets) {
  const auto target = makeFrame(latch(10, 100, 10, 100, 100));
  const auto lookalike = makeFrame(target->latch());

  ModernReplaySubmissionScheduler owner(fullConfig(), anchoredState());
  ModernReplaySubmissionScheduler foreign(fullConfig(), anchoredState());
  owner.enqueue(target);
  foreign.enqueue(target);
  ModernReplaySubmissionPlan foreignPlan = owner.plan(snapshotFor(target));
  ModernReplaySubmissionPlan foreignRejected =
      foreign.downgradeToNativeReanchor(std::move(foreignPlan),
                                        snapshotFor(target));
  EXPECT_EQ(foreignRejected.route(),
            ModernReplaySubmissionRoute::M0PresentOnly);
  EXPECT_FALSE(owner.validatePlanForSubmit(foreignPlan));
  EXPECT_EQ(owner.pendingFrameCount(), 1u);
  EXPECT_EQ(foreign.pendingFrameCount(), 1u);

  ModernReplaySubmissionPlan lookalikePlan = owner.plan(snapshotFor(target));
  ModernReplaySubmissionPlan lookalikeRejected =
      owner.downgradeToNativeReanchor(std::move(lookalikePlan),
                                      snapshotFor(lookalike));
  EXPECT_EQ(lookalikeRejected.route(),
            ModernReplaySubmissionRoute::M0PresentOnly);
  EXPECT_FALSE(owner.validatePlanForSubmit(lookalikePlan));
  EXPECT_EQ(owner.pendingFrameCount(), 1u);

  ModernReplaySubmissionPlan winner = owner.plan(snapshotFor(target));
  ModernReplaySubmissionPlan stale = owner.plan(snapshotFor(target));
  ASSERT_TRUE(owner.commitSubmitted(std::move(winner)).valid());
  ModernReplaySubmissionPlan staleRejected =
      owner.downgradeToNativeReanchor(std::move(stale), snapshotFor(target));
  EXPECT_EQ(staleRejected.route(), ModernReplaySubmissionRoute::M0PresentOnly);
  EXPECT_EQ(owner.pendingFrameCount(), 0u);
}

TEST(GpuModernReplaySubmissionTest,
     IncompleteUnsupportedAndStructuralErrorsRemainPendingWithoutOptIn) {
  {
    ModernReplaySubmissionScheduler scheduler(fullConfig(), anchoredState());
    FrameLatch incompleteLatch = latch(10, 100, 10, 100, 100);
    incompleteLatch.complete = false;
    const auto incomplete = makeFrame(incompleteLatch);
    scheduler.enqueue(incomplete);
    ModernReplaySubmissionPlan plan = scheduler.plan(snapshotFor(incomplete));
    EXPECT_EQ(plan.route(), ModernReplaySubmissionRoute::M0PresentOnly);
    EXPECT_EQ(plan.reason(), ReplayPreflightReason::FrameIncomplete);
    EXPECT_FALSE(plan.advancesReplayState());
    EXPECT_EQ(scheduler.pendingFrameCount(), 1u);
    EXPECT_EQ(scheduler.scheduledState(), anchoredState());
  }

  {
    ModernReplaySubmissionScheduler scheduler(fullConfig(), anchoredState());
    FrameLatch unsupportedLatch = latch(10, 100, 10, 100, 100);
    unsupportedLatch.complete = false;
    unsupportedLatch.unsupportedCount = 1;
    const auto unsupported = makeFrame(unsupportedLatch);
    scheduler.enqueue(unsupported);
    ModernReplaySubmissionPlan plan = scheduler.plan(snapshotFor(unsupported));
    EXPECT_EQ(plan.route(), ModernReplaySubmissionRoute::M0PresentOnly);
    EXPECT_EQ(plan.reason(), ReplayPreflightReason::UnsupportedMutationCount);
    EXPECT_FALSE(plan.advancesReplayState());
    EXPECT_EQ(scheduler.pendingFrameCount(), 1u);
    EXPECT_EQ(scheduler.scheduledState(), anchoredState());
  }

  {
    ModernReplaySubmissionScheduler scheduler(fullConfig(), anchoredState());
    const auto sequenceGap = makeFrame(latch(10, 100, 10, 101, 101));
    scheduler.enqueue(sequenceGap);
    ModernReplaySubmissionPlan plan = scheduler.plan(snapshotFor(sequenceGap));
    EXPECT_EQ(plan.route(), ModernReplaySubmissionRoute::M0PresentOnly);
    EXPECT_EQ(plan.reason(), ReplayPreflightReason::SequenceRangeDiscontinuity);
    EXPECT_EQ(scheduler.pendingFrameCount(), 1u);
    EXPECT_EQ(scheduler.scheduledState(), anchoredState());
  }
}

TEST(GpuModernReplaySubmissionTest,
     IncompleteNativeReanchorOptInConsumesExactPrefixAndUnblocksNextFrame) {
  const auto exercise = [](bool unsupported) {
    SCOPED_TRACE(unsupported ? "unsupported" : "incomplete");
    ModernReplaySubmissionConfig config = fullConfig();
    config.allowIncompleteNativeReanchor = true;
    ModernReplaySubmissionScheduler scheduler(config, anchoredState());

    FrameLatch brokenLatch = latch(10, 100, 10, 100, 101);
    brokenLatch.complete = false;
    if (unsupported)
      brokenLatch.unsupportedCount = 1;
    const auto broken = makeFrame(brokenLatch);
    const auto target = makeFrame(latch(11, 101, 11, 101, 101));
    scheduler.enqueue(std::array<CrashReplayFramePtr, 2>{broken, target});

    const ReplayPreflightReason expectedStrictReason =
        unsupported ? ReplayPreflightReason::UnsupportedMutationCount
                    : ReplayPreflightReason::FrameIncomplete;
    ModernReplaySubmissionPlan anchor = scheduler.plan(snapshotFor(target));
    ASSERT_EQ(anchor.route(), ModernReplaySubmissionRoute::M0Reanchor);
    EXPECT_EQ(anchor.strictReason(), expectedStrictReason);
    EXPECT_EQ(anchor.reason(), ReplayPreflightReason::None);
    ASSERT_EQ(anchor.frames().size(), 2u);
    EXPECT_EQ(anchor.frames()[0].get(), broken.get());
    EXPECT_EQ(anchor.frames()[1].get(), target.get());
    EXPECT_EQ(anchor.candidateState().lastFrameNumber, 11u);
    EXPECT_EQ(anchor.candidateState().nextPacketSequence, 101u);
    EXPECT_EQ(scheduler.pendingFrameCount(), 2u);
    EXPECT_EQ(scheduler.scheduledState(), anchoredState());

    ASSERT_TRUE(scheduler.validatePlanForSubmit(anchor));
    ModernReplaySubmissionCommit commit =
        scheduler.commitSubmitted(std::move(anchor));
    ASSERT_TRUE(commit.valid());
    EXPECT_EQ(commit.route(), ModernReplaySubmissionRoute::M0Reanchor);
    ASSERT_EQ(commit.frames().size(), 2u);
    EXPECT_EQ(scheduler.pendingFrameCount(), 0u);
    EXPECT_EQ(scheduler.scheduledState().lastFrameNumber, 11u);
    EXPECT_EQ(scheduler.scheduledState().nextPacketSequence, 101u);

    const auto following = makeFrame(latch(12, 102, 12, 101, 101));
    scheduler.enqueue(following);
    ModernReplaySubmissionPlan replay = scheduler.plan(snapshotFor(following));
    EXPECT_EQ(replay.route(), ModernReplaySubmissionRoute::M1Replay);
    EXPECT_EQ(replay.strictReason(), ReplayPreflightReason::None);
    EXPECT_EQ(replay.candidateState().lastFrameNumber, 12u);
  };

  exercise(false);
  exercise(true);

  ModernReplaySubmissionConfig config = fullConfig();
  config.allowIncompleteNativeReanchor = true;
  ModernReplaySubmissionScheduler scheduler(config, anchoredState());
  FrameLatch unrecoverableLatch = latch(10, 100, 10, 100, 101);
  unrecoverableLatch.complete = false;
  unrecoverableLatch.unsupportedCount = 1;
  unrecoverableLatch.unrecoverableCount = 1;
  const auto unrecoverable = makeFrame(unrecoverableLatch);
  scheduler.enqueue(unrecoverable);

  ModernReplaySubmissionPlan rejected =
      scheduler.plan(snapshotFor(unrecoverable));
  EXPECT_EQ(rejected.route(), ModernReplaySubmissionRoute::M0PresentOnly);
  EXPECT_EQ(rejected.strictReason(),
            ReplayPreflightReason::UnsupportedMutationCount);
  EXPECT_EQ(rejected.reason(), ReplayPreflightReason::UnsupportedMutationCount);
  EXPECT_FALSE(rejected.advancesReplayState());
  EXPECT_EQ(scheduler.pendingFrameCount(), 1u);
  EXPECT_EQ(scheduler.scheduledState(), anchoredState());
}

TEST(GpuModernReplaySubmissionTest,
     FourTimesPolicyIsIndependentFromTheCapturedLatch) {
  ModernReplaySubmissionScheduler scheduler(fullConfig(), anchoredState());
  ReplayPolicyState oneX = kPolicy4x;
  oneX.internalScale = 1;
  const auto wrongPolicy = makeFrame(latch(10, 100, 10, 100, 100, oneX));
  scheduler.enqueue(wrongPolicy);

  ModernReplaySubmissionPlan plan = scheduler.plan(snapshotFor(wrongPolicy));
  EXPECT_EQ(plan.route(), ModernReplaySubmissionRoute::M0PresentOnly);
  EXPECT_EQ(plan.strictReason(), ReplayPreflightReason::PolicyMismatch);
  EXPECT_EQ(plan.reason(), ReplayPreflightReason::PolicyMismatch);
  EXPECT_EQ(scheduler.pendingFrameCount(), 1u);
  EXPECT_EQ(scheduler.scheduledState(), anchoredState());
}

TEST(GpuModernReplaySubmissionTest,
     MissingTokenAndInvalidNearClipNeverBecomeSnapshotReanchors) {
  ModernReplaySubmissionConfig config = fullConfig();
  config.nearClipMode = ReplayNearClipMode::GeometricClip;
  ModernReplaySubmissionScheduler scheduler(config, anchoredState());
  const auto frame = makeFrame(latch(10, 100, 10, 100, 100));
  scheduler.enqueue(frame);

  ReplayNativeSnapshotIdentity noToken = snapshotFor(frame);
  noToken.sealedFrame.reset();
  ModernReplaySubmissionPlan missing = scheduler.plan(noToken);
  EXPECT_EQ(missing.route(), ModernReplaySubmissionRoute::M0PresentOnly);
  EXPECT_EQ(missing.reason(), ReplayPreflightReason::TargetLatchUnavailable);

  ModernReplaySubmissionPlan invalidClip = scheduler.plan(snapshotFor(frame));
  EXPECT_EQ(invalidClip.route(), ModernReplaySubmissionRoute::M0PresentOnly);
  EXPECT_EQ(invalidClip.strictReason(),
            ReplayPreflightReason::InvalidNearClipMode);
  EXPECT_EQ(invalidClip.reason(), ReplayPreflightReason::InvalidNearClipMode);
  EXPECT_FALSE(scheduler.validatePlanForSubmit(invalidClip));
  EXPECT_EQ(scheduler.pendingFrameCount(), 1u);
  EXPECT_EQ(scheduler.scheduledState(), anchoredState());
}

TEST(GpuModernReplaySubmissionTest, NullBatchIsRejectedAtomically) {
  ModernReplaySubmissionScheduler scheduler(fullConfig(), anchoredState());
  const auto frame = makeFrame(latch(10, 100, 10, 100, 100));
  const std::array<CrashReplayFramePtr, 2> batch{frame, {}};
  EXPECT_THROW(scheduler.enqueue(batch), std::invalid_argument);
  EXPECT_EQ(scheduler.pendingFrameCount(), 0u);
  EXPECT_THROW(scheduler.enqueue(CrashReplayFramePtr{}), std::invalid_argument);
  EXPECT_EQ(scheduler.pendingFrameCount(), 0u);
}
