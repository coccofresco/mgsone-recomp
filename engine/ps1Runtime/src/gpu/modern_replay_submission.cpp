#include "runtime/gpu/modern_replay_submission.h"

#include "runtime/gpu/modern_replay_execution_plan.h"
#include "runtime/metrics.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ps1::gpu::replay {

namespace {

// Append-only diagnostic ABIs. reject_schema versions the packet/vertex bit
// mask, while coverage_schema versions the aggregate numerator/denominator
// fields independently so adding coverage classifications does not silently
// change the meaning of an existing reject mask.
constexpr int64_t kPrecisionRejectTraceSchema = 1;
constexpr int64_t kPrecisionCoverageTraceSchema = 2;

enum class PrecisionRejectBit : uint8_t {
  MissingOrNonFiniteX = 0,
  MissingOrNonFiniteY = 1,
  IncompleteXY = 2,
  InvalidDepthProvenance = 3,
  ReconciledX = 4,
  ReconciledY = 5,
  InventedW = 6,
  IncompleteTextureUV = 7,
  InexactScreen2D = 8,
  PacketFallback = 9,
  DonorDepthState = 10,
};

constexpr uint32_t rejectBit(PrecisionRejectBit bit) noexcept {
  return 1u << static_cast<uint8_t>(bit);
}

FrameLatch
expectedLatchForSnapshot(const ReplayNativeSnapshotIdentity &snapshot) {
  FrameLatch expected;
  if (snapshot.sealedFrame)
    expected = snapshot.sealedFrame->latch();
  expected.frameNumber = snapshot.frameNumber;
  expected.contentRevision = snapshot.contentRevision;
  expected.completedSubmissionId = snapshot.completedSubmissionId;
  expected.display = snapshot.display;
  return expected;
}

bool exactPrefixReturned(std::span<const CrashReplayFramePtr> expected,
                         std::span<const CrashReplayFramePtr> actual) {
  if (expected.size() != actual.size())
    return false;
  return std::equal(
      expected.begin(), expected.end(), actual.begin(),
      [](const CrashReplayFramePtr &left, const CrashReplayFramePtr &right) {
        return left.get() == right.get();
      });
}

int64_t traceQ16(double value) {
  if (!std::isfinite(value))
    return std::numeric_limits<int64_t>::min();
  constexpr double scale = 65536.0;
  const double scaled = value * scale;
  if (scaled <= static_cast<double>(std::numeric_limits<int64_t>::min()))
    return std::numeric_limits<int64_t>::min();
  if (scaled >= static_cast<double>(std::numeric_limits<int64_t>::max()))
    return std::numeric_limits<int64_t>::max();
  return static_cast<int64_t>(std::llround(scaled));
}

uint32_t precisionVertexRejectMask(const DrawPacket &packet,
                                   std::size_t vertexIndex) {
  const ReplayVertex &vertex = packet.vertices[vertexIndex];
  if (packet.route == PacketRoute::Screen2D) {
    const bool exact = !vertex.preciseXY &&
                       vertex.x == static_cast<double>(vertex.nativeX) &&
                       vertex.y == static_cast<double>(vertex.nativeY) &&
                       vertex.w == CRASH_REPLAY_FALLBACK_W;
    return exact ? 0u : rejectBit(PrecisionRejectBit::InexactScreen2D);
  }

  uint32_t mask = 0;
  if (!vertex.preciseX || !std::isfinite(vertex.x))
    mask |= rejectBit(PrecisionRejectBit::MissingOrNonFiniteX);
  if (!vertex.preciseY || !std::isfinite(vertex.y))
    mask |= rejectBit(PrecisionRejectBit::MissingOrNonFiniteY);
  if (!vertex.preciseXY || !vertex.preciseX || !vertex.preciseY ||
      !std::isfinite(vertex.x) || !std::isfinite(vertex.y)) {
    mask |= rejectBit(PrecisionRejectBit::IncompleteXY);
  }
  if (!vertex.rawDepthValid || (packet.validMask & (1u << vertexIndex)) == 0 ||
      !std::isfinite(vertex.rawDepth) || !std::isfinite(vertex.nearDepth)) {
    mask |= rejectBit(PrecisionRejectBit::InvalidDepthProvenance);
  }
  if (vertex.reconciledX)
    mask |= rejectBit(PrecisionRejectBit::ReconciledX);
  if (vertex.reconciledY)
    mask |= rejectBit(PrecisionRejectBit::ReconciledY);
  if (vertex.inventedW)
    mask |= rejectBit(PrecisionRejectBit::InventedW);
  if (packet.textured && (!vertex.preciseUV || !std::isfinite(vertex.u) ||
                          !std::isfinite(vertex.v))) {
    mask |= rejectBit(PrecisionRejectBit::IncompleteTextureUV);
  }
  return mask;
}

void tracePrecisionCoverageReject(const ReplayNativeSnapshotIdentity &snapshot,
                                  std::span<const CrashReplayFramePtr> frames,
                                  const ReplayPrecisionCoverage &coverage) {
  const bool aggregateSelected =
      ps1::metrics::modernRejectTraceEnabledAtFrame(snapshot.frameNumber);
  const bool anyFrameSelected =
      std::any_of(frames.begin(), frames.end(), [](const auto &frame) {
        return frame && ps1::metrics::modernRejectTraceEnabledAtFrame(
                            frame->latch().frameNumber);
      });
  if (!aggregateSelected && !anyFrameSelected)
    return;

  if (aggregateSelected) {
    ps1::metrics::traceAtFrame(
        "replay.precision_coverage_reject", snapshot.frameNumber,
        {{"reject_schema", kPrecisionRejectTraceSchema},
         {"coverage_schema", kPrecisionCoverageTraceSchema},
         {"batch_frames", static_cast<int64_t>(coverage.frames)},
         {"null_frames", static_cast<int64_t>(coverage.nullFrames)},
         {"incomplete_frames", static_cast<int64_t>(coverage.incompleteFrames)},
         {"unsupported_mutations",
          static_cast<int64_t>(coverage.unsupportedMutations)},
         {"capture_complete", coverage.captureComplete() ? 1 : 0},
         {"projected_coordinates_complete",
          coverage.projectedCoordinatesComplete() ? 1 : 0},
         {"projected_depth_complete",
          coverage.projectedDepthProvenanceComplete() ? 1 : 0},
         {"projected_uv_complete",
          coverage.projectedTextureCoordinatesComplete() ? 1 : 0},
         {"screen_2d_complete", coverage.screen2DCoordinatesComplete() ? 1 : 0},
         {"donor_depth_complete",
          coverage.donorDepthApplicationComplete() ? 1 : 0},
         {"draw_packets", static_cast<int64_t>(coverage.drawPackets)},
         {"projected_packets", static_cast<int64_t>(coverage.projectedPackets)},
         {"screen_2d_packets", static_cast<int64_t>(coverage.screen2DPackets)},
         {"vertices", static_cast<int64_t>(coverage.vertices)},
         {"projected_vertices",
          static_cast<int64_t>(coverage.projectedVertices)},
         {"precise_x",
          static_cast<int64_t>(coverage.preciseProjectedXVertices)},
         {"precise_y",
          static_cast<int64_t>(coverage.preciseProjectedYVertices)},
         {"precise_xy",
          static_cast<int64_t>(coverage.preciseProjectedXYVertices)},
         {"valid_w", static_cast<int64_t>(coverage.validProjectedWVertices)},
         {"homogeneous_q",
          static_cast<int64_t>(coverage.homogeneousProjectedQVertices)},
         {"reconciled_x",
          static_cast<int64_t>(coverage.reconciledProjectedXVertices)},
         {"reconciled_y",
          static_cast<int64_t>(coverage.reconciledProjectedYVertices)},
         {"invented_w",
          static_cast<int64_t>(coverage.inventedProjectedWVertices)},
         {"near_saturated",
          static_cast<int64_t>(coverage.nearSaturatedProjectedVertices)},
         {"textured_vertices",
          static_cast<int64_t>(coverage.texturedProjectedVertices)},
         {"precise_uv",
          static_cast<int64_t>(coverage.preciseProjectedUVVertices)},
         {"screen_2d_vertices",
          static_cast<int64_t>(coverage.screen2DVertices)},
         {"screen_2d_exact",
          static_cast<int64_t>(coverage.exactScreen2DVertices)},
         {"fallback_packets", static_cast<int64_t>(coverage.fallbackPackets)},
         {"depth_eligible_packets",
          static_cast<int64_t>(coverage.donorDepthEligiblePackets)},
         {"depth_eligible_vertices",
          static_cast<int64_t>(coverage.donorDepthEligibleVertices)},
         {"depth_matched_packets",
          static_cast<int64_t>(coverage.donorDepthStateMatchedPackets)},
         {"depth_matched_vertices",
          static_cast<int64_t>(coverage.donorDepthStateMatchedVertices)},
         {"painter_projected_packets",
          static_cast<int64_t>(coverage.painterOrderedProjectedPackets)},
         {"painter_projected_vertices",
          static_cast<int64_t>(coverage.painterOrderedProjectedVertices)},
         {"painter_precise_translucent_packets",
          static_cast<int64_t>(coverage.painterPreciseTranslucentPackets)},
         {"painter_precise_translucent_vertices",
          static_cast<int64_t>(coverage.painterPreciseTranslucentVertices)},
         {"ot_visibility_boundary_packets",
          static_cast<int64_t>(coverage.otVisibilityBoundaryPackets)},
         {"ot_visibility_boundary_vertices",
          static_cast<int64_t>(coverage.otVisibilityBoundaryVertices)},
         {"painter_fallback_packets",
          static_cast<int64_t>(coverage.painterFallbackPackets)},
         {"painter_fallback_vertices",
          static_cast<int64_t>(coverage.painterFallbackVertices)},
         {"painter_depth_policy_off_packets",
          static_cast<int64_t>(coverage.painterDepthPolicyOffPackets)},
         {"painter_depth_policy_off_vertices",
          static_cast<int64_t>(coverage.painterDepthPolicyOffVertices)},
         {"painter_depth_state_gap_packets",
          static_cast<int64_t>(coverage.painterDepthStateGapPackets)},
         {"painter_depth_state_gap_vertices",
          static_cast<int64_t>(coverage.painterDepthStateGapVertices)},
         {"painter_unexpected_opaque_packets",
          static_cast<int64_t>(coverage.painterUnexpectedOpaquePackets)},
         {"painter_unexpected_opaque_vertices",
          static_cast<int64_t>(coverage.painterUnexpectedOpaqueVertices)},
         {"screen_2d_depth_invalidate_matched_packets",
          static_cast<int64_t>(coverage.screen2DDepthInvalidateMatchedPackets)},
         {"screen_2d_depth_state_gap_packets",
          static_cast<int64_t>(coverage.screen2DDepthStateGapPackets)}});
  }

  for (std::size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
    const CrashReplayFramePtr &frame = frames[frameIndex];
    if (!frame || !ps1::metrics::modernRejectTraceEnabledAtFrame(
                      frame->latch().frameNumber))
      continue;
    const bool depthRequested =
        frame->latch().policy.pgxp &&
        frame->latch().policy.trueDepthMode != ReplayDepthMode::Off;
    const std::span<const Operation> operations = frame->operations();
    for (std::size_t operationIndex = 0; operationIndex < operations.size();
         ++operationIndex) {
      const DrawPacket *packet =
          std::get_if<DrawPacket>(&operations[operationIndex]);
      if (packet == nullptr)
        continue;

      const bool depthEligible =
          depthRequested && donorTrueDepthEligible(*packet);
      const DepthState &depth = packet->state.depth;
      const bool translucentBoundaryExpected =
          frame->latch().policy.pgxp &&
          frame->latch().policy.trueDepthMode == ReplayDepthMode::Hybrid &&
          packet->route == PacketRoute::Projected3D &&
          packet->state.blend.enabled;
      const bool depthMatched =
          depthEligible
              ? (frame->latch().policy.trueDepthMode == ReplayDepthMode::Hybrid
                     ? (!depth.testEnabled && depth.writeEnabled &&
                        depth.compare == DepthCompare::Always &&
                        !depth.invalidateOnAccepted)
                     : (depth.testEnabled && depth.writeEnabled &&
                        depth.compare == DepthCompare::LessOrEqual &&
                        !depth.invalidateOnAccepted))
          : translucentBoundaryExpected
              ? (!depth.testEnabled && !depth.writeEnabled &&
                 depth.compare == DepthCompare::Always &&
                 depth.invalidateOnAccepted)
              : true;
      const uint32_t packetRejectMask =
          (packet->fallbackReason != PacketFallbackReason::None
               ? rejectBit(PrecisionRejectBit::PacketFallback)
               : 0u) |
          (!depthMatched ? rejectBit(PrecisionRejectBit::DonorDepthState) : 0u);
      if (packetRejectMask != 0) {
        ps1::metrics::traceAtFrame(
            "replay.precision_packet_reject", frame->latch().frameNumber,
            {{"reject_schema", kPrecisionRejectTraceSchema},
             {"batch_frame_index", static_cast<int64_t>(frameIndex)},
             {"operation_index", static_cast<int64_t>(operationIndex)},
             {"packet_sequence", static_cast<int64_t>(packet->packetSequence)},
             {"submission_id", static_cast<int64_t>(packet->submissionId)},
             {"opcode", packet->opcode},
             {"route", static_cast<int64_t>(packet->route)},
             {"semantic_reason", static_cast<int64_t>(packet->semanticReason)},
             {"fallback_reason", static_cast<int64_t>(packet->fallbackReason)},
             {"reject_mask", packetRejectMask},
             {"valid_mask", packet->validMask},
             {"vertex_count", packet->vertexCount},
             {"depth_test", depth.testEnabled ? 1 : 0},
             {"depth_write", depth.writeEnabled ? 1 : 0},
             {"depth_compare", static_cast<int64_t>(depth.compare)},
             {"depth_invalidate", depth.invalidateOnAccepted ? 1 : 0}});
      }

      for (std::size_t vertexIndex = 0; vertexIndex < packet->vertexCount;
           ++vertexIndex) {
        const uint32_t rejectMask =
            precisionVertexRejectMask(*packet, vertexIndex);
        if (rejectMask == 0)
          continue;
        const ReplayVertex &vertex = packet->vertices[vertexIndex];
        ps1::metrics::traceAtFrame(
            "replay.precision_vertex_reject", frame->latch().frameNumber,
            {{"reject_schema", kPrecisionRejectTraceSchema},
             {"batch_frame_index", static_cast<int64_t>(frameIndex)},
             {"operation_index", static_cast<int64_t>(operationIndex)},
             {"packet_sequence", static_cast<int64_t>(packet->packetSequence)},
             {"submission_id", static_cast<int64_t>(packet->submissionId)},
             {"opcode", packet->opcode},
             {"route", static_cast<int64_t>(packet->route)},
             {"semantic_reason", static_cast<int64_t>(packet->semanticReason)},
             {"vertex_index", static_cast<int64_t>(vertexIndex)},
             {"reject_mask", rejectMask},
             {"native_x", vertex.nativeX},
             {"native_y", vertex.nativeY},
             {"x_q16", traceQ16(vertex.x)},
             {"y_q16", traceQ16(vertex.y)},
             {"u_q16", traceQ16(vertex.u)},
             {"v_q16", traceQ16(vertex.v)},
             {"raw_depth_q16", traceQ16(vertex.rawDepth)},
             {"near_depth_q16", traceQ16(vertex.nearDepth)},
             {"qx_q16", traceQ16(vertex.qx)},
             {"qy_q16", traceQ16(vertex.qy)},
             {"precise_x", vertex.preciseX ? 1 : 0},
             {"precise_y", vertex.preciseY ? 1 : 0},
             {"precise_xy", vertex.preciseXY ? 1 : 0},
             {"precise_uv", vertex.preciseUV ? 1 : 0},
             {"raw_depth_valid", vertex.rawDepthValid ? 1 : 0},
             {"homogeneous_q", vertex.homogeneousQ ? 1 : 0},
             {"reconciled_x", vertex.reconciledX ? 1 : 0},
             {"reconciled_y", vertex.reconciledY ? 1 : 0},
             {"invented_w", vertex.inventedW ? 1 : 0}});
      }
    }
  }
}

} // namespace

