#include "runtime/gpu/modern_replay_visibility_stack.h"

#include <gtest/gtest.h>

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

ReplayVisibilityAuditOwner comparableOwner(std::size_t ordinal) {
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

ReplayVisibilityStackFragment fragment(uint32_t sample, uint32_t owner,
                                       float depth, uint32_t operation) {
  return {sample, owner, std::bit_cast<uint32_t>(depth), operation};
}

constexpr uint8_t exactOtEvidence =
    ReplayVisibilityAuditOtOwnerExact |
    ReplayVisibilityAuditAcceptanceMatched |
    ReplayVisibilityAuditBarrierFree;

} // namespace

TEST(GpuModernReplayVisibilityStackTest,
     CapturesEveryPhysicalOccluderWithoutAFamilyOrLayerCap) {
  constexpr uint32_t width = 1;
  const std::vector<uint32_t> otOwners{6};
  const std::vector<float> otDepth{0.9f};
  const std::vector<uint8_t> evidence{exactOtEvidence};
  const ReplayVisibilityStackPlanes planes{
      plane(otOwners, width), plane(otDepth, width), plane(evidence, width)};

  std::vector<ReplayVisibilityAuditOwner> owners;
  for (std::size_t ordinal = 0; ordinal < 6; ++ordinal)
    owners.push_back(comparableOwner(ordinal));
  const std::vector<ReplayVisibilityStackFragment> fragments{
      fragment(0, 4, 0.4f, 4), fragment(0, 1, 0.1f, 1),
      fragment(0, 6, 0.9f, 6), fragment(0, 3, 0.3f, 3),
      fragment(0, 5, 0.5f, 5), fragment(0, 2, 0.2f, 2)};

  const ReplayVisibilityStackReport report = auditReplayVisibilityStack(
      {10, 20, width, 1}, planes, owners, REPLAY_VISIBILITY_STACK_SCHEMA,
      fragments.size(), 32, false, fragments, {});

  ASSERT_EQ(report.status, ReplayVisibilityStackStatus::Compared);
  EXPECT_TRUE(report.complete());
  EXPECT_EQ(report.completeOwnedSamples, 1u);
  EXPECT_EQ(report.occludedOtSamples, 1u);
  EXPECT_EQ(report.maximumDistinctOwnersPerSample, 6u);
  ASSERT_EQ(report.occluders.size(), 5u);
  for (std::size_t index = 0; index < report.occluders.size(); ++index) {
    const ReplayVisibilityStackOccluder &occluder = report.occluders[index];
    EXPECT_EQ(occluder.otOwner, 6u);
    EXPECT_EQ(occluder.physicalOwner, index + 1u);
    EXPECT_EQ(occluder.samples, 1u);
    EXPECT_EQ(occluder.firstAtlasX, 10u);
    EXPECT_EQ(occluder.firstAtlasY, 20u);
    EXPECT_FLOAT_EQ(occluder.minimumOtDepth, 0.9f);
    EXPECT_FLOAT_EQ(occluder.maximumPhysicalDepth,
                    static_cast<float>(index + 1u) / 10.0f);
  }
}

TEST(GpuModernReplayVisibilityStackTest,
     SpatialAndFragmentBoundariesInvalidateOnlyEarlierSamples) {
  constexpr uint32_t width = 3;
  const std::vector<uint32_t> otOwners{2, 2, 2};
  const std::vector<float> otDepth{0.8f, 0.7f, 0.6f};
  const std::vector<uint8_t> evidence(width, exactOtEvidence);
  const ReplayVisibilityStackPlanes planes{
      plane(otOwners, width), plane(otDepth, width), plane(evidence, width)};
  const std::vector<ReplayVisibilityAuditOwner> owners{
      comparableOwner(1), comparableOwner(2)};
  const std::vector<ReplayVisibilityStackFragment> fragments{
      fragment(0, 1, 0.2f, 1), fragment(0, 2, 0.8f, 3),
      fragment(1, 1, 0.1f, 1), fragment(1, 2, 0.7f, 3),
      fragment(2, 1, 0.15f, 1), fragment(2, 0, 1.0f, 2),
      fragment(2, 2, 0.6f, 3)};
  const std::vector<ReplayVisibilityStackBarrier> barriers{
      {2, {11, 20, 1, 1}}};

  const ReplayVisibilityStackReport report = auditReplayVisibilityStack(
      {10, 20, width, 1}, planes, owners, REPLAY_VISIBILITY_STACK_SCHEMA,
      fragments.size(), fragments.size(), false, fragments, barriers);

  ASSERT_EQ(report.status, ReplayVisibilityStackStatus::Compared);
  EXPECT_TRUE(report.complete());
  EXPECT_EQ(report.boundaryRecords, 1u);
  EXPECT_EQ(report.staleRecords, 2u);
  EXPECT_EQ(report.completeOwnedSamples, 3u);
  EXPECT_EQ(report.maximumDistinctOwnersPerSample, 2u);
  EXPECT_EQ(report.occludedOtSamples, 1u);
  ASSERT_EQ(report.occluders.size(), 1u);
  EXPECT_EQ(report.occluders[0].otOwner, 2u);
  EXPECT_EQ(report.occluders[0].physicalOwner, 1u);
  EXPECT_EQ(report.occluders[0].samples, 1u);
  EXPECT_EQ(report.occluders[0].firstAtlasX, 10u);
}

