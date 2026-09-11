#pragma once

#include "runtime/gpu/gpu.h"
#include "runtime/gpu/renderer_options.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

struct SDL_Window;

namespace ps1::gpu {

// Map both rectangle endpoints so cropped/offset signals retain the same
// framing with noninteger, anisotropic presentation scaling.
inline OutputViewport mapVulkanScanoutViewport(const DisplayScanoutPlan &plan,
                                               uint32_t width,
                                               uint32_t height) {
  if (!plan.canvas.width || !plan.canvas.height)
    return {};
  const auto x = uint64_t(plan.destinationOrigin.x) * width / plan.canvas.width;
  const auto y =
      uint64_t(plan.destinationOrigin.y) * height / plan.canvas.height;
  const auto right =
      (uint64_t(plan.destinationOrigin.x) + plan.activeExtent.width) * width /
      plan.canvas.width;
  const auto bottom =
      (uint64_t(plan.destinationOrigin.y) + plan.activeExtent.height) * height /
      plan.canvas.height;
  return {int(x), int(y), int(right - x), int(bottom - y)};
}

struct VulkanReplayExtent {
  uint32_t width = 0;
  uint32_t height = 0;
};

// Presentation-only inspection modes. They never alter replay attachments or
// guest-visible GPU state; future G-buffer/effect diagnostics can extend this
// enum without adding game-specific renderer switches.
enum class VulkanPresentationView : uint8_t {
  Color = 0,
  // Persistent R32 last-visible-surface plane after presentation correction.
  // It stays first in the F8 cycle to preserve the previous one-press view.
  DepthPostCorrection,
  // Independent strict-W reconstruction of the current accepted triangle
  // stream, before presentation-depth policy/candidate correction.
  DepthPreCorrection,
  // Spatially aligned split: pre-correction on the left, post-correction on
  // the right. Both halves use the same false-colour transfer function.
  DepthComparison,
};

inline constexpr bool
vulkanPresentationViewUsesDepth(VulkanPresentationView view) {
  return view != VulkanPresentationView::Color;
}

inline constexpr bool
vulkanPresentationViewUsesPreCorrectionDepth(VulkanPresentationView view) {
  return view == VulkanPresentationView::DepthPreCorrection ||
         view == VulkanPresentationView::DepthComparison;
}

inline constexpr VulkanPresentationView
nextVulkanPresentationView(VulkanPresentationView view) {
  switch (view) {
  case VulkanPresentationView::Color:
    return VulkanPresentationView::DepthPostCorrection;
  case VulkanPresentationView::DepthPostCorrection:
    return VulkanPresentationView::DepthPreCorrection;
  case VulkanPresentationView::DepthPreCorrection:
    return VulkanPresentationView::DepthComparison;
  case VulkanPresentationView::DepthComparison:
    return VulkanPresentationView::Color;
  }
  return VulkanPresentationView::Color;
}

// Backend-independent push ABI shared by the native-R16 and replay-RGBA8
// scanout shaders. The renderer owns canvas placement through its viewport and
// scissor; this payload describes only the active image's source mapping.
//
// source = {sourceByteX, sourceY, rowStep, sourceScale}
// active = {activeWidth, activeHeight, DisplayScanoutFormat,
//           DisplayScanoutField}
//
// sourceScale is one for the authoritative R16 snapshot and the atlas scale
// for an RGB555 replay resolve. PackedRGB888 is byte-addressed native VRAM and
// therefore has a valid execution mapping only at sourceScale=1.
struct VulkanScanoutPushConstants {
  std::array<uint32_t, 4> source{};
  std::array<uint32_t, 4> active{};
};

static_assert(std::is_standard_layout_v<VulkanScanoutPushConstants>);
static_assert(std::is_trivially_copyable_v<VulkanScanoutPushConstants>);
static_assert(sizeof(VulkanScanoutPushConstants) == 32);
static_assert(offsetof(VulkanScanoutPushConstants, source) == 0);
static_assert(offsetof(VulkanScanoutPushConstants, active) == 16);
static_assert(static_cast<uint32_t>(DisplayScanoutFormat::RGB555) == 0);
static_assert(static_cast<uint32_t>(DisplayScanoutFormat::PackedRGB888) == 1);
static_assert(static_cast<uint32_t>(DisplayScanoutField::NotApplicable) == 0);
static_assert(static_cast<uint32_t>(DisplayScanoutField::Unknown) == 1);
static_assert(static_cast<uint32_t>(DisplayScanoutField::Even) == 2);
static_assert(static_cast<uint32_t>(DisplayScanoutField::Odd) == 3);

// Temporal resolve samples two different guest display pages: the page where
// endpoint B's midpoint was rendered and the still-authoritative endpoint-A
// page.  Keeping the second source explicit prevents the PSX double-buffer
// flip from turning A -> midpoint -> B into midpoint -> A.
struct VulkanTemporalScanoutPushConstants {
  VulkanScanoutPushConstants midpoint{};
  // sourceByteX, sourceY, rowStep, sourceScale.
  std::array<uint32_t, 4> previousSource{};
};

static_assert(std::is_standard_layout_v<VulkanTemporalScanoutPushConstants>);
static_assert(std::is_trivially_copyable_v<VulkanTemporalScanoutPushConstants>);
static_assert(sizeof(VulkanTemporalScanoutPushConstants) == 48);
static_assert(offsetof(VulkanTemporalScanoutPushConstants, midpoint) == 0);
static_assert(offsetof(VulkanTemporalScanoutPushConstants, previousSource) ==
              32);

inline constexpr VulkanScanoutPushConstants
makeVulkanScanoutPushConstants(const DisplayScanoutPlan &plan,
                               uint32_t sourceScale) {
  return {{plan.sourceByteX, plan.sourceY, plan.rowStep, sourceScale},
          {plan.activeExtent.width, plan.activeExtent.height,
           static_cast<uint32_t>(plan.format),
           static_cast<uint32_t>(plan.field)}};
}

inline constexpr VulkanTemporalScanoutPushConstants
makeVulkanTemporalScanoutPushConstants(const DisplayScanoutPlan &midpoint,
                                       const DisplayScanoutPlan &previous,
                                       uint32_t sourceScale) {
  return {
      makeVulkanScanoutPushConstants(midpoint, sourceScale),
      {previous.sourceByteX, previous.sourceY, previous.rowStep, sourceScale}};
}

inline constexpr bool
isVulkanScanoutPushConstantsValid(const VulkanScanoutPushConstants &push) {
  const uint32_t format = push.active[2];
  const bool knownFormat =
      format == static_cast<uint32_t>(DisplayScanoutFormat::RGB555) ||
      format == static_cast<uint32_t>(DisplayScanoutFormat::PackedRGB888);
  const bool packed =
      format == static_cast<uint32_t>(DisplayScanoutFormat::PackedRGB888);
  return push.source[0] < GPU::VRAM_WIDTH * sizeof(Color16) &&
         push.source[1] < GPU::VRAM_HEIGHT && push.source[2] != 0 &&
         push.source[2] <= GPU::VRAM_HEIGHT && push.source[3] >= 1 &&
         push.source[3] <= replay::MAX_INTERNAL_SCALE && push.active[0] != 0 &&
         push.active[1] != 0 && knownFormat &&
         (packed ? push.source[3] == 1 : (push.source[0] & 1u) == 0);
}

inline constexpr bool isVulkanTemporalScanoutPushConstantsValid(
    const VulkanTemporalScanoutPushConstants &push) {
  const auto &previous = push.previousSource;
  return isVulkanScanoutPushConstantsValid(push.midpoint) &&
         previous[0] < GPU::VRAM_WIDTH * sizeof(Color16) &&
         previous[1] < GPU::VRAM_HEIGHT && previous[2] != 0 &&
         previous[2] <= GPU::VRAM_HEIGHT && previous[3] >= 1 &&
         previous[3] <= replay::MAX_INTERNAL_SCALE && (previous[0] & 1u) == 0;
}

// Endpoint-A colour may fill disoccluded pixels in a synthetic midpoint only
// while both endpoints describe the same intrinsic scene membership.  These
// bits deliberately encode categorical identities, never screen-space or
// face-count heuristics: a non-zero mask means that retaining an old pixel can
// resurrect an outgoing sector, object, or mesh topology.
enum VulkanTemporalHistoryDiscontinuity : uint32_t {
  VulkanTemporalHistoryContinuous = 0u,
  VulkanTemporalHistoryResidentPresence = 1u << 0u,
  VulkanTemporalHistoryResidentTopology = 1u << 1u,
  VulkanTemporalHistoryResidentResources = 1u << 2u,
  VulkanTemporalHistoryResidentCohort = 1u << 3u,
  VulkanTemporalHistoryObjectMembership = 1u << 4u,
  VulkanTemporalHistoryObjectGeometry = 1u << 5u,
};

struct VulkanTemporalHistoryIdentity {
  uint64_t topologyFingerprint = 0u;
  uint64_t resourceSetFingerprint = 0u;
  uint64_t activeCohortFingerprint = 0u;
  uint32_t activeCohortGeneration = 0u;
  bool residentPresent = false;
  bool objectPresent = false;
};

inline constexpr uint32_t vulkanTemporalHistoryDiscontinuityMask(
    const VulkanTemporalHistoryIdentity &previous,
    const VulkanTemporalHistoryIdentity &current, bool objectMembershipChanged,
    bool objectGeometryChanged) {
  uint32_t mask = VulkanTemporalHistoryContinuous;
  if (previous.residentPresent != current.residentPresent)
    mask |= VulkanTemporalHistoryResidentPresence;
  if ((previous.residentPresent || current.residentPresent) &&
      previous.topologyFingerprint != current.topologyFingerprint)
    mask |= VulkanTemporalHistoryResidentTopology;
  if ((previous.residentPresent || current.residentPresent) &&
      previous.resourceSetFingerprint != current.resourceSetFingerprint)
    mask |= VulkanTemporalHistoryResidentResources;
  if ((previous.residentPresent || current.residentPresent) &&
      (previous.activeCohortFingerprint != current.activeCohortFingerprint ||
       previous.activeCohortGeneration != current.activeCohortGeneration))
    mask |= VulkanTemporalHistoryResidentCohort;
  if (previous.objectPresent != current.objectPresent ||
      objectMembershipChanged)
    mask |= VulkanTemporalHistoryObjectMembership;
  if (objectGeometryChanged)
    mask |= VulkanTemporalHistoryObjectGeometry;
  return mask;
}

// A background underlay is coverage for one resident scene revision, not an
// independent persistent object. Reusing its old projected vertices after a
// source/topology/cohort change can move yesterday's sky under today's camera
// even when the main resident layer correctly snaps to the new endpoint.
// Unrelated object animation/membership must not stop the whole background;
// object-owned underlays also retain their per-object variant checks.
inline constexpr bool
vulkanTemporalBackgroundUnderlayHistoryCompatible(uint32_t discontinuityMask) {
  constexpr uint32_t independentObjectBits =
      VulkanTemporalHistoryObjectMembership |
      VulkanTemporalHistoryObjectGeometry;
  // Future/unknown discontinuity domains remain fail-closed until their
  // relationship to this scene-owned layer is explicitly established.
  return (discontinuityMask & ~independentObjectBits) == 0u;
}

// One bit per native 512x256 framebuffer quadrant in the 1024x512 PS1 VRAM.
// Zero is deliberately invalid, so callers cannot accidentally treat an
// arbitrary scanout offset as a reconstructed depth page.
inline constexpr uint8_t vulkanReplayDepthPageBit(uint32_t nativeX,
                                                  uint32_t nativeY) {
  constexpr uint32_t pageWidth = 512u;
  constexpr uint32_t pageHeight = 256u;
  if ((nativeX % pageWidth) != 0u || (nativeY % pageHeight) != 0u ||
      nativeX >= GPU::VRAM_WIDTH || nativeY >= GPU::VRAM_HEIGHT)
    return 0u;
  const uint32_t page = nativeX / pageWidth +
                        (nativeY / pageHeight) * (GPU::VRAM_WIDTH / pageWidth);
  return static_cast<uint8_t>(1u << page);
}

struct VulkanScanoutSourceCoordinate {
  uint32_t x = 0;
  uint32_t y = 0;

