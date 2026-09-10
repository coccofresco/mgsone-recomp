#pragma once

#include "runtime/gpu/modern_replay.h"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ps1::gpu::replay {

inline constexpr uint32_t CRASH_REPLAY_NATIVE_WIDTH = 1024;
inline constexpr uint32_t CRASH_REPLAY_NATIVE_HEIGHT = 512;
inline constexpr uint32_t CRASH_REPLAY_NATIVE_TILE_COLUMNS =
    CRASH_REPLAY_NATIVE_WIDTH / CRASH_REPLAY_NATIVE_TILE_WIDTH;
inline constexpr uint32_t CRASH_REPLAY_NATIVE_TILE_ROWS =
    CRASH_REPLAY_NATIVE_HEIGHT / CRASH_REPLAY_NATIVE_TILE_HEIGHT;
inline constexpr std::size_t CRASH_REPLAY_NATIVE_TILE_COUNT =
    static_cast<std::size_t>(CRASH_REPLAY_NATIVE_TILE_COLUMNS) *
    CRASH_REPLAY_NATIVE_TILE_ROWS;

using ReplayCapabilityMask = uint64_t;

// Preflight capabilities describe commands and packet paths which one replay
// recorder can execute completely.  They are deliberately independent from
// Vulkan feature bits so this module remains a CPU-only CrashReplayV1 gate.
enum class ReplayCapability : ReplayCapabilityMask {
  NativeTileUpload = UINT64_C(1) << 0,
  SurfaceSeed = UINT64_C(1) << 1,
  DrawPacket = UINT64_C(1) << 2,
  FillRect = UINT64_C(1) << 3,
  CopyRect = UINT64_C(1) << 4,
  CpuUpload = UINT64_C(1) << 5,
  PaintPixels = UINT64_C(1) << 6,
  PaintRect = UINT64_C(1) << 7,
  ClearDepth = UINT64_C(1) << 8,
  Reset = UINT64_C(1) << 9,
  FrameLatch = UINT64_C(1) << 10,

  Projected3D = UINT64_C(1) << 11,
  Screen2D = UINT64_C(1) << 12,
  FallbackInvalidW = UINT64_C(1) << 13,
  FallbackMissingPrecision = UINT64_C(1) << 14,
  FallbackProjectiveContext = UINT64_C(1) << 15,
  FallbackRasterSetup = UINT64_C(1) << 16,
  FallbackUnsupportedFeature = UINT64_C(1) << 17,
  NearPlaneSaturate = UINT64_C(1) << 18,
  // Schema 4 retains homogeneous numerators, but this remains reserved until
  // the Vulkan executor implements packet-atomic geometric clipping.
  NearPlaneGeometricClip = UINT64_C(1) << 19,
  TextureDisable = UINT64_C(1) << 20,
  TextureIndexed4 = UINT64_C(1) << 21,
  TextureIndexed8 = UINT64_C(1) << 22,
  TextureDirect15 = UINT64_C(1) << 23,
  TextureReserved = UINT64_C(1) << 24,

  // DrawPacket capabilities are composable. Advertising the command family
  // alone is never enough: the consumer must opt into every packet property
  // and exactly one depth-state class used by that draw.
  DrawOpaque = UINT64_C(1) << 25,
  DrawSemiTransparent = UINT64_C(1) << 26,
  DrawUntextured = UINT64_C(1) << 27,
  DrawTextured = UINT64_C(1) << 28,
  DestinationColorFeedback = UINT64_C(1) << 29,
  DestinationMaskRead = UINT64_C(1) << 30,
  ForceMaskWrite = UINT64_C(1) << 31,
  DepthTestWriteLessEqual = UINT64_C(1) << 32,
  DepthPainterOrdered = UINT64_C(1) << 33,
  DepthInvalidate = UINT64_C(1) << 34,
  DepthNonCanonical = UINT64_C(1) << 35,
  // GP0/OT-authoritative visibility with genuine projective W stored in D32.
  DepthPainterWrite = UINT64_C(1) << 36,
};

constexpr ReplayCapabilityMask replayCapabilityBit(ReplayCapability value) {
  return static_cast<ReplayCapabilityMask>(value);
}

constexpr ReplayCapabilityMask operator|(ReplayCapability left,
                                         ReplayCapability right) {
  return replayCapabilityBit(left) | replayCapabilityBit(right);
}

constexpr ReplayCapabilityMask operator|(ReplayCapabilityMask left,
                                         ReplayCapability right) {
  return left | replayCapabilityBit(right);
}

inline constexpr ReplayCapabilityMask CRASH_REPLAY_V1_ALL_CAPABILITIES =
    ((UINT64_C(1) << 37) - 1) &
    ~replayCapabilityBit(ReplayCapability::NearPlaneGeometricClip);

