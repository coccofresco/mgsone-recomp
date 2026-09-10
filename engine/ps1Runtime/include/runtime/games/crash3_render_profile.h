#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace ps1::gpu::replay {
struct ReplayVisibilityAuditOwner;
namespace vulkan_abi {
struct Vertex;
}
} // namespace ps1::gpu::replay

namespace ps1::games::crash3 {

// Shared with every Crash background shader: next binary32 below 2^32.
inline constexpr uint32_t CAMERA_BACKGROUND_DEPTH_BITS = UINT32_C(0x4f7fffff);

// Renderer-only result for the Crash 3 presentation-depth table. Keeping this
// API separate from crash3_profile.h prevents generic GTE users from depending
// on Vulkan replay types.
enum class PresentationDepthStatus : uint8_t {
  Apply = 0,
  // Correct the persistent presented-surface plane while retaining original
  // OT color/occlusion until cross-family ordering is independently proven.
  ApplySurfaceOnly,
  Disabled,
  IneligibleOwner,
  UnmappedOwner,
  InvalidVertexSpan,
  ProjectiveWOutOfDomain,
};

struct PresentationDepthDecision {
  PresentationDepthStatus status = PresentationDepthStatus::Disabled;
  float multiplier = 1.0f;

  bool applies() const noexcept {
    return status == PresentationDepthStatus::Apply ||
           status == PresentationDepthStatus::ApplySurfaceOnly;
  }
};

// Stable game-local classification for the SLST block that contains the
// recovered warp-room side geometry. Cohort entries delimit where a deferred
// correction may be replayed; Recovered entries are the only triangles the
// certified mode is allowed to draw again. No screen coordinate or frame
// number participates in this classification.
enum class SceneryOcclusionClass : uint8_t {
  None = 0,
  Cohort,
  Recovered,
};

SceneryOcclusionClass classifySceneryOcclusion(
    const gpu::replay::ReplayVisibilityAuditOwner &owner) noexcept;

// Decode the exact Crash-owned SLST identity carried by all three projected
// vertices of one replay owner. This is a host-side correlation key: it lets
// Vulkan bind reconstructed resident geometry to the original packet cohort
// and render target without exposing any added face or timing state to the
// guest. Incoherent/non-scenery owners fail closed.
std::optional<uint16_t> sceneryProjectionEntry(
    const gpu::replay::ReplayVisibilityAuditOwner &owner) noexcept;

// Exact host-only sidecar generation carried by the same coherent scenery
// projection. A non-zero value joins one resident snapshot to the packet
// cohort constructed from it even while the camera is moving; numeric frame
// offsets and projected-coordinate equality are not temporal identity.
std::optional<uint32_t> sceneryProjectionCapture(
    const gpu::replay::ReplayVisibilityAuditOwner &owner) noexcept;

// One source packet may be expanded into two Vulkan triangles. The packet
// planner consumes both immutable owner records together so a quad receives
// one affine transform and cannot acquire a diagonal depth seam.
struct PresentationSurfaceTriangle {
  const gpu::replay::ReplayVisibilityAuditOwner *owner = nullptr;
};

enum class PresentationSurfaceStatus : uint8_t {
  Apply = 0,
  Disabled,
  IneligiblePacket,
  MissingOrderingTableAnchor,
  IncoherentOrderingTableAnchor,
  UnsupportedProjectionFamily,
  ProjectiveWOutOfDomain,
};

struct PresentationSurfaceDecision {
  PresentationSurfaceStatus status = PresentationSurfaceStatus::Disabled;
  float multiplier = 1.0f;
  float bias = 0.0f;
  uint16_t orderingTableBucket = 0;
  uint8_t uniqueVertexCount = 0;

  bool applies() const noexcept {
    return status == PresentationSurfaceStatus::Apply;
  }
};

// Crash's exact OT anchor places the packet in the intended cross-family
// order. Genuine GTE W retains the continuous within-packet surface. The
// returned affine map is presentation-only and must not silently become the
// color/occlusion policy.
PresentationSurfaceDecision planPresentedSurfaceDepth(
    std::span<const PresentationSurfaceTriangle> triangles) noexcept;

// Exact Crash 3 construction-lane lookup. Numeric coordinates, frame numbers,
// object positions and observed depth are deliberately absent from the key.
PresentationDepthDecision planPresentationDepth(
    const gpu::replay::ReplayVisibilityAuditOwner &owner,
    std::span<const gpu::replay::vulkan_abi::Vertex> vertices) noexcept;

} // namespace ps1::games::crash3
