#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace ps1 {

class Memory;

// Process-lifetime switch for the parallel high-precision/provenance lane.
// The host resolves it before starting guest execution. Architectural CPU,
// GTE and GPU state remain active when it is false; only sidecar work which a
// native 1x diagnostic presentation cannot consume is bypassed.
extern bool g_pgxpTrackingEnabled;

[[gnu::always_inline]] inline bool pgxpTrackingEnabled() noexcept {
  return g_pgxpTrackingEnabled;
}

void configurePGXPTracking(bool enabled) noexcept;

// Diagnostic provenance carried with every PGXP value. X/Y/Z and homogeneous
// Qx/Qy/W remain valid only when their flags and packed guest word agree.
enum class PGXPOrigin : uint8_t {
  Unknown,
  GteProjection,
};

enum class PGXPRamLookup : uint8_t {
  NotApplicable,
  Unobserved,
  NoSidecar,
  Empty,
  Stale,
  HitXY,
  HitXYZ,
  HitSignedProjection,
  Count,
};

enum class PGXPSubmissionPath : uint8_t {
  DirectMmio,
  HleGenerated,
  HleOrderingTable,
  DmaBlock,
  DmaLinkedList,
  Bios,
  Count,
};

// Stable numeric reason emitted by the opt-in pgxp.vertex_reject trace.
// DeltaX/DeltaY are retained as schema values for existing trace consumers;
// current code reconciles unsafe screen components to their packed values and
// reports that separately rather than rejecting otherwise valid depth.
enum class PGXPVertexRejectReason : uint8_t {
  None,
  NoXY,
  Stale,
  XYOnly,
  NonFinite,
  NonPositiveZ,
  DeltaX,
  DeltaY,
  Count,
};

// Where one resolved screen component came from. A usable projection may mix
// a high-precision component with its packed/native counterpart while keeping
// the projection's valid Z. This is safer than switching an entire polygon
// between modern and compatibility raster paths because one side-channel
// component is implausible.
enum class PGXPCoordinateSource : uint8_t {
  Unavailable,
  Precise,
  NativeNonFinite,
  NativeDelta,
  NativeGuardBand,
  // Authentic GTE SXY retained while positive W is below the divider's near
  // threshold. Q/W may be far outside the screen here by construction, but
  // the donor saturation lane consumes the finite clamped SXY, not Q/W.
  PreciseGteSaturated,
};

inline constexpr bool
isAuthenticPGXPCoordinateSource(PGXPCoordinateSource source) noexcept {
  return source == PGXPCoordinateSource::Precise ||
         source == PGXPCoordinateSource::PreciseGteSaturated;
}

inline constexpr float PGXP_SCREEN_DELTA_LIMIT = 8.0f;
inline constexpr double PGXP_SIGNED_PROJECTION_GUARD_BAND = 8192.0;

// Side-channel value propagated alongside a packed PS1 word. The integer
// value is part of the validity contract: reusing an address or register with
// different guest data must fall back to the native affine coordinate rather
// than attaching stale precision to a new primitive.
struct PGXPValue {
  enum : uint8_t {
    VALID_X = 1u << 0,
    VALID_Y = 1u << 1,
    VALID_XY = VALID_X | VALID_Y,
    VALID_Z = 1u << 2,
    VALID_QX = 1u << 3,
    VALID_QY = 1u << 4,
    VALID_SIGNED_PROJECTION = VALID_QX | VALID_QY,
    // These bits certify intrinsic guest provenance attached at the GTE
    // projection. They are deliberately independent from numerical validity:
    // a projected operand may retain an exact writer/source lineage even when
    // its signed W later selects a compatibility presentation path.
    VALID_TRANSFORM_LINEAGE = 1u << 5,
    VALID_MODEL_VERTEX_SOURCE = 1u << 6,
    VALID_MODEL_VALUE_FLOW = 1u << 7,
    VALID_INTRINSIC_PROVENANCE = VALID_TRANSFORM_LINEAGE |
                                 VALID_MODEL_VERTEX_SOURCE |
                                 VALID_MODEL_VALUE_FLOW,
  };

