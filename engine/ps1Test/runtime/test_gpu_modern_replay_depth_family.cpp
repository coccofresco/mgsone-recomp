#include "runtime/gpu/modern_replay_depth_family.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace ps1::gpu::replay;

namespace {

ReplayDepthFamilyObjectKey object(uint64_t identity) {
  return {100 + identity, 200 + identity, 300 + identity, identity,
          REPLAY_DEPTH_FAMILY_REQUIRED_OBJECT_EVIDENCE};
}

ReplayDepthFamilyObservation observation(ReplayDepthFamilyObjectKey key,
                                         uint64_t lineage,
                                         ReplayDepthGauge gauge,
                                         uint64_t frame,
                                         ReplayDepthFamilyValidationSet set) {
  return {key,
          lineage,
          frame,
          gauge,
          ReplayDepthFamilyGaugeProof::GuestTransformDataflow,
          set};
}

void appendValidatedLineage(std::vector<ReplayDepthFamilyObservation> &values,
                            ReplayDepthFamilyObjectKey key, uint64_t lineage,
                            ReplayDepthGauge gauge, uint64_t firstFrame) {
  values.push_back(observation(key, lineage, gauge, firstFrame,
                               ReplayDepthFamilyValidationSet::Discovery));
  values.push_back(observation(key, lineage, gauge, firstFrame + 1,
                               ReplayDepthFamilyValidationSet::Holdout));
}

} // namespace

TEST(GpuModernReplayDepthFamilyTest,
     CertifiesReducedRationalsAcrossIndependentFrameSets) {
  std::vector<ReplayDepthFamilyObservation> observations;
  appendValidatedLineage(observations, object(1), 1001, {2, 2}, 20);
  appendValidatedLineage(observations, object(2), 1002, {3, 4}, 20);
  appendValidatedLineage(observations, object(3), 1002, {6, 8}, 20);

  const ReplayDepthFamilyReport report =
      certifyReplayDepthFamilies(observations);

  ASSERT_EQ(report.status, ReplayDepthFamilyStatus::Certified);
  ASSERT_EQ(report.families.size(), 2u);
  EXPECT_EQ(report.families[0].sourceGauge, (ReplayDepthGauge{3, 4}));
  EXPECT_EQ(report.families[0].objects.size(), 2u);
  EXPECT_EQ(report.families[1].sourceGauge, (ReplayDepthGauge{1, 1}));
  EXPECT_EQ(report.families[1].objects.size(), 1u);
  EXPECT_FALSE(report.prior.exceedsLikelyFamilyCount);
  EXPECT_FALSE(report.prior.allGaugesExpressibleInTenths);
  ASSERT_EQ(report.lineages.size(), 2u);
  EXPECT_EQ(report.lineages[1].discoveryFrames, std::vector<uint64_t>({20}));
  EXPECT_EQ(report.lineages[1].holdoutFrames, std::vector<uint64_t>({21}));
}

TEST(GpuModernReplayDepthFamilyTest,
     RejectsIncompleteOrNonIntrinsicObservations) {
  ReplayDepthFamilyObservation incomplete = observation(
      object(1), 1001, {1, 1}, 20,
      ReplayDepthFamilyValidationSet::Discovery);
  incomplete.object.evidence &= ~ReplayDepthFamilyTemporalIdentity;
  EXPECT_EQ(certifyReplayDepthFamilies({incomplete}).status,
            ReplayDepthFamilyStatus::InvalidObservation);

  ReplayDepthFamilyObservation heuristic = observation(
      object(1), 1001, {1, 1}, 20,
      ReplayDepthFamilyValidationSet::Discovery);
  heuristic.proof = ReplayDepthFamilyGaugeProof::None;
  EXPECT_EQ(certifyReplayDepthFamilies({heuristic}).status,
            ReplayDepthFamilyStatus::InvalidObservation);
}

TEST(GpuModernReplayDepthFamilyTest,
     RejectsContradictoryObjectAndGaugeLineageAssignments) {
  ReplayDepthFamilyObservation first = observation(
      object(1), 1001, {1, 1}, 20,
      ReplayDepthFamilyValidationSet::Discovery);
  ReplayDepthFamilyObservation moved = observation(
      object(1), 1002, {2, 1}, 21,
      ReplayDepthFamilyValidationSet::Holdout);
  EXPECT_EQ(certifyReplayDepthFamilies({first, moved}).status,
            ReplayDepthFamilyStatus::ContradictoryObject);

  ReplayDepthFamilyObservation differentObject = observation(
      object(2), 1001, {2, 1}, 21,
      ReplayDepthFamilyValidationSet::Holdout);
  EXPECT_EQ(certifyReplayDepthFamilies({first, differentObject}).status,
            ReplayDepthFamilyStatus::ContradictoryGaugeLineage);
}

