#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ps1::gpu::replay::vulkan_abi {
struct SidecarPreviousVertex;
}

namespace ps1::gpu::interpolation {

// Stable renderer-side identity.  The producer decides what each word means;
// the interpolation core deliberately knows nothing about a particular game,
// draw order or transient GPU address.  Equality of all four words is the
// minimum proof that two samples describe the same logical vertex.
struct TemporalVertexIdentity {
  std::array<uint64_t, 4> words{};

  bool operator==(const TemporalVertexIdentity &) const = default;
};

struct TemporalVertexIdentityHash {
  std::size_t operator()(const TemporalVertexIdentity &identity) const noexcept;
};

// A final projected endpoint, expressed relative to the presentation page
// which contains it.  Keeping the page origin separate is essential on PS1:
// double-buffered 512-pixel pages alternate in VRAM even though they occupy
// the same physical screen.
struct TemporalVertexSample {
  TemporalVertexIdentity identity{};
  std::array<float, 2> positionNative{};
  float projectiveW = 0.0f;
  std::array<float, 2> presentationOriginNative{};
  // Exact affine depth family applied after projective W.  A changed family
  // is a presentation discontinuity and must snap instead of interpolating.
  std::array<uint32_t, 4> presentationDepthBits{};
  bool eligible = false;
};

enum class TemporalVertexInterpolationStatus : uint8_t {
  Interpolated = 0,
  IdentityMismatch,
  Ineligible,
  DepthFamilyMismatch,
  InvalidWeight,
  InvalidEndpoint,
  InvalidResult,
};

struct TemporalVertexInterpolationResult {
  std::array<float, 2> positionNative{};
  float projectiveW = 0.0f;
  TemporalVertexInterpolationStatus status =
      TemporalVertexInterpolationStatus::InvalidEndpoint;

  bool interpolated() const noexcept {
    return status == TemporalVertexInterpolationStatus::Interpolated;
  }
};

// A packet polygon may expand into several triangles. They must all use the
// same temporal phase and agree on shared source corners. Otherwise invalidate
// its previous records atomically, retaining the complete authentic endpoint.
// Returns the number of previously interpolatable vertices now invalidated.
uint32_t cohereTemporalPrimitive(
    std::span<replay::vulkan_abi::SidecarPreviousVertex> previous,
    std::span<const uint8_t> sourceCorners) noexcept;

// Interpolate in homogeneous projected space, then express the result on the
// current endpoint's page.  Linear interpolation of screen X/Y and W as three
// unrelated scalars is not equivalent and bends perspective motion whenever
// W changes.  This is the projected equivalent of RT64's previous/current
// transformed-position interpolation; no guest timing or state is advanced.
TemporalVertexInterpolationResult
interpolateTemporalVertex(const TemporalVertexSample &previous,
                          const TemporalVertexSample &current,
                          float currentWeight) noexcept;

} // namespace ps1::gpu::interpolation