  float x = 0.0f;
  float y = 0.0f;
  double z = 0.0;
  uint32_t value = 0;
  uint8_t flags = 0;
  PGXPOrigin origin = PGXPOrigin::Unknown;
  PGXPRamLookup ramLookup = PGXPRamLookup::NotApplicable;
  PGXPSubmissionPath submissionPath = PGXPSubmissionPath::DirectMmio;
  // Homogeneous screen-space projection. For non-zero Z, qx/z and qy/z are
  // the unclamped projected coordinates. Unlike ordinary raster depth this
  // provenance remains meaningful on and behind the camera plane, allowing a
  // later modern replay stage to clip against nearDepth without inventing Z.
  double qx = 0.0;
  double qy = 0.0;
  double nearDepth = 0.0;
  uint64_t projectionId = 0;
  uint64_t projectionContext = 0;
  // Crash 3-only stable construction identity. It carries the exact GTE
  // instruction PC, RTPS/RTPT vector lane and the renderer's own coordinate
  // normalization shift. It is deliberately separate from generic
  // provenance and remains zero for every other game profile.
  uint64_t gameProfileCallsite = 0;
  // Exact value fingerprint of the 3x3 rotation and translation consumed by
  // this GTE projection. It is a frame-local equality witness only; unlike
  // projectionTransformLineage it must never become persistent identity.
  uint64_t projectionTransformValueFingerprint = 0;
  // Categorical identity evidence, never a fitted presentation parameter.
  // transformLineage fingerprints the exact guest writers/sources of RT/TR;
  // modelVertexSource is guestAddress+1 for a proven contiguous VXY/VZ load.
  // Model value-flow fingerprints cover a complete computed VXY/VZ DAG and
  // are available only in explicitly instrumented runs. Zero remains
  // representable for hashes and is disambiguated by flags.
  uint64_t projectionTransformLineage = 0;
  uint64_t modelVertexSource = 0;
  uint64_t modelValueFlowSourceFingerprint = 0;
  uint64_t modelValueFlowProgramFingerprint = 0;

  [[gnu::always_inline]] bool hasX(uint32_t currentValue) const {
    return (flags & VALID_X) != 0 &&
           static_cast<uint16_t>(value) == static_cast<uint16_t>(currentValue);
  }
  [[gnu::always_inline]] bool hasY(uint32_t currentValue) const {
    return (flags & VALID_Y) != 0 &&
           static_cast<uint16_t>(value >> 16) ==
               static_cast<uint16_t>(currentValue >> 16);
  }
  [[gnu::always_inline]] bool hasXY(uint32_t currentValue) const {
    return hasX(currentValue) && hasY(currentValue);
  }
  bool hasXYZ(uint32_t currentValue) const {
    return hasXY(currentValue) && (flags & VALID_Z) != 0 && z > 0.0f;
  }
  bool hasAuthenticProjectionProvenance() const {
    // W remains signed so zero/behind-camera GTE results can reach the
    // selected clipping/saturation policy. The positive near plane and GTE
    // origin authenticate Q; projectionContext is a consistency fingerprint
    // whose FNV producer does not reserve zero as an invalid value.
    return origin == PGXPOrigin::GteProjection && projectionId != 0 &&
           std::isfinite(z) && std::isfinite(nearDepth) && nearDepth > 0.0;
  }
  bool hasProjectiveX(uint32_t currentValue) const {
    return hasX(currentValue) && (flags & VALID_QX) != 0 &&
           hasAuthenticProjectionProvenance() && std::isfinite(qx);
  }
  bool hasProjectiveY(uint32_t currentValue) const {
    return hasY(currentValue) && (flags & VALID_QY) != 0 &&
           hasAuthenticProjectionProvenance() && std::isfinite(qy);
  }
  bool hasSignedProjection(uint32_t currentValue) const {
    return hasProjectiveX(currentValue) && hasProjectiveY(currentValue);
  }
  bool hasCompleteProjection(uint32_t currentValue) const {
    // Preserve compatibility with callers/tests that construct legacy
    // positive-depth PGXPValue objects without homogeneous provenance.
    return hasXYZ(currentValue) || hasSignedProjection(currentValue);
  }
  [[gnu::always_inline]] void invalidate() { flags = 0; }
};

static_assert(sizeof(PGXPValue) == 112,
              "PGXPValue layout is shared by CPU, RAM and GP0 sidecars");

struct PGXPResolvedVertex {
  enum : uint8_t {
    RECONCILED_X = 1u << 0,
    RECONCILED_Y = 1u << 1,
  };