ModernReplaySubmissionPlan::ModernReplaySubmissionPlan(
    ModernReplaySubmissionPlan &&other) noexcept
    : route_(other.route_), reason_(other.reason_),
      strictReason_(other.strictReason_),
      missingCapabilities_(other.missingCapabilities_),
      counters_(other.counters_), frames_(std::move(other.frames_)),
      candidateState_(std::move(other.candidateState_)), owner_(other.owner_),
      headGeneration_(other.headGeneration_) {
  other.owner_ = nullptr;
}

ModernReplaySubmissionPlan &ModernReplaySubmissionPlan::operator=(
    ModernReplaySubmissionPlan &&other) noexcept {
  if (this == &other)
    return *this;
  route_ = other.route_;
  reason_ = other.reason_;
  strictReason_ = other.strictReason_;
  missingCapabilities_ = other.missingCapabilities_;
  counters_ = other.counters_;
  frames_ = std::move(other.frames_);
  candidateState_ = std::move(other.candidateState_);
  owner_ = other.owner_;
  headGeneration_ = other.headGeneration_;
  other.owner_ = nullptr;
  return *this;
}

ModernReplaySubmissionCommit::ModernReplaySubmissionCommit(
    ModernReplaySubmissionCommit &&other) noexcept
    : owner_(other.owner_), queueSerial_(other.queueSerial_),
      route_(other.route_), frames_(std::move(other.frames_)),
      candidateState_(std::move(other.candidateState_)) {
  other.owner_ = nullptr;
  other.queueSerial_ = 0;
}

