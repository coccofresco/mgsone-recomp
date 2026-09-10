#include "runtime/gpu/modern_replay_visibility_audit.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <vector>

using namespace ps1::gpu::replay;

namespace {

template <typename T>
ReplayVisibilityAuditPlaneView plane(const std::vector<T> &values,
                                     uint32_t width) {
  return {values.data(), values.size() * sizeof(T), width * sizeof(T)};
}

ReplayVisibilityAuditOwner owner(std::size_t ordinal) {
  ReplayVisibilityAuditOwner result;
  result.packetSequence = ordinal;
  result.submissionId = 1;
  result.sourceOrdinal = ordinal;
  result.route = PacketRoute::Projected3D;
  result.opaque = true;
  result.fullyPrecise = true;
  result.authenticDepth = true;
  return result;
}

constexpr uint8_t exactEvidence = REPLAY_VISIBILITY_AUDIT_REQUIRED_EVIDENCE;

} // namespace

TEST(GpuModernReplayVisibilityAuditTest, CombinesOnlyIndependentPassEvidence) {
  constexpr uint8_t ot = ReplayVisibilityAuditOtOwnerExact |
                         ReplayVisibilityAuditAcceptanceMatched |
                         ReplayVisibilityAuditBarrierFree;
  constexpr uint8_t strict = ReplayVisibilityAuditDepthOwnerExact |
                             ReplayVisibilityAuditAcceptanceMatched |
                             ReplayVisibilityAuditBarrierFree;
  EXPECT_EQ(combineReplayVisibilityAuditEvidence(ot, strict), exactEvidence);
  EXPECT_EQ(combineReplayVisibilityAuditEvidence(
                ot, ReplayVisibilityAuditDepthOwnerExact),
            ReplayVisibilityAuditOtOwnerExact |
                ReplayVisibilityAuditDepthOwnerExact);
  EXPECT_EQ(combineReplayVisibilityAuditEvidence(0xffu, 0u),
            ReplayVisibilityAuditOtOwnerExact);
}

TEST(GpuModernReplayVisibilityAuditTest,
     CombinesCandidateEvidenceOnlyAcrossThreeIndependentPasses) {
  constexpr uint8_t common =
      ReplayVisibilityAuditAcceptanceMatched | ReplayVisibilityAuditBarrierFree;
  constexpr uint8_t ot = ReplayVisibilityAuditOtOwnerExact | common;
  constexpr uint8_t strict = ReplayVisibilityAuditDepthOwnerExact | common;
  constexpr uint8_t lifted = ReplayVisibilityAuditLiftedOwnerExact | common;
  EXPECT_EQ(combineReplayVisibilityCandidateEvidence(ot, strict, lifted),
            REPLAY_VISIBILITY_CANDIDATE_REQUIRED_EVIDENCE);
  EXPECT_EQ(combineReplayVisibilityCandidateEvidence(
                ot, strict,
                ReplayVisibilityAuditLiftedOwnerExact |
                    ReplayVisibilityAuditBarrierFree),
            ReplayVisibilityAuditOtOwnerExact |
                ReplayVisibilityAuditDepthOwnerExact |
                ReplayVisibilityAuditLiftedOwnerExact |
                ReplayVisibilityAuditBarrierFree);
  EXPECT_EQ(combineReplayVisibilityCandidateEvidence(0xffu, 0u, 0u),
            ReplayVisibilityAuditOtOwnerExact);
}

