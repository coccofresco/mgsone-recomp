#include "runtime/gpu/modern_replay_execution_plan.h"

#include <bit>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ps1::gpu::replay {
namespace {

bool depthModeEnabled(const ReplayPolicyState &policy) {
  return policy.pgxp && policy.trueDepthMode != ReplayDepthMode::Off;
}

bool visibilityBoundaryEnabled(const ReplayPolicyState &policy) {
  return policy.pgxp && policy.trueDepthMode == ReplayDepthMode::Hybrid;
}

bool isPainterOrderedDepthState(const DepthState &depth) {
  return !depth.testEnabled && !depth.writeEnabled &&
         depth.compare == DepthCompare::Always && !depth.invalidateOnAccepted;
}

bool isPainterWriteDepthState(const DepthState &depth) {
  return !depth.testEnabled && depth.writeEnabled &&
         depth.compare == DepthCompare::Always && !depth.invalidateOnAccepted;
}

bool isAcceptedFarInvalidateDepthState(const DepthState &depth) {
  return !depth.testEnabled && !depth.writeEnabled &&
         depth.compare == DepthCompare::Always && depth.invalidateOnAccepted;
}

bool donorDepthStateMatches(const DrawPacket &packet,
                            const ReplayPolicyState &policy) {
  const DepthState &depth = packet.state.depth;
  if (packet.route == PacketRoute::Screen2D)
    return isAcceptedFarInvalidateDepthState(depth);

  const bool expected =
      depthModeEnabled(policy) && donorTrueDepthEligible(packet);
  if (visibilityBoundaryEnabled(policy) && packet.state.blend.enabled) {
    // OT painter packets bypass the previous opaque owner. Their accepted
    // composite samples have no physical W, so they invalidate D32 to far;
    // rejected samples preserve every attachment. Later Hybrid opaque work
    // follows OT order independently through its ALWAYS-write state.
    return isAcceptedFarInvalidateDepthState(depth);
  }
  if (!expected) {
    // Opaque fallback and depth-disabled packets remain pure painter work.
    return isPainterOrderedDepthState(depth);
  }

  // A full source mesh owns its topology and opaque visibility. It uses the
  // modern depth test even while native effects retain Hybrid composition.
  const bool sourceTopology =
      packet.originalMesh && packet.originalMesh->sourceTopology;
  if (policy.trueDepthMode == ReplayDepthMode::Hybrid && !sourceTopology)
    return isPainterWriteDepthState(depth);
  return depth.testEnabled && depth.writeEnabled &&
         depth.compare == DepthCompare::LessOrEqual &&
         !depth.invalidateOnAccepted;
}

uint64_t acceptedA0Pixels(const CpuUpload &upload) {
  uint64_t accepted = 0;
  for (const uint64_t word : upload.acceptedBits)
    accepted += std::popcount(word);
  return accepted;
}

void makeOrderedMutationBoundary(ReplayExecutionOperation &item) {
  item.hazards.barrierBefore = true;
  item.hazards.barrierAfter = true;
}

void classifyDraw(const DrawPacket &packet, const ReplayPolicyState &policy,
                  ReplayExecutionOperation &item) {
  const bool translucent = packet.state.blend.enabled;
  item.kind = translucent ? ReplayExecutionKind::DrawTranslucent
                          : ReplayExecutionKind::DrawOpaque;
  item.draw.textured = packet.textured;
  item.draw.checkMask = packet.state.mask.checkMaskBit;
  item.draw.forceMask = packet.state.mask.forceMaskBit;
  item.draw.textureZeroCanDiscard = packet.textured;
  item.draw.sourceStpWritesMask = packet.textured;
  if (!translucent) {
    item.draw.blendSelection = ReplayBlendSelection::None;
  } else if (packet.textured) {
    item.draw.blendSelection = ReplayBlendSelection::SourceStp;
  } else {
    item.draw.blendSelection = ReplayBlendSelection::WholePrimitive;
  }

  item.access.nativeRead = packet.nativeRead.required;
  item.access.colorRead = translucent;
  item.access.colorWrite = true;
  item.access.maskRead = packet.state.mask.checkMaskBit;
  item.access.maskWrite = true;

  const bool destinationFeedback =
      item.access.colorRead || item.access.maskRead;
  item.hazards.destinationFeedbackBoundary = destinationFeedback;
  item.hazards.destinationScratchRequired = destinationFeedback;
  item.hazards.refreshDestinationBetweenTriangles =
      destinationFeedback && packet.indexCount > 3;
  item.hazards.barrierBefore = item.access.nativeRead || destinationFeedback;
  item.hazards.barrierAfter = destinationFeedback;

  const DepthState &sourceDepth = packet.state.depth;
  item.depth.donorEligible = donorTrueDepthEligible(packet);
  item.depth.requestedTest = sourceDepth.testEnabled;
  item.depth.requestedWrite = sourceDepth.writeEnabled;
  item.depth.compare = sourceDepth.compare;
  item.depth.epoch = sourceDepth.epoch;
  if (sourceDepth.invalidateOnAccepted) {
    item.depth.invalidateOnAccepted = sourceDepth.invalidateOnAccepted;
    item.access.depthWrite = sourceDepth.invalidateOnAccepted;
  } else if (depthModeEnabled(policy) && item.depth.donorEligible) {
    item.depth.test = sourceDepth.testEnabled;
    item.depth.write = sourceDepth.writeEnabled;
    item.access.depthRead = item.depth.test;
    item.access.depthWrite = item.depth.write;
  }

  item.hazards.fragmentAcceptanceControlsDepth =
      (item.depth.test || item.depth.write ||
       item.depth.invalidateOnAccepted) &&
      (item.draw.textureZeroCanDiscard || item.draw.checkMask);
}

void classifyOperation(const Operation &operation,
                       const ReplayPolicyState &policy,
                       ReplayExecutionOperation &item) {
  if (std::holds_alternative<NativeTileUpload>(operation)) {
    item.kind = ReplayExecutionKind::NativeTileUpload;
    item.access.nativeWrite = true;
    makeOrderedMutationBoundary(item);
    return;
  }
  if (const auto *seed = std::get_if<SurfaceSeed>(&operation)) {
    item.kind = ReplayExecutionKind::SurfaceSeed;
    item.access.nativeWrite = true;
    item.access.colorWrite = true;
    item.access.maskWrite = true;
    item.access.depthWrite = true;
    item.depth.clear = true;
    item.depth.epoch = seed->depthEpoch;
    makeOrderedMutationBoundary(item);
    return;
  }
  if (const auto *packet = std::get_if<DrawPacket>(&operation)) {
    classifyDraw(*packet, policy, item);
    return;
  }
  if (std::holds_alternative<FillRect>(operation)) {
    item.kind = ReplayExecutionKind::FillRect;
    item.access.nativeWrite = true;
    item.access.colorWrite = true;
    item.access.maskWrite = true;
    item.access.depthWrite = true;
    item.depth.invalidateOnAccepted = true;
    makeOrderedMutationBoundary(item);
    return;
  }
  if (const auto *copy = std::get_if<CopyRect>(&operation)) {
    item.kind = ReplayExecutionKind::CopyRect;
    item.access.nativeRead = true;
    item.access.nativeWrite = true;
    item.access.colorRead = true;
    item.access.colorWrite = true;
    item.access.maskRead = true;
    item.access.maskWrite = true;
    item.access.depthWrite = true;
    item.depth.invalidateOnAccepted = true;
    item.hazards.copySourceScratchRequired = true;
    item.hazards.destinationFeedbackBoundary = copy->mask.checkMaskBit;
    item.hazards.destinationScratchRequired = copy->mask.checkMaskBit;
    item.hazards.fragmentAcceptanceControlsDepth = copy->mask.checkMaskBit;
    makeOrderedMutationBoundary(item);
    return;
  }
  if (const auto *upload = std::get_if<CpuUpload>(&operation)) {
    switch (upload->phase) {
    case CpuUploadPhase::Begin:
      item.kind = ReplayExecutionKind::CpuUploadBegin;
      break;
    case CpuUploadPhase::Data: {
      item.kind = ReplayExecutionKind::CpuUploadData;
      item.a0.data = true;
      item.a0.consumedPixels = upload->pixels.size();
      item.a0.acceptedPixels = acceptedA0Pixels(*upload);
      item.a0.rejectedPixels = item.a0.consumedPixels - item.a0.acceptedPixels;
      if (item.a0.acceptedPixels != 0) {
        // acceptedBits already contains the native E6 decision.  Re-reading
        // the destination mask here would allow asynchronous execution to
        // produce a different answer from the authoritative GP0 stream.
        item.access.nativeWrite = true;
        item.access.colorWrite = true;
        item.access.maskWrite = true;
        item.access.depthWrite = true;
        item.depth.invalidateOnAccepted = true;
        makeOrderedMutationBoundary(item);
      }
      break;
    }
    case CpuUploadPhase::Complete:
      item.kind = ReplayExecutionKind::CpuUploadComplete;
      break;
    case CpuUploadPhase::Abort:
      item.kind = ReplayExecutionKind::CpuUploadAbort;
      break;
    }
    return;
  }
  if (const auto *paint = std::get_if<PaintPixels>(&operation)) {
    item.kind = ReplayExecutionKind::PaintPixels;
    item.access.nativeWrite = true;
    item.access.colorRead = paint->blend.enabled;
    item.access.colorWrite = true;
    // PaintPixels contains only writes for which the native E6 test already
    // succeeded. Re-testing the destination asynchronously could disagree
    // with the authoritative packet order.
    item.access.maskRead = false;
    item.access.maskWrite = true;
    item.access.depthWrite = true;
    item.depth.invalidateOnAccepted = paint->depth.invalidateOnAccepted;
    item.depth.epoch = paint->depth.epoch;
    item.hazards.destinationFeedbackBoundary = paint->blend.enabled;
    item.hazards.destinationScratchRequired = paint->blend.enabled;
    item.hazards.fragmentAcceptanceControlsDepth = false;
    makeOrderedMutationBoundary(item);
    return;
  }
  if (const auto *paint = std::get_if<PaintRect>(&operation)) {
    item.kind = ReplayExecutionKind::PaintRect;
    item.access.nativeWrite = true;
    item.access.colorWrite = true;
    item.access.maskRead = paint->mask.checkMaskBit;
    item.access.maskWrite = true;
    item.access.depthWrite = true;
    item.depth.invalidateOnAccepted = paint->depth.invalidateOnAccepted;
    item.depth.epoch = paint->depth.epoch;
    item.hazards.destinationFeedbackBoundary = paint->mask.checkMaskBit;
    item.hazards.destinationScratchRequired = paint->mask.checkMaskBit;
    item.hazards.fragmentAcceptanceControlsDepth =
        paint->depth.invalidateOnAccepted && paint->mask.checkMaskBit;
    makeOrderedMutationBoundary(item);
    return;
  }
  if (const auto *clear = std::get_if<ClearDepth>(&operation)) {
    item.kind = ReplayExecutionKind::ClearDepth;
    item.access.depthWrite = true;
    item.depth.clear = true;
    item.depth.epoch = clear->nextEpoch;
    makeOrderedMutationBoundary(item);
    return;
  }
  if (const auto *reset = std::get_if<Reset>(&operation)) {
    item.kind = ReplayExecutionKind::Reset;
    item.access.depthWrite = reset->clearDepth;
    item.depth.clear = reset->clearDepth;
    item.depth.epoch = reset->nextDepthEpoch;
    makeOrderedMutationBoundary(item);
    return;
  }
  item.kind = ReplayExecutionKind::FrameLatch;
  // A complete latch resolves the selected aperture from the persistent
  // color atlas. It is therefore the final read/barrier of the atomic batch,
  // even though presentation itself is recorded by the owning renderer.
  item.access.colorRead = true;
  item.hazards.barrierBefore = true;
}

ReplayExecutionPlanResult failure(ReplayExecutionPlanReason reason,
                                  std::size_t frameIndex,
                                  std::size_t operationIndex) {
  ReplayExecutionPlanResult result;
  result.reason = reason;
  result.frameIndex = frameIndex;
  result.operationIndex = operationIndex;
  return result;
}

} // namespace