ModernReplaySubmissionScheduler::ModernReplaySubmissionScheduler(
    ModernReplaySubmissionConfig config, ReplayPreflightState initialState)
    : config_(std::move(config)), scheduledState_(initialState),
      confirmedState_(std::move(initialState)) {}

void ModernReplaySubmissionScheduler::enqueue(CrashReplayFramePtr frame) {
  if (!frame)
    throw std::invalid_argument("cannot enqueue a null CrashReplay frame");
  pending_.push_back(std::move(frame));
}

void ModernReplaySubmissionScheduler::enqueue(
    std::span<const CrashReplayFramePtr> frames) {
  if (std::any_of(frames.begin(), frames.end(),
                  [](const CrashReplayFramePtr &frame) { return !frame; })) {
    throw std::invalid_argument("cannot enqueue a null CrashReplay frame");
  }
  pending_.insert(pending_.end(), frames.begin(), frames.end());
}

std::vector<CrashReplayFramePtr>
ModernReplaySubmissionScheduler::pendingFrames() const {
  return {pending_.begin(), pending_.end()};
}

void ModernReplaySubmissionScheduler::rebaseGuestTimeline() noexcept {
  pending_.clear();
  scheduledState_.timelineInitialized = false;
  confirmedState_ = scheduledState_;
  confirmedQueueSerial_ = lastSubmittedQueueSerial_;
  if (headGeneration_ != std::numeric_limits<uint64_t>::max())
    ++headGeneration_;
}

