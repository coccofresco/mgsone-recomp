#pragma once

#include "runtime/gpu/modern_replay.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace ps1::gpu::replay {

// CPU-only classification for a CrashReplay prefix which has already passed
// strict M1 preflight.  This is deliberately a resource/order plan rather
// than a Vulkan command list: it contains no handles, layouts or queue state.
enum class ReplayExecutionKind : uint8_t {
  NativeTileUpload,
  SurfaceSeed,
  DrawOpaque,
  DrawTranslucent,
  FillRect,
  CopyRect,
  CpuUploadBegin,
  CpuUploadData,
  CpuUploadComplete,
  CpuUploadAbort,
  PaintPixels,
  PaintRect,
  ClearDepth,
  Reset,
  FrameLatch,
};

enum class ReplayBlendSelection : uint8_t {
  None,
  WholePrimitive,
  SourceStp,
};

struct ReplayExecutionAccess {
  bool nativeRead = false;
  bool nativeWrite = false;
  bool colorRead = false;
  bool colorWrite = false;
  bool maskRead = false;
  bool maskWrite = false;
  bool depthRead = false;
  bool depthWrite = false;

  bool operator==(const ReplayExecutionAccess &) const = default;
};

struct ReplayExecutionHazards {
  // A draw which blends or checks the destination mask must snapshot its
  // affected destination after every preceding write.  A feedback quad also
  // refreshes that snapshot between its two ordered raster halves.
  bool destinationFeedbackBoundary = false;
  bool destinationScratchRequired = false;
  bool refreshDestinationBetweenTriangles = false;

  // CopyRect snapshots source color/mask unconditionally: source/destination
  // can overlap after native wrap even when their unwrapped boxes do not.
  bool copySourceScratchRequired = false;

  // These are executor segment boundaries.  In particular FillRect and
  // CopyRect are never eligible for reordering or opaque-draw batching.
  bool barrierBefore = false;
  bool barrierAfter = false;

  // Texture-zero discard and mask rejection must happen before any D32
  // write or accepted-write invalidation. This prohibits an early-depth-write
  // pipeline for this item.
  bool fragmentAcceptanceControlsDepth = false;

  bool operator==(const ReplayExecutionHazards &) const = default;
};

struct ReplayDrawExecution {
  ReplayBlendSelection blendSelection = ReplayBlendSelection::None;
  bool textured = false;
  bool checkMask = false;
  bool forceMask = false;
  bool textureZeroCanDiscard = false;
  // On any textured packet source STP writes the independent PS1 mask plane.
  // It selects blending only when blendSelection is SourceStp.
  bool sourceStpWritesMask = false;

  bool operator==(const ReplayDrawExecution &) const = default;
};

struct ReplayDepthExecution {
  // Eligibility is packet-atomic. Only an opaque, fully valid Projected3D
  // packet with no fallback reason may publish authentic W. Hybrid stores it
  // with ALWAYS so GP0/OT remains authoritative; Strict also tests LEQUAL.
  // Complete original source meshes own visibility and use LEQUAL in Hybrid.
  // Screen2D and Hybrid semi-transparent packets can instead write the far
  // sentinel only for accepted fragments. Opaque invalid-W fallbacks never
  // publish fabricated depth.
  bool donorEligible = false;
  bool requestedTest = false;
  bool requestedWrite = false;
  bool test = false;
  bool write = false;
  bool invalidateOnAccepted = false;
  bool clear = false;
  DepthCompare compare = DepthCompare::Always;
  uint64_t epoch = 0;

  bool operator==(const ReplayDepthExecution &) const = default;
};

struct ReplayA0Execution {
  bool data = false;
  uint64_t consumedPixels = 0;
  uint64_t acceptedPixels = 0;
  uint64_t rejectedPixels = 0;

  bool operator==(const ReplayA0Execution &) const = default;
};

struct ReplayExecutionOperation {
  std::size_t ordinal = 0;
  std::size_t frameIndex = 0;
  std::size_t operationIndex = 0;
  ReplayExecutionKind kind = ReplayExecutionKind::FrameLatch;
  ReplayExecutionAccess access;
  ReplayExecutionHazards hazards;
  ReplayDrawExecution draw;
  ReplayDepthExecution depth;
  ReplayA0Execution a0;

  bool operator==(const ReplayExecutionOperation &) const = default;
};

struct ReplayExecutionPlanResult;

