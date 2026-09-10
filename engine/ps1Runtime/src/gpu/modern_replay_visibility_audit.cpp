#include "runtime/gpu/modern_replay_visibility_audit.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace ps1::gpu::replay {
namespace {

template <typename T>
bool validPlane(const ReplayVisibilityAuditPlaneView &plane, uint32_t width,
                uint32_t height) {
  if (plane.data == nullptr || plane.rowPitchBytes < sizeof(T) * width)
    return false;
  if (height == 0)
    return true;
  const std::size_t lastRow =
      static_cast<std::size_t>(height - 1u) * plane.rowPitchBytes;
  const std::size_t lastRowBytes = static_cast<std::size_t>(width) * sizeof(T);
  return lastRow <= plane.sizeBytes &&
         lastRowBytes <= plane.sizeBytes - lastRow;
}

template <typename T>
T readPlane(const ReplayVisibilityAuditPlaneView &plane, uint32_t x,
            uint32_t y) {
  T value{};
  const auto *bytes = static_cast<const uint8_t *>(plane.data);
  std::memcpy(&value,
              bytes + static_cast<std::size_t>(y) * plane.rowPitchBytes +
                  static_cast<std::size_t>(x) * sizeof(T),
              sizeof(T));
  return value;
}

bool validPhysicalDepth(float depth) {
  return std::isfinite(depth) && depth > 0.0f && depth < 1.0f;
}

void recordSample(ReplayVisibilityAuditReport &report,
                  ReplayVisibilityAuditOptions options, uint32_t x, uint32_t y,
                  uint32_t otOwner, uint32_t depthOwner, float otDepth,
                  float strictDepth, uint8_t evidence,
                  ReplayVisibilityAuditSampleClass classification) {
  if (report.samples.size() >= options.maximumRecordedSamples)
    return;
  report.samples.push_back(
      {report.region.atlasX + x, report.region.atlasY + y, otOwner, depthOwner,
       std::bit_cast<uint32_t>(otDepth), std::bit_cast<uint32_t>(strictDepth),
       evidence, classification});
}

ReplayVisibilityAuditConflict &
findOrCreateConflict(ReplayVisibilityAuditReport &report, uint32_t otOwner,
                     uint32_t depthOwner, uint32_t atlasX, uint32_t atlasY) {
  const auto found = std::find_if(
      report.conflicts.begin(), report.conflicts.end(),
      [otOwner, depthOwner](const ReplayVisibilityAuditConflict &conflict) {
        return conflict.otOwner == otOwner && conflict.depthOwner == depthOwner;
      });
  if (found != report.conflicts.end())
    return *found;

  ReplayVisibilityAuditConflict conflict;
  conflict.otOwner = otOwner;
  conflict.depthOwner = depthOwner;
  conflict.firstAtlasX = atlasX;
  conflict.firstAtlasY = atlasY;
  conflict.minAtlasX = atlasX;
  conflict.minAtlasY = atlasY;
  conflict.maxAtlasX = atlasX;
  conflict.maxAtlasY = atlasY;
  report.conflicts.push_back(conflict);
  return report.conflicts.back();
}

void recordCandidateSample(
    ReplayVisibilityCandidateAuditReport &report,
    ReplayVisibilityAuditOptions options, uint32_t x, uint32_t y,
    uint32_t otOwner, uint32_t strictOwner, uint32_t liftedOwner, float otDepth,
    float strictDepth, float liftedDepth, uint8_t evidence,
    ReplayVisibilityCandidateSampleClass classification) {
  if (report.samples.size() >= options.maximumRecordedSamples)
    return;
  report.samples.push_back(
      {report.region.atlasX + x, report.region.atlasY + y, otOwner, strictOwner,
       liftedOwner, std::bit_cast<uint32_t>(otDepth),
       std::bit_cast<uint32_t>(strictDepth),
       std::bit_cast<uint32_t>(liftedDepth), evidence, classification});
}

ReplayVisibilityCandidateConflict &
findOrCreateCandidateConflict(ReplayVisibilityCandidateAuditReport &report,
                              uint32_t otOwner, uint32_t strictOwner) {
  const auto found = std::find_if(
      report.conflicts.begin(), report.conflicts.end(),
      [otOwner, strictOwner](const ReplayVisibilityCandidateConflict &value) {
        return value.otOwner == otOwner && value.strictOwner == strictOwner;
      });
  if (found != report.conflicts.end())
    return *found;
  report.conflicts.push_back({otOwner, strictOwner});
  return report.conflicts.back();
}

void recordCandidateOutcome(
    ReplayVisibilityCandidateAuditReport &report,
    ReplayVisibilityCandidateSampleClass classification, uint32_t x,
    uint32_t y, uint32_t otOwner, uint32_t strictOwner, uint32_t liftedOwner,
    float otDepth, float strictDepth, float liftedDepth) {
  const auto found = std::find_if(
      report.outcomes.begin(), report.outcomes.end(),
      [classification, otOwner, strictOwner, liftedOwner](
          const ReplayVisibilityCandidateOutcome &value) {
        return value.classification == classification &&
               value.otOwner == otOwner && value.strictOwner == strictOwner &&
               value.liftedOwner == liftedOwner;
      });
  ReplayVisibilityCandidateOutcome *outcome = nullptr;
  if (found == report.outcomes.end()) {
    ReplayVisibilityCandidateOutcome value;
    value.classification = classification;
    value.otOwner = otOwner;
    value.strictOwner = strictOwner;
    value.liftedOwner = liftedOwner;
    value.firstAtlasX = report.region.atlasX + x;
    value.firstAtlasY = report.region.atlasY + y;
    report.outcomes.push_back(value);
    outcome = &report.outcomes.back();
  } else {
    outcome = &*found;
  }
  ++outcome->samples;
  if (std::isfinite(otDepth)) {
    outcome->minimumOtDepth = std::min(outcome->minimumOtDepth, otDepth);
    outcome->maximumOtDepth = std::max(outcome->maximumOtDepth, otDepth);
  }
  if (std::isfinite(strictDepth)) {
    outcome->minimumStrictDepth =
        std::min(outcome->minimumStrictDepth, strictDepth);
    outcome->maximumStrictDepth =
        std::max(outcome->maximumStrictDepth, strictDepth);
  }
  if (std::isfinite(liftedDepth)) {
    outcome->minimumLiftedDepth =
        std::min(outcome->minimumLiftedDepth, liftedDepth);
    outcome->maximumLiftedDepth =
        std::max(outcome->maximumLiftedDepth, liftedDepth);
  }
}

} // namespace