  bool operator==(const VulkanScanoutSourceCoordinate &) const = default;
};

// Map one source-resolution RGB555 sample to the R16 snapshot (scale=1) or
// replay atlas (scale=S). scaledLocalX/Y are relative to the active viewport.
inline constexpr VulkanScanoutSourceCoordinate
mapVulkanScanoutRgb555Source(const VulkanScanoutPushConstants &push,
                             uint32_t scaledLocalX, uint32_t scaledLocalY) {
  const uint32_t scale = push.source[3];
  if (scale == 0)
    return {};
  const uint32_t nativeX = scaledLocalX / scale;
  const uint32_t nativeY = scaledLocalY / scale;
  const uint32_t subX = scaledLocalX % scale;
  const uint32_t subY = scaledLocalY % scale;
  const uint32_t sourceWordX = static_cast<uint32_t>(
      (static_cast<uint64_t>(push.source[0] / sizeof(Color16)) + nativeX) %
      GPU::VRAM_WIDTH);
  const uint32_t sourceRow =
      static_cast<uint32_t>((static_cast<uint64_t>(push.source[1]) +
                             static_cast<uint64_t>(nativeY) * push.source[2]) %
                            GPU::VRAM_HEIGHT);
  return {sourceWordX * scale + subX, sourceRow * scale + subY};
}

struct VulkanScanoutPackedRgb888Source {
  std::array<uint32_t, 3> byteX{};
  uint32_t y = 0;