enum class ReplayNearClipMode : uint8_t {
  Saturate,
  GeometricClip,
};

enum class ReplayPreflightRoute : uint8_t {
  M0Fallback,
  M1Replay,
};

enum class ReplayPreflightReason : uint8_t {
  None,
  SchemaNameMismatch,
  SchemaVersionMismatch,
  InvalidExpectedAperture,
  InvalidExpectedPolicy,
  InvalidFramePolicy,
  PolicyMismatch,
  InvalidNearClipMode,
  TargetLatchUnavailable,
  NullFrame,
  InvalidOperationLayout,
  FrameNumberDiscontinuity,
  FrameNumberOverflow,
  ContentRevisionRegression,
  ContentRevisionMismatch,
  SubmissionRegression,
  SubmissionMismatch,
  PacketSubmissionOutOfRange,
  InvalidAperture,
  ApertureMismatch,
  DisplayStateMismatch,
  SequenceRangeDiscontinuity,
  SequenceRangeMismatch,
  PacketSequenceDiscontinuity,
  FrameIncomplete,
  UnsupportedMutationCount,
  MissingCapability,
  DecodedTextureStateMismatch,
  NativeTileGeometryInvalid,
  NativeRevisionNotAdvanced,
  NativeReadLayoutMismatch,
  NativeReadCountMismatch,
  NativeReadTileUnknown,
  NativeReadHashMismatch,
  SurfaceNotInitialized,
  NativeAnchorBaselineUnavailable,
  CpuUploadDuplicateBegin,
  CpuUploadMissingBegin,
  CpuUploadTransactionMismatch,
  CpuUploadMetadataMismatch,
  CpuUploadBeginOutOfRange,
  CpuUploadOffsetDiscontinuity,
  CpuUploadInterleavedOperation,
  DepthNotInitialized,
  DepthEpochOverflow,
  DepthEpochDiscontinuity,
  DepthEpochMismatch,
  InvalidDepthCompare,
  PrecisionCoverageIncomplete,
};

struct ReplayCpuUploadState {
  bool active = false;
  uint64_t transactionId = UNASSIGNED_PACKET_SEQUENCE;
  NativeRect transfer;
  MaskState mask;
  uint64_t nextOffset = 0;

  bool operator==(const ReplayCpuUploadState &) const = default;
};

// State immediately preceding the batch being checked. Preflight always
// works on a copy and returns a candidate. A renderer with multiple frames in
// flight keeps a scheduled state (advanced only after vkQueueSubmit succeeds)
// as well as a confirmed state (advanced when that submission's fence
// signals); the next ordered preflight uses the scheduled state.
struct ReplayPreflightState {
  bool timelineInitialized = false;
  bool surfaceInitialized = false;
  bool depthInitialized = false;
  uint64_t depthEpoch = 0;
  uint64_t lastFrameNumber = 0;
  uint64_t contentRevision = 0;
  uint64_t completedSubmissionId = 0;
  uint64_t nextPacketSequence = 0;
  // These are the revisions installed in the ordered replay shadow, not the
  // newest revisions of the authoritative native snapshot. M0Reanchor keeps
  // these labels while replacing the complete shadow contents; a later
  // texture/CLUT read will still publish any producer-side revision advance.
  std::array<uint64_t, CRASH_REPLAY_NATIVE_TILE_COUNT> nativeTileRevisions{};
  std::bitset<CRASH_REPLAY_NATIVE_TILE_COUNT> knownNativeTiles{};
  ReplayCpuUploadState cpuUpload;

  bool operator==(const ReplayPreflightState &) const = default;
};

struct ReplayPreflightRequest {
  uint32_t schemaVersion = CRASH_REPLAY_SCHEMA_VERSION;
  std::string_view schemaName = CRASH_REPLAY_SCHEMA_NAME;
  FrameLatch expectedLatch;
  // Desired renderer policy, supplied from renderer configuration rather than
  // copied from expectedLatch. This prevents a 1x capture from being accepted
  // by a 4x consumer after a configuration or mailbox transition.
  ReplayPolicyState expectedPolicy;
  ReplayCapabilityMask capabilities = 0;
  ReplayNearClipMode nearClipMode = ReplayNearClipMode::Saturate;
  // Production GPU packets must keep trace/raw register evidence identical to
  // the decoded fields used by the recorder's conservative read-set. Tests or
  // import tools can disable this only while canonicalizing legacy captures.
  bool requireCanonicalRawState = true;
};

