#include "runtime/pgxp.h"

#include "runtime/games/crash3_profile.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

namespace ps1 {

bool g_pgxpTrackingEnabled = true;

void configurePGXPTracking(bool enabled) noexcept {
  g_pgxpTrackingEnabled = enabled;
}

namespace {

constexpr std::size_t stageCount = static_cast<std::size_t>(PGXPStage::Count);
std::array<std::atomic<uint64_t>, stageCount> stageCounters{};
std::array<uint64_t, stageCount> stagePublished{};
constexpr std::array<const char *, stageCount> stageNames = {
    "pgxp.projection.xyz", "pgxp.projection.signed", "pgxp.projection.xy_only",
    "pgxp.mfc2.hit",       "pgxp.mfc2.miss",         "pgxp.swc2.hit",
    "pgxp.swc2.miss",      "pgxp.gpr_store.hit",     "pgxp.ram_store",
    "pgxp.ram_lookup.hit", "pgxp.ram_lookup.miss",   "pgxp.gp0_word.xy",
    "pgxp.gp0_word.xyz",   "pgxp.gp0_word.signed",
};

enum class SemanticClass : uint8_t {
  Screen2D,
  Projected3D,
  Count,
};

enum class PrecisionClass : uint8_t {
  XYZ,
  SignedProjection,
  XYOnly,
  Stale,
  None,
  Count,
};

enum class FallbackReason : uint8_t {
  None,
  Intentional2D,
  DisplayAnchoredComposite,
  XYOnly,
  CpuGenerated,
  PacketCopyLoss,
  QueueDesync,
  Count,
};

constexpr std::size_t semanticCount =
    static_cast<std::size_t>(SemanticClass::Count);
constexpr std::size_t precisionCount =
    static_cast<std::size_t>(PrecisionClass::Count);
constexpr std::size_t reasonCount =
    static_cast<std::size_t>(FallbackReason::Count);
constexpr std::size_t primitiveCount =
    static_cast<std::size_t>(PGXPPrimitive::Count);
constexpr std::size_t pathCount =
    static_cast<std::size_t>(PGXPSubmissionPath::Count);
constexpr std::size_t lookupCount =
    static_cast<std::size_t>(PGXPRamLookup::Count);

constexpr std::array<const char *, semanticCount> semanticNames = {
    "screen_2d", "projected_3d"};
constexpr std::array<const char *, precisionCount> precisionNames = {
    "xyz", "signed_projection", "xy_only", "stale", "none"};
constexpr std::array<const char *, reasonCount> reasonNames = {
    "none",          "intentional_2d",   "full_target_composite", "xy_only",
    "cpu_generated", "packet_copy_loss", "queue_desync"};
constexpr std::array<const char *, primitiveCount> primitiveNames = {
    "flat_triangle",
    "textured_triangle",
    "flat_quad",
    "textured_quad",
    "gouraud_triangle",
    "gouraud_textured_triangle",
    "gouraud_quad",
    "gouraud_textured_quad",
    "line",
    "polyline",
    "gouraud_line",
    "gouraud_polyline",
    "rectangle",
    "sprite",
};
constexpr std::array<const char *, pathCount> pathNames = {
    "direct_mmio", "hle_generated",   "hle_otag",
    "dma_block",   "dma_linked_list", "bios",
};
constexpr std::array<const char *, lookupCount> lookupNames = {
    "not_applicable", "unobserved", "no_sidecar", "empty",
    "stale",          "hit_xy",     "hit_xyz",    "hit_signed_projection",
};

struct Breakdown {
  std::atomic<uint64_t> total{0};
  std::array<std::atomic<uint64_t>, semanticCount> semantic{};
  std::array<std::atomic<uint64_t>, precisionCount> precision{};
  std::array<std::atomic<uint64_t>, reasonCount> reason{};
  std::array<std::atomic<uint64_t>, lookupCount> lookup{};
};

struct PublishedBreakdown {
  uint64_t total = 0;
  std::array<uint64_t, semanticCount> semantic{};
  std::array<uint64_t, precisionCount> precision{};
  std::array<uint64_t, reasonCount> reason{};
  std::array<uint64_t, lookupCount> lookup{};
};

Breakdown globalBreakdown;
PublishedBreakdown globalPublished;
std::array<Breakdown, 256> opcodeBreakdowns{};
std::array<PublishedBreakdown, 256> opcodePublished{};
std::array<Breakdown, primitiveCount> primitiveBreakdowns{};
std::array<PublishedBreakdown, primitiveCount> primitivePublished{};
std::array<Breakdown, pathCount> pathBreakdowns{};
std::array<PublishedBreakdown, pathCount> pathPublished{};

std::atomic<uint64_t> xyzRequired{0};
std::atomic<uint64_t> xyzValid{0};
std::atomic<uint64_t> xyzMissing{0};
std::atomic<uint64_t> xyzNotApplicable{0};
uint64_t xyzRequiredPublished = 0;
uint64_t xyzValidPublished = 0;
uint64_t xyzMissingPublished = 0;
uint64_t xyzNotApplicablePublished = 0;

// Exact-value evidence is intentionally bounded and diagnostic-only. A
// matching value means a projection with the same packed SXY was seen in the
// current or previous two guest frames (the game can build an OT one VBlank
// before submission). It is reported as likely copy loss, but never used as
// rendering provenance because distinct vertices can share packed XY.
constexpr std::size_t projectionSlotCount = 1u << 16;
constexpr uint32_t projectionFrameWindow = 2;
std::array<std::atomic<uint64_t>, projectionSlotCount> recentProjections{};

uint32_t hashWord(uint32_t word) {
  word ^= word >> 16;
  word *= 0x7feb352du;
  word ^= word >> 15;
  return word;
}

bool recentlyProjected(uint32_t word) {
  const uint64_t entry =
      recentProjections[hashWord(word) & (projectionSlotCount - 1)].load(
          std::memory_order_relaxed);
  if (static_cast<uint32_t>(entry) != word)
    return false;
  const uint32_t seenToken = static_cast<uint32_t>(entry >> 32);
  const uint32_t nowToken = static_cast<uint32_t>(metrics::traceFrame()) + 1u;
  return seenToken != 0 && nowToken - seenToken <= projectionFrameWindow;
}

struct LastRamLookup {
  uint32_t address = 0;
  uint32_t value = 0;
  PGXPRamLookup result = PGXPRamLookup::Unobserved;
  bool valid = false;
};
thread_local LastRamLookup lastRamLookup;

constexpr uint32_t directSourceAddress = 0x200000u;
constexpr uint32_t scratchpadSourceBase = directSourceAddress + 1u;
constexpr uint32_t scratchpadSourceEnd = scratchpadSourceBase + 0x400u;

uint32_t diagnosticSourceAddress(uint32_t address) {
  uint32_t physical = address;
  if (address >= 0x80000000u)
    physical = address & 0x1fffffffu;
  if (physical < 0x00800000u)
    return physical & 0x1fffffu;
  if (physical >= 0x1f800000u && physical < 0x1f800400u)
    return scratchpadSourceBase + (physical - 0x1f800000u);
  return 0x3fffffu;
}

// Lossy metrics would defeat the purpose of this diagnostic. This fixed
// open-addressed table stores every distinct non-XYZ source tuple seen in a
// run up to 262,144 tuples, and exposes an explicit overflow counter.
constexpr std::size_t sourceBucketCount = 1u << 18;
struct SourceBucket {
  std::atomic<uint64_t> keyPlusOne{0};
  std::atomic<uint64_t> count{0};
  uint64_t published = 0;
};
std::array<SourceBucket, sourceBucketCount> sourceBuckets{};
std::atomic<uint64_t> sourceOverflow{0};
uint64_t sourceOverflowPublished = 0;
std::mutex publishMutex;

uint64_t liveFlushInterval() {
  static const uint64_t interval = [] {
    const char *value = std::getenv("PS1_PGXP_METRICS_FLUSH_INTERVAL");
    if (value == nullptr || *value == '\0')
      return uint64_t{0};
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return end != value && *end == '\0' ? static_cast<uint64_t>(parsed)
                                        : uint64_t{0};
  }();
  return interval;
}

uint64_t makeSourceKey(uint32_t sourceAddress, uint8_t opcode,
                       uint8_t wordIndex, PGXPSubmissionPath path,
                       PGXPPrimitive primitive, PrecisionClass precision,
                       FallbackReason reason, PGXPRamLookup lookup) {
  const uint64_t source = sourceAddress == UINT32_MAX
                              ? directSourceAddress
                              : diagnosticSourceAddress(sourceAddress);
  return source | (static_cast<uint64_t>(opcode) << 22) |
         (static_cast<uint64_t>(wordIndex & 0x0fu) << 30) |
         (static_cast<uint64_t>(path) << 34) |
         (static_cast<uint64_t>(reason) << 37) |
         (static_cast<uint64_t>(primitive) << 40) |
         (static_cast<uint64_t>(precision) << 44) |
         (static_cast<uint64_t>(lookup) << 47);
}

void countSource(uint64_t key) {
  const uint64_t storedKey = key + 1;
  std::size_t slot =
      static_cast<std::size_t>((key ^ (key >> 29) ^ (key >> 17)) *
                               0x9e3779b97f4a7c15ull) &
      (sourceBucketCount - 1);
  for (std::size_t probe = 0; probe < sourceBucketCount; ++probe) {
    SourceBucket &bucket = sourceBuckets[slot];
    uint64_t current = bucket.keyPlusOne.load(std::memory_order_acquire);
    if (current == storedKey) {
      bucket.count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (current == 0 && bucket.keyPlusOne.compare_exchange_strong(
                            current, storedKey, std::memory_order_acq_rel,
                            std::memory_order_relaxed)) {
      bucket.count.store(1, std::memory_order_release);
      return;
    }
    slot = (slot + 1) & (sourceBucketCount - 1);
  }
  sourceOverflow.fetch_add(1, std::memory_order_relaxed);
}

void countBreakdown(Breakdown &breakdown, SemanticClass semantic,
                    PrecisionClass precision, FallbackReason reason,
                    PGXPRamLookup lookup) {
  breakdown.total.fetch_add(1, std::memory_order_relaxed);
  breakdown.semantic[static_cast<std::size_t>(semantic)].fetch_add(
      1, std::memory_order_relaxed);
  breakdown.precision[static_cast<std::size_t>(precision)].fetch_add(
      1, std::memory_order_relaxed);
  breakdown.reason[static_cast<std::size_t>(reason)].fetch_add(
      1, std::memory_order_relaxed);
  breakdown.lookup[static_cast<std::size_t>(lookup)].fetch_add(
      1, std::memory_order_relaxed);
}

void publishAtomic(std::string name, const std::atomic<uint64_t> &counter,
                   uint64_t &published, bool includeZero = false) {
  const uint64_t value = counter.load(std::memory_order_relaxed);
  const uint64_t delta = value - published;
  if (delta != 0 || includeZero)
    metrics::count(name, delta);
  published = value;
}

void publishBreakdown(const std::string &prefix, const Breakdown &breakdown,
                      PublishedBreakdown &published, bool includeZero = false) {
  publishAtomic(prefix + ".total", breakdown.total, published.total,
                includeZero);
  for (std::size_t i = 0; i < semanticCount; ++i)
    publishAtomic(prefix + ".class." + semanticNames[i], breakdown.semantic[i],
                  published.semantic[i], includeZero);
  for (std::size_t i = 0; i < precisionCount; ++i)
    publishAtomic(prefix + ".precision." + precisionNames[i],
                  breakdown.precision[i], published.precision[i], includeZero);
  for (std::size_t i = 0; i < reasonCount; ++i)
    publishAtomic(prefix + ".fallback." + reasonNames[i], breakdown.reason[i],
                  published.reason[i], includeZero);
  for (std::size_t i = 0; i < lookupCount; ++i)
    publishAtomic(prefix + ".lookup." + lookupNames[i], breakdown.lookup[i],
                  published.lookup[i], includeZero);
}

void clearBreakdown(Breakdown &breakdown, PublishedBreakdown &published) {
  breakdown.total.store(0, std::memory_order_relaxed);
  for (auto &counter : breakdown.semantic)
    counter.store(0, std::memory_order_relaxed);
  for (auto &counter : breakdown.precision)
    counter.store(0, std::memory_order_relaxed);
  for (auto &counter : breakdown.reason)
    counter.store(0, std::memory_order_relaxed);
  for (auto &counter : breakdown.lookup)
    counter.store(0, std::memory_order_relaxed);
  published = {};
}

} // namespace

bool derivePGXPProjectedScreenOffset(const PGXPValue &centre,
                                     uint32_t centrePacked,
                                     uint32_t derivedPacked,
                                     PGXPValue &derived) noexcept {
  derived.invalidate();
  if (!centre.hasSignedProjection(centrePacked))
    return false;

  const int32_t deltaX =
      static_cast<int32_t>(static_cast<int16_t>(derivedPacked)) -
      static_cast<int32_t>(static_cast<int16_t>(centrePacked));
  const int32_t deltaY =
      static_cast<int32_t>(static_cast<int16_t>(derivedPacked >> 16u)) -
      static_cast<int32_t>(static_cast<int16_t>(centrePacked >> 16u));

  PGXPValue result = centre;
  result.value = derivedPacked;
  result.x += static_cast<float>(deltaX);
  result.y += static_cast<float>(deltaY);
  result.qx += static_cast<double>(deltaX) * result.z;
  result.qy += static_cast<double>(deltaY) * result.z;
  result.ramLookup = PGXPRamLookup::NotApplicable;
  result.submissionPath = PGXPSubmissionPath::DirectMmio;
  if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
      !std::isfinite(result.qx) || !std::isfinite(result.qy))
    return false;