  bool operator==(const VulkanScanoutPackedRgb888Source &) const = default;
};

// Packed-24 scanout advances three bytes per native output pixel. Each byte,
// including the second/third component, wraps independently within its VRAM
// row; rowStep is applied before the 512-row wrap.
inline constexpr VulkanScanoutPackedRgb888Source
mapVulkanScanoutPackedRgb888Source(const VulkanScanoutPushConstants &push,
                                   uint32_t nativeLocalX,
                                   uint32_t nativeLocalY) {
  constexpr uint32_t rowBytes = GPU::VRAM_WIDTH * sizeof(Color16);
  const uint64_t firstByte = static_cast<uint64_t>(push.source[0]) +
                             static_cast<uint64_t>(nativeLocalX) * 3u;
  const uint32_t sourceRow = static_cast<uint32_t>(
      (static_cast<uint64_t>(push.source[1]) +
       static_cast<uint64_t>(nativeLocalY) * push.source[2]) %
      GPU::VRAM_HEIGHT);
  return {{{static_cast<uint32_t>(firstByte % rowBytes),
            static_cast<uint32_t>((firstByte + 1u) % rowBytes),
            static_cast<uint32_t>((firstByte + 2u) % rowBytes)}},
          sourceRow};
}

// CPU-visible allocation plan for the one persistent precision-replay atlas.
// Byte counts describe texel payloads; Vulkan allocation alignment/padding can
// make the actual device-memory commitment larger.
struct VulkanReplayResourcePlan {
  uint32_t internalScale = 1;
  VulkanReplayExtent nativeShadowExtent{};
  VulkanReplayExtent atlasExtent{};
  uint64_t nativeShadowBytes = 0;
  uint64_t colorBytes = 0;
  uint64_t maskBytes = 0;
  // R32_SFLOAT records the last accepted geometric surface in presentation
  // order. It is deliberately separate from D32, whose value remains free to
  // enforce opaque occlusion without erasing translucent depth information.
  uint64_t surfaceDepthBytes = 0;
  uint64_t depthBytes = 0;

