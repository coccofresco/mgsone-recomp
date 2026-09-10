#pragma once

#include "runtime/gpu/modern_replay.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps1::gpu::replay {

// The visibility audit compares two diagnostic owner renders of the exact
// same accepted fragment stream:
//
//   * otOwner: painter/ordering-table authority (ALWAYS);
//   * depthOwner: counterfactual physical-depth authority (LEQUAL).
//
// Owner token zero is background/unknown. Non-zero tokens are one-based
// indices into the immutable owner catalog supplied with the planes.
struct ReplayVisibilityAuditPlaneView {
  const void *data = nullptr;
  std::size_t sizeBytes = 0;
  std::size_t rowPitchBytes = 0;
};

struct ReplayVisibilityAuditRegion {
  uint32_t atlasX = 0;
  uint32_t atlasY = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

enum ReplayVisibilityAuditSampleEvidence : uint8_t {
  ReplayVisibilityAuditOtOwnerExact = 1u << 0,
  ReplayVisibilityAuditDepthOwnerExact = 1u << 1,
  ReplayVisibilityAuditAcceptanceMatched = 1u << 2,
  ReplayVisibilityAuditBarrierFree = 1u << 3,
  // Set only by the independent candidate-depth pass. It is deliberately
  // distinct from DepthOwnerExact: replaying the strict attachment cannot
  // accidentally certify candidate coverage.
  ReplayVisibilityAuditLiftedOwnerExact = 1u << 4,
};

inline constexpr uint8_t REPLAY_VISIBILITY_AUDIT_REQUIRED_EVIDENCE =
    ReplayVisibilityAuditOtOwnerExact | ReplayVisibilityAuditDepthOwnerExact |
    ReplayVisibilityAuditAcceptanceMatched | ReplayVisibilityAuditBarrierFree;

inline constexpr uint8_t REPLAY_VISIBILITY_CANDIDATE_REQUIRED_EVIDENCE =
    REPLAY_VISIBILITY_AUDIT_REQUIRED_EVIDENCE |
    ReplayVisibilityAuditLiftedOwnerExact;

// Each GPU pass owns its evidence independently. Only pass-specific owner
// bits cross directly; acceptance and barrier-free status require agreement
// from both sides. Unknown bits are deliberately discarded.
inline constexpr uint8_t
combineReplayVisibilityAuditEvidence(uint8_t otEvidence,
                                     uint8_t strictEvidence) noexcept {
  uint8_t combined = static_cast<uint8_t>(
      (otEvidence & ReplayVisibilityAuditOtOwnerExact) |
      (strictEvidence & ReplayVisibilityAuditDepthOwnerExact));
  if ((otEvidence & ReplayVisibilityAuditAcceptanceMatched) != 0 &&
      (strictEvidence & ReplayVisibilityAuditAcceptanceMatched) != 0) {
    combined =
        static_cast<uint8_t>(combined | ReplayVisibilityAuditAcceptanceMatched);
  }
  if ((otEvidence & ReplayVisibilityAuditBarrierFree) != 0 &&
      (strictEvidence & ReplayVisibilityAuditBarrierFree) != 0) {
    combined =
        static_cast<uint8_t>(combined | ReplayVisibilityAuditBarrierFree);
  }
  return combined;
}

// Candidate acceptance/barrier evidence is valid only when all three passes
// agree. Owner bits remain pass-local. Unknown bits are discarded.
inline constexpr uint8_t
combineReplayVisibilityCandidateEvidence(uint8_t otEvidence,
                                         uint8_t strictEvidence,
                                         uint8_t liftedEvidence) noexcept {
  uint8_t combined = static_cast<uint8_t>(
      (otEvidence & ReplayVisibilityAuditOtOwnerExact) |
      (strictEvidence & ReplayVisibilityAuditDepthOwnerExact) |
      (liftedEvidence & ReplayVisibilityAuditLiftedOwnerExact));
  if ((otEvidence & ReplayVisibilityAuditAcceptanceMatched) != 0 &&
      (strictEvidence & ReplayVisibilityAuditAcceptanceMatched) != 0 &&
      (liftedEvidence & ReplayVisibilityAuditAcceptanceMatched) != 0) {
    combined =
        static_cast<uint8_t>(combined | ReplayVisibilityAuditAcceptanceMatched);
  }
  if ((otEvidence & ReplayVisibilityAuditBarrierFree) != 0 &&
      (strictEvidence & ReplayVisibilityAuditBarrierFree) != 0 &&
      (liftedEvidence & ReplayVisibilityAuditBarrierFree) != 0) {
    combined =
        static_cast<uint8_t>(combined | ReplayVisibilityAuditBarrierFree);
  }
  return combined;
}

struct ReplayVisibilityAuditPlanes {
  ReplayVisibilityAuditPlaneView otOwnerR32{};
  ReplayVisibilityAuditPlaneView depthOwnerR32{};
  ReplayVisibilityAuditPlaneView otDepthD32{};
  ReplayVisibilityAuditPlaneView strictDepthD32{};
  ReplayVisibilityAuditPlaneView evidenceR8{};
};

// Third-pass candidate input. "Lifted" means only that an explicit
// homogeneous-W multiplier was presented to the independent LEQUAL pass; it
// does not imply that the multiplier or its component assignment is valid.
struct ReplayVisibilityCandidateAuditPlanes {
  ReplayVisibilityAuditPlaneView otOwnerR32{};
  ReplayVisibilityAuditPlaneView strictOwnerR32{};
  ReplayVisibilityAuditPlaneView liftedOwnerR32{};
  ReplayVisibilityAuditPlaneView otDepthD32{};
  ReplayVisibilityAuditPlaneView strictDepthD32{};
  ReplayVisibilityAuditPlaneView liftedDepthD32{};
  ReplayVisibilityAuditPlaneView evidenceR8{};
};

// One catalog entry describes one diagnostic owner token. A packet may own
// multiple tokens when its triangles must remain distinguishable.
struct ReplayVisibilityAuditOwner {
  uint64_t packetSequence = UNASSIGNED_PACKET_SEQUENCE;
  uint64_t submissionId = 0;
  std::size_t sourceOrdinal = 0;
  std::size_t frameIndex = 0;
  std::size_t operationIndex = 0;
  uint8_t triangleIndex = 0;
  ReplayTriangleProvenance provenance{};
  // Numeric observation retained beside, but deliberately excluded from,
  // categorical owner identity. It is the effective positive GTE W consumed
  // by Vulkan for each source corner and lets per-game research derive a
  // physical/OT affine relation without fitting against screenshots.
  std::array<float, 3> effectiveProjectiveW{};
  // Exact packet coordinates retained for viewport/culling diagnosis. Native
  // X/Y are the submitted PS1 integers after draw offset; precise X/Y are the
  // corresponding high-precision replay inputs before presentation scaling.
  // They are observations only and never participate in owner identity.
  std::array<int32_t, 3> nativeX{};
  std::array<int32_t, 3> nativeY{};
  std::array<double, 3> preciseX{};
  std::array<double, 3> preciseY{};
  PacketRoute route = PacketRoute::Projected3D;
  bool opaque = false;
  bool fullyPrecise = false;
  bool authenticDepth = false;