const char *
replayVisibilityAuditStatusName(ReplayVisibilityAuditStatus status) {
  switch (status) {
  case ReplayVisibilityAuditStatus::Compared:
    return "compared";
  case ReplayVisibilityAuditStatus::EmptyRegion:
    return "empty-region";
  case ReplayVisibilityAuditStatus::InvalidPlane:
    return "invalid-plane";
  case ReplayVisibilityAuditStatus::InvalidOwnerToken:
    return "invalid-owner-token";
  }
  return "unknown";
}

ReplayVisibilityAuditReport
auditReplayVisibility(const ReplayVisibilityAuditRegion &region,
                      const ReplayVisibilityAuditPlanes &planes,
                      const std::vector<ReplayVisibilityAuditOwner> &owners,
                      ReplayVisibilityAuditOptions options) {
  ReplayVisibilityAuditReport report;
  report.region = region;
  if (region.width == 0 || region.height == 0) {
    report.status = ReplayVisibilityAuditStatus::EmptyRegion;
    return report;
  }
  if (!validPlane<uint32_t>(planes.otOwnerR32, region.width, region.height) ||
      !validPlane<uint32_t>(planes.depthOwnerR32, region.width,
                            region.height) ||
      !validPlane<float>(planes.otDepthD32, region.width, region.height) ||
      !validPlane<float>(planes.strictDepthD32, region.width, region.height) ||
      !validPlane<uint8_t>(planes.evidenceR8, region.width, region.height)) {
    report.status = ReplayVisibilityAuditStatus::InvalidPlane;
    return report;
  }

  report.status = ReplayVisibilityAuditStatus::Compared;
  report.totalSamples = static_cast<uint64_t>(region.width) * region.height;
  for (uint32_t y = 0; y < region.height; ++y) {
    for (uint32_t x = 0; x < region.width; ++x) {
      const uint32_t otOwner = readPlane<uint32_t>(planes.otOwnerR32, x, y);
      const uint32_t depthOwner =
          readPlane<uint32_t>(planes.depthOwnerR32, x, y);
      const float otDepth = readPlane<float>(planes.otDepthD32, x, y);
      const float strictDepth = readPlane<float>(planes.strictDepthD32, x, y);
      const uint8_t evidence = readPlane<uint8_t>(planes.evidenceR8, x, y);

      if (otOwner != 0)
        ++report.otOwnedSamples;
      if (depthOwner != 0)
        ++report.depthOwnedSamples;

      if (otOwner > owners.size() || depthOwner > owners.size()) {
        report.status = ReplayVisibilityAuditStatus::InvalidOwnerToken;
        report.conflicts.clear();
        report.samples.clear();
        return report;
      }

      const uint8_t missingEvidence = static_cast<uint8_t>(
          REPLAY_VISIBILITY_AUDIT_REQUIRED_EVIDENCE & ~evidence);
      if (missingEvidence != 0) {
        ++report.indeterminateSamples;
        if ((missingEvidence & ReplayVisibilityAuditOtOwnerExact) != 0)
          ++report.missingOtOwnerEvidenceSamples;
        if ((missingEvidence & ReplayVisibilityAuditDepthOwnerExact) != 0)
          ++report.missingDepthOwnerEvidenceSamples;
        if ((missingEvidence & ReplayVisibilityAuditAcceptanceMatched) != 0)
          ++report.unmatchedAcceptanceSamples;
        if ((missingEvidence & ReplayVisibilityAuditBarrierFree) != 0)
          ++report.barrierContaminatedSamples;
        recordSample(report, options, x, y, otOwner, depthOwner, otDepth,
                     strictDepth, evidence,
                     ReplayVisibilityAuditSampleClass::Indeterminate);
        continue;
      }

      const bool otEligible = otOwner == 0 || owners[otOwner - 1u].comparable();
      const bool depthEligible =
          depthOwner == 0 || owners[depthOwner - 1u].comparable();
      if (!otEligible || !depthEligible) {
        ++report.indeterminateSamples;
        ++report.ineligibleOwnerSamples;
        recordSample(report, options, x, y, otOwner, depthOwner, otDepth,
                     strictDepth, evidence,
                     ReplayVisibilityAuditSampleClass::Indeterminate);
        continue;
      }

      if (otOwner == depthOwner) {
        ++report.agreementSamples;
        if (otOwner == 0)
          ++report.backgroundAgreementSamples;
        else
          ++report.ownedAgreementSamples;
        continue;
      }
      if (otOwner == 0 || depthOwner == 0) {
        ++report.coverageDivergenceSamples;
        recordSample(report, options, x, y, otOwner, depthOwner, otDepth,
                     strictDepth, evidence,
                     ReplayVisibilityAuditSampleClass::CoverageDivergence);
        continue;
      }

      const ReplayVisibilityAuditOwner &ot = owners[otOwner - 1u];
      const ReplayVisibilityAuditOwner &depth = owners[depthOwner - 1u];
      if (!validPhysicalDepth(otDepth) || !validPhysicalDepth(strictDepth)) {
        ++report.inconsistentEvidenceSamples;
        ++report.invalidDepthSamples;
        recordSample(report, options, x, y, otOwner, depthOwner, otDepth,
                     strictDepth, evidence,
                     ReplayVisibilityAuditSampleClass::InconsistentEvidence);
        continue;
      }
      if (ot.sourceOrdinal <= depth.sourceOrdinal) {
        ++report.inconsistentEvidenceSamples;
        ++report.ownerOrderContradictionSamples;
        recordSample(report, options, x, y, otOwner, depthOwner, otDepth,
                     strictDepth, evidence,
                     ReplayVisibilityAuditSampleClass::InconsistentEvidence);
        continue;
      }
      if (!(strictDepth < otDepth)) {
        ++report.inconsistentEvidenceSamples;
        ++report.depthRelationContradictionSamples;
        recordSample(report, options, x, y, otOwner, depthOwner, otDepth,
                     strictDepth, evidence,
                     ReplayVisibilityAuditSampleClass::InconsistentEvidence);
        continue;
      }

      ++report.confirmedConflictSamples;
      const uint32_t atlasX = region.atlasX + x;
      const uint32_t atlasY = region.atlasY + y;
      ReplayVisibilityAuditConflict &conflict =
          findOrCreateConflict(report, otOwner, depthOwner, atlasX, atlasY);
      ++conflict.samples;
      conflict.minAtlasX = std::min(conflict.minAtlasX, atlasX);
      conflict.minAtlasY = std::min(conflict.minAtlasY, atlasY);
      conflict.maxAtlasX = std::max(conflict.maxAtlasX, atlasX);
      conflict.maxAtlasY = std::max(conflict.maxAtlasY, atlasY);
      conflict.minimumOtDepth = std::min(conflict.minimumOtDepth, otDepth);
      conflict.maximumOtDepth = std::max(conflict.maximumOtDepth, otDepth);
      conflict.minimumStrictDepth =
          std::min(conflict.minimumStrictDepth, strictDepth);
      conflict.maximumStrictDepth =
          std::max(conflict.maximumStrictDepth, strictDepth);
      recordSample(
          report, options, x, y, otOwner, depthOwner, otDepth, strictDepth,
          evidence,
          ReplayVisibilityAuditSampleClass::ConfirmedOrderDepthConflict);
    }
  }
  return report;
}

