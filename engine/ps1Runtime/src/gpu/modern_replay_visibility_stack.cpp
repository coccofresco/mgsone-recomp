#include "runtime/gpu/modern_replay_visibility_stack.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

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
T readPlane(const ReplayVisibilityAuditPlaneView &plane, uint32_t sample,
            uint32_t width) {
  const uint32_t x = sample % width;
  const uint32_t y = sample / width;
  T result{};
  const auto *bytes = static_cast<const uint8_t *>(plane.data);
  std::memcpy(&result,
              bytes + static_cast<std::size_t>(y) * plane.rowPitchBytes +
                  static_cast<std::size_t>(x) * sizeof(T),
              sizeof(T));
  return result;
}

bool validPhysicalDepth(float value) {
  return std::isfinite(value) && value > 0.0f && value < 1.0f;
}

bool checkedPixels(const ReplayVisibilityAuditRegion &region,
                   std::size_t &pixels) {
  if (region.width == 0 || region.height == 0)
    return false;
  if (region.width >
      std::numeric_limits<std::size_t>::max() / region.height) {
    return false;
  }
  pixels = static_cast<std::size_t>(region.width) * region.height;
  return true;
}

void applyBarrier(std::vector<uint32_t> &lastBarrier,
                  const ReplayVisibilityAuditRegion &capture,
                  const ReplayVisibilityStackBarrier &barrier) {
  const uint64_t captureRight =
      static_cast<uint64_t>(capture.atlasX) + capture.width;
  const uint64_t captureBottom =
      static_cast<uint64_t>(capture.atlasY) + capture.height;
  const uint64_t barrierRight =
      static_cast<uint64_t>(barrier.region.atlasX) + barrier.region.width;
  const uint64_t barrierBottom =
      static_cast<uint64_t>(barrier.region.atlasY) + barrier.region.height;
  const uint64_t left = std::max<uint64_t>(capture.atlasX,
                                           barrier.region.atlasX);
  const uint64_t top = std::max<uint64_t>(capture.atlasY,
                                          barrier.region.atlasY);
  const uint64_t right = std::min(captureRight, barrierRight);
  const uint64_t bottom = std::min(captureBottom, barrierBottom);
  if (left >= right || top >= bottom)
    return;
  for (uint64_t atlasY = top; atlasY < bottom; ++atlasY) {
    const std::size_t row =
        static_cast<std::size_t>(atlasY - capture.atlasY) * capture.width;
    for (uint64_t atlasX = left; atlasX < right; ++atlasX) {
      uint32_t &value =
          lastBarrier[row + static_cast<std::size_t>(atlasX - capture.atlasX)];
      value = std::max(value, barrier.operation);
    }
  }
}

} // namespace

const char *replayVisibilityStackStatusName(
    ReplayVisibilityStackStatus status) noexcept {
  switch (status) {
  case ReplayVisibilityStackStatus::Compared:
    return "compared";
  case ReplayVisibilityStackStatus::EmptyRegion:
    return "empty-region";
  case ReplayVisibilityStackStatus::InvalidPlane:
    return "invalid-plane";
  case ReplayVisibilityStackStatus::InvalidHeader:
    return "invalid-header";
  case ReplayVisibilityStackStatus::Overflow:
    return "overflow";
  case ReplayVisibilityStackStatus::InvalidFragment:
    return "invalid-fragment";
  case ReplayVisibilityStackStatus::InvalidOwnerToken:
    return "invalid-owner-token";
  }
  return "unknown";
}