  bool comparable() const {
    return route == PacketRoute::Projected3D && opaque && fullyPrecise &&
           authenticDepth;
  }
};

enum class ReplayVisibilityAuditStatus : uint8_t {
  Compared = 0,
  EmptyRegion,
  InvalidPlane,
  InvalidOwnerToken,
};

enum class ReplayVisibilityAuditSampleClass : uint8_t {
  Agreement = 0,
  ConfirmedOrderDepthConflict,
  CoverageDivergence,
  Indeterminate,
  InconsistentEvidence,
};

struct ReplayVisibilityAuditSample {
  uint32_t atlasX = 0;
  uint32_t atlasY = 0;
  uint32_t otOwner = 0;
  uint32_t depthOwner = 0;
  uint32_t otDepthBits = 0;
  uint32_t strictDepthBits = 0;
  uint8_t evidence = 0;
  ReplayVisibilityAuditSampleClass classification =
      ReplayVisibilityAuditSampleClass::Indeterminate;
};

struct ReplayVisibilityAuditConflict {
  uint32_t otOwner = 0;
  uint32_t depthOwner = 0;
  uint64_t samples = 0;
  uint32_t firstAtlasX = 0;
  uint32_t firstAtlasY = 0;
  uint32_t minAtlasX = 0;
  uint32_t minAtlasY = 0;
  uint32_t maxAtlasX = 0;
  uint32_t maxAtlasY = 0;
  float minimumOtDepth = 1.0f;
  float maximumOtDepth = 0.0f;
  float minimumStrictDepth = 1.0f;
  float maximumStrictDepth = 0.0f;
};

struct ReplayVisibilityAuditOptions {
  std::size_t maximumRecordedSamples = 64;
};

struct ReplayVisibilityAuditReport {
  ReplayVisibilityAuditStatus status =
      ReplayVisibilityAuditStatus::InvalidPlane;
  ReplayVisibilityAuditRegion region{};
  uint64_t totalSamples = 0;
  uint64_t agreementSamples = 0;
  uint64_t backgroundAgreementSamples = 0;
  uint64_t ownedAgreementSamples = 0;
  uint64_t otOwnedSamples = 0;
  uint64_t depthOwnedSamples = 0;
  uint64_t confirmedConflictSamples = 0;
  uint64_t coverageDivergenceSamples = 0;
  uint64_t indeterminateSamples = 0;
  uint64_t inconsistentEvidenceSamples = 0;
  uint64_t missingOtOwnerEvidenceSamples = 0;
  uint64_t missingDepthOwnerEvidenceSamples = 0;
  uint64_t unmatchedAcceptanceSamples = 0;
  uint64_t barrierContaminatedSamples = 0;
  uint64_t ineligibleOwnerSamples = 0;
  uint64_t invalidDepthSamples = 0;
  uint64_t ownerOrderContradictionSamples = 0;
  uint64_t depthRelationContradictionSamples = 0;
  std::vector<ReplayVisibilityAuditConflict> conflicts;
  std::vector<ReplayVisibilityAuditSample> samples;