  constexpr uint64_t highPrecisionAtlasBytes() const {
    return colorBytes + maskBytes + surfaceDepthBytes + depthBytes;
  }

  constexpr uint64_t totalBytes() const {
    return nativeShadowBytes + highPrecisionAtlasBytes();
  }
};

// The visibility oracle is intentionally a separate, opt-in allocation. OT,
// strict and the optional lifted-candidate pass retain independent evidence
// images so no pass can certify another accidentally; the CPU combines them
// only after the owning frame fence. Normal presentation allocates none of
// these resources.
struct VulkanReplayVisibilityResourcePlan {
  uint32_t internalScale = 1;
  VulkanReplayExtent atlasExtent{};
  uint64_t ownerPlaneBytes = 0;
  uint64_t depthPlaneBytes = 0;
  uint64_t evidencePlaneBytes = 0;
  uint32_t passCount = 2;

  constexpr uint64_t passBytes() const {
    return ownerPlaneBytes + depthPlaneBytes + evidencePlaneBytes;
  }

  constexpr uint64_t totalBytes() const { return passBytes() * passCount; }
};

// CPU-visible device facts used to qualify the exact persistent replay image
// set before Vulkan allocation. imageUsageSupported represents an exact
// vkGetPhysicalDeviceImageFormatProperties query for the resource's complete
// usage mask, not a looser per-format approximation.
struct VulkanReplayImageSupport {
  bool requiredFeaturesSupported = false;
  bool imageUsageSupported = false;
  bool sampleCountOneSupported = false;
  VulkanReplayExtent maxExtent{};
};

struct VulkanReplayDeviceSupport {
  uint32_t maxImageDimension2D = 0;
  uint32_t maxFramebufferWidth = 0;
  uint32_t maxFramebufferHeight = 0;
  VulkanReplayImageSupport nativeShadow{};
  VulkanReplayImageSupport color{};
  VulkanReplayImageSupport mask{};
  VulkanReplayImageSupport surfaceDepth{};
  VulkanReplayImageSupport depth{};
};

enum class VulkanReplayCapabilityReason : uint32_t {
  NotProbed = 0,
  Supported,
  NativeShadowFormatFeatures,
  NativeShadowImageUsage,
  NativeShadowSampleCount,
  NativeShadowExtent,
  ColorFormatFeatures,
  ColorImageUsage,
  ColorSampleCount,
  ColorExtent,
  MaskFormatFeatures,
  MaskImageUsage,
  MaskSampleCount,
  MaskExtent,
  SurfaceDepthFormatFeatures,
  SurfaceDepthImageUsage,
  SurfaceDepthSampleCount,
  SurfaceDepthExtent,
  DepthFormatFeatures,
  DepthImageUsage,
  DepthSampleCount,
  DepthExtent,
  AtlasImageDimension,
  AtlasFramebufferExtent,
};

struct VulkanReplayCapabilityAssessment {
  VulkanReplayResourcePlan plan{};
  VulkanReplayDeviceSupport support{};
  VulkanReplayCapabilityReason reason = VulkanReplayCapabilityReason::NotProbed;