ReplayVisibilityStackReport auditReplayVisibilityStack(
    const ReplayVisibilityAuditRegion &region,
    const ReplayVisibilityStackPlanes &planes,
    const std::vector<ReplayVisibilityAuditOwner> &owners, uint32_t schema,
    uint64_t attemptedRecords, uint64_t recordCapacity, bool overflow,
    std::span<const ReplayVisibilityStackFragment> fragments,
    std::span<const ReplayVisibilityStackBarrier> barriers) {
  ReplayVisibilityStackReport report;
  report.region = region;
  report.attemptedRecords = attemptedRecords;
  report.retainedRecords = fragments.size();

  std::size_t pixels = 0;
  if (!checkedPixels(region, pixels)) {
    report.status = ReplayVisibilityStackStatus::EmptyRegion;
    return report;
  }
  if (schema != REPLAY_VISIBILITY_STACK_SCHEMA || recordCapacity == 0 ||
      fragments.size() > recordCapacity ||
      (!overflow && attemptedRecords != fragments.size()) ||
      (overflow && attemptedRecords <= recordCapacity)) {
    report.status = ReplayVisibilityStackStatus::InvalidHeader;
    return report;
  }
  if (overflow) {
    report.status = ReplayVisibilityStackStatus::Overflow;
    return report;
  }
  if (!validPlane<uint32_t>(planes.otOwnerR32, region.width, region.height) ||
      !validPlane<float>(planes.otDepthD32, region.width, region.height) ||
      !validPlane<uint8_t>(planes.otEvidenceR8, region.width, region.height)) {
    report.status = ReplayVisibilityStackStatus::InvalidPlane;
    return report;
  }

  std::vector<uint32_t> lastBarrier(pixels, 0);
  for (const ReplayVisibilityStackBarrier &barrier : barriers) {
    if (barrier.operation == 0 || barrier.region.width == 0 ||
        barrier.region.height == 0) {
      report.status = ReplayVisibilityStackStatus::InvalidFragment;
      return report;
    }
    applyBarrier(lastBarrier, region, barrier);
  }

  std::vector<ReplayVisibilityStackFragment> ordered;
  ordered.reserve(fragments.size());
  for (const ReplayVisibilityStackFragment &fragment : fragments) {
    if (fragment.localSample >= pixels || fragment.operation == 0) {
      report.status = ReplayVisibilityStackStatus::InvalidFragment;
      return report;
    }
    if (fragment.owner > owners.size()) {
      report.status = ReplayVisibilityStackStatus::InvalidOwnerToken;
      return report;
    }
    // Token zero is a conservative visibility boundary, not a physical-depth
    // observation. Ineligible replay draws deliberately use far D32, so its
    // depth bits must not be validated as comparable geometry.
    if (fragment.owner == 0) {
      ++report.boundaryRecords;
      lastBarrier[fragment.localSample] =
          std::max(lastBarrier[fragment.localSample], fragment.operation);
      continue;
    }
    const float depth = std::bit_cast<float>(fragment.depthBits);
    if (!validPhysicalDepth(depth)) {
      report.status = ReplayVisibilityStackStatus::InvalidFragment;
      return report;
    }
    if (!owners[fragment.owner - 1u].comparable()) {
      report.status = ReplayVisibilityStackStatus::InvalidOwnerToken;
      return report;
    }
    ordered.push_back(fragment);
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto &left,
                                                const auto &right) {
    if (left.localSample != right.localSample)
      return left.localSample < right.localSample;
    if (left.owner != right.owner)
      return left.owner < right.owner;
    if (left.operation != right.operation)
      return left.operation < right.operation;
    return left.depthBits < right.depthBits;
  });

  struct OwnerDepth {
    uint32_t owner = 0;
    uint32_t depthBits = 0;
  };
  std::unordered_map<uint64_t, std::size_t> occluderIndices;
  std::size_t cursor = 0;
  for (uint32_t sample = 0; sample < pixels; ++sample) {
    const uint32_t otOwner =
        readPlane<uint32_t>(planes.otOwnerR32, sample, region.width);
    const float otDepth =
        readPlane<float>(planes.otDepthD32, sample, region.width);
    const uint8_t evidence =
        readPlane<uint8_t>(planes.otEvidenceR8, sample, region.width);
    constexpr uint8_t required = ReplayVisibilityAuditOtOwnerExact |
                                 ReplayVisibilityAuditAcceptanceMatched |
                                 ReplayVisibilityAuditBarrierFree;
    const bool otEvidenced = (evidence & required) == required;
    if (otEvidenced)
      ++report.otEvidenceSamples;
    if (otOwner > owners.size()) {
      report.status = ReplayVisibilityStackStatus::InvalidOwnerToken;
      return report;
    }

    while (cursor < ordered.size() &&
           ordered[cursor].localSample < sample) {
      ++cursor;
    }
    std::size_t end = cursor;
    while (end < ordered.size() && ordered[end].localSample == sample)
      ++end;

    std::vector<OwnerDepth> distinct;
    distinct.reserve(end - cursor);
    bool inconsistentDepth = false;
    for (std::size_t index = cursor; index < end;) {
      const uint32_t owner = ordered[index].owner;
      bool retained = false;
      uint32_t retainedDepthBits = 0;
      std::size_t next = index;
      while (next < end && ordered[next].owner == owner) {
        if (ordered[next].operation > lastBarrier[sample]) {
          if (retained) {
            ++report.duplicateOwnerRecords;
            if (ordered[next].depthBits != retainedDepthBits) {
              ++report.inconsistentDuplicateDepthRecords;
              inconsistentDepth = true;
            }
          } else {
            retainedDepthBits = ordered[next].depthBits;
          }
          retained = true;
        } else {
          ++report.staleRecords;
        }
        ++next;
      }
      if (retained)
        distinct.push_back({owner, retainedDepthBits});
      index = next;
    }
    cursor = end;

    report.maximumDistinctOwnersPerSample =
        std::max<uint32_t>(report.maximumDistinctOwnersPerSample,
                           static_cast<uint32_t>(distinct.size()));
    if (!otEvidenced || otOwner == 0)
      continue;
    if (!owners[otOwner - 1u].comparable()) {
      ++report.invalidOwnerSamples;
      continue;
    }
    if (!validPhysicalDepth(otDepth) || inconsistentDepth) {
      ++report.invalidDepthSamples;
      continue;
    }
    const auto otFragment = std::find_if(
        distinct.begin(), distinct.end(), [otOwner](const OwnerDepth &value) {
          return value.owner == otOwner;
        });
    if (otFragment == distinct.end()) {
      ++report.missingOtFragmentSamples;
      continue;
    }
    if (otFragment->depthBits != std::bit_cast<uint32_t>(otDepth)) {
      ++report.otDepthMismatchSamples;
      continue;
    }
    ++report.completeOwnedSamples;

    bool occluded = false;
    for (const OwnerDepth &physical : distinct) {
      if (physical.owner == otOwner)
        continue;
      const float physicalDepth = std::bit_cast<float>(physical.depthBits);
      if (!(physicalDepth < otDepth))
        continue;
      occluded = true;
      const uint64_t key = (static_cast<uint64_t>(otOwner) << 32u) |
                           physical.owner;
      auto found = occluderIndices.find(key);
      ReplayVisibilityStackOccluder *occluder = nullptr;
      if (found == occluderIndices.end()) {
        ReplayVisibilityStackOccluder value;
        value.otOwner = otOwner;
        value.physicalOwner = physical.owner;
        value.firstAtlasX = region.atlasX + sample % region.width;
        value.firstAtlasY = region.atlasY + sample / region.width;
        const std::size_t index = report.occluders.size();
        report.occluders.push_back(value);
        occluderIndices.emplace(key, index);
        occluder = &report.occluders.back();
      } else {
        occluder = &report.occluders[found->second];
      }
      ++occluder->samples;
      occluder->minimumOtDepth =
          std::min(occluder->minimumOtDepth, otDepth);
      occluder->maximumOtDepth =
          std::max(occluder->maximumOtDepth, otDepth);
      occluder->minimumPhysicalDepth =
          std::min(occluder->minimumPhysicalDepth, physicalDepth);
      occluder->maximumPhysicalDepth =
          std::max(occluder->maximumPhysicalDepth, physicalDepth);
    }
    if (occluded)
      ++report.occludedOtSamples;
  }

  report.status = ReplayVisibilityStackStatus::Compared;
  std::sort(report.occluders.begin(), report.occluders.end(),
            [](const auto &left, const auto &right) {
              if (left.otOwner != right.otOwner)
                return left.otOwner < right.otOwner;
              return left.physicalOwner < right.physicalOwner;
            });
  return report;
}

} // namespace ps1::gpu::replay