TEST(GpuModernReplayVisibilityStackTest,
     OverflowFailsClosedAndPreservesRequiredCapacityEvidence) {
  const std::vector<uint32_t> otOwners{1};
  const std::vector<float> otDepth{0.5f};
  const std::vector<uint8_t> evidence{exactOtEvidence};
  const ReplayVisibilityStackPlanes planes{
      plane(otOwners, 1), plane(otDepth, 1), plane(evidence, 1)};
  const std::vector<ReplayVisibilityAuditOwner> owners{comparableOwner(1)};
  const std::vector<ReplayVisibilityStackFragment> retained{
      fragment(0, 1, 0.5f, 1), fragment(0, 1, 0.5f, 2),
      fragment(0, 1, 0.5f, 3)};

  const ReplayVisibilityStackReport report = auditReplayVisibilityStack(
      {0, 0, 1, 1}, planes, owners, REPLAY_VISIBILITY_STACK_SCHEMA, 5, 3,
      true, retained, {});

  EXPECT_EQ(report.status, ReplayVisibilityStackStatus::Overflow);
  EXPECT_FALSE(report.complete());
  EXPECT_EQ(report.attemptedRecords, 5u);
  EXPECT_EQ(report.retainedRecords, 3u);
  EXPECT_TRUE(report.occluders.empty());
}

TEST(GpuModernReplayVisibilityStackTest,
     MissingOtFragmentAndConflictingDuplicatesCannotPassCompleteness) {
  constexpr uint32_t width = 2;
  const std::vector<uint32_t> otOwners{2, 2};
  const std::vector<float> otDepth{0.8f, 0.8f};
  const std::vector<uint8_t> evidence(width, exactOtEvidence);
  const ReplayVisibilityStackPlanes planes{
      plane(otOwners, width), plane(otDepth, width), plane(evidence, width)};
  const std::vector<ReplayVisibilityAuditOwner> owners{
      comparableOwner(1), comparableOwner(2)};
  const std::vector<ReplayVisibilityStackFragment> fragments{
      fragment(0, 1, 0.2f, 1), fragment(1, 2, 0.8f, 1),
      fragment(1, 2, 0.7f, 2)};

  const ReplayVisibilityStackReport report = auditReplayVisibilityStack(
      {0, 0, width, 1}, planes, owners, REPLAY_VISIBILITY_STACK_SCHEMA,
      fragments.size(), fragments.size(), false, fragments, {});

  EXPECT_EQ(report.status, ReplayVisibilityStackStatus::Compared);
  EXPECT_FALSE(report.complete());
  EXPECT_EQ(report.missingOtFragmentSamples, 1u);
  EXPECT_EQ(report.duplicateOwnerRecords, 1u);
  EXPECT_EQ(report.inconsistentDuplicateDepthRecords, 1u);
  EXPECT_EQ(report.invalidDepthSamples, 1u);
  EXPECT_EQ(report.completeOwnedSamples, 0u);
  EXPECT_TRUE(report.occluders.empty());
}

TEST(GpuModernReplayVisibilityStackTest, RejectsMalformedHeadersAndTokens) {
  const std::vector<uint32_t> otOwners{1};
  const std::vector<float> otDepth{0.5f};
  const std::vector<uint8_t> evidence{exactOtEvidence};
  const ReplayVisibilityStackPlanes planes{
      plane(otOwners, 1), plane(otDepth, 1), plane(evidence, 1)};
  const std::vector<ReplayVisibilityAuditOwner> owners{comparableOwner(1)};
  const std::vector<ReplayVisibilityStackFragment> fragments{
      fragment(0, 2, 0.5f, 1)};

  EXPECT_EQ(auditReplayVisibilityStack(
                {0, 0, 1, 1}, planes, owners,
                REPLAY_VISIBILITY_STACK_SCHEMA + 1u, fragments.size(),
                fragments.size(), false, fragments, {})
                .status,
            ReplayVisibilityStackStatus::InvalidHeader);
  EXPECT_EQ(auditReplayVisibilityStack(
                {0, 0, 1, 1}, planes, owners,
                REPLAY_VISIBILITY_STACK_SCHEMA, fragments.size(),
                fragments.size(), false, fragments, {})
                .status,
            ReplayVisibilityStackStatus::InvalidOwnerToken);
}