  constexpr bool capable() const {
    return reason == VulkanReplayCapabilityReason::Supported;
  }
};

const char *
vulkanReplayCapabilityReasonName(VulkanReplayCapabilityReason reason);

VulkanReplayCapabilityAssessment
assessVulkanReplayCapability(const VulkanReplayResourcePlan &plan,
                             const VulkanReplayDeviceSupport &support);

inline constexpr VulkanReplayResourcePlan
planVulkanReplayResources(int requestedScale) {
  constexpr uint32_t nativeWidth = GPU::VRAM_WIDTH;
  constexpr uint32_t nativeHeight = GPU::VRAM_HEIGHT;
  const uint32_t scale = requestedScale < 1 ? 1u
                         : requestedScale > int(replay::MAX_INTERNAL_SCALE)
                             ? replay::MAX_INTERNAL_SCALE
                             : static_cast<uint32_t>(requestedScale);
  const uint64_t nativePixels =
      static_cast<uint64_t>(nativeWidth) * nativeHeight;
  const uint64_t atlasPixels = nativePixels * scale * scale;
  return {
      scale,
      {nativeWidth, nativeHeight},
      {nativeWidth * scale, nativeHeight * scale},
      nativePixels * 2u, // R16_UINT
      atlasPixels * 4u,  // R8G8B8A8_UNORM
      atlasPixels,       // R8_UINT
      atlasPixels * 4u,  // R32_SFLOAT presented-surface depth
      atlasPixels * 4u,  // D32_SFLOAT
  };
}

inline constexpr VulkanReplayVisibilityResourcePlan
planVulkanReplayVisibilityResources(int requestedScale,
                                    bool includeLiftedCandidate = false) {
  const VulkanReplayResourcePlan replay =
      planVulkanReplayResources(requestedScale);
  const uint64_t pixels = static_cast<uint64_t>(replay.atlasExtent.width) *
                          replay.atlasExtent.height;
  return {
      replay.internalScale,
      replay.atlasExtent,
      pixels * 4u, // R32_UINT owner
      pixels * 4u, // D32_SFLOAT physical W
      pixels,      // R8_UINT pass-local evidence
      includeLiftedCandidate ? 3u : 2u,
  };
}

// Vulkan owns presentation and every above-native-resolution resource. The
// native Color16 snapshot is the complete PS1 oracle/feedback surface; it is
// uploaded directly and never expanded to a 4x CPU RGBA buffer.
class RendererVulkan {
public:
  explicit RendererVulkan(GPU &gpu, RendererOptions options = {});
  ~RendererVulkan();

