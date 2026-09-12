#pragma once

#include "runtime/gpu/modern_replay_execution_plan.h"
#include "runtime/gpu/modern_replay_visibility_audit.h"
#include "runtime/gpu/modern_replay_vulkan_abi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace ps1::gpu::replay {

// Capabilities must describe the complete pipeline which will consume this
// bake.  Every extension to the first opaque Projected3D profile is a separate
// opt-in so a partial executor fails before publishing any payload.
struct VulkanReplayBakeFeatures {
  bool allowTextured = false;
  bool allowSemiTransparent = false;
  bool allowScreen2D = false;
  bool allowDestinationColorFeedback = false;
  bool allowDestinationMaskRead = false;
  bool allowPainterOrdered = false;
  bool allowPainterWriteRealDepth = false;
  bool allowDepthInvalidate = false;
  bool allowCpuUpload = false;
  // General CopyRect execution needs transfer-local source snapshots and,
  // for E6 destination checks, a second mask snapshot.  Keeping this separate
  // from the donor-proved direct path prevents a partial backend from
  // accepting overlap, wrap or mask semantics accidentally.
  bool allowCopyRectScratch = false;
  // Sparse line/rectangle compatibility writes are expanded into ordered
  // one-pixel Screen2D draws plus exact R16 mirror uploads.  Keep the command
  // family opt-in even when all of those lower-level paths are available.
  bool allowPaintPixels = false;
  // Explicit coexistence with original screen-coordinate polygons. These
  // retain packet-atomic W and painter ordering, without claiming precision.
  bool allowNativeProjectionFallback = false;
};

struct VulkanReplayBakedRect {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;

  bool operator==(const VulkanReplayBakedRect &) const = default;
};

struct VulkanReplayBakedNativeUpload {
  struct Region {
    // Word offset in VulkanReplayBakedBatch::nativeUploadPixels.  The future
    // executor converts it to a byte offset for VkBufferImageCopy.
    std::size_t sourceWordOffset = 0;
    uint32_t bufferRowLength = 0;
    uint32_t bufferImageHeight = 0;
    VulkanReplayBakedRect destination;

    bool operator==(const Region &) const = default;
  };

  NativeRect destination;
  std::size_t firstPixel = 0;
  std::size_t pixelCount = 0;
  uint64_t nativeRevision = 0;
  // Native VRAM wraps independently on both axes.  Keeping one row-major
  // payload while splitting the destination produces at most four direct
  // Vulkan copy regions without repacking or duplicating pixels.
  std::array<Region, 4> regions{};
  uint8_t regionCount = 0;

  bool operator==(const VulkanReplayBakedNativeUpload &) const = default;
};

struct VulkanReplayBakedDepthClear {
  VulkanReplayBakedRect area;
  uint64_t nextEpoch = 0;
  bool fromReset = false;
  float clearValue = 1.0f;

  bool operator==(const VulkanReplayBakedDepthClear &) const = default;
};

struct VulkanReplayBakedPaintRect {
  // High-resolution color/mask/depth attachments are updated over this area.
  VulkanReplayBakedRect atlasArea;
  // The native R16 mirror is sourced from the batch-wide upload arena.  Its
  // sourceWordOffset is always even, satisfying Vulkan's four-byte
  // VkBufferImageCopy::bufferOffset alignment after conversion to bytes.
  VulkanReplayBakedNativeUpload::Region nativeRegion;
  Rgb8 color;
  uint32_t maskValue = 0;
  uint64_t depthEpoch = 0;
  float depthClearValue = 1.0f;
  uint64_t coalescedCommandCount = 0;

  bool operator==(const VulkanReplayBakedPaintRect &) const = default;
};

struct VulkanReplayBakedFillRect {
  struct Region {
    // Fill payloads are homogeneous, so each wrapped segment is tightly
    // repacked at an independently four-byte-aligned arena offset.
    VulkanReplayBakedNativeUpload::Region nativeRegion;
    VulkanReplayBakedRect atlasArea;

    bool operator==(const Region &) const = default;
  };

  std::array<Region, 4> regions{};
  uint8_t regionCount = 0;
  Rgb8 color;
  uint32_t maskValue = 0;
  float depthClearValue = 1.0f;

  bool operator==(const VulkanReplayBakedFillRect &) const = default;
};

// VkCmdCopyImage can use the source attachments in-place only after the bake
// proves that every source/destination rectangle is non-wrapped and disjoint.
// Other donor-equivalent cases use a transfer-local snapshot, so all source
// samples and the optional pre-write destination mask are fixed before the
// first write. Ordered overlaps which are not snapshot-equivalent fail bake
// and remain atomic M0 reanchors rather than selecting either path below.
enum class VulkanReplayCopyPath : uint8_t {
  DirectNonOverlap,
  ScratchSnapshot,
};

