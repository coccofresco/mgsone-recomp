#pragma once

#include "runtime/gpu/replay_limits.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace ps1::gpu::replay {

// CrashReplayV1 is the stable boundary between the single-writer PS1 command
// processor and an accelerated renderer.  The IR contains no Vulkan handles
// and records every state value required by a draw, so consumers never need to
// observe the mutable GP0 register file.
// Schema 9 admits authenticated GP0(68) points and fixed two-endpoint lines
// lowered to four-vertex Projected3D draws. Unprojected pixels stay native.
inline constexpr uint32_t CRASH_REPLAY_SCHEMA_VERSION = 9;
inline constexpr std::string_view CRASH_REPLAY_SCHEMA_NAME = "CrashReplayV1";
// Raw GTE W uses 32768 as normalized 1.0.  Donor-compatible invalid-W
// handling replaces every vertex in the packet with this exact value.
inline constexpr double CRASH_REPLAY_FALLBACK_W = 32768.0;
inline constexpr uint64_t UNASSIGNED_PACKET_SEQUENCE = UINT64_MAX;

struct NativePoint {
  int32_t x = 0;
  int32_t y = 0;

  bool operator==(const NativePoint &) const = default;
};

// Width and height are kept separate from the origin because VRAM operations
// may wrap at the PS1 VRAM boundary.  A producer must not pre-clip them.
struct NativeRect {
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;

  bool operator==(const NativeRect &) const = default;
};

struct Rgb8 {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  bool operator==(const Rgb8 &) const = default;
};

struct DrawArea {
  // Inclusive native-pixel bounds, matching GP0(E3/E4).
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 1023;
  int32_t bottom = 511;

  bool operator==(const DrawArea &) const = default;
};

struct DrawOffset {
  int32_t x = 0;
  int32_t y = 0;

  bool operator==(const DrawOffset &) const = default;
};

enum class TextureDepth : uint8_t {
  Indexed4,
  Indexed8,
  Direct15,
  Reserved,
};

struct TexturePageState {
  // Decoded fields are supplied for the fast path; raw preserves every guest
  // bit for traces and future schema-compatible validation.
  uint16_t raw = 0;
  uint16_t baseX = 0;
  uint16_t baseY = 0;
  TextureDepth depth = TextureDepth::Indexed4;
  bool xFlip = false;
  bool yFlip = false;
  bool textureDisable = false;

  bool operator==(const TexturePageState &) const = default;
};

struct ClutState {
  uint16_t raw = 0;
  uint16_t x = 0;
  uint16_t y = 0;

  bool operator==(const ClutState &) const = default;
};

struct TextureWindowState {
  // raw preserves the packed five-bit GP0(E2) fields.  The decoded values are
  // already expanded to native texel bits (multiples of eight, range 0..248)
  // so every consumer applies the same mask without decoding mutable state.
  uint32_t raw = 0;
  uint8_t maskX = 0;
  uint8_t maskY = 0;
  uint8_t offsetX = 0;
  uint8_t offsetY = 0;

  bool operator==(const TextureWindowState &) const = default;
};

// Packet-wide donor UV correction.  ReplayVertex::u/v already include the
// offset below; the bounds clamp perspective reconstruction before the PS1
// sampler performs its architectural eight-bit wrap and texture window.
struct TextureUvDomainState {
  double minU = 0.0;
  double minV = 0.0;
  double maxU = 255.0;
  double maxV = 255.0;
  uint16_t offsetU = 0;
  uint16_t offsetV = 0;
  bool enabled = false;
  bool flippedU = false;
  bool flippedV = false;

  bool operator==(const TextureUvDomainState &) const = default;
};

enum class BlendMode : uint8_t {
  HalfBackgroundPlusHalfForeground,
  BackgroundPlusForeground,
  BackgroundMinusForeground,
  BackgroundPlusQuarterForeground,
};

struct BlendState {
  BlendMode mode = BlendMode::HalfBackgroundPlusHalfForeground;
  bool enabled = false;
  // Textured PS1 polygons blend only texels carrying STP.  Untextured draws
  // instead blend the whole primitive.
  bool texturedStpOnly = false;