  bool compared() const {
    return status == ReplayVisibilityAuditStatus::Compared;
  }

  bool hasCertainConflict() const {
    return compared() && confirmedConflictSamples != 0;
  }
};

enum class ReplayVisibilityCandidateSampleClass : uint8_t {
  AgreementPreserved = 0,
  ConflictResolvedToOt,
  ConflictRetainedByStrict,
  ConflictChangedWinner,
  BaselineCoverageDivergence,
  CandidateCoverageDivergence,
  CandidateRegression,
  Indeterminate,
  InconsistentEvidence,
};

struct ReplayVisibilityCandidateSample {
  uint32_t atlasX = 0;
  uint32_t atlasY = 0;
  uint32_t otOwner = 0;
  uint32_t strictOwner = 0;
  uint32_t liftedOwner = 0;
  uint32_t otDepthBits = 0;
  uint32_t strictDepthBits = 0;
  uint32_t liftedDepthBits = 0;
  uint8_t evidence = 0;
  ReplayVisibilityCandidateSampleClass classification =
      ReplayVisibilityCandidateSampleClass::Indeterminate;
};

struct ReplayVisibilityCandidateConflict {
  uint32_t otOwner = 0;
  uint32_t strictOwner = 0;
  uint64_t samples = 0;
  uint64_t resolvedToOtSamples = 0;
  uint64_t retainedByStrictSamples = 0;
  uint64_t changedWinnerSamples = 0;
};

// Aggregated non-passing raster outcome. Keeping the lifted winner makes a
// third-object intrusion distinguishable from a retained strict winner, while
// the first coordinate and depth extrema make the raw attachment result
// independently inspectable. This is rejection evidence only; it cannot
// assign a component or fit a new multiplier.
struct ReplayVisibilityCandidateOutcome {
  ReplayVisibilityCandidateSampleClass classification =
      ReplayVisibilityCandidateSampleClass::Indeterminate;
  uint32_t otOwner = 0;
  uint32_t strictOwner = 0;
  uint32_t liftedOwner = 0;
  uint64_t samples = 0;
  uint32_t firstAtlasX = 0;
  uint32_t firstAtlasY = 0;
  float minimumOtDepth = 1.0f;
  float maximumOtDepth = 0.0f;
  float minimumStrictDepth = 1.0f;
  float maximumStrictDepth = 0.0f;
  float minimumLiftedDepth = 1.0f;
  float maximumLiftedDepth = 0.0f;
};

struct ReplayVisibilityCandidateAuditReport {
  ReplayVisibilityAuditStatus status =
      ReplayVisibilityAuditStatus::InvalidPlane;
  ReplayVisibilityAuditRegion region{};
  uint64_t totalSamples = 0;
  uint64_t baselineAgreementSamples = 0;
  uint64_t agreementPreservedSamples = 0;
  uint64_t baselineConflictSamples = 0;
  uint64_t conflictResolvedToOtSamples = 0;
  uint64_t conflictRetainedByStrictSamples = 0;
  uint64_t conflictChangedWinnerSamples = 0;
  uint64_t baselineCoverageDivergenceSamples = 0;
  uint64_t candidateCoverageDivergenceSamples = 0;
  uint64_t candidateRegressionSamples = 0;
  uint64_t indeterminateSamples = 0;
  uint64_t inconsistentEvidenceSamples = 0;
  uint64_t missingOtOwnerEvidenceSamples = 0;
  uint64_t missingStrictOwnerEvidenceSamples = 0;
  uint64_t missingLiftedOwnerEvidenceSamples = 0;
  uint64_t unmatchedAcceptanceSamples = 0;
  uint64_t barrierContaminatedSamples = 0;
  uint64_t ineligibleOwnerSamples = 0;
  uint64_t invalidDepthSamples = 0;
  uint64_t ownerOrderContradictionSamples = 0;
  uint64_t depthRelationContradictionSamples = 0;
  std::vector<ReplayVisibilityCandidateConflict> conflicts;
  std::vector<ReplayVisibilityCandidateOutcome> outcomes;
  std::vector<ReplayVisibilityCandidateSample> samples;

