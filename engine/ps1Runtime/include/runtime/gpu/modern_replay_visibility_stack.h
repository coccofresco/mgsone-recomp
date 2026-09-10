#pragma once

#include "runtime/gpu/modern_replay_visibility_audit.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ps1::gpu::replay {

inline constexpr uint32_t REPLAY_VISIBILITY_STACK_SCHEMA = 1;

struct ReplayVisibilityStackFragment {
  uint32_t localSample = 0;
  uint32_t owner = 0;
  uint32_t depthBits = 0;
  uint32_t operation = 0;
};

// A visibility barrier invalidates earlier fragments only inside its atlas
// rectangle. operation uses the same one-based command ordinal as fragments.
struct ReplayVisibilityStackBarrier {
  uint32_t operation = 0;
  ReplayVisibilityAuditRegion region{};
};

struct ReplayVisibilityStackPlanes {
  ReplayVisibilityAuditPlaneView otOwnerR32{};
  ReplayVisibilityAuditPlaneView otDepthD32{};
  ReplayVisibilityAuditPlaneView otEvidenceR8{};
};

enum class ReplayVisibilityStackStatus : uint8_t {
  Compared = 0,
  EmptyRegion,
  InvalidPlane,
  InvalidHeader,
  Overflow,
  InvalidFragment,
  InvalidOwnerToken,
};

struct ReplayVisibilityStackOccluder {
  uint32_t otOwner = 0;
  uint32_t physicalOwner = 0;
  uint64_t samples = 0;
  uint32_t firstAtlasX = 0;
  uint32_t firstAtlasY = 0;
  float minimumOtDepth = 1.0f;
  float maximumOtDepth = 0.0f;
  float minimumPhysicalDepth = 1.0f;
  float maximumPhysicalDepth = 0.0f;
};

struct ReplayVisibilityStackReport {
  ReplayVisibilityStackStatus status =
      ReplayVisibilityStackStatus::InvalidHeader;
  ReplayVisibilityAuditRegion region{};
  uint64_t attemptedRecords = 0;
  uint64_t retainedRecords = 0;
  uint64_t staleRecords = 0;
  uint64_t boundaryRecords = 0;
  uint64_t duplicateOwnerRecords = 0;
  uint64_t inconsistentDuplicateDepthRecords = 0;
  uint64_t otDepthMismatchSamples = 0;
  uint64_t otEvidenceSamples = 0;
  uint64_t completeOwnedSamples = 0;
  uint64_t missingOtFragmentSamples = 0;
  uint64_t invalidOwnerSamples = 0;
  uint64_t invalidDepthSamples = 0;
  uint64_t occludedOtSamples = 0;
  uint32_t maximumDistinctOwnersPerSample = 0;
  std::vector<ReplayVisibilityStackOccluder> occluders;

  bool compared() const noexcept {
    return status == ReplayVisibilityStackStatus::Compared;
  }

  bool complete() const noexcept {
    return compared() && inconsistentDuplicateDepthRecords == 0 &&
           otDepthMismatchSamples == 0 && missingOtFragmentSamples == 0 &&
           invalidOwnerSamples == 0 && invalidDepthSamples == 0;
  }
};

const char *
replayVisibilityStackStatusName(ReplayVisibilityStackStatus status) noexcept;

ReplayVisibilityStackReport auditReplayVisibilityStack(
    const ReplayVisibilityAuditRegion &region,
    const ReplayVisibilityStackPlanes &planes,
    const std::vector<ReplayVisibilityAuditOwner> &owners,
    uint32_t schema, uint64_t attemptedRecords, uint64_t recordCapacity,
    bool overflow, std::span<const ReplayVisibilityStackFragment> fragments,
    std::span<const ReplayVisibilityStackBarrier> barriers);

} // namespace ps1::gpu::replay