ModernReplaySubmissionPlan ModernReplaySubmissionScheduler::plan(
    const ReplayNativeSnapshotIdentity &snapshot) const {
  ModernReplaySubmissionPlan plan;
  plan.owner_ = this;
  plan.headGeneration_ = headGeneration_;
  plan.candidateState_ = scheduledState_;

  if (!snapshot.sealedFrame) {
    plan.reason_ = ReplayPreflightReason::TargetLatchUnavailable;
    return plan;
  }

  const auto target =
      std::find_if(pending_.begin(), pending_.end(), [&](const auto &frame) {
        return frame.get() == snapshot.sealedFrame.get();
      });
  if (target == pending_.end()) {
    plan.reason_ = ReplayPreflightReason::TargetLatchUnavailable;
    return plan;
  }

  plan.frames_.assign(pending_.begin(), std::next(target));

  ReplayPreflightRequest strictRequest;
  strictRequest.schemaVersion = config_.schemaVersion;
  strictRequest.schemaName = config_.schemaName;
  strictRequest.expectedLatch = expectedLatchForSnapshot(snapshot);
  strictRequest.expectedPolicy = config_.expectedPolicy;
  strictRequest.capabilities = config_.capabilities;
  strictRequest.nearClipMode = config_.nearClipMode;
  strictRequest.requireCanonicalRawState = config_.requireCanonicalRawState;

  ReplayPreflightResult strict =
      preflightCrashReplay(plan.frames_, strictRequest, scheduledState_);
  plan.strictReason_ = strict.reason;
  plan.missingCapabilities_ = strict.missingCapabilities;
  bool precisionCoverageAccepted = true;
  if (config_.requireFullPrecisionCoverage) {
    const ReplayPrecisionCoverage precisionCoverage =
        measureReplayPrecisionCoverage(strict.frames);
    precisionCoverageAccepted = precisionCoverage.fullyHighPrecision();
    if (strict.route == ReplayPreflightRoute::M1Replay &&
        !precisionCoverageAccepted) {
      tracePrecisionCoverageReject(snapshot, strict.frames, precisionCoverage);
    }
  }
  if (strict.route == ReplayPreflightRoute::M1Replay &&
      exactPrefixReturned(plan.frames_, strict.frames) &&
      precisionCoverageAccepted) {
    plan.route_ = ModernReplaySubmissionRoute::M1Replay;
    plan.reason_ = ReplayPreflightReason::None;
    plan.counters_ = strict.counters;
    plan.frames_ = std::move(strict.frames);
    plan.candidateState_ = std::move(strict.candidateState);
    return plan;
  }
  if (strict.route == ReplayPreflightRoute::M1Replay &&
      !precisionCoverageAccepted) {
    plan.strictReason_ = ReplayPreflightReason::PrecisionCoverageIncomplete;
  }

  // CrashReplay schema 7 retains Q, but the current executor cannot recover
  // geometric clipping from a native snapshot: the anchor request is
  // intentionally saturation-only. Do not hide an invalid consumer policy.
  if (strict.reason == ReplayPreflightReason::InvalidNearClipMode) {
    plan.reason_ = strict.reason;
    plan.counters_ = strict.counters;
    return plan;
  }

  ReplayNativeAnchorRequest anchorRequest;
  anchorRequest.schemaVersion = config_.schemaVersion;
  anchorRequest.schemaName = config_.schemaName;
  anchorRequest.snapshot = snapshot;
  anchorRequest.expectedPolicy = config_.expectedPolicy;
  anchorRequest.requireCanonicalRawState = config_.requireCanonicalRawState;
  anchorRequest.allowIncompleteNativeReanchor =
      config_.allowIncompleteNativeReanchor;
  ReplayNativeAnchorResult anchor =
      deriveNativeSnapshotAnchor(plan.frames_, anchorRequest, scheduledState_);
  const bool anchorReturnedExactPrefix =
      exactPrefixReturned(plan.frames_, anchor.frames);
  plan.reason_ = anchor.reason;
  plan.counters_ = anchor.counters;
  if (!anchor.frames.empty())
    plan.frames_ = std::move(anchor.frames);
  if (anchor.route == ReplayNativeAnchorRoute::M0Reanchor &&
      anchorReturnedExactPrefix) {
    plan.route_ = ModernReplaySubmissionRoute::M0Reanchor;
    plan.candidateState_ = std::move(anchor.candidateState);
  } else if (anchor.route == ReplayNativeAnchorRoute::M0Reanchor) {
    plan.reason_ = ReplayPreflightReason::TargetLatchUnavailable;
  }
  return plan;
}