const Operation &ModernReplayExecutionPlan::sourceOperation(
    const ReplayExecutionOperation &item) const {
  if (item.frameIndex >= frames_.size() || !frames_[item.frameIndex])
    throw std::out_of_range("replay execution frame index is out of range");
  const std::span<const Operation> source =
      frames_[item.frameIndex]->operations();
  if (item.operationIndex >= source.size()) {
    throw std::out_of_range("replay execution operation index is out of range");
  }
  return source[item.operationIndex];
}

bool donorTrueDepthEligible(const DrawPacket &packet) noexcept {
  if (packet.route != PacketRoute::Projected3D || packet.state.blend.enabled ||
      packet.fallbackReason != PacketFallbackReason::None ||
      packet.vertexCount < 3 || packet.vertexCount > DrawPacket::MAX_VERTICES) {
    return false;
  }
  const uint8_t fullMask =
      static_cast<uint8_t>((1u << packet.vertexCount) - 1u);
  return packet.validMask == fullMask;
}

ReplayPrecisionCoverage
measureReplayPrecisionCoverage(std::span<const CrashReplayFramePtr> frames) {
  ReplayPrecisionCoverage coverage;
  coverage.frames = frames.size();
  for (const CrashReplayFramePtr &frame : frames) {
    if (!frame) {
      ++coverage.nullFrames;
      continue;
    }

    const FrameLatch &latch = frame->latch();
    if (!latch.complete)
      ++coverage.incompleteFrames;
    coverage.unsupportedMutations += latch.unsupportedCount;
    const bool depthRequested =
        latch.policy.pgxp && latch.policy.trueDepthMode != ReplayDepthMode::Off;
    const bool visibilityBoundaryRequested =
        latch.policy.pgxp &&
        latch.policy.trueDepthMode == ReplayDepthMode::Hybrid;

    for (const Operation &operation : frame->operations()) {
      const DrawPacket *packet = std::get_if<DrawPacket>(&operation);
      if (packet == nullptr)
        continue;
      ++coverage.drawPackets;
      coverage.vertices += packet->vertexCount;
      if (packet->fallbackReason != PacketFallbackReason::None)
        ++coverage.fallbackPackets;

      for (uint8_t triangleIndex = 0; triangleIndex < packet->indexCount / 3u;
           ++triangleIndex) {
        const ReplayTriangleProvenance owner =
            captureReplayTriangleProvenance(*packet, triangleIndex);
        if (!owner.validTopology)
          continue;
        ++coverage.triangleOwners;
        const uint8_t evidence = owner.completeEvidence();
        if ((evidence & ReplayVertexProjectionEventExact) != 0)
          ++coverage.projectionEventOwnerTriangles;
        if (owner.coherentProjectionContext())
          ++coverage.coherentProjectionContextOwnerTriangles;
        if ((evidence & ReplayVertexTransformLineageExact) != 0)
          ++coverage.transformLineageOwnerTriangles;
        if (owner.coherentTransformLineage())
          ++coverage.coherentTransformLineageOwnerTriangles;
        if ((evidence & ReplayVertexModelSourceExact) != 0)
          ++coverage.directModelSourceOwnerTriangles;
        if ((evidence & ReplayVertexModelValueFlowExact) != 0)
          ++coverage.computedModelFlowOwnerTriangles;
        if ((evidence & ReplayVertexGp0WordSourceExact) != 0)
          ++coverage.gp0WordSourceOwnerTriangles;
        if ((evidence & ReplayVertexDmaTraversalExact) != 0)
          ++coverage.dmaTraversalOwnerTriangles;
        if (owner.coherentDmaTraversal())
          ++coverage.coherentDmaTraversalOwnerTriangles;
      }

      if (packet->route == PacketRoute::Screen2D) {
        ++coverage.screen2DPackets;
        coverage.screen2DVertices += packet->vertexCount;
        for (std::size_t vertexIndex = 0; vertexIndex < packet->vertexCount;
             ++vertexIndex) {
          const ReplayVertex &vertex = packet->vertices[vertexIndex];
          if (!vertex.preciseXY &&
              vertex.x == static_cast<double>(vertex.nativeX) &&
              vertex.y == static_cast<double>(vertex.nativeY) &&
              vertex.w == CRASH_REPLAY_FALLBACK_W) {
            ++coverage.exactScreen2DVertices;
          }
        }
        const DepthState &depth = packet->state.depth;
        if (isAcceptedFarInvalidateDepthState(depth)) {
          ++coverage.screen2DDepthInvalidateMatchedPackets;
        } else {
          ++coverage.screen2DDepthStateGapPackets;
        }
        continue;
      }

      ++coverage.projectedPackets;
      coverage.projectedVertices += packet->vertexCount;
      if (packet->textured)
        coverage.texturedProjectedVertices += packet->vertexCount;
      for (std::size_t vertexIndex = 0; vertexIndex < packet->vertexCount;
           ++vertexIndex) {
        const ReplayVertex &vertex = packet->vertices[vertexIndex];
        if (vertex.preciseX && std::isfinite(vertex.x))
          ++coverage.preciseProjectedXVertices;
        if (vertex.preciseY && std::isfinite(vertex.y))
          ++coverage.preciseProjectedYVertices;
        if (vertex.preciseXY && vertex.preciseX && vertex.preciseY &&
            std::isfinite(vertex.x) && std::isfinite(vertex.y)) {
          ++coverage.preciseProjectedXYVertices;
        }
        if (vertex.rawDepthValid &&
            (packet->validMask & (1u << vertexIndex)) != 0 &&
            std::isfinite(vertex.rawDepth) && std::isfinite(vertex.nearDepth)) {
          ++coverage.validProjectedWVertices;
        }
        if (vertex.homogeneousQ && std::isfinite(vertex.qx) &&
            std::isfinite(vertex.qy))
          ++coverage.homogeneousProjectedQVertices;
        if (vertex.reconciledX)
          ++coverage.reconciledProjectedXVertices;
        if (vertex.reconciledY)
          ++coverage.reconciledProjectedYVertices;
        if (vertex.inventedW)
          ++coverage.inventedProjectedWVertices;
        if (vertex.nearSaturated)
          ++coverage.nearSaturatedProjectedVertices;
        if (packet->textured && vertex.preciseUV && std::isfinite(vertex.u) &&
            std::isfinite(vertex.v)) {
          ++coverage.preciseProjectedUVVertices;
        }
      }

      const bool depthEligible =
          depthRequested && donorTrueDepthEligible(*packet);
      if (depthEligible) {
        ++coverage.donorDepthEligiblePackets;
        coverage.donorDepthEligibleVertices += packet->vertexCount;
        if (donorDepthStateMatches(*packet, latch.policy)) {
          ++coverage.donorDepthStateMatchedPackets;
          coverage.donorDepthStateMatchedVertices += packet->vertexCount;
        }
      } else {
        ++coverage.painterOrderedProjectedPackets;
        coverage.painterOrderedProjectedVertices += packet->vertexCount;
        const uint8_t fullMask =
            packet->vertexCount >= 3 &&
                    packet->vertexCount <= DrawPacket::MAX_VERTICES
                ? static_cast<uint8_t>((1u << packet->vertexCount) - 1u)
                : 0;
        const DepthState &depth = packet->state.depth;
        const bool expectsVisibilityBoundary =
            visibilityBoundaryRequested && packet->state.blend.enabled;
        const bool depthStateMatched =
            expectsVisibilityBoundary ? isAcceptedFarInvalidateDepthState(depth)
                                      : isPainterOrderedDepthState(depth);
        if (!depthStateMatched) {
          ++coverage.painterDepthStateGapPackets;
          coverage.painterDepthStateGapVertices += packet->vertexCount;
        } else if (packet->fallbackReason != PacketFallbackReason::None ||
                   fullMask == 0 || packet->validMask != fullMask) {
          ++coverage.painterFallbackPackets;
          coverage.painterFallbackVertices += packet->vertexCount;
        } else if (packet->state.blend.enabled) {
          ++coverage.painterPreciseTranslucentPackets;
          coverage.painterPreciseTranslucentVertices += packet->vertexCount;
          if (expectsVisibilityBoundary) {
            ++coverage.otVisibilityBoundaryPackets;
            coverage.otVisibilityBoundaryVertices += packet->vertexCount;
          }
        } else if (!depthRequested) {
          ++coverage.painterDepthPolicyOffPackets;
          coverage.painterDepthPolicyOffVertices += packet->vertexCount;
        } else {
          ++coverage.painterUnexpectedOpaquePackets;
          coverage.painterUnexpectedOpaqueVertices += packet->vertexCount;
        }
      }
    }
  }
  return coverage;
}