struct VulkanReplayBakedCopyRegion {
  // Logical transfer coordinates are also the coordinates in each bounded
  // scratch image. Source and destination rectangles are independently split
  // wherever either wrapped axis crosses the native surface edge. Each rect
  // below is therefore in bounds and suitable for one Vulkan image region.
  VulkanReplayBakedRect scratchNative;
  VulkanReplayBakedRect sourceNative;
  VulkanReplayBakedRect destinationNative;
  VulkanReplayBakedRect scratchAtlas;
  VulkanReplayBakedRect sourceAtlas;
  VulkanReplayBakedRect destinationAtlas;

  bool operator==(const VulkanReplayBakedCopyRegion &) const = default;
};

struct VulkanReplayBakedCopyRect {
  static constexpr std::size_t MAX_SCRATCH_REGIONS = 9;

  VulkanReplayCopyPath path = VulkanReplayCopyPath::DirectNonOverlap;
  // DirectNonOverlap retains these four legacy rectangles unchanged.
  // ScratchSnapshot consumers use regions instead; the aggregate rectangles
  // remain zero so a consumer cannot accidentally route a wrapped copy to an
  // in-place VkCmdCopyImage.
  VulkanReplayBakedRect sourceNative;
  VulkanReplayBakedRect destinationNative;
  VulkanReplayBakedRect sourceAtlas;
  VulkanReplayBakedRect destinationAtlas;

  // Complete schema-7 source proof retained after the execution plan is
  // released.  ScratchSnapshot must snapshot every enabled source plane for
  // all regions before applying any destination region.
  NativeReadDependency nativeRead;
  // checkMaskBit gates every destination plane against the pre-write mask
  // snapshot. forceMaskBit is ORed into accepted native/mask outputs only;
  // source true color is never re-quantized.
  MaskState mask;
  VulkanReplayBakedRect scratchNativeExtent;
  VulkanReplayBakedRect scratchAtlasExtent;
  std::array<VulkanReplayBakedCopyRegion, MAX_SCRATCH_REGIONS> regions{};
  uint8_t regionCount = 0;
  // DirectNonOverlap may keep the read-through native cache current as a
  // transfer optimization. ScratchSnapshot deliberately leaves R16 stale:
  // the recorder publishes an authoritative NativeTileUpload before every
  // later native reader, while the modern color/mask planes must be updated
  // immediately for presentation and ordered destination feedback.
  bool snapshotSourceNativeR16 = false;
  bool snapshotSourceColorRgba8 = false;
  bool snapshotSourceMaskR8 = false;
  // This is distinct from the source-mask snapshot, even when source and
  // destination overlap. Its zero bit accepts native/color/mask/R32/D32.
  bool snapshotDestinationMaskR8 = false;
  bool copyNativeR16 = true;
  bool copyColorRgba8 = true;
  bool copyMaskR8 = true;
  // With checkMaskBit, invalidation is performed only for accepted pixels.
  // Rejected pixels retain every destination plane, including R32 and D32.
  bool invalidateDepthD32 = true;
  float depthClearValue = 1.0f;

  bool operator==(const VulkanReplayBakedCopyRect &) const = default;
};

struct VulkanReplayBakedCpuUploadRun {
  // Offset in the source CpuUpload::pixels chunk.  Accepted runs retain source
  // order even though rejected pixels are deliberately absent from the arena.
  std::size_t sourceChunkOffset = 0;
  // A tightly packed one-row R16 copy. sourceWordOffset is always even, so its
  // byte conversion satisfies VkBufferImageCopy's four-byte alignment.
  VulkanReplayBakedNativeUpload::Region nativeRegion;
  // The same accepted run in the scaled modern attachments.  The executor
  // expands R16 into true color/mask, writes sentinel R32 and invalidates D32
  // only over this area.
  VulkanReplayBakedRect atlasArea;
  bool expandColorMask = true;
  bool invalidateDepthD32 = true;
  float depthClearValue = 1.0f;

  bool operator==(const VulkanReplayBakedCpuUploadRun &) const = default;
};

struct VulkanReplayBakedCpuUpload {
  uint64_t transactionId = UNASSIGNED_PACKET_SEQUENCE;
  CpuUploadPhase phase = CpuUploadPhase::Begin;
  NativeRect transfer;
  uint64_t pixelOffset = 0;
  uint64_t consumedPixelCount = 0;
  uint64_t acceptedPixelCount = 0;
  uint64_t rejectedPixelCount = 0;
  // checkMaskBit is provenance only: acceptedBits has already applied E6 and
  // the executor must never read the live destination mask again. forceMaskBit
  // has already been ORed into every word referenced by runs.
  MaskState sourceMask;
  std::vector<VulkanReplayBakedCpuUploadRun> runs;