  bool operator==(const BlendState &) const = default;
};

struct MaskState {
  bool forceMaskBit = false;
  bool checkMaskBit = false;

  bool operator==(const MaskState &) const = default;
};

enum class DepthCompare : uint8_t {
  Always,
  Less,
  LessOrEqual,
};

// Capture policy values which affect baked DrawPacket data. They are carried
// by every latch and matched independently against the consumer configuration;
// deriving the expected policy from the producer frame would make the check
// tautological.
enum class ReplayDepthMode : uint8_t {
  Off,
  Hybrid,
  Strict,
};

struct ReplayPolicyState {
  uint32_t internalScale = 1;
  bool pgxp = false;
  bool trueColor = false;
  ReplayDepthMode trueDepthMode = ReplayDepthMode::Off;

  bool operator==(const ReplayPolicyState &) const = default;
};

struct DepthState {
  bool testEnabled = false;
  bool writeEnabled = false;
  DepthCompare compare = DepthCompare::Always;
  // Epoch changes provide O(1) logical depth clears in the accelerated
  // backend.  Zero is reserved for depth-disabled/invalid samples.
  uint64_t epoch = 0;
  // Hybrid opaque Projected3D uses no-test+write+Always: the ordering table
  // owns inter-packet visibility, while genuine W is retained in D32. Ordered
  // fallback, Screen2D and native writes instead invalidate depth only for
  // samples which pass texture-zero/mask testing and are actually written.
  bool invalidateOnAccepted = false;

  bool operator==(const DepthState &) const = default;
};

// A complete snapshot of state that affects one draw packet.  Keeping this in
// the packet (rather than emitting mutable state commands) makes packets
// independently replayable and prevents a producer/consumer race from
// changing draw semantics.
struct BakedDrawState {
  DrawArea drawArea;
  DrawOffset drawOffset;
  TexturePageState texturePage;
  ClutState clut;
  TextureWindowState textureWindow;
  TextureUvDomainState textureUvDomain;
  BlendState blend;
  MaskState mask;
  DepthState depth;
  bool dither = false;
  bool drawToDisplayArea = false;

  bool operator==(const BakedDrawState &) const = default;
};

// Exact categorical evidence carried beside a replay vertex. None of these
// fields is inferred from screen position, colour, W, motion or an expected
// number of depth families. projectionId is a process-local event join; the
// writer/source lineages are the cross-frame evidence from which a later
// certifier may construct stable object identities.
enum ReplayVertexProvenanceEvidence : uint8_t {
  ReplayVertexProjectionEventExact = 1u << 0,
  ReplayVertexTransformLineageExact = 1u << 1,
  ReplayVertexModelSourceExact = 1u << 2,
  ReplayVertexGp0WordSourceExact = 1u << 3,
  ReplayVertexDmaTraversalExact = 1u << 4,
  // Complete, value-independent Merkle DAG fingerprints captured by explicit
  // value-flow instrumentation. This is construction-path evidence, not a
  // fitted family or a stable object identity.
  ReplayVertexModelValueFlowExact = 1u << 5,
  // Exact last zero-word DMA link before this packet. It is transport
  // evidence only: generic replay does not assume that every such link is an
  // ordering-table bucket or infer a depth scale from its address.
  ReplayVertexOrderingTableAnchorExact = 1u << 6,
};

inline constexpr uint8_t REPLAY_VERTEX_PROVENANCE_KNOWN_EVIDENCE =
    ReplayVertexProjectionEventExact | ReplayVertexTransformLineageExact |
    ReplayVertexModelSourceExact | ReplayVertexGp0WordSourceExact |
    ReplayVertexDmaTraversalExact | ReplayVertexModelValueFlowExact |
    ReplayVertexOrderingTableAnchorExact;