  derived = result;
  return true;
}

bool lookupPGXPSubmissionPrecision(const Memory *memory, uint32_t address,
                                   uint32_t packedWord,
                                   PGXPValue &precision) noexcept {
  if (!pgxpTrackingEnabled())
    return false;
  if (memory != nullptr &&
      memory->lookupPrecision(address, packedWord, precision))
    return true;
  if (!games::crash3::lookupProjectedBillboardPrecision(
          address, packedWord, precision))
    return false;

  // Override the ordinary miss recorded above with the final, recovered
  // submission result consumed by GPU::writeGP0. Both counters remain useful:
  // one describes physical RAM sidecars, the other the exact repair boundary.
  notePGXPRamLookup(address, packedWord,
                    PGXPRamLookup::HitSignedProjection);
  countPGXPStage(PGXPStage::RamLookupHit);
  return true;
}

void countPGXPStage(PGXPStage stage) {
  if (!metrics::enabled())
    return;
  const auto index = static_cast<std::size_t>(stage);
  if (index < stageCounters.size())
    stageCounters[index].fetch_add(1, std::memory_order_relaxed);
}

void recordPGXPProjection(const PGXPValue &value) {
  if (!metrics::enabled() || !value.hasXY(value.value))
    return;
  const uint32_t frameToken = static_cast<uint32_t>(metrics::traceFrame()) + 1u;
  recentProjections[hashWord(value.value) & (projectionSlotCount - 1)].store(
      (static_cast<uint64_t>(frameToken) << 32) | value.value,
      std::memory_order_relaxed);
}

void notePGXPRamLookup(uint32_t address, uint32_t value, PGXPRamLookup result) {
  if (!metrics::enabled() && !metrics::pgxpVertexTraceEnabled())
    return;
  lastRamLookup = {diagnosticSourceAddress(address), value, result, true};
}

PGXPRamLookup consumePGXPRamLookup(uint32_t address, uint32_t value) {
  if (!metrics::enabled() && !metrics::pgxpVertexTraceEnabled())
    return PGXPRamLookup::NotApplicable;
  if (!lastRamLookup.valid ||
      lastRamLookup.address != diagnosticSourceAddress(address) ||
      lastRamLookup.value != value)
    return PGXPRamLookup::Unobserved;
  lastRamLookup.valid = false;
  return lastRamLookup.result;
}

namespace {

int32_t decodePackedSigned11(uint32_t value) {
  value &= 0x7ffu;
  return static_cast<int32_t>((value & 0x400u) != 0 ? value | 0xfffff800u
                                                    : value);
}

uint32_t diagnosticFloatBits(float value) {
  static_assert(sizeof(value) == sizeof(uint32_t));
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

int64_t diagnosticDoubleBits(double value) {
  static_assert(sizeof(value) == sizeof(uint64_t));
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return static_cast<int64_t>(bits);
}

int64_t diagnosticQ16(float value) {
  if (!std::isfinite(value))
    return std::numeric_limits<int64_t>::min();
  const double scaled = static_cast<double>(value) * 65536.0;
  if (scaled >= static_cast<double>(std::numeric_limits<int64_t>::max()))
    return std::numeric_limits<int64_t>::max();
  if (scaled <= static_cast<double>(std::numeric_limits<int64_t>::min()))
    return std::numeric_limits<int64_t>::min();
  return static_cast<int64_t>(std::llround(scaled));
}

PGXPVertexRejectReason classifyVertexReject(const PGXPValue &precision,
                                            uint32_t packedWord, float &deltaX,
                                            float &deltaY, int32_t &nativeX,
                                            int32_t &nativeY) {
  nativeX = decodePackedSigned11(packedWord);
  nativeY = decodePackedSigned11(packedWord >> 16);
  deltaX = precision.x - static_cast<float>(nativeX);
  deltaY = precision.y - static_cast<float>(nativeY);

  const bool hasX = precision.hasX(packedWord);
  const bool hasY = precision.hasY(packedWord);
  if (!hasX || !hasY) {
    const bool staleX = (precision.flags & PGXPValue::VALID_X) != 0 && !hasX;
    const bool staleY = (precision.flags & PGXPValue::VALID_Y) != 0 && !hasY;
    return staleX || staleY ? PGXPVertexRejectReason::Stale
                            : PGXPVertexRejectReason::NoXY;
  }

  if (precision.hasSignedProjection(packedWord))
    return PGXPVertexRejectReason::None;
  if ((precision.flags & PGXPValue::VALID_Z) == 0)
    return PGXPVertexRejectReason::XYOnly;
  // X and Y are resolved independently below: a bad screen component does
  // not invalidate an otherwise associated, finite depth value.
  if (!std::isfinite(precision.z))
    return PGXPVertexRejectReason::NonFinite;
  if (!(precision.z > 0.0f))
    return PGXPVertexRejectReason::NonPositiveZ;
  return PGXPVertexRejectReason::None;
}

} // namespace

PGXPResolvedVertex resolvePGXPVertex(const PGXPValue &precision,
                                     uint32_t packedWord) {
  const float nativeX = static_cast<float>(decodePackedSigned11(packedWord));
  const float nativeY =
      static_cast<float>(decodePackedSigned11(packedWord >> 16));
  PGXPResolvedVertex resolved;
  resolved.x = nativeX;
  resolved.y = nativeY;

  const bool hasSignedProjection = precision.hasSignedProjection(packedWord);
  if (hasSignedProjection) {
    resolved.z = precision.z;
    resolved.qx = precision.qx;
    resolved.qy = precision.qy;
    resolved.nearDepth = precision.nearDepth;
    resolved.projectionId = precision.projectionId;
    resolved.projectionContext = precision.projectionContext;
    resolved.gameProfileCallsite = precision.gameProfileCallsite;
    resolved.projectionTransformValueFingerprint =
        precision.projectionTransformValueFingerprint;
    resolved.projectionTransformLineage = precision.projectionTransformLineage;
    resolved.modelVertexSource = precision.modelVertexSource;
    resolved.modelValueFlowSourceFingerprint =
        precision.modelValueFlowSourceFingerprint;
    resolved.modelValueFlowProgramFingerprint =
        precision.modelValueFlowProgramFingerprint;
    resolved.intrinsicProvenance =
        precision.flags & PGXPValue::VALID_INTRINSIC_PROVENANCE;
    resolved.hasSignedProjection = true;
  }

  auto resolveComponent = [](float precise, float native, bool enforceDelta,
                             PGXPCoordinateSource &source) {
    if (!std::isfinite(precise)) {
      source = PGXPCoordinateSource::NativeNonFinite;
      return native;
    }
    if (enforceDelta && std::abs(precise - native) > PGXP_SCREEN_DELTA_LIMIT) {
      source = PGXPCoordinateSource::NativeDelta;
      return native;
    }
    source = PGXPCoordinateSource::Precise;
    return precise;
  };

  const bool hasUsableDepth = precision.hasXY(packedWord) &&
                              (precision.flags & PGXPValue::VALID_Z) != 0 &&
                              std::isfinite(precision.z) && precision.z > 0.0f;

  // The legacy 8-pixel plausibility test predates exact homogeneous GTE
  // provenance. Fractional view Z can legitimately move an off-screen
  // projection farther than that from the divider-quantized packed SXY (the
  // Crash 3 tuple at 0x80076B3C differs by 8.676 px). Trust a complete signed
  // projection only for authentic positive W and while both the stored screen
  // position and Q/W reconstruction stay inside a conservative donor guard
  // band. Positive W below near remains valid provenance for the later raster
  // policy. An extreme signed projection reconciles both axes: this makes
  // the downstream packet gate reject the complete primitive rather than
  // mixing precise and affine components.
  // projectionId is already required by hasSignedProjection(). The context
  // hash is a cross-component consistency fingerprint, not a validity token,
  // and zero is not reserved by its FNV producer, so it must not be used as an
  // authenticity bit here.
  const bool authenticPositiveSignedProjection =
      hasSignedProjection && precision.origin == PGXPOrigin::GteProjection &&
      precision.nearDepth > 0.0 && precision.z > 0.0;
  bool signedProjectionWithinGuardBand = false;
  bool authenticGteSaturatedScreen = false;
  if (authenticPositiveSignedProjection && std::isfinite(precision.x) &&
      std::isfinite(precision.y)) {
    const double projectedX = precision.qx / precision.z;
    const double projectedY = precision.qy / precision.z;
    signedProjectionWithinGuardBand =
        std::isfinite(projectedX) && std::isfinite(projectedY) &&
        std::abs(projectedX) <= PGXP_SIGNED_PROJECTION_GUARD_BAND &&
        std::abs(projectedY) <= PGXP_SIGNED_PROJECTION_GUARD_BAND &&
        std::abs(static_cast<double>(precision.x)) <=
            PGXP_SIGNED_PROJECTION_GUARD_BAND &&
        std::abs(static_cast<double>(precision.y)) <=
            PGXP_SIGNED_PROJECTION_GUARD_BAND;
    // For 0 < W < near the architectural divider is saturated. The GTE
    // producer stores its independently computed, hardware-clamped SXY next
    // to genuine Q; Q/W is intentionally not the position rendered by the
    // donor saturation policy. This exception is safe only for authenticated
    // GTE provenance and the exact signed-11 screen domain.
    authenticGteSaturatedScreen =
        precision.z < precision.nearDepth && precision.x >= -1024.0f &&
        precision.x <= 1023.0f && precision.y >= -1024.0f &&
        precision.y <= 1023.0f;
  }
  // Crash's widened host admission can retain a face which crosses the modern
  // near plane even though one of its vertices is behind the camera. The
  // packed SXY for that vertex is only a donor saturation witness; geometric
  // clipping consumes authentic Q and signed W instead. A Crash-owned
  // projection tag therefore makes its finite, hardware-clamped X/Y complete
  // for coverage without replacing Q with the packed guard coordinate. Keep
  // this exception profile-authenticated so other executables retain the
  // generic fail-closed guard-band policy.
  const bool crash3SignedNearPlaneScreen =
      hasSignedProjection && precision.gameProfileCallsite != 0u &&
      precision.origin == PGXPOrigin::GteProjection &&
      std::isfinite(precision.z) && precision.z <= 0.0 &&
      std::isfinite(precision.qx) && std::isfinite(precision.qy) &&
      std::isfinite(precision.x) && std::isfinite(precision.y) &&
      precision.nearDepth > 0.0 && precision.x >= -1024.0f &&
      precision.x <= 1023.0f && precision.y >= -1024.0f &&
      precision.y <= 1023.0f;

  // Screen provenance remains useful independently of W validity. SwanStation
  // keeps the resolved PGXP X/Y returned by GetPreciseVertex even when that
  // function reports an invalid W, then makes the complete packet use W=1.
  // Retaining the position here lets the renderer reproduce that packet-atomic
  // fallback rather than silently switching to packed compatibility geometry.
  if (precision.hasXY(packedWord)) {
    if ((authenticPositiveSignedProjection && std::isfinite(precision.x) &&
         std::isfinite(precision.y) && authenticGteSaturatedScreen) ||
        crash3SignedNearPlaneScreen) {
      resolved.x = precision.x;
      resolved.y = precision.y;
      resolved.xSource = PGXPCoordinateSource::PreciseGteSaturated;
      resolved.ySource = PGXPCoordinateSource::PreciseGteSaturated;
      if (crash3SignedNearPlaneScreen)
        metrics::count("host.crash3.pgxp.signed_near_plane_screen");
    } else if (authenticPositiveSignedProjection &&
               std::isfinite(precision.x) && std::isfinite(precision.y) &&
               !signedProjectionWithinGuardBand) {
      resolved.xSource = PGXPCoordinateSource::NativeGuardBand;
      resolved.ySource = PGXPCoordinateSource::NativeGuardBand;
    } else {
      const bool enforceLegacyDelta = !signedProjectionWithinGuardBand;
      resolved.x = resolveComponent(precision.x, nativeX, enforceLegacyDelta,
                                    resolved.xSource);
      resolved.y = resolveComponent(precision.y, nativeY, enforceLegacyDelta,
                                    resolved.ySource);
    }
  }

  // Z belongs to the complete packed coordinate. A partial/stale XY match is
  // therefore not enough to retain it, even if the numerical value looks
  // plausible.
  if (!hasUsableDepth)
    return resolved;

  resolved.z = precision.z;
  resolved.hasDepth = true;
  return resolved;
}

PGXPSemanticDecision
classifyPGXPPolygonSemantic(const PGXPPolygonSemanticInput &input) {
  const auto projected = PGXPSemanticDecision::projected3D();

  // 0x2A is the semi-transparent, untextured monochrome quad used for an
  // ordered full-screen blend. 0x2E is its modulated-texture counterpart and
  // is also used for display-anchored page transitions. Opaque, raw-texture
  // and Gouraud packets stay in the projected domain until equally strong
  // packet-level evidence exists.
  if ((input.opcode != 0x2au && input.opcode != 0x2eu) ||
      input.vertexCount != 4 || !input.programmedDisplayGeometry ||
      input.drawX1 < input.drawX0 || input.drawY1 < input.drawY0)
    return projected;

  const int64_t targetWidth =
      static_cast<int64_t>(input.drawX1) - input.drawX0 + 1;
  const int64_t targetHeight =
      static_cast<int64_t>(input.drawY1) - input.drawY0 + 1;
  if (targetWidth <= 0 || targetHeight <= 0 ||
      targetWidth != static_cast<int64_t>(input.apertureWidth) ||
      targetHeight != static_cast<int64_t>(input.apertureHeight) ||
      input.drawOffsetX != input.drawX0 || input.drawOffsetY != input.drawY0)
    return projected;

  std::array<int32_t, 4> x{};
  std::array<int32_t, 4> y{};
  for (std::size_t i = 0; i < 4; ++i) {
    // Any surviving projection provenance makes the whole packet 3D. This is
    // intentionally independent of RAM-lookup diagnostics and trace state.
    if (input.precision[i].flags != 0 ||
        input.precision[i].origin == PGXPOrigin::GteProjection)
      return projected;
    x[i] = decodePackedSigned11(input.packedVertices[i]);
    y[i] = decodePackedSigned11(input.packedVertices[i] >> 16);
  }

  const auto isFullTargetQuad = [&] {
    const auto [minX, maxX] = std::minmax_element(x.begin(), x.end());
    const auto [minY, maxY] = std::minmax_element(y.begin(), y.end());
    if (*minX != 0 || *minY != 0 ||
        static_cast<int64_t>(*maxX) != targetWidth ||
        static_cast<int64_t>(*maxY) != targetHeight)
      return false;

    uint8_t cornerMask = 0;
    std::array<uint8_t, 4> corner{};
    for (std::size_t i = 0; i < 4; ++i) {
      if ((x[i] != 0 && x[i] != *maxX) || (y[i] != 0 && y[i] != *maxY))
        return false;
      corner[i] = static_cast<uint8_t>((x[i] == *maxX ? 1u : 0u) |
                                       (y[i] == *maxY ? 2u : 0u));
      const uint8_t bit = static_cast<uint8_t>(1u << corner[i]);
      if ((cornerMask & bit) != 0)
        return false;
      cornerMask |= bit;

      const int64_t adjustedX = static_cast<int64_t>(x[i]) + input.drawOffsetX;
      const int64_t adjustedY = static_cast<int64_t>(y[i]) + input.drawOffsetY;
      if ((adjustedX != input.drawX0 &&
           adjustedX != static_cast<int64_t>(input.drawX1) + 1) ||
          (adjustedY != input.drawY0 &&
           adjustedY != static_cast<int64_t>(input.drawY1) + 1))
        return false;
    }

    // GPU's quad path splits (0,1,2) and (1,2,3). The shared edge must be one
    // rectangle diagonal and the remaining vertices the other; merely having
    // the right bounding box must not admit a strip or bow-tie.
    return cornerMask == 0x0fu && (corner[0] ^ corner[3]) == 3u &&
           (corner[1] ^ corner[2]) == 3u;
  };

  if (!isFullTargetQuad()) {
    if (input.opcode != 0x2eu || (targetWidth & 1) != 0 ||
        (targetHeight & 1) != 0)
      return projected;

    const int64_t halfWidth = targetWidth / 2;
    const int64_t halfHeight = targetHeight / 2;
    const std::array<int64_t, 4> px = {x[0], x[1], x[2], x[3]};
    const std::array<int64_t, 4> py = {y[0], y[1], y[2], y[3]};

    // Crash's page-turn transition tessellates the target into four rotated
    // parallelograms. Each packet is anchored at the exact display centre and
    // its two basis edges independently reach a horizontal and vertical
    // target edge. Requiring both the centre anchor and exact parallelogram
    // closure avoids treating an ordinary CPU-generated billboard as 2D.
    const bool parallelogram =
        px[0] + px[3] == px[1] + px[2] && py[0] + py[3] == py[1] + py[2];
    const bool centreAnchor = px[0] == halfWidth && py[0] == halfHeight;
    const auto reachesEdge = [](int64_t value, int64_t extent,
                                int64_t direction) {
      return direction < 0 ? value <= 0 : value >= extent;
    };
    const auto isCornerPanel = [&](std::size_t horizontalIndex,
                                   std::size_t verticalIndex) {
      const int64_t hx = px[horizontalIndex] - px[0];
      const int64_t hy = py[horizontalIndex] - py[0];
      const int64_t vx = px[verticalIndex] - px[0];
      const int64_t vy = py[verticalIndex] - py[0];
      return std::abs(hx) >= halfWidth && std::abs(hy) < halfHeight &&
             std::abs(vy) >= halfHeight && std::abs(vx) < halfWidth &&
             reachesEdge(px[horizontalIndex], targetWidth, hx) &&
             reachesEdge(py[verticalIndex], targetHeight, vy);
    };
    const bool cornerPanel = parallelogram && centreAnchor &&
                             (isCornerPanel(1, 2) || isCornerPanel(2, 1));

    // The same transition places a target-relative diamond at its centre.
    // Its four points are exact quarters of the programmed aperture and both
    // GPU triangle pairs must join opposite points. An arbitrary centred quad
    // (the common CPU billboard case) deliberately does not satisfy this.
    bool centredDiamond = false;
    if ((targetWidth & 3) == 0 && (targetHeight & 3) == 0 &&
        px[0] + px[3] == targetWidth && py[0] + py[3] == targetHeight &&
        px[1] + px[2] == targetWidth && py[1] + py[2] == targetHeight) {
      const std::array<std::array<int64_t, 2>, 4> expected = {{
          {halfWidth, targetHeight / 4},
          {targetWidth * 3 / 4, halfHeight},
          {targetWidth / 4, halfHeight},
          {halfWidth, targetHeight * 3 / 4},
      }};
      uint8_t pointMask = 0;
      for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t point = 0; point < expected.size(); ++point) {
          if (px[i] == expected[point][0] && py[i] == expected[point][1])
            pointMask |= static_cast<uint8_t>(1u << point);
        }
      }
      centredDiamond = pointMask == 0x0fu;
    }