  bool operator==(const VulkanReplayBakedCpuUpload &) const = default;
};

enum class VulkanReplayDrawDepthPath : uint8_t {
  LessEqualWrite,
  // Preserve GP0/OT visibility while storing genuine projective W.
  PainterWriteRealDepth,
  PainterOrdered,
  InvalidateAcceptedFar,
};

enum class VulkanReplayFeedbackPlane : uint8_t {
  Color = 1u << 0u,
  Mask = 1u << 1u,
};

using VulkanReplayFeedbackPlaneMask = uint8_t;

constexpr VulkanReplayFeedbackPlaneMask
vulkanReplayFeedbackPlaneBit(VulkanReplayFeedbackPlane plane) noexcept {
  return static_cast<VulkanReplayFeedbackPlaneMask>(plane);
}

enum class VulkanReplayDestinationSnapshotAction : uint8_t {
  None,
  Initial,
  Refresh,
};

struct VulkanReplayBakedDrawSegment {
  uint32_t firstVertex = 0;
  uint32_t vertexCount = 0;
  VulkanReplayBakedRect scissor;
  // Empty for draws which do not sample the destination.  Otherwise this is
  // a conservatively padded, atlas-space rectangle bounded by scissor.
  VulkanReplayBakedRect destinationSnapshot;
  vulkan_abi::DrawUniforms uniforms;
  VulkanReplayDestinationSnapshotAction snapshotAction =
      VulkanReplayDestinationSnapshotAction::None;

  bool operator==(const VulkanReplayBakedDrawSegment &other) const {
    return firstVertex == other.firstVertex &&
           vertexCount == other.vertexCount && scissor == other.scissor &&
           destinationSnapshot == other.destinationSnapshot &&
           uniforms.positionScaleBias == other.uniforms.positionScaleBias &&
           uniforms.depthScaleBias == other.uniforms.depthScaleBias &&
           uniforms.uvDomain == other.uniforms.uvDomain &&
           uniforms.textureState == other.uniforms.textureState &&
           uniforms.textureWindow == other.uniforms.textureWindow &&
           uniforms.destinationState == other.uniforms.destinationState &&
           snapshotAction == other.snapshotAction;
  }
};

struct VulkanReplayBakedDraw {
  // Legacy packet-wide view retained while the live opaque executor migrates
  // to segments.  It mirrors the complete vertex span and first segment.
  uint32_t firstVertex = 0;
  uint32_t vertexCount = 0;
  VulkanReplayBakedRect scissor;
  vulkan_abi::DrawUniforms uniforms;
  uint64_t depthEpoch = 0;
  // Preserve the semantic route through bake so executor telemetry can
  // distinguish Projected3D OT boundaries from Screen2D invalidation even
  // when both use the same fixed-function ALWAYS+far-write pipeline.
  PacketRoute route = PacketRoute::Projected3D;
  bool sourceRecovery = false;
  VulkanReplayDrawDepthPath depthPath =
      VulkanReplayDrawDepthPath::LessEqualWrite;
  VulkanReplayFeedbackPlaneMask feedbackPlanes = 0;
  std::array<VulkanReplayBakedDrawSegment, 2> segments{};
  uint8_t segmentCount = 0;
  // One-based indices into VulkanReplayBakedBatch::visibilityOwners. Each
  // source triangle gets a stable token even when its intrinsic provenance is
  // incomplete; the audit can then report it but cannot certify a family.
  std::array<uint32_t, 2> visibilityOwnerTokens{};
  uint8_t visibilityOwnerCount = 0;
};

struct VulkanReplayBakedLatch {
  FrameLatch latch;

  bool operator==(const VulkanReplayBakedLatch &) const = default;
};

using VulkanReplayBakedCommandPayload =
    std::variant<VulkanReplayBakedNativeUpload, VulkanReplayBakedDepthClear,
                 VulkanReplayBakedPaintRect, VulkanReplayBakedFillRect,
                 VulkanReplayBakedCopyRect, VulkanReplayBakedCpuUpload,
                 VulkanReplayBakedDraw, VulkanReplayBakedLatch>;

struct VulkanReplayBakedCommand {
  std::size_t sourceOrdinal = 0;
  VulkanReplayBakedCommandPayload payload;
};

