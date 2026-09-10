#include "runtime/gpu/modern_replay_depth_family.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ps1::gpu::replay {
namespace {

bool exactGaugeProof(ReplayDepthFamilyGaugeProof proof) {
  return proof == ReplayDepthFamilyGaugeProof::GuestTransformDataflow ||
         proof == ReplayDepthFamilyGaugeProof::ExactAffineGaugeRatio;
}

template <typename T>
void appendUnique(std::vector<T> &values, const T &value) {
  if (std::find(values.begin(), values.end(), value) == values.end())
    values.push_back(value);
}

ReplayDepthFamilyLineage *findLineage(ReplayDepthFamilyReport &report,
                                      uint64_t lineage) {
  const auto found =
      std::find_if(report.lineages.begin(), report.lineages.end(),
                   [lineage](const ReplayDepthFamilyLineage &candidate) {
                     return candidate.gaugeLineage == lineage;
                   });
  return found == report.lineages.end() ? nullptr : &*found;
}

ReplayDepthFamily *findFamily(ReplayDepthFamilyReport &report,
                              ReplayDepthGauge gauge) {
  const auto found =
      std::find_if(report.families.begin(), report.families.end(),
                   [gauge](const ReplayDepthFamily &candidate) {
                     return candidate.sourceGauge == gauge;
                   });
  return found == report.families.end() ? nullptr : &*found;
}

struct ObjectAssignment {
  ReplayDepthFamilyObjectKey object{};
  uint64_t gaugeLineage = 0;
  ReplayDepthGauge sourceGauge{};
};

ReplayDepthGauge reduceGauge(ReplayDepthGauge gauge) {
  if (!gauge.positive())
    return {};
  const uint64_t divisor = std::gcd(gauge.numerator, gauge.denominator);
  gauge.numerator /= divisor;
  gauge.denominator /= divisor;
  return gauge;
}

bool gaugeLess(ReplayDepthGauge left, ReplayDepthGauge right) {
  // Compare positive fractions exactly without overflowing uint64_t and
  // without relying on compiler-specific 128-bit integers. Each reciprocal
  // step reverses the ordering of the remaining fractional parts.
  bool reversed = false;
  for (;;) {
    const uint64_t leftQuotient = left.numerator / left.denominator;
    const uint64_t rightQuotient = right.numerator / right.denominator;
    if (leftQuotient != rightQuotient)
      return reversed ? leftQuotient > rightQuotient
                      : leftQuotient < rightQuotient;

    const uint64_t leftRemainder = left.numerator % left.denominator;
    const uint64_t rightRemainder = right.numerator % right.denominator;
    if (leftRemainder == 0 || rightRemainder == 0) {
      if (leftRemainder == rightRemainder)
        return false;
      const bool currentLess = leftRemainder == 0;
      return reversed ? !currentLess : currentLess;
    }

    left = {left.denominator, leftRemainder};
    right = {right.denominator, rightRemainder};
    reversed = !reversed;
  }
}

bool expressibleInTenths(ReplayDepthGauge gauge) {
  gauge = reduceGauge(gauge);
  return gauge.positive() && 10u % gauge.denominator == 0;
}

} // namespace

const char *replayDepthFamilyStatusName(ReplayDepthFamilyStatus status) {
  switch (status) {
  case ReplayDepthFamilyStatus::Certified:
    return "certified";
  case ReplayDepthFamilyStatus::Empty:
    return "empty";
  case ReplayDepthFamilyStatus::InvalidObservation:
    return "invalid-observation";
  case ReplayDepthFamilyStatus::ContradictoryObject:
    return "contradictory-object";
  case ReplayDepthFamilyStatus::ContradictoryGaugeLineage:
    return "contradictory-gauge-lineage";
  case ReplayDepthFamilyStatus::MissingIdentityFamily:
    return "missing-identity-family";
  case ReplayDepthFamilyStatus::MissingIndependentHoldout:
    return "missing-independent-holdout";
  }
  return "unknown";
}