  float x = 0.0f;
  float y = 0.0f;
  double z = 0.0;
  double qx = 0.0;
  double qy = 0.0;
  double nearDepth = 0.0;
  uint64_t projectionId = 0;
  uint64_t projectionContext = 0;
  uint64_t gameProfileCallsite = 0;
  uint64_t projectionTransformValueFingerprint = 0;
  uint64_t projectionTransformLineage = 0;
  uint64_t modelVertexSource = 0;
  uint64_t modelValueFlowSourceFingerprint = 0;
  uint64_t modelValueFlowProgramFingerprint = 0;
  uint8_t intrinsicProvenance = 0;
  bool hasSignedProjection = false;
  bool hasDepth = false;
  PGXPCoordinateSource xSource = PGXPCoordinateSource::Unavailable;
  PGXPCoordinateSource ySource = PGXPCoordinateSource::Unavailable;

  bool hasScreenPosition() const {
    return xSource != PGXPCoordinateSource::Unavailable &&
           ySource != PGXPCoordinateSource::Unavailable;
  }

  uint8_t reconciledMask() const {
    uint8_t mask = 0;
    if (xSource == PGXPCoordinateSource::NativeNonFinite ||
        xSource == PGXPCoordinateSource::NativeDelta ||
        xSource == PGXPCoordinateSource::NativeGuardBand)
      mask |= RECONCILED_X;
    if (ySource == PGXPCoordinateSource::NativeNonFinite ||
        ySource == PGXPCoordinateSource::NativeDelta ||
        ySource == PGXPCoordinateSource::NativeGuardBand)
      mask |= RECONCILED_Y;
    return mask;
  }
};

// Derive a projected screen-space point which the guest constructs by adding
// integer X/Y offsets to one authentic GTE projection. The architectural
// packed word remains exactly `derivedPacked`; only the parallel PGXP sidecar
// is translated. Homogeneous Q follows the same offset (Q += delta * W), so
// every derived point retains the centre's real depth, near plane and
// intrinsic projection provenance without treating a 3D billboard as UI.
//
// This primitive is deliberately game-independent and fail-closed. Callers
// must identify a proven construction site themselves; an arbitrary nearby
// packed coordinate is never matched to a recent projection heuristically.
bool derivePGXPProjectedScreenOffset(const PGXPValue &centre,
                                     uint32_t centrePacked,
                                     uint32_t derivedPacked,
                                     PGXPValue &derived) noexcept;

// Resolve a sidecar at a final GPU submission boundary. Ordinary tracked RAM
// remains authoritative; explicitly game-owned, construction-proven repairs
// are consulted only after that lookup misses. Keeping this out of generated
// CPU code makes donor-driven precision fixes cheap to iterate and gives DMA,
// linked-list and HLE ordering-table transports one identical policy.
bool lookupPGXPSubmissionPrecision(const Memory *memory, uint32_t address,
                                   uint32_t packedWord,
                                   PGXPValue &precision) noexcept;

// Resolve a projected polygon operand component-by-component. Matching X/Y
// remains available for a complete signed projection at zero/negative W so a
// renderer can reproduce the GTE divider's saturated screen position. Z is
// accepted as ordinary depth only when it is finite and positive. A non-finite
// or implausibly divergent legacy X/Y component falls back independently to
// the packed 11-bit GPU coordinate. A finite, positive-W signed projection
// may legitimately differ by more than the legacy delta limit, but only
// inside the explicit guard band. Authentic positive-W GTE divider saturation
// is the narrow exception: while W is below near, finite SXY inside the exact
// [-1024, 1023] clamp remains authoritative even when raw Q/W is extreme.
// A game-profile-authenticated signed near-plane vertex may likewise retain
// its finite clamped screen witness while geometric clipping consumes Q/W.
// Crossing the bound in every other case reconciles both screen components so
// the packet-level replay gate fails closed atomically. No depth is invented
// for stale or XY-only operands.
PGXPResolvedVertex resolvePGXPVertex(const PGXPValue &precision,
                                     uint32_t packedWord);

// One semantic class is assigned to every GP0 coordinate operand.  The
// classification KPI is deliberately independent from genuine XYZ coverage:
// Screen2D never needs a geometric Z, while Projected3D does.
enum class PGXPPrimitive : uint8_t {
  FlatTriangle,
  TexturedTriangle,
  FlatQuad,
  TexturedQuad,
  GouraudTriangle,
  GouraudTexturedTriangle,
  GouraudQuad,
  GouraudTexturedQuad,
  Line,
  Polyline,
  GouraudLine,
  GouraudPolyline,
  Rectangle,
  Sprite,
  Count,
};