  bool compared() const {
    return status == ReplayVisibilityAuditStatus::Compared;
  }

  // Passing is permission for broader holdout raster validation, not proof of
  // a unique gauge or a persistent presentation-family identity.
  bool passesRasterGate() const {
    return compared() && baselineConflictSamples != 0 &&
           conflictResolvedToOtSamples == baselineConflictSamples &&
           agreementPreservedSamples == baselineAgreementSamples &&
           conflictRetainedByStrictSamples == 0 &&
           conflictChangedWinnerSamples == 0 &&
           baselineCoverageDivergenceSamples == 0 &&
           candidateCoverageDivergenceSamples == 0 &&
           candidateRegressionSamples == 0 && indeterminateSamples == 0 &&
           inconsistentEvidenceSamples == 0;
  }
};

const char *replayVisibilityAuditStatusName(ReplayVisibilityAuditStatus status);

ReplayVisibilityAuditReport
auditReplayVisibility(const ReplayVisibilityAuditRegion &region,
                      const ReplayVisibilityAuditPlanes &planes,
                      const std::vector<ReplayVisibilityAuditOwner> &owners,
                      ReplayVisibilityAuditOptions options = {});

ReplayVisibilityCandidateAuditReport auditReplayVisibilityCandidate(
    const ReplayVisibilityAuditRegion &region,
    const ReplayVisibilityCandidateAuditPlanes &planes,
    const std::vector<ReplayVisibilityAuditOwner> &owners,
    ReplayVisibilityAuditOptions options = {});

} // namespace ps1::gpu::replay