struct ReplayVertexProvenance {
  uint64_t projectionId = 0;
  uint64_t projectionContext = 0;
  // Stable, game-owned GTE callsite/lane identity. Unlike gameProfileTag it
  // contains no live register or pointer values and can therefore be compared
  // across frames. Generic replay identity and certification ignore it.
  uint64_t gameProfileCallsite = 0;
  // Opaque identifier owned by an explicitly selected per-game profile. It
  // is never part of the generic visibility-manifest key.
  uint64_t gameProfileTag = 0;
  // Numeric transform values are retained only as a same-frame equality
  // witness. They are excluded from persistent triangle identity.
  uint64_t transformValueFingerprint = 0;
  uint64_t transformLineage = 0;
  // Address source tokens are guestAddress+1, leaving zero as unavailable.
  uint64_t modelVertexSource = 0;
  uint64_t modelValueFlowSourceFingerprint = 0;
  uint64_t modelValueFlowProgramFingerprint = 0;
  uint64_t gp0WordSource = 0;
  uint64_t dmaListSource = 0;
  uint64_t dmaNodeSource = 0;
  uint64_t dmaOrderingTableAnchorSource = 0;
  uint32_t dmaTraversalOrdinal = 0;
  uint8_t evidence = 0;

  bool has(uint8_t required) const noexcept {
    return (evidence & required) == required;
  }

  bool operator==(const ReplayVertexProvenance &) const = default;
};

// Immutable per-triangle owner descriptor. It retains all three exact vertex
// records even when they disagree, so incoherence remains diagnosable instead
// of being collapsed into a guessed family. A valid triangle with no intrinsic
// evidence is distinct from invalid topology and simply cannot be certified.
struct ReplayTriangleProvenance {
  std::array<uint8_t, 3> vertexIndices{};
  std::array<ReplayVertexProvenance, 3> vertices{};
  bool validTopology = false;

  uint8_t completeEvidence() const noexcept {
    if (!validTopology)
      return 0;
    return vertices[0].evidence & vertices[1].evidence & vertices[2].evidence;
  }

  bool coherentProjectionContext() const noexcept {
    return (completeEvidence() & ReplayVertexProjectionEventExact) != 0 &&
           vertices[0].projectionContext == vertices[1].projectionContext &&
           vertices[1].projectionContext == vertices[2].projectionContext;
  }

  bool coherentTransformLineage() const noexcept {
    return (completeEvidence() & ReplayVertexTransformLineageExact) != 0 &&
           vertices[0].transformLineage == vertices[1].transformLineage &&
           vertices[1].transformLineage == vertices[2].transformLineage;
  }

  bool coherentTransformValue() const noexcept {
    return (completeEvidence() & ReplayVertexProjectionEventExact) != 0 &&
           vertices[0].transformValueFingerprint ==
               vertices[1].transformValueFingerprint &&
           vertices[1].transformValueFingerprint ==
               vertices[2].transformValueFingerprint;
  }

  bool coherentDmaTraversal() const noexcept {
    return (completeEvidence() & ReplayVertexDmaTraversalExact) != 0 &&
           vertices[0].dmaListSource == vertices[1].dmaListSource &&
           vertices[1].dmaListSource == vertices[2].dmaListSource &&
           vertices[0].dmaNodeSource == vertices[1].dmaNodeSource &&
           vertices[1].dmaNodeSource == vertices[2].dmaNodeSource &&
           vertices[0].dmaTraversalOrdinal == vertices[1].dmaTraversalOrdinal &&
           vertices[1].dmaTraversalOrdinal == vertices[2].dmaTraversalOrdinal;
  }

  bool coherentOrderingTableAnchor() const noexcept {
    return (completeEvidence() & ReplayVertexOrderingTableAnchorExact) != 0 &&
           vertices[0].dmaOrderingTableAnchorSource ==
               vertices[1].dmaOrderingTableAnchorSource &&
           vertices[1].dmaOrderingTableAnchorSource ==
               vertices[2].dmaOrderingTableAnchorSource;
  }

  bool operator==(const ReplayTriangleProvenance &) const = default;
};

struct ReplayVertex {
  enum class CoordinateSpace : uint8_t {
    PostDrawOffsetNativePixels,
  };