    if (!cornerPanel && !centredDiamond)
      return projected;
  }

  return {PGXPSemanticDomain::Screen2D,
          PGXPSemanticReason::DisplayAnchoredComposite};
}

void recordPGXPVertex(uint8_t opcode, uint8_t wordIndex, uint32_t sourceAddress,
                      PGXPPrimitive primitive, PGXPSemanticDecision decision,
                      const PGXPValue &precision, uint32_t packedWord) {
  const bool collectMetrics = metrics::enabled();
  const bool traceRejects = metrics::pgxpVertexTraceEnabled();
  if (!collectMetrics && !traceRejects)
    return;

  const auto primitiveIndex = static_cast<std::size_t>(primitive);
  const bool screen2D = decision.domain == PGXPSemanticDomain::Screen2D;
  float deltaX = 0.0f;
  float deltaY = 0.0f;
  int32_t nativeX = 0;
  int32_t nativeY = 0;
  const PGXPVertexRejectReason rejectReason = classifyVertexReject(
      precision, packedWord, deltaX, deltaY, nativeX, nativeY);
  const PGXPResolvedVertex resolved = resolvePGXPVertex(precision, packedWord);
  const uint8_t reconciledMask = resolved.reconciledMask();
  const bool hasResolvedScreen =
      resolved.hasDepth || resolved.hasSignedProjection;
  const bool needsNearPlaneClip =
      resolved.hasSignedProjection && resolved.z < resolved.nearDepth;
  const bool authenticSaturatedScreen =
      resolved.xSource == PGXPCoordinateSource::PreciseGteSaturated &&
      resolved.ySource == PGXPCoordinateSource::PreciseGteSaturated;

  if (traceRejects && !screen2D &&
      rejectReason != PGXPVertexRejectReason::None) {
    const int64_t wordAddress =
        sourceAddress == UINT32_MAX ? -1 : static_cast<int64_t>(sourceAddress);
    const int64_t packetHead =
        sourceAddress == UINT32_MAX
            ? -1
            : static_cast<int64_t>(sourceAddress -
                                   static_cast<uint32_t>(wordIndex) * 4u);
    metrics::trace(
        "pgxp.vertex_reject",
        {{"reason", static_cast<int64_t>(rejectReason)},
         {"source", wordAddress},
         {"packet_head", packetHead},
         {"opcode", opcode},
         {"word_index", wordIndex},
         {"packed", packedWord},
         {"value", precision.value},
         {"flags", precision.flags},
         {"origin", static_cast<int64_t>(precision.origin)},
         {"ram_lookup", static_cast<int64_t>(precision.ramLookup)},
         {"path", static_cast<int64_t>(precision.submissionPath)},
         {"has_x", precision.hasX(packedWord) ? 1 : 0},
         {"has_y", precision.hasY(packedWord) ? 1 : 0},
         {"has_xy", precision.hasXY(packedWord) ? 1 : 0},
         {"has_xyz", resolved.hasDepth ? 1 : 0},
         {"has_signed_projection", resolved.hasSignedProjection ? 1 : 0},
         {"x_bits", diagnosticFloatBits(precision.x)},
         {"y_bits", diagnosticFloatBits(precision.y)},
         {"z_bits", diagnosticDoubleBits(precision.z)},
         {"qx_bits", diagnosticDoubleBits(precision.qx)},
         {"qy_bits", diagnosticDoubleBits(precision.qy)},
         {"near_depth_bits", diagnosticDoubleBits(precision.nearDepth)},
         {"projection_id", static_cast<int64_t>(precision.projectionId)},
         {"projection_context",
          static_cast<int64_t>(precision.projectionContext)},
         {"native_x", nativeX},
         {"native_y", nativeY},
         {"dx_q16", diagnosticQ16(deltaX)},
         {"dy_q16", diagnosticQ16(deltaY)}});
  }

  if (traceRejects && !screen2D && needsNearPlaneClip) {
    const int64_t wordAddress =
        sourceAddress == UINT32_MAX ? -1 : static_cast<int64_t>(sourceAddress);
    const int64_t packetHead =
        sourceAddress == UINT32_MAX
            ? -1
            : static_cast<int64_t>(sourceAddress -
                                   static_cast<uint32_t>(wordIndex) * 4u);
    metrics::trace(
        "pgxp.vertex_near_plane",
        {{"source", wordAddress},
         {"packet_head", packetHead},
         {"opcode", opcode},
         {"word_index", wordIndex},
         {"packed", packedWord},
         {"z_bits", diagnosticDoubleBits(resolved.z)},
         {"near_depth_bits", diagnosticDoubleBits(resolved.nearDepth)},
         {"projection_id", static_cast<int64_t>(resolved.projectionId)},
         {"projection_context",
          static_cast<int64_t>(resolved.projectionContext)},
         {"x_source", static_cast<int64_t>(resolved.xSource)},
         {"y_source", static_cast<int64_t>(resolved.ySource)},
         {"x_bits", diagnosticFloatBits(precision.x)},
         {"y_bits", diagnosticFloatBits(precision.y)},
         {"qx_bits", diagnosticDoubleBits(precision.qx)},
         {"qy_bits", diagnosticDoubleBits(precision.qy)},
         {"native_x", nativeX},
         {"native_y", nativeY},
         {"ram_lookup", static_cast<int64_t>(precision.ramLookup)},
         {"path", static_cast<int64_t>(precision.submissionPath)}});
  }

  if (traceRejects && !screen2D && hasResolvedScreen && reconciledMask != 0) {
    const int64_t wordAddress =
        sourceAddress == UINT32_MAX ? -1 : static_cast<int64_t>(sourceAddress);
    const int64_t packetHead =
        sourceAddress == UINT32_MAX
            ? -1
            : static_cast<int64_t>(sourceAddress -
                                   static_cast<uint32_t>(wordIndex) * 4u);
    metrics::trace(
        "pgxp.vertex_reconcile",
        {{"source", wordAddress},
         {"packet_head", packetHead},
         {"opcode", opcode},
         {"word_index", wordIndex},
         {"packed", packedWord},
         {"value", precision.value},
         {"flags", precision.flags},
         {"origin", static_cast<int64_t>(precision.origin)},
         {"ram_lookup", static_cast<int64_t>(precision.ramLookup)},
         {"path", static_cast<int64_t>(precision.submissionPath)},
         {"component_mask", reconciledMask},
         {"x_source", static_cast<int64_t>(resolved.xSource)},
         {"y_source", static_cast<int64_t>(resolved.ySource)},
         {"original_x_bits", diagnosticFloatBits(precision.x)},
         {"original_y_bits", diagnosticFloatBits(precision.y)},
         {"original_z_bits", diagnosticDoubleBits(precision.z)},
         {"original_qx_bits", diagnosticDoubleBits(precision.qx)},
         {"original_qy_bits", diagnosticDoubleBits(precision.qy)},
         {"near_depth_bits", diagnosticDoubleBits(precision.nearDepth)},
         {"projection_id", static_cast<int64_t>(precision.projectionId)},
         {"projection_context",
          static_cast<int64_t>(precision.projectionContext)},
         {"signed_projection",
          precision.hasSignedProjection(packedWord) ? 1 : 0},
         {"resolved_x_bits", diagnosticFloatBits(resolved.x)},
         {"resolved_y_bits", diagnosticFloatBits(resolved.y)},
         {"resolved_z_bits", diagnosticDoubleBits(resolved.z)},
         {"native_x", nativeX},
         {"native_y", nativeY},
         {"dx_q16", diagnosticQ16(deltaX)},
         {"dy_q16", diagnosticQ16(deltaY)},
         {"resolved_x_q16", diagnosticQ16(resolved.x)},
         {"resolved_y_q16", diagnosticQ16(resolved.y)}});
  }

  if (!collectMetrics)
    return;

  if (!screen2D && needsNearPlaneClip)
    metrics::count("pgxp.vertex.near_plane");
  if (!screen2D && authenticSaturatedScreen)
    metrics::count("pgxp.vertex.gte_saturated_screen.precise");

  if (!screen2D && hasResolvedScreen && reconciledMask != 0) {
    metrics::count("pgxp.vertex.reconcile.total");
    // Keep the latest exact operand/provenance tuple in the compact metrics
    // report. This complements the opt-in per-event trace and makes a rare
    // whole-run precision gate reproducible without writing millions of
    // unrelated JSONL events.
    const int64_t wordAddress =
        sourceAddress == UINT32_MAX ? -1 : static_cast<int64_t>(sourceAddress);
    const int64_t packetHead =
        sourceAddress == UINT32_MAX
            ? -1
            : static_cast<int64_t>(sourceAddress -
                                   static_cast<uint32_t>(wordIndex) * 4u);
    metrics::setState("pgxp.vertex.reconcile.last.frame",
                      static_cast<int64_t>(metrics::traceFrame()));
    metrics::setState("pgxp.vertex.reconcile.last.source", wordAddress);
    metrics::setState("pgxp.vertex.reconcile.last.packet_head", packetHead);
    metrics::setState("pgxp.vertex.reconcile.last.opcode", opcode);
    metrics::setState("pgxp.vertex.reconcile.last.word_index", wordIndex);
    metrics::setState("pgxp.vertex.reconcile.last.packed", packedWord);
    metrics::setState("pgxp.vertex.reconcile.last.value", precision.value);
    metrics::setState("pgxp.vertex.reconcile.last.flags", precision.flags);
    metrics::setState("pgxp.vertex.reconcile.last.origin",
                      static_cast<int64_t>(precision.origin));
    metrics::setState("pgxp.vertex.reconcile.last.ram_lookup",
                      static_cast<int64_t>(precision.ramLookup));
    metrics::setState("pgxp.vertex.reconcile.last.path",
                      static_cast<int64_t>(precision.submissionPath));
    metrics::setState("pgxp.vertex.reconcile.last.component_mask",
                      reconciledMask);
    metrics::setState("pgxp.vertex.reconcile.last.x_source",
                      static_cast<int64_t>(resolved.xSource));
    metrics::setState("pgxp.vertex.reconcile.last.y_source",
                      static_cast<int64_t>(resolved.ySource));
    metrics::setState("pgxp.vertex.reconcile.last.original_x_bits",
                      diagnosticFloatBits(precision.x));
    metrics::setState("pgxp.vertex.reconcile.last.original_y_bits",
                      diagnosticFloatBits(precision.y));
    metrics::setState("pgxp.vertex.reconcile.last.original_z_bits",
                      diagnosticDoubleBits(precision.z));
    metrics::setState("pgxp.vertex.reconcile.last.original_qx_bits",
                      diagnosticDoubleBits(precision.qx));
    metrics::setState("pgxp.vertex.reconcile.last.original_qy_bits",
                      diagnosticDoubleBits(precision.qy));
    metrics::setState("pgxp.vertex.reconcile.last.near_depth_bits",
                      diagnosticDoubleBits(precision.nearDepth));
    metrics::setState("pgxp.vertex.reconcile.last.projection_id",
                      static_cast<int64_t>(precision.projectionId));
    metrics::setState("pgxp.vertex.reconcile.last.projection_context",
                      static_cast<int64_t>(precision.projectionContext));
    metrics::setState("pgxp.vertex.reconcile.last.signed_projection",
                      precision.hasSignedProjection(packedWord) ? 1 : 0);
    metrics::setState("pgxp.vertex.reconcile.last.native_x", nativeX);
    metrics::setState("pgxp.vertex.reconcile.last.native_y", nativeY);
    metrics::setState("pgxp.vertex.reconcile.last.dx_q16",
                      diagnosticQ16(deltaX));
    metrics::setState("pgxp.vertex.reconcile.last.dy_q16",
                      diagnosticQ16(deltaY));
    if ((reconciledMask & PGXPResolvedVertex::RECONCILED_X) != 0) {
      metrics::count("pgxp.vertex.reconcile.x");
      if (resolved.xSource == PGXPCoordinateSource::NativeNonFinite)
        metrics::count("pgxp.vertex.reconcile.x.nonfinite");
      else if (resolved.xSource == PGXPCoordinateSource::NativeGuardBand)
        metrics::count("pgxp.vertex.reconcile.x.guard_band");
      else
        metrics::count("pgxp.vertex.reconcile.x.delta");
    }
    if ((reconciledMask & PGXPResolvedVertex::RECONCILED_Y) != 0) {
      metrics::count("pgxp.vertex.reconcile.y");
      if (resolved.ySource == PGXPCoordinateSource::NativeNonFinite)
        metrics::count("pgxp.vertex.reconcile.y.nonfinite");
      else if (resolved.ySource == PGXPCoordinateSource::NativeGuardBand)
        metrics::count("pgxp.vertex.reconcile.y.guard_band");
      else
        metrics::count("pgxp.vertex.reconcile.y.delta");
    }
    if (reconciledMask ==
        (PGXPResolvedVertex::RECONCILED_X | PGXPResolvedVertex::RECONCILED_Y))
      metrics::count("pgxp.vertex.reconcile.xy");
  }

  const bool frontFacingDepth = resolved.hasDepth;
  const bool signedProjection = resolved.hasSignedProjection;
  const bool validXYZ = frontFacingDepth || signedProjection;
  const bool validXY = precision.hasXY(packedWord);
  const bool stale = precision.flags != 0 && precision.value != packedWord;
  const PrecisionClass precisionClass = frontFacingDepth ? PrecisionClass::XYZ
                                        : signedProjection
                                            ? PrecisionClass::SignedProjection
                                        : validXY ? PrecisionClass::XYOnly
                                        : stale   ? PrecisionClass::Stale
                                                  : PrecisionClass::None;

  const bool lookupHit =
      precision.ramLookup == PGXPRamLookup::HitXY ||
      precision.ramLookup == PGXPRamLookup::HitXYZ ||
      precision.ramLookup == PGXPRamLookup::HitSignedProjection;
  const bool projectionMatch = recentlyProjected(packedWord);

  SemanticClass semantic;
  FallbackReason reason;
  if (screen2D) {
    semantic = SemanticClass::Screen2D;
    // Keep the historical metric key stable: it now aggregates every
    // conservatively proven display-anchored polygon composite.
    reason = decision.reason == PGXPSemanticReason::DisplayAnchoredComposite
                 ? FallbackReason::DisplayAnchoredComposite
                 : FallbackReason::Intentional2D;
  } else if (validXYZ) {
    semantic = SemanticClass::Projected3D;
    reason = FallbackReason::None;
  } else if (validXY) {
    semantic = SemanticClass::Projected3D;
    reason = FallbackReason::XYOnly;
  } else if (lookupHit) {
    semantic = SemanticClass::Projected3D;
    reason = FallbackReason::QueueDesync;
  } else if (stale || precision.ramLookup == PGXPRamLookup::Stale ||
             projectionMatch) {
    semantic = SemanticClass::Projected3D;
    reason = FallbackReason::PacketCopyLoss;
  } else {
    // Polygon-family operands belong to the Projected3D semantic pass even
    // when the CPU generated their packed coordinates. The reason records
    // the missing provenance; it is not a third rendering-domain class.
    semantic = SemanticClass::Projected3D;
    reason = FallbackReason::CpuGenerated;
  }

  PGXPSubmissionPath path = precision.submissionPath;
  if (static_cast<std::size_t>(path) >= pathCount)
    path = PGXPSubmissionPath::DirectMmio;
  PGXPRamLookup lookup = precision.ramLookup;
  if (static_cast<std::size_t>(lookup) >= lookupCount)
    lookup = PGXPRamLookup::Unobserved;
  PGXPPrimitive safePrimitive = primitive;
  if (primitiveIndex >= primitiveCount)
    safePrimitive = PGXPPrimitive::Rectangle;

  countBreakdown(globalBreakdown, semantic, precisionClass, reason, lookup);
  countBreakdown(opcodeBreakdowns[opcode], semantic, precisionClass, reason,
                 lookup);
  countBreakdown(primitiveBreakdowns[static_cast<std::size_t>(safePrimitive)],
                 semantic, precisionClass, reason, lookup);
  countBreakdown(pathBreakdowns[static_cast<std::size_t>(path)], semantic,
                 precisionClass, reason, lookup);

  if (semantic == SemanticClass::Projected3D) {
    xyzRequired.fetch_add(1, std::memory_order_relaxed);
    if (validXYZ)
      xyzValid.fetch_add(1, std::memory_order_relaxed);
    else
      xyzMissing.fetch_add(1, std::memory_order_relaxed);
  } else if (semantic == SemanticClass::Screen2D) {
    xyzNotApplicable.fetch_add(1, std::memory_order_relaxed);
  }

  if (!validXYZ)
    countSource(makeSourceKey(sourceAddress, opcode, wordIndex, path,
                              safePrimitive, precisionClass, reason, lookup));

  // Headless display drivers can block the host thread inside present(), so a
  // forced smoke-test shutdown is not guaranteed to reach main_host's final
  // dump. This opt-in checkpoint is emitted by the emulation thread itself.
  const uint64_t interval = liveFlushInterval();
  const uint64_t total = globalBreakdown.total.load(std::memory_order_relaxed);
  if (interval != 0 && (total == 1 || total % interval == 0)) {
    publishPGXPStageMetrics();
    metrics::dumpJson();
  }
}