TEST(GpuModernReplayDepthFamilyTest,
     TreatsFamilyCountAsPriorAndIdentityAnchorAsExplicitConvention) {
  std::vector<ReplayDepthFamilyObservation> fiveProvedFamilies;
  for (uint64_t factor : {1, 2, 3, 4, 5}) {
    appendValidatedLineage(fiveProvedFamilies, object(factor), 1000 + factor,
                           {factor, 1}, 20);
  }
  const ReplayDepthFamilyReport fiveFamilyReport =
      certifyReplayDepthFamilies(fiveProvedFamilies);
  EXPECT_EQ(fiveFamilyReport.status, ReplayDepthFamilyStatus::Certified);
  EXPECT_EQ(fiveFamilyReport.families.size(), 5u);
  EXPECT_TRUE(fiveFamilyReport.prior.exceedsLikelyFamilyCount);

  std::vector<ReplayDepthFamilyObservation> noIdentity;
  appendValidatedLineage(noIdentity, object(1), 1001, {2, 1}, 20);
  ReplayDepthFamilyOptions requireIdentity;
  requireIdentity.requireIdentityFamily = true;
  EXPECT_EQ(certifyReplayDepthFamilies(noIdentity, requireIdentity).status,
            ReplayDepthFamilyStatus::MissingIdentityFamily);
  EXPECT_EQ(certifyReplayDepthFamilies(noIdentity).status,
            ReplayDepthFamilyStatus::Certified);

  const std::vector<ReplayDepthFamilyObservation> sameFrame{
      observation(object(1), 1001, {1, 1}, 20,
                  ReplayDepthFamilyValidationSet::Discovery),
      observation(object(1), 1001, {1, 1}, 20,
                  ReplayDepthFamilyValidationSet::Holdout)};
  EXPECT_EQ(certifyReplayDepthFamilies(sameFrame).status,
            ReplayDepthFamilyStatus::MissingIndependentHoldout);
}

TEST(GpuModernReplayDepthFamilyTest, SortsLargeRationalsWithoutOverflow) {
  const uint64_t maximum = std::numeric_limits<uint64_t>::max();
  std::vector<ReplayDepthFamilyObservation> observations;
  appendValidatedLineage(observations, object(1), 1001,
                         {maximum, maximum - 1}, 20);
  appendValidatedLineage(observations, object(2), 1002, {1, 1}, 20);
  appendValidatedLineage(observations, object(3), 1003,
                         {maximum - 1, maximum}, 20);

  const ReplayDepthFamilyReport report =
      certifyReplayDepthFamilies(observations);

  ASSERT_EQ(report.status, ReplayDepthFamilyStatus::Certified);
  ASSERT_EQ(report.families.size(), 3u);
  EXPECT_EQ(report.families[0].sourceGauge,
            (ReplayDepthGauge{maximum - 1, maximum}));
  EXPECT_EQ(report.families[1].sourceGauge, (ReplayDepthGauge{1, 1}));
  EXPECT_EQ(report.families[2].sourceGauge,
            (ReplayDepthGauge{maximum, maximum - 1}));
}

TEST(GpuModernReplayDepthFamilyTest,
     AppliesOneCommonGaugeWithoutChangingScreenCoordinates) {
  const auto lifted =
      tryApplyReplayDepthGauge(60.0, -30.0, 120.0, {3, 2});
  ASSERT_TRUE(lifted.has_value());
  EXPECT_DOUBLE_EQ(lifted->qx, 40.0);
  EXPECT_DOUBLE_EQ(lifted->qy, -20.0);
  EXPECT_DOUBLE_EQ(lifted->w, 80.0);
  EXPECT_DOUBLE_EQ(lifted->qx / lifted->w, 0.5);
  EXPECT_DOUBLE_EQ(lifted->qy / lifted->w, -0.25);

  EXPECT_FALSE(
      tryApplyReplayDepthGauge(1.0, 1.0, 1.0, {0, 1}).has_value());
  EXPECT_FALSE(
      tryApplyReplayDepthGauge(1.0, 1.0, 1.0, {1, 0}).has_value());
  EXPECT_FALSE(tryApplyReplayDepthGauge(std::numeric_limits<double>::infinity(),
                                        1.0, 1.0, {1, 1})
                   .has_value());
  EXPECT_FALSE(
      tryApplyReplayDepthGauge(1.0, 1.0, -1.0, {1, 1}).has_value());
}