ReplayExecutionPlanResult
planModernReplayExecution(std::span<const CrashReplayFramePtr> frames) {
  if (frames.empty()) {
    return failure(ReplayExecutionPlanReason::EmptyBatch,
                   ReplayExecutionPlanResult::NO_INDEX,
                   ReplayExecutionPlanResult::NO_INDEX);
  }

  ModernReplayExecutionPlan candidate;
  candidate.frames_.assign(frames.begin(), frames.end());
  bool hasPolicy = false;
  ReplayPolicyState batchPolicy;
  std::size_t ordinal = 0;

  for (std::size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
    const CrashReplayFramePtr &frame = frames[frameIndex];
    if (!frame) {
      return failure(ReplayExecutionPlanReason::NullFrame, frameIndex,
                     ReplayExecutionPlanResult::NO_INDEX);
    }
    const std::span<const Operation> operations = frame->operations();
    if (operations.empty() ||
        !std::holds_alternative<FrameLatch>(operations.back())) {
      return failure(ReplayExecutionPlanReason::InvalidFrameLayout, frameIndex,
                     operations.size());
    }
    for (std::size_t operationIndex = 0; operationIndex + 1 < operations.size();
         ++operationIndex) {
      if (std::holds_alternative<FrameLatch>(operations[operationIndex])) {
        return failure(ReplayExecutionPlanReason::InvalidFrameLayout,
                       frameIndex, operationIndex);
      }
    }

    const ReplayPolicyState &policy = frame->latch().policy;
    if (hasPolicy && policy != batchPolicy) {
      return failure(ReplayExecutionPlanReason::PolicyDiscontinuity, frameIndex,
                     operations.size() - 1u);
    }
    batchPolicy = policy;
    hasPolicy = true;

    for (std::size_t operationIndex = 0; operationIndex < operations.size();
         ++operationIndex) {
      const Operation &operation = operations[operationIndex];
      if (const auto *packet = std::get_if<DrawPacket>(&operation);
          packet != nullptr && !donorDepthStateMatches(*packet, policy)) {
        return failure(ReplayExecutionPlanReason::DonorDepthStateMismatch,
                       frameIndex, operationIndex);
      }

      ReplayExecutionOperation item;
      item.ordinal = ordinal++;
      item.frameIndex = frameIndex;
      item.operationIndex = operationIndex;
      classifyOperation(operation, policy, item);
      candidate.operations_.push_back(std::move(item));
    }
  }

  ReplayExecutionPlanResult result;
  result.plan = std::move(candidate);
  return result;
}

std::string_view
replayExecutionPlanReasonName(ReplayExecutionPlanReason reason) noexcept {
  switch (reason) {
  case ReplayExecutionPlanReason::None:
    return "none";
  case ReplayExecutionPlanReason::EmptyBatch:
    return "empty_batch";
  case ReplayExecutionPlanReason::NullFrame:
    return "null_frame";
  case ReplayExecutionPlanReason::InvalidFrameLayout:
    return "invalid_frame_layout";
  case ReplayExecutionPlanReason::PolicyDiscontinuity:
    return "policy_discontinuity";
  case ReplayExecutionPlanReason::DonorDepthStateMismatch:
    return "donor_depth_state_mismatch";
  }
  return "unknown";
}

} // namespace ps1::gpu::replay