struct VulkanReplayBakedBatch {
  uint32_t internalScale = 0;
  std::vector<VulkanReplayBakedCommand> commands;
  // Draws reference this storage through firstVertex/vertexCount.  Triangle
  // indices have already been expanded, so vkCmdDraw can retain source order
  // without an index buffer or topology reconstruction.
  std::vector<vulkan_abi::Vertex> vertices;
  std::vector<ReplayVisibilityAuditOwner> visibilityOwners;
  // Native uploads and accepted mutation runs use the same flat R16 arena and
  // therefore remain owned after the source execution plan is released.
  std::vector<uint16_t> nativeUploadPixels;

  bool empty() const noexcept {
    return commands.empty() && vertices.empty() && visibilityOwners.empty() &&
           nativeUploadPixels.empty();
  }
};

enum class VulkanReplayVisibilityBoundsStatus : uint8_t {
  Complete = 0,
  Empty,
  InvalidBatch,
};

// Conservative atlas-space union of every triangle which can affect either
// visibility pass. This follows submitted vertices and scissors, rather than
// the currently displayed page: double-buffered games commonly render the
// next frame outside the live scanout aperture. One atlas sample of padding
// on each edge prevents a fractional high-precision boundary from being
// omitted. No object/family count, screen-size cluster or scale-factor prior
// participates in this selection.
struct VulkanReplayVisibilityBoundsResult {
  VulkanReplayVisibilityBoundsStatus status =
      VulkanReplayVisibilityBoundsStatus::InvalidBatch;
  VulkanReplayBakedRect atlasArea{};

  bool complete() const noexcept {
    return status == VulkanReplayVisibilityBoundsStatus::Complete;
  }
};

VulkanReplayVisibilityBoundsResult measureVulkanReplayVisibilityBounds(
    const VulkanReplayBakedBatch &batch) noexcept;

enum class VulkanReplayBakeReason : uint8_t {
  None,
  EmptyPlan,
  InvalidScale,
  PolicyScaleMismatch,
  SourceOperationMismatch,
  UnsupportedSurfaceSeed,
  UnsupportedExecutionKind,
  TexturedDrawDisabled,
  SemiTransparentDrawDisabled,
  Screen2DDrawDisabled,
  DestinationColorFeedbackDisabled,
  DestinationMaskReadDisabled,
  PainterOrderedDepthDisabled,
  DepthInvalidateDisabled,
  CpuUploadDisabled,
  UnsupportedDrawRoute,
  UnsupportedDrawState,
  InvalidDepthState,
  IncompletePrecisionProvenance,
  InvalidTriangleIndex,
  NonFiniteVertexAttribute,
  ProjectiveWOutOfDomain,
  InvalidNativeTileUpload,
  InvalidPaintRect,
  UnsupportedPaintRectState,
  InvalidFillRect,
  UnsupportedFillRectState,
  InvalidCopyRect,
  CopyRectScratchRequired,
  UnsupportedCopyRectState,
  InvalidCpuUpload,
  UnsupportedCpuUploadState,
  InvalidDepthCommand,
  NumericOverflow,
  // A full immutable source snapshot is not equivalent to ordered native
  // execution when a destination pixel can become a later source pixel.
  // Keep this append-only reason distinct from malformed state so telemetry
  // can track the remaining donor-semantics gap.
  CopyRectOrderedOverlapUnproven,
  // Append-only: numeric bake reasons are exported in runtime telemetry.
  PainterWriteRealDepthDisabled,
  PaintPixelsDisabled,
  InvalidPaintPixels,
  UnsupportedPaintPixelsState,
};

struct VulkanReplayBakeResult {
  static constexpr std::size_t NO_SOURCE_ORDINAL =
      std::numeric_limits<std::size_t>::max();

  VulkanReplayBakeReason reason = VulkanReplayBakeReason::None;
  std::size_t sourceOrdinal = NO_SOURCE_ORDINAL;
  ReplayExecutionKind sourceKind = ReplayExecutionKind::FrameLatch;
  VulkanReplayBakedBatch batch;

  bool accepted() const noexcept {
    return reason == VulkanReplayBakeReason::None;
  }
};

// This is a CPU-only, transactional transform.  On every validation failure
// the returned batch owns no commands, vertices or upload pixels; no Vulkan
// object or mutable renderer state is observed.
VulkanReplayBakeResult
bakeModernReplayVulkan(const ModernReplayExecutionPlan &plan,
                       uint32_t internalScale,
                       VulkanReplayBakeFeatures features = {});

std::string_view
vulkanReplayBakeReasonName(VulkanReplayBakeReason reason) noexcept;

} // namespace ps1::gpu::replay