  RendererVulkan(const RendererVulkan &) = delete;
  RendererVulkan &operator=(const RendererVulkan &) = delete;

  bool init(const std::string &windowTitle, int width = 800, int height = 600,
            bool vsync = true, bool fullscreen = false);
  void renderFrame();
  bool processEvents();
  // F8 calls this from the host event loop. It cycles colour, post-correction
  // presented-surface R32, independent pre-correction strict-W depth and a
  // spatially aligned split comparison. Depth uses a shared RGB false-colour
  // logarithmic transfer; the inspection path never mutates guest state.
  void toggleDepthVisualization();
  VulkanPresentationView presentationView() const { return presentationView_; }
  // Save-state loads replace guest frame ordering while Vulkan resources stay
  // alive. Rebase the replay consumer so the next native seed can become the
  // new precision/depth timeline instead of permanently failing preflight.
  void rebaseGuestTimeline();
  // Complete only opt-in diagnostic readbacks and publish their metrics. This
  // waits on the owning FrameContext fences, never on the whole queue.
  void drainReplayDiagnostics();
  void destroy();

  // Passive capability information for the selected M0 device. Replay remains
  // inactive until a later submission path explicitly requests it.
  VulkanReplayCapabilityAssessment replayCapability() const;

private:
  struct Impl;

  GPU &gpu_;
  RendererOptions options_;
  SDL_Window *window_ = nullptr;
  std::unique_ptr<Impl> impl_;
  std::vector<Color16> nativeSnapshotUploadBuffer_;
  uint64_t nativeSnapshotUploadedContentRevision_ = UINT64_MAX;
  VulkanPresentationView presentationView_ = VulkanPresentationView::Color;
  bool ownsVideoSubsystem_ = false;
  bool healthy_ = false;
};

} // namespace ps1::gpu
