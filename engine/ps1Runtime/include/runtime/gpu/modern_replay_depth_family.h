#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ps1::gpu::replay {

// Current Crash observations suggest a small catalog, often no more than four
// gauges. This is a diagnostic prior only: an independently proved fifth
// family remains valid and must never be merged or rejected to satisfy it.
inline constexpr std::size_t REPLAY_LIKELY_PRESENTATION_DEPTH_FAMILIES = 4;

enum ReplayDepthFamilyObjectEvidence : uint8_t {
  ReplayDepthFamilyTransformLineage = 1u << 0,
  ReplayDepthFamilyGeometryLineage = 1u << 1,
  ReplayDepthFamilySubmissionLineage = 1u << 2,
  ReplayDepthFamilyTemporalIdentity = 1u << 3,
};

inline constexpr uint8_t REPLAY_DEPTH_FAMILY_REQUIRED_OBJECT_EVIDENCE =
    ReplayDepthFamilyTransformLineage | ReplayDepthFamilyGeometryLineage |
    ReplayDepthFamilySubmissionLineage | ReplayDepthFamilyTemporalIdentity;

// All fields are categorical provenance. Screen position, bounding boxes,
// colour and observed depth are deliberately absent: they may locate a
// candidate, but cannot establish object identity.
struct ReplayDepthFamilyObjectKey {
  uint64_t transformLineage = 0;
  uint64_t geometryLineage = 0;
  uint64_t submissionLineage = 0;
  uint64_t temporalIdentity = 0;
  uint8_t evidence = 0;

  bool complete() const {
    return (evidence & REPLAY_DEPTH_FAMILY_REQUIRED_OBJECT_EVIDENCE) ==
           REPLAY_DEPTH_FAMILY_REQUIRED_OBJECT_EVIDENCE;
  }

  bool operator==(const ReplayDepthFamilyObjectKey &other) const {
    // Evidence certifies the fields; it is not itself part of object identity.
    return transformLineage == other.transformLineage &&
           geometryLineage == other.geometryLineage &&
           submissionLineage == other.submissionLineage &&
           temporalIdentity == other.temporalIdentity;
  }
};

// A factor is admitted only when it came from intrinsic guest state. In
// particular, screenshot fitting, motion clustering and visibility conflicts
// are not proof kinds and therefore cannot enter this API.
enum class ReplayDepthFamilyGaugeProof : uint8_t {
  None = 0,
  // Exact dataflow from a guest multiplier applied to the complete affine
  // camera transform (basis and translation), with its rounding semantics.
  GuestTransformDataflow,
  // Division-free comparison of complete fixed-point affine transforms proves
  // that they differ only by the stated common projective gauge.
  ExactAffineGaugeRatio,
};

enum class ReplayDepthFamilyValidationSet : uint8_t {
  Discovery = 0,
  Holdout,
};

// Exact positive projective gauge. Keeping a reduced rational avoids baking
// the current "integer or one decimal" observation into the correctness
// boundary; fixed-point guest code can legitimately prove values such as 3/4.
struct ReplayDepthGauge {
  uint64_t numerator = 0;
  uint64_t denominator = 0;

  bool positive() const { return numerator != 0 && denominator != 0; }
  bool identity() const { return positive() && numerator == denominator; }
  bool operator==(const ReplayDepthGauge &other) const {
    return numerator == other.numerator && denominator == other.denominator;
  }
};

struct ReplayDepthFamilyObservation {
  ReplayDepthFamilyObjectKey object{};
  // Stable lineage of the guest operation or affine relation which produced
  // the gauge. Multiple object identities may intentionally share it.
  uint64_t gaugeLineage = 0;
  uint64_t frameIndex = 0;
  // Guest homogeneous camera scale relative to the selected canonical scene.
  // The certifier reduces this exact positive rational before comparing it.
  ReplayDepthGauge sourceGauge{};
  ReplayDepthFamilyGaugeProof proof = ReplayDepthFamilyGaugeProof::None;
  ReplayDepthFamilyValidationSet validationSet =
      ReplayDepthFamilyValidationSet::Discovery;
};

enum class ReplayDepthFamilyStatus : uint8_t {
  Certified = 0,
  Empty,
  InvalidObservation,
  ContradictoryObject,
  ContradictoryGaugeLineage,
  MissingIdentityFamily,
  MissingIndependentHoldout,
};

struct ReplayDepthFamilyLineage {
  uint64_t gaugeLineage = 0;
  ReplayDepthGauge sourceGauge{};
  std::vector<uint64_t> discoveryFrames;
  std::vector<uint64_t> holdoutFrames;
};

struct ReplayDepthFamily {
  ReplayDepthGauge sourceGauge{};
  std::vector<uint64_t> gaugeLineages;
  std::vector<ReplayDepthFamilyObjectKey> objects;
};

struct ReplayDepthFamilyOptions {
  // A global projective scale is presentation-invariant, so relative gauges
  // do not intrinsically require a 1/1 member. A caller may request that
  // convention when an independently identified canonical scene exists.
  bool requireIdentityFamily = false;
  // Every gauge lineage must repeat in a disjoint, caller-designated frame
  // set. This is a fixed validation contract, not a similarity threshold.
  bool requireIndependentHoldout = true;
  // Advisory prior used only in the report. Zero disables the count warning.
  std::size_t likelyMaximumFamilyCount =
      REPLAY_LIKELY_PRESENTATION_DEPTH_FAMILIES;
};

struct ReplayDepthFamilyPriorAssessment {
  std::size_t likelyMaximumFamilyCount = 0;
  bool exceedsLikelyFamilyCount = false;
  bool allGaugesExpressibleInTenths = true;
};

struct ReplayDepthFamilyReport {
  ReplayDepthFamilyStatus status = ReplayDepthFamilyStatus::Empty;
  std::vector<ReplayDepthFamilyLineage> lineages;
  std::vector<ReplayDepthFamily> families;
  ReplayDepthFamilyPriorAssessment prior{};

  bool certified() const {
    return status == ReplayDepthFamilyStatus::Certified;
  }
};

const char *replayDepthFamilyStatusName(ReplayDepthFamilyStatus status);

ReplayDepthFamilyReport certifyReplayDepthFamilies(
    const std::vector<ReplayDepthFamilyObservation> &observations,
    ReplayDepthFamilyOptions options = {});

struct ReplayHomogeneousPresentationPoint {
  double qx = 0.0;
  double qy = 0.0;
  double w = 0.0;
};

// Applies only a certified common projective gauge. Scaling Qx, Qy and W by
// the same exact rational leaves Q/W unchanged while moving the presentation
// point onto the canonical depth scale. Guest geometry is never modified.
std::optional<ReplayHomogeneousPresentationPoint>
tryApplyReplayDepthGauge(double qx, double qy, double w,
                         ReplayDepthGauge sourceGauge);

} // namespace ps1::gpu::replay