void publishPGXPStageMetrics() {
  std::lock_guard<std::mutex> lock(publishMutex);
  for (std::size_t i = 0; i < stageCounters.size(); ++i) {
    const uint64_t value = stageCounters[i].load(std::memory_order_relaxed);
    metrics::count(stageNames[i], value - stagePublished[i]);
    stagePublished[i] = value;
  }

  const uint64_t classifiedDelta =
      globalBreakdown.total.load(std::memory_order_relaxed) -
      globalPublished.total;
  publishBreakdown("pgxp.vertex", globalBreakdown, globalPublished, true);
  metrics::count("pgxp.vertex.class.unknown", 0);
  metrics::count("pgxp.vertex.classified", classifiedDelta);
  metrics::count("pgxp.vertex.classified.total", classifiedDelta);
  metrics::count("pgxp.vertex.classified.unknown", 0);
  metrics::setState("pgxp.vertex.classified.current",
                    static_cast<int64_t>(
                        globalBreakdown.total.load(std::memory_order_relaxed)));
  metrics::setState("pgxp.vertex.unknown.current", 0);

  for (std::size_t opcode = 0; opcode < opcodeBreakdowns.size(); ++opcode) {
    if (opcodeBreakdowns[opcode].total.load(std::memory_order_relaxed) == 0)
      continue;
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "pgxp.vertex.op.%02zX", opcode);
    publishBreakdown(prefix, opcodeBreakdowns[opcode], opcodePublished[opcode]);
  }
  for (std::size_t primitive = 0; primitive < primitiveCount; ++primitive) {
    if (primitiveBreakdowns[primitive].total.load(std::memory_order_relaxed) ==
        0)
      continue;
    publishBreakdown(
        std::string("pgxp.vertex.primitive.") + primitiveNames[primitive],
        primitiveBreakdowns[primitive], primitivePublished[primitive]);
  }
  for (std::size_t path = 0; path < pathCount; ++path) {
    if (pathBreakdowns[path].total.load(std::memory_order_relaxed) == 0)
      continue;
    publishBreakdown(std::string("pgxp.vertex.path.") + pathNames[path],
                     pathBreakdowns[path], pathPublished[path]);
  }

  publishAtomic("pgxp.vertex.xyz.required", xyzRequired, xyzRequiredPublished,
                true);
  publishAtomic("pgxp.vertex.xyz.valid", xyzValid, xyzValidPublished, true);
  publishAtomic("pgxp.vertex.xyz.missing", xyzMissing, xyzMissingPublished,
                true);
  publishAtomic("pgxp.vertex.xyz.not_applicable", xyzNotApplicable,
                xyzNotApplicablePublished, true);
  metrics::setState(
      "pgxp.vertex.xyz.missing.current",
      static_cast<int64_t>(xyzMissing.load(std::memory_order_relaxed)));

  char metricName[192];
  for (SourceBucket &bucket : sourceBuckets) {
    const uint64_t storedKey =
        bucket.keyPlusOne.load(std::memory_order_acquire);
    if (storedKey == 0)
      continue;
    const uint64_t key = storedKey - 1;
    const uint64_t value = bucket.count.load(std::memory_order_relaxed);
    const uint64_t delta = value - bucket.published;
    if (delta == 0)
      continue;
    bucket.published = value;

    const uint32_t source = key & 0x3fffffu;
    const uint8_t opcode = (key >> 22) & 0xffu;
    const uint8_t wordIndex = (key >> 30) & 0x0fu;
    const auto path = static_cast<PGXPSubmissionPath>((key >> 34) & 0x7u);
    const auto reason = static_cast<FallbackReason>((key >> 37) & 0x7u);
    const auto primitive = static_cast<PGXPPrimitive>((key >> 40) & 0x0fu);
    const auto precision = static_cast<PrecisionClass>((key >> 44) & 0x7u);
    const auto lookup = static_cast<PGXPRamLookup>((key >> 47) & 0x7u);
    const char *sourceName = source == directSourceAddress ? "direct" : nullptr;
    if (sourceName) {
      std::snprintf(metricName, sizeof(metricName),
                    "pgxp.vertex.source.%s.op.%02X.w%u.%s.%s.%s.%s.%s",
                    sourceName, opcode, wordIndex,
                    pathNames[static_cast<std::size_t>(path)],
                    primitiveNames[static_cast<std::size_t>(primitive)],
                    precisionNames[static_cast<std::size_t>(precision)],
                    lookupNames[static_cast<std::size_t>(lookup)],
                    reasonNames[static_cast<std::size_t>(reason)]);
    } else if (source >= scratchpadSourceBase && source < scratchpadSourceEnd) {
      std::snprintf(
          metricName, sizeof(metricName),
          "pgxp.vertex.source.scratch.%03X.op.%02X.w%u.%s.%s.%s.%s.%s",
          static_cast<unsigned>(source - scratchpadSourceBase), opcode,
          wordIndex, pathNames[static_cast<std::size_t>(path)],
          primitiveNames[static_cast<std::size_t>(primitive)],
          precisionNames[static_cast<std::size_t>(precision)],
          lookupNames[static_cast<std::size_t>(lookup)],
          reasonNames[static_cast<std::size_t>(reason)]);
    } else {
      std::snprintf(metricName, sizeof(metricName),
                    "pgxp.vertex.source.%06X.op.%02X.w%u.%s.%s.%s.%s.%s",
                    source, opcode, wordIndex,
                    pathNames[static_cast<std::size_t>(path)],
                    primitiveNames[static_cast<std::size_t>(primitive)],
                    precisionNames[static_cast<std::size_t>(precision)],
                    lookupNames[static_cast<std::size_t>(lookup)],
                    reasonNames[static_cast<std::size_t>(reason)]);
    }
    metrics::count(metricName, delta);
  }
  publishAtomic("pgxp.vertex.source_overflow", sourceOverflow,
                sourceOverflowPublished, true);
}