TEST(GpuModernReplayVisibilityAuditTest,
     SeparatesCertainConflictCoverageAndUnknownSamples) {
  constexpr uint32_t width = 2;
  const std::vector<uint32_t> otOwners{1, 2, 2, 2};
  const std::vector<uint32_t> depthOwners{1, 1, 0, 1};
  const std::vector<float> otDepth{0.2f, 0.8f, 0.8f, 0.8f};
  const std::vector<float> strictDepth{0.2f, 0.2f, 1.0f, 0.2f};
  const std::vector<uint8_t> evidence{
      exactEvidence, exactEvidence, exactEvidence,
      static_cast<uint8_t>(exactEvidence & ~ReplayVisibilityAuditBarrierFree)};
  const ReplayVisibilityAuditPlanes planes{
      plane(otOwners, width), plane(depthOwners, width), plane(otDepth, width),
      plane(strictDepth, width), plane(evidence, width)};
  const std::vector<ReplayVisibilityAuditOwner> owners{owner(4), owner(9)};

  const ReplayVisibilityAuditReport report = auditReplayVisibility(
      {32, 64, width, 2}, planes, owners, {.maximumRecordedSamples = 8});

  ASSERT_EQ(report.status, ReplayVisibilityAuditStatus::Compared);
  EXPECT_EQ(report.totalSamples, 4u);
  EXPECT_EQ(report.agreementSamples, 1u);
  EXPECT_EQ(report.backgroundAgreementSamples, 0u);
  EXPECT_EQ(report.ownedAgreementSamples, 1u);
  EXPECT_EQ(report.otOwnedSamples, 4u);
  EXPECT_EQ(report.depthOwnedSamples, 3u);
  EXPECT_EQ(report.confirmedConflictSamples, 1u);
  EXPECT_EQ(report.coverageDivergenceSamples, 1u);
  EXPECT_EQ(report.indeterminateSamples, 1u);
  EXPECT_EQ(report.inconsistentEvidenceSamples, 0u);
  EXPECT_EQ(report.barrierContaminatedSamples, 1u);
  ASSERT_EQ(report.conflicts.size(), 1u);
  const ReplayVisibilityAuditConflict &conflict = report.conflicts.front();
  EXPECT_EQ(conflict.otOwner, 2u);
  EXPECT_EQ(conflict.depthOwner, 1u);
  EXPECT_EQ(conflict.samples, 1u);
  EXPECT_EQ(conflict.firstAtlasX, 33u);
  EXPECT_EQ(conflict.firstAtlasY, 64u);
  EXPECT_FLOAT_EQ(conflict.minimumOtDepth, 0.8f);
  EXPECT_FLOAT_EQ(conflict.maximumStrictDepth, 0.2f);
  ASSERT_EQ(report.samples.size(), 3u);
  EXPECT_EQ(report.samples[0].classification,
            ReplayVisibilityAuditSampleClass::ConfirmedOrderDepthConflict);
  EXPECT_EQ(report.samples[1].classification,
            ReplayVisibilityAuditSampleClass::CoverageDivergence);
  EXPECT_EQ(report.samples[2].classification,
            ReplayVisibilityAuditSampleClass::Indeterminate);
}

TEST(GpuModernReplayVisibilityAuditTest,
     RejectsOwnerOrderAndDepthRelationContradictions) {
  constexpr uint32_t width = 2;
  const std::vector<uint32_t> otOwners{1, 2};
  const std::vector<uint32_t> depthOwners{2, 1};
  const std::vector<float> otDepth{0.8f, 0.2f};
  const std::vector<float> strictDepth{0.2f, 0.8f};
  const std::vector<uint8_t> evidence(width, exactEvidence);
  const ReplayVisibilityAuditPlanes planes{
      plane(otOwners, width), plane(depthOwners, width), plane(otDepth, width),
      plane(strictDepth, width), plane(evidence, width)};
  const std::vector<ReplayVisibilityAuditOwner> owners{owner(4), owner(9)};

  const ReplayVisibilityAuditReport report =
      auditReplayVisibility({0, 0, width, 1}, planes, owners);

  EXPECT_EQ(report.confirmedConflictSamples, 0u);
  EXPECT_EQ(report.inconsistentEvidenceSamples, 2u);
  EXPECT_EQ(report.ownerOrderContradictionSamples, 1u);
  EXPECT_EQ(report.depthRelationContradictionSamples, 1u);
  EXPECT_TRUE(report.conflicts.empty());
}

TEST(GpuModernReplayVisibilityAuditTest,
     FailsClosedForIneligibleOwnersAndCorruptTokens) {
  constexpr uint32_t width = 1;
  const std::vector<uint32_t> otOwners{2};
  const std::vector<uint32_t> depthOwners{1};
  const std::vector<float> otDepth{0.8f};
  const std::vector<float> strictDepth{0.2f};
  const std::vector<uint8_t> evidence{exactEvidence};
  const ReplayVisibilityAuditPlanes planes{
      plane(otOwners, width), plane(depthOwners, width), plane(otDepth, width),
      plane(strictDepth, width), plane(evidence, width)};
  std::vector<ReplayVisibilityAuditOwner> owners{owner(4), owner(9)};
  owners[1].opaque = false;

  ReplayVisibilityAuditReport report =
      auditReplayVisibility({0, 0, width, 1}, planes, owners);
  EXPECT_EQ(report.status, ReplayVisibilityAuditStatus::Compared);
  EXPECT_EQ(report.indeterminateSamples, 1u);
  EXPECT_EQ(report.ineligibleOwnerSamples, 1u);
  EXPECT_FALSE(report.hasCertainConflict());

  owners.pop_back();
  report = auditReplayVisibility({0, 0, width, 1}, planes, owners);
  EXPECT_EQ(report.status, ReplayVisibilityAuditStatus::InvalidOwnerToken);
  EXPECT_FALSE(report.hasCertainConflict());
}