// Frame-batch coverage is measured from the immutable IR itself, never from
// pixels which happened to survive rasterization. This makes the 100% gate a
// per-vertex proof and prevents invisible/off-screen geometry from escaping
// the precision denominator.
struct ReplayPrecisionCoverage {
  uint64_t frames = 0;
  uint64_t nullFrames = 0;
  uint64_t incompleteFrames = 0;
  uint64_t unsupportedMutations = 0;
  uint64_t drawPackets = 0;
  uint64_t projectedPackets = 0;
  uint64_t screen2DPackets = 0;
  uint64_t fallbackPackets = 0;
  uint64_t vertices = 0;
  uint64_t projectedVertices = 0;
  uint64_t screen2DVertices = 0;
  uint64_t exactScreen2DVertices = 0;
  uint64_t preciseProjectedXVertices = 0;
  uint64_t preciseProjectedYVertices = 0;
  uint64_t preciseProjectedXYVertices = 0;
  uint64_t validProjectedWVertices = 0;
  uint64_t homogeneousProjectedQVertices = 0;
  uint64_t reconciledProjectedXVertices = 0;
  uint64_t reconciledProjectedYVertices = 0;
  uint64_t inventedProjectedWVertices = 0;
  uint64_t nearSaturatedProjectedVertices = 0;
  uint64_t texturedProjectedVertices = 0;
  uint64_t preciseProjectedUVVertices = 0;
  // Exact categorical owner coverage. These counters never participate in
  // the current M1 coordinate/depth gate and never infer a family; they expose
  // which independent identity fields are actually available per triangle.
  uint64_t triangleOwners = 0;
  uint64_t projectionEventOwnerTriangles = 0;
  uint64_t coherentProjectionContextOwnerTriangles = 0;
  uint64_t transformLineageOwnerTriangles = 0;
  uint64_t coherentTransformLineageOwnerTriangles = 0;
  uint64_t directModelSourceOwnerTriangles = 0;
  uint64_t computedModelFlowOwnerTriangles = 0;
  uint64_t gp0WordSourceOwnerTriangles = 0;
  uint64_t dmaTraversalOwnerTriangles = 0;
  uint64_t coherentDmaTraversalOwnerTriangles = 0;
  uint64_t donorDepthEligiblePackets = 0;
  uint64_t donorDepthEligibleVertices = 0;
  uint64_t donorDepthStateMatchedPackets = 0;
  uint64_t donorDepthStateMatchedVertices = 0;
  uint64_t painterOrderedProjectedPackets = 0;
  uint64_t painterOrderedProjectedVertices = 0;
  uint64_t painterPreciseTranslucentPackets = 0;
  uint64_t painterPreciseTranslucentVertices = 0;
  // Subset of precise translucent packets whose Hybrid policy state actually
  // installs an accepted-sample far boundary. Keep this separate from the
  // generic painter class used by Off/Strict diagnostics.
  uint64_t otVisibilityBoundaryPackets = 0;
  uint64_t otVisibilityBoundaryVertices = 0;
  uint64_t painterFallbackPackets = 0;
  uint64_t painterFallbackVertices = 0;
  uint64_t painterDepthPolicyOffPackets = 0;
  uint64_t painterDepthPolicyOffVertices = 0;
  uint64_t painterDepthStateGapPackets = 0;
  uint64_t painterDepthStateGapVertices = 0;
  uint64_t painterUnexpectedOpaquePackets = 0;
  uint64_t painterUnexpectedOpaqueVertices = 0;
  uint64_t screen2DDepthInvalidateMatchedPackets = 0;
  uint64_t screen2DDepthStateGapPackets = 0;