void resetPGXPDiagnosticsForTesting() {
  std::lock_guard<std::mutex> lock(publishMutex);
  for (auto &counter : stageCounters)
    counter.store(0, std::memory_order_relaxed);
  stagePublished.fill(0);
  clearBreakdown(globalBreakdown, globalPublished);
  for (std::size_t i = 0; i < opcodeBreakdowns.size(); ++i)
    clearBreakdown(opcodeBreakdowns[i], opcodePublished[i]);
  for (std::size_t i = 0; i < primitiveBreakdowns.size(); ++i)
    clearBreakdown(primitiveBreakdowns[i], primitivePublished[i]);
  for (std::size_t i = 0; i < pathBreakdowns.size(); ++i)
    clearBreakdown(pathBreakdowns[i], pathPublished[i]);
  xyzRequired.store(0, std::memory_order_relaxed);
  xyzValid.store(0, std::memory_order_relaxed);
  xyzMissing.store(0, std::memory_order_relaxed);
  xyzNotApplicable.store(0, std::memory_order_relaxed);
  xyzRequiredPublished = 0;
  xyzValidPublished = 0;
  xyzMissingPublished = 0;
  xyzNotApplicablePublished = 0;
  for (auto &entry : recentProjections)
    entry.store(0, std::memory_order_relaxed);
  lastRamLookup = {};
  for (SourceBucket &bucket : sourceBuckets) {
    bucket.keyPlusOne.store(0, std::memory_order_relaxed);
    bucket.count.store(0, std::memory_order_relaxed);
    bucket.published = 0;
  }
  sourceOverflow.store(0, std::memory_order_relaxed);
  sourceOverflowPublished = 0;
}

} // namespace ps1