TEST(GpuModernReplayVisibilityAuditTest, RejectsTruncatedOrEmptyPlanes) {
  const std::vector<uint32_t> ownerValues{0};
  const std::vector<float> depthValues{1.0f};
  const std::vector<uint8_t> evidence{exactEvidence};
  ReplayVisibilityAuditPlanes planes{
      plane(ownerValues, 1), plane(ownerValues, 1), plane(depthValues, 1),
      plane(depthValues, 1), plane(evidence, 1)};
  planes.depthOwnerR32.sizeBytes = 3;

  EXPECT_EQ(auditReplayVisibility({0, 0, 1, 1}, planes, {}).status,
            ReplayVisibilityAuditStatus::InvalidPlane);
  EXPECT_EQ(auditReplayVisibility({0, 0, 0, 1}, planes, {}).status,
            ReplayVisibilityAuditStatus::EmptyRegion);
}

TEST(GpuModernReplayVisibilityAuditTest,
     CandidateGateRequiresEveryConflictResolvedAndAgreementPreserved) {
  constexpr uint32_t width = 3;
  const std::vector<uint32_t> otOwners{0, 1, 2};
  const std::vector<uint32_t> strictOwners{0, 1, 1};
  const std::vector<uint32_t> liftedOwners{0, 1, 2};
  const std::vector<float> otDepth{1.0f, 0.2f, 0.8f};
  const std::vector<float> strictDepth{1.0f, 0.2f, 0.2f};
  const std::vector<float> liftedDepth{1.0f, 0.2f, 0.7f};
  const std::vector<uint8_t> evidence(
      width, REPLAY_VISIBILITY_CANDIDATE_REQUIRED_EVIDENCE);
  const ReplayVisibilityCandidateAuditPlanes planes{
      plane(otOwners, width),     plane(strictOwners, width),
      plane(liftedOwners, width), plane(otDepth, width),
      plane(strictDepth, width),  plane(liftedDepth, width),
      plane(evidence, width)};
  const std::vector<ReplayVisibilityAuditOwner> owners{owner(4), owner(9)};

  const ReplayVisibilityCandidateAuditReport report =
      auditReplayVisibilityCandidate({10, 20, width, 1}, planes, owners,
                                     {.maximumRecordedSamples = 8});

  ASSERT_EQ(report.status, ReplayVisibilityAuditStatus::Compared);
  EXPECT_EQ(report.baselineAgreementSamples, 2u);
  EXPECT_EQ(report.agreementPreservedSamples, 2u);
  EXPECT_EQ(report.baselineConflictSamples, 1u);
  EXPECT_EQ(report.conflictResolvedToOtSamples, 1u);
  EXPECT_EQ(report.conflictRetainedByStrictSamples, 0u);
  EXPECT_TRUE(report.passesRasterGate());
  ASSERT_EQ(report.conflicts.size(), 1u);
  EXPECT_EQ(report.conflicts[0].resolvedToOtSamples, 1u);
  ASSERT_EQ(report.samples.size(), 1u);
  EXPECT_EQ(report.samples[0].classification,
            ReplayVisibilityCandidateSampleClass::ConflictResolvedToOt);
}