  constexpr bool captureComplete() const noexcept {
    return frames != 0 && nullFrames == 0 && incompleteFrames == 0 &&
           unsupportedMutations == 0;
  }
  constexpr bool projectedCoordinatesComplete() const noexcept {
    return preciseProjectedXVertices == projectedVertices &&
           preciseProjectedYVertices == projectedVertices &&
           preciseProjectedXYVertices == projectedVertices &&
           reconciledProjectedXVertices == 0 &&
           reconciledProjectedYVertices == 0 && fallbackPackets == 0;
  }
  constexpr bool projectedDepthProvenanceComplete() const noexcept {
    return validProjectedWVertices == projectedVertices &&
           inventedProjectedWVertices == 0;
  }
  constexpr bool projectedTextureCoordinatesComplete() const noexcept {
    return preciseProjectedUVVertices == texturedProjectedVertices;
  }
  constexpr bool screen2DCoordinatesComplete() const noexcept {
    return exactScreen2DVertices == screen2DVertices;
  }
  constexpr bool donorDepthApplicationComplete() const noexcept {
    // Subtract each disjoint class from its total rather than summing the
    // classes: cumulative soak counters are uint64_t and a wrapped sum could
    // otherwise make a corrupt partition look complete.
    const auto consume = [](uint64_t &remaining,
                            uint64_t count) constexpr noexcept {
      if (count > remaining)
        return false;
      remaining -= count;
      return true;
    };

    uint64_t unclassifiedProjectedPackets = projectedPackets;
    if (!consume(unclassifiedProjectedPackets, donorDepthEligiblePackets) ||
        !consume(unclassifiedProjectedPackets,
                 painterOrderedProjectedPackets) ||
        unclassifiedProjectedPackets != 0) {
      return false;
    }

    uint64_t unclassifiedProjectedVertices = projectedVertices;
    if (!consume(unclassifiedProjectedVertices, donorDepthEligibleVertices) ||
        !consume(unclassifiedProjectedVertices,
                 painterOrderedProjectedVertices) ||
        unclassifiedProjectedVertices != 0) {
      return false;
    }

    uint64_t unclassifiedPainterPackets = painterOrderedProjectedPackets;
    if (!consume(unclassifiedPainterPackets,
                 painterPreciseTranslucentPackets) ||
        !consume(unclassifiedPainterPackets, painterFallbackPackets) ||
        !consume(unclassifiedPainterPackets, painterDepthPolicyOffPackets) ||
        !consume(unclassifiedPainterPackets, painterDepthStateGapPackets) ||
        !consume(unclassifiedPainterPackets, painterUnexpectedOpaquePackets) ||
        unclassifiedPainterPackets != 0) {
      return false;
    }

    uint64_t unclassifiedPainterVertices = painterOrderedProjectedVertices;
    if (!consume(unclassifiedPainterVertices,
                 painterPreciseTranslucentVertices) ||
        !consume(unclassifiedPainterVertices, painterFallbackVertices) ||
        !consume(unclassifiedPainterVertices, painterDepthPolicyOffVertices) ||
        !consume(unclassifiedPainterVertices, painterDepthStateGapVertices) ||
        !consume(unclassifiedPainterVertices,
                 painterUnexpectedOpaqueVertices) ||
        unclassifiedPainterVertices != 0) {
      return false;
    }

    uint64_t unclassifiedScreen2DPackets = screen2DPackets;
    if (!consume(unclassifiedScreen2DPackets,
                 screen2DDepthInvalidateMatchedPackets) ||
        !consume(unclassifiedScreen2DPackets, screen2DDepthStateGapPackets)) {
      return false;
    }

    return donorDepthStateMatchedPackets == donorDepthEligiblePackets &&
           donorDepthStateMatchedVertices == donorDepthEligibleVertices &&
           painterDepthStateGapPackets == 0 &&
           painterUnexpectedOpaquePackets == 0 &&
           unclassifiedScreen2DPackets == 0 &&
           screen2DDepthStateGapPackets == 0;
  }
  constexpr bool fullyHighPrecision() const noexcept {
    return captureComplete() && projectedCoordinatesComplete() &&
           projectedDepthProvenanceComplete() &&
           projectedTextureCoordinatesComplete() &&
           screen2DCoordinatesComplete() && donorDepthApplicationComplete();
  }

  bool operator==(const ReplayPrecisionCoverage &) const = default;
};

class ModernReplayExecutionPlan final {
public:
  std::span<const CrashReplayFramePtr> frames() const noexcept {
    return frames_;
  }
  std::span<const ReplayExecutionOperation> operations() const noexcept {
    return operations_;
  }

  // Strong frame ownership keeps every referenced immutable Operation alive
  // until the caller releases the plan (normally after queue submission has
  // transferred ownership to a FrameContext fence record).
  const Operation &sourceOperation(const ReplayExecutionOperation &item) const;

private:
  friend struct ReplayExecutionPlanResult;
  friend ReplayExecutionPlanResult
      planModernReplayExecution(std::span<const CrashReplayFramePtr>);

  std::vector<CrashReplayFramePtr> frames_;
  std::vector<ReplayExecutionOperation> operations_;
};

enum class ReplayExecutionPlanReason : uint8_t {
  None,
  EmptyBatch,
  NullFrame,
  InvalidFrameLayout,
  PolicyDiscontinuity,
  DonorDepthStateMismatch,
};

struct ReplayExecutionPlanResult {
  static constexpr std::size_t NO_INDEX =
      std::numeric_limits<std::size_t>::max();

  ReplayExecutionPlanReason reason = ReplayExecutionPlanReason::None;
  std::size_t frameIndex = NO_INDEX;
  std::size_t operationIndex = NO_INDEX;
  ModernReplayExecutionPlan plan;

  bool accepted() const noexcept {
    return reason == ReplayExecutionPlanReason::None;
  }
};

// The input must be the exact strong-reference prefix returned by successful
// strict M1 preflight (or by ModernReplaySubmissionPlan::frames() on its
// M1Replay route).  Planning is atomic: a semantic mismatch returns an empty
// plan and identifies its first source operation.
ReplayExecutionPlanResult
planModernReplayExecution(std::span<const CrashReplayFramePtr> frames);

ReplayPrecisionCoverage
measureReplayPrecisionCoverage(std::span<const CrashReplayFramePtr> frames);

bool donorTrueDepthEligible(const DrawPacket &packet) noexcept;
std::string_view
replayExecutionPlanReasonName(ReplayExecutionPlanReason reason) noexcept;

} // namespace ps1::gpu::replay