ReplayVisibilityCandidateAuditReport auditReplayVisibilityCandidate(
    const ReplayVisibilityAuditRegion &region,
    const ReplayVisibilityCandidateAuditPlanes &planes,
    const std::vector<ReplayVisibilityAuditOwner> &owners,
    ReplayVisibilityAuditOptions options) {
  ReplayVisibilityCandidateAuditReport report;
  report.region = region;
  if (region.width == 0 || region.height == 0) {
    report.status = ReplayVisibilityAuditStatus::EmptyRegion;
    return report;
  }
  if (!validPlane<uint32_t>(planes.otOwnerR32, region.width, region.height) ||
      !validPlane<uint32_t>(planes.strictOwnerR32, region.width,
                            region.height) ||
      !validPlane<uint32_t>(planes.liftedOwnerR32, region.width,
                            region.height) ||
      !validPlane<float>(planes.otDepthD32, region.width, region.height) ||
      !validPlane<float>(planes.strictDepthD32, region.width, region.height) ||
      !validPlane<float>(planes.liftedDepthD32, region.width, region.height) ||
      !validPlane<uint8_t>(planes.evidenceR8, region.width, region.height)) {
    report.status = ReplayVisibilityAuditStatus::InvalidPlane;
    return report;
  }

  report.status = ReplayVisibilityAuditStatus::Compared;
  report.totalSamples = static_cast<uint64_t>(region.width) * region.height;
  for (uint32_t y = 0; y < region.height; ++y) {
    for (uint32_t x = 0; x < region.width; ++x) {
      const uint32_t otOwner = readPlane<uint32_t>(planes.otOwnerR32, x, y);
      const uint32_t strictOwner =
          readPlane<uint32_t>(planes.strictOwnerR32, x, y);
      const uint32_t liftedOwner =
          readPlane<uint32_t>(planes.liftedOwnerR32, x, y);
      const float otDepth = readPlane<float>(planes.otDepthD32, x, y);
      const float strictDepth = readPlane<float>(planes.strictDepthD32, x, y);
      const float liftedDepth = readPlane<float>(planes.liftedDepthD32, x, y);
      const uint8_t evidence = readPlane<uint8_t>(planes.evidenceR8, x, y);

      if (otOwner > owners.size() || strictOwner > owners.size() ||
          liftedOwner > owners.size()) {
        report.status = ReplayVisibilityAuditStatus::InvalidOwnerToken;
        report.conflicts.clear();
        report.outcomes.clear();
        report.samples.clear();
        return report;
      }

      const uint8_t missingEvidence = static_cast<uint8_t>(
          REPLAY_VISIBILITY_CANDIDATE_REQUIRED_EVIDENCE & ~evidence);
      if (missingEvidence != 0) {
        ++report.indeterminateSamples;
        if ((missingEvidence & ReplayVisibilityAuditOtOwnerExact) != 0)
          ++report.missingOtOwnerEvidenceSamples;
        if ((missingEvidence & ReplayVisibilityAuditDepthOwnerExact) != 0)
          ++report.missingStrictOwnerEvidenceSamples;
        if ((missingEvidence & ReplayVisibilityAuditLiftedOwnerExact) != 0)
          ++report.missingLiftedOwnerEvidenceSamples;
        if ((missingEvidence & ReplayVisibilityAuditAcceptanceMatched) != 0)
          ++report.unmatchedAcceptanceSamples;
        if ((missingEvidence & ReplayVisibilityAuditBarrierFree) != 0)
          ++report.barrierContaminatedSamples;
        recordCandidateSample(
            report, options, x, y, otOwner, strictOwner, liftedOwner, otDepth,
            strictDepth, liftedDepth, evidence,
            ReplayVisibilityCandidateSampleClass::Indeterminate);
        recordCandidateOutcome(
            report, ReplayVisibilityCandidateSampleClass::Indeterminate, x, y,
            otOwner, strictOwner, liftedOwner, otDepth, strictDepth,
            liftedDepth);
        continue;
      }

      const auto comparable = [&owners](uint32_t token) {
        return token == 0 || owners[token - 1u].comparable();
      };
      if (!comparable(otOwner) || !comparable(strictOwner) ||
          !comparable(liftedOwner)) {
        ++report.indeterminateSamples;
        ++report.ineligibleOwnerSamples;
        recordCandidateSample(
            report, options, x, y, otOwner, strictOwner, liftedOwner, otDepth,
            strictDepth, liftedDepth, evidence,
            ReplayVisibilityCandidateSampleClass::Indeterminate);
        recordCandidateOutcome(
            report, ReplayVisibilityCandidateSampleClass::Indeterminate, x, y,
            otOwner, strictOwner, liftedOwner, otDepth, strictDepth,
            liftedDepth);
        continue;
      }

      const bool otCovered = otOwner != 0;
      const bool strictCovered = strictOwner != 0;
      const bool liftedCovered = liftedOwner != 0;
      if (otCovered != strictCovered) {
        ++report.baselineCoverageDivergenceSamples;
        recordCandidateSample(
            report, options, x, y, otOwner, strictOwner, liftedOwner, otDepth,
            strictDepth, liftedDepth, evidence,
            ReplayVisibilityCandidateSampleClass::BaselineCoverageDivergence);
        recordCandidateOutcome(
            report,
            ReplayVisibilityCandidateSampleClass::BaselineCoverageDivergence,
            x, y, otOwner, strictOwner, liftedOwner, otDepth, strictDepth,
            liftedDepth);
        continue;
      }
      if (otCovered != liftedCovered) {
        ++report.candidateCoverageDivergenceSamples;
        recordCandidateSample(
            report, options, x, y, otOwner, strictOwner, liftedOwner, otDepth,
            strictDepth, liftedDepth, evidence,
            ReplayVisibilityCandidateSampleClass::CandidateCoverageDivergence);
        recordCandidateOutcome(
            report,
            ReplayVisibilityCandidateSampleClass::CandidateCoverageDivergence,
            x, y, otOwner, strictOwner, liftedOwner, otDepth, strictDepth,
            liftedDepth);
        continue;
      }

      const bool invalidDepth =
          (otCovered && !validPhysicalDepth(otDepth)) ||
          (strictCovered && !validPhysicalDepth(strictDepth)) ||
          (liftedCovered && !validPhysicalDepth(liftedDepth));
      if (invalidDepth) {
        ++report.inconsistentEvidenceSamples;
        ++report.invalidDepthSamples;
        recordCandidateSample(
            report, options, x, y, otOwner, strictOwner, liftedOwner, otDepth,
            strictDepth, liftedDepth, evidence,
            ReplayVisibilityCandidateSampleClass::InconsistentEvidence);
        recordCandidateOutcome(
            report,
            ReplayVisibilityCandidateSampleClass::InconsistentEvidence, x, y,
            otOwner, strictOwner, liftedOwner, otDepth, strictDepth,
            liftedDepth);
        continue;
      }

      if (otOwner == strictOwner) {
        ++report.baselineAgreementSamples;
        if (liftedOwner == otOwner) {
          ++report.agreementPreservedSamples;
        } else {
          ++report.candidateRegressionSamples;
          recordCandidateSample(
              report, options, x, y, otOwner, strictOwner, liftedOwner, otDepth,
              strictDepth, liftedDepth, evidence,
              ReplayVisibilityCandidateSampleClass::CandidateRegression);
          recordCandidateOutcome(
              report, ReplayVisibilityCandidateSampleClass::CandidateRegression,
              x, y, otOwner, strictOwner, liftedOwner, otDepth, strictDepth,
              liftedDepth);
        }
        continue;
      }

      const ReplayVisibilityAuditOwner &ot = owners[otOwner - 1u];
      const ReplayVisibilityAuditOwner &strict = owners[strictOwner - 1u];
      if (ot.sourceOrdinal <= strict.sourceOrdinal) {
        ++report.inconsistentEvidenceSamples;
        ++report.ownerOrderContradictionSamples;
        recordCandidateSample(
            report, options, x, y, otOwner, strictOwner, liftedOwner, otDepth,
            strictDepth, liftedDepth, evidence,
            ReplayVisibilityCandidateSampleClass::InconsistentEvidence);
        recordCandidateOutcome(
            report,
            ReplayVisibilityCandidateSampleClass::InconsistentEvidence, x, y,
            otOwner, strictOwner, liftedOwner, otDepth, strictDepth,
            liftedDepth);
        continue;
      }
      if (!(strictDepth < otDepth)) {
        ++report.inconsistentEvidenceSamples;
        ++report.depthRelationContradictionSamples;
        recordCandidateSample(
            report, options, x, y, otOwner, strictOwner, liftedOwner, otDepth,
            strictDepth, liftedDepth, evidence,
            ReplayVisibilityCandidateSampleClass::InconsistentEvidence);
        recordCandidateOutcome(
            report,
            ReplayVisibilityCandidateSampleClass::InconsistentEvidence, x, y,
            otOwner, strictOwner, liftedOwner, otDepth, strictDepth,
            liftedDepth);
        continue;
      }

      ++report.baselineConflictSamples;
      ReplayVisibilityCandidateConflict &conflict =
          findOrCreateCandidateConflict(report, otOwner, strictOwner);
      ++conflict.samples;
      if (liftedOwner == otOwner) {
        ++report.conflictResolvedToOtSamples;
        ++conflict.resolvedToOtSamples;
        recordCandidateSample(
            report, options, x, y, otOwner, strictOwner, liftedOwner, otDepth,
            strictDepth, liftedDepth, evidence,
            ReplayVisibilityCandidateSampleClass::ConflictResolvedToOt);
      } else if (liftedOwner == strictOwner) {
        ++report.conflictRetainedByStrictSamples;
        ++conflict.retainedByStrictSamples;
        recordCandidateSample(
            report, options, x, y, otOwner, strictOwner, liftedOwner, otDepth,
            strictDepth, liftedDepth, evidence,
            ReplayVisibilityCandidateSampleClass::ConflictRetainedByStrict);
        recordCandidateOutcome(
            report,
            ReplayVisibilityCandidateSampleClass::ConflictRetainedByStrict, x,
            y, otOwner, strictOwner, liftedOwner, otDepth, strictDepth,
            liftedDepth);
      } else {
        ++report.conflictChangedWinnerSamples;
        ++conflict.changedWinnerSamples;
        recordCandidateSample(
            report, options, x, y, otOwner, strictOwner, liftedOwner, otDepth,
            strictDepth, liftedDepth, evidence,
            ReplayVisibilityCandidateSampleClass::ConflictChangedWinner);
        recordCandidateOutcome(
            report,
            ReplayVisibilityCandidateSampleClass::ConflictChangedWinner, x, y,
            otOwner, strictOwner, liftedOwner, otDepth, strictDepth,
            liftedDepth);
      }
    }
  }
  return report;
}

} // namespace ps1::gpu::replay