  // Guest integer coordinate after drawOffset is applied, exactly matching
  // GPU::decodeVertex().  Consumers must never add BakedDrawState::drawOffset
  // again; it is retained only as packet provenance/state evidence.
  int32_t nativeX = 0;
  int32_t nativeY = 0;

  // Native-pixel-space high precision attributes.  x/y are also explicitly
  // post-drawOffset; u/v are unwrapped and already include the packet-wide
  // TextureUvDomainState offset.  w is the finite positive effective raw GTE
  // W after the configured near-plane policy, where 32768 represents
  // normalized 1.0 (it is not reciprocal/normalized).
  double x = 0.0;
  double y = 0.0;
  double u = 0.0;
  double v = 0.0;
  // Effective W consumed by the donor-compatible raster path. rawDepth keeps
  // the genuine GTE Z even when near-plane saturation changes this value.
  double w = CRASH_REPLAY_FALLBACK_W;
  double rawDepth = 0.0;
  double nearDepth = 0.0;
  // Genuine homogeneous numerators, already adjusted into the same
  // post-drawOffset coordinate space as x/y.
  double qx = 0.0;
  double qy = 0.0;
  Rgb8 color;
  ReplayVertexProvenance provenance;

  bool preciseXY = false;
  bool preciseUV = false;
  bool preciseX = false;
  bool preciseY = false;
  bool rawDepthValid = false;
  bool homogeneousQ = false;
  bool reconciledX = false;
  bool reconciledY = false;
  bool inventedW = false;
  bool nearSaturated = false;
  CoordinateSpace coordinateSpace = CoordinateSpace::PostDrawOffsetNativePixels;

  bool operator==(const ReplayVertex &) const = default;
};

struct NativeTileUpload {
  NativeRect destination;
  // Tightly packed PS1 ABGR1555 words.  This synchronizes authoritative
  // native tiles required by texture/CLUT feedback before a later draw.
  std::vector<uint16_t> pixels;
  uint64_t nativeRevision = 0;

  bool operator==(const NativeTileUpload &) const = default;
};

inline constexpr uint32_t CRASH_REPLAY_NATIVE_TILE_WIDTH = 8;
inline constexpr uint32_t CRASH_REPLAY_NATIVE_TILE_HEIGHT = 8;
inline constexpr uint64_t CRASH_REPLAY_READ_HASH_OFFSET =
    UINT64_C(14695981039346656037);

// Strong proof that the renderer's ordered native shadow contains every tile
// needed by this packet.  The producer and consumer hash the conservative
// texture-page/CLUT read set as ordered (tileIndex, nativeRevision) pairs.
// A missing, stale or reordered upload therefore fails preflight before any
// M1 command is recorded and selects the whole-frame M0 fallback.
struct NativeReadDependency {
  uint64_t versionHash = CRASH_REPLAY_READ_HASH_OFFSET;
  uint32_t tileCount = 0;
  uint16_t tileWidth = CRASH_REPLAY_NATIVE_TILE_WIDTH;
  uint16_t tileHeight = CRASH_REPLAY_NATIVE_TILE_HEIGHT;
  bool required = false;

  bool operator==(const NativeReadDependency &) const = default;
};

uint64_t appendNativeReadDependencyHash(uint64_t hash, uint32_t tileIndex,
                                        uint64_t nativeRevision);

// Seed/load is intentionally distinct from GP1(00) reset: it establishes a
// new authoritative surface baseline, while Reset below never clears VRAM.
struct SurfaceSeed {
  NativeRect destination;
  std::vector<uint16_t> pixels;
  uint64_t nativeRevision = 0;
  uint64_t depthEpoch = 1;
  bool replaceWholeSurface = true;

  bool operator==(const SurfaceSeed &) const = default;
};

enum class PacketRoute : uint8_t {
  Projected3D,
  Screen2D,
};

enum class PacketSemanticReason : uint8_t {
  PolygonDefault,
  Intrinsic2DFamily,
  DisplayAnchoredComposite,
};

