#include "runtime/gpu/modern_frame_interpolation.h"
#include "runtime/gpu/modern_replay_vulkan_abi.h"

#include <cmath>
#include <limits>

namespace ps1::gpu::interpolation {

uint32_t cohereTemporalPrimitive(
    std::span<replay::vulkan_abi::SidecarPreviousVertex> previous,
    std::span<const uint8_t> sourceCorners) noexcept {
  constexpr uint32_t valid = replay::vulkan_abi::SIDECAR_PREVIOUS_VALID;
  uint32_t eligible = 0u;
  bool coherent = previous.size() == sourceCorners.size();
  for (std::size_t i = 0u; i < previous.size(); ++i) {
    eligible += (previous[i].packedOriginAndFlags & valid) != 0u;
    if (!coherent)
      continue;
    for (std::size_t j = 0u; j < i; ++j) {
      if (sourceCorners[i] == sourceCorners[j] &&
          (previous[i].positionW != previous[j].positionW ||
           previous[i].packedOriginAndFlags !=
               previous[j].packedOriginAndFlags))
        coherent = false;
    }
  }
  if (eligible == 0u || (coherent && eligible == previous.size()))
    return 0u;
  for (auto &vertex : previous)
    vertex.packedOriginAndFlags &= ~valid;
  return eligible;
}

std::size_t TemporalVertexIdentityHash::operator()(
    const TemporalVertexIdentity &identity) const noexcept {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (const uint64_t word : identity.words) {
    for (uint32_t shift = 0; shift < 64; shift += 8) {
      hash ^= static_cast<uint8_t>(word >> shift);
      hash *= UINT64_C(1099511628211);
    }
  }
  return static_cast<std::size_t>(hash);
}

TemporalVertexInterpolationResult
interpolateTemporalVertex(const TemporalVertexSample &previous,
                          const TemporalVertexSample &current,
                          float currentWeight) noexcept {
  TemporalVertexInterpolationResult result;
  result.positionNative = current.positionNative;
  result.projectiveW = current.projectiveW;

  if (!(previous.identity == current.identity)) {
    result.status = TemporalVertexInterpolationStatus::IdentityMismatch;
    return result;
  }
  if (!previous.eligible || !current.eligible) {
    result.status = TemporalVertexInterpolationStatus::Ineligible;
    return result;
  }
  if (previous.presentationDepthBits != current.presentationDepthBits) {
    result.status = TemporalVertexInterpolationStatus::DepthFamilyMismatch;
    return result;
  }
  if (!std::isfinite(currentWeight) || currentWeight < 0.0f ||
      currentWeight > 1.0f) {
    result.status = TemporalVertexInterpolationStatus::InvalidWeight;
    return result;
  }
  const auto validSample = [](const TemporalVertexSample &sample) {
    return std::isfinite(sample.positionNative[0]) &&
           std::isfinite(sample.positionNative[1]) &&
           std::isfinite(sample.presentationOriginNative[0]) &&
           std::isfinite(sample.presentationOriginNative[1]) &&
           std::isfinite(sample.projectiveW) && sample.projectiveW > 0.0f;
  };
  if (!validSample(previous) || !validSample(current)) {
    result.status = TemporalVertexInterpolationStatus::InvalidEndpoint;
    return result;
  }

  const double weight = static_cast<double>(currentWeight);
  const double previousWeight = 1.0 - weight;
  const double previousW = static_cast<double>(previous.projectiveW);
  const double currentW = static_cast<double>(current.projectiveW);
  const double interpolatedW = previousWeight * previousW + weight * currentW;
  if (!std::isfinite(interpolatedW) || interpolatedW <= 0.0 ||
      interpolatedW > static_cast<double>(std::numeric_limits<float>::max())) {
    result.status = TemporalVertexInterpolationStatus::InvalidResult;
    return result;
  }

  std::array<float, 2> position{};
  for (std::size_t component = 0; component < position.size(); ++component) {
    const double previousLocal =
        static_cast<double>(previous.positionNative[component]) -
        static_cast<double>(previous.presentationOriginNative[component]);
    const double currentLocal =
        static_cast<double>(current.positionNative[component]) -
        static_cast<double>(current.presentationOriginNative[component]);
    const double local = (previousWeight * previousLocal * previousW +
                          weight * currentLocal * currentW) /
                         interpolatedW;
    const double absolute =
        local +
        static_cast<double>(current.presentationOriginNative[component]);
    if (!std::isfinite(absolute) ||
        std::abs(absolute) >
            static_cast<double>(std::numeric_limits<float>::max())) {
      result.status = TemporalVertexInterpolationStatus::InvalidResult;
      return result;
    }
    position[component] = static_cast<float>(absolute);
  }

  result.positionNative = position;
  result.projectiveW = static_cast<float>(interpolatedW);
  result.status = TemporalVertexInterpolationStatus::Interpolated;
  return result;
}

} // namespace ps1::gpu::interpolation