TEST(GpuModernReplayVisibilityAuditTest,
     CandidateAuditSeparatesRetainedChangedAndRegressedWinners) {
  constexpr uint32_t width = 3;
  const std::vector<uint32_t> otOwners{2, 2, 1};
  const std::vector<uint32_t> strictOwners{1, 1, 1};
  const std::vector<uint32_t> liftedOwners{1, 3, 2};
  const std::vector<float> otDepth{0.8f, 0.8f, 0.2f};
  const std::vector<float> strictDepth{0.2f, 0.2f, 0.2f};
  const std::vector<float> liftedDepth{0.2f, 0.4f, 0.3f};
  const std::vector<uint8_t> evidence(
      width, REPLAY_VISIBILITY_CANDIDATE_REQUIRED_EVIDENCE);
  const ReplayVisibilityCandidateAuditPlanes planes{
      plane(otOwners, width),     plane(strictOwners, width),
      plane(liftedOwners, width), plane(otDepth, width),
      plane(strictDepth, width),  plane(liftedDepth, width),
      plane(evidence, width)};
  const std::vector<ReplayVisibilityAuditOwner> owners{owner(4), owner(9),
                                                       owner(7)};

  const ReplayVisibilityCandidateAuditReport report =
      auditReplayVisibilityCandidate({0, 0, width, 1}, planes, owners,
                                     {.maximumRecordedSamples = 8});

  EXPECT_EQ(report.baselineConflictSamples, 2u);
  EXPECT_EQ(report.conflictRetainedByStrictSamples, 1u);
  EXPECT_EQ(report.conflictChangedWinnerSamples, 1u);
  EXPECT_EQ(report.candidateRegressionSamples, 1u);
  EXPECT_FALSE(report.passesRasterGate());
  ASSERT_EQ(report.outcomes.size(), 3u);
  EXPECT_EQ(report.outcomes[0].classification,
            ReplayVisibilityCandidateSampleClass::ConflictRetainedByStrict);
  EXPECT_EQ(report.outcomes[0].otOwner, 2u);
  EXPECT_EQ(report.outcomes[0].strictOwner, 1u);
  EXPECT_EQ(report.outcomes[0].liftedOwner, 1u);
  EXPECT_EQ(report.outcomes[0].samples, 1u);
  EXPECT_EQ(report.outcomes[0].firstAtlasX, 0u);
  EXPECT_FLOAT_EQ(report.outcomes[0].minimumLiftedDepth, 0.2f);
  EXPECT_FLOAT_EQ(report.outcomes[0].maximumOtDepth, 0.8f);
  EXPECT_EQ(report.outcomes[1].classification,
            ReplayVisibilityCandidateSampleClass::ConflictChangedWinner);
  EXPECT_EQ(report.outcomes[1].liftedOwner, 3u);
  EXPECT_EQ(report.outcomes[2].classification,
            ReplayVisibilityCandidateSampleClass::CandidateRegression);
  EXPECT_EQ(report.outcomes[2].otOwner, 1u);
  EXPECT_EQ(report.outcomes[2].liftedOwner, 2u);
  ASSERT_EQ(report.samples.size(), 3u);
  EXPECT_EQ(report.samples[0].classification,
            ReplayVisibilityCandidateSampleClass::ConflictRetainedByStrict);
  EXPECT_EQ(report.samples[1].classification,
            ReplayVisibilityCandidateSampleClass::ConflictChangedWinner);
  EXPECT_EQ(report.samples[2].classification,
            ReplayVisibilityCandidateSampleClass::CandidateRegression);
}

TEST(GpuModernReplayVisibilityAuditTest,
     CandidateAuditFailsClosedForCoverageEvidenceAndPlaneErrors) {
  constexpr uint32_t width = 3;
  const std::vector<uint32_t> otOwners{2, 2, 2};
  const std::vector<uint32_t> strictOwners{0, 1, 1};
  const std::vector<uint32_t> liftedOwners{2, 0, 2};
  const std::vector<float> otDepth(width, 0.8f);
  const std::vector<float> strictDepth{1.0f, 0.2f, 0.2f};
  const std::vector<float> liftedDepth{0.8f, 1.0f, 0.8f};
  std::vector<uint8_t> evidence(width,
                                REPLAY_VISIBILITY_CANDIDATE_REQUIRED_EVIDENCE);
  evidence[2] = static_cast<uint8_t>(evidence[2] &
                                     ~ReplayVisibilityAuditLiftedOwnerExact);
  ReplayVisibilityCandidateAuditPlanes planes{
      plane(otOwners, width),     plane(strictOwners, width),
      plane(liftedOwners, width), plane(otDepth, width),
      plane(strictDepth, width),  plane(liftedDepth, width),
      plane(evidence, width)};
  const std::vector<ReplayVisibilityAuditOwner> owners{owner(4), owner(9)};

  ReplayVisibilityCandidateAuditReport report =
      auditReplayVisibilityCandidate({0, 0, width, 1}, planes, owners);
  EXPECT_EQ(report.baselineCoverageDivergenceSamples, 1u);
  EXPECT_EQ(report.candidateCoverageDivergenceSamples, 1u);
  EXPECT_EQ(report.indeterminateSamples, 1u);
  EXPECT_EQ(report.missingLiftedOwnerEvidenceSamples, 1u);
  EXPECT_FALSE(report.passesRasterGate());

  planes.liftedDepthD32.sizeBytes = sizeof(float) * 2u;
  report = auditReplayVisibilityCandidate({0, 0, width, 1}, planes, owners);
  EXPECT_EQ(report.status, ReplayVisibilityAuditStatus::InvalidPlane);
}