// Rendering domain is a packet property, not an implication of the GP0
// opcode family. Most polygons require projected XYZ, while a narrowly proven
// display-anchored composite is screen-space even though the PS1 encodes it as
// a polygon. All vertices in one packet receive the same decision.
enum class PGXPSemanticDomain : uint8_t {
  Screen2D,
  Projected3D,
};

enum class PGXPSemanticReason : uint8_t {
  Intrinsic2DFamily,
  DisplayAnchoredComposite,
  PolygonDefault,
};

struct PGXPSemanticDecision {
  PGXPSemanticDomain domain = PGXPSemanticDomain::Projected3D;
  PGXPSemanticReason reason = PGXPSemanticReason::PolygonDefault;

  constexpr bool requiresXYZ() const {
    return domain == PGXPSemanticDomain::Projected3D;
  }

  static constexpr PGXPSemanticDecision projected3D() { return {}; }
  static constexpr PGXPSemanticDecision intrinsicScreen2D() {
    return {PGXPSemanticDomain::Screen2D,
            PGXPSemanticReason::Intrinsic2DFamily};
  }
};

// Self-contained input for the pure packet classifier. Coordinate words are
// still in the GP0 packet's local signed-11 space; draw offset maps them to the
// inclusive drawing area. `apertureWidth/Height` must come from the programmed
// GP1 display geometry, independently of presentation/debug policy.
struct PGXPPolygonSemanticInput {
  uint8_t opcode = 0;
  uint8_t vertexCount = 0;
  std::array<uint32_t, 4> packedVertices{};
  std::array<PGXPValue, 4> precision{};
  int32_t drawOffsetX = 0;
  int32_t drawOffsetY = 0;
  int32_t drawX0 = 0;
  int32_t drawY0 = 0;
  int32_t drawX1 = -1;
  int32_t drawY1 = -1;
  uint32_t apertureWidth = 0;
  uint32_t apertureHeight = 0;
  bool programmedDisplayGeometry = false;
};

// Conservatively recognize packet-level screen composites. The rule accepts
// exact full-target blends and a narrow set of target-relative textured
// transition panels; axis alignment or missing PGXP alone is never sufficient.
PGXPSemanticDecision
classifyPGXPPolygonSemantic(const PGXPPolygonSemanticInput &input);

// Lock-free diagnostic stages for the GTE -> CPU/RAM -> GP0 precision path.
// They only increment while PS1_METRICS is enabled; publishing folds their
// deltas into the ordinary JSON collector once at shutdown.
enum class PGXPStage : uint8_t {
  ProjectionXYZ,
  ProjectionSigned,
  ProjectionXYOnly,
  Mfc2Hit,
  Mfc2Miss,
  Swc2Hit,
  Swc2Miss,
  GprStoreHit,
  RamStore,
  RamLookupHit,
  RamLookupMiss,
  Gp0WordXY,
  Gp0WordXYZ,
  Gp0WordSigned,
  Count,
};

void countPGXPStage(PGXPStage stage);

// Record the exact packed output of a GTE projection.  The bounded recent
// value cache is diagnostic evidence for a later coordinate that reached GP0
// without its sidecar; it is never used to render or to synthesize depth.
void recordPGXPProjection(const PGXPValue &value);

// Memory::lookupPrecision records why a RAM packet lookup succeeded or
// failed. GPU::writeGP0 consumes the immediately preceding lookup and stores
// the result in lockstep with the command word.
void notePGXPRamLookup(uint32_t address, uint32_t value, PGXPRamLookup result);
PGXPRamLookup consumePGXPRamLookup(uint32_t address, uint32_t value);

// Classify one explicit GP0 coordinate word.  Polygon callers may have true
// GTE XYZ; line/rectangle/sprite callers are always Screen2D and intentionally
// remain outside the host depth buffer.
void recordPGXPVertex(uint8_t opcode, uint8_t wordIndex, uint32_t sourceAddress,
                      PGXPPrimitive primitive, PGXPSemanticDecision semantic,
                      const PGXPValue &precision, uint32_t packedWord);

void publishPGXPStageMetrics();

// Reset diagnostic state as well as publish high-water marks. Tests only.
void resetPGXPDiagnosticsForTesting();

} // namespace ps1