// Identity copied atomically with the authoritative 1024x512 native snapshot.
// Policy is deliberately absent: the consumer supplies it independently in
// ReplayNativeAnchorRequest, exactly as it does for ordinary M1 preflight.
struct ReplayNativeSnapshotIdentity {
  uint64_t frameNumber = 0;
  uint64_t contentRevision = 0;
  uint64_t completedSubmissionId = 0;
  DisplayLatchState display;
  // Strong token returned by the GPU together with the native pixels. When
  // present, anchor prefix selection uses object identity instead of scalar
  // latch fields, which may legitimately repeat across otherwise-empty
  // VBlanks. Offline/import callers may omit it and use the weaker fields.
  CrashReplayFramePtr sealedFrame;

  bool operator==(const ReplayNativeSnapshotIdentity &) const = default;
};

struct ReplayNativeAnchorRequest {
  uint32_t schemaVersion = CRASH_REPLAY_SCHEMA_VERSION;
  std::string_view schemaName = CRASH_REPLAY_SCHEMA_NAME;
  ReplayNativeSnapshotIdentity snapshot;
  ReplayPolicyState expectedPolicy;
  bool requireCanonicalRawState = true;
  // Exact native pixels can deliberately supersede an incomplete visual IR
  // prefix. This is opt-in because missing structural continuation state is
  // still unsafe for import/offline callers without a strong live token.
  bool allowIncompleteNativeReanchor = false;
};

struct ReplayPreflightCounters {
  uint64_t inputFrames = 0;
  uint64_t retainedFrames = 0;
  uint64_t examinedFrames = 0;
  uint64_t operations = 0;
  uint64_t mutations = 0;
  uint64_t drawPackets = 0;
  uint64_t nativeTileUploads = 0;
  uint64_t nativeTilesTouched = 0;
  uint64_t nativeReadPackets = 0;
  uint64_t nativeReadTiles = 0;

  bool operator==(const ReplayPreflightCounters &) const = default;
};

struct ReplayPreflightResult {
  ReplayPreflightRoute route = ReplayPreflightRoute::M0Fallback;
  ReplayPreflightReason reason = ReplayPreflightReason::None;
  ReplayCapabilityMask missingCapabilities = 0;
  ReplayPreflightCounters counters;

  // Strong references keep the exact immutable batch alive across swapchain
  // retry and submit.  They include frames only through the selected latch;
  // later FIFO entries remain outside this atomic decision.
  std::vector<CrashReplayFramePtr> frames;

  // Equal to the input state on every fallback.  It advances only when route
  // is M1Replay and still must not be committed before successful submission.
  ReplayPreflightState candidateState;
};

enum class ReplayNativeAnchorRoute : uint8_t {
  M0PresentOnly,
  M0Reanchor,
};

struct ReplayNativeAnchorResult {
  ReplayNativeAnchorRoute route = ReplayNativeAnchorRoute::M0PresentOnly;
  ReplayPreflightReason reason = ReplayPreflightReason::None;
  ReplayPreflightCounters counters;

  // The exact immutable prefix superseded by the native snapshot. The caller
  // must retain and consume it only after the M0Reanchor queue submit succeeds.
  std::vector<CrashReplayFramePtr> frames;

  // Equal to the input state on every error. On M0Reanchor it describes the
  // structural timeline after the target latch; skipped visual mutations are
  // represented by the full native snapshot rather than executed from IR.
  ReplayPreflightState candidateState;
};

// Returns the canonical ascending, duplicate-free 8x8 tile set which the
// fragment shader may read for this packet.  It intentionally covers the
// complete packed texture page and, for indexed textures, the complete CLUT.
std::vector<uint32_t>
conservativeNativeReadTileIndices(const DrawPacket &packet);

// CopyRect reads the complete wrapped source rectangle. This overload exposes
// the same canonical source-tile set used by producer proof generation and
// preflight validation.
std::vector<uint32_t>
conservativeNativeReadTileIndices(const CopyRect &copy);

ReplayPreflightResult
preflightCrashReplay(std::span<const CrashReplayFramePtr> fifoFrames,
                     const ReplayPreflightRequest &request,
                     const ReplayPreflightState &confirmedState = {});

// Derives the metadata paired with an exact authoritative native snapshot.
// Unlike M1 preflight it assumes every executor capability is available, but
// it preserves schema, policy, timeline, A0, depth, native revision and
// texture/CLUT proof validation. By default capture must be complete. A live
// caller may opt into whole-prefix replacement of incomplete visual raster IR
// only with the exact strong snapshot token; structural/state gaps remain
// unrecoverable. No state is committed by this function.
ReplayNativeAnchorResult
deriveNativeSnapshotAnchor(std::span<const CrashReplayFramePtr> fifoFrames,
                           const ReplayNativeAnchorRequest &request,
                           const ReplayPreflightState &scheduledState = {});

std::string_view replayCapabilityName(ReplayCapability capability);
std::string_view replayPreflightReasonName(ReplayPreflightReason reason);

} // namespace ps1::gpu::replay