ModernReplaySubmissionPlan
ModernReplaySubmissionScheduler::downgradeToNativeReanchor(
    ModernReplaySubmissionPlan &&acceptedM1,
    const ReplayNativeSnapshotIdentity &snapshot) const {
  ModernReplaySubmissionPlan downgraded;
  downgraded.owner_ = this;
  downgraded.headGeneration_ = headGeneration_;
  downgraded.reason_ = ReplayPreflightReason::TargetLatchUnavailable;

  const bool exactAcceptedM1 =
      acceptedM1.route_ == ModernReplaySubmissionRoute::M1Replay &&
      planStillMatchesPending(acceptedM1) &&
      acceptedM1.headGeneration_ == headGeneration_ &&
      !acceptedM1.frames_.empty() && snapshot.sealedFrame &&
      snapshot.sealedFrame.get() == acceptedM1.frames_.back().get();
  if (!exactAcceptedM1) {
    acceptedM1.owner_ = nullptr;
    return downgraded;
  }

  ReplayNativeAnchorRequest anchorRequest;
  anchorRequest.schemaVersion = config_.schemaVersion;
  anchorRequest.schemaName = config_.schemaName;
  anchorRequest.snapshot = snapshot;
  anchorRequest.expectedPolicy = config_.expectedPolicy;
  anchorRequest.requireCanonicalRawState = config_.requireCanonicalRawState;
  anchorRequest.allowIncompleteNativeReanchor =
      config_.allowIncompleteNativeReanchor;
  ReplayNativeAnchorResult anchor = deriveNativeSnapshotAnchor(
      acceptedM1.frames_, anchorRequest, scheduledState_);
  const bool exactPrefix =
      exactPrefixReturned(acceptedM1.frames_, anchor.frames);

  downgraded.strictReason_ = ReplayPreflightReason::MissingCapability;
  downgraded.reason_ = anchor.reason;
  downgraded.counters_ = anchor.counters;
  if (!anchor.frames.empty())
    downgraded.frames_ = std::move(anchor.frames);
  if (anchor.route == ReplayNativeAnchorRoute::M0Reanchor && exactPrefix) {
    downgraded.route_ = ModernReplaySubmissionRoute::M0Reanchor;
    downgraded.reason_ = ReplayPreflightReason::None;
    downgraded.candidateState_ = std::move(anchor.candidateState);
  } else if (anchor.route == ReplayNativeAnchorRoute::M0Reanchor) {
    downgraded.reason_ = ReplayPreflightReason::TargetLatchUnavailable;
  }

  acceptedM1.owner_ = nullptr;
  return downgraded;
}