// Non-None is an explicit completeness/precision gate.  It records why a
// projected packet did not take the ordinary fully precise route even when a
// donor-compatible packet-atomic path can still render it.
enum class PacketFallbackReason : uint8_t {
  None,
  InvalidWPacketAtomic,
  MissingPrecisionPacketAtomic,
  ProjectiveContextMismatch,
  RasterSetupRejected,
  UnsupportedFeature,
};

// These values are part of the CrashReplayV1 shader ABI.  Keep them identical
// to replay_precise.frag; renderer packing uses shaderDrawFlags() below rather
// than open-coded magic numbers.
enum class ReplayDrawFlag : uint32_t {
  Textured = 1u << 0,
  RawTexture = 1u << 1,
  SemiTransparent = 1u << 2,
  ForceMask = 1u << 3,
  CheckMask = 1u << 4,
  LimitUvDomain = 1u << 5,
  // The packet carries genuine positive projective W for every active
  // vertex. The Vulkan surface-depth color plane may therefore record it even
  // when painter composition deliberately invalidates the occlusion D32.
  SurfaceDepthValid = 1u << 6,
};

constexpr uint32_t replayDrawFlagBit(ReplayDrawFlag flag) {
  return static_cast<uint32_t>(flag);
}

// Original-asset geometry consumed by the MGS Vulkan bridge. Offsets address
// the immutable storage buffer in bytes; bit 31 selects signed packed i16 XYZ.
// Per-corner transforms also represent rigid parent seams without skin weights.
struct OriginalTexture {
  std::array<uint32_t, 4>
      data{}; // index byte offset, palette byte offset, width, height
  std::array<int32_t, 4> origin{};   // page-relative UV origin, mode (1=static,
                                     // 2=dynamic palette), reserved
  std::array<uint32_t, 8> palette{}; // 16 live palette words, two per uint
  bool operator==(const OriginalTexture &) const = default;
};
struct OriginalMeshGeometry {
  uint32_t textureId = ~0u; // live logical KMD material ID, for PCX provenance
  bool sourceRecovery =
      false; // host-only draw without a native GP0 counterpart
  std::array<std::array<float, 4>, 4> weights{
      {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
  std::array<uint32_t, 4> pointBytes{};
  std::array<std::array<float, 4>, 12> viewRows{};
  std::array<float, 4> projection{};
  bool operator==(const OriginalMeshGeometry &) const = default;
};

struct DrawPacket {
  static constexpr std::size_t MAX_VERTICES = 4;
  static constexpr std::size_t MAX_INDICES = 6;

  uint8_t opcode = 0xFF;
  PacketRoute route = PacketRoute::Projected3D;
  PacketSemanticReason semanticReason = PacketSemanticReason::PolygonDefault;
  PacketFallbackReason fallbackReason = PacketFallbackReason::None;
  uint64_t packetSequence = UNASSIGNED_PACKET_SEQUENCE;
  uint64_t submissionId = 0;
  // Genuine XYZ/W provenance, one bit per active vertex.  If this is not the
  // full active mask, every active vertex.w must already contain the
  // packet-atomic CRASH_REPLAY_FALLBACK_W value.
  uint8_t validMask = 0;
  NativeReadDependency nativeRead;
  BakedDrawState state;
  std::array<ReplayVertex, MAX_VERTICES> vertices{};
  std::array<uint8_t, MAX_INDICES> triangleIndices{};
  uint8_t vertexCount = 0;
  uint8_t indexCount = 0;
  // Effective raster state, not the GP0 packet layout. A textured-family
  // opcode remains in `opcode` after the GP1(09)-gated E1 texture-disable bit
  // turns its raster semantics into an untextured primitive.
  bool textured = false;
  bool gouraud = false;
  bool rawTexture = false;
  std::optional<OriginalMeshGeometry> originalMesh;
  std::optional<OriginalTexture> originalTexture;

  bool operator==(const DrawPacket &) const = default;
};

uint32_t shaderDrawFlags(const DrawPacket &packet);

// triangleIndex is zero-based in the packet's expanded index stream. Invalid
// topology returns validTopology=false; missing provenance on a valid triangle
// remains a valid, explicitly uncertifiable descriptor.
ReplayTriangleProvenance
captureReplayTriangleProvenance(const DrawPacket &packet,
                                uint8_t triangleIndex) noexcept;

struct FillRect {
  NativeRect destination;
  Rgb8 color;
  // Fill semantics are native-first, but retaining the raw word prevents a
  // true-colour backend from having to re-quantize before updating feedback.
  uint16_t nativeColor = 0;

  bool operator==(const FillRect &) const = default;
};

struct CopyRect {
  NativePoint source;
  NativePoint destination;
  uint32_t width = 0;
  uint32_t height = 0;
  // CopyRect is itself a native reader. The producer snapshots every source
  // tile whose authoritative revision is not already queued, then hashes the
  // complete wrapped source set here. Consumers must reject an absent or stale
  // proof before executing any part of the batch.
  NativeReadDependency nativeRead;
  MaskState mask;

  bool operator==(const CopyRect &) const = default;
};

enum class CpuUploadPhase : uint8_t {
  Begin,
  Data,
  Complete,
  Abort,
};

struct CpuUpload {
  // The A0 header's packet sequence identifies one transaction across frame
  // latches. Begin and exactly one terminal phase carry no pixel payload;
  // Data chunks retain the accepted-write bitmap described below.
  uint64_t transactionId = UNASSIGNED_PACKET_SEQUENCE;
  CpuUploadPhase phase = CpuUploadPhase::Begin;
  // Full A0 transfer geometry, not merely this flush's bounding rectangle.
  // pixelOffset locates pixels[0] in row-major transfer order, allowing a
  // flush to begin/end in the middle of a row without inventing padding.
  NativeRect transfer;
  uint64_t pixelOffset = 0;
  std::vector<uint16_t> pixels;
  // LSB-first, one bit per consumed source pixel. Rejected mask-test writes
  // remain in pixels with a clear accepted bit, preserving stream position.
  std::vector<uint64_t> acceptedBits;
  MaskState mask;

  static std::size_t acceptedWordCount(std::size_t pixelCount);
  bool acceptedAt(std::size_t chunkIndex) const;
  NativePoint destinationAt(std::size_t chunkIndex) const;

  bool operator==(const CpuUpload &) const = default;
};

struct PaintedPixel {
  NativePoint destination;
  uint16_t nativeColor = 0;
  Rgb8 color;
  bool sourceMaskBit = false;

  bool operator==(const PaintedPixel &) const = default;
};

// Sparse/compatibility writes remain one ordered operation.  A renderer may
// batch their execution internally, but must not interleave another command.
struct PaintPixels {
  // Only writes already accepted by the authoritative native raster appear
  // here. mask is retained as provenance; consumers must not repeat the E6
  // destination test. blend + per-pixel sourceMaskBit retain enough source
  // information for a true-colour executor instead of freezing RGB555.
  std::vector<PaintedPixel> pixels;
  BlendState blend;
  MaskState mask;
  DepthState depth;
  bool dither = false;

  bool operator==(const PaintPixels &) const = default;
};

// Allocation-free representation for GP0(68h) and coalesced adjacent points
// with identical color/state.  It prevents one heap payload/variant per point.
struct PaintRect {
  NativeRect destination;
  uint16_t nativeColor = 0;
  Rgb8 color;
  MaskState mask;
  DepthState depth;
  uint64_t coalescedCommandCount = 1;

  bool operator==(const PaintRect &) const = default;
};

struct ClearDepth {
  bool wholeSurface = true;
  NativeRect area;
  uint64_t nextEpoch = 1;

  bool operator==(const ClearDepth &) const = default;
};

struct Reset {
  // GP1(00): reset guest draw state and the host depth interpretation only.
  // Native/modern color and the native mask bit are deliberately preserved.
  bool resetDrawState = true;
  bool clearDepth = true;
  uint64_t nextDepthEpoch = 1;

  bool operator==(const Reset &) const = default;
};

struct DisplayLatchState {
  NativeRect aperture;
  bool enabled = true;
  bool pal = false;
  bool interlaced = false;
  bool oddField = false;

  bool operator==(const DisplayLatchState &) const = default;
};

// FrameLatch is terminal.  It identifies the complete ordered mutation stream
// whose result may be presented and supplies already-decoded scanout state.
struct FrameLatch {
  uint64_t frameNumber = 0;
  uint64_t contentRevision = 0;
  uint64_t completedSubmissionId = 0;
  // Half-open DrawPacket sequence range [sequenceBegin, sequenceEnd).
  uint64_t sequenceBegin = 0;
  uint64_t sequenceEnd = 0;
  // complete is true only when every mutation in the range has an IR
  // representation. Unsupported commands must increment unsupportedCount.
  uint64_t unsupportedCount = 0;
  // Subset of unsupportedCount which an exact native color/mask snapshot
  // cannot safely dominate (for example unknown state or transfer framing).
  // Visual raster commands may be recoverable by whole-prefix reanchor.
  uint64_t unrecoverableCount = 0;
  bool complete = true;
  ReplayPolicyState policy;
  DisplayLatchState display;

  bool operator==(const FrameLatch &) const = default;
};

using Operation = std::variant<NativeTileUpload, SurfaceSeed, DrawPacket,
                               FillRect, CopyRect, CpuUpload, PaintPixels,
                               PaintRect, ClearDepth, Reset, FrameLatch>;

class CrashReplayFrame final {
public:
  static constexpr uint32_t schemaVersion() {
    return CRASH_REPLAY_SCHEMA_VERSION;
  }
  static constexpr std::string_view schemaName() {
    return CRASH_REPLAY_SCHEMA_NAME;
  }

  CrashReplayFrame(const CrashReplayFrame &) = delete;
  CrashReplayFrame &operator=(const CrashReplayFrame &) = delete;

  std::span<const Operation> operations() const { return operations_; }
  std::size_t mutationCount() const { return operations_.size() - 1; }
  const FrameLatch &latch() const;

private:
  friend class CrashReplayFrameBuilder;
  explicit CrashReplayFrame(std::vector<Operation> operations);

  std::vector<Operation> operations_;
};

using CrashReplayFramePtr = std::shared_ptr<const CrashReplayFrame>;

// Single-writer frame builder.  Every record() validates and appends exactly
// one complete operation; validation failures leave the frame unchanged.
class CrashReplayFrameBuilder final {
public:
  CrashReplayFrameBuilder() = default;

  void record(NativeTileUpload operation);
  void record(SurfaceSeed operation);
  void record(DrawPacket operation);
  void record(FillRect operation);
  void record(CopyRect operation);
  void record(CpuUpload operation);
  void record(PaintPixels operation);
  void record(PaintRect operation);
  void record(ClearDepth operation);
  void record(Reset operation);

  CrashReplayFramePtr seal(FrameLatch latch);

  bool isSealed() const { return sealed_; }
  std::size_t mutationCount() const { return operations_.size(); }

private:
  void ensureOpen() const;
  void append(Operation operation);

  std::vector<Operation> operations_;
  bool hasPacketSequence_ = false;
  uint64_t firstPacketSequence_ = 0;
  uint64_t lastPacketSequence_ = 0;
  bool sealed_ = false;
};

// An unbounded FIFO mailbox is deliberate: replacing an older frame with a
// newer one would also discard ordered VRAM mutations needed by texture and
// CLUT feedback.  Consumers may coalesce presentation after replay, never the
// mutation stream itself.
class CrashReplayMailbox final {
public:
  CrashReplayMailbox() = default;
  CrashReplayMailbox(const CrashReplayMailbox &) = delete;
  CrashReplayMailbox &operator=(const CrashReplayMailbox &) = delete;

  void publish(CrashReplayFramePtr frame);
  CrashReplayFramePtr tryConsume();
  CrashReplayFramePtr waitConsume();
  CrashReplayFramePtr waitConsumeFor(std::chrono::milliseconds timeout);

  // Closing wakes waiters.  Already-published frames remain consumable.
  void close();
  bool isClosed() const;
  std::size_t pendingFrameCount() const;

private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<CrashReplayFramePtr> frames_;
  bool closed_ = false;
};

} // namespace ps1::gpu::replay