ReplayDepthFamilyReport certifyReplayDepthFamilies(
    const std::vector<ReplayDepthFamilyObservation> &observations,
    ReplayDepthFamilyOptions options) {
  ReplayDepthFamilyReport report;
  report.prior.likelyMaximumFamilyCount = options.likelyMaximumFamilyCount;
  if (observations.empty())
    return report;

  std::vector<ObjectAssignment> assignments;
  for (const ReplayDepthFamilyObservation &observation : observations) {
    const ReplayDepthGauge sourceGauge = reduceGauge(observation.sourceGauge);
    if (!observation.object.complete() || observation.gaugeLineage == 0 ||
        !sourceGauge.positive() || !exactGaugeProof(observation.proof)) {
      report.status = ReplayDepthFamilyStatus::InvalidObservation;
      return report;
    }

    const auto object =
        std::find_if(assignments.begin(), assignments.end(),
                     [&observation](const ObjectAssignment &candidate) {
                       return candidate.object == observation.object;
                     });
    if (object == assignments.end()) {
      assignments.push_back({observation.object, observation.gaugeLineage,
                             sourceGauge});
    } else if (object->gaugeLineage != observation.gaugeLineage ||
               !(object->sourceGauge == sourceGauge)) {
      report.status = ReplayDepthFamilyStatus::ContradictoryObject;
      return report;
    }

    ReplayDepthFamilyLineage *lineage =
        findLineage(report, observation.gaugeLineage);
    if (lineage == nullptr) {
      report.lineages.push_back(
          {observation.gaugeLineage, sourceGauge, {}, {}});
      lineage = &report.lineages.back();
    } else if (!(lineage->sourceGauge == sourceGauge)) {
      report.status = ReplayDepthFamilyStatus::ContradictoryGaugeLineage;
      return report;
    }

    std::vector<uint64_t> &frames =
        observation.validationSet == ReplayDepthFamilyValidationSet::Discovery
            ? lineage->discoveryFrames
            : lineage->holdoutFrames;
    appendUnique(frames, observation.frameIndex);

    ReplayDepthFamily *family =
        findFamily(report, sourceGauge);
    if (family == nullptr) {
      report.families.push_back({sourceGauge, {}, {}});
      family = &report.families.back();
    }
    appendUnique(family->gaugeLineages, observation.gaugeLineage);
    appendUnique(family->objects, observation.object);
  }

  if (options.requireIdentityFamily && findFamily(report, {1, 1}) == nullptr) {
    report.status = ReplayDepthFamilyStatus::MissingIdentityFamily;
    return report;
  }

  if (options.requireIndependentHoldout) {
    for (const ReplayDepthFamilyLineage &lineage : report.lineages) {
      if (lineage.discoveryFrames.empty() || lineage.holdoutFrames.empty()) {
        report.status = ReplayDepthFamilyStatus::MissingIndependentHoldout;
        return report;
      }
      for (uint64_t frame : lineage.discoveryFrames) {
        if (std::find(lineage.holdoutFrames.begin(),
                      lineage.holdoutFrames.end(),
                      frame) != lineage.holdoutFrames.end()) {
          report.status = ReplayDepthFamilyStatus::MissingIndependentHoldout;
          return report;
        }
      }
    }
  }

  std::sort(report.lineages.begin(), report.lineages.end(),
            [](const ReplayDepthFamilyLineage &left,
               const ReplayDepthFamilyLineage &right) {
              return left.gaugeLineage < right.gaugeLineage;
            });
  std::sort(report.families.begin(), report.families.end(),
            [](const ReplayDepthFamily &left, const ReplayDepthFamily &right) {
              return gaugeLess(left.sourceGauge, right.sourceGauge);
            });
  report.prior.exceedsLikelyFamilyCount =
      report.prior.likelyMaximumFamilyCount != 0 &&
      report.families.size() > report.prior.likelyMaximumFamilyCount;
  report.prior.allGaugesExpressibleInTenths =
      std::all_of(report.families.begin(), report.families.end(),
                  [](const ReplayDepthFamily &family) {
                    return expressibleInTenths(family.sourceGauge);
                  });
  report.status = ReplayDepthFamilyStatus::Certified;
  return report;
}

std::optional<ReplayHomogeneousPresentationPoint>
tryApplyReplayDepthGauge(double qx, double qy, double w,
                         ReplayDepthGauge sourceGauge) {
  sourceGauge = reduceGauge(sourceGauge);
  if (!sourceGauge.positive() || !std::isfinite(qx) || !std::isfinite(qy) ||
      !std::isfinite(w) || w <= 0.0)
    return std::nullopt;

  const double multiplier = static_cast<double>(sourceGauge.denominator) /
                            static_cast<double>(sourceGauge.numerator);
  ReplayHomogeneousPresentationPoint result{qx * multiplier, qy * multiplier,
                                            w * multiplier};
  if (!std::isfinite(result.qx) || !std::isfinite(result.qy) ||
      !std::isfinite(result.w) || result.w <= 0.0)
    return std::nullopt;
  return result;
}

} // namespace ps1::gpu::replay