bool ModernReplaySubmissionScheduler::planStillMatchesPending(
    const ModernReplaySubmissionPlan &plan) const noexcept {
  if (plan.owner_ != this || plan.headGeneration_ != headGeneration_ ||
      !plan.advancesReplayState() || plan.frames_.empty() ||
      plan.frames_.size() > pending_.size()) {
    return false;
  }
  return std::equal(plan.frames_.begin(), plan.frames_.end(), pending_.begin(),
                    [](const CrashReplayFramePtr &planned,
                       const CrashReplayFramePtr &pending) {
                      return planned.get() == pending.get();
                    });
}

bool ModernReplaySubmissionScheduler::validatePlanForSubmit(
    const ModernReplaySubmissionPlan &plan) const noexcept {
  return planStillMatchesPending(plan) &&
         headGeneration_ != std::numeric_limits<uint64_t>::max() &&
         lastSubmittedQueueSerial_ != std::numeric_limits<uint64_t>::max();
}

ModernReplaySubmissionCommit ModernReplaySubmissionScheduler::commitSubmitted(
    ModernReplaySubmissionPlan &&plan) noexcept {
  ModernReplaySubmissionCommit commit;
  if (!validatePlanForSubmit(plan)) {
    plan.owner_ = nullptr;
    return commit;
  }

  const std::size_t prefixSize = plan.frames_.size();
  const uint64_t queueSerial = lastSubmittedQueueSerial_ + 1u;

  commit.owner_ = this;
  commit.queueSerial_ = queueSerial;
  commit.route_ = plan.route_;
  commit.frames_ = std::move(plan.frames_);
  commit.candidateState_ = plan.candidateState_;

  scheduledState_ = std::move(plan.candidateState_);
  for (std::size_t index = 0; index < prefixSize; ++index)
    pending_.pop_front();
  ++headGeneration_;
  lastSubmittedQueueSerial_ = queueSerial;
  plan.owner_ = nullptr;
  return commit;
}

void ModernReplaySubmissionScheduler::confirmQueueOrderedFence(
    const ModernReplaySubmissionCommit &commit) noexcept {
  if (commit.owner_ != this || !commit.valid() ||
      commit.queueSerial_ <= confirmedQueueSerial_ ||
      commit.queueSerial_ > lastSubmittedQueueSerial_) {
    return;
  }
  confirmedState_ = commit.candidateState_;
  confirmedQueueSerial_ = commit.queueSerial_;
}

} // namespace ps1::gpu::replay
