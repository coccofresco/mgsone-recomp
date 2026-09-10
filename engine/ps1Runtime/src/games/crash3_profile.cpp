#include "runtime/games/crash3_profile.h"

#include "runtime/games/crash3_render_profile.h"
#include "runtime/games/crash3_scenery_construction.h"
#include "runtime/validated_index_cache.h"
#include "runtime/games/crash3_scenery_detail.h"

#include "runtime/cpu_context.h"
#include "runtime/gpu/gpu.h"
#include "runtime/gpu/modern_replay_visibility_audit.h"
#include "runtime/gpu/modern_replay_vulkan_abi.h"
#include "runtime/gte.h"
#include "runtime/gte_color_math.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include "runtime/pgxp.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ps1::games::crash3 {
namespace {

class ScopedCrash3MetricTimer {
public:
  explicit ScopedCrash3MetricTimer(std::string_view metric) noexcept
      : metric_(metric), enabled_(metrics::enabled()) {
    if (enabled_)
      start_ = Clock::now();
  }

  ~ScopedCrash3MetricTimer() {
    if (!enabled_)
      return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - start_);
    metrics::count(metric_, static_cast<uint64_t>(std::max<int64_t>(
                                elapsed.count(), int64_t{0})));
  }

private:
  using Clock = std::chrono::steady_clock;
  std::string_view metric_;
  bool enabled_ = false;
  Clock::time_point start_{};
};

ProfileOptions g_options{};
int32_t g_rendererScaleShift = -1;
uint32_t g_rendererObjectSource = 0;
uint32_t g_rendererObjectCaptureId = 0;
uint32_t g_rendererLogicalObject = 0;
bool g_rendererOrderingTableBasesCaptured = false;
constexpr std::size_t kObjectProjectionCommandJournalSize = 1u << 18u;
constexpr std::size_t kObjectProjectionCommandJournalMask =
    kObjectProjectionCommandJournalSize - 1u;
std::array<std::atomic<uint64_t>, kObjectProjectionCommandJournalSize>
    g_objectProjectionCommandJournal{};
std::array<std::atomic<uint64_t>, kObjectProjectionCommandJournalSize>
    g_objectProjectionLogicalObjectJournal{};
constexpr uint8_t kNoRendererRotationCapture = UINT8_MAX;
uint8_t g_rendererRotationCaptureNext = kNoRendererRotationCapture;

struct ProjectedBillboardCentreRecord {
  uint32_t packedSxy = 0;
  PGXPValue precision{};
  uint64_t frame = 0;
};

std::mutex g_projectedBillboardMutex;
std::unordered_map<uint32_t, ProjectedBillboardCentreRecord>
    g_projectedBillboardCentres;
uint64_t g_projectedBillboardPruneFrame = 0;
constexpr uint64_t kProjectedBillboardFrameWindow = 2u;

void clearProjectedBillboardCentres() noexcept {
  std::scoped_lock lock(g_projectedBillboardMutex);
  g_projectedBillboardCentres.clear();
  g_projectedBillboardPruneFrame = 0;
}

// `%d p l%c` in the PAL executable (loaded address 0x800100A8). Publishing
// the payload before the release-store generation gives the host a lock-free,
// internally ordered level-transition signal without touching guest state.
constexpr uint32_t kDemoLevelPrintfFormatAddress = UINT32_C(0x800100A8);
std::atomic<uint64_t> g_demoLevelLoadGeneration{0u};
std::atomic<uint32_t> g_demoLevelLoadOrdinal{0u};
std::atomic<uint8_t> g_demoLevelCode{0u};

constexpr uint64_t kTagMarker = UINT64_C(0xC3A3000000000000);
constexpr uint64_t kTagMarkerMask = UINT64_C(0xFFFF000000000000);
constexpr uint64_t kTagPayloadMask = UINT64_C(0x0000FFFFFFFFFFFF);
// Crash projects procedural surfaces before their faces are admitted to the
// ordering table. Give those sources a disjoint structured carrier so
// adjacent Vulkan endpoints can retain the same grid vertex without looking
// at screen space. Bits 41..38 select the exact source schema; the remaining
// 38 bits are schema-owned. Both walkers carry a toroidal cell and the source
// resource/work buffer, never the order in which a moving view visits it.
// Their transposed traversals are canonicalized to the same world axes.
constexpr uint64_t kProceduralProjectionIdentityFlag = UINT64_C(0x21) << 42u;
constexpr uint64_t kProceduralProjectionPayloadMask = (UINT64_C(1) << 42u) - 1u;
constexpr uint64_t kProceduralProjectionDataMask = (UINT64_C(1) << 38u) - 1u;
constexpr uint64_t kProceduralProjectionSchemaShift = 38u;
constexpr uint64_t kProceduralProjectionOrdinalSchema =
    UINT64_C(1) << kProceduralProjectionSchemaShift;
constexpr uint64_t kProceduralProjectionWaterGridSchema =
    UINT64_C(2) << kProceduralProjectionSchemaShift;
constexpr uint64_t kProceduralProjectionTerrainGridSchema =
    UINT64_C(3) << kProceduralProjectionSchemaShift;
constexpr uint32_t kRendererScratchBase = UINT32_C(0x1F800000);
// func_80040E38 derives this exact right-shift count before entering the
// scratchpad polygon renderer. It scales TRX/TRY/TRZ and model coordinates
// together so the GTE's signed 16-bit inputs do not overflow.
constexpr uint32_t kRendererScaleShiftAddress = UINT32_C(0x1F8000EC);

// Verified directly against SCES-01420's func_8001A680 object constructor and
// func_8001B858 renderer-list builder. CrashUtils documents the same semantic
// sequence for the earlier engine, but its Crash 1 self offset is deliberately
// not reused here.
constexpr uint32_t kCrash3ObjectBytes = 0x124u;
constexpr uint32_t kCrash3ObjectSelfOffset = 0x40u;
constexpr uint32_t kCrash3RendererNodeObjectOffset = 0x10u;
constexpr uint32_t kCrash3RendererNodeBytes = 0x14u;
// 0x80061BD8 is the PAL renderer owner established by func_8001166C and
// func_8001B858. owner+0x0c points at the transient renderer state; +8336 is
// the normal type-1/2/5 linked-list head and +8348 is the separate type-4
// list. The host only reads these structures.
constexpr uint32_t kCrash3RendererOwnerAddress = UINT32_C(0x80061BD8);
constexpr uint32_t kCrash3RendererOwnerStateOffset = 0x0cu;
constexpr uint32_t kCrash3RendererListHeadOffset = 8336u;
constexpr uint32_t kMaxCrash3RendererListNodes = 128u;
constexpr uint32_t kObjectSourcePhysicalMask = (UINT32_C(1) << 21u) - 1u;
constexpr uint32_t kObjectCaptureMask = (UINT32_C(1) << 11u) - 1u;
constexpr uint32_t kKusegObjectCaptureAliasLast = 3u;
constexpr uint32_t kKseg0ObjectCaptureAliasFirst = UINT32_C(0x80000000) >> 21u;
constexpr uint32_t kKseg0ObjectCaptureAliasLast = UINT32_C(0x80600000) >> 21u;
constexpr uint32_t kKseg1ObjectCaptureAliasFirst = UINT32_C(0xA0000000) >> 21u;
constexpr uint32_t kKseg1ObjectCaptureAliasLast = UINT32_C(0xA0600000) >> 21u;

bool isCanonicalGuestRamRange(uint32_t address, uint32_t byteCount,
                              uint32_t alignment = 1u) noexcept {
  if (address == 0u || byteCount == 0u || alignment == 0u ||
      (address & (alignment - 1u)) != 0u || byteCount > Memory::RAM_SIZE)
    return false;
  const bool kuseg = address < UINT32_C(0x00800000);
  const bool kseg0 =
      address >= UINT32_C(0x80000000) && address < UINT32_C(0x80800000);
  const bool kseg1 =
      address >= UINT32_C(0xA0000000) && address < UINT32_C(0xA0800000);
  if (!kuseg && !kseg0 && !kseg1)
    return false;
  const uint32_t physical = Memory::toPhysical(address);
  return physical <= Memory::RAM_SIZE - byteCount;
}

bool isCrash3PresentationObject(const Memory &memory,
                                uint32_t address) noexcept {
  return isCanonicalGuestRamRange(address, kCrash3ObjectBytes, 4u) &&
         memory.read32(address) == 1u &&
         memory.read32(address + kCrash3ObjectSelfOffset) == address;
}

bool hasExtendedObjectCallsiteIdentity(uint64_t callsite) noexcept {
  const uint32_t encoded = static_cast<uint32_t>(callsite >> 32u);
  return encoded != 0u &&
         !isCanonicalGuestRamRange(encoded, kCrash3RendererNodeBytes, 4u);
}

bool isDynamicObjectRendererProjection(uint32_t pc) noexcept {
  // Exact RTPT/RTPS sites reachable from the SCES-01420 type-1/type-5 render
  // functions after func_80040E38 installs the current node transform. They
  // are deliberately distinct from func_8004103C's resident-scenery sites.
  switch (pc) {
  case UINT32_C(0x8003CE34):
  case UINT32_C(0x8003CFB8):
  case UINT32_C(0x8003D6DC):
  case UINT32_C(0x8003D704):
  case UINT32_C(0x8003DE64):
  case UINT32_C(0x8003E038):
  case UINT32_C(0x8003E094):
  case UINT32_C(0x8003E0E8):
  case UINT32_C(0x8003E130):
  case UINT32_C(0x8003E590):
  case UINT32_C(0x8003E5B0):
  case UINT32_C(0x8003EE34):
  case UINT32_C(0x8003EEAC):
  case UINT32_C(0x8003F148):
  case UINT32_C(0x8003F19C):
  case UINT32_C(0x8003FE64):
  case UINT32_C(0x8003FEF4):
  case UINT32_C(0x800401C8):
  case UINT32_C(0x80040254):
  case UINT32_C(0x800405C4):
  case UINT32_C(0x800406D4):
  case UINT32_C(0x800407EC):
    return true;
  default:
    return false;
  }
}

bool isResidentSceneryRendererProjection(uint32_t pc) noexcept {
  return pc == UINT32_C(0x800417AC) || pc == UINT32_C(0x80041A30) ||
         pc == UINT32_C(0x80041AE0);
}

enum class PresentationProjectionScale : uint8_t {
  NativeWorld,
  QuarterScalePresentation,
  FirstEndpointLine,
  CameraBackgroundPoint,
  Unsupported,
};

PresentationProjectionScale presentationProjectionScale(uint32_t pc) noexcept {
  switch (pc) {
  case UINT32_C(0x80039EB8): // line: first SZ >> 6, not AVSZ3/4
    return PresentationProjectionScale::FirstEndpointLine;
  case UINT32_C(0x80043648): // camera-rotated point cloud, fixed far OT insertion
    return PresentationProjectionScale::CameraBackgroundPoint;
  case UINT32_C(0x8003CE34):
  case UINT32_C(0x8003CFB8):
  case UINT32_C(0x8003D6DC):
  case UINT32_C(0x8003D704): // fourth corner after the 0x8003D6DC RTPT
  case UINT32_C(0x8003DE64):
  case UINT32_C(0x8003E038):
  case UINT32_C(0x8003E094):
  case UINT32_C(0x8003E0E8):
  case UINT32_C(0x8003E130):
  case UINT32_C(0x8003E590):
  case UINT32_C(0x8003E5B0):
  case UINT32_C(0x8003EE34):
  case UINT32_C(0x8003EEAC):
  case UINT32_C(0x8003F148):
  case UINT32_C(0x8003F19C):
  case UINT32_C(0x8003FE64):
  case UINT32_C(0x8003FEF4):
  case UINT32_C(0x800401C8):
  case UINT32_C(0x80040254):
  case UINT32_C(0x800405C4):
  case UINT32_C(0x800406D4):
  case UINT32_C(0x800407EC):
  case UINT32_C(0x80019414):
  case UINT32_C(0x800196A0): // fourth corner after the 0x80019414 RTPT
    return PresentationProjectionScale::NativeWorld;
  case UINT32_C(0x800417AC):
  case UINT32_C(0x80041A30):
  case UINT32_C(0x80041AE0):
  case UINT32_C(0x80044938):
    return PresentationProjectionScale::QuarterScalePresentation;
  default:
    return PresentationProjectionScale::Unsupported;
  }
}

struct PresentationDepthEntry {
  std::array<uint64_t, 3> tags{};
  std::array<uint32_t, 3> gp0NodeOffsets{};
  float multiplier = 1.0f;
};

// Derived from the 15-frame Crash 3 discovery/holdout corpus. These are the
// 47 non-neutral entries from the former 330-entry generic WIP manifest. The
// native renderer's Crash-owned construction tag maps all 330 old entries
// one-to-one with no ambiguity; neutral 1x entries intentionally need no row.
constexpr std::array<PresentationDepthEntry, 47> kPresentationDepthEntries{{
    {{{UINT64_C(17775705987405), UINT64_C(136358640487534),
       UINT64_C(238903054247183)}},
     {{8u, 20u, 32u}},
     2.0f},
    {{{UINT64_C(19643073132264), UINT64_C(111173786096739),
       UINT64_C(148855233568186)}},
     {{8u, 16u, 24u}},
     61.0f},
    {{{UINT64_C(37073890796208), UINT64_C(145770839657582),
       UINT64_C(239583674599569)}},
     {{8u, 16u, 24u}},
     61.0f},
    {{{UINT64_C(40351718895446), UINT64_C(149723020671431),
       UINT64_C(229203277453264)}},
     {{8u, 20u, 32u}},
     2.0f},
    {{{UINT64_C(40452086462423), UINT64_C(202274499793102),
       UINT64_C(164603657610929)}},
     {{8u, 16u, 24u}},
     31.0f},
    {{{UINT64_C(46644924911383), UINT64_C(111011580288900),
       UINT64_C(76795580845528)}},
     {{8u, 16u, 24u}},
     56.0f},
    {{{UINT64_C(48109892555140), UINT64_C(203684489700695),
       UINT64_C(141883570469862)}},
     {{8u, 16u, 24u}},
     61.0f},
    {{{UINT64_C(49469183266029), UINT64_C(255555278493916),
       UINT64_C(40452086462423)}},
     {{8u, 16u, 24u}},
     31.0f},
    {{{UINT64_C(53293488153090), UINT64_C(49469183266029),
       UINT64_C(255555278493916)}},
     {{8u, 16u, 24u}},
     31.0f},
    {{{UINT64_C(55944386758357), UINT64_C(193153867248809),
       UINT64_C(227506986528512)}},
     {{8u, 16u, 24u}},
     31.0f},
    {{{UINT64_C(66776293513770), UINT64_C(74621701234706),
       UINT64_C(234118975536195)}},
     {{8u, 16u, 24u}},
     56.0f},
    {{{UINT64_C(66776293513770), UINT64_C(85488331359350),
       UINT64_C(74621701234706)}},
     {{8u, 16u, 24u}},
     56.0f},
    {{{UINT64_C(68083729284809), UINT64_C(248084433996306),
       UINT64_C(248680697855115)}},
     {{8u, 20u, 32u}},
     2.0f},
    {{{UINT64_C(74621701234706), UINT64_C(234118975536195),
       UINT64_C(46644924911383)}},
     {{8u, 16u, 24u}},
     56.0f},
    {{{UINT64_C(76795580845528), UINT64_C(148499210866801),
       UINT64_C(92017237801997)}},
     {{8u, 16u, 24u}},
     56.0f},
    {{{UINT64_C(80087381450152), UINT64_C(171703781015179),
       UINT64_C(104160765799834)}},
     {{8u, 16u, 24u}},
     31.0f},
    {{{UINT64_C(84702087837808), UINT64_C(174526930855531),
       UINT64_C(156184399025394)}},
     {{8u, 16u, 24u}},
     61.0f},
    {{{UINT64_C(92017237801997), UINT64_C(37298090961138),
       UINT64_C(54189132134486)}},
     {{8u, 16u, 24u}},
     56.0f},
    {{{UINT64_C(111011580288900), UINT64_C(76795580845528),
       UINT64_C(148499210866801)}},
     {{8u, 16u, 24u}},
     56.0f},
    {{{UINT64_C(111173786096739), UINT64_C(148855233568186),
       UINT64_C(161097402611477)}},
     {{8u, 16u, 24u}},
     61.0f},
    {{{UINT64_C(119186596688585), UINT64_C(84702087837808),
       UINT64_C(174526930855531)}},
     {{8u, 16u, 24u}},
     61.0f},
    {{{UINT64_C(135579887910613), UINT64_C(40351718895446),
       UINT64_C(149723020671431)}},
     {{8u, 20u, 32u}},
     2.0f},
    {{{UINT64_C(136358640487534), UINT64_C(238903054247183),
       UINT64_C(217146768900696)}},
     {{8u, 20u, 32u}},
     2.0f},
    {{{UINT64_C(141441885377585), UINT64_C(197421097017434),
       UINT64_C(243363938996739)}},
     {{8u, 20u, 32u}},
     2.0f},
    {{{UINT64_C(141441885377585), UINT64_C(243363938996739),
       UINT64_C(220022982740244)}},
     {{8u, 20u, 32u}},
     2.0f},
    {{{UINT64_C(141883570469862), UINT64_C(119186596688585),
       UINT64_C(84702087837808)}},
     {{8u, 16u, 24u}},
     61.0f},
    {{{UINT64_C(145770839657582), UINT64_C(239583674599569),
       UINT64_C(19643073132264)}},
     {{8u, 16u, 24u}},
     61.0f},
    {{{UINT64_C(148499210866801), UINT64_C(92017237801997),
       UINT64_C(37298090961138)}},
     {{8u, 16u, 24u}},
     56.0f},
    {{{UINT64_C(148855233568186), UINT64_C(161097402611477),
       UINT64_C(48109892555140)}},
     {{8u, 16u, 24u}},
     61.0f},
    {{{UINT64_C(149723020671431), UINT64_C(229203277453264),
       UINT64_C(68083729284809)}},
     {{8u, 20u, 32u}},
     2.0f},
    {{{UINT64_C(161097402611477), UINT64_C(48109892555140),
       UINT64_C(203684489700695)}},
     {{8u, 16u, 24u}},
     61.0f},
    {{{UINT64_C(161991340967132), UINT64_C(17775705987405),
       UINT64_C(136358640487534)}},
     {{8u, 20u, 32u}},
     2.0f},
    {{{UINT64_C(164603657610929), UINT64_C(80087381450152),
       UINT64_C(171703781015179)}},
     {{8u, 16u, 24u}},
     31.0f},
    {{{UINT64_C(171703781015179), UINT64_C(104160765799834),
       UINT64_C(164895012473045)}},
     {{8u, 16u, 24u}},
     31.0f},
    {{{UINT64_C(193153867248809), UINT64_C(227506986528512),
       UINT64_C(247525875253363)}},
     {{8u, 16u, 24u}},
     31.0f},
    {{{UINT64_C(202274499793102), UINT64_C(164603657610929),
       UINT64_C(80087381450152)}},
     {{8u, 16u, 24u}},
     31.0f},
    {{{UINT64_C(203684489700695), UINT64_C(141883570469862),
       UINT64_C(119186596688585)}},
     {{8u, 16u, 24u}},
     61.0f},
    {{{UINT64_C(220022982740244), UINT64_C(135579887910613),
       UINT64_C(40351718895446)}},
     {{8u, 20u, 32u}},
     2.0f},
    {{{UINT64_C(227506986528512), UINT64_C(247525875253363),
       UINT64_C(53293488153090)}},
     {{8u, 16u, 24u}},
     31.0f},
    {{{UINT64_C(229203277453264), UINT64_C(68083729284809),
       UINT64_C(248084433996306)}},
     {{8u, 20u, 32u}},
     2.0f},
    {{{UINT64_C(234118975536195), UINT64_C(46644924911383),
       UINT64_C(111011580288900)}},
     {{8u, 16u, 24u}},
     56.0f},
    {{{UINT64_C(239583674599569), UINT64_C(19643073132264),
       UINT64_C(111173786096739)}},
     {{8u, 16u, 24u}},
     61.0f},
    {{{UINT64_C(243363938996739), UINT64_C(220022982740244),
       UINT64_C(135579887910613)}},
     {{8u, 20u, 32u}},
     2.0f},
    {{{UINT64_C(247525875253363), UINT64_C(53293488153090),
       UINT64_C(49469183266029)}},
     {{8u, 16u, 24u}},
     31.0f},
    {{{UINT64_C(248084433996306), UINT64_C(248680697855115),
       UINT64_C(161991340967132)}},
     {{8u, 20u, 32u}},
     2.0f},
    {{{UINT64_C(248680697855115), UINT64_C(161991340967132),
       UINT64_C(17775705987405)}},
     {{8u, 20u, 32u}},
     2.0f},
    {{{UINT64_C(255555278493916), UINT64_C(40452086462423),
       UINT64_C(202274499793102)}},
     {{8u, 16u, 24u}},
     31.0f},
}};

static_assert(std::is_sorted(kPresentationDepthEntries.begin(),
                             kPresentationDepthEntries.end(),
                             [](const PresentationDepthEntry &left,
                                const PresentationDepthEntry &right) {
                               return left.tags < right.tags;
                             }));

void hash32(uint64_t &hash, uint32_t value) noexcept {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    hash ^= (value >> shift) & 0xffu;
    hash *= UINT64_C(1099511628211);
  }
}

uint32_t mapViewportCullPresentationSxy(uint32_t packedSxy,
                                        uint32_t guardPixels = 0u) noexcept {
  const int32_t x = static_cast<int16_t>(packedSxy & 0xffffu);
  int32_t mapped = screen2DPresentationX(x, 0, 511, x);
  if (guardPixels != 0u) {
    // The final image remains 512 pixels wide. Compress only the cull-local
    // witness so the hard-coded 0..511 test admits a wider interval of the
    // true presentation projection. For G pixels per side, [-G,511+G] maps
    // back to [0,511]; packet/PGXP coordinates retain their real positions
    // and Vulkan clips them against the unchanged output scissor.
    constexpr int64_t kCenter = 256;
    constexpr int64_t kViewportWidth = 512;
    const int64_t denominator =
        kViewportWidth + 2 * static_cast<int64_t>(guardPixels);
    const int64_t delta = static_cast<int64_t>(mapped) - kCenter;
    const int64_t magnitude = delta < 0 ? -delta : delta;
    const int64_t scaledMagnitude =
        (magnitude * kViewportWidth + denominator / 2) / denominator;
    mapped = static_cast<int32_t>(
        kCenter + (delta < 0 ? -scaledMagnitude : scaledMagnitude));
  }
  return (packedSxy & 0xffff0000u) |
         static_cast<uint16_t>(static_cast<int16_t>(mapped));
}

uint32_t translateSceneryCullGuardSxy(uint32_t packedSxy) noexcept {
  const int32_t guard = static_cast<int32_t>(sceneryCullGuardPixels());
  if (guard == 0)
    return packedSxy;
  const int32_t x = static_cast<int16_t>(packedSxy) + guard;
  const int16_t shifted = static_cast<int16_t>(
      std::clamp(x, static_cast<int32_t>(std::numeric_limits<int16_t>::min()),
                 static_cast<int32_t>(std::numeric_limits<int16_t>::max())));
  return (packedSxy & UINT32_C(0xffff0000)) | static_cast<uint16_t>(shifted);
}

enum class ScenerySelectionMode : uint8_t {
  Original,
  Audit,
  Adjacent,
  Symmetric,
  Geometric,
  GeometricResident,
  Bypass,
};

struct ResidentSceneryWorld {
  uint32_t recordAddress = 0;
  uint32_t infoAddress = 0;
  uint32_t triangleCount = 0;
  uint32_t quadCount = 0;
};

struct ScenerySelectionState {
  uint32_t listAddress = 0;
  ScenerySelectionMode mode = ScenerySelectionMode::Original;
  // A resolved selection can deliberately remain inactive after a checked
  // fail-closed result. Keep that distinct from the transient, unresolved
  // state after configure/savestate restore: otherwise every threaded entry
  // read retries the same expensive resident reconstruction.
  bool resolved = false;
  bool virtualActive = false;
  std::vector<uint16_t> entries;
};

struct SceneryListSnapshot {
  uint32_t sourceAddress = 0;
  uint32_t tableCursor = 0;
  bool forward = false;
  std::vector<uint16_t> entries;
};

struct SceneryDeltaHistory {
  uint32_t lastDestination = 0;
  std::vector<SceneryListSnapshot> preceding;
};

struct SceneryPathCache {
  uint32_t entryBase = 0;
  uint32_t stateCount = 0;
  uint32_t currentIndex = 0;
  bool valid = false;
  bool detailTracePending = false;
  uint64_t revision = 0;
  std::vector<std::vector<uint16_t>> states;
  std::vector<std::pair<uint32_t, uint64_t>> variantBindings;
  bool variantBoundsValid = false;
};

// Diagnostic-only bridge from Crash's one-entry-ahead SLST decoder to the
// GTE projections emitted by its threaded polygon renderer. The ordinary
// renderer never consults this state. When explicitly armed it gives every
// projected vertex a stable world/class/polygon identity, independent of the
// alternating packet buffer, camera position and host submission number.
struct SceneryProjectionIdentityState {
  bool enabled = false;
  bool traceEnabled = false;
  bool currentValid = false;
  bool prefetchedValid = false;
  uint16_t current = 0;
  uint16_t prefetched = 0;
  uint32_t lastCursor = 0;
  uint32_t captureId = 0;
};

enum class SceneryOcclusionMode : uint8_t {
  Off,
  CertifiedWarpEdge,
  StrictScenery,
};

enum class PresentationCullStrategy : uint8_t {
  Native,
  Exact16x9,
  GuardBand,
};

ScenerySelectionState g_scenerySelection{};
SceneryDeltaHistory g_sceneryDeltaHistory{};
SceneryPathCache g_sceneryPathCache{};
uint64_t g_sceneryPathRevision = 0u;
struct SceneryZoneProgram {
  uint32_t owner = 0u;
  uint64_t revision = 0u;
  uint32_t stateCount = 0u;
  bool pathsTraced = false;
  std::vector<ScenerySourcePath> sources;
  std::vector<SceneryBoundSourcePath> registeredSources;
  std::vector<std::pair<uint32_t, uint64_t>> resources;
  std::vector<SceneryPathCache> paths;
};
SceneryZoneProgram g_sceneryZoneProgram;

bool sceneryVariantsEnabled() noexcept {
  const char *sidecar = std::getenv("PS1_CRASH3_VULKAN_SCENE_SIDECAR");
  const char *policy = std::getenv("PS1_CRASH3_SCENERY_VARIANTS");
  return g_options.enabled && sidecar != nullptr && *sidecar == '1' &&
         (policy == nullptr || std::strcmp(policy, "native") != 0);
}
SceneryProjectionIdentityState g_sceneryProjectionIdentity{};
SceneryOcclusionMode g_sceneryOcclusionMode =
    SceneryOcclusionMode::CertifiedWarpEdge;
ResidentPresentationMode g_residentPresentationMode =
    ResidentPresentationMode::Off;
bool g_residentCohortPresentationEnabled = true;
ObjectPresentationMode g_objectPresentationMode = ObjectPresentationMode::Off;
PresentationCullStrategy g_presentationCullStrategy =
    PresentationCullStrategy::Native;
std::mutex g_residentPresentationMutex;
std::deque<std::shared_ptr<const ResidentPresentationSnapshot>>
    g_residentPresentationSnapshots;
uint32_t g_residentPresentationCaptureId = 0;
struct ResidentOrderingTableProofState {
  uint32_t captureId = 0;
  uint64_t topologyFingerprint = 0;
  uint64_t matches = 0;
  uint64_t mismatches = 0;
  uint64_t unavailable = 0;
  uint32_t mismatchSamples = 0;
  std::vector<uint16_t> expectedBuckets;
  std::vector<uint8_t> expectedValid;
  std::vector<std::array<std::array<int16_t, 3>, 3>> expectedModelInputs;
  std::vector<uint32_t> expectedModelInputGenerations;
  uint32_t modelInputGeneration = 0;
  std::array<uint32_t, 8> expectedProjectionControls{};
  bool expectedProjectionControlsValid = false;
};
std::mutex g_residentOrderingTableProofMutex;
ResidentOrderingTableProofState g_residentOrderingTableProof;
struct ResidentOrderingTableProofCapture {
  uint32_t captureId = 0u;
  ResidentOrderingTableProof proof{};
};
std::deque<ResidentOrderingTableProofCapture>
    g_residentOrderingTableProofCaptures;
std::mutex g_objectPresentationMutex;
std::deque<ObjectPresentationSnapshot> g_objectPresentationSnapshots;
ObjectPresentationSnapshot g_objectPresentationBuilding;
bool g_objectPresentationBuildingActive = false;
bool g_traceObjectPresentation = false;
uint32_t g_objectPresentationCaptureId = 0;

constexpr uint32_t kResidentSceneryOwnerAddress = UINT32_C(0x80065EBC);
constexpr uint32_t kSceneryDecoderListStartAddress = UINT32_C(0x1F80009C);
constexpr uint32_t kResidentSceneryRecordStride = 0x30u;
constexpr uint32_t kResidentSceneryFirstRecord = 0x04u;
constexpr uint32_t kResidentSceneryTranslationXOffset = 0x04u;
constexpr uint32_t kResidentSceneryTranslationYOffset = 0x08u;
constexpr uint32_t kResidentSceneryTranslationZOffset = 0x0cu;
constexpr uint32_t kResidentSceneryInfoOffset = 0x10u;
constexpr uint32_t kSceneryInfoVertexCountOffset = 0x10u;
constexpr uint32_t kSceneryInfoTriangleCountOffset = 0x14u;
constexpr uint32_t kSceneryInfoQuadCountOffset = 0x18u;
constexpr uint32_t kSceneryScratchRecordOffset = 0x280u;
constexpr uint32_t kSceneryScratchRecordStride = 0x20u;
constexpr uint32_t kSceneryScratchTranslationXyOffset = 0x00u;
constexpr uint32_t kSceneryScratchTranslationZOffset = 0x04u;
constexpr uint32_t kSceneryScratchTrianglePivotOffset = 0x08u;
constexpr uint32_t kSceneryScratchQuadStartOffset = 0x0cu;
constexpr uint32_t kSceneryScratchVertexPivotOffset = 0x10u;
constexpr uint32_t kSceneryScratchColorTableOffset = 0x14u;
constexpr uint32_t kSceneryScratchMaterialRemapOffset = 0x18u;
constexpr uint32_t kSceneryScratchMaterialTableOffset = 0x1cu;
// func_8004103C copies 32 bytes from 0x8005D4B4 to scratch+0xd0 before
// drawing. Its distance-selected material program then indexes the scratch
// destination, not that source address, without imposing a second bound.
constexpr uint32_t kSceneryMaterialLodShiftTableBytes = 32u;
constexpr uint32_t kSceneryMaterialLodShiftScratch =
    kRendererScratchBase + 0xd0u;
constexpr uint32_t kMaxSceneryWorlds = 8u;
struct PendingResidentPresentationCapture {
  bool active = false;
  const Memory *memory = nullptr;
  uint32_t listAddress = 0;
  std::array<ResidentSceneryWorld, kMaxSceneryWorlds> worlds{};
  uint32_t worldCount = 0;
  uint32_t residentFaceCount = 0;
  uint32_t captureId = 0;
  std::optional<uint32_t> materialLodDepthShift;
  uint32_t materialAnimationState = 0;
};
PendingResidentPresentationCapture g_pendingResidentPresentationCapture{};
constexpr uint32_t kMaxSceneryPolygonIdCount = 0x800u;
constexpr uint32_t kMaxSceneryVertexCount = 0x1000u;
// func_8002AA20 allocates two 0xBE4-byte decoded SLST buffers. Four bytes are
// header/alignment and every selected polygon occupies one uint16_t.
constexpr uint32_t kDecodedSceneryListBytes = 0xBE4u;
constexpr uint32_t kMaxDecodedSceneryEntries =
    (kDecodedSceneryListBytes - 4u) / sizeof(uint16_t);
constexpr uint32_t kMaxAdjacentSceneryDeltas = 64u;
constexpr uint32_t kMaxSceneryPathStates = 4096u;
constexpr uint32_t kMaxSceneryDeltaWords = 16384u;
constexpr uint32_t kSceneryIdentityCount =
    kMaxSceneryWorlds * 2u * kMaxSceneryPolygonIdCount;
constexpr uint64_t kSceneryProjectionIdentityFlag = UINT64_C(1) << 47u;
constexpr uint32_t kSceneryProjectionCaptureMask = (UINT32_C(1) << 26u) - 1u;

uint32_t nextResidentPresentationCaptureId() noexcept {
  g_residentPresentationCaptureId =
      (g_residentPresentationCaptureId + 1u) & kSceneryProjectionCaptureMask;
  if (g_residentPresentationCaptureId == 0u)
    g_residentPresentationCaptureId = 1u;
  return g_residentPresentationCaptureId;
}

uint32_t nextObjectPresentationCaptureId() noexcept {
  const auto aliasesCanonicalGuestRam = [](uint32_t captureId) {
    return captureId <= kKusegObjectCaptureAliasLast ||
           (captureId >= kKseg0ObjectCaptureAliasFirst &&
            captureId <= kKseg0ObjectCaptureAliasLast) ||
           (captureId >= kKseg1ObjectCaptureAliasFirst &&
            captureId <= kKseg1ObjectCaptureAliasLast);
  };
  do {
    g_objectPresentationCaptureId =
        (g_objectPresentationCaptureId + 1u) & kObjectCaptureMask;
  } while (aliasesCanonicalGuestRam(g_objectPresentationCaptureId));
  return g_objectPresentationCaptureId;
}

SceneryOcclusionMode configuredSceneryOcclusionMode() noexcept {
  const char *value = std::getenv("PS1_CRASH3_SCENERY_OCCLUSION");
  if (value == nullptr || *value == '\0' ||
      std::strcmp(value, "certified") == 0)
    return SceneryOcclusionMode::CertifiedWarpEdge;
  if (std::strcmp(value, "strict") == 0)
    return SceneryOcclusionMode::StrictScenery;
  if (std::strcmp(value, "off") == 0 || std::strcmp(value, "0") == 0)
    return SceneryOcclusionMode::Off;
  return SceneryOcclusionMode::CertifiedWarpEdge;
}

ResidentPresentationMode configuredResidentPresentationMode() noexcept {
  if (!active())
    return ResidentPresentationMode::Off;
  const char *value = std::getenv("PS1_CRASH3_VULKAN_RESIDENT_MODE");
  if (value == nullptr || *value == '\0') {
    // Unattended demo checkpoint discovery consumes only the immutable
    // resident-resource journal.  Make its public switch self-contained:
    // audit constructs the sidecar and fingerprints but never presents host
    // geometry or changes guest RAM.  An explicit resident-mode value still
    // wins, including an explicit "off" for diagnosis.
    const char *collector = std::getenv("PS1_CRASH3_AUTOSAVE_DEMO_LEVELS");
    return collector != nullptr && *collector != '\0' && *collector != '0'
               ? ResidentPresentationMode::Audit
               : ResidentPresentationMode::Off;
  }
  if (std::strcmp(value, "off") == 0 || std::strcmp(value, "0") == 0) {
    return ResidentPresentationMode::Off;
  }
  if (std::strcmp(value, "audit") == 0)
    return ResidentPresentationMode::Audit;
  if (std::strcmp(value, "debug") == 0)
    return ResidentPresentationMode::Debug;
  return ResidentPresentationMode::Off;
}

ObjectPresentationMode configuredObjectPresentationMode() noexcept {
  if (!active())
    return ObjectPresentationMode::Off;
  const char *value = std::getenv("PS1_CRASH3_VULKAN_OBJECT_MODE");
  if (value == nullptr || *value == '\0' || std::strcmp(value, "off") == 0 ||
      std::strcmp(value, "0") == 0) {
    return ObjectPresentationMode::Off;
  }
  if (std::strcmp(value, "audit") == 0)
    return ObjectPresentationMode::Audit;
  if (std::strcmp(value, "debug") == 0)
    return ObjectPresentationMode::Debug;
  return ObjectPresentationMode::Off;
}

PresentationCullStrategy configuredPresentationCullStrategy() noexcept {
  const char *value = std::getenv("PS1_CRASH3_VULKAN_CULL_STRATEGY");
  // A configured positive guard is the operational policy. Equal admission
  // and presentation apertures are useful only as explicit A/B baselines;
  // never select one merely because the environment variable is absent.
  const PresentationCullStrategy configuredDefault =
      g_options.enabled && g_options.widescreen16x9 &&
              g_options.sceneryCullGuardPixels != 0u
          ? PresentationCullStrategy::GuardBand
          : PresentationCullStrategy::Native;
  if (value == nullptr || *value == '\0')
    return configuredDefault;
  if (std::strcmp(value, "native") == 0 || std::strcmp(value, "off") == 0 ||
      std::strcmp(value, "0") == 0) {
    return PresentationCullStrategy::Native;
  }
  if (std::strcmp(value, "exact") == 0 || std::strcmp(value, "16:9") == 0) {
    return PresentationCullStrategy::Exact16x9;
  }
  if (std::strcmp(value, "guard") == 0 || std::strcmp(value, "wide") == 0) {
    return PresentationCullStrategy::GuardBand;
  }
  return configuredDefault;
}

bool presentationCullReadsEnabled() noexcept {
  return g_residentPresentationMode == ResidentPresentationMode::Off ||
         g_presentationCullStrategy != PresentationCullStrategy::Native;
}

uint32_t presentationCullGuardPixels() noexcept {
  return g_residentPresentationMode != ResidentPresentationMode::Off &&
                 g_presentationCullStrategy ==
                     PresentationCullStrategy::GuardBand
             ? g_options.sceneryCullGuardPixels
             : 0u;
}

void clearResidentPresentationSnapshots() noexcept {
  clearSceneryDetailSnapshots();
  g_sceneryZoneProgram = {};
  std::scoped_lock lock(g_residentPresentationMutex);
  g_residentPresentationSnapshots.clear();
}

void clearObjectPresentationSnapshots() noexcept {
  std::scoped_lock lock(g_objectPresentationMutex);
  g_objectPresentationSnapshots.clear();
  g_objectPresentationBuilding = {};
  g_objectPresentationBuildingActive = false;
}

// Caller holds g_objectPresentationMutex. A renderer-pool address can name
// different logical objects even within ONE capture (including temporary
// type-0 nodes). All consumers must resolve capture -> owner -> node on both
// the cached and cold paths. Never keep a borrowed pointer across mutations.
ObjectPresentationRendererNode *
currentObjectRendererNodeLocked(uint8_t *modelType = nullptr) {
  if (!g_objectPresentationBuildingActive || g_rendererLogicalObject == 0u ||
      g_rendererObjectSource == 0u || g_rendererObjectCaptureId == 0u ||
      g_objectPresentationBuilding.captureId != g_rendererObjectCaptureId)
    return nullptr;
  static ValidatedIndexCache owners, nodes;
  auto *object =
      owners.find(std::span{g_objectPresentationBuilding.objects},
                  [](const auto &candidate) {
                    return candidate.address == g_rendererLogicalObject;
                  });
  if (object == nullptr)
    return nullptr;
  if (modelType != nullptr)
    *modelType = object->modelType;
  return nodes.find(std::span{object->rendererNodes},
                    [](const auto &candidate) {
                      return candidate.address == g_rendererObjectSource;
                    });
}

void publishObjectPresentationBuildingLocked() {
  if (!g_objectPresentationBuildingActive)
    return;
  constexpr std::size_t kRetainedFrames = 8u;
  g_objectPresentationBuilding.sealed = true;
  uint32_t supportedObjects = 0u;
  uint32_t listedObjects = 0u;
  uint32_t transformedObjects = 0u;
  for (const ObjectPresentationObject &object :
       g_objectPresentationBuilding.objects) {
    supportedObjects += object.supportedModelType ? 1u : 0u;
    listedObjects += object.rendererNodes.empty() ? 0u : 1u;
    transformedObjects +=
        std::any_of(object.rendererNodes.begin(), object.rendererNodes.end(),
                    [](const ObjectPresentationRendererNode &node) {
                      return node.transformInstalled;
                    })
            ? 1u
            : 0u;
  }
  metrics::trace(
      "game.crash3.object_catalog_frame",
      {{"capture_id", g_objectPresentationBuilding.captureId},
       {"frame",
        static_cast<int64_t>(g_objectPresentationBuilding.frameNumber)},
       {"objects",
        static_cast<int64_t>(g_objectPresentationBuilding.objects.size())},
       {"supported_objects", supportedObjects},
       {"listed_objects", listedObjects},
       {"transformed_objects", transformedObjects},
       {"renderer_list_scan_attempted",
        g_objectPresentationBuilding.rendererListScanAttempted ? 1u : 0u},
       {"renderer_list_complete",
        g_objectPresentationBuilding.rendererListComplete ? 1u : 0u},
       {"renderer_list_head", g_objectPresentationBuilding.rendererListHead},
       {"renderer_list_nodes", g_objectPresentationBuilding.rendererListNodes},
       {"renderer_node_bindings",
        g_objectPresentationBuilding.rendererNodeBindings},
       {"renderer_transform_bindings",
        g_objectPresentationBuilding.rendererTransformBindings},
       {"rejected_objects", g_objectPresentationBuilding.rejectedObjects},
       {"rejected_renderer_nodes",
        g_objectPresentationBuilding.rejectedRendererNodes}});
  g_objectPresentationSnapshots.push_back(
      std::move(g_objectPresentationBuilding));
  while (g_objectPresentationSnapshots.size() > kRetainedFrames)
    g_objectPresentationSnapshots.pop_front();
  g_objectPresentationBuilding = {};
  g_objectPresentationBuildingActive = false;
}

ObjectPresentationObject captureObjectPresentation(const Memory &memory,
                                                   uint32_t address) {
  ObjectPresentationObject object;
  object.address = address;
  object.modelResource = memory.read32(address + 0x08u);
  object.globalResource = memory.read32(address + 0x0cu);
  object.externalResource = memory.read32(address + 0x10u);
  object.zoneResource = memory.read32(address + 0x14u);
  for (uint32_t index = 0; index < object.lightingWords.size(); ++index)
    object.lightingWords[index] = memory.read32(address + 0x20u + index * 4u);
  for (uint32_t axis = 0; axis < 3u; ++axis) {
    object.translation[axis] =
        std::bit_cast<int32_t>(memory.read32(address + 0x60u + axis * 4u));
    object.rotation[axis] =
        std::bit_cast<int32_t>(memory.read32(address + 0x6cu + axis * 4u));
    object.scale[axis] =
        std::bit_cast<int32_t>(memory.read32(address + 0x78u + axis * 4u));
  }
  object.statusA = memory.read32(address + 0xa8u);
  object.statusB = memory.read32(address + 0xacu);
  object.statusC = memory.read32(address + 0xb0u);
  object.subtype = memory.read32(address + 0xb4u);
  object.id = memory.read32(address + 0xb8u);
  object.animationSequence = memory.read32(address + 0xe8u);
  object.animationFrame = memory.read32(address + 0xecu);
  object.routineFlags = memory.read32(address + 0x100u);
  object.displayMode = memory.read32(address + 0x108u);
  object.renderSelector = memory.read32(address + 0x120u);
  if (isCanonicalGuestRamRange(object.animationSequence, 1u))
    object.modelType = memory.read8(object.animationSequence);
  object.supportedModelType = object.modelType == 1u ||
                              object.modelType == 2u ||
                              object.modelType == 4u || object.modelType == 5u;
  return object;
}

uint32_t rendererNodeBytesForModelType(uint8_t modelType) noexcept {
  // Exact allocation advances in func_8001B858: type 1 uses 0x70 bytes, type
  // 2 uses 0x2c, type 4 uses 0x48 and type 5 uses 0x30.
  switch (modelType) {
  case 1u:
    return 0x70u;
  case 2u:
    return 0x2cu;
  case 4u:
    return 0x48u;
  case 5u:
    return 0x30u;
  default:
    return kCrash3RendererNodeBytes;
  }
}

void captureType1RendererStreams(ObjectPresentationRendererNode &node,
                                 const Memory &memory,
                                 uint32_t scratchColorTable,
                                 bool prefixAlreadyCopied = true) {
  node.type1StreamsCaptured = false;
  node.type1ColorWordCount = 0u;
  node.type1CommandWords.clear();
  node.type1ColorWords.clear();
  node.type1ScratchColorWords.clear();

  constexpr uint32_t kMaximumCommands = 65536u;
  constexpr uint32_t kMaximumColorWords = 256u;
  const uint32_t commandTable = node.words[0x38u / sizeof(uint32_t)];
  const uint32_t colorTable = node.words[0x3cu / sizeof(uint32_t)];
  if (!isCanonicalGuestRamRange(commandTable, sizeof(uint32_t), 4u)) {
    metrics::count(
        "host.crash3.object_sidecar.type1_stream_capture_invalid_command");
    return;
  }

  const uint32_t colorWords = memory.read32(commandTable);
  if (colorWords > kMaximumColorWords) {
    metrics::count(
        "host.crash3.object_sidecar.type1_stream_capture_invalid_color");
    return;
  }

  node.type1CommandWords.reserve(1024u);
  bool terminated = false;
  for (uint32_t index = 0u; index < kMaximumCommands; ++index) {
    const uint64_t byteOffset =
        (static_cast<uint64_t>(index) + 1u) * sizeof(uint32_t);
    if (byteOffset > UINT32_MAX || byteOffset > UINT32_MAX - commandTable ||
        !isCanonicalGuestRamRange(commandTable +
                                      static_cast<uint32_t>(byteOffset),
                                  sizeof(uint32_t), 4u)) {
      break;
    }
    const uint32_t command =
        memory.read32(commandTable + static_cast<uint32_t>(byteOffset));
    node.type1CommandWords.push_back(command);
    if (command == UINT32_MAX) {
      terminated = true;
      break;
    }
  }
  if (!terminated) {
    node.type1CommandWords.clear();
    metrics::count(
        "host.crash3.object_sidecar.type1_stream_capture_unterminated");
    return;
  }

  // The header controls only how many words func_8003F73C copies into its
  // aliased scratch cache. Commands which do not select that cache index the
  // original table directly and may legally refer beyond the copied prefix.
  uint32_t requiredSourceColorWords = colorWords;
  const auto requireDirectColor = [&](uint32_t byteOffset) {
    requiredSourceColorWords =
        std::max(requiredSourceColorWords, byteOffset / uint32_t{4} + 1u);
  };
  for (const uint32_t command : node.type1CommandWords) {
    if (command == UINT32_MAX)
      break;
    if ((command >> 16u) == 0u) {
      if ((command & 1u) == 0u)
        requireDirectColor((command >> 7u) & 0x1fcu);
      if ((command & 2u) == 0u)
        requireDirectColor(command & 0x1fcu);
    } else if ((command & UINT32_C(0x08000000)) == 0u) {
      requireDirectColor((command >> 7u) & 0x1fcu);
    }
  }
  if (requiredSourceColorWords > kMaximumColorWords ||
      (requiredSourceColorWords != 0u &&
       !isCanonicalGuestRamRange(
           colorTable, requiredSourceColorWords * sizeof(uint32_t), 4u))) {
    node.type1CommandWords.clear();
    metrics::count(
        "host.crash3.object_sidecar.type1_stream_capture_invalid_color");
    return;
  }

  node.type1ColorWords.reserve(requiredSourceColorWords);
  for (uint32_t index = 0u; index < requiredSourceColorWords; ++index) {
    node.type1ColorWords.push_back(
        memory.read32(colorTable + index * sizeof(uint32_t)));
  }
  const uint32_t scratchPhysical = Memory::toPhysical(scratchColorTable);
  constexpr uint32_t kScratchEnd =
      Memory::SCRATCHPAD_BASE + Memory::SCRATCHPAD_SIZE;
  if (prefixAlreadyCopied && colorWords != 0u &&
      (scratchPhysical < Memory::SCRATCHPAD_BASE ||
       scratchPhysical > kScratchEnd ||
       colorWords * sizeof(uint32_t) > kScratchEnd - scratchPhysical)) {
    node.type1CommandWords.clear();
    node.type1ColorWords.clear();
    metrics::count(
        "host.crash3.object_sidecar.type1_stream_capture_invalid_scratch");
    return;
  }
  node.type1ScratchColorWords.reserve(colorWords);
  for (uint32_t index = 0u; index < colorWords; ++index) {
    node.type1ScratchColorWords.push_back(
        prefixAlreadyCopied
            ? memory.read32(scratchColorTable + index * sizeof(uint32_t))
            : node.type1ColorWords[index]);
  }
  node.type1ColorWordCount = colorWords;
  node.type1StreamsCaptured = true;
  metrics::count("host.crash3.object_sidecar.type1_stream_capture");
  metrics::count("host.crash3.object_sidecar.type1_command_word_captured",
                 node.type1CommandWords.size());
  metrics::count("host.crash3.object_sidecar.type1_color_word_captured",
                 node.type1ColorWords.size());
  metrics::count("host.crash3.object_sidecar.type1_scratch_word_captured",
                 node.type1ScratchColorWords.size());
}

ObjectPresentationRendererNode captureObjectRendererNode(const Memory &memory,
                                                         uint32_t address,
                                                         uint8_t modelType) {
  ObjectPresentationRendererNode node;
  node.address = address;
  node.byteCount = rendererNodeBytesForModelType(modelType);
  for (uint32_t index = 0; index < node.byteCount / sizeof(uint32_t); ++index)
    node.words[index] = memory.read32(address + index * 4u);
  return node;
}

void installObjectRendererTransform(ObjectPresentationRendererNode &node,
                                    const CPUContext &ctx,
                                    uint32_t translatedX) {
  std::copy_n(ctx.cop2c, node.gteControlWords.size(),
              node.gteControlWords.begin());
  node.gteControlWords[GTE_TRX] = translatedX;
  node.scaleShift = ctx.r[4] <= 31u ? static_cast<int32_t>(ctx.r[4]) : -1;
  node.transformInstalled = true;
}

// Capture the builder's entire normal list atomically before using any of its
// nodes for presentation. This is deliberately invoked from the first common
// transform write rather than from a generated instruction hook: profile and
// renderer iterations then rebuild only the small runtime objects. The global
// list head remains the builder's immutable root while func_8003F510 walks a
// scratch copy, so nodes which take func_80040E38's early type-2 path are still
// reachable here.
bool captureCompleteObjectRendererListLocked(const Memory &memory) {
  auto &snapshot = g_objectPresentationBuilding;
  if (!g_objectPresentationBuildingActive || snapshot.rendererListScanAttempted)
    return snapshot.rendererListComplete;
  snapshot.rendererListScanAttempted = true;

  // Debug presentation decodes streamed resources asynchronously after this
  // guest frame.  Preserve the immutable source image at the first exact
  // renderer boundary even when the global-list scan below cannot yet prove
  // completeness.  Directly bound nodes captured later in the same pass then
  // remain reconstructible instead of making the whole modern scene blink
  // back to the original 4:3 packets.
  if (g_objectPresentationMode == ObjectPresentationMode::Debug &&
      snapshot.guestRam == nullptr) {
    auto guestRam = std::make_shared<std::vector<uint8_t>>(Memory::RAM_SIZE);
    std::memcpy(guestRam->data(), memory.ramPtr(), Memory::RAM_SIZE);
    snapshot.guestRam = std::move(guestRam);
    metrics::count("host.crash3.object_sidecar.ram_snapshot");
    metrics::count("host.crash3.object_sidecar.ram_snapshot_bytes",
                   Memory::RAM_SIZE);
  }

  const uint32_t rendererState = memory.read32(kCrash3RendererOwnerAddress +
                                               kCrash3RendererOwnerStateOffset);
  if (!isCanonicalGuestRamRange(
          rendererState, kCrash3RendererListHeadOffset + sizeof(uint32_t),
          4u)) {
    metrics::count("host.crash3.object_sidecar.renderer_list_state_invalid");
    return false;
  }

  const uint32_t head =
      memory.read32(rendererState + kCrash3RendererListHeadOffset);
  snapshot.rendererListHead = head;
  if (head == 0u) {
    snapshot.rendererListComplete = true;
    metrics::count("host.crash3.object_sidecar.renderer_list_complete");
    return true;
  }

  struct PendingNode {
    uint32_t objectAddress = 0;
    uint8_t modelType = 0;
    ObjectPresentationRendererNode node;
  };
  std::vector<PendingNode> pending;
  pending.reserve(32u);
  uint32_t address = head;
  while (address != 0u && pending.size() < kMaxCrash3RendererListNodes) {
    if (!isCanonicalGuestRamRange(address, kCrash3RendererNodeBytes, 4u) ||
        std::any_of(pending.begin(), pending.end(),
                    [&](const PendingNode &candidate) {
                      return candidate.node.address == address;
                    })) {
      ++snapshot.rejectedRendererNodes;
      metrics::count("host.crash3.object_sidecar.renderer_list_invalid_node");
      return false;
    }

    const uint32_t objectAddress =
        memory.read32(address + kCrash3RendererNodeObjectOffset);
    const auto object =
        std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
                     [&](const ObjectPresentationObject &candidate) {
                       return candidate.address == objectAddress;
                     });
    if (object == snapshot.objects.end() || !object->supportedModelType) {
      ++snapshot.rejectedRendererNodes;
      metrics::count(
          "host.crash3.object_sidecar.renderer_list_uncatalogued_node");
      return false;
    }

    const uint32_t nodeBytes = rendererNodeBytesForModelType(object->modelType);
    if (!isCanonicalGuestRamRange(address, nodeBytes, 4u)) {
      ++snapshot.rejectedRendererNodes;
      metrics::count("host.crash3.object_sidecar.renderer_list_short_node");
      return false;
    }
    PendingNode captured;
    captured.objectAddress = objectAddress;
    captured.modelType = object->modelType;
    captured.node =
        captureObjectRendererNode(memory, address, object->modelType);
    captured.node.listDiscovered = true;
    pending.push_back(std::move(captured));
    address = memory.read32(address);
  }

  if (address != 0u) {
    ++snapshot.rejectedRendererNodes;
    metrics::count("host.crash3.object_sidecar.renderer_list_limit");
    return false;
  }

  for (PendingNode &captured : pending) {
    auto object =
        std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
                     [&](const ObjectPresentationObject &candidate) {
                       return candidate.address == captured.objectAddress;
                     });
    object->rendererNodes.push_back(std::move(captured.node));
    ++snapshot.rendererNodeBindings;
    metrics::count("host.crash3.object_sidecar.renderer_list_binding");
    if (g_traceObjectPresentation) {
      const ObjectPresentationRendererNode &node = object->rendererNodes.back();
      metrics::trace("game.crash3.object_renderer_list_binding",
                     {{"capture_id", snapshot.captureId},
                      {"renderer_node", node.address},
                      {"next_node", node.words[0]},
                      {"object", captured.objectAddress},
                      {"model_type", captured.modelType},
                      {"node_bytes", node.byteCount},
                      {"setup_function", node.words[1]},
                      {"render_function", node.words[2]},
                      {"cleanup_function", node.words[3]},
                      {"model_resource", node.words[5]},
                      {"node_word_18", node.words[6]},
                      {"node_word_1c", node.words[7]},
                      {"node_word_20", node.words[8]},
                      {"node_word_24", node.words[9]},
                      {"node_word_28", node.words[10]},
                      {"node_word_2c", node.words[11]},
                      {"node_word_30", node.words[12]},
                      {"node_word_34", node.words[13]},
                      {"node_word_38", node.words[14]},
                      {"node_word_3c", node.words[15]},
                      {"node_word_40", node.words[16]},
                      {"node_word_44", node.words[17]},
                      {"node_word_48", node.words[18]},
                      {"node_word_4c", node.words[19]},
                      {"node_word_50", node.words[20]},
                      {"node_word_54", node.words[21]},
                      {"node_word_58", node.words[22]},
                      {"node_word_5c", node.words[23]},
                      {"node_word_60", node.words[24]},
                      {"node_word_64", node.words[25]},
                      {"node_word_68", node.words[26]},
                      {"node_word_6c", node.words[27]}});
    }
  }
  snapshot.rendererListNodes = static_cast<uint32_t>(pending.size());
  snapshot.rendererListComplete = true;
  metrics::count("host.crash3.object_sidecar.renderer_list_complete");
  metrics::count("host.crash3.object_sidecar.renderer_list_node",
                 static_cast<int64_t>(pending.size()));
  return true;
}

std::optional<uint16_t> sceneryEntryFromProjectionTag(uint64_t tag) noexcept {
  // Stable scenery payloads reserve bits 47..42 as 100000. Dynamic Crash
  // construction hashes have no such schema and therefore remain ineligible
  // unless all three vertices decode to the same exact SLST entry and capture
  // below. The independent gameProfileCallsite authenticates the renderer PC.
  if (((tag >> 42u) & UINT64_C(0x3f)) != UINT64_C(0x20))
    return std::nullopt;
  return static_cast<uint16_t>((tag >> 26u) & UINT64_C(0xffff));
}

std::optional<uint32_t> sceneryCaptureFromProjectionTag(uint64_t tag) noexcept {
  if (((tag >> 42u) & UINT64_C(0x3f)) != UINT64_C(0x20))
    return std::nullopt;
  const uint32_t captureId =
      static_cast<uint32_t>(tag) & kSceneryProjectionCaptureMask;
  return captureId != 0u ? std::optional<uint32_t>{captureId} : std::nullopt;
}

std::optional<uint64_t>
proceduralProjectionOrdinalFromTag(uint64_t tag) noexcept {
  if ((tag & (UINT64_C(0x3f) << 42u)) != kProceduralProjectionIdentityFlag) {
    return std::nullopt;
  }
  const uint64_t payload = tag & kProceduralProjectionPayloadMask;
  if ((payload & ~kProceduralProjectionDataMask) !=
      kProceduralProjectionOrdinalSchema) {
    return std::nullopt;
  }
  const uint64_t encoded = payload & kProceduralProjectionDataMask;
  return encoded != 0u ? std::optional<uint64_t>{encoded - 1u} : std::nullopt;
}

std::optional<uint64_t>
proceduralProjectionIdentityFromTag(uint64_t tag) noexcept {
  if ((tag & (UINT64_C(0x3f) << 42u)) != kProceduralProjectionIdentityFlag) {
    return std::nullopt;
  }
  const uint64_t payload = tag & kProceduralProjectionPayloadMask;
  const uint64_t schema = payload & ~kProceduralProjectionDataMask;
  if (schema != kProceduralProjectionOrdinalSchema &&
      schema != kProceduralProjectionWaterGridSchema &&
      schema != kProceduralProjectionTerrainGridSchema) {
    return std::nullopt;
  }
  return payload;
}

std::optional<uint64_t>
terrainProjectionIdentity(const CPUContext &ctx) noexcept {
  if (ctx.pc != UINT32_C(0x80043E00) || ctx.r[3] != kRendererScratchBase)
    return std::nullopt;

  // func_80043740 stores the selected vertex walker at scratch+884. Both
  // walkers have already overwritten their source offset registers, but the
  // signed GTE X/Z and unchanged camera remainders still invert their exact
  // construction. The terrain sample is ((Z & 63)<<8)|((X & 63)<<2) relative
  // to r11. A pre-cull projection ordinal was not an identity: cache hits,
  // row admission and the transposed walker all change the visit sequence.
  const auto &recomp = static_cast<const recomp_context &>(ctx);
  if (recomp.mem == nullptr)
    return std::nullopt;
  const uint32_t walker = recomp.mem->read32(kRendererScratchBase + 884u);
  if (walker != UINT32_C(0x80043D48) && walker != UINT32_C(0x80043D88)) {
    metrics::count("host.crash3.procedural_identity.terrain_invalid_walker");
    metrics::trace("game.crash3.procedural_terrain_invalid_walker",
                   {{"walker", walker}, {"r1", ctx.r[1]}});
    return std::nullopt;
  }
  const bool transposed = walker == UINT32_C(0x80043D88);
  const int64_t modelX = static_cast<int16_t>(ctx.cop2d[GTE_VXY0]);
  const int64_t modelZ = static_cast<int16_t>(ctx.cop2d[GTE_VZ0]);
  const int64_t remainderX = static_cast<int32_t>(ctx.r[12]);
  const int64_t remainderZ = static_cast<int32_t>(ctx.r[13]);
  const int64_t localX = modelX - remainderX;
  const int64_t localZ = modelZ - remainderZ;
  const uint32_t xOriginScale = transposed ? 4u : 256u;
  const uint32_t zOriginScale = transposed ? 256u : 4u;
  if (localX % 1024 != 0 || localZ % 1024 != 0 ||
      ctx.r[8] % xOriginScale != 0u || ctx.r[10] % zOriginScale != 0u ||
      (ctx.r[11] & 3u) != 0u ||
      Memory::toPhysical(ctx.r[11]) >= Memory::RAM_SIZE) {
    metrics::count("host.crash3.procedural_identity.terrain_invalid_grid");
    return std::nullopt;
  }
  const int64_t cellX = localX / 1024 + ctx.r[8] / xOriginScale;
  const int64_t cellZ = localZ / 1024 + ctx.r[10] / zOriginScale;
  // Retain eight bits per world cell, not just the six bits of the repeated
  // height-map lookup. Neighbouring copies of the same sample stay distinct.
  const uint64_t coordinate =
      (static_cast<uint64_t>(cellX) & UINT64_C(0xff)) |
      ((static_cast<uint64_t>(cellZ) & UINT64_C(0xff)) << 8u);
  const uint64_t resource = Memory::toPhysical(ctx.r[11]);
  metrics::count("host.crash3.procedural_identity.terrain_grid");
  if (metrics::traceEnabled()) {
    metrics::trace("game.crash3.procedural_terrain_identity",
                   {{"walker", walker},
                    {"resource", static_cast<int64_t>(resource)},
                    {"cell_x", cellX},
                    {"cell_z", cellZ},
                    {"model_x", modelX},
                    {"model_z", modelZ},
                    {"remainder_x", remainderX},
                    {"remainder_z", remainderZ}});
  }
  return kProceduralProjectionTerrainGridSchema | coordinate |
         (resource << 16u);
}

std::optional<uint64_t>
waterProjectionIdentity(const CPUContext &ctx) noexcept {
  if (ctx.pc != UINT32_C(0x80044938))
    return std::nullopt;

  // func_800447D8/0x80044814 preserves its cache-slot address in r22 until
  // RTPS. The slot relation identifies which of the two exact transposed
  // walkers made the point; a symmetric point is resolved by the walker's
  // fixed-point origin alignment. VXY0/VZ0 still contain the game-authored
  // model input, while
  // r8/r9 and r13/r14 retain respectively the integral grid origin and its
  // camera-relative remainder. Reversing those integer operations yields the
  // same 8+8-bit toroidal cell that Crash itself uses for the water lookup.
  // This is categorical source provenance before face admission, not a
  // projected-position match.
  const uint32_t slotBytes = ctx.r[22] - ctx.r[3];
  if ((slotBytes & 3u) != 0u) {
    metrics::count("host.crash3.procedural_identity.water_invalid_slot");
    return std::nullopt;
  }
  const uint32_t slot = slotBytes >> 2u;
  if (slot > UINT16_MAX) {
    metrics::count("host.crash3.procedural_identity.water_invalid_slot");
    return std::nullopt;
  }

  const int64_t modelX = static_cast<int16_t>(ctx.cop2d[GTE_VXY0]);
  const int64_t modelZ = static_cast<int16_t>(ctx.cop2d[GTE_VZ0]);
  const int64_t originX = static_cast<int32_t>(ctx.r[8]);
  const int64_t originZ = static_cast<int32_t>(ctx.r[9]);
  const int64_t remainderX = static_cast<int32_t>(ctx.r[13]);
  const int64_t remainderZ = static_cast<int32_t>(ctx.r[14]);
  const uint16_t expectedRegularAxis =
      static_cast<uint16_t>((static_cast<uint64_t>(slot) << 8u) + ctx.r[14]);
  const uint16_t expectedTransposedAxis =
      static_cast<uint16_t>((static_cast<uint64_t>(slot) << 8u) + ctx.r[13]);
  const bool regularAxisMatches =
      expectedRegularAxis == static_cast<uint16_t>(ctx.cop2d[GTE_VZ0]);
  const bool transposedAxisMatches =
      expectedTransposedAxis == static_cast<uint16_t>(ctx.cop2d[GTE_VXY0]);
  const int64_t regularOtherAxis = modelX - remainderX + originX;
  const int64_t transposedOtherAxis = modelZ - remainderZ + originZ;
  const bool regularGridAligned = (regularOtherAxis % 256) == 0;
  const bool transposedGridAligned = (transposedOtherAxis % 256) == 0;
  bool transposed = false;
  if (regularAxisMatches != transposedAxisMatches) {
    transposed = transposedAxisMatches;
  } else if (regularAxisMatches) {
    // A diagonal point can satisfy both slot equations. Crash stores the
    // integral origin in fixed-point on exactly the walker's other axis, so
    // reversing both candidates leaves only the real one aligned to its
    // 256-unit grid. This remains valid across all six callers in
    // func_800445E0 and does not depend on a return address. A doubly aligned
    // (or corrupt) state is genuinely ambiguous and must snap to endpoint B.
    if (regularGridAligned != transposedGridAligned) {
      transposed = transposedGridAligned;
    } else {
      metrics::count("host.crash3.procedural_identity.water_ambiguous_walker");
      return std::nullopt;
    }
  } else {
    metrics::count("host.crash3.procedural_identity.water_slot_axis_mismatch");
    return std::nullopt;
  }

  const int64_t fixedOtherAxis =
      transposed ? transposedOtherAxis : regularOtherAxis;
  if ((fixedOtherAxis % 256) != 0) {
    metrics::count(
        "host.crash3.procedural_identity.water_unaligned_grid_coordinate");
    return std::nullopt;
  }
  const int64_t cellX =
      transposed ? static_cast<int64_t>(slot) + originX : fixedOtherAxis / 256;
  const int64_t cellZ =
      transposed ? fixedOtherAxis / 256 : static_cast<int64_t>(slot) + originZ;
  const uint64_t coordinate =
      (static_cast<uint64_t>(cellX) & UINT64_C(0xff)) |
      ((static_cast<uint64_t>(cellZ) & UINT64_C(0xff)) << 8u);
  const uint64_t workBuffer =
      static_cast<uint64_t>(Memory::toPhysical(ctx.r[3])) & UINT64_C(0x1fffff);
  const uint64_t data = coordinate | (workBuffer << 16u);
  metrics::count(transposed ? "host.crash3.procedural_identity.water_transposed"
                            : "host.crash3.procedural_identity.water_regular");
  metrics::trace("game.crash3.procedural_water_identity",
                 {{"return_pc", ctx.r[31]},
                  {"transposed", transposed ? 1 : 0},
                  {"slot", slot},
                  {"model_x", modelX},
                  {"model_z", modelZ},
                  {"origin_x", originX},
                  {"origin_z", originZ},
                  {"remainder_x", remainderX},
                  {"remainder_z", remainderZ},
                  {"cell_x", cellX},
                  {"cell_z", cellZ},
                  {"work_buffer", static_cast<int64_t>(workBuffer)},
                  {"identity", static_cast<int64_t>(data)}});
  return kProceduralProjectionWaterGridSchema | data;
}

bool certifiedWarpEdgeRecoveredEntry(uint16_t packed) noexcept {
  // Slot 3 proves that these resident side-band faces are physically nearer
  // yet receive no final OT samples. Slot 4 observes the same stable faces to
  // the right of Coco. The exact conflict plane independently identifies the
  // original SLST owners that replace them. Numeric screen bounds, camera
  // position, frame number and W are deliberately not part of this lookup.
  constexpr std::array<uint16_t, 12> kRecoveredEntries{
      UINT16_C(0x004d), UINT16_C(0x10fc), UINT16_C(0x1902), UINT16_C(0x1a02),
      UINT16_C(0x1a0b), UINT16_C(0x1a0d), UINT16_C(0x1a11), UINT16_C(0x1a13),
      UINT16_C(0x1a4f), UINT16_C(0x1a52), UINT16_C(0x202d), UINT16_C(0x3971)};
  return std::binary_search(kRecoveredEntries.begin(), kRecoveredEntries.end(),
                            packed);
}

uint32_t observeSceneryProjectionEntry(uint32_t pc, uint32_t cursorAddress,
                                       uint32_t value) noexcept {
  auto &identity = g_sceneryProjectionIdentity;
  if (!identity.enabled)
    return value;

  const bool previousCurrentValid = identity.currentValid;
  const bool previousPrefetchedValid = identity.prefetchedValid;
  const uint16_t previousCurrent = identity.current;
  const uint16_t previousPrefetched = identity.prefetched;
  const uint32_t previousCursor = identity.lastCursor;
  const uint16_t packed = static_cast<uint16_t>(value);
  // Four threaded paths perform a second load from the same SLST cursor after
  // installing the prefetched primitive's GTE inputs.  0x80041354 is the
  // triangle path, 0x80041388/0x800415F0 the state-1/state-2 half-quad paths
  // and 0x800414C4 the state-3 quad's fourth-vertex path. These are exact
  // pipeline commits: the following RTPT[/RTPS] and AVSZ consume `prefetched`.
  // Make that code-owned boundary explicit so camera-dependent branch changes
  // cannot leave projection tags and the local-OT observer one primitive
  // behind.
  const bool projectionPrefetchCommit =
      (pc == UINT32_C(0x80041354) ||
       (pc == UINT32_C(0x80041388) && ((packed >> 11u) & 3u) == 1u) ||
       (pc == UINT32_C(0x800415F0) && ((packed >> 11u) & 3u) == 2u) ||
       pc == UINT32_C(0x800414C4)) &&
      identity.currentValid && identity.prefetchedValid &&
      cursorAddress == identity.lastCursor && packed == identity.prefetched;
  if (projectionPrefetchCommit) {
    identity.current = identity.prefetched;
    metrics::count("host.crash3.scenery_projection_identity.prefetch_commit");
  } else if (!identity.currentValid) {
    identity.current = packed;
    identity.currentValid = true;
  } else if (cursorAddress != identity.lastCursor) {
    if (identity.prefetchedValid)
      identity.current = identity.prefetched;
    identity.prefetched = packed;
    identity.prefetchedValid = true;
  }
  identity.lastCursor = cursorAddress;
  if (identity.traceEnabled) {
    metrics::trace(
        "game.crash3.scenery_projection_entry",
        {{"capture_id", identity.captureId},
         {"pc", pc},
         {"cursor", cursorAddress},
         {"packed", packed},
         {"previous_cursor", previousCursor},
         {"previous_current_valid", previousCurrentValid ? 1u : 0u},
         {"previous_current", previousCurrent},
         {"previous_prefetched_valid", previousPrefetchedValid ? 1u : 0u},
         {"previous_prefetched", previousPrefetched},
         {"current_valid", identity.currentValid ? 1u : 0u},
         {"current", identity.current},
         {"prefetched_valid", identity.prefetchedValid ? 1u : 0u},
         {"prefetched", identity.prefetched},
         {"projection_prefetch_commit", projectionPrefetchCommit ? 1u : 0u}});
  }
  return value;
}

ScenerySelectionMode scenerySelectionMode() noexcept {
  if (!active())
    return ScenerySelectionMode::Original;
  const char *value = std::getenv("PS1_CRASH3_SLST_MODE");
  if (value == nullptr)
    return ScenerySelectionMode::Original;
  if (std::strcmp(value, "audit") == 0)
    return ScenerySelectionMode::Audit;
  if (std::strcmp(value, "adjacent") == 0)
    return ScenerySelectionMode::Adjacent;
  if (std::strcmp(value, "symmetric") == 0)
    return ScenerySelectionMode::Symmetric;
  if (std::strcmp(value, "geometric") == 0)
    return ScenerySelectionMode::Geometric;
  if (std::strcmp(value, "geometric-resident") == 0)
    return ScenerySelectionMode::GeometricResident;
  if (std::strcmp(value, "bypass") == 0)
    return ScenerySelectionMode::Bypass;
  return ScenerySelectionMode::Original;
}

uint32_t adjacentSceneryDeltaCount() noexcept {
  const char *value = std::getenv("PS1_CRASH3_SLST_ADJACENT_DELTAS");
  if (value == nullptr || *value == '\0')
    return 1u;
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value || *end != '\0')
    return 1u;
  if (parsed >= kMaxAdjacentSceneryDeltas)
    return kMaxAdjacentSceneryDeltas;
  return std::max(static_cast<uint32_t>(parsed), 1u);
}

bool isRamRange(uint32_t address, uint32_t size) noexcept {
  const uint32_t physical = Memory::toPhysical(address);
  return physical < Memory::RAM_SIZE && size <= Memory::RAM_SIZE - physical;
}

bool isScratchpadRange(uint32_t address, uint32_t size) noexcept {
  const uint32_t physical = Memory::toPhysical(address);
  return physical >= Memory::SCRATCHPAD_BASE &&
         physical < Memory::SCRATCHPAD_BASE + Memory::SCRATCHPAD_SIZE &&
         size <= Memory::SCRATCHPAD_BASE + Memory::SCRATCHPAD_SIZE - physical;
}

bool readDecodedSceneryList(const Memory &memory, uint32_t address,
                            std::vector<uint16_t> &entries) {
  if (!isRamRange(address, 4u))
    return false;
  const int32_t count = static_cast<int16_t>(memory.read16(address));
  if (count < 0 || static_cast<uint32_t>(count) > kMaxDecodedSceneryEntries ||
      !isRamRange(address + 4u, static_cast<uint32_t>(count) * 2u))
    return false;
  entries.resize(static_cast<std::size_t>(count));
  for (uint32_t index = 0; index < static_cast<uint32_t>(count); ++index)
    entries[index] = memory.read16(address + 4u + index * 2u);
  return true;
}

bool readSceneryPathItem(const Memory &memory, uint32_t entryBase,
                         uint32_t itemIndex, uint32_t &itemAddress) noexcept {
  const uint64_t slot = static_cast<uint64_t>(entryBase) + 16u +
                        static_cast<uint64_t>(itemIndex) * 4u;
  if (slot > std::numeric_limits<uint32_t>::max() ||
      !isRamRange(static_cast<uint32_t>(slot), 4u))
    return false;
  itemAddress = memory.read32(static_cast<uint32_t>(slot));
  return isRamRange(itemAddress, 4u);
}

struct SceneryDeltaWords {
  std::vector<uint16_t> removed;
  std::vector<uint16_t> added;
  std::vector<uint16_t> swapped;
};

bool readSceneryDelta(const Memory &memory, uint32_t address,
                      SceneryDeltaWords &delta) {
  if (!isRamRange(address, 8u))
    return false;
  const uint32_t wordCount = memory.read16(address);
  const uint32_t type = memory.read16(address + 2u);
  const uint32_t addedStart = memory.read16(address + 4u);
  const uint32_t swappedStart = memory.read16(address + 6u);
  if (type != 1u || wordCount < 2u || wordCount > kMaxSceneryDeltaWords ||
      addedStart < 2u || addedStart > wordCount || swappedStart < addedStart ||
      swappedStart > wordCount || !isRamRange(address, 4u + wordCount * 2u))
    return false;

  const auto readRange = [&](uint32_t first, uint32_t last,
                             std::vector<uint16_t> &words) {
    words.resize(last - first);
    for (uint32_t index = first; index < last; ++index)
      words[index - first] = memory.read16(address + 4u + index * 2u);
  };
  readRange(2u, addedStart, delta.removed);
  readRange(addedStart, swappedStart, delta.added);
  readRange(swappedStart, wordCount, delta.swapped);
  return true;
}

struct SceneryRebuildCursor {
  const std::vector<uint16_t> *words = nullptr;
  std::size_t cursor = 0;
  uint32_t at = 0;
  uint32_t remaining = 0;
  uint16_t value = 0;
};

bool sceneryRebuildNextNode(SceneryRebuildCursor &node) noexcept {
  if (node.words == nullptr)
    return false;
  if (node.cursor >= node.words->size()) {
    node.remaining = 0;
    return true;
  }
  const uint16_t command = (*node.words)[node.cursor++];
  if (command == UINT16_C(0xFFFF)) {
    node.remaining = 0;
    return true;
  }
  node.at = command & 0x0fffu;
  node.remaining = 1u + ((command >> 12u) & 0x0fu);
  if (node.cursor >= node.words->size())
    return false;
  node.value = (*node.words)[node.cursor++];
  return true;
}

bool sceneryRebuildConsume(SceneryRebuildCursor &node) noexcept {
  if (node.remaining == 0u)
    return false;
  --node.remaining;
  if (node.remaining == 0u)
    return sceneryRebuildNextNode(node);
  if (node.cursor >= node.words->size())
    return false;
  node.value = (*node.words)[node.cursor++];
  return true;
}

bool applySceneryDeltaForward(const SceneryDeltaWords &delta,
                              const std::vector<uint16_t> &source,
                              std::vector<uint16_t> &destination) {
  SceneryRebuildCursor removed{&delta.removed};
  SceneryRebuildCursor added{&delta.added};
  if (!sceneryRebuildNextNode(removed) || !sceneryRebuildNextNode(added))
    return false;

  destination.clear();
  destination.reserve(source.size());
  std::size_t sourceIndex = 0;
  uint32_t removedCount = 0;
  const std::size_t iterationLimit =
      source.size() + delta.removed.size() + delta.added.size() + 1u;
  std::size_t iterations = 0;
  while (sourceIndex < source.size() || removed.remaining != 0u ||
         added.remaining != 0u) {
    if (++iterations > iterationLimit * 2u)
      return false;

    if (sourceIndex < source.size() && removed.remaining != 0u &&
        removed.at != 0u && removed.at - 1u == sourceIndex) {
      ++sourceIndex;
      ++removedCount;
      if (!sceneryRebuildConsume(removed))
        return false;
      continue;
    }
    if (added.remaining != 0u &&
        static_cast<uint64_t>(added.at) + removedCount == sourceIndex) {
      destination.push_back(added.value);
      if (destination.size() > kMaxDecodedSceneryEntries ||
          !sceneryRebuildConsume(added))
        return false;
      continue;
    }
    if (sourceIndex >= source.size())
      return false;
    destination.push_back(source[sourceIndex++]);
    if (destination.size() > kMaxDecodedSceneryEntries)
      return false;
  }

  uint32_t previousSource = 0;
  for (std::size_t cursor = 0; cursor < delta.swapped.size();) {
    const uint16_t command = delta.swapped[cursor++];
    if (command == UINT16_C(0xFFFF))
      break;

    uint32_t sourceIndexForSwap = 0;
    uint32_t relativeDestination = 0;
    if ((command & 0x8000u) != 0u) {
      sourceIndexForSwap = command & 0x07ffu;
      relativeDestination = (command >> 11u) & 0x0fu;
    } else if ((command & 0x4000u) != 0u) {
      sourceIndexForSwap = previousSource + ((command & 0x3fe0u) >> 5u);
      relativeDestination = (command & 0x001fu) + 16u;
    } else if ((command & 0x2000u) != 0u) {
      const uint32_t polygon = command & 0x07ffu;
      if (polygon >= destination.size())
        return false;
      destination[polygon] ^= command & 0x1800u;
      previousSource = polygon;
      continue;
    } else {
      sourceIndexForSwap = command & 0x0fffu;
      if (cursor >= delta.swapped.size())
        return false;
      relativeDestination = delta.swapped[cursor++] & 0x0fffu;
    }
    const uint64_t destinationIndex =
        static_cast<uint64_t>(sourceIndexForSwap) + relativeDestination + 1u;
    if (sourceIndexForSwap >= destination.size() ||
        destinationIndex >= destination.size())
      return false;
    std::swap(destination[sourceIndexForSwap],
              destination[static_cast<std::size_t>(destinationIndex)]);
    previousSource = sourceIndexForSwap;
  }
  return true;
}

bool decodeSceneryPath(const Memory &memory, uint32_t entryBase,
                       uint32_t stateCount, SceneryPathCache &result) {
  result = {};
  if (stateCount == 0u || stateCount > kMaxSceneryPathStates)
    return false;

  uint32_t itemAddress = 0;
  std::vector<uint16_t> state;
  if (!readSceneryPathItem(memory, entryBase, 0u, itemAddress) ||
      !readDecodedSceneryList(memory, itemAddress, state))
    return false;

  std::vector<std::vector<uint16_t>> states;
  states.reserve(stateCount);
  states.push_back(state);
  for (uint32_t index = 1u; index < stateCount; ++index) {
    SceneryDeltaWords delta;
    std::vector<uint16_t> next;
    if (!readSceneryPathItem(memory, entryBase, index, itemAddress) ||
        !readSceneryDelta(memory, itemAddress, delta) ||
        !applySceneryDeltaForward(delta, states.back(), next))
      return false;
    states.push_back(std::move(next));
  }

  std::vector<uint16_t> endpoint;
  if (!readSceneryPathItem(memory, entryBase, stateCount, itemAddress) ||
      !readDecodedSceneryList(memory, itemAddress, endpoint) ||
      endpoint != states.back())
    return false;

  result.entryBase = entryBase;
  result.stateCount = stateCount;
  result.currentIndex = 0u;
  result.valid = true;
  result.detailTracePending = true;
  result.revision = ++g_sceneryPathRevision;
  result.states = std::move(states);
  return true;
}

bool buildSceneryPathCache(const Memory &memory, uint32_t entryBase,
                           uint32_t stateCount) {
  return decodeSceneryPath(memory, entryBase, stateCount, g_sceneryPathCache);
}

bool readResidentScenery(
    const Memory &memory,
    std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    uint32_t &worldCount, uint32_t &residentEntryCount,
    uint32_t &residentFaceCount) noexcept {
  const uint32_t owner = memory.read32(kResidentSceneryOwnerAddress);
  if (!isRamRange(owner, 0x14u))
    return false;
  const uint32_t table = memory.read32(owner + 0x10u);
  if (!isRamRange(table, 4u))
    return false;

  worldCount = memory.read32(table);
  if (worldCount == 0 || worldCount > kMaxSceneryWorlds ||
      !isRamRange(table, kResidentSceneryFirstRecord +
                             worldCount * kResidentSceneryRecordStride))
    return false;

  uint64_t entries = 0;
  uint64_t faces = 0;
  for (uint32_t world = 0; world < worldCount; ++world) {
    const uint32_t recordOffset =
        kResidentSceneryFirstRecord + world * kResidentSceneryRecordStride;
    const uint32_t record = table + recordOffset;
    const uint32_t info = memory.read32(record + kResidentSceneryInfoOffset);
    if (!isRamRange(info, 0x1cu))
      return false;
    const uint32_t triangles =
        memory.read32(info + kSceneryInfoTriangleCountOffset);
    const uint32_t quads = memory.read32(info + kSceneryInfoQuadCountOffset);
    if (triangles > kMaxSceneryPolygonIdCount ||
        quads > kMaxSceneryPolygonIdCount)
      return false;
    worlds[world] = {record, info, triangles, quads};
    entries += static_cast<uint64_t>(triangles) + quads;
    faces += static_cast<uint64_t>(triangles) + 2u * quads;
  }
  if (entries > static_cast<uint64_t>(std::numeric_limits<int16_t>::max()) ||
      faces > std::numeric_limits<uint32_t>::max())
    return false;
  residentEntryCount = static_cast<uint32_t>(entries);
  residentFaceCount = static_cast<uint32_t>(faces);
  return true;
}

bool readResidentSceneryPartial(
    const Memory &memory,
    std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    uint32_t &worldCount, uint32_t &residentEntryCount,
    uint32_t &residentFaceCount) noexcept {
  const uint32_t owner = memory.read32(kResidentSceneryOwnerAddress);
  if (!isRamRange(owner, 0x14u))
    return false;
  const uint32_t table = memory.read32(owner + 0x10u);
  if (!isRamRange(table, 4u))
    return false;
  worldCount = memory.read32(table);
  if (worldCount == 0u || worldCount > kMaxSceneryWorlds ||
      !isRamRange(table, kResidentSceneryFirstRecord +
                             worldCount * kResidentSceneryRecordStride)) {
    return false;
  }

  uint64_t entries = 0u;
  uint64_t faces = 0u;
  uint32_t validRows = 0u;
  for (uint32_t world = 0u; world < worldCount; ++world) {
    const uint32_t record = table + kResidentSceneryFirstRecord +
                            world * kResidentSceneryRecordStride;
    const uint32_t info = memory.read32(record + kResidentSceneryInfoOffset);
    worlds[world].recordAddress = record;
    worlds[world].infoAddress = info;
    if (!isRamRange(info, 0x1cu))
      continue;
    const uint32_t triangles =
        memory.read32(info + kSceneryInfoTriangleCountOffset);
    const uint32_t quads = memory.read32(info + kSceneryInfoQuadCountOffset);
    if (triangles > kMaxSceneryPolygonIdCount ||
        quads > kMaxSceneryPolygonIdCount) {
      continue;
    }
    worlds[world].triangleCount = triangles;
    worlds[world].quadCount = quads;
    entries += static_cast<uint64_t>(triangles) + quads;
    faces += static_cast<uint64_t>(triangles) + 2u * quads;
    ++validRows;
  }
  if (validRows == 0u ||
      entries > static_cast<uint64_t>(std::numeric_limits<int16_t>::max()) ||
      faces > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  residentEntryCount = static_cast<uint32_t>(entries);
  residentFaceCount = static_cast<uint32_t>(faces);
  return true;
}

void buildResidentSceneryList(
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    uint32_t worldCount, uint32_t residentEntryCount) {
  g_scenerySelection.entries.clear();
  g_scenerySelection.entries.reserve(residentEntryCount);

  // func_8004103C consumes its list backwards. Store the reverse of a simple
  // world/triangle/quad traversal so the decoder observes stable ascending
  // world and polygon indices. State 3 asks Crash's original quad path to emit
  // both constituent triangles; states 1 and 2 are the SLST's half-quad culls.
  for (uint32_t world = worldCount; world-- > 0;) {
    for (uint32_t quad = worlds[world].quadCount; quad-- > 0;) {
      g_scenerySelection.entries.push_back(
          static_cast<uint16_t>((world << 13u) | (3u << 11u) | quad));
    }
    for (uint32_t triangle = worlds[world].triangleCount; triangle-- > 0;) {
      g_scenerySelection.entries.push_back(
          static_cast<uint16_t>((world << 13u) | triangle));
    }
  }
}

uint32_t sceneryEntryIdentity(uint16_t packed) noexcept {
  const uint32_t world = packed >> 13u;
  const uint32_t polygonClass = ((packed >> 11u) & 3u) != 0u ? 1u : 0u;
  const uint32_t id = packed & 0x7ffu;
  return ((world * 2u + polygonClass) * kMaxSceneryPolygonIdCount) + id;
}

void publishResidentOrderingTableProofCaptureLocked(
    const ResidentOrderingTableProofState &proof) {
  if (proof.captureId == 0u)
    return;
  const ResidentOrderingTableProof value{proof.topologyFingerprint,
                                         proof.matches, proof.mismatches,
                                         proof.unavailable};
  const auto existing = std::find_if(
      g_residentOrderingTableProofCaptures.begin(),
      g_residentOrderingTableProofCaptures.end(), [&](const auto &capture) {
        return capture.captureId == proof.captureId;
      });
  if (existing != g_residentOrderingTableProofCaptures.end()) {
    existing->proof = value;
    return;
  }
  constexpr std::size_t kRetainedProofCaptures = 16u;
  g_residentOrderingTableProofCaptures.push_back({proof.captureId, value});
  while (g_residentOrderingTableProofCaptures.size() > kRetainedProofCaptures) {
    g_residentOrderingTableProofCaptures.pop_front();
  }
}

void prepareResidentOrderingTableProof(
    const ResidentPresentationSnapshot &snapshot, const CPUContext &ctx) {
  std::scoped_lock lock(g_residentOrderingTableProofMutex);
  ResidentOrderingTableProofState &proof = g_residentOrderingTableProof;
  if (proof.topologyFingerprint != snapshot.topologyFingerprint) {
    const uint64_t matches = proof.matches;
    const uint64_t mismatches = proof.mismatches;
    const uint64_t unavailable = proof.unavailable;
    proof = {};
    proof.topologyFingerprint = snapshot.topologyFingerprint;
    // The decoded AVSZ3/AVSZ4 program is one immutable Crash renderer
    // function, not a property learned from a particular mesh. Preserve its
    // exact packet corpus across resource paging while replacing all
    // face-local expectations below. Any disagreement remains permanently
    // fail-closed for the run instead of being hidden by a section change.
    proof.matches = matches;
    proof.mismatches = mismatches;
    proof.unavailable = unavailable;
  }
  proof.captureId = snapshot.captureId;
  for (std::size_t index = 0u; index < proof.expectedProjectionControls.size();
       ++index) {
    proof.expectedProjectionControls[index] = ctx.cop2c[index];
  }
  proof.expectedProjectionControlsValid = true;
  proof.expectedBuckets.assign(kSceneryIdentityCount * 3u, 0u);
  proof.expectedValid.assign(kSceneryIdentityCount * 3u, 0u);
  constexpr std::size_t kExpectedModelInputSlots = kSceneryIdentityCount * 3u;
  if (proof.expectedModelInputs.size() != kExpectedModelInputSlots) {
    proof.expectedModelInputs.resize(kExpectedModelInputSlots);
    proof.expectedModelInputGenerations.assign(kExpectedModelInputSlots, 0u);
    proof.modelInputGeneration = 0u;
  }
  ++proof.modelInputGeneration;
  if (proof.modelInputGeneration == 0u) {
    std::fill(proof.expectedModelInputGenerations.begin(),
              proof.expectedModelInputGenerations.end(), 0u);
    proof.modelInputGeneration = 1u;
  }
  for (const ResidentPresentationFace &face : snapshot.faces) {
    // A retained/reprojected resource has no corresponding packet in this
    // construction capture. It is presentation input, not evidence against
    // Crash's current AVSZ path.
    if (face.retainedResource || !face.orderingTableBucketValid ||
        face.half > 2u) {
      continue;
    }
    const uint32_t slot = sceneryEntryIdentity(face.packed) * 3u + face.half;
    if (slot >= proof.expectedBuckets.size())
      continue;
    if (proof.expectedValid[slot] != 0u &&
        proof.expectedBuckets[slot] != face.orderingTableBucket) {
      ++proof.unavailable;
      proof.expectedValid[slot] = 0u;
      continue;
    }
    proof.expectedBuckets[slot] = face.orderingTableBucket;
    proof.expectedValid[slot] = 1u;
    if (!face.nearPlaneClipped) {
      proof.expectedModelInputs[slot] = face.modelPosition;
      proof.expectedModelInputGenerations[slot] = proof.modelInputGeneration;
    }
  }
  publishResidentOrderingTableProofCaptureLocked(proof);
}

bool validSceneryEntry(
    uint16_t packed,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    uint32_t worldCount) noexcept {
  const uint32_t world = packed >> 13u;
  const uint32_t state = (packed >> 11u) & 3u;
  const uint32_t id = packed & 0x7ffu;
  if (world >= worldCount)
    return false;
  return state == 0u ? id < worlds[world].triangleCount
                     : id < worlds[world].quadCount;
}

uint32_t sceneryFaceCount(const std::vector<uint16_t> &entries) noexcept {
  uint32_t faces = 0;
  for (const uint16_t packed : entries) {
    const uint32_t state = (packed >> 11u) & 3u;
    faces += state == 0u ? 1u : std::popcount(state);
  }
  return faces;
}

enum class AdjacentMergeFailure : uint8_t {
  None,
  NoMatchingHistory,
  InvalidList,
  InvalidEntry,
  DuplicateEntry,
  OrderingConflict,
  CapacityExceeded,
};

struct AdjacentMergeStats {
  uint32_t snapshotsUsed = 0;
  uint32_t entriesAdded = 0;
  uint32_t quadStatesUpgraded = 0;
  uint32_t reorderedAnchors = 0;
  AdjacentMergeFailure failure = AdjacentMergeFailure::None;
};

bool initializeSceneryDecoderSequence(
    const std::vector<uint16_t> &stored,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    uint32_t worldCount, std::vector<uint16_t> &decoder,
    AdjacentMergeStats &stats) {
  decoder.assign(stored.rbegin(), stored.rend());
  std::vector<uint8_t> seen(kSceneryIdentityCount);
  for (const uint16_t packed : decoder) {
    if (!validSceneryEntry(packed, worlds, worldCount)) {
      stats.failure = AdjacentMergeFailure::InvalidEntry;
      return false;
    }
    const uint32_t identity = sceneryEntryIdentity(packed);
    if (seen[identity] != 0u) {
      stats.failure = AdjacentMergeFailure::DuplicateEntry;
      return false;
    }
    seen[identity] = 1u;
  }
  return true;
}

bool buildCurrentSceneryList(
    const Memory &memory, uint32_t listAddress,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    uint32_t worldCount, AdjacentMergeStats &stats) {
  std::vector<uint16_t> current;
  if (!readDecodedSceneryList(memory, listAddress, current)) {
    stats.failure = AdjacentMergeFailure::InvalidList;
    return false;
  }

  // Reuse the checked decoder initialization only for validation. The stored
  // order remains exactly the current source-authored SLST order; resident
  // presentation extensions are deliberately layered around it later.
  std::vector<uint16_t> decoder;
  if (!initializeSceneryDecoderSequence(current, worlds, worldCount, decoder,
                                        stats))
    return false;
  g_scenerySelection.entries = std::move(current);
  return true;
}

bool mergeSceneryDecoderSequence(
    std::vector<uint16_t> &decoder, const std::vector<uint16_t> &storedNeighbor,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    uint32_t worldCount, AdjacentMergeStats &stats) {
  std::vector<int32_t> positions(kSceneryIdentityCount, -1);
  for (std::size_t index = 0; index < decoder.size(); ++index)
    positions[sceneryEntryIdentity(decoder[index])] =
        static_cast<int32_t>(index);

  struct Insertion {
    std::size_t before = 0;
    std::vector<uint16_t> entries;
  };
  std::vector<Insertion> insertions;
  std::vector<uint16_t> pending;
  std::vector<uint8_t> neighborSeen(kSceneryIdentityCount);
  int32_t previousAnchor = -1;
  uint32_t commonAnchors = 0;

  // Both lists are traversed in the exact order used by func_8004103C. Runs
  // found only in the neighboring state are inserted at their next common
  // polygon. Common polygons always retain the current list's authoritative
  // order: SLST swap nodes legitimately reorder them between camera samples,
  // so requiring both complete orders would reject almost every real delta.
  for (auto it = storedNeighbor.rbegin(); it != storedNeighbor.rend(); ++it) {
    const uint16_t packed = *it;
    if (!validSceneryEntry(packed, worlds, worldCount)) {
      stats.failure = AdjacentMergeFailure::InvalidEntry;
      return false;
    }
    const uint32_t identity = sceneryEntryIdentity(packed);
    if (neighborSeen[identity] != 0u) {
      stats.failure = AdjacentMergeFailure::DuplicateEntry;
      return false;
    }
    neighborSeen[identity] = 1u;

    const int32_t position = positions[identity];
    if (position < 0) {
      pending.push_back(packed);
      continue;
    }
    ++commonAnchors;
    if (position < previousAnchor)
      ++stats.reorderedAnchors;
    if (!pending.empty()) {
      insertions.push_back(
          {static_cast<std::size_t>(position), std::move(pending)});
      pending.clear();
    }

    const uint32_t currentState = (decoder[position] >> 11u) & 3u;
    const uint32_t neighborState = (packed >> 11u) & 3u;
    if (currentState != 0u && neighborState != 0u) {
      const uint32_t mergedState = currentState | neighborState;
      if (mergedState != currentState) {
        decoder[position] = static_cast<uint16_t>(
            (decoder[position] & ~(3u << 11u)) | (mergedState << 11u));
        ++stats.quadStatesUpgraded;
      }
    }
    previousAnchor = position;
  }
  if (commonAnchors == 0u) {
    stats.failure = AdjacentMergeFailure::OrderingConflict;
    return false;
  }
  if (!pending.empty()) {
    const std::size_t afterLastAnchor =
        previousAnchor >= 0 ? static_cast<std::size_t>(previousAnchor + 1)
                            : decoder.size();
    insertions.push_back({afterLastAnchor, std::move(pending)});
  }

  std::stable_sort(insertions.begin(), insertions.end(),
                   [](const Insertion &left, const Insertion &right) {
                     return left.before < right.before;
                   });

  std::size_t added = 0;
  for (const Insertion &insertion : insertions)
    added += insertion.entries.size();
  if (decoder.size() + added > kMaxDecodedSceneryEntries) {
    stats.failure = AdjacentMergeFailure::CapacityExceeded;
    return false;
  }

  std::vector<uint16_t> merged;
  merged.reserve(decoder.size() + added);
  std::size_t insertionIndex = 0;
  for (std::size_t position = 0; position <= decoder.size(); ++position) {
    while (insertionIndex < insertions.size() &&
           insertions[insertionIndex].before == position) {
      const auto &entries = insertions[insertionIndex].entries;
      merged.insert(merged.end(), entries.begin(), entries.end());
      ++insertionIndex;
    }
    if (position < decoder.size())
      merged.push_back(decoder[position]);
  }
  if (insertionIndex != insertions.size()) {
    stats.failure = AdjacentMergeFailure::OrderingConflict;
    return false;
  }
  decoder = std::move(merged);
  stats.entriesAdded += static_cast<uint32_t>(added);
  ++stats.snapshotsUsed;
  return true;
}

bool buildAdjacentSceneryList(
    const Memory &memory, uint32_t listAddress,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    uint32_t worldCount, AdjacentMergeStats &stats) {
  if (g_sceneryDeltaHistory.lastDestination != listAddress ||
      g_sceneryDeltaHistory.preceding.empty()) {
    stats.failure = AdjacentMergeFailure::NoMatchingHistory;
    return false;
  }

  std::vector<uint16_t> current;
  if (!readDecodedSceneryList(memory, listAddress, current)) {
    stats.failure = AdjacentMergeFailure::InvalidList;
    return false;
  }
  std::vector<uint16_t> decoder;
  if (!initializeSceneryDecoderSequence(current, worlds, worldCount, decoder,
                                        stats))
    return false;

  const uint32_t requested = adjacentSceneryDeltaCount();
  const std::size_t available = g_sceneryDeltaHistory.preceding.size();
  const std::size_t first =
      available > requested ? available - requested : std::size_t{0};
  for (std::size_t index = available; index-- > first;) {
    if (!mergeSceneryDecoderSequence(
            decoder, g_sceneryDeltaHistory.preceding[index].entries, worlds,
            worldCount, stats))
      return false;
  }

  g_scenerySelection.entries.assign(decoder.rbegin(), decoder.rend());
  return g_scenerySelection.entries != current;
}

bool buildSymmetricSceneryList(
    const Memory &memory, uint32_t listAddress,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    uint32_t worldCount, AdjacentMergeStats &stats, uint32_t &leftUsed,
    uint32_t &rightUsed, uint32_t radius) {
  leftUsed = 0u;
  rightUsed = 0u;
  if (!g_sceneryPathCache.valid ||
      g_sceneryPathCache.currentIndex >= g_sceneryPathCache.states.size()) {
    stats.failure = AdjacentMergeFailure::NoMatchingHistory;
    return false;
  }

  std::vector<uint16_t> current;
  if (!readDecodedSceneryList(memory, listAddress, current) ||
      current != g_sceneryPathCache.states[g_sceneryPathCache.currentIndex]) {
    stats.failure = AdjacentMergeFailure::InvalidList;
    return false;
  }
  std::vector<uint16_t> decoder;
  if (!initializeSceneryDecoderSequence(current, worlds, worldCount, decoder,
                                        stats))
    return false;

  const uint32_t currentIndex = g_sceneryPathCache.currentIndex;
  for (uint32_t distance = 1u; distance <= radius; ++distance) {
    if (distance <= currentIndex) {
      const uint32_t index = currentIndex - distance;
      if (!mergeSceneryDecoderSequence(decoder,
                                       g_sceneryPathCache.states[index], worlds,
                                       worldCount, stats))
        return false;
      ++leftUsed;
    }
    const uint64_t right = static_cast<uint64_t>(currentIndex) + distance;
    if (right < g_sceneryPathCache.states.size()) {
      if (!mergeSceneryDecoderSequence(
              decoder,
              g_sceneryPathCache.states[static_cast<std::size_t>(right)],
              worlds, worldCount, stats))
        return false;
      ++rightUsed;
    }
  }

  g_scenerySelection.entries.assign(decoder.rbegin(), decoder.rend());
  return g_scenerySelection.entries != current;
}

struct ResidentSceneryGeometry {
  uint32_t translationXy = 0;
  int32_t translationZ = 0;
  uint32_t trianglePivot = 0;
  uint32_t quadStart = 0;
  uint32_t vertexPivot = 0;
  uint32_t colorTable = 0;
  uint32_t materialRemap = 0;
  uint32_t materialTable = 0;
  uint32_t vertexCount = 0;
};

struct ResidentResourceKey {
  uint32_t resourceId = 0;
  uint64_t geometryFingerprint = 0;

  auto operator<=>(const ResidentResourceKey &) const = default;
};

struct CachedResidentVertex {
  uint32_t rawXy = 0;
  uint16_t rawZ = 0;
  bool colorDecoded = false;
  std::array<float, 3> color255{};
};

struct CachedResidentFace {
  ResidentPresentationFace presentation{};
  // Keep source material separate from a constructor's flat-black output.
  // Retention must be able to light/texture it again in a later capture.
  ResidentPresentationMaterial unconstructedMaterial{};
  std::array<uint32_t, 3> indices{};
  std::array<uint32_t, 4> completeIndices{};
};

struct SceneryResourceVariants {
  uint64_t observedPathRevision = 0;
  std::vector<SceneryVariantPair> pairs;
  std::vector<uint32_t> refinementVeto;
  std::shared_ptr<const SceneryVariantPlan> plan;
};
std::map<std::tuple<uint32_t, uint64_t, uint64_t>,
         std::weak_ptr<SceneryResourceVariants>>
    g_sceneryVariantResources;

struct CachedResidentResource {
  ResidentPresentationResource descriptor{};
  std::vector<CachedResidentVertex> vertices;
  std::vector<CachedResidentFace> faces;
  // Sparse, capture-owned highest-detail records. Retained geometry must carry
  // this choice with its native oracle instead of falling back to its last
  // distance-selected material. Entries follow resourceFaceOrdinal order.
  std::vector<SceneryDetailOverride> highestMaterials;
  // The evidence belongs to this immutable resource, not a camera frame.
  // Published plans remain immutable even if a later path vetoes a candidate.
  std::shared_ptr<SceneryResourceVariants> variants;
  // Immutable topology is cached independently of projected presentation
  // faces. This is the source of the device-local payload and survives a later
  // production path which omits CPU projection for raw opaque geometry.
  std::vector<ResidentGpuTopologyFace> gpuTopology;
  // Shared immutable upload source. Copies of a cohort cache retain the same
  // payload, so renderer-side device-local storage can survive both ordinary
  // frames and the bounded transactional handoff without rebuilding topology.
  std::shared_ptr<const ResidentGpuResourcePayload> gpuPayload;
  uint32_t colorTableAddress = 0u;
  uint64_t colorTableFingerprint = 0u;
  // Static (non-remapped) material records are immutable scene data, but the
  // paging allocator can reuse their RAM address. Hash the exact contiguous
  // token range so the sparse producer lane can reuse decoded opaque records
  // only while both identity and contents still agree.
  uint32_t materialTableAddress = 0u;
  uint32_t staticMaterialByteCount = 0u;
  uint32_t materialRemapAddress = 0u;
  uint32_t materialRemapByteCount = 0u;
  uint32_t materialNearestShift = 0u;
  uint64_t staticMaterialFingerprint = 0u;
  bool constructedLighting = false;
  uint32_t outsidePresentationCaptures = 0;
  // Incoming resources are admitted while a resident-table transaction is
  // still being assembled.  Outgoing resources are the exact difference of
  // the immediately preceding committed cohort.  Keeping the roles explicit
  // prevents speculative rows from becoming an unbounded scene history.
  bool adjacentOutgoing = false;
  uint32_t lastObservedCaptureId = 0;
  uint32_t absenceObservations = 0;
};

bool ensureResidentGpuResourcePayload(CachedResidentResource &resource) {
  const auto reusable = [&] {
    const auto &payload = resource.gpuPayload;
    return payload != nullptr &&
           payload->resourceId == resource.descriptor.resourceId &&
           payload->geometryFingerprint ==
               resource.descriptor.geometryFingerprint &&
           payload->colorTableFingerprint == resource.colorTableFingerprint &&
           payload->staticMaterialFingerprint ==
               resource.staticMaterialFingerprint &&
           payload->vertices.size() == resource.vertices.size() &&
           payload->faces.size() == resource.gpuTopology.size();
  };
  if (reusable())
    return true;

  auto payload = std::make_shared<ResidentGpuResourcePayload>();
  payload->resourceId = resource.descriptor.resourceId;
  payload->geometryFingerprint = resource.descriptor.geometryFingerprint;
  payload->colorTableFingerprint = resource.colorTableFingerprint;
  payload->staticMaterialFingerprint = resource.staticMaterialFingerprint;
  payload->vertices.reserve(resource.vertices.size());
  for (const CachedResidentVertex &source : resource.vertices) {
    ResidentGpuRawVertex vertex;
    vertex.rawXy = source.rawXy;
    vertex.rawZ = source.rawZ;
    vertex.colorDecoded = source.colorDecoded;
    if (source.colorDecoded) {
      for (std::size_t component = 0u; component < vertex.color255.size();
           ++component) {
        const float value = source.color255[component];
        if (!std::isfinite(value) || value < 0.0f || value > 255.0f)
          return false;
        vertex.color255[component] = static_cast<uint8_t>(value);
      }
    }
    payload->vertices.push_back(vertex);
  }
  payload->faces.reserve(resource.gpuTopology.size());
  for (const ResidentGpuTopologyFace &source : resource.gpuTopology) {
    if (std::any_of(source.sourceVertexIndex.begin(),
                    source.sourceVertexIndex.end(),
                    [&](uint32_t index) {
                      return index >= payload->vertices.size();
                    }) ||
        (source.half != 0u &&
         std::any_of(source.completeSourceVertexIndex.begin(),
                     source.completeSourceVertexIndex.end(),
                     [&](uint32_t index) {
                       return index >= payload->vertices.size();
                     }))) {
      return false;
    }
    payload->faces.push_back(source);
  }
  payload->staticOpaqueFaces.reserve(resource.faces.size());
  for (std::size_t ordinal = 0u; ordinal < resource.faces.size(); ++ordinal) {
    const CachedResidentFace &source = resource.faces[ordinal];
    const ResidentPresentationMaterial &material = source.presentation.material;
    if (!material.decoded || material.dynamicTexture ||
        material.semiTransparent) {
      continue;
    }
    if (source.presentation.resourceFaceOrdinal != ordinal ||
        (material.sourceToken & 0x800u) != 0u ||
        material.packetVertexCount != (source.presentation.half == 0u
                                           ? (material.textured ? 3u : 0u)
                                           : (material.textured ? 4u : 0u))) {
      return false;
    }
    ResidentGpuStaticOpaqueFace face;
    face.resourceFaceOrdinal = static_cast<uint32_t>(ordinal);
    face.packed = source.presentation.packed;
    face.sourceToken = material.sourceToken;
    face.resolvedToken = material.resolvedToken;
    face.texturePage = material.texturePage;
    face.clut = material.clut;
    face.half = source.presentation.half;
    face.textured = material.textured;
    if (face.textured) {
      for (std::size_t corner = 0u; corner < face.packetUv.size(); ++corner) {
        const float u = material.packetUv[corner][0];
        const float v = material.packetUv[corner][1];
        if (!std::isfinite(u) || !std::isfinite(v) || u < 0.0f || v < 0.0f ||
            u > 255.0f || v > 255.0f || std::trunc(u) != u ||
            std::trunc(v) != v) {
          return false;
        }
        face.packetUv[corner] = static_cast<uint16_t>(
            static_cast<uint32_t>(u) | (static_cast<uint32_t>(v) << 8u));
      }
      for (std::size_t component = 0u; component < face.uvDomain.size();
           ++component) {
        const float value = material.uvDomain[component];
        if (!std::isfinite(value) || value < 0.0f || value > 255.0f ||
            std::trunc(value) != value) {
          return false;
        }
        face.uvDomain[component] = static_cast<uint8_t>(value);
      }
    }
    payload->staticOpaqueFaces.push_back(face);
  }
  resource.gpuPayload = std::move(payload);
  return true;
}

struct PendingResidentResource {
  ResidentResourceKey key{};
  uint32_t lastCaptureId = 0;
  uint32_t observations = 0;
  CachedResidentResource cache{};
};

struct ResidentCohortJournal {
  uint64_t activeFingerprint = 0;
  uint64_t candidateFingerprint = 0;
  uint32_t candidateObservations = 0;
  uint32_t generation = 0;
  std::vector<ResidentResourceKey> activeResources;
  std::vector<ResidentResourceKey> candidateResources;
  std::vector<CachedResidentResource> activeCache;
  // Non-active members of the in-flight handoff: independently certified
  // incoming rows plus the immediately outgoing overlap.
  std::vector<CachedResidentResource> sceneCache;
  std::vector<PendingResidentResource> pendingResources;
  std::unordered_map<uint32_t, uint64_t> geometryByResourceId;
};

ResidentCohortJournal g_residentCohortJournal{};
// Single-threaded construction scratch. The stable resident path exchanges
// this with the previous active cohort so its per-resource vertex/face
// capacities survive into the next frame instead of being allocated anew.
std::vector<CachedResidentResource> g_residentCurrentCacheScratch;

uint64_t hashResidentMemoryRange(const Memory &memory, uint64_t hash,
                                 uint32_t address,
                                 uint32_t byteCount) noexcept {
  const uint32_t physical = Memory::toPhysical(address);
  if (physical >= Memory::RAM_SIZE || byteCount > Memory::RAM_SIZE - physical) {
    for (uint32_t offset = 0u; offset < byteCount; ++offset) {
      hash ^= memory.read8(address + offset);
      hash *= UINT64_C(1099511628211);
    }
    return hash;
  }
  const uint8_t *bytes = memory.ramPtr() + physical;
  for (uint32_t offset = 0u; offset < byteCount; ++offset) {
    hash ^= bytes[offset];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

uint64_t
residentGeometryFingerprint(const Memory &memory,
                            const ResidentSceneryWorld &resident,
                            const ResidentSceneryGeometry &geometry) noexcept {
  uint64_t hash = UINT64_C(14695981039346656037);
  hash32(hash, resident.triangleCount);
  hash32(hash, resident.quadCount);
  hash32(hash, geometry.vertexCount);
  hash = hashResidentMemoryRange(memory, hash,
                                 geometry.trianglePivot -
                                     resident.triangleCount * sizeof(uint32_t),
                                 resident.triangleCount * 6u);
  hash = hashResidentMemoryRange(memory, hash, geometry.quadStart,
                                 resident.quadCount * 8u);
  return hashResidentMemoryRange(memory, hash,
                                 geometry.vertexPivot -
                                     geometry.vertexCount * sizeof(uint32_t),
                                 geometry.vertexCount * 6u);
}

bool buildResidentResourceCatalog(
    const Memory &memory,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    const std::array<ResidentSceneryGeometry, kMaxSceneryWorlds> &geometry,
    const std::array<uint8_t, kMaxSceneryWorlds> &geometryValidity,
    uint32_t worldCount, std::vector<ResidentPresentationResource> &resources,
    uint64_t &setFingerprint, bool &placementValidated) {
  resources.clear();
  resources.reserve(worldCount);
  std::vector<ResidentResourceKey> keys;
  keys.reserve(worldCount);
  std::optional<std::array<int32_t, 3>> commonCameraOffset;
  placementValidated = true;
  for (uint32_t world = 0u; world < worldCount; ++world) {
    const ResidentSceneryWorld &resident = worlds[world];
    const ResidentSceneryGeometry &decoded = geometry[world];
    ResidentPresentationResource resource;
    const uint32_t resourcePair = memory.read32(resident.recordAddress);
    if (isRamRange(resourcePair, 8u)) {
      resource.entryAddress = memory.read32(resourcePair);
      resource.resourceId = memory.read32(resourcePair + 4u);
    }
    resource.infoAddress = resident.infoAddress;
    const bool infoValid = isRamRange(resident.infoAddress, 0x1cu);
    if (geometryValidity[world] != 0u) {
      resource.geometryFingerprint =
          residentGeometryFingerprint(memory, resident, decoded);
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      if (infoValid) {
        resource.origin[axis] = static_cast<int32_t>(
            memory.read32(resident.infoAddress + axis * sizeof(uint32_t)));
      }
      resource.translation[axis] = static_cast<int32_t>(memory.read32(
          resident.recordAddress + kResidentSceneryTranslationXOffset +
          axis * sizeof(uint32_t)));
    }
    if (infoValid)
      resource.flags = memory.read32(resident.infoAddress + 0x0cu);
    resource.vertexCount = decoded.vertexCount;
    resource.triangleCount = resident.triangleCount;
    resource.quadCount = resident.quadCount;
    resource.identityValid =
        geometryValidity[world] != 0u && infoValid &&
        (resource.resourceId & 1u) != 0u &&
        isRamRange(resource.entryAddress, 0x30u) &&
        resource.infoAddress == resource.entryAddress + 0x30u;

    bool placementValid = geometryValidity[world] != 0u && resource.flags <= 1u;
    std::array<int32_t, 3> cameraOffset{};
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      placementValid &= (resource.origin[axis] & 3) == 0;
      cameraOffset[axis] =
          resource.translation[axis] - resource.origin[axis] / 4;
    }
    if (placementValid && resource.flags == 1u) {
      placementValid = cameraOffset == std::array<int32_t, 3>{};
    } else if (placementValid) {
      if (!commonCameraOffset.has_value())
        commonCameraOffset = cameraOffset;
      else
        placementValid = cameraOffset == *commonCameraOffset;
    }
    resource.placementValidated = placementValid;
    placementValidated &= placementValid;
    placementValidated &= resource.identityValid;
    resources.push_back(resource);
    keys.push_back({resource.resourceId, resource.geometryFingerprint});
  }

  std::sort(keys.begin(), keys.end());
  if (std::adjacent_find(keys.begin(), keys.end(),
                         [](const auto &left, const auto &right) {
                           return left.resourceId == right.resourceId;
                         }) != keys.end()) {
    placementValidated = false;
  }
  setFingerprint = UINT64_C(14695981039346656037);
  hash32(setFingerprint, static_cast<uint32_t>(keys.size()));
  for (const ResidentResourceKey &key : keys) {
    hash32(setFingerprint, key.resourceId);
    hash32(setFingerprint, static_cast<uint32_t>(key.geometryFingerprint));
    hash32(setFingerprint,
           static_cast<uint32_t>(key.geometryFingerprint >> 32u));
  }
  return std::all_of(
      resources.begin(), resources.end(),
      [](const auto &resource) { return resource.identityValid; });
}

ResidentResourceKey
residentResourceKey(const ResidentPresentationResource &resource) noexcept {
  return {resource.resourceId, resource.geometryFingerprint};
}

std::vector<ResidentResourceKey> residentResourceKeys(
    const std::vector<ResidentPresentationResource> &resources) {
  std::vector<ResidentResourceKey> keys;
  keys.reserve(resources.size());
  for (const ResidentPresentationResource &resource : resources)
    keys.push_back(residentResourceKey(resource));
  std::sort(keys.begin(), keys.end());
  return keys;
}

void upsertResidentSceneResource(
    std::vector<CachedResidentResource> &sceneCache,
    CachedResidentResource resource) {
  const ResidentResourceKey key = residentResourceKey(resource.descriptor);
  const auto existing = std::find_if(
      sceneCache.begin(), sceneCache.end(), [&](const auto &candidate) {
        return residentResourceKey(candidate.descriptor) == key;
      });
  if (existing == sceneCache.end())
    sceneCache.push_back(std::move(resource));
  else
    *existing = std::move(resource);
}

bool residentResourcePayloadComplete(
    const CachedResidentResource &resource) noexcept {
  const uint64_t expectedFaces =
      static_cast<uint64_t>(resource.descriptor.triangleCount) +
      2u * resource.descriptor.quadCount;
  return expectedFaces == resource.faces.size() &&
         std::all_of(resource.faces.begin(), resource.faces.end(),
                     [](const CachedResidentFace &face) {
                       return face.presentation.material.decoded &&
                              face.presentation.colorDecoded &&
                              face.presentation.orderingTableBucketValid;
                     });
}

bool residentResourceStructuralPlacement(
    const ResidentPresentationResource &resource,
    std::array<int32_t, 3> &cameraOffset) noexcept {
  if (resource.flags > 1u)
    return false;
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    if ((resource.origin[axis] & 3) != 0)
      return false;
    cameraOffset[axis] = resource.translation[axis] - resource.origin[axis] / 4;
  }
  return resource.flags == 0u || cameraOffset == std::array<int32_t, 3>{};
}

// Certify resources independently of the table-wide transaction. A streamed
// table rewrite can leave one unrelated row transient for dozens of captures;
// suppressing every already coherent incoming resource during that interval
// creates a visible loading hole. The current SLST is the intrinsic placement
// witness: a flags-0 row is admitted only when its offset is used by a
// source-selected resource in this same construction (or when the complete
// table has already proven one common placement). New identities require two
// consecutive complete observations before joining the connected scene.
void observeResidentSceneResources(
    ResidentPresentationSnapshot &snapshot,
    const std::vector<CachedResidentResource> &currentCache) {
  ResidentCohortJournal &journal = g_residentCohortJournal;
  const bool stableCompleteCohort =
      snapshot.resourceSetValidated && !journal.activeResources.empty() &&
      residentResourceKeys(snapshot.resources) == journal.activeResources;
  std::unordered_map<uint32_t, uint32_t> validResourceIdCounts;
  validResourceIdCounts.reserve(snapshot.resources.size());
  for (const ResidentPresentationResource &resource : snapshot.resources) {
    if (resource.identityValid)
      ++validResourceIdCounts[resource.resourceId];
  }
  const auto resourceIdentityConflicts = [&](const ResidentResourceKey &key) {
    const auto committed = journal.geometryByResourceId.find(key.resourceId);
    if (committed != journal.geometryByResourceId.end() &&
        committed->second != key.geometryFingerprint) {
      return true;
    }
    const bool cachedConflict =
        std::any_of(journal.sceneCache.begin(), journal.sceneCache.end(),
                    [&](const CachedResidentResource &candidate) {
                      const ResidentResourceKey candidateKey =
                          residentResourceKey(candidate.descriptor);
                      return candidateKey.resourceId == key.resourceId &&
                             candidateKey != key;
                    });
    const bool pendingConflict = std::any_of(
        journal.pendingResources.begin(), journal.pendingResources.end(),
        [&](const PendingResidentResource &candidate) {
          const ResidentResourceKey candidateKey = candidate.key;
          return candidateKey.resourceId == key.resourceId &&
                 candidateKey != key;
        });
    return cachedConflict || pendingConflict;
  };
  std::vector<std::array<int32_t, 3>> sourceCameraOffsets;
  sourceCameraOffsets.reserve(currentCache.size());
  for (std::size_t world = 0u;
       world < currentCache.size() && world < snapshot.resources.size();
       ++world) {
    const CachedResidentResource &resource = currentCache[world];
    ResidentPresentationResource &descriptor = snapshot.resources[world];
    std::array<int32_t, 3> offset{};
    descriptor.individualPayloadComplete =
        residentResourcePayloadComplete(resource);
    descriptor.individualSourceWitness =
        std::any_of(resource.faces.begin(), resource.faces.end(),
                    [](const CachedResidentFace &face) {
                      return face.presentation.sourceSelected;
                    });
    const ResidentResourceKey key = residentResourceKey(descriptor);
    if (!descriptor.identityValid ||
        validResourceIdCounts[key.resourceId] != 1u ||
        resourceIdentityConflicts(key) || descriptor.flags != 0u ||
        !residentResourceStructuralPlacement(resource.descriptor, offset) ||
        !descriptor.individualPayloadComplete ||
        !descriptor.individualSourceWitness) {
      continue;
    }
    if (std::find(sourceCameraOffsets.begin(), sourceCameraOffsets.end(),
                  offset) == sourceCameraOffsets.end()) {
      sourceCameraOffsets.push_back(offset);
    }
  }

  for (std::size_t world = 0u;
       world < currentCache.size() && world < snapshot.resources.size();
       ++world) {
    const CachedResidentResource &resource = currentCache[world];
    ResidentPresentationResource &descriptor = snapshot.resources[world];
    std::array<int32_t, 3> offset{};
    const bool structuralPlacement =
        residentResourceStructuralPlacement(descriptor, offset);
    descriptor.individualPlacementWitness =
        structuralPlacement &&
        (descriptor.flags == 1u || snapshot.resourceSetValidated ||
         std::find(sourceCameraOffsets.begin(), sourceCameraOffsets.end(),
                   offset) != sourceCameraOffsets.end());
    const ResidentResourceKey key = residentResourceKey(descriptor);
    bool valid = descriptor.identityValid &&
                 validResourceIdCounts[key.resourceId] == 1u &&
                 !resourceIdentityConflicts(key) && structuralPlacement &&
                 descriptor.individualPayloadComplete &&
                 descriptor.individualPlacementWitness;
    descriptor.individuallyValidated = valid;
    if (!valid)
      continue;
    ++snapshot.individuallyValidatedResources;

    const bool active = std::binary_search(journal.activeResources.begin(),
                                           journal.activeResources.end(), key);
    auto scene =
        std::find_if(journal.sceneCache.begin(), journal.sceneCache.end(),
                     [&](const auto &candidate) {
                       return residentResourceKey(candidate.descriptor) == key;
                     });
    if (active) {
      // A partial table can persist while textures, colors and the waving
      // vertex program continue to evolve. Refresh every independently valid
      // active row so atomic recovery does not freeze the last complete-table
      // payload for the duration of streaming. A complete stable cohort is
      // exchanged wholesale by observeResidentCohort immediately after this
      // pass, so copying every large row here would be redundant.
      if (!stableCompleteCohort) {
        const auto activeCache = std::find_if(
            journal.activeCache.begin(), journal.activeCache.end(),
            [&](const CachedResidentResource &candidate) {
              return residentResourceKey(candidate.descriptor) == key;
            });
        if (activeCache != journal.activeCache.end())
          *activeCache = resource;
      }
      descriptor.sceneCacheMember = true;
      descriptor.individualObservations = 2u;
      continue;
    }
    if (scene != journal.sceneCache.end()) {
      // Refresh material/color state when an inactive scene member briefly
      // reappears in the current paging table.
      const bool adjacentOutgoing = scene->adjacentOutgoing;
      *scene = resource;
      scene->adjacentOutgoing = adjacentOutgoing;
      scene->lastObservedCaptureId = snapshot.captureId;
      scene->absenceObservations = 0u;
      descriptor.sceneCacheMember = true;
      descriptor.individualObservations = 2u;
      continue;
    }

    auto pending = std::find_if(journal.pendingResources.begin(),
                                journal.pendingResources.end(),
                                [&](const PendingResidentResource &candidate) {
                                  return candidate.key == key;
                                });
    if (pending == journal.pendingResources.end()) {
      journal.pendingResources.push_back(
          {key, snapshot.captureId, 1u, resource});
      pending = std::prev(journal.pendingResources.end());
    } else {
      pending->observations = pending->lastCaptureId + 1u == snapshot.captureId
                                  ? pending->observations + 1u
                                  : 1u;
      pending->lastCaptureId = snapshot.captureId;
      pending->cache = resource;
    }
    descriptor.individualObservations = pending->observations;
    if (pending->observations >= 2u) {
      pending->cache.adjacentOutgoing = false;
      pending->cache.lastObservedCaptureId = snapshot.captureId;
      pending->cache.absenceObservations = 0u;
      upsertResidentSceneResource(journal.sceneCache,
                                  std::move(pending->cache));
      descriptor.sceneCacheMember = true;
      ++snapshot.individuallyAdmittedResources;
      journal.pendingResources.erase(pending);
    }
  }

  journal.pendingResources.erase(
      std::remove_if(journal.pendingResources.begin(),
                     journal.pendingResources.end(),
                     [&](const PendingResidentResource &pending) {
                       return pending.lastCaptureId + 1u < snapshot.captureId;
                     }),
      journal.pendingResources.end());

  // Admission and revocation use the same route-independent debounce.  A
  // speculative incoming row may survive one uncertain/partly rewritten
  // construction, but two consecutive captures without that certified key
  // revoke it.  The immediately outgoing committed difference has a separate
  // lifetime and is replaced only by the next completed handoff below.
  std::vector<ResidentResourceKey> observedResources;
  observedResources.reserve(snapshot.resources.size());
  for (const ResidentPresentationResource &resource : snapshot.resources) {
    if (resource.individuallyValidated)
      observedResources.push_back(residentResourceKey(resource));
  }
  std::sort(observedResources.begin(), observedResources.end());
  journal.sceneCache.erase(
      std::remove_if(journal.sceneCache.begin(), journal.sceneCache.end(),
                     [&](CachedResidentResource &resource) {
                       if (resource.adjacentOutgoing)
                         return false;
                       const bool observed = std::binary_search(
                           observedResources.begin(), observedResources.end(),
                           residentResourceKey(resource.descriptor));
                       if (observed) {
                         resource.lastObservedCaptureId = snapshot.captureId;
                         resource.absenceObservations = 0u;
                         return false;
                       }
                       resource.absenceObservations =
                           resource.lastObservedCaptureId + 1u ==
                                   snapshot.captureId
                               ? resource.absenceObservations + 1u
                               : 1u;
                       resource.lastObservedCaptureId = snapshot.captureId;
                       if (resource.absenceObservations < 2u)
                         return false;
                       ++snapshot.individuallyRevokedResources;
                       return true;
                     }),
      journal.sceneCache.end());
}

uint32_t sharedResidentResourceCount(
    const std::vector<ResidentResourceKey> &left,
    const std::vector<ResidentResourceKey> &right) noexcept {
  uint32_t shared = 0u;
  std::size_t leftIndex = 0u;
  std::size_t rightIndex = 0u;
  while (leftIndex < left.size() && rightIndex < right.size()) {
    if (left[leftIndex] < right[rightIndex]) {
      ++leftIndex;
    } else if (right[rightIndex] < left[leftIndex]) {
      ++rightIndex;
    } else {
      ++shared;
      ++leftIndex;
      ++rightIndex;
    }
  }
  return shared;
}

void observeResidentCohort(ResidentPresentationSnapshot &snapshot,
                           std::vector<CachedResidentResource> currentCache) {
  ResidentCohortJournal &journal = g_residentCohortJournal;
  const auto recycleCurrentCache = [&] {
    g_residentCurrentCacheScratch = std::move(currentCache);
  };
  snapshot.activeCohortFingerprint = journal.activeFingerprint;
  snapshot.activeCohortGeneration = journal.generation;

  const std::vector<ResidentResourceKey> currentResources =
      residentResourceKeys(snapshot.resources);
  if (snapshot.resourceSetValidated) {
    for (const ResidentResourceKey &resource : currentResources) {
      const auto known = journal.geometryByResourceId.find(resource.resourceId);
      if (known != journal.geometryByResourceId.end() &&
          known->second != resource.geometryFingerprint) {
        // An EID identifies immutable resource contents. Seeing the same EID
        // with different topology means the paging target is being rewritten
        // (or is corrupt), never a new section identity to accept.
        snapshot.resourceIdentityConflict = true;
        snapshot.resourceSetValidated = false;
        break;
      }
    }
  }
  if (!snapshot.resourceSetValidated) {
    journal.candidateFingerprint = 0u;
    journal.candidateObservations = 0u;
    journal.candidateResources.clear();
    recycleCurrentCache();
    return;
  }
  snapshot.sharedActiveResourceCount =
      sharedResidentResourceCount(currentResources, journal.activeResources);
  if (!journal.activeResources.empty() &&
      currentResources == journal.activeResources) {
    journal.activeCache.swap(currentCache);
    recycleCurrentCache();
    journal.candidateFingerprint = 0u;
    journal.candidateObservations = 0u;
    journal.candidateResources.clear();
    snapshot.activeCohortFingerprint = journal.activeFingerprint;
    snapshot.activeCohortGeneration = journal.generation;
    return;
  }
  snapshot.cohortCandidate = true;
  snapshot.cohortDisjoint = !journal.activeResources.empty() &&
                            snapshot.sharedActiveResourceCount == 0u;
  // Compare the complete sorted key set, not just its diagnostic hash. This
  // makes debounce correctness independent of a theoretical hash collision
  // and of packed-world ordering in the transient table.
  if (currentResources != journal.candidateResources) {
    journal.candidateResources = currentResources;
    journal.candidateFingerprint = snapshot.resourceSetFingerprint;
    journal.candidateObservations = 1u;
  } else {
    ++journal.candidateObservations;
  }
  snapshot.candidateObservations = journal.candidateObservations;
  // Two complete, consecutive construction captures reject a partially
  // rewritten table without baking in Crash's cadence or any section ID.
  if (journal.candidateObservations < 2u) {
    recycleCurrentCache();
    return;
  }

  if (journal.activeResources.empty() || snapshot.cohortDisjoint) {
    // A disjoint set is a scene boundary, not a widening overlap. Keeping the
    // previous level here would create ghosts after a teleport/load.
    journal.sceneCache.clear();
    journal.pendingResources.clear();
  } else {
    // A completed handoff replaces the previous overlap. Keeping resources
    // across multiple completed handoffs exposed geometry which crossed the
    // camera from a sector the guest had deliberately retired. Completeness
    // comes from admitting coherent incoming rows during the rewrite below,
    // not from accumulating an unbounded level history.
    journal.sceneCache.clear();
    for (CachedResidentResource &resource : journal.activeCache) {
      const ResidentResourceKey key = residentResourceKey(resource.descriptor);
      if (std::binary_search(currentResources.begin(), currentResources.end(),
                             key))
        continue;
      resource.adjacentOutgoing = true;
      resource.lastObservedCaptureId = snapshot.captureId;
      resource.absenceObservations = 0u;
      upsertResidentSceneResource(journal.sceneCache, std::move(resource));
    }
  }
  journal.sceneCache.erase(
      std::remove_if(journal.sceneCache.begin(), journal.sceneCache.end(),
                     [&](const auto &resource) {
                       return std::binary_search(
                           currentResources.begin(), currentResources.end(),
                           residentResourceKey(resource.descriptor));
                     }),
      journal.sceneCache.end());
  journal.activeResources = currentResources;
  journal.activeCache.swap(currentCache);
  recycleCurrentCache();
  journal.activeFingerprint = snapshot.resourceSetFingerprint;
  journal.candidateFingerprint = 0u;
  journal.candidateObservations = 0u;
  journal.candidateResources.clear();
  for (const ResidentResourceKey &resource : journal.activeResources)
    journal.geometryByResourceId[resource.resourceId] =
        resource.geometryFingerprint;
  ++journal.generation;
  snapshot.cohortCommitted = true;
  snapshot.activeCohortFingerprint = journal.activeFingerprint;
  snapshot.activeCohortGeneration = journal.generation;
}

struct SceneryCameraVertex {
  int64_t x = 0;
  int64_t y = 0;
  int64_t z = 0;
  std::array<int16_t, 3> model{};
  bool colorDecoded = false;
  std::array<float, 3> color255{};
};

struct SceneryNearClipVertex {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  // Weights relative to the original triangle. Keeping them through
  // Sutherland-Hodgman clipping lets the presentation sidecar reconstruct
  // every guest-authored per-vertex attribute without consulting the GTE's
  // saturated screen result.
  std::array<double, 3> barycentric{};
};

struct SceneryNearClipPolygon {
  std::array<SceneryNearClipVertex, 4> vertices{};
  uint8_t count = 0;
};

SceneryNearClipPolygon clipSceneryTriangleToNearPlane(
    const std::array<SceneryCameraVertex, 3> &triangle, double nearZ) noexcept {
  SceneryNearClipPolygon output;
  const auto convert = [](const SceneryCameraVertex &vertex,
                          std::size_t corner) {
    SceneryNearClipVertex converted;
    converted.x = static_cast<double>(vertex.x);
    converted.y = static_cast<double>(vertex.y);
    converted.z = static_cast<double>(vertex.z);
    converted.barycentric[corner] = 1.0;
    return converted;
  };
  const auto append = [&](const SceneryNearClipVertex &vertex) {
    if (output.count < output.vertices.size())
      output.vertices[output.count++] = vertex;
  };
  const auto intersection = [&](const SceneryNearClipVertex &from,
                                const SceneryNearClipVertex &to) {
    const double denominator = to.z - from.z;
    const double t = denominator != 0.0 ? (nearZ - from.z) / denominator : 0.0;
    SceneryNearClipVertex clipped;
    clipped.x = from.x + (to.x - from.x) * t;
    clipped.y = from.y + (to.y - from.y) * t;
    clipped.z = nearZ;
    for (std::size_t corner = 0; corner < clipped.barycentric.size();
         ++corner) {
      clipped.barycentric[corner] =
          from.barycentric[corner] +
          (to.barycentric[corner] - from.barycentric[corner]) * t;
    }
    return clipped;
  };

  SceneryNearClipVertex previous = convert(triangle.back(), 2u);
  bool previousInside = previous.z >= nearZ;
  for (std::size_t corner = 0; corner < triangle.size(); ++corner) {
    const SceneryNearClipVertex current = convert(triangle[corner], corner);
    const bool currentInside = current.z >= nearZ;
    if (currentInside != previousInside)
      append(intersection(previous, current));
    if (currentInside)
      append(current);
    previous = current;
    previousInside = currentInside;
  }
  return output;
}

struct SceneryGeometryStats {
  uint32_t sourceUnionEntries = 0;
  uint32_t testedFaces = 0;
  uint32_t retainedFaces = 0;
  uint32_t rejectedFaces = 0;
  uint32_t retainedEntries = 0;
  uint32_t failure = 0;
  uint32_t failureWorld = 0;
  uint32_t expected = 0;
  uint32_t observed = 0;
  uint32_t originalPositive = 0;
  uint32_t originalNegative = 0;
  uint32_t originalDegenerate = 0;
  uint32_t addedPositive = 0;
  uint32_t addedNegative = 0;
  uint32_t addedDegenerate = 0;
  uint32_t retainedPositive = 0;
  uint32_t retainedNegative = 0;
  uint32_t residentQuadHalvesTested = 0;
  uint32_t residentQuadHalvesRetained = 0;
  uint32_t residentQuadHalvesRejected = 0;
  uint32_t residentTrianglesTested = 0;
  uint32_t residentTrianglesRetained = 0;
  uint32_t residentTrianglesRejected = 0;
  uint32_t residentQuadsTested = 0;
  uint32_t residentQuadsRetained = 0;
  uint32_t residentQuadsRejected = 0;
  uint32_t residentAbsentQuadHalvesTested = 0;
  uint32_t residentAbsentQuadHalvesRetained = 0;
  uint32_t residentAbsentQuadHalvesRejected = 0;
  uint32_t residentEntriesAdded = 0;
  uint32_t transformedVertices = 0;
  uint32_t deformedVertices = 0;
  uint32_t dynamicMaterialMissingLodShift = 0;
  uint32_t dynamicMaterialDepthUnavailable = 0;
  uint32_t dynamicMaterialLodIndexOutOfRange = 0;
  uint32_t dynamicMaterialScratchTailReads = 0;
  uint32_t dynamicMaterialMaximumLodIndex = 0;
  uint32_t dynamicMaterialMissingRemap = 0;
  uint32_t dynamicMaterialDescriptorUnavailable = 0;
  uint32_t vertexCacheHits = 0;
  uint32_t residentSeedFaces = 0;
  uint32_t residentClosureFaces = 0;
  uint32_t residentCandidateComponents = 0;
  uint32_t residentRetainedComponents = 0;
  uint32_t residentFrustumFaces = 0;
  uint32_t residentGuardBandFaces = 0;
  uint32_t residentAllFrustum = 0;
  uint32_t cullGuardPixels = 0;
  uint32_t topologyFaces = 0;
  uint32_t topologySharedEdges = 0;
  uint32_t topologyRebuilt = 0;
};

struct SceneryCameraTransform {
  std::array<std::array<int16_t, 3>, 3> rotation{};
  std::array<int32_t, 3> translation{};
  int64_t projectionPlane = 0;
};

struct SceneryCameraWorkspace {
  struct VertexDeformation {
    bool active = false;
    bool distanceCued = false;
    uint32_t trianglePostprocessor = 0;
    uint32_t quadPostprocessor = 0;
    uint32_t time = 0;
    uint32_t phaseShift = 0;
    uint32_t vectorShift = 0;
    uint32_t bias = 0;
  } deformation;
  SceneryCameraTransform transform{};
  std::array<int32_t, 3> farColor{};
  SceneryConstructionState construction{};
  std::array<std::vector<SceneryCameraVertex>, kMaxSceneryWorlds> vertices;
  std::array<std::vector<uint8_t>, kMaxSceneryWorlds> vertexState;
};

struct ResidentSceneryFace {
  uint16_t packed = 0;
  uint8_t world = 0;
  uint8_t half = 0;
  std::array<uint32_t, 3> indices{};
  // Canonical {a,b,c,d} for a quad. Keeping this beside the immutable
  // topology avoids decoding the same two descriptor words twice per half on
  // every presentation capture. Triangles retain their three indices here.
  std::array<uint32_t, 4> completeIndices{};
  // The packed topology words also carry the material token. They are
  // immutable for the lifetime of this topology cache, so retain the already
  // decoded source words instead of rereading guest RAM for every frame.
  uint32_t descriptorA = 0u;
  uint32_t descriptorB = 0u;
};

struct ResidentSceneryTopologyWorld {
  bool valid = false;
  uint32_t trianglePivot = 0;
  uint32_t quadStart = 0;
  uint32_t vertexPivot = 0;
  uint32_t vertexCount = 0;
  uint32_t triangleCount = 0;
  uint32_t quadCount = 0;
};

struct ResidentSceneryTopologyCache {
  bool valid = false;
  uint32_t worldCount = 0;
  std::array<ResidentSceneryTopologyWorld, kMaxSceneryWorlds> worlds{};
  std::vector<ResidentSceneryFace> faces;
  std::vector<std::array<uint32_t, 2>> sharedEdges;
};

ResidentSceneryTopologyCache g_residentSceneryTopology{};

bool prepareResidentGpuSceneCapture(
    const Memory &memory, const SceneryCameraWorkspace &workspace,
    const std::array<ResidentSceneryGeometry, kMaxSceneryWorlds> &geometry,
    const std::array<uint8_t, kMaxSceneryWorlds> &geometryValidity,
    uint32_t worldCount, std::vector<CachedResidentResource> &resources,
    std::vector<ResidentGpuPresentationFace> presentationFaces,
    bool staticOpaquePlanActive, ResidentPresentationSnapshot &snapshot) {
  // The raw GPU program currently implements projection/waves, not this
  // primitive-dependent lighting constructor. Keep its correctly shaded
  // precise vertices on the projected Vulkan lane; never reuse raw base RGB.
  if (workspace.construction.twoPointLighting)
    return false;
  ResidentGpuSceneCapture scene;
  scene.captureId = snapshot.captureId;
  scene.topologyFingerprint = snapshot.topologyFingerprint;
  scene.resourceSetFingerprint = snapshot.resourceSetFingerprint;
  scene.rotation = workspace.transform.rotation;
  scene.translation = workspace.transform.translation;
  scene.projectionPlane = workspace.transform.projectionPlane;
  scene.farColor = workspace.farColor;
  scene.deformation.active = workspace.deformation.active;
  scene.deformation.distanceCued = workspace.deformation.distanceCued;
  scene.deformation.trianglePostprocessor =
      workspace.deformation.trianglePostprocessor;
  scene.deformation.quadPostprocessor = workspace.deformation.quadPostprocessor;
  scene.deformation.time = workspace.deformation.time;
  scene.deformation.phaseShift = workspace.deformation.phaseShift;
  scene.deformation.vectorShift = workspace.deformation.vectorShift;
  scene.deformation.bias = workspace.deformation.bias;
  if (scene.deformation.active) {
    constexpr uint32_t kWaveTableBase = UINT32_C(0x8005CA80);
    constexpr uint32_t kWaveSampleStride = 128u;
    constexpr uint32_t kWaveSampleCount = 17u;
    static_assert(kWaveSampleCount ==
                  std::tuple_size_v<decltype(scene.deformation.waveSamples)>);
    if (!isRamRange(kWaveTableBase,
                    (kWaveSampleCount - 1u) * kWaveSampleStride +
                        sizeof(uint16_t))) {
      return false;
    }
    for (uint32_t sample = 0u; sample < kWaveSampleCount; ++sample) {
      scene.deformation.waveSamples[sample] = static_cast<int16_t>(
          memory.read16(kWaveTableBase + sample * kWaveSampleStride));
    }
  }

  scene.resources.reserve(worldCount);
  bool complete =
      snapshot.resourceSetValidated && resources.size() >= worldCount;
  for (uint32_t world = 0u; world < worldCount; ++world) {
    if (geometryValidity[world] == 0u || world >= resources.size()) {
      complete = false;
      continue;
    }
    CachedResidentResource &resource = resources[world];
    const auto previousPayload = resource.gpuPayload;
    if (!ensureResidentGpuResourcePayload(resource) ||
        resource.gpuPayload == nullptr) {
      complete = false;
      continue;
    }
    if (previousPayload != nullptr && previousPayload == resource.gpuPayload)
      ++scene.reusedPayloadCount;
    else
      ++scene.rebuiltPayloadCount;
    scene.rawVertexCount +=
        static_cast<uint32_t>(resource.gpuPayload->vertices.size());
    scene.rawFaceCount +=
        static_cast<uint32_t>(resource.gpuPayload->faces.size());
    scene.resources.push_back(
        {world, resource.descriptor.flags, geometry[world].translationXy,
         geometry[world].translationZ, resource.gpuPayload});
  }
  complete &= scene.resources.size() == worldCount;
  scene.staticOpaquePlanActive = staticOpaquePlanActive;
  if (staticOpaquePlanActive) {
    uint64_t staticOpaqueFaceCount = 0u;
    for (const ResidentGpuResourceInstance &instance : scene.resources) {
      // Camera-anchored resources need one lightweight CPU projection for the
      // modern backdrop underlay. Keep their immutable payload device-local,
      // but leave their faces in the per-frame presentation plan instead of
      // consuming them through the projection-free static suffix.
      if (instance.flags == 1u)
        continue;
      if (instance.payload == nullptr ||
          instance.payload->staticOpaqueFaces.size() >
              std::numeric_limits<uint32_t>::max() - staticOpaqueFaceCount) {
        complete = false;
        break;
      }
      staticOpaqueFaceCount += instance.payload->staticOpaqueFaces.size();
    }
    scene.staticOpaqueFaceCount = static_cast<uint32_t>(staticOpaqueFaceCount);
  }
  const uint64_t plannedFaceCount =
      static_cast<uint64_t>(presentationFaces.size()) +
      scene.staticOpaqueFaceCount;
  complete &= plannedFaceCount == scene.rawFaceCount &&
              std::all_of(presentationFaces.begin(), presentationFaces.end(),
                          [](const ResidentGpuPresentationFace &face) {
                            return face.material.decoded && face.colorDecoded &&
                                   (face.orderingTableBucketValid ||
                                    (face.gpuRawOnly &&
                                     !face.material.semiTransparent));
                          });
  scene.gpuRawOnlyFaceCount =
      scene.staticOpaqueFaceCount +
      static_cast<uint32_t>(
          std::count_if(presentationFaces.begin(), presentationFaces.end(),
                        [](const ResidentGpuPresentationFace &face) {
                          return face.gpuRawOnly;
                        }));
  scene.presentationFaces = std::move(presentationFaces);
  scene.completeForPresentation = complete;
  snapshot.gpuScene = std::move(scene);
  return true;
}

bool isBackwardRamRange(uint32_t pivot, uint32_t byteCount) noexcept {
  const uint32_t physical = Memory::toPhysical(pivot);
  return physical >= byteCount && isRamRange(pivot - byteCount, byteCount);
}

bool readResidentSceneryGeometryWorld(const Memory &memory,
                                      const ResidentSceneryWorld &resident,
                                      uint32_t world,
                                      ResidentSceneryGeometry &geometry,
                                      SceneryGeometryStats &stats) {
  if (!isRamRange(resident.recordAddress, kResidentSceneryRecordStride) ||
      !isRamRange(resident.infoAddress,
                  kSceneryInfoQuadCountOffset + sizeof(uint32_t))) {
    stats.failure = 1u;
    stats.failureWorld = world;
    return false;
  }

  const uint32_t vertexCount =
      memory.read32(resident.infoAddress + kSceneryInfoVertexCountOffset);
  if (vertexCount == 0u || vertexCount > kMaxSceneryVertexCount) {
    stats.failure = 2u;
    stats.failureWorld = world;
    stats.observed = vertexCount;
    return false;
  }

  const uint32_t translationX = memory.read32(
      resident.recordAddress + kResidentSceneryTranslationXOffset);
  const uint32_t translationY = memory.read32(
      resident.recordAddress + kResidentSceneryTranslationYOffset);
  const uint32_t translationXy =
      (translationX & 0xffffu) | (translationY << 16u);
  const int32_t translationZ = static_cast<int32_t>(memory.read32(
      resident.recordAddress + kResidentSceneryTranslationZOffset));

  const uint32_t scratch = kRendererScratchBase + kSceneryScratchRecordOffset +
                           world * kSceneryScratchRecordStride;
  const uint32_t scratchTranslationXy =
      memory.read32(scratch + kSceneryScratchTranslationXyOffset);
  const int32_t scratchTranslationZ = static_cast<int32_t>(
      memory.read32(scratch + kSceneryScratchTranslationZOffset));
  const int16_t expectedX = static_cast<int16_t>(translationXy);
  const int16_t expectedY = static_cast<int16_t>(translationXy >> 16u);
  const int16_t scratchX = static_cast<int16_t>(scratchTranslationXy);
  const int16_t scratchY = static_cast<int16_t>(scratchTranslationXy >> 16u);
  // Crash adds X/Y as one packed word. Its setup path subtracts one from Y
  // when it must cancel the carry generated by the low X half. Accept only
  // that exact representation and then consume the same packed translation
  // as func_8004103C; broader disagreement still fails closed.
  const int32_t yCarryCompensation =
      static_cast<int32_t>(expectedY) - static_cast<int32_t>(scratchY);
  if (scratchX != expectedX ||
      (yCarryCompensation != 0 && yCarryCompensation != 1)) {
    stats.failure = 3u;
    stats.failureWorld = world;
    stats.expected = translationXy;
    stats.observed = scratchTranslationXy;
    return false;
  }
  if (scratchTranslationZ != translationZ) {
    stats.failure = 4u;
    stats.failureWorld = world;
    stats.expected = static_cast<uint32_t>(translationZ);
    stats.observed = static_cast<uint32_t>(scratchTranslationZ);
    return false;
  }

  ResidentSceneryGeometry decoded;
  decoded.translationXy = scratchTranslationXy;
  decoded.translationZ = translationZ;
  decoded.trianglePivot =
      memory.read32(scratch + kSceneryScratchTrianglePivotOffset);
  decoded.quadStart = memory.read32(scratch + kSceneryScratchQuadStartOffset);
  decoded.vertexPivot =
      memory.read32(scratch + kSceneryScratchVertexPivotOffset);
  decoded.colorTable = memory.read32(scratch + kSceneryScratchColorTableOffset);
  decoded.materialRemap =
      memory.read32(scratch + kSceneryScratchMaterialRemapOffset);
  decoded.materialTable =
      memory.read32(scratch + kSceneryScratchMaterialTableOffset);
  decoded.vertexCount = vertexCount;

  const uint32_t triangleWords = resident.triangleCount * 4u;
  const uint32_t triangleTails = resident.triangleCount * 2u;
  const uint32_t quadBytes = resident.quadCount * 8u;
  const uint32_t vertexWords = vertexCount * 4u;
  const uint32_t vertexTails = vertexCount * 2u;
  if (!isBackwardRamRange(decoded.trianglePivot, triangleWords) ||
      !isRamRange(decoded.trianglePivot, triangleTails) ||
      !isRamRange(decoded.quadStart, quadBytes) ||
      !isBackwardRamRange(decoded.vertexPivot, vertexWords) ||
      !isRamRange(decoded.vertexPivot, vertexTails) ||
      !isRamRange(decoded.colorTable, 0x1000u)) {
    stats.failure = 5u;
    stats.failureWorld = world;
    stats.expected = decoded.vertexCount;
    stats.observed = decoded.vertexPivot;
    return false;
  }
  geometry = decoded;
  return true;
}

bool readResidentSceneryGeometry(
    const Memory &memory,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    uint32_t worldCount,
    std::array<ResidentSceneryGeometry, kMaxSceneryWorlds> &geometry,
    SceneryGeometryStats &stats) {
  for (uint32_t world = 0; world < worldCount; ++world) {
    if (!readResidentSceneryGeometryWorld(memory, worlds[world], world,
                                          geometry[world], stats)) {
      return false;
    }
  }
  return true;
}

uint32_t readResidentSceneryGeometryPartial(
    const Memory &memory,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    uint32_t worldCount,
    std::array<ResidentSceneryGeometry, kMaxSceneryWorlds> &geometry,
    std::array<uint8_t, kMaxSceneryWorlds> &validity,
    SceneryGeometryStats &stats) {
  validity.fill(0u);
  uint32_t validWorlds = 0u;
  for (uint32_t world = 0u; world < worldCount; ++world) {
    SceneryGeometryStats localStats;
    if (readResidentSceneryGeometryWorld(memory, worlds[world], world,
                                         geometry[world], localStats)) {
      validity[world] = 1u;
      ++validWorlds;
      continue;
    }
    // Preserve one representative failure for diagnostics while continuing
    // to decode every independent row. No invalid geometry is consumed.
    if (stats.failure == 0u)
      stats = localStats;
  }
  return validWorlds;
}

bool decodeSceneryVertexColor(const Memory &memory,
                              const ResidentSceneryGeometry &geometry,
                              uint32_t index,
                              std::array<float, 3> &color255) noexcept {
  if (index >= geometry.vertexCount)
    return false;
  const uint32_t xyAddress = geometry.vertexPivot - (index + 1u) * 4u;
  const uint32_t zAddress = geometry.vertexPivot + index * 2u;
  if (!isRamRange(xyAddress, sizeof(uint32_t)) ||
      !isRamRange(zAddress, sizeof(uint16_t))) {
    return false;
  }

  const uint32_t rawXy = memory.read32(xyAddress);
  const uint32_t rawZ = memory.read16(zAddress);
  // Exact reconstruction of the index assembled at 0x80041300-0x8004136C
  // and 0x8004187C-0x800418A4 in func_8004103C. Position decoding masks the
  // same low nibbles, leaving this lighting identity independent of geometry.
  const uint32_t byteOffset = ((rawZ << 2u) & UINT32_C(0x003c)) |
                              ((rawXy >> 6u) & UINT32_C(0x0c00)) |
                              ((rawXy << 6u) & UINT32_C(0x03c0));
  const uint32_t address = geometry.colorTable + byteOffset;
  if (!isRamRange(address, sizeof(uint32_t)))
    return false;
  const uint32_t packed = memory.read32(address);
  color255 = {{static_cast<float>(packed & 0xffu),
               static_cast<float>((packed >> 8u) & 0xffu),
               static_cast<float>((packed >> 16u) & 0xffu)}};
  return true;
}

void prepareSceneryCameraWorkspace(
    const CPUContext &ctx,
    const std::array<ResidentSceneryGeometry, kMaxSceneryWorlds> &geometry,
    uint32_t worldCount, SceneryCameraWorkspace &workspace) {
  workspace.deformation = {};
  const auto low = [](uint32_t value) {
    return static_cast<int16_t>(value & 0xffffu);
  };
  const auto high = [](uint32_t value) {
    return static_cast<int16_t>(value >> 16u);
  };
  workspace.transform.rotation = {{
      {{low(ctx.cop2c[GTE_RT11RT12]), high(ctx.cop2c[GTE_RT11RT12]),
        low(ctx.cop2c[GTE_RT13RT21])}},
      {{high(ctx.cop2c[GTE_RT13RT21]), low(ctx.cop2c[GTE_RT22RT23]),
        high(ctx.cop2c[GTE_RT22RT23])}},
      {{low(ctx.cop2c[GTE_RT31RT32]), high(ctx.cop2c[GTE_RT31RT32]),
        low(ctx.cop2c[GTE_RT33])}},
  }};
  workspace.transform.translation = {{
      static_cast<int32_t>(ctx.cop2c[GTE_TRX]),
      static_cast<int32_t>(ctx.cop2c[GTE_TRY]),
      static_cast<int32_t>(ctx.cop2c[GTE_TRZ]),
  }};
  workspace.transform.projectionPlane = static_cast<uint16_t>(ctx.cop2c[GTE_H]);
  workspace.farColor = {{static_cast<int32_t>(ctx.cop2c[GTE_RFC]),
                         static_cast<int32_t>(ctx.cop2c[GTE_GFC]),
                         static_cast<int32_t>(ctx.cop2c[GTE_BFC])}};
  for (uint32_t world = 0; world < worldCount; ++world) {
    workspace.vertices[world].resize(geometry[world].vertexCount);
    workspace.vertexState[world].assign(geometry[world].vertexCount, 0u);
  }
}

bool prepareResidentSceneryVertexDeformation(
    const Memory &memory, SceneryCameraWorkspace &workspace) noexcept {
  auto &construction = workspace.construction;
  construction.triangleProgram = memory.read32(kRendererScratchBase + 100u);
  construction.quadProgram = memory.read32(kRendererScratchBase + 104u);
  construction.twoPointLighting = sceneryUsesTwoPointLighting(
      construction.triangleProgram, construction.quadProgram);
  const char *constructorControl = std::getenv("PS1_CRASH3_SCENERY_CONSTRUCTION");
  if (constructorControl != nullptr &&
      std::strcmp(constructorControl, "legacy") == 0)
    construction.twoPointLighting = false;
  construction.farColor = workspace.farColor;
  if (construction.twoPointLighting) {
    for (uint32_t point = 0; point < construction.points.size(); ++point) {
      construction.points[point].packedXy =
          memory.read32(kRendererScratchBase + 124u + point * 12u);
      construction.points[point].z = static_cast<int32_t>(
          memory.read32(kRendererScratchBase + 128u + point * 12u));
    }
  }
  // func_8004103C selects these paired triangle/quad post-processors for its
  // waving resident-vertex paths. The first pair displaces raw-XY bit-18
  // vertices only. The second also feeds the same intrinsic wave sample to
  // DPCS unless raw-XY bit 19 disables it. Requiring either complete pair
  // prevents unrelated handler modes from being guessed.
  constexpr uint32_t kTriangleDeformationHandler = UINT32_C(0x80041DC8);
  constexpr uint32_t kQuadDeformationHandler = UINT32_C(0x80041E8C);
  constexpr uint32_t kTriangleDistanceCueHandler = UINT32_C(0x80042A34);
  constexpr uint32_t kQuadDistanceCueHandler = UINT32_C(0x80042B48);
  constexpr uint32_t kTrianglePostprocessorOffset = 168u;
  constexpr uint32_t kQuadPostprocessorOffset = 172u;
  constexpr uint32_t kDeformationTimeOffset = 28u;
  constexpr uint32_t kDeformationVectorShiftOffset = 96u;
  constexpr uint32_t kDeformationBiasOffset = 204u;
  constexpr uint32_t kDeformationPhaseShiftOffset = 240u;
  constexpr uint32_t kWaveTableBase = UINT32_C(0x8005CA80);
  constexpr uint32_t kWaveTableFirst = kWaveTableBase - 2048u;
  constexpr uint32_t kWaveTableLast = kWaveTableBase + 4096u;
  if (!isScratchpadRange(kRendererScratchBase + kDeformationTimeOffset,
                         sizeof(uint32_t)) ||
      !isScratchpadRange(kRendererScratchBase + kDeformationVectorShiftOffset,
                         sizeof(uint32_t)) ||
      !isScratchpadRange(kRendererScratchBase + kTrianglePostprocessorOffset,
                         2u * sizeof(uint32_t)) ||
      !isScratchpadRange(kRendererScratchBase + kDeformationBiasOffset,
                         sizeof(uint32_t)) ||
      !isScratchpadRange(kRendererScratchBase + kDeformationPhaseShiftOffset,
                         sizeof(uint32_t)) ||
      !isRamRange(kWaveTableFirst,
                  kWaveTableLast - kWaveTableFirst + sizeof(uint16_t))) {
    return false;
  }
  const uint32_t trianglePostprocessor =
      memory.read32(kRendererScratchBase + kTrianglePostprocessorOffset);
  const uint32_t quadPostprocessor =
      memory.read32(kRendererScratchBase + kQuadPostprocessorOffset);
  workspace.deformation.trianglePostprocessor = trianglePostprocessor;
  workspace.deformation.quadPostprocessor = quadPostprocessor;
  const bool geometryOnly =
      trianglePostprocessor == kTriangleDeformationHandler &&
      quadPostprocessor == kQuadDeformationHandler;
  const bool distanceCued =
      trianglePostprocessor == kTriangleDistanceCueHandler &&
      quadPostprocessor == kQuadDistanceCueHandler;
  if (!geometryOnly && !distanceCued) {
    return false;
  }
  workspace.deformation.active = true;
  workspace.deformation.distanceCued = distanceCued;
  workspace.deformation.time =
      memory.read32(kRendererScratchBase + kDeformationTimeOffset);
  workspace.deformation.vectorShift =
      memory.read32(kRendererScratchBase + kDeformationVectorShiftOffset);
  workspace.deformation.bias =
      memory.read32(kRendererScratchBase + kDeformationBiasOffset);
  workspace.deformation.phaseShift =
      memory.read32(kRendererScratchBase + kDeformationPhaseShiftOffset);
  return true;
}

std::optional<int16_t>
deformResidentSceneryVertex(const Memory &memory, uint32_t rawXy,
                            uint32_t &modelXy, int16_t &modelZ,
                            const SceneryCameraWorkspace &workspace,
                            SceneryGeometryStats &stats) noexcept {
  // The geometry-only pair executes the wave program for raw-XY bit 18.
  // The distance-cued pair executes it for every bit-18 vertex and for each
  // otherwise static vertex whose bit 19 is clear. It writes the displaced
  // position only for bit 18, while DPCS consumes the returned horizontal
  // sample only when bit 19 is clear.
  if (!workspace.deformation.active)
    return std::nullopt;
  const bool displace = (rawXy & (UINT32_C(1) << 18u)) != 0u;
  const bool skipDistanceCue = (rawXy & (UINT32_C(1) << 19u)) != 0u;
  if ((!workspace.deformation.distanceCued && !displace) ||
      (workspace.deformation.distanceCued && !displace && skipDistanceCue)) {
    return std::nullopt;
  }

  constexpr uint32_t kWaveTableBase = UINT32_C(0x8005CA80);
  const auto &deformation = workspace.deformation;
  uint32_t phase = static_cast<uint32_t>(static_cast<int32_t>(modelZ));
  phase += deformation.bias;
  phase += modelXy;
  phase = static_cast<uint32_t>(static_cast<int32_t>(phase) >> 2u);
  const bool reflectPhase = (phase & 0x20u) != 0u;
  phase += static_cast<uint32_t>(static_cast<int32_t>(deformation.time) >>
                                 (deformation.phaseShift & 31u));
  if (reflectPhase)
    phase = 0u - phase;

  const uint32_t tableOffset = (phase & 0x1fu) << 7u;
  const bool quadrantBit4 = (phase & 0x10u) != 0u;
  const bool quadrantBit5 = (phase & 0x20u) != 0u;
  const auto sample = [&](uint32_t address) {
    return static_cast<int16_t>(memory.read16(address));
  };
  const auto amplitude = [&](int16_t sampleValue) {
    const int32_t sample = sampleValue;
    const uint32_t shifted = static_cast<uint32_t>(sample) << 16u;
    return static_cast<int32_t>(shifted) >> (deformation.vectorShift & 31u);
  };

  int32_t z = modelZ;
  int16_t cue = 0;
  if (!quadrantBit4) {
    const int16_t zSample = sample(kWaveTableBase - tableOffset + 2048u);
    const int16_t xySample = sample(kWaveTableBase + tableOffset);
    cue = xySample;
    if (displace) {
      const int32_t zAmplitude = amplitude(zSample);
      const int32_t xyAmplitude = amplitude(xySample);
      if (!quadrantBit5) {
        z = static_cast<int32_t>(static_cast<uint32_t>(z) +
                                 static_cast<uint32_t>(zAmplitude));
        modelXy += static_cast<uint32_t>(xyAmplitude);
      } else {
        z = static_cast<int32_t>(static_cast<uint32_t>(z) -
                                 static_cast<uint32_t>(zAmplitude));
        modelXy -= static_cast<uint32_t>(xyAmplitude);
      }
    }
  } else {
    const int16_t xySample = sample(kWaveTableBase - tableOffset + 4096u);
    const int16_t zSample = sample(kWaveTableBase + tableOffset - 2048u);
    cue = xySample;
    if (displace) {
      const int32_t xyAmplitude = amplitude(xySample);
      const int32_t zAmplitude = amplitude(zSample);
      if (!quadrantBit5) {
        modelXy += static_cast<uint32_t>(xyAmplitude);
        z = static_cast<int32_t>(static_cast<uint32_t>(z) -
                                 static_cast<uint32_t>(zAmplitude));
      } else {
        modelXy -= static_cast<uint32_t>(xyAmplitude);
        z = static_cast<int32_t>(static_cast<uint32_t>(z) +
                                 static_cast<uint32_t>(zAmplitude));
      }
    }
  }
  if (displace) {
    modelZ = static_cast<int16_t>(z);
    ++stats.deformedVertices;
  }
  return workspace.deformation.distanceCued && !skipDistanceCue
             ? std::optional<int16_t>{cue}
             : std::nullopt;
}

void applyResidentSceneryDistanceCue(const SceneryCameraWorkspace &workspace,
                                     int16_t factor,
                                     std::array<float, 3> &color255) noexcept {
  color255 = gte_math::distanceCueColor(color255, workspace.farColor, factor);
}

bool readSceneryCameraVertex(const Memory &memory,
                             const ResidentSceneryGeometry &geometry,
                             uint32_t world, uint32_t index,
                             SceneryCameraWorkspace &workspace,
                             SceneryGeometryStats &stats,
                             SceneryCameraVertex &camera) noexcept {
  if (index >= geometry.vertexCount)
    return false;
  uint8_t &state = workspace.vertexState[world][index];
  if (state != 0u) {
    ++stats.vertexCacheHits;
    if (state != 1u)
      return false;
    camera = workspace.vertices[world][index];
    return true;
  }
  const uint32_t xyAddress = geometry.vertexPivot - (index + 1u) * 4u;
  const uint32_t zAddress = geometry.vertexPivot + index * 2u;
  if (!isRamRange(xyAddress, 4u) || !isRamRange(zAddress, 2u)) {
    state = 2u;
    return false;
  }

  const uint32_t rawXy = memory.read32(xyAddress);
  const uint32_t rawZ = memory.read16(zAddress);
  uint32_t modelXy =
      ((((rawXy & UINT32_C(0xfff0fff0)) >> 2u) + geometry.translationXy) &
       UINT32_C(0xfffeffff));
  int16_t modelZ = static_cast<int16_t>(((rawZ & UINT32_C(0xfff0)) >> 2u) +
                                        geometry.translationZ);
  const std::optional<int16_t> distanceCue = deformResidentSceneryVertex(
      memory, rawXy, modelXy, modelZ, workspace, stats);
  const int16_t modelX = static_cast<int16_t>(modelXy);
  const int16_t modelY = static_cast<int16_t>(modelXy >> 16u);

  const std::array<int16_t, 3> model{{modelX, modelY, modelZ}};
  std::array<int64_t, 3> transformed{};
  for (std::size_t row = 0; row < transformed.size(); ++row) {
    transformed[row] =
        static_cast<int64_t>(workspace.transform.translation[row]) * 4096;
    for (std::size_t column = 0; column < model.size(); ++column)
      transformed[row] +=
          static_cast<int64_t>(workspace.transform.rotation[row][column]) *
          model[column];
  }
  camera = {transformed[0], transformed[1], transformed[2], model};
  camera.colorDecoded =
      decodeSceneryVertexColor(memory, geometry, index, camera.color255);
  if (camera.colorDecoded && distanceCue.has_value())
    applyResidentSceneryDistanceCue(workspace, *distanceCue, camera.color255);
  workspace.vertices[world][index] = camera;
  state = 1u;
  ++stats.transformedVertices;
  return true;
}

SceneryCameraVertex
transformCachedSceneryVertex(const Memory &memory,
                             const CachedResidentVertex &vertex,
                             uint32_t translationXy, int32_t translationZ,
                             const SceneryCameraWorkspace &workspace,
                             SceneryGeometryStats &stats) noexcept {
  const uint32_t rawXy = vertex.rawXy;
  uint32_t modelXy = ((((rawXy & UINT32_C(0xfff0fff0)) >> 2u) + translationXy) &
                      UINT32_C(0xfffeffff));
  int16_t modelZ = static_cast<int16_t>(
      ((vertex.rawZ & UINT32_C(0xfff0)) >> 2u) + translationZ);
  const std::optional<int16_t> distanceCue = deformResidentSceneryVertex(
      memory, rawXy, modelXy, modelZ, workspace, stats);
  const int16_t modelX = static_cast<int16_t>(modelXy);
  const int16_t modelY = static_cast<int16_t>(modelXy >> 16u);

  const std::array<int16_t, 3> model{{modelX, modelY, modelZ}};
  std::array<int64_t, 3> transformed{};
  for (std::size_t row = 0; row < transformed.size(); ++row) {
    transformed[row] =
        static_cast<int64_t>(workspace.transform.translation[row]) * 4096;
    for (std::size_t column = 0; column < model.size(); ++column)
      transformed[row] +=
          static_cast<int64_t>(workspace.transform.rotation[row][column]) *
          model[column];
  }
  SceneryCameraVertex camera{transformed[0], transformed[1], transformed[2],
                             model};
  camera.colorDecoded = vertex.colorDecoded;
  camera.color255 = vertex.color255;
  if (camera.colorDecoded && distanceCue.has_value())
    applyResidentSceneryDistanceCue(workspace, *distanceCue, camera.color255);
  ++stats.transformedVertices;
  return camera;
}

bool readCurrentCachedSceneryCameraVertex(
    const Memory &memory, const ResidentSceneryGeometry &geometry,
    uint32_t world, uint32_t index, const CachedResidentResource &resource,
    SceneryCameraWorkspace &workspace, SceneryGeometryStats &stats,
    SceneryCameraVertex &camera) noexcept {
  if (world >= kMaxSceneryWorlds || index >= geometry.vertexCount ||
      index >= resource.vertices.size()) {
    return false;
  }
  uint8_t &state = workspace.vertexState[world][index];
  if (state != 0u) {
    ++stats.vertexCacheHits;
    if (state != 1u)
      return false;
    camera = workspace.vertices[world][index];
    return true;
  }

  // currentResourceCache already owns the exact raw vertex and decoded base
  // colour needed for transition retention. Transform that copy instead of
  // rereading guest RAM and decoding the colour table a second time. Dynamic
  // deformation and distance cueing are still evaluated for this capture.
  camera = transformCachedSceneryVertex(
      memory, resource.vertices[index], geometry.translationXy,
      geometry.translationZ, workspace, stats);
  workspace.vertices[world][index] = camera;
  state = 1u;
  return true;
}

void constructSceneryLighting(
    const SceneryConstructionState &state,
    const CachedResidentResource &resource,
    const std::array<uint32_t, 3> &indices,
    const std::array<uint32_t, 4> &completeIndices,
    const std::array<SceneryCameraVertex, 3> &triangle,
    const std::array<SceneryCameraVertex, 4> &quad,
    uint32_t depthMode, ResidentPresentationFace &face) {
  if (!state.twoPointLighting)
    return;
  std::array<SceneryConstructionVertex, 4> native{};
  uint32_t count = 3u;
  if (face.half == 0u) {
    for (uint32_t corner = 0; corner < count; ++corner)
      native[corner] = {resource.vertices[indices[corner]].rawXy,
                         triangle[corner].model, triangle[corner].color255};
  } else {
    // Actual GTE load order is {b,a,c,d}, not the catalogue's {a,b,c,d}.
    const std::array<uint32_t, 4> order =
        depthMode == 2u ? std::array<uint32_t, 4>{0, 2, 3, 0}
                        : std::array<uint32_t, 4>{1, 0, 2, 3};
    count = depthMode == 3u ? 4u : 3u;
    for (uint32_t corner = 0; corner < count; ++corner) {
      const uint32_t source = order[corner];
      native[corner] = {resource.vertices[completeIndices[source]].rawXy,
                         quad[source].model, quad[source].color255};
    }
  }
  const auto selection = selectSceneryPrimitiveLighting(
      state, std::span<const SceneryConstructionVertex>{native.data(), count});
  for (uint32_t corner = 0; corner < 3u; ++corner)
    face.color255[corner] = shadeSceneryConstructionVertex(
        state, selection,
        {resource.vertices[indices[corner]].rawXy, triangle[corner].model,
         triangle[corner].color255});
  if (selection.flatBlack) {
    face.material.textured = false;
    face.material.semiTransparent = false;
    face.material.dynamicTexture = false;
    face.material.modernUvOffsetU = face.material.modernUvOffsetV = 0u;
    metrics::count("host.crash3.resident_sidecar.constructor_flat_black_faces");
  }
  metrics::count("host.crash3.resident_sidecar.constructor_lit_faces");
}

uint32_t
sceneryAverageDepthFromCameraVertices(const SceneryCameraVertex *vertices,
                                      uint32_t vertexCount) noexcept {
  uint32_t szSum = 0u;
  for (uint32_t corner = 0u; corner < vertexCount; ++corner) {
    const int32_t mac3 = static_cast<int32_t>(vertices[corner].z >> 12u);
    const uint32_t sz = mac3 <= 0
                            ? 0u
                            : static_cast<uint32_t>(std::min<int32_t>(
                                  mac3, static_cast<int32_t>(UINT16_MAX)));
    szSum += sz;
  }
  const uint32_t zScale = vertexCount == 3u ? 1365u : 1024u;
  return (zScale * szSum) >> 15u;
}

std::optional<uint32_t> sceneryFaceAverageDepthFromCameraVertices(
    const std::array<SceneryCameraVertex, 3> &triangle,
    const std::array<SceneryCameraVertex, 4> &completeQuad, uint32_t quadHalf,
    uint32_t depthMode) noexcept {
  if (quadHalf == 0u)
    return sceneryAverageDepthFromCameraVertices(triangle.data(), 3u);
  if (depthMode == 3u)
    return sceneryAverageDepthFromCameraVertices(completeQuad.data(), 4u);

  std::array<SceneryCameraVertex, 3> selected{};
  if (depthMode == 1u) {
    selected = {completeQuad[1], completeQuad[0], completeQuad[2]};
  } else if (depthMode == 2u) {
    selected = {completeQuad[0], completeQuad[2], completeQuad[3]};
  } else {
    return std::nullopt;
  }
  return sceneryAverageDepthFromCameraVertices(selected.data(), 3u);
}

bool sceneryFaceIntersectsPresentationFrustum(
    const Memory &memory, const ResidentSceneryGeometry &geometry,
    uint32_t world, SceneryCameraWorkspace &workspace,
    SceneryGeometryStats &stats, const std::array<uint32_t, 3> &indices,
    bool &valid, int32_t *orientation = nullptr,
    bool *touchesNativeHorizontalBoundary = nullptr,
    bool *insideHorizontalGuardOnly = nullptr,
    std::optional<uint32_t> guardOverride = std::nullopt) noexcept {
  if (insideHorizontalGuardOnly != nullptr)
    *insideHorizontalGuardOnly = false;
  std::array<SceneryCameraVertex, 3> vertices{};
  for (std::size_t corner = 0; corner < vertices.size(); ++corner) {
    if (!readSceneryCameraVertex(memory, geometry, world, indices[corner],
                                 workspace, stats, vertices[corner])) {
      valid = false;
      return false;
    }
  }

  if (orientation != nullptr) {
    const __int128 determinant =
        static_cast<__int128>(vertices[0].x) *
            (static_cast<__int128>(vertices[1].y) * vertices[2].z -
             static_cast<__int128>(vertices[1].z) * vertices[2].y) -
        static_cast<__int128>(vertices[0].y) *
            (static_cast<__int128>(vertices[1].x) * vertices[2].z -
             static_cast<__int128>(vertices[1].z) * vertices[2].x) +
        static_cast<__int128>(vertices[0].z) *
            (static_cast<__int128>(vertices[1].x) * vertices[2].y -
             static_cast<__int128>(vertices[1].y) * vertices[2].x);
    *orientation = determinant > 0 ? 1 : determinant < 0 ? -1 : 0;
  }

  const bool allBehind = std::all_of(
      vertices.begin(), vertices.end(),
      [](const SceneryCameraVertex &vertex) { return vertex.z <= 0; });
  if (allBehind)
    return false;
  const bool allInFront = std::all_of(
      vertices.begin(), vertices.end(),
      [](const SceneryCameraVertex &vertex) { return vertex.z > 0; });
  if (!allInFront) {
    if (touchesNativeHorizontalBoundary != nullptr)
      *touchesNativeHorizontalBoundary = false;
    return true;
  }

  const int64_t projectionPlane = workspace.transform.projectionPlane;
  if (touchesNativeHorizontalBoundary != nullptr) {
    *touchesNativeHorizontalBoundary = std::any_of(
        vertices.begin(), vertices.end(),
        [&](const SceneryCameraVertex &vertex) {
          const __int128 nativeX =
              static_cast<__int128>(256) * vertex.z +
              static_cast<__int128>(projectionPlane) * vertex.x;
          return nativeX < 0 || nativeX > static_cast<__int128>(512) * vertex.z;
        });
  }
  const auto allOutside = [&](const auto &outside) {
    return std::all_of(vertices.begin(), vertices.end(), outside);
  };
  // The 3/4 horizontal projection is Crash 3's exact 4:3 -> 16:9 transform.
  // Work in homogeneous camera space: no GTE divide or screen quantisation is
  // needed to make a conservative common-plane rejection. The configurable
  // host guard expands only geometry admission. qx4/(4*z) is the final
  // 0..512 presentation coordinate, hence a G-pixel margin moves each plane
  // by exactly 4*G*z. Vulkan still clips against the real output viewport.
  const auto outsideHorizontal = [&](uint32_t guardPixels) {
    return allOutside([&](const SceneryCameraVertex &vertex) {
             const __int128 qx4 =
                 static_cast<__int128>(1024) * vertex.z +
                 static_cast<__int128>(3) * projectionPlane * vertex.x;
             const __int128 guard =
                 static_cast<__int128>(4) * guardPixels * vertex.z;
             return qx4 < -guard;
           }) ||
           allOutside([&](const SceneryCameraVertex &vertex) {
             const __int128 qx4 =
                 static_cast<__int128>(1024) * vertex.z +
                 static_cast<__int128>(3) * projectionPlane * vertex.x;
             const __int128 guard =
                 static_cast<__int128>(4) * guardPixels * vertex.z;
             return qx4 > static_cast<__int128>(2048) * vertex.z + guard;
           });
  };
  const bool outsideVertical =
      allOutside([&](const SceneryCameraVertex &vertex) {
        const __int128 qy = static_cast<__int128>(128) * vertex.z +
                            static_cast<__int128>(projectionPlane) * vertex.y;
        return qy < 0;
      }) ||
      allOutside([&](const SceneryCameraVertex &vertex) {
        const __int128 qy = static_cast<__int128>(128) * vertex.z +
                            static_cast<__int128>(projectionPlane) * vertex.y;
        return qy > static_cast<__int128>(256) * vertex.z;
      });
  const uint32_t guardPixels = guardOverride.value_or(sceneryCullGuardPixels());
  const bool outsideExactHorizontal = outsideHorizontal(0u);
  const bool outsideGuardHorizontal = outsideHorizontal(guardPixels);
  if (insideHorizontalGuardOnly != nullptr) {
    *insideHorizontalGuardOnly = guardPixels != 0u && outsideExactHorizontal &&
                                 !outsideGuardHorizontal && !outsideVertical;
  }
  if (outsideGuardHorizontal || outsideVertical)
    return false;
  return true;
}

bool sceneryFaceIndices(const Memory &memory,
                        const ResidentSceneryGeometry &geometry,
                        uint32_t polygonId, uint32_t quadHalf,
                        std::array<uint32_t, 3> &indices) noexcept {
  if (quadHalf == 0u) {
    const uint32_t wordA =
        memory.read32(geometry.trianglePivot - (polygonId + 1u) * 4u);
    const uint32_t wordB =
        memory.read16(geometry.trianglePivot + polygonId * 2u);
    indices = {{(wordA >> 20u) & 0xfffu, (wordA >> 8u) & 0xfffu,
                (wordB >> 4u) & 0xfffu}};
    return true;
  }

  const uint32_t quad = geometry.quadStart + polygonId * 8u;
  const uint32_t wordA = memory.read32(quad);
  const uint32_t wordB = memory.read32(quad + 4u);
  const uint32_t a = (wordA >> 8u) & 0xfffu;
  const uint32_t b = (wordA >> 20u) & 0xfffu;
  const uint32_t c = (wordB >> 20u) & 0xfffu;
  const uint32_t d = (wordB >> 8u) & 0xfffu;
  indices = quadHalf == 1u ? std::array<uint32_t, 3>{{b, a, c}}
                           : std::array<uint32_t, 3>{{a, c, d}};
  return true;
}

std::optional<uint32_t> sceneryFaceAverageDepthFromDescriptors(
    const Memory &memory, const ResidentSceneryGeometry &geometry,
    uint32_t world, uint32_t quadHalf, uint32_t depthMode, uint32_t descriptorA,
    uint32_t descriptorB, SceneryCameraWorkspace &cameraWorkspace,
    SceneryGeometryStats &stats) noexcept {
  std::array<uint32_t, 4> indices{};
  uint32_t vertexCount = 3u;
  if (quadHalf == 0u) {
    indices[0] = (descriptorA >> 20u) & 0xfffu;
    indices[1] = (descriptorA >> 8u) & 0xfffu;
    indices[2] = (descriptorB >> 4u) & 0xfffu;
  } else {
    const uint32_t a = (descriptorA >> 8u) & 0xfffu;
    const uint32_t b = (descriptorA >> 20u) & 0xfffu;
    const uint32_t c = (descriptorB >> 20u) & 0xfffu;
    const uint32_t d = (descriptorB >> 8u) & 0xfffu;
    if (depthMode == 1u) {
      // A state-1 SLST entry executes the first three-corner path and AVSZ3.
      indices = {{b, a, c, 0u}};
    } else if (depthMode == 2u) {
      // State 2 starts at func_8004103C's second-half path and also uses
      // AVSZ3.  This distinction is observable in its material LOD program.
      indices = {{a, c, d, 0u}};
    } else if (depthMode == 3u) {
      // Only state 3 executes RTPT+RTPS followed by AVSZ4.  A completely
      // absent quad is reconstructed in this canonical complete form too.
      indices = {{a, b, c, d}};
      vertexCount = 4u;
    } else {
      return std::nullopt;
    }
  }

  uint32_t szSum = 0u;
  for (uint32_t corner = 0u; corner < vertexCount; ++corner) {
    SceneryCameraVertex camera;
    if (!readSceneryCameraVertex(memory, geometry, world, indices[corner],
                                 cameraWorkspace, stats, camera)) {
      return std::nullopt;
    }
    // RTPT first publishes (raw MAC3 >> 12) through the architectural signed
    // 32-bit MAC3 register, then transfers that value to the unsigned 16-bit
    // SZ FIFO. Preserve the intervening truncation: distant transformed
    // scenery can cross INT32_MAX during rotation, and clamping the raw 64-bit
    // accumulator directly would assign a different local OT bucket.
    const int32_t mac3 = static_cast<int32_t>(camera.z >> 12u);
    const uint32_t sz = mac3 <= 0
                            ? 0u
                            : static_cast<uint32_t>(std::min<int32_t>(
                                  mac3, static_cast<int32_t>(UINT16_MAX)));
    szSum += sz;
  }

  // The renderer installs ZSF3=1365 and ZSF4=1024 and reads MAC0>>15 after
  // AVSZ rather than the clamped OTZ register.
  const uint32_t zScale = vertexCount == 3u ? 1365u : 1024u;
  return (zScale * szSum) >> 15u;
}

std::optional<uint32_t> sceneryFaceAverageDepth(
    const Memory &memory, const ResidentSceneryGeometry &geometry,
    uint32_t world, uint32_t polygonId, uint32_t quadHalf, uint32_t depthMode,
    SceneryCameraWorkspace &cameraWorkspace,
    SceneryGeometryStats &stats) noexcept {
  uint32_t descriptorA = 0u;
  uint32_t descriptorB = 0u;
  if (quadHalf == 0u) {
    const uint32_t descriptorAddress =
        geometry.trianglePivot - (polygonId + 1u) * 4u;
    const uint32_t tailAddress = geometry.trianglePivot + polygonId * 2u;
    if (!isRamRange(descriptorAddress, sizeof(uint32_t)) ||
        !isRamRange(tailAddress, sizeof(uint16_t))) {
      return std::nullopt;
    }
    descriptorA = memory.read32(descriptorAddress);
    descriptorB = memory.read16(tailAddress);
  } else {
    const uint32_t descriptorAddress = geometry.quadStart + polygonId * 8u;
    if (!isRamRange(descriptorAddress, 2u * sizeof(uint32_t)))
      return std::nullopt;
    descriptorA = memory.read32(descriptorAddress);
    descriptorB = memory.read32(descriptorAddress + sizeof(uint32_t));
  }
  return sceneryFaceAverageDepthFromDescriptors(
      memory, geometry, world, quadHalf, depthMode, descriptorA, descriptorB,
      cameraWorkspace, stats);
}

std::optional<uint32_t> sceneryDynamicMaterialSelectorShift(
    const Memory &memory, const ResidentSceneryGeometry &geometry,
    uint32_t world, uint32_t quadHalf, uint32_t depthMode, uint32_t descriptorA,
    uint32_t descriptorB, SceneryCameraWorkspace &cameraWorkspace,
    SceneryGeometryStats &stats, std::optional<uint32_t> lodDepthShift,
    std::optional<uint32_t> averageDepth, bool &fromScratchTail) noexcept {
  fromScratchTail = false;
  if (!lodDepthShift.has_value()) {
    ++stats.dynamicMaterialMissingLodShift;
    return std::nullopt;
  }
  if (!averageDepth.has_value()) {
    averageDepth = sceneryFaceAverageDepthFromDescriptors(
        memory, geometry, world, quadHalf, depthMode, descriptorA, descriptorB,
        cameraWorkspace, stats);
  }
  if (!averageDepth.has_value()) {
    ++stats.dynamicMaterialDepthUnavailable;
    return std::nullopt;
  }
  const uint32_t tableIndex = *averageDepth >> (*lodDepthShift & UINT32_C(31));
  stats.dynamicMaterialMaximumLodIndex =
      std::max(stats.dynamicMaterialMaximumLodIndex, tableIndex);
  if (tableIndex > UINT32_MAX - kSceneryMaterialLodShiftScratch ||
      !isScratchpadRange(kSceneryMaterialLodShiftScratch + tableIndex,
                         sizeof(uint8_t))) {
    ++stats.dynamicMaterialLodIndexOutOfRange;
    return std::nullopt;
  }
  // This is the exact address used at 0x80041938/0x80041C10. Indices 0..31
  // read the copied ROM table; farther indices deliberately observe adjacent
  // renderer scratch. Source-selected faces in the second gameplay holdout
  // prove that saturating at byte 31 produces a different original packet.
  fromScratchTail = tableIndex >= kSceneryMaterialLodShiftTableBytes;
  if (fromScratchTail)
    ++stats.dynamicMaterialScratchTailReads;
  return memory.read8(kSceneryMaterialLodShiftScratch + tableIndex) & 31u;
}

bool decodeSceneryFaceMaterial(
    const Memory &memory, const ResidentSceneryGeometry &geometry,
    uint32_t world, uint32_t quadHalf, uint32_t depthMode, uint32_t descriptorA,
    uint32_t descriptorB, SceneryCameraWorkspace &cameraWorkspace,
    SceneryGeometryStats &stats, uint32_t animationState,
    std::optional<uint32_t> lodDepthShift, std::optional<uint32_t> averageDepth,
    ResidentPresentationMaterial &material,
    std::optional<uint32_t> selectorOverride = std::nullopt) noexcept {
  // This is r21=(r19<<4)|(r28&0xf) at 0x80041250/0x800413BC.
  // Zero selects Crash's untextured gouraud packet; values 1..0x7ff address a
  // stable record directly; bit 11 selects the resident renderer's animated
  // remap program at 0x80041924..0x80041994/0x80041BFC..0x80041C6C.
  const uint16_t token = static_cast<uint16_t>(((descriptorA & 0xffu) << 4u) |
                                               (descriptorB & 0x0fu));
  material.sourceToken = token;
  material.resolvedToken = token;
  if (token == 0u) {
    material.decoded = true;
    return true;
  }
  uint32_t materialIndex = token & 0x7ffu;
  if ((token & 0x800u) != 0u) {
    material.dynamicTexture = true;
    bool selectorFromScratchTail = false;
    const std::optional<uint32_t> selectorShift = selectorOverride.has_value()
        ? selectorOverride : sceneryDynamicMaterialSelectorShift(
            memory, geometry, world, quadHalf, depthMode, descriptorA,
            descriptorB, cameraWorkspace, stats, lodDepthShift, averageDepth,
            selectorFromScratchTail);
    if (!selectorShift.has_value())
      return false;
    material.selectorFromScratchTail = selectorFromScratchTail;
    if (geometry.materialRemap == 0u) {
      ++stats.dynamicMaterialMissingRemap;
      return false;
    }

    const auto readDescriptor = [&](uint32_t index,
                                    uint32_t &descriptor) noexcept {
      const uint64_t byteOffset =
          static_cast<uint64_t>(index) * sizeof(uint32_t);
      if (byteOffset > std::numeric_limits<uint32_t>::max() ||
          byteOffset >
              std::numeric_limits<uint32_t>::max() - geometry.materialRemap) {
        return false;
      }
      const uint32_t address =
          geometry.materialRemap + static_cast<uint32_t>(byteOffset);
      if (!isRamRange(address, sizeof(uint32_t)))
        return false;
      descriptor = memory.read32(address);
      return true;
    };

    const auto selection = selectSceneryMaterial(
        token, animationState, *selectorShift, *selectorShift,
        [&](uint32_t index) -> std::optional<uint32_t> {
          uint32_t descriptor = 0u;
          if (!readDescriptor(index, descriptor))
            return std::nullopt;
          return descriptor;
        });
    if (!selection.has_value()) {
      ++stats.dynamicMaterialDescriptorUnavailable;
      return false;
    }
    materialIndex = selection->nativeToken;
    material.resolvedToken = static_cast<uint16_t>(materialIndex);
    if (materialIndex == 0u) {
      material.decoded = true;
      return true;
    }
  }
  if (geometry.materialTable == 0u)
    return false;

  const uint64_t byteOffset =
      static_cast<uint64_t>(materialIndex - 1u) * 3u * sizeof(uint32_t);
  if (byteOffset > std::numeric_limits<uint32_t>::max())
    return false;
  const uint32_t address =
      geometry.materialTable + static_cast<uint32_t>(byteOffset);
  if (!isRamRange(address, 3u * sizeof(uint32_t)))
    return false;

  const uint32_t word0 = memory.read32(address);
  const uint32_t word1 = memory.read32(address + 4u);
  const uint32_t word2 = memory.read32(address + 8u);
  const auto decodeUv = [](uint16_t packed) {
    return std::array<float, 2>{
        {static_cast<float>(packed & 0xffu), static_cast<float>(packed >> 8u)}};
  };
  const std::array<std::array<float, 2>, 4> packetUv{{
      decodeUv(static_cast<uint16_t>(word0)),
      decodeUv(static_cast<uint16_t>(word1)),
      decodeUv(static_cast<uint16_t>(word2)),
      decodeUv(static_cast<uint16_t>(word2 >> 16u)),
  }};
  material.packetUv = packetUv;
  material.packetVertexCount = quadHalf == 0u ? 3u : 4u;
  const std::array<uint8_t, 3> corners =
      quadHalf == 2u ? std::array<uint8_t, 3>{{1u, 2u, 3u}}
                     : std::array<uint8_t, 3>{{0u, 1u, 2u}};
  for (std::size_t corner = 0; corner < corners.size(); ++corner)
    material.uv[corner] = packetUv[corners[corner]];

  const std::size_t packetVertexCount = quadHalf == 0u ? 3u : 4u;
  float minU = packetUv[0][0];
  float minV = packetUv[0][1];
  float maxU = minU;
  float maxV = minV;
  for (std::size_t corner = 1; corner < packetVertexCount; ++corner) {
    minU = std::min(minU, packetUv[corner][0]);
    minV = std::min(minV, packetUv[corner][1]);
    maxU = std::max(maxU, packetUv[corner][0]);
    maxV = std::max(maxV, packetUv[corner][1]);
  }
  material.decoded = true;
  material.textured = true;
  // func_8004103C uses ABR=3 as the opaque marker and selects opcode 0x3e
  // for every other embedded mode; retain the original tpage unchanged.
  material.semiTransparent =
      (word1 & UINT32_C(0x00600000)) != UINT32_C(0x00600000);
  material.clut = static_cast<uint16_t>(word0 >> 16u);
  // The scenery record carries Crash-private flags above the nine polygon
  // tpage bits. GP0 masks those bits and reintroduces the current global GPU
  // state, so the sidecar must retain only the packet-authored portion.
  material.texturePage =
      static_cast<uint16_t>((word1 >> 16u) & UINT32_C(0x01ff));
  if (minU != maxU)
    --maxU;
  if (minV != maxV)
    --maxV;
  material.uvDomain = {{minU, minV, maxU, maxV}};
  return true;
}

template <typename ProjectVertex>
void decodeSceneryModernUvOffset(
    const std::array<SceneryCameraVertex, 4> &cameraVertices,
    ProjectVertex &&projectVertex,
    ResidentPresentationMaterial &material) noexcept {
  material.modernUvOffsetU = 0u;
  material.modernUvOffsetV = 0u;
  if (!material.textured || material.packetVertexCount != 4u)
    return;

  std::array<gpu::ModernUVInput, 4> inputs{};
  for (std::size_t corner = 0u; corner < inputs.size(); ++corner) {
    ResidentPresentationVertex projected;
    if (!projectVertex(
            SceneryNearClipVertex{
                static_cast<double>(cameraVertices[corner].x),
                static_cast<double>(cameraVertices[corner].y),
                static_cast<double>(cameraVertices[corner].z)},
            projected)) {
      return;
    }
    inputs[corner] = {
        static_cast<float>(projected.x),
        static_cast<float>(projected.y),
        static_cast<float>(projected.projectiveW),
        static_cast<uint16_t>(material.packetUv[corner][0]),
        static_cast<uint16_t>(material.packetUv[corner][1]),
    };
  }
  // The donor predicate has only two regimes: native 1x and above-native.
  // Scale 2 selects the latter without baking the user's current 4x choice
  // into the immutable game-side material record.
  const gpu::ModernUVDomain domain =
      gpu::prepareModernUVDomain(inputs.data(), inputs.size(), 2u, true);
  material.modernUvOffsetU = static_cast<uint8_t>(domain.offsetU);
  material.modernUvOffsetV = static_cast<uint8_t>(domain.offsetV);
}

bool residentSceneryTopologyMatches(
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    const std::array<ResidentSceneryGeometry, kMaxSceneryWorlds> &geometry,
    const std::array<uint8_t, kMaxSceneryWorlds> &validity,
    uint32_t worldCount) noexcept {
  if (!g_residentSceneryTopology.valid ||
      g_residentSceneryTopology.worldCount != worldCount)
    return false;
  for (uint32_t world = 0; world < worldCount; ++world) {
    const auto &cached = g_residentSceneryTopology.worlds[world];
    if (cached.valid != (validity[world] != 0u))
      return false;
    if (validity[world] == 0u)
      continue;
    if (cached.trianglePivot != geometry[world].trianglePivot ||
        cached.quadStart != geometry[world].quadStart ||
        cached.vertexPivot != geometry[world].vertexPivot ||
        cached.vertexCount != geometry[world].vertexCount ||
        cached.triangleCount != worlds[world].triangleCount ||
        cached.quadCount != worlds[world].quadCount)
      return false;
  }
  return true;
}

bool ensureResidentSceneryTopology(
    const Memory &memory,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    const std::array<ResidentSceneryGeometry, kMaxSceneryWorlds> &geometry,
    const std::array<uint8_t, kMaxSceneryWorlds> &validity, uint32_t worldCount,
    SceneryGeometryStats &stats) {
  if (residentSceneryTopologyMatches(worlds, geometry, validity, worldCount)) {
    stats.topologyFaces =
        static_cast<uint32_t>(g_residentSceneryTopology.faces.size());
    stats.topologySharedEdges =
        static_cast<uint32_t>(g_residentSceneryTopology.sharedEdges.size());
    return true;
  }

  ResidentSceneryTopologyCache rebuilt;
  rebuilt.worldCount = worldCount;
  uint64_t faceCount = 0;
  for (uint32_t world = 0; world < worldCount; ++world) {
    rebuilt.worlds[world].valid = validity[world] != 0u;
    if (validity[world] == 0u)
      continue;
    rebuilt.worlds[world] = {true,
                             geometry[world].trianglePivot,
                             geometry[world].quadStart,
                             geometry[world].vertexPivot,
                             geometry[world].vertexCount,
                             worlds[world].triangleCount,
                             worlds[world].quadCount};
    faceCount += static_cast<uint64_t>(worlds[world].triangleCount) +
                 2u * worlds[world].quadCount;
  }
  if (faceCount > std::numeric_limits<uint32_t>::max())
    return false;
  rebuilt.faces.reserve(static_cast<std::size_t>(faceCount));
  for (uint32_t world = 0; world < worldCount; ++world) {
    if (validity[world] == 0u)
      continue;
    for (uint32_t triangle = 0; triangle < worlds[world].triangleCount;
         ++triangle) {
      ResidentSceneryFace face;
      face.packed = static_cast<uint16_t>((world << 13u) | triangle);
      face.world = static_cast<uint8_t>(world);
      face.descriptorA =
          memory.read32(geometry[world].trianglePivot - (triangle + 1u) * 4u);
      face.descriptorB =
          memory.read16(geometry[world].trianglePivot + triangle * 2u);
      sceneryFaceIndices(memory, geometry[world], triangle, 0u, face.indices);
      if (std::any_of(face.indices.begin(), face.indices.end(),
                      [&](uint32_t index) {
                        return index >= geometry[world].vertexCount;
                      }))
        return false;
      std::copy(face.indices.begin(), face.indices.end(),
                face.completeIndices.begin());
      rebuilt.faces.push_back(face);
    }
    for (uint32_t quad = 0; quad < worlds[world].quadCount; ++quad) {
      const uint32_t descriptorAddress = geometry[world].quadStart + quad * 8u;
      const uint32_t descriptorA = memory.read32(descriptorAddress);
      const uint32_t descriptorB = memory.read32(descriptorAddress + 4u);
      std::array<uint32_t, 3> firstHalf{};
      std::array<uint32_t, 3> secondHalf{};
      if (!sceneryFaceIndices(memory, geometry[world], quad, 1u, firstHalf) ||
          !sceneryFaceIndices(memory, geometry[world], quad, 2u, secondHalf)) {
        return false;
      }
      const std::array<uint32_t, 4> completeIndices{
          firstHalf[1], firstHalf[0], firstHalf[2], secondHalf[2]};
      if (std::any_of(completeIndices.begin(), completeIndices.end(),
                      [&](uint32_t index) {
                        return index >= geometry[world].vertexCount;
                      })) {
        return false;
      }
      for (uint32_t half = 1u; half <= 2u; ++half) {
        ResidentSceneryFace face;
        face.packed =
            static_cast<uint16_t>((world << 13u) | (half << 11u) | quad);
        face.world = static_cast<uint8_t>(world);
        face.half = static_cast<uint8_t>(half);
        face.indices = half == 1u ? firstHalf : secondHalf;
        face.completeIndices = completeIndices;
        face.descriptorA = descriptorA;
        face.descriptorB = descriptorB;
        rebuilt.faces.push_back(face);
      }
    }
  }

  std::unordered_map<uint32_t, uint32_t> firstFaceForEdge;
  firstFaceForEdge.reserve(rebuilt.faces.size() * 3u);
  for (uint32_t faceIndex = 0; faceIndex < rebuilt.faces.size(); ++faceIndex) {
    const ResidentSceneryFace &face = rebuilt.faces[faceIndex];
    for (uint32_t edge = 0; edge < 3u; ++edge) {
      const uint32_t first = face.indices[edge];
      const uint32_t second = face.indices[(edge + 1u) % 3u];
      const uint32_t low = std::min(first, second);
      const uint32_t high = std::max(first, second);
      const uint32_t key =
          (static_cast<uint32_t>(face.world) << 24u) | (low << 12u) | high;
      const auto [it, inserted] = firstFaceForEdge.emplace(key, faceIndex);
      if (!inserted && it->second != faceIndex)
        rebuilt.sharedEdges.push_back({it->second, faceIndex});
    }
  }

  rebuilt.valid = true;
  g_residentSceneryTopology = std::move(rebuilt);
  stats.topologyRebuilt = 1u;
  stats.topologyFaces =
      static_cast<uint32_t>(g_residentSceneryTopology.faces.size());
  stats.topologySharedEdges =
      static_cast<uint32_t>(g_residentSceneryTopology.sharedEdges.size());
  return true;
}

bool ensureResidentSceneryTopology(
    const Memory &memory,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    const std::array<ResidentSceneryGeometry, kMaxSceneryWorlds> &geometry,
    uint32_t worldCount, SceneryGeometryStats &stats) {
  std::array<uint8_t, kMaxSceneryWorlds> validity{};
  std::fill_n(validity.begin(), worldCount, uint8_t{1});
  return ensureResidentSceneryTopology(memory, worlds, geometry, validity,
                                       worldCount, stats);
}

bool mainSceneryProjection(const CPUContext &ctx) noexcept {
  constexpr int32_t kMainOfx = 256 << 16;
  constexpr int32_t kMainOfy = 128 << 16;
  return static_cast<int32_t>(ctx.cop2c[GTE_OFX]) == kMainOfx &&
         static_cast<int32_t>(ctx.cop2c[GTE_OFY]) == kMainOfy &&
         static_cast<uint16_t>(ctx.cop2c[GTE_H]) != 0u;
}

struct ResidentFaceDirectory {
  uint32_t resourceId = 0u;
  std::vector<uint32_t> faceIndices;
};

std::optional<uint32_t>
cachedSceneryAverageDepth(const CachedResidentFace &face,
                          const std::vector<SceneryCameraVertex> &vertices,
                          uint32_t depthMode) noexcept {
  uint32_t vertexCount = 3u;
  std::array<uint32_t, 4> indices{};
  if (face.presentation.half == 0u) {
    std::copy(face.indices.begin(), face.indices.end(), indices.begin());
  } else if (depthMode == 1u) {
    // A partially selected quad shares the selected half's AVSZ3/material
    // decision with both reconstructed halves. completeIndices is canonical
    // {a,b,c,d}, so this remains valid even while rebuilding half 2.
    indices = {{face.completeIndices[1], face.completeIndices[0],
                face.completeIndices[2], 0u}};
  } else if (depthMode == 2u) {
    indices = {{face.completeIndices[0], face.completeIndices[2],
                face.completeIndices[3], 0u}};
  } else if (depthMode == 3u) {
    indices = face.completeIndices;
    vertexCount = 4u;
  } else {
    return std::nullopt;
  }

  uint32_t szSum = 0u;
  for (uint32_t corner = 0u; corner < vertexCount; ++corner) {
    if (indices[corner] >= vertices.size())
      return std::nullopt;
    const int32_t mac3 =
        static_cast<int32_t>(vertices[indices[corner]].z >> 12u);
    const uint32_t sz = mac3 <= 0
                            ? 0u
                            : static_cast<uint32_t>(std::min<int32_t>(
                                  mac3, static_cast<int32_t>(UINT16_MAX)));
    szSum += sz;
  }
  const uint32_t zScale = vertexCount == 3u ? 1365u : 1024u;
  return (zScale * szSum) >> 15u;
}

bool appendCachedResidentResource(
    const Memory &memory, const CPUContext &ctx,
    const CachedResidentResource &resource,
    const std::vector<ResidentFaceDirectory> &currentFaces,
    const std::vector<ResidentPresentationFace> &currentFaceSource,
    const std::vector<SceneryDetailOverride> &currentDetailSource,
    const std::array<int32_t, 3> &cameraOffset, bool forceRetained,
    ResidentPresentationSnapshot &snapshot,
    SceneryDetailSnapshot &textureDetails, bool &intersectsPresentation) {
  const ResidentPresentationResource &descriptor = resource.descriptor;
  if (resource.variants != nullptr && resource.variants->plan != nullptr &&
      (!resource.variants->plan->replacements.empty() ||
       !resource.variants->plan->refinements.empty())) {
    const auto &plan = resource.variants->plan;
    if (std::none_of(textureDetails.variants.begin(),
                     textureDetails.variants.end(), [&](const auto &old) {
                       return old->resourceId == plan->resourceId &&
                              old->geometryFingerprint ==
                                  plan->geometryFingerprint;
                     }))
      textureDetails.variants.push_back(plan);
  }
  std::array<int32_t, 3> translation{};
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    if ((descriptor.origin[axis] & 3) != 0)
      return false;
    const int64_t value = static_cast<int64_t>(descriptor.origin[axis] / 4) +
                          (descriptor.flags == 0u ? cameraOffset[axis] : 0);
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
      return false;
    }
    translation[axis] = static_cast<int32_t>(value);
  }
  if (descriptor.flags > 1u)
    return false;
  const uint32_t translationXy =
      static_cast<uint16_t>(translation[0]) |
      (static_cast<uint32_t>(static_cast<uint16_t>(translation[1])) << 16u);

  std::array<ResidentSceneryGeometry, kMaxSceneryWorlds> noGeometry{};
  SceneryCameraWorkspace workspace;
  prepareSceneryCameraWorkspace(ctx, noGeometry, 0u, workspace);
  (void)prepareResidentSceneryVertexDeformation(memory, workspace);
  SceneryGeometryStats stats;
  std::vector<SceneryCameraVertex> vertices;
  vertices.reserve(resource.vertices.size());
  for (const CachedResidentVertex &vertex : resource.vertices) {
    vertices.push_back(transformCachedSceneryVertex(
        memory, vertex, translationXy, translation[2], workspace, stats));
  }

  const double projectionPlane =
      static_cast<double>(workspace.transform.projectionPlane);
  const double horizontalScale = horizontalProjectionScale();
  const double nearZ = projectionPlane * 2048.0 + 1.0;
  const auto projectVertex = [&](const SceneryNearClipVertex &camera,
                                 ResidentPresentationVertex &output) {
    const double projectiveW = camera.z / 4096.0;
    if (!std::isfinite(camera.x) || !std::isfinite(camera.y) ||
        !std::isfinite(camera.z) ||
        !gpu::replay::vulkan_abi::isProjectiveWInDepthDomain(projectiveW)) {
      return false;
    }
    output = {256.0 + horizontalScale * projectionPlane * camera.x / camera.z,
              128.0 + projectionPlane * camera.y / camera.z, projectiveW};
    return std::isfinite(output.x) && std::isfinite(output.y);
  };
  const auto noteIntersection = [&](const ResidentPresentationFace &face) {
    double minX = std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    for (const ResidentPresentationVertex &vertex : face.vertices) {
      minX = std::min(minX, vertex.x);
      minY = std::min(minY, vertex.y);
      maxX = std::max(maxX, vertex.x);
      maxY = std::max(maxY, vertex.y);
    }
    intersectsPresentation |=
        maxX >= 0.0 && minX <= 512.0 && maxY >= 0.0 && minY <= 256.0;
  };

  for (const CachedResidentFace &cachedFace : resource.faces) {
    ResidentPresentationFace face = cachedFace.presentation;
    const ResidentPresentationFace &cachedPresentation =
        cachedFace.presentation;
    const auto currentResource = std::find_if(
        currentFaces.begin(), currentFaces.end(), [&](const auto &directory) {
          return directory.resourceId == face.resourceId;
        });
    const uint32_t currentIndex =
        currentResource != currentFaces.end() &&
                face.resourceFaceOrdinal < currentResource->faceIndices.size()
            ? currentResource->faceIndices[face.resourceFaceOrdinal]
            : UINT32_MAX;
    const ResidentPresentationFace *currentFace =
        currentIndex < currentFaceSource.size()
            ? &currentFaceSource[currentIndex]
            : nullptr;
    if (currentFace != nullptr) {
      const auto savedVertices = face.vertices;
      const auto savedModel = face.modelPosition;
      const auto savedMaterial = face.material;
      const auto savedColors = face.color255;
      const bool savedColorDecoded = face.colorDecoded;
      face = *currentFace;
      face.vertices = savedVertices;
      face.modelPosition = savedModel;
      if (currentFace->nearPlaneClipped) {
        face.material = savedMaterial;
        face.color255 = savedColors;
        face.colorDecoded = savedColorDecoded;
      }
    } else {
      face.sourceSelectionState = 0u;
      face.sourceTraversalOrdinal = 0u;
      face.sourceTraversalOrdinalValid = false;
      face.sourceSelected = false;
    }
    // This function reconstructs current projected coordinates from packed
    // vertices, even when the cached capture originally elided them. Do not
    // carry that capture's raw-only storage mode into the new endpoint.
    face.gpuRawOnly = false;
    face.nearPlaneClipped = false;
    face.topologyOrdinal = static_cast<uint32_t>(snapshot.faces.size());

    std::array<SceneryCameraVertex, 3> triangle{};
    for (std::size_t corner = 0u; corner < cachedFace.indices.size();
         ++corner) {
      const uint32_t index = cachedFace.indices[corner];
      if (index >= vertices.size())
        return false;
      triangle[corner] = vertices[index];
      face.modelPosition[corner] = vertices[index].model;
      face.sourceVertexIndex[corner] = index;
      if (vertices[index].colorDecoded)
        face.color255[corner] = vertices[index].color255;
    }
    face.colorDecoded = std::all_of(
        cachedFace.indices.begin(), cachedFace.indices.end(),
        [&](uint32_t index) {
          return index < vertices.size() && vertices[index].colorDecoded;
        });

    if (face.half != 0u) {
      std::array<SceneryCameraVertex, 4> completeCameraVertices{};
      for (std::size_t corner = 0u; corner < cachedFace.completeIndices.size();
           ++corner) {
        const uint32_t index = cachedFace.completeIndices[corner];
        if (index >= vertices.size())
          return false;
        completeCameraVertices[corner] = vertices[index];
      }
      decodeSceneryModernUvOffset(completeCameraVertices, projectVertex,
                                  face.material);
    }

    const bool requiresNearClip =
        std::any_of(triangle.begin(), triangle.end(), [&](const auto &vertex) {
          return static_cast<double>(vertex.z) < nearZ;
        });
    bool packetProjectionMatches =
        !forceRetained && currentFace != nullptr && !requiresNearClip &&
        !currentFace->nearPlaneClipped &&
        currentFace->modelPosition == face.modelPosition;
    for (std::size_t corner = 0u;
         packetProjectionMatches && corner < triangle.size(); ++corner) {
      ResidentPresentationVertex projected;
      packetProjectionMatches =
          projectVertex({static_cast<double>(triangle[corner].x),
                         static_cast<double>(triangle[corner].y),
                         static_cast<double>(triangle[corner].z)},
                        projected) &&
          projected.x == currentFace->vertices[corner].x &&
          projected.y == currentFace->vertices[corner].y &&
          projected.projectiveW == currentFace->vertices[corner].projectiveW;
    }
    face.retainedResource = !packetProjectionMatches;
    if (!packetProjectionMatches) {
      // The current guest table can contain the same EID while its placement
      // record is only partly rewritten. Such a packet is not a witness for
      // the atomically reprojected geometry: retain the last certified
      // material/topology and remove every current-SLST attachment claim.
      face.packed = cachedPresentation.packed;
      face.world = cachedPresentation.world;
      face.half = cachedPresentation.half;
      face.sourceSelectionState = 0u;
      face.sourceTraversalOrdinal = 0u;
      face.sourceTraversalOrdinalValid = false;
      face.sourceSelected = false;
      face.material = cachedPresentation.material;
    }

    uint32_t depthMode = face.half;
    if (face.half != 0u) {
      depthMode =
          face.sourceSelectionState == 1u || face.sourceSelectionState == 2u
              ? face.sourceSelectionState
              : 3u;
    }
    const std::optional<uint32_t> averageDepth =
        cachedSceneryAverageDepth(cachedFace, vertices, depthMode);
    face.orderingTableBucketValid = averageDepth.has_value();
    if (averageDepth.has_value()) {
      constexpr uint32_t kOrderingTablePivot = 1904u;
      face.orderingTableBucket =
          static_cast<uint16_t>(*averageDepth < kOrderingTablePivot
                                    ? kOrderingTablePivot - *averageDepth
                                    : 0u);
    }
    if (packetProjectionMatches &&
        (!currentFace->orderingTableBucketValid ||
         !face.orderingTableBucketValid ||
         currentFace->orderingTableBucket != face.orderingTableBucket)) {
      // A complete quad's AVSZ4 also consumes the fourth vertex, which is not
      // present in either individual packet triangle. Require that independent
      // bucket witness as well before attaching a rebuilt half to the packet.
      face.retainedResource = true;
      face.packed = cachedPresentation.packed;
      face.world = cachedPresentation.world;
      face.half = cachedPresentation.half;
      face.sourceSelectionState = 0u;
      face.sourceTraversalOrdinal = 0u;
      face.sourceTraversalOrdinalValid = false;
      face.sourceSelected = false;
      face.material = cachedPresentation.material;
    }

    std::optional<ResidentPresentationMaterial> highestMaterial;
    if (face.retainedResource || currentFace == nullptr ||
        currentFace->nearPlaneClipped) {
      const auto high = std::lower_bound(
          resource.highestMaterials.begin(), resource.highestMaterials.end(),
          face.resourceFaceOrdinal, [](const auto &record, uint32_t ordinal) {
            return record.faceOrdinal < ordinal;
          });
      if (high != resource.highestMaterials.end() &&
          high->faceOrdinal == face.resourceFaceOrdinal &&
          high->resourceId == face.resourceId &&
          high->geometryFingerprint == face.resourceGeometryFingerprint)
        highestMaterial = high->material;
    } else {
      const auto high = std::lower_bound(
          currentDetailSource.begin(), currentDetailSource.end(), currentIndex,
          [](const auto &record, uint32_t index) {
            return record.presentationFaceIndex < index;
          });
      if (high != currentDetailSource.end() &&
          high->presentationFaceIndex == currentIndex)
        highestMaterial =
            applySceneryDetailMaterial(*currentFace, *high, currentIndex);
    }
    if (highestMaterial.has_value() && face.half != 0u) {
      std::array<SceneryCameraVertex, 4> completeCameraVertices{};
      for (std::size_t corner = 0u; corner < cachedFace.completeIndices.size();
           ++corner)
        completeCameraVertices[corner] =
            vertices[cachedFace.completeIndices[corner]];
      decodeSceneryModernUvOffset(completeCameraVertices, projectVertex,
                                  *highestMaterial);
    }

    if (workspace.construction.twoPointLighting || resource.constructedLighting) {
      std::array<SceneryCameraVertex, 4> quad{};
      if (face.half != 0u)
        for (uint32_t corner = 0; corner < 4u; ++corner)
          quad[corner] = vertices[cachedFace.completeIndices[corner]];
      if (cachedFace.unconstructedMaterial.decoded)
        face.material = cachedFace.unconstructedMaterial;
      if (face.half != 0u)
        decodeSceneryModernUvOffset(quad, projectVertex, face.material);
      constructSceneryLighting(workspace.construction, resource,
                               cachedFace.indices, cachedFace.completeIndices,
                               triangle, quad, depthMode, face);
      // Flat-black is a constructor operation, not a low-detail texture.
      // A detail override must never undo it.
      if (!face.material.textured)
        highestMaterial.reset();
    }

    if (!requiresNearClip) {
      bool valid = true;
      for (std::size_t corner = 0u; corner < triangle.size(); ++corner) {
        valid &= projectVertex({static_cast<double>(triangle[corner].x),
                                static_cast<double>(triangle[corner].y),
                                static_cast<double>(triangle[corner].z)},
                               face.vertices[corner]);
      }
      if (!valid)
        return false;
      noteIntersection(face);
      if (highestMaterial.has_value())
        appendSceneryDetailMaterial(
            textureDetails, face, static_cast<uint32_t>(snapshot.faces.size()),
            *highestMaterial);
      snapshot.faces.push_back(std::move(face));
      continue;
    }

    const SceneryNearClipPolygon clipped =
        clipSceneryTriangleToNearPlane(triangle, nearZ);
    if (clipped.count < 3u)
      continue;
    for (uint8_t outputTriangle = 1u; outputTriangle + 1u < clipped.count;
         ++outputTriangle) {
      ResidentPresentationFace output = face;
      output.nearPlaneClipped = true;
      const std::array<uint8_t, 3> indices{
          {0u, outputTriangle, static_cast<uint8_t>(outputTriangle + 1u)}};
      bool valid = true;
      for (std::size_t corner = 0u; corner < indices.size(); ++corner) {
        const SceneryNearClipVertex &clippedVertex =
            clipped.vertices[indices[corner]];
        valid &= projectVertex(clippedVertex, output.vertices[corner]);
        for (std::size_t component = 0u; component < 2u; ++component) {
          output.material.uv[corner][component] = 0.0f;
          for (std::size_t source = 0u;
               source < clippedVertex.barycentric.size(); ++source) {
            output.material.uv[corner][component] +=
                static_cast<float>(clippedVertex.barycentric[source] *
                                   face.material.uv[source][component]);
          }
        }
        for (std::size_t component = 0u; component < 3u; ++component) {
          output.color255[corner][component] = 0.0f;
          for (std::size_t source = 0u;
               source < clippedVertex.barycentric.size(); ++source) {
            output.color255[corner][component] +=
                static_cast<float>(clippedVertex.barycentric[source] *
                                   face.color255[source][component]);
          }
        }
      }
      if (!valid)
        return false;
      noteIntersection(output);
      if (highestMaterial.has_value()) {
        SceneryClipWeights weights;
        for (std::size_t corner = 0u; corner < indices.size(); ++corner)
          weights[corner] = clipped.vertices[indices[corner]].barycentric;
        appendSceneryDetailMaterial(
            textureDetails, output,
            static_cast<uint32_t>(snapshot.faces.size()),
            clipSceneryDetailMaterial(*highestMaterial, weights));
      }
      snapshot.faces.push_back(std::move(output));
    }
  }
  return true;
}

bool applyResidentCohortPresentation(const Memory &memory,
                                     const CPUContext &ctx,
                                     const std::vector<uint16_t> &source,
                                     ResidentPresentationSnapshot &snapshot,
                                     SceneryDetailSnapshot &textureDetails) {
  snapshot.cohortPresentationValid = true;
  if (!g_residentCohortPresentationEnabled)
    return true;
  ResidentCohortJournal &journal = g_residentCohortJournal;
  if (journal.activeResources.empty() || journal.activeCache.empty())
    return true;

  const std::vector<ResidentResourceKey> currentResources =
      residentResourceKeys(snapshot.resources);
  const bool currentIsActive = snapshot.resourceSetValidated &&
                               currentResources == journal.activeResources;
  const bool rebuildActive = !snapshot.resourceSetValidated;
  const bool presentCandidate =
      snapshot.resourceSetValidated && !currentIsActive;
  snapshot.candidateCohortPresented = presentCandidate;

  // A valid candidate is already the best source for its incoming resources.
  // During an overlapping handoff, supplement it with only the active
  // resources it has not acquired yet. A disjoint candidate is a hard scene
  // boundary and must remain free of the previous scene. Invalid/partially
  // rewritten tables instead fall back atomically to the complete active set.
  std::vector<const CachedResidentResource *> activeToAppend;
  if (rebuildActive) {
    activeToAppend.reserve(journal.activeCache.size());
    for (const CachedResidentResource &resource : journal.activeCache)
      activeToAppend.push_back(&resource);
  } else if (presentCandidate && !snapshot.cohortDisjoint) {
    for (const CachedResidentResource &resource : journal.activeCache) {
      if (!std::binary_search(currentResources.begin(), currentResources.end(),
                              residentResourceKey(resource.descriptor))) {
        activeToAppend.push_back(&resource);
      }
    }
  }
  const bool appendAdjacent = !presentCandidate || !snapshot.cohortDisjoint;
  if (activeToAppend.empty() &&
      (!appendAdjacent || journal.sceneCache.empty())) {
    return true;
  }

  std::optional<std::array<int32_t, 3>> cameraOffset;
  const auto useAnchor = [&](uint32_t world) {
    if (world >= snapshot.resources.size())
      return false;
    const ResidentPresentationResource &resource = snapshot.resources[world];
    if (!resource.identityValid || !resource.placementValidated ||
        resource.flags != 0u) {
      return false;
    }
    std::array<int32_t, 3> offset{};
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      if ((resource.origin[axis] & 3) != 0)
        return false;
      offset[axis] = resource.translation[axis] - resource.origin[axis] / 4;
    }
    cameraOffset = offset;
    return true;
  };
  for (auto entry = source.rbegin();
       entry != source.rend() && !cameraOffset.has_value(); ++entry) {
    (void)useAnchor(*entry >> 13u);
  }
  for (uint32_t world = 0u;
       world < snapshot.resources.size() && !cameraOffset.has_value();
       ++world) {
    (void)useAnchor(world);
  }
  // A flags-1 resource is intrinsically placed at origin/4 and therefore has
  // no camera-offset dependency.  Some valid resident cohorts contain only
  // that form; requiring an unrelated flags-0 row would make their retention
  // fail for reasons outside the resource contract.  Conversely, never invent
  // zero when even one appended resource actually needs a camera offset.
  bool appendedResourceNeedsCameraOffset = std::any_of(
      activeToAppend.begin(), activeToAppend.end(),
      [](const auto *resource) { return resource->descriptor.flags == 0u; });
  if (appendAdjacent) {
    appendedResourceNeedsCameraOffset |= std::any_of(
        journal.sceneCache.begin(), journal.sceneCache.end(),
        [](const auto &resource) { return resource.descriptor.flags == 0u; });
  }
  if (!cameraOffset.has_value() && !appendedResourceNeedsCameraOffset)
    cameraOffset = std::array<int32_t, 3>{};
  if (!cameraOffset.has_value()) {
    snapshot.cohortPresentationValid = false;
    return false;
  }

  std::vector<ResidentPresentationFace> originalFaces;
  std::vector<SceneryDetailOverride> originalDetails;
  const auto originalVariants = textureDetails.variants;
  const std::size_t originalFaceCount = snapshot.faces.size();
  const std::size_t originalDetailCount = textureDetails.materials.size();
  if (rebuildActive) {
    originalFaces = std::move(snapshot.faces);
    snapshot.faces.clear();
    originalDetails = std::move(textureDetails.materials);
    textureDetails.materials.clear();
    textureDetails.variants.clear();
  }
  const std::vector<ResidentPresentationFace> &currentFaceSource =
      rebuildActive ? originalFaces : snapshot.faces;
  const auto &currentDetailSource =
      rebuildActive ? originalDetails : textureDetails.materials;
  std::vector<ResidentFaceDirectory> currentFaces;
  currentFaces.reserve(kMaxSceneryWorlds);
  for (uint32_t faceIndex = 0u; faceIndex < currentFaceSource.size();
       ++faceIndex) {
    const ResidentPresentationFace &face = currentFaceSource[faceIndex];
    auto directory = std::find_if(
        currentFaces.begin(), currentFaces.end(), [&](const auto &candidate) {
          return candidate.resourceId == face.resourceId;
        });
    if (directory == currentFaces.end()) {
      currentFaces.push_back({face.resourceId, {}});
      directory = std::prev(currentFaces.end());
    }
    if (face.resourceFaceOrdinal >= directory->faceIndices.size()) {
      directory->faceIndices.resize(
          static_cast<std::size_t>(face.resourceFaceOrdinal) + 1u, UINT32_MAX);
    }
    uint32_t &stored = directory->faceIndices[face.resourceFaceOrdinal];
    if (stored == UINT32_MAX || (currentFaceSource[stored].nearPlaneClipped &&
                                 !face.nearPlaneClipped)) {
      stored = faceIndex;
    }
  }

  bool valid = true;
  std::vector<ResidentResourceKey> appendedResources;
  appendedResources.reserve(activeToAppend.size() + journal.sceneCache.size());
  for (const CachedResidentResource *resource : activeToAppend) {
    bool intersects = false;
    valid = appendCachedResidentResource(memory, ctx, *resource, currentFaces,
                                         currentFaceSource, currentDetailSource,
                                         *cameraOffset, !rebuildActive,
                                         snapshot, textureDetails, intersects);
    if (!valid)
      break;
    appendedResources.push_back(residentResourceKey(resource->descriptor));
    if (rebuildActive) {
      ++snapshot.reprojectedActiveResourceCount;
      snapshot.reprojectedActiveResourceFaces +=
          static_cast<uint32_t>(resource->faces.size());
    } else {
      ++snapshot.retainedResourceCount;
      snapshot.retainedResourceFaces +=
          static_cast<uint32_t>(resource->faces.size());
    }
  }
  snapshot.activeCohortReprojected =
      rebuildActive && snapshot.reprojectedActiveResourceCount != 0u;

  if (valid && appendAdjacent) {
    const std::vector<ResidentResourceKey> noCurrentResources;
    const std::vector<ResidentResourceKey> &presentedCurrentResources =
        rebuildActive ? noCurrentResources : currentResources;
    for (CachedResidentResource &resource : journal.sceneCache) {
      const ResidentResourceKey key = residentResourceKey(resource.descriptor);
      if (std::binary_search(presentedCurrentResources.begin(),
                             presentedCurrentResources.end(), key) ||
          std::find(appendedResources.begin(), appendedResources.end(), key) !=
              appendedResources.end()) {
        continue;
      }
      bool intersects = false;
      valid = appendCachedResidentResource(
          memory, ctx, resource, currentFaces, currentFaceSource,
          currentDetailSource, *cameraOffset, true, snapshot, textureDetails,
          intersects);
      if (!valid)
        break;
      resource.outsidePresentationCaptures =
          intersects ? 0u : resource.outsidePresentationCaptures + 1u;
      appendedResources.push_back(key);
      ++snapshot.retainedResourceCount;
      snapshot.retainedResourceFaces +=
          static_cast<uint32_t>(resource.faces.size());
    }
  }
  if (!valid) {
    textureDetails.variants = originalVariants;
    if (rebuildActive) {
      snapshot.faces = std::move(originalFaces);
      textureDetails.materials = std::move(originalDetails);
    } else {
      snapshot.faces.resize(originalFaceCount);
      textureDetails.materials.resize(originalDetailCount);
    }
    snapshot.retainedResourceCount = 0u;
    snapshot.retainedResourceFaces = 0u;
    snapshot.reprojectedActiveResourceCount = 0u;
    snapshot.reprojectedActiveResourceFaces = 0u;
    snapshot.activeCohortReprojected = false;
    snapshot.cohortPresentationValid = false;
    snapshot.cohortPresentationApplied = false;
    return false;
  }
  snapshot.cohortPresentationApplied =
      snapshot.activeCohortReprojected || snapshot.retainedResourceCount != 0u;
  return true;
}

// Re-express an atomically reprojected active cohort through the raw Vulkan
// ABI. The guest's current table remains authoritative for game logic, but a
// row which has fallen outside its narrow SLST/placement window must not force
// thousands of otherwise stable faces back through projected CPU records.
//
// Admission is content-addressed and fail-closed: the current table must name
// exactly the already committed resource-key set, one still-valid flags-0 row
// must witness the shared camera offset (unless every resource is flags-1),
// and every cached face must still agree with its immutable GPU topology.
bool recoverResidentGpuSceneFromCertifiedCohort(
    ResidentPresentationSnapshot &snapshot) {
  if (!snapshot.gpuScene.has_value() || !snapshot.activeCohortReprojected ||
      !snapshot.cohortPresentationValid || snapshot.resourceIdentityConflict ||
      snapshot.retainedResourceCount != 0u) {
    return false;
  }

  ResidentCohortJournal &journal = g_residentCohortJournal;
  if (journal.activeResources.empty() || journal.activeCache.empty() ||
      !journal.sceneCache.empty() ||
      residentResourceKeys(snapshot.resources) != journal.activeResources ||
      snapshot.resourceSetFingerprint != journal.activeFingerprint) {
    return false;
  }

  std::optional<std::array<int32_t, 3>> cameraOffset;
  for (const ResidentPresentationResource &resource : snapshot.resources) {
    if (!resource.identityValid || !resource.placementValidated ||
        resource.flags != 0u) {
      continue;
    }
    std::array<int32_t, 3> candidate{};
    if (!residentResourceStructuralPlacement(resource, candidate))
      continue;
    if (cameraOffset.has_value() && *cameraOffset != candidate)
      return false;
    cameraOffset = candidate;
  }
  const bool needsCameraOffset =
      std::any_of(journal.activeCache.begin(), journal.activeCache.end(),
                  [](const CachedResidentResource &resource) {
                    return resource.descriptor.flags == 0u;
                  });
  if (!cameraOffset.has_value() && !needsCameraOffset)
    cameraOffset = std::array<int32_t, 3>{};
  if (!cameraOffset.has_value())
    return false;

  // Build transactionally: a late topology/material mismatch must leave the
  // original fail-closed capture intact for the projected fallback.
  ResidentGpuSceneCapture recovered = *snapshot.gpuScene;
  recovered.resourceSetFingerprint = journal.activeFingerprint;
  recovered.resources.clear();
  recovered.presentationFaces.clear();
  recovered.staticOpaquePlanActive = false;
  recovered.staticOpaqueFaceCount = 0u;
  recovered.rawVertexCount = 0u;
  recovered.rawFaceCount = 0u;
  recovered.gpuRawOnlyFaceCount = 0u;
  recovered.reusedPayloadCount = 0u;
  recovered.rebuiltPayloadCount = 0u;
  recovered.certifiedCohortRecovery = true;
  recovered.completeForPresentation = false;

  std::vector<uint32_t> topologyBases;
  topologyBases.reserve(journal.activeCache.size());
  recovered.resources.reserve(journal.activeCache.size());
  uint64_t rawVertexCount = 0u;
  uint64_t rawFaceCount = 0u;
  for (uint32_t resourceIndex = 0u; resourceIndex < journal.activeCache.size();
       ++resourceIndex) {
    CachedResidentResource &resource = journal.activeCache[resourceIndex];
    const ResidentResourceKey key = residentResourceKey(resource.descriptor);
    if (!std::binary_search(journal.activeResources.begin(),
                            journal.activeResources.end(), key) ||
        resource.descriptor.flags > 1u ||
        !residentResourcePayloadComplete(resource)) {
      return false;
    }
    const auto duplicate = std::find_if(
        journal.activeCache.begin() + resourceIndex + 1u,
        journal.activeCache.end(), [&](const CachedResidentResource &other) {
          return residentResourceKey(other.descriptor) == key;
        });
    if (duplicate != journal.activeCache.end())
      return false;

    const auto priorPayload = resource.gpuPayload;
    if (!ensureResidentGpuResourcePayload(resource) ||
        resource.gpuPayload == nullptr ||
        resource.gpuPayload->faces.size() != resource.faces.size()) {
      return false;
    }
    recovered.reusedPayloadCount +=
        priorPayload != nullptr && priorPayload == resource.gpuPayload ? 1u
                                                                       : 0u;
    recovered.rebuiltPayloadCount +=
        priorPayload == nullptr || priorPayload != resource.gpuPayload ? 1u
                                                                       : 0u;

    const uint64_t nextVertexCount =
        rawVertexCount + resource.gpuPayload->vertices.size();
    const uint64_t nextFaceCount =
        rawFaceCount + resource.gpuPayload->faces.size();
    if (nextVertexCount > std::numeric_limits<uint32_t>::max() ||
        nextFaceCount > std::numeric_limits<uint32_t>::max()) {
      return false;
    }

    std::array<int32_t, 3> translation{};
    for (uint32_t axis = 0u; axis < translation.size(); ++axis) {
      if ((resource.descriptor.origin[axis] & 3) != 0)
        return false;
      const int64_t value =
          static_cast<int64_t>(resource.descriptor.origin[axis] / 4) +
          (resource.descriptor.flags == 0u ? (*cameraOffset)[axis] : 0);
      if (value < std::numeric_limits<int32_t>::min() ||
          value > std::numeric_limits<int32_t>::max()) {
        return false;
      }
      translation[axis] = static_cast<int32_t>(value);
    }
    topologyBases.push_back(static_cast<uint32_t>(rawFaceCount));
    recovered.resources.push_back(
        {resourceIndex, resource.descriptor.flags,
         static_cast<uint16_t>(translation[0]) |
             (static_cast<uint32_t>(static_cast<uint16_t>(translation[1]))
              << 16u),
         translation[2], resource.gpuPayload});
    rawVertexCount = nextVertexCount;
    rawFaceCount = nextFaceCount;
  }
  if (recovered.resources.size() != journal.activeCache.size() ||
      rawVertexCount == 0u || rawFaceCount == 0u)
    return false;

  recovered.presentationFaces.reserve(static_cast<std::size_t>(rawFaceCount));
  for (uint32_t resourceIndex = 0u; resourceIndex < journal.activeCache.size();
       ++resourceIndex) {
    const CachedResidentResource &resource = journal.activeCache[resourceIndex];
    const auto &payload = *resource.gpuPayload;
    for (uint32_t ordinal = 0u; ordinal < resource.faces.size(); ++ordinal) {
      const CachedResidentFace &cached = resource.faces[ordinal];
      const ResidentPresentationFace &face = cached.presentation;
      const ResidentGpuTopologyFace &topology = payload.faces[ordinal];
      if (face.resourceId != resource.descriptor.resourceId ||
          face.resourceGeometryFingerprint !=
              resource.descriptor.geometryFingerprint ||
          face.resourceFaceOrdinal != ordinal ||
          topology.resourceFaceOrdinal != ordinal ||
          face.packed != topology.packed || face.half != topology.half ||
          face.sourceVertexIndex != topology.sourceVertexIndex ||
          !face.material.decoded || !face.colorDecoded ||
          !face.orderingTableBucketValid) {
        return false;
      }
      ResidentGpuPresentationFace plan;
      plan.resourceIndex = resourceIndex;
      plan.resourceFaceOrdinal = ordinal;
      plan.topologyOrdinal = topologyBases[resourceIndex] + ordinal;
      plan.packed = face.packed;
      plan.half = face.half;
      plan.sourceSelectionState = face.sourceSelectionState;
      plan.sourceTraversalOrdinal = face.sourceTraversalOrdinal;
      plan.orderingTableBucket = face.orderingTableBucket;
      plan.sourceTraversalOrdinalValid = face.sourceTraversalOrdinalValid;
      plan.orderingTableBucketValid = true;
      plan.sourceSelected = face.sourceSelected;
      plan.requiresProjectedFallback = false;
      plan.colorDecoded = true;
      // Recovery frames have already paid for the projected oracle. Keeping
      // it as an atomic fallback avoids inheriting an old compact-plan promise
      // while still letting the raw Vulkan path consume every matched face.
      plan.gpuRawOnly = false;
      plan.material = face.material;
      recovered.presentationFaces.push_back(std::move(plan));
    }
  }

  for (ResidentPresentationFace &face : snapshot.faces) {
    const ResidentResourceKey key{face.resourceId,
                                  face.resourceGeometryFingerprint};
    const auto resource =
        std::find_if(journal.activeCache.begin(), journal.activeCache.end(),
                     [&](const CachedResidentResource &candidate) {
                       return residentResourceKey(candidate.descriptor) == key;
                     });
    if (resource == journal.activeCache.end() ||
        face.resourceFaceOrdinal >= resource->faces.size()) {
      return false;
    }
    const uint32_t resourceIndex =
        static_cast<uint32_t>(resource - journal.activeCache.begin());
    const ResidentGpuTopologyFace &topology =
        resource->gpuPayload->faces[face.resourceFaceOrdinal];
    if (face.packed != topology.packed || face.half != topology.half ||
        (!face.nearPlaneClipped &&
         face.sourceVertexIndex != topology.sourceVertexIndex)) {
      return false;
    }
    face.world = static_cast<uint8_t>(resourceIndex);
    face.resourceFlags = resource->descriptor.flags;
    face.topologyOrdinal =
        topologyBases[resourceIndex] + face.resourceFaceOrdinal;
    face.gpuRawOnly = false;

    if (!face.nearPlaneClipped) {
      ResidentGpuPresentationFace &plan =
          recovered.presentationFaces[face.topologyOrdinal];
      plan.sourceSelectionState = face.sourceSelectionState;
      plan.sourceTraversalOrdinal = face.sourceTraversalOrdinal;
      plan.orderingTableBucket = face.orderingTableBucket;
      plan.sourceTraversalOrdinalValid = face.sourceTraversalOrdinalValid;
      plan.orderingTableBucketValid = face.orderingTableBucketValid;
      plan.sourceSelected = face.sourceSelected;
      plan.colorDecoded = face.colorDecoded;
      plan.material = face.material;
      if (!plan.orderingTableBucketValid || !plan.colorDecoded ||
          !plan.material.decoded) {
        return false;
      }
    }
  }

  recovered.rawVertexCount = static_cast<uint32_t>(rawVertexCount);
  recovered.rawFaceCount = static_cast<uint32_t>(rawFaceCount);
  recovered.completeForPresentation =
      recovered.presentationFaces.size() == recovered.rawFaceCount;
  if (!recovered.completeForPresentation)
    return false;
  snapshot.gpuScene = std::move(recovered);
  return true;
}

void publishResidentPresentationSnapshot(
    ResidentPresentationSnapshot snapshot, SceneryDetailSnapshot textureDetails) {
  constexpr std::size_t kRetainedFrames = 8u;
  auto published =
      std::make_shared<const ResidentPresentationSnapshot>(std::move(snapshot));
  publishSceneryDetailSnapshot(std::move(textureDetails), published);
  std::scoped_lock lock(g_residentPresentationMutex);
  const auto existing = std::find_if(
      g_residentPresentationSnapshots.begin(),
      g_residentPresentationSnapshots.end(), [&](const auto &candidate) {
        return candidate->captureId == published->captureId;
      });
  if (existing != g_residentPresentationSnapshots.end()) {
    *existing = std::move(published);
    return;
  }
  g_residentPresentationSnapshots.push_back(std::move(published));
  while (g_residentPresentationSnapshots.size() > kRetainedFrames)
    g_residentPresentationSnapshots.pop_front();
}

struct ResidentProjectionTraceConfig {
  bool requested = false;
  bool facesRequested = false;
  uint64_t frameFrom = 0u;
  uint64_t frameTo = std::numeric_limits<uint64_t>::max();
};

const ResidentProjectionTraceConfig &residentProjectionTraceConfig() {
  static const ResidentProjectionTraceConfig config = [] {
    ResidentProjectionTraceConfig parsed;
    const char *requested = std::getenv("PS1_TRACE_CRASH3_RESIDENT_PROJECTION");
    parsed.requested =
        requested != nullptr && *requested != '\0' && *requested != '0';
    const char *faces = std::getenv("PS1_TRACE_CRASH3_RESIDENT_FACES");
    parsed.facesRequested = faces != nullptr && *faces != '\0' && *faces != '0';
    const auto parseBound = [](const char *name, uint64_t fallback) {
      const char *value = std::getenv(name);
      if (value == nullptr || *value == '\0')
        return fallback;
      char *end = nullptr;
      const unsigned long long result = std::strtoull(value, &end, 10);
      return end != value && *end == '\0' ? static_cast<uint64_t>(result)
                                          : fallback;
    };
    parsed.frameFrom = parseBound("PS1_TRACE_CRASH3_RESIDENT_FRAME_FROM", 0u);
    parsed.frameTo = parseBound("PS1_TRACE_CRASH3_RESIDENT_FRAME_TO",
                                std::numeric_limits<uint64_t>::max());
    if (parsed.frameFrom > parsed.frameTo)
      parsed.requested = false;
    return parsed;
  }();
  return config;
}

void traceResidentProjectionCatalog(
    const ResidentPresentationSnapshot &snapshot, const Memory &memory,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    const std::array<ResidentSceneryGeometry, kMaxSceneryWorlds> &geometry,
    uint32_t worldCount) {
  const ResidentProjectionTraceConfig &config = residentProjectionTraceConfig();
  if (!metrics::traceEnabled() || !config.requested ||
      snapshot.frameNumber < config.frameFrom ||
      snapshot.frameNumber > config.frameTo) {
    return;
  }

  metrics::traceAtFrame(
      "game.crash3.resident_projection_catalog", snapshot.frameNumber,
      {{"catalog_schema", 4},
       {"face_schema", 3},
       {"capture_id", snapshot.captureId},
       {"topology_fingerprint",
        std::bit_cast<int64_t>(snapshot.topologyFingerprint)},
       {"resource_set_fingerprint",
        std::bit_cast<int64_t>(snapshot.resourceSetFingerprint)},
       {"active_cohort_fingerprint",
        std::bit_cast<int64_t>(snapshot.activeCohortFingerprint)},
       {"active_cohort_generation", snapshot.activeCohortGeneration},
       {"candidate_observations", snapshot.candidateObservations},
       {"shared_active_resources", snapshot.sharedActiveResourceCount},
       {"resource_set_validated", snapshot.resourceSetValidated ? 1 : 0},
       {"placement_validated", snapshot.placementValidated ? 1 : 0},
       {"resource_identity_conflict",
        snapshot.resourceIdentityConflict ? 1 : 0},
       {"cohort_candidate", snapshot.cohortCandidate ? 1 : 0},
       {"cohort_disjoint", snapshot.cohortDisjoint ? 1 : 0},
       {"cohort_committed", snapshot.cohortCommitted ? 1 : 0},
       {"active_cohort_reprojected", snapshot.activeCohortReprojected ? 1 : 0},
       {"candidate_cohort_presented",
        snapshot.candidateCohortPresented ? 1 : 0},
       {"cohort_presentation_valid", snapshot.cohortPresentationValid ? 1 : 0},
       {"cohort_presentation_applied",
        snapshot.cohortPresentationApplied ? 1 : 0},
       {"cohort_presentation_enabled",
        g_residentCohortPresentationEnabled ? 1 : 0},
       {"retained_resources", snapshot.retainedResourceCount},
       {"retained_resource_faces", snapshot.retainedResourceFaces},
       {"scene_cache_resources", snapshot.sceneCacheResourceCount},
       {"scene_cache_faces", snapshot.sceneCacheFaces},
       {"individually_validated_resources",
        snapshot.individuallyValidatedResources},
       {"individually_admitted_resources",
        snapshot.individuallyAdmittedResources},
       {"individually_revoked_resources",
        snapshot.individuallyRevokedResources},
       {"face_trace_enabled", config.facesRequested ? 1 : 0},
       {"reprojected_active_resources",
        snapshot.reprojectedActiveResourceCount},
       {"reprojected_active_faces", snapshot.reprojectedActiveResourceFaces},
       {"world_count", worldCount},
       {"source_faces", snapshot.sourceFaceCount},
       {"resident_faces", snapshot.residentFaceCount},
       {"projected_faces", static_cast<int64_t>(snapshot.faces.size())},
       {"near_generated_faces", snapshot.nearPlaneGeneratedFaces},
       {"near_rejected_faces", snapshot.nearPlaneRejectedFaces}});
  for (uint32_t world = 0u; world < worldCount; ++world) {
    const ResidentSceneryWorld &resident = worlds[world];
    const ResidentSceneryGeometry &decoded = geometry[world];
    const ResidentPresentationResource &resource = snapshot.resources[world];
    const uint32_t resourceWord = memory.read32(resident.recordAddress);
    const bool resourcePairValid = isRamRange(resourceWord, 8u);
    // A resolved Crash hash pair is {entry pointer, EID}. The entry pointer
    // is residency state and may change when paging reuses RAM; the EID is
    // the stable resource identity used by the transition journal.
    const uint32_t resourceEntry =
        resourcePairValid ? memory.read32(resourceWord) : 0u;
    const uint32_t resourceId =
        resourcePairValid ? memory.read32(resourceWord + 4u) : 0u;
    const bool infoValid = isRamRange(resident.infoAddress, 0x1cu);
    metrics::traceAtFrame(
        "game.crash3.resident_projection_world", snapshot.frameNumber,
        {{"world_schema", 2},
         {"capture_id", snapshot.captureId},
         {"world", world},
         {"resource_word", resourceWord},
         {"resource_pair_valid", resourcePairValid ? 1 : 0},
         {"resource_entry", resourceEntry},
         {"resource_id", resourceId},
         {"record_address", resident.recordAddress},
         {"info_address", resident.infoAddress},
         {"info_valid", infoValid ? 1 : 0},
         {"info_origin_x", static_cast<uint32_t>(resource.origin[0])},
         {"info_origin_y", static_cast<uint32_t>(resource.origin[1])},
         {"info_origin_z", static_cast<uint32_t>(resource.origin[2])},
         {"info_flags", resource.flags},
         {"translation_x", memory.read32(resident.recordAddress +
                                         kResidentSceneryTranslationXOffset)},
         {"translation_y", memory.read32(resident.recordAddress +
                                         kResidentSceneryTranslationYOffset)},
         {"translation_z", memory.read32(resident.recordAddress +
                                         kResidentSceneryTranslationZOffset)},
         {"triangle_pivot", decoded.trianglePivot},
         {"quad_start", decoded.quadStart},
         {"vertex_pivot", decoded.vertexPivot},
         {"color_table", decoded.colorTable},
         {"material_remap", decoded.materialRemap},
         {"material_table", decoded.materialTable},
         {"vertex_count", decoded.vertexCount},
         {"triangle_count", resident.triangleCount},
         {"quad_count", resident.quadCount},
         {"geometry_fingerprint",
          std::bit_cast<int64_t>(resource.geometryFingerprint)},
         {"identity_valid", resource.identityValid ? 1 : 0},
         {"placement_validated", resource.placementValidated ? 1 : 0},
         {"individually_validated", resource.individuallyValidated ? 1 : 0},
         {"individual_payload_complete",
          resource.individualPayloadComplete ? 1 : 0},
         {"individual_source_witness",
          resource.individualSourceWitness ? 1 : 0},
         {"individual_placement_witness",
          resource.individualPlacementWitness ? 1 : 0},
         {"individual_observations", resource.individualObservations},
         {"scene_cache_member", resource.sceneCacheMember ? 1 : 0}});
  }
  if (!config.facesRequested)
    return;
  for (std::size_t ordinal = 0u; ordinal < snapshot.faces.size(); ++ordinal) {
    const ResidentPresentationFace &face = snapshot.faces[ordinal];
    metrics::traceAtFrame(
        "game.crash3.resident_projection_face", snapshot.frameNumber,
        {{"face_schema", 3},
         {"capture_id", snapshot.captureId},
         {"output_ordinal", static_cast<int64_t>(ordinal)},
         {"topology_ordinal", face.topologyOrdinal},
         {"resource_id", face.resourceId},
         {"resource_geometry_fingerprint",
          std::bit_cast<int64_t>(face.resourceGeometryFingerprint)},
         {"resource_face_ordinal", face.resourceFaceOrdinal},
         {"resource_flags", face.resourceFlags},
         {"retained_resource", face.retainedResource ? 1 : 0},
         {"packed", face.packed},
         {"world", face.world},
         {"half", face.half},
         {"source_selection_state", face.sourceSelectionState},
         {"source_selected", face.sourceSelected ? 1 : 0},
         {"source_traversal_valid", face.sourceTraversalOrdinalValid ? 1 : 0},
         {"source_traversal_ordinal", face.sourceTraversalOrdinal},
         {"ordering_table_bucket_valid", face.orderingTableBucketValid ? 1 : 0},
         {"ordering_table_bucket", face.orderingTableBucket},
         {"near_clipped", face.nearPlaneClipped ? 1 : 0},
         {"material_decoded", face.material.decoded ? 1 : 0},
         {"semi_transparent", face.material.semiTransparent ? 1 : 0},
         {"source_token", face.material.sourceToken},
         {"resolved_token", face.material.resolvedToken},
         {"texture_page", face.material.texturePage},
         {"clut", face.material.clut},
         {"source_vertex_0", face.sourceVertexIndex[0]},
         {"source_vertex_1", face.sourceVertexIndex[1]},
         {"source_vertex_2", face.sourceVertexIndex[2]},
         {"model_0_x", face.modelPosition[0][0]},
         {"model_0_y", face.modelPosition[0][1]},
         {"model_0_z", face.modelPosition[0][2]},
         {"model_1_x", face.modelPosition[1][0]},
         {"model_1_y", face.modelPosition[1][1]},
         {"model_1_z", face.modelPosition[1][2]},
         {"model_2_x", face.modelPosition[2][0]},
         {"model_2_y", face.modelPosition[2][1]},
         {"model_2_z", face.modelPosition[2][2]},
         {"v0_x_bits", std::bit_cast<int64_t>(face.vertices[0].x)},
         {"v0_y_bits", std::bit_cast<int64_t>(face.vertices[0].y)},
         {"v0_w_bits", std::bit_cast<int64_t>(face.vertices[0].projectiveW)},
         {"v1_x_bits", std::bit_cast<int64_t>(face.vertices[1].x)},
         {"v1_y_bits", std::bit_cast<int64_t>(face.vertices[1].y)},
         {"v1_w_bits", std::bit_cast<int64_t>(face.vertices[1].projectiveW)},
         {"v2_x_bits", std::bit_cast<int64_t>(face.vertices[2].x)},
         {"v2_y_bits", std::bit_cast<int64_t>(face.vertices[2].y)},
         {"v2_w_bits", std::bit_cast<int64_t>(face.vertices[2].projectiveW)}});
  }
}

// Compile a separate presentation choice from the real recompiled selector's
// complete program. Current SLST membership remains an oracle, not a culler.
void traceSceneryVariantProgram(const SceneryPathCache &path,
                                const ResidentPresentationSnapshot &snapshot);
bool sceneryPathTraceEnabled() {
  const char *value = std::getenv("PS1_TRACE_CRASH3_SCENERY_PATHS");
  return value != nullptr && *value == '1' && metrics::traceEnabled();
}

const SceneryZoneProgram *
prepareSceneryZoneProgram(const Memory &memory,
                          const std::vector<uint16_t> &source,
                          const ResidentPresentationSnapshot &snapshot) {
  const uint32_t owner = memory.read32(kResidentSceneryOwnerAddress);
  const auto sources = readSceneryZonePaths(memory, owner);
  if (!sources.has_value())
    return nullptr;
  std::vector<std::pair<uint32_t, uint64_t>> bindings;
  for (const auto &resource : snapshot.resources)
    bindings.emplace_back(resource.resourceId, resource.geometryFingerprint);
  const auto registered = readRegisteredSceneryPaths(memory, owner,
                                                     snapshot.resources);
  if (g_sceneryZoneProgram.owner != owner ||
      g_sceneryZoneProgram.sources != *sources ||
      g_sceneryZoneProgram.registeredSources != registered ||
      g_sceneryZoneProgram.resources != bindings) {
    SceneryZoneProgram next;
    next.owner = owner;
    next.sources = *sources;
    next.registeredSources = registered;
    next.resources = std::move(bindings);
    uint64_t decodedEntries = 0u;
    for (const auto &path : *sources) {
      SceneryPathCache decoded;
      if (!decodeSceneryPath(memory, path.entryBase, path.stateCount, decoded))
        return nullptr;
      next.stateCount += decoded.stateCount;
      for (const auto &state : decoded.states) {
        decodedEntries += state.size();
        if (decodedEntries > 8u * 1024u * 1024u)
          return nullptr;
        for (const uint16_t packed : state) {
          const uint32_t world = packed >> 13u;
          const uint32_t half = (packed >> 11u) & 3u;
          if (world >= snapshot.resources.size() ||
              (packed & 0x7ffu) >=
                  (half == 0u ? snapshot.resources[world].triangleCount
                              : snapshot.resources[world].quadCount))
            return nullptr;
        }
      }
      next.paths.push_back(std::move(decoded));
    }
    // Verify each complete native program in its original order/slot space,
    // including the independent terminal full list. Only THEN project its
    // membership onto immutable mesh identities in the current table. This is
    // not current-view admission and never extends the guest SLST.
    for (const auto &bound : registered) {
      SceneryPathCache decoded;
      if (!decodeSceneryPath(memory, bound.source.entryBase,
                              bound.source.stateCount, decoded))
        continue;
      bool valid = true;
      for (auto &state : decoded.states) {
        decodedEntries += state.size();
        if (decodedEntries > 8u * 1024u * 1024u)
          return nullptr;
        auto output = state.begin();
        for (const auto packed : state) {
          const uint32_t originalWorld = packed >> 13u;
          if (originalWorld >= bound.worldCount) {
            valid = false;
            break;
          }
          const int32_t world = bound.worlds[originalWorld];
          if (world < 0)
            continue;
          const uint32_t half = (packed >> 11u) & 3u;
          if ((packed & 0x7ffu) >=
              (half == 0u ? snapshot.resources[world].triangleCount
                           : snapshot.resources[world].quadCount)) {
            valid = false;
            break;
          }
          *output++ = static_cast<uint16_t>((world << 13u) | (packed & 0x1fffu));
        }
        if (!valid)
          break;
        state.resize(static_cast<std::size_t>(output - state.begin()));
      }
      if (!valid)
        continue;
      next.stateCount += decoded.stateCount;
      next.paths.push_back(std::move(decoded));
    }
    next.revision = ++g_sceneryPathRevision;
    g_sceneryZoneProgram = std::move(next);
    metrics::count("host.crash3.scenery_variants.zone_program_build");
  }
  // These are the values committed by the actual 80021674 routine. This join
  // works immediately after F9 as well as after a live forward/reverse delta.
  const uint32_t section = memory.read32(0x80065EC4u);
  const uint32_t index = memory.read32(0x80065ED8u);
  const uint32_t states = memory.read32(0x80065EDCu);
  for (uint32_t path = 0u; path < g_sceneryZoneProgram.sources.size(); ++path) {
    if (g_sceneryZoneProgram.sources[path].section == section &&
        g_sceneryZoneProgram.sources[path].stateCount == states &&
        index < states &&
        g_sceneryZoneProgram.paths[path].states[index] == source) {
      if (!g_sceneryZoneProgram.pathsTraced && sceneryPathTraceEnabled()) {
        for (const auto &decoded : g_sceneryZoneProgram.paths)
          traceSceneryVariantProgram(decoded, snapshot);
        g_sceneryZoneProgram.pathsTraced = true;
      }
      return &g_sceneryZoneProgram;
    }
  }
  return nullptr;
}

void selectSceneryResourceVariants(
    const Memory &memory, const std::vector<uint16_t> &source,
    const ResidentPresentationSnapshot &snapshot,
    std::vector<CachedResidentResource> &resources,
    SceneryDetailSnapshot &details) {
  if (!sceneryVariantsEnabled() ||
      !sceneryVariantBindingsReady(snapshot.resources))
    return;
  const auto *zone = prepareSceneryZoneProgram(memory, source, snapshot);
  auto &path = g_sceneryPathCache;
  bool pathMatches = zone == nullptr && path.valid &&
                     path.currentIndex < path.states.size() &&
                     source == path.states[path.currentIndex];
  if (pathMatches) {
    std::vector<std::pair<uint32_t, uint64_t>> bindings;
    for (const auto &resource : snapshot.resources)
      bindings.emplace_back(resource.resourceId, resource.geometryFingerprint);
    if (bindings == path.variantBindings) {
      pathMatches = path.variantBoundsValid;
    } else {
      // Validate the entire path against this table incarnation before
      // assigning any packed world/face bit to a resource. Never learn during a
      // half-built streamed table, or from stale addresses after a restore.
      for (const auto &state : path.states) {
        for (const uint16_t packed : state) {
          const uint32_t world = packed >> 13u;
          const uint32_t half = (packed >> 11u) & 3u;
          if (world >= resources.size() ||
              (packed & 0x7ffu) >=
                  (half == 0u ? resources[world].descriptor.triangleCount
                              : resources[world].descriptor.quadCount)) {
            pathMatches = false;
            break;
          }
        }
        if (!pathMatches)
          break;
      }
      path.variantBindings = std::move(bindings);
      path.variantBoundsValid = pathMatches;
    }
  }
  for (uint32_t world = 0u; world < resources.size(); ++world) {
    auto &resource = resources[world];
    const auto resourceKey = std::tuple{resource.descriptor.resourceId,
                                        resource.descriptor.geometryFingerprint,
                                        resource.staticMaterialFingerprint};
    if (resource.variants == nullptr) {
      const auto known = g_sceneryVariantResources.find(resourceKey);
      if (known != g_sceneryVariantResources.end())
        resource.variants = known->second.lock();
    }
    if (resource.variants == nullptr) {
      resource.variants = std::make_shared<SceneryResourceVariants>();
      std::vector<ResidentPresentationMaterial> materials;
      materials.reserve(resource.faces.size());
      for (const auto &face : resource.faces)
        materials.push_back(face.presentation.material);
      resource.variants->pairs =
          findSceneryVariantPairs(resource.gpuTopology, materials);
      std::erase_if(g_sceneryVariantResources,
                    [](const auto &entry) { return entry.second.expired(); });
      g_sceneryVariantResources[resourceKey] = resource.variants;
      metrics::count("host.crash3.scenery_variants.catalog_build");
      metrics::count("host.crash3.scenery_variants.candidates",
                     resource.variants->pairs.size());
    }
    auto &variants = *resource.variants;
    const uint64_t revision = zone != nullptr ? zone->revision : path.revision;
    if ((zone != nullptr || pathMatches) &&
        variants.observedPathRevision != revision) {
      variants.observedPathRevision = revision;
      bool validMembership = true;
      const auto observe = [&](const auto &decoded) {
        return variants.pairs.empty() ||
               observeSceneryVariantMembership(
                   variants.pairs, world, resource.descriptor.triangleCount,
                   resource.descriptor.quadCount, decoded.states);
      };
      if (zone != nullptr) {
        for (const auto &decoded : zone->paths)
          validMembership &= observe(decoded);
      } else {
        validMembership = observe(path);
      }
      if (!validMembership) {
        metrics::count("host.crash3.scenery_variants.path_invalid");
        continue;
      }
      SceneryVariantPlan plan;
      plan.resourceId = resource.descriptor.resourceId;
      plan.geometryFingerprint = resource.descriptor.geometryFingerprint;
      plan.materialFingerprint = resource.staticMaterialFingerprint;
      for (const auto &pair : variants.pairs)
        if (pair.exclusivePathWitness && !pair.coSelected)
          plan.replacements.push_back({pair.lowerFace, pair.higherFace});
      const char *refinementControl =
          std::getenv("PS1_CRASH3_SCENERY_SURFACE_REFINEMENT");
      if (refinementControl == nullptr ||
          std::strcmp(refinementControl, "off") != 0) {
        std::vector<ResidentGpuRawVertex> vertices;
        vertices.reserve(resource.vertices.size());
        for (const auto &vertex : resource.vertices)
          vertices.push_back({vertex.rawXy, vertex.rawZ, false, {}});
        std::vector<ResidentPresentationMaterial> materials;
        std::vector<const ResidentPresentationMaterial *> nearest(
            resource.faces.size());
        for (const auto &detail : resource.highestMaterials)
          if (detail.faceOrdinal < nearest.size())
            nearest[detail.faceOrdinal] = &detail.material;
        for (const auto &face : resource.faces) {
          const auto &native = face.unconstructedMaterial.decoded
                                   ? face.unconstructedMaterial
                                   : face.presentation.material;
          const uint32_t offset = (native.sourceToken & 0x7ffu) * 4u;
          const uint32_t descriptor =
              (native.sourceToken & 0x800u) &&
                      offset < resource.materialRemapByteCount
                  ? memory.read32(resource.materialRemapAddress + offset) : 0u;
          const auto material = sceneryRefinementMaterial(
              native, descriptor, nearest[materials.size()],
              resource.materialNearestShift);
          materials.push_back(material.value_or(ResidentPresentationMaterial{}));
        }
        std::vector<SceneryPathStates> programs;
        if (zone != nullptr)
          for (const auto &decoded : zone->paths)
            programs.push_back(decoded.states);
        else
          programs.push_back(path.states);
        const char *supportControl =
            std::getenv("PS1_CRASH3_SCENERY_SHARED_SUPPORT");
        plan.refinements = findScenerySurfaceRefinements(
            resource.gpuTopology, vertices, materials, world,
            resource.descriptor.triangleCount, resource.descriptor.quadCount,
            programs,
            supportControl == nullptr ||
                std::strcmp(supportControl, "off") != 0);
        if (variants.plan != nullptr)
          for (const auto &previous : variants.plan->refinements) {
            const bool strongerSourceProof = std::any_of(
                plan.refinements.begin(), plan.refinements.end(),
                [&](const auto &candidate) {
                  return candidate.lowerFaces == previous.lowerFaces &&
                         candidate.anchoredSourcePatch;
                });
            if (!strongerSourceProof && scenerySurfaceRefinementContradicted(
                    previous, world, resource.descriptor.triangleCount,
                    programs)) {
              variants.refinementVeto.insert(variants.refinementVeto.end(),
                                             previous.lowerFaces.begin(),
                                             previous.lowerFaces.end());
              metrics::count("host.crash3.scenery_variants.path_contradiction");
            } else if (std::none_of(
                           plan.refinements.begin(), plan.refinements.end(),
                           [&](const auto &candidate) {
                             return candidate.lowerFaces == previous.lowerFaces;
                           }))
              plan.refinements.push_back(previous);
          }
        std::erase_if(plan.refinements, [&](const auto &family) {
          if (family.anchoredSourcePatch)
            return false; // Full source-patch proof supersedes an overlap veto.
          return std::any_of(family.lowerFaces.begin(), family.lowerFaces.end(),
                             [&](uint32_t face) {
                               return std::find(variants.refinementVeto.begin(),
                                                variants.refinementVeto.end(),
                                                face) !=
                                      variants.refinementVeto.end();
                             });
        });
        std::sort(plan.refinements.begin(), plan.refinements.end(),
                  [](const auto &a, const auto &b) {
                    return a.lowerFaces < b.lowerFaces;
                  });
      }
      if (variants.plan == nullptr ||
          variants.plan->replacements != plan.replacements ||
          variants.plan->refinements != plan.refinements) {
        const char *selectionControl =
            std::getenv("PS1_CRASH3_SCENERY_SOURCE_SELECTION");
        const bool compileSelection =
            selectionControl == nullptr ||
            std::strcmp(selectionControl, "direct") != 0;
        // Bounded previous-checkpoint control, never a production policy.
        if (!compileSelection) {
          std::set<uint32_t> lower;
          for (const auto &family : plan.refinements)
            lower.insert(family.lowerFaces.begin(), family.lowerFaces.end());
          std::erase_if(plan.refinements, [&](const auto &family) {
            for (const auto *targets :
                 {&family.higherFaces, &family.supportFaces})
              for (uint32_t face : *targets)
                if (lower.contains(face))
                  return true;
            return false;
          });
        }
        if (compileSelection &&
            !compileSceneryDrawSelection(plan, resource.faces.size())) {
          metrics::count(
              "host.crash3.scenery_variants.source_selection_invalid");
          if (variants.plan != nullptr)
            details.variants.push_back(variants.plan);
          continue;
        }
        if (compileSelection) {
          const auto &selection = plan.selection->program;
          metrics::count("host.crash3.scenery_variants.source_selection_build");
          metrics::count("host.crash3.scenery_variants.source_selection_chains",
                         selection.chainedRules);
          metrics::trace(
              "game.crash3.scenery_source_selection",
              {{"capture_id", snapshot.captureId},
               {"resource_id", plan.resourceId},
               {"geometry_fingerprint",
                std::bit_cast<int64_t>(plan.geometryFingerprint)},
               {"source_faces", static_cast<int64_t>(selection.owner.size())},
               {"selected_faces",
                static_cast<int64_t>(selection.selectedSources.size())},
               {"source_rules",
                static_cast<int64_t>(selection.terminals.size())},
               {"chained_rules", selection.chainedRules}});
        }
        if (variants.plan != nullptr &&
            plan.replacements.size() < variants.plan->replacements.size())
          metrics::count("host.crash3.scenery_variants.path_contradiction");
        variants.plan =
            std::make_shared<const SceneryVariantPlan>(std::move(plan));
        for (const auto &pair : variants.plan->replacements)
          metrics::trace(
              "game.crash3.scenery_variant_choice",
              {{"resource_id", variants.plan->resourceId},
               {"capture_id", snapshot.captureId},
               {"geometry_fingerprint",
                std::bit_cast<int64_t>(variants.plan->geometryFingerprint)},
               {"path_revision", static_cast<int64_t>(revision)},
               {"zone_paths", zone != nullptr
                                  ? static_cast<uint32_t>(zone->paths.size())
                                  : 0u},
               {"state_count",
                zone != nullptr ? zone->stateCount : path.stateCount},
               {"lower_face", pair[0]},
               {"higher_face", pair[1]}});
        for (const auto &family : variants.plan->refinements)
          for (uint32_t lower : family.lowerFaces) {
            const auto traceChoice = [&](uint32_t higher, bool shared) {
              metrics::trace(
                  "game.crash3.scenery_surface_refinement",
                  {{"capture_id", snapshot.captureId},
                   {"resource_id", variants.plan->resourceId},
                   {"geometry_fingerprint",
                    std::bit_cast<int64_t>(variants.plan->geometryFingerprint)},
                   {"lower_face", lower},
                   {"higher_face", higher},
                   {"shared_support", shared ? 1u : 0u},
                   {"anchored_source_patch", family.anchoredSourcePatch ? 1u : 0u},
                   {"lower_faces",
                    static_cast<int64_t>(family.lowerFaces.size())},
                   {"higher_faces",
                    static_cast<int64_t>(family.higherFaces.size())},
                   {"support_faces",
                    static_cast<int64_t>(family.supportFaces.size())}});
            };
            for (uint32_t higher : family.higherFaces)
              traceChoice(higher, false);
            for (uint32_t support : family.supportFaces)
              traceChoice(support, true);
          }
      }
    }
    if (variants.plan != nullptr && (!variants.plan->replacements.empty() ||
                                     !variants.plan->refinements.empty()))
      details.variants.push_back(variants.plan);
  }
}

// Diagnostic-only exhaustive membership proof for one authored path. This
// never supplies presentation admission, and does not change the guest SLST.
// Capture it at the resident renderer, where resource incarnations are checked,
// not at the earlier delta call while its table may still be under
// construction.
void traceSceneryVariantProgram(const SceneryPathCache &path,
                                const ResidentPresentationSnapshot &snapshot) {
  const uint32_t wordCount = (path.stateCount + 63u) / 64u;
  std::vector<std::vector<std::vector<uint64_t>>> masks;
  uint64_t fingerprint = UINT64_C(14695981039346656037);
  hash32(fingerprint, path.stateCount);
  for (const auto &resource : snapshot.resources) {
    hash32(fingerprint, resource.resourceId);
    hash32(fingerprint, static_cast<uint32_t>(resource.geometryFingerprint));
    hash32(fingerprint,
           static_cast<uint32_t>(resource.geometryFingerprint >> 32u));
    masks.emplace_back(resource.triangleCount + 2u * resource.quadCount,
                       std::vector<uint64_t>(wordCount));
  }
  for (uint32_t state = 0u; state < path.states.size(); ++state) {
    hash32(fingerprint, static_cast<uint32_t>(path.states[state].size()));
    for (const uint16_t packed : path.states[state]) {
      hash32(fingerprint, packed);
      const uint32_t world = packed >> 13u;
      const uint32_t polygon = packed & 0x7ffu;
      const uint32_t half = (packed >> 11u) & 3u;
      if (world >= snapshot.resources.size())
        return;
      const auto &resource = snapshot.resources[world];
      if (polygon >= (half == 0u ? resource.triangleCount : resource.quadCount))
        return;
      const uint64_t bit = UINT64_C(1) << (state & 63u);
      if (half == 0u) {
        masks[world][polygon][state / 64u] |= bit;
      } else {
        const uint32_t base = resource.triangleCount + polygon * 2u;
        if ((half & 1u) != 0u)
          masks[world][base][state / 64u] |= bit;
        if ((half & 2u) != 0u)
          masks[world][base + 1u][state / 64u] |= bit;
      }
    }
  }
  const int64_t id = std::bit_cast<int64_t>(fingerprint);
  metrics::trace("game.crash3.scenery_variant_path_begin",
                 {{"path_id", id},
                  {"entry_base", path.entryBase},
                  {"state_count", path.stateCount},
                  {"world_count", static_cast<int64_t>(masks.size())}});
  uint32_t rows = 0u;
  for (uint32_t world = 0u; world < masks.size(); ++world) {
    const auto &resource = snapshot.resources[world];
    metrics::trace("game.crash3.scenery_variant_path_resource",
                   {{"path_id", id},
                    {"world", world},
                    {"resource_id", resource.resourceId},
                    {"geometry_fingerprint",
                     std::bit_cast<int64_t>(resource.geometryFingerprint)},
                    {"face_count", static_cast<int64_t>(masks[world].size())}});
    for (uint32_t face = 0u; face < masks[world].size(); ++face) {
      for (uint32_t word = 0u; word < wordCount; ++word) {
        if (masks[world][face][word] == 0u)
          continue;
        ++rows;
        metrics::trace(
            "game.crash3.scenery_variant_path_mask",
            {{"path_id", id},
             {"world", world},
             {"face", face},
             {"word", word},
             {"mask", std::bit_cast<int64_t>(masks[world][face][word])}});
      }
    }
  }
  metrics::trace("game.crash3.scenery_variant_path_end",
                 {{"path_id", id}, {"mask_rows", rows}});
}

void traceSceneryVariantPath(const std::vector<uint16_t> &source,
                             const ResidentPresentationSnapshot &snapshot) {
  auto &path = g_sceneryPathCache;
  if (!sceneryPathTraceEnabled() || !path.valid || !path.detailTracePending ||
      !sceneryVariantBindingsReady(snapshot.resources) ||
      path.currentIndex >= path.states.size() ||
      source != path.states[path.currentIndex])
    return;
  traceSceneryVariantProgram(path, snapshot);
  path.detailTracePending = false;
}

bool captureResidentPresentationSnapshot(
    const Memory &memory, const CPUContext &ctx, uint32_t listAddress,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    uint32_t worldCount, uint32_t residentFaceCount, uint32_t captureId,
    std::optional<uint32_t> materialLodDepthShift,
    uint32_t materialAnimationState) {
  const ScopedCrash3MetricTimer captureTimer(
      "host.crash3.resident_sidecar.capture_build_us");
  const bool collectCaptureStageMetrics = metrics::enabled();
  using CaptureStageClock = std::chrono::steady_clock;
  CaptureStageClock::time_point captureStageStartedAt{};
  if (collectCaptureStageMetrics)
    captureStageStartedAt = CaptureStageClock::now();
  const auto finishCaptureStage = [&](std::string_view metric) {
    if (!collectCaptureStageMetrics)
      return;
    const CaptureStageClock::time_point now = CaptureStageClock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        now - captureStageStartedAt);
    metrics::count(metric, static_cast<uint64_t>(
                               std::max<int64_t>(elapsed.count(), int64_t{0})));
    captureStageStartedAt = now;
  };
  if (captureId == 0u)
    return false;
  std::vector<uint16_t> source;
  if (!readDecodedSceneryList(memory, listAddress, source))
    return false;

  const auto environmentSwitchEnabled = [](const char *name) {
    const char *value = std::getenv(name);
    return value != nullptr && *value != '\0' && *value != '0';
  };
  const auto diagnosticSwitchEnabled = [&](const char *name, bool implicit) {
    const char *value = std::getenv(name);
    return value != nullptr ? environmentSwitchEnabled(name) : implicit;
  };
  const char *validationSetting = std::getenv("PS1_CRASH3_SIDECAR_VALIDATE");
  const bool validationExplicitlyDisabled =
      validationSetting != nullptr &&
      !environmentSwitchEnabled("PS1_CRASH3_SIDECAR_VALIDATE");
  const bool auditPresentationFrustum =
      diagnosticSwitchEnabled("PS1_CRASH3_RESIDENT_FRUSTUM_AUDIT",
                              (metrics::enabled() || metrics::traceEnabled()) &&
                                  !validationExplicitlyDisabled);
  const bool compactRawOpaqueRequested =
      environmentSwitchEnabled("PS1_CRASH3_VULKAN_GPU_COMPACT_OPAQUE");
  const char *textureDetailSetting =
      std::getenv("PS1_CRASH3_SCENERY_TEXTURE_DETAIL");
  const bool nearestTextureDetail =
      environmentSwitchEnabled("PS1_CRASH3_VULKAN_SCENE_SIDECAR") &&
      (textureDetailSetting == nullptr ||
       std::strcmp(textureDetailSetting, "native") != 0);
  SceneryDetailSnapshot textureDetails;
  textureDetails.captureId = captureId;
  const uint32_t nearestTextureShift = memory.read8(0x8005D4B4u) & 31u;
  // The highest band's decoded record depends on resource/token/quad half and
  // this capture's animation clock, not on camera distance or face ordinal.
  // Decode each such record once; only geometric UV offsets remain per face.
  std::array<
      std::unordered_map<uint16_t, std::optional<ResidentPresentationMaterial>>,
      kMaxSceneryWorlds>
      highestMaterialRecords;
  const bool gpuNearClipRequested =
      compactRawOpaqueRequested &&
      environmentSwitchEnabled("PS1_CRASH3_VULKAN_GPU_NEAR_CLIP");
  const bool staticOpaquePlanRequested =
      gpuNearClipRequested &&
      environmentSwitchEnabled("PS1_CRASH3_VULKAN_GPU_DIRECT_MATERIAL") &&
      environmentSwitchEnabled("PS1_CRASH3_VULKAN_GPU_PERSISTENT_FACE_PLAN") &&
      environmentSwitchEnabled("PS1_CRASH3_VULKAN_GPU_STATIC_OPAQUE_PLAN");
  const bool faceTraceRequested =
      metrics::traceEnabled() &&
      environmentSwitchEnabled("PS1_TRACE_CRASH3_RESIDENT_PROJECTION") &&
      environmentSwitchEnabled("PS1_TRACE_CRASH3_RESIDENT_FACES");
  const bool staticOpaquePlanPermitted =
      staticOpaquePlanRequested && validationExplicitlyDisabled &&
      !auditPresentationFrustum && !faceTraceRequested;

  constexpr uint8_t kTrianglePresent = 4u;
  std::vector<uint8_t> sourceStates(kSceneryIdentityCount, 0u);
  bool sourceEntriesValid = true;
  for (const uint16_t packed : source) {
    if (!validSceneryEntry(packed, worlds, worldCount)) {
      sourceEntriesValid = false;
      continue;
    }
    const uint32_t state = (packed >> 11u) & 3u;
    sourceStates[sceneryEntryIdentity(packed)] =
        static_cast<uint8_t>(state == 0u ? kTrianglePresent : state);
  }
  constexpr uint32_t kInvalidTraversalOrdinal = UINT32_MAX;
  std::vector<uint32_t> sourceTraversalOrdinals(kSceneryIdentityCount * 3u,
                                                kInvalidTraversalOrdinal);
  uint32_t sourceTraversalOrdinal = 0u;
  for (auto entry = source.rbegin(); entry != source.rend(); ++entry) {
    if (!validSceneryEntry(*entry, worlds, worldCount)) {
      ++sourceTraversalOrdinal;
      continue;
    }
    const uint32_t identity = sceneryEntryIdentity(*entry);
    const uint32_t state = (*entry >> 11u) & 3u;
    if (state == 0u) {
      sourceTraversalOrdinals[identity * 3u] = sourceTraversalOrdinal;
    } else {
      if ((state & 1u) != 0u)
        sourceTraversalOrdinals[identity * 3u + 1u] = sourceTraversalOrdinal;
      if ((state & 2u) != 0u)
        sourceTraversalOrdinals[identity * 3u + 2u] = sourceTraversalOrdinal;
    }
    ++sourceTraversalOrdinal;
  }

  SceneryGeometryStats stats;
  std::array<ResidentSceneryGeometry, kMaxSceneryWorlds> geometry{};
  std::array<uint8_t, kMaxSceneryWorlds> geometryValidity{};
  const uint32_t validGeometryWorlds = readResidentSceneryGeometryPartial(
      memory, worlds, worldCount, geometry, geometryValidity, stats);
  if (validGeometryWorlds == 0u ||
      !ensureResidentSceneryTopology(memory, worlds, geometry, geometryValidity,
                                     worldCount, stats)) {
    return false;
  }
  finishCaptureStage("host.crash3.resident_sidecar.capture_source_topology_us");

  SceneryCameraWorkspace cameraWorkspace;
  prepareSceneryCameraWorkspace(ctx, geometry, worldCount, cameraWorkspace);
  const bool vertexDeformationActive =
      prepareResidentSceneryVertexDeformation(memory, cameraWorkspace);
  ResidentPresentationSnapshot snapshot;
  snapshot.captureId = captureId;
  const bool resourceIdentitiesValid = buildResidentResourceCatalog(
      memory, worlds, geometry, geometryValidity, worldCount,
      snapshot.resources, snapshot.resourceSetFingerprint,
      snapshot.placementValidated);
  // A raw endpoint needs a coherent source table, including placement. In a
  // cold restore there may be no committed host cohort to reconstruct a row
  // which the guest left at an older camera position. Preserve this capture's
  // precise projected Vulkan endpoint until the raw scene can be certified;
  // compaction is an implementation choice, never permission to resurrect
  // culled/native scenery alongside independently reconstructed objects.
  const auto outsideCurrentTable = [&](const CachedResidentResource &cached) {
    const auto key = residentResourceKey(cached.descriptor);
    return std::none_of(snapshot.resources.begin(), snapshot.resources.end(),
                        [&](const auto &current) {
                          return residentResourceKey(current) == key;
                        });
  };
  // The raw ABI currently carries one exact table, not the adjacent bridge
  // assembled by the cohort journal. Keep the current endpoint projected
  // while that assembly can append resources. This is an exact resource-set
  // test, not a frame timeout or a game/sector-specific visibility rule.
  const bool projectedCohortMayBeNeeded =
      g_residentCohortPresentationEnabled &&
      (std::any_of(g_residentCohortJournal.activeCache.begin(),
                   g_residentCohortJournal.activeCache.end(), outsideCurrentTable) ||
       std::any_of(g_residentCohortJournal.sceneCache.begin(),
                   g_residentCohortJournal.sceneCache.end(), outsideCurrentTable));
  const bool compactRawOpaquePermitted =
      compactRawOpaqueRequested && resourceIdentitiesValid &&
      sourceEntriesValid && snapshot.placementValidated &&
      !projectedCohortMayBeNeeded;
  if (compactRawOpaqueRequested && !compactRawOpaquePermitted)
    metrics::count("host.crash3.resident_sidecar.raw_compaction_deferred");
  std::array<uint16_t, kMaxSceneryWorlds> maximumStaticMaterialToken{};
  std::array<uint16_t, kMaxSceneryWorlds> materialRemapRecords{};
  std::array<std::array<bool, 2048>, kMaxSceneryWorlds> remapSeen{};
  for (const ResidentSceneryFace &face : g_residentSceneryTopology.faces) {
    if (face.world >= worldCount)
      return false;
    const uint16_t token = static_cast<uint16_t>(
        ((face.descriptorA & 0xffu) << 4u) | (face.descriptorB & 0x0fu));
    if ((token & 0x800u) == 0u) {
      maximumStaticMaterialToken[face.world] =
          std::max(maximumStaticMaterialToken[face.world], token);
    } else if (!remapSeen[face.world][token & 0x7ffu]) {
      remapSeen[face.world][token & 0x7ffu] = true;
      const uint32_t count = (token & 0x7ffu) + 1u;
      const uint32_t remap = geometry[face.world].materialRemap;
      if (remap != 0 && isRamRange(remap, count * 4u)) {
        materialRemapRecords[face.world] = std::max<uint16_t>(
            materialRemapRecords[face.world], count);
        const uint32_t descriptor = memory.read32(remap + (count - 1u) * 4u);
        if (descriptor & 0x80000000u) {
          const uint16_t nearest = (descriptor & 0x7ffu) +
                                   ((descriptor >> nearestTextureShift) & 3u);
          if (nearest < 0x800u)
            maximumStaticMaterialToken[face.world] =
                std::max(maximumStaticMaterialToken[face.world], nearest);
        }
      }
    }
  }
  std::vector<CachedResidentResource> currentResourceCache =
      std::move(g_residentCurrentCacheScratch);
  currentResourceCache.resize(worldCount);
  std::array<uint8_t, kMaxSceneryWorlds> gpuTopologyReusable{};
  std::array<uint8_t, kMaxSceneryWorlds> presentationFaceCacheReusable{};
  for (uint32_t world = 0u; world < worldCount; ++world) {
    CachedResidentResource &cached = currentResourceCache[world];
    const ResidentResourceKey previousKey =
        residentResourceKey(cached.descriptor);
    const ResidentResourceKey currentKey =
        residentResourceKey(snapshot.resources[world]);
    const bool rawGeometryReusable =
        previousKey == currentKey &&
        cached.vertices.size() == geometry[world].vertexCount;
    cached.descriptor = snapshot.resources[world];
    cached.outsidePresentationCaptures = 0u;
    cached.adjacentOutgoing = false;
    cached.lastObservedCaptureId = 0u;
    cached.absenceObservations = 0u;
    if (geometryValidity[world] == 0u) {
      cached.vertices.clear();
      cached.faces.clear();
      cached.gpuTopology.clear();
      cached.colorTableAddress = 0u;
      cached.colorTableFingerprint = 0u;
      cached.materialTableAddress = 0u;
      cached.staticMaterialByteCount = 0u;
      cached.materialRemapAddress = 0u;
      cached.materialRemapByteCount = 0u;
      cached.staticMaterialFingerprint = 0u;
      cached.variants.reset();
      continue;
    }
    const uint64_t colorTableFingerprint =
        hashResidentMemoryRange(memory, UINT64_C(14695981039346656037),
                                geometry[world].colorTable, 0x1000u);
    const bool baseColorsReusable =
        rawGeometryReusable &&
        cached.colorTableAddress == geometry[world].colorTable &&
        cached.colorTableFingerprint == colorTableFingerprint;
    const uint64_t staticMaterialBytes64 =
        static_cast<uint64_t>(maximumStaticMaterialToken[world]) * 3u *
        sizeof(uint32_t);
    if (staticMaterialBytes64 > std::numeric_limits<uint32_t>::max())
      return false;
    const uint32_t staticMaterialBytes =
        static_cast<uint32_t>(staticMaterialBytes64);
    if (staticMaterialBytes != 0u &&
        (geometry[world].materialTable == 0u ||
         !isRamRange(geometry[world].materialTable, staticMaterialBytes))) {
      return false;
    }
    const bool staticMaterialIdentityReusable =
        rawGeometryReusable &&
        cached.materialTableAddress == geometry[world].materialTable &&
        cached.materialRemapAddress == geometry[world].materialRemap &&
        cached.materialRemapByteCount == materialRemapRecords[world] * 4u &&
        cached.materialNearestShift == nearestTextureShift &&
        cached.staticMaterialByteCount == staticMaterialBytes;
    uint64_t staticMaterialFingerprint = staticMaterialIdentityReusable
                                             ? cached.staticMaterialFingerprint
                                             : UINT64_C(14695981039346656037);
    if (!staticMaterialIdentityReusable && staticMaterialBytes != 0u) {
      // EID + geometry fingerprint + resolved table address is Crash's
      // immutable resource contract. Hash the complete addressable material
      // range once on acquisition/address reuse; repeated per-frame hashing
      // would merely reread static asset RAM after that identity has already
      // been proven. Explicit validation disables the sparse lane entirely.
      staticMaterialFingerprint = hashResidentMemoryRange(
          memory, staticMaterialFingerprint, geometry[world].materialTable,
          staticMaterialBytes);
    }
    if (!staticMaterialIdentityReusable && materialRemapRecords[world] != 0u) {
      // Include the immutable direct-distance program and nearest band in the
      // certificate identity. Animated programs are hashed, never certified as
      // static. Camera-dependent resolved tokens are not cache keys.
      hash32(staticMaterialFingerprint, nearestTextureShift);
      staticMaterialFingerprint = hashResidentMemoryRange(
          memory, staticMaterialFingerprint, geometry[world].materialRemap,
          materialRemapRecords[world] * 4u);
    }
    const std::size_t expectedFaces =
        static_cast<std::size_t>(worlds[world].triangleCount) +
        2u * worlds[world].quadCount;
    if (!rawGeometryReusable ||
        cached.materialTableAddress != geometry[world].materialTable ||
        cached.staticMaterialFingerprint != staticMaterialFingerprint)
      cached.variants.reset();
    presentationFaceCacheReusable[world] =
        rawGeometryReusable && baseColorsReusable &&
        cached.constructedLighting == cameraWorkspace.construction.twoPointLighting &&
        cached.materialTableAddress == geometry[world].materialTable &&
        cached.staticMaterialByteCount == staticMaterialBytes &&
        cached.staticMaterialFingerprint == staticMaterialFingerprint &&
        cached.faces.size() == expectedFaces;
    if (presentationFaceCacheReusable[world] == 0u)
      cached.faces.clear();
    cached.highestMaterials.clear();
    cached.vertices.resize(geometry[world].vertexCount);
    cached.faces.reserve(expectedFaces);
    const std::size_t expectedGpuFaces = expectedFaces;
    gpuTopologyReusable[world] =
        rawGeometryReusable && cached.gpuTopology.size() == expectedGpuFaces;
    if (gpuTopologyReusable[world] == 0u) {
      cached.gpuTopology.clear();
      cached.gpuTopology.reserve(expectedGpuFaces);
    }
    for (uint32_t vertex = 0u; vertex < geometry[world].vertexCount; ++vertex) {
      CachedResidentVertex &output = cached.vertices[vertex];
      if (!rawGeometryReusable) {
        output.rawXy = memory.read32(geometry[world].vertexPivot -
                                     (vertex + 1u) * sizeof(uint32_t));
        output.rawZ = memory.read16(geometry[world].vertexPivot +
                                    vertex * sizeof(uint16_t));
      }
      if (!baseColorsReusable) {
        output.colorDecoded = decodeSceneryVertexColor(memory, geometry[world],
                                                       vertex, output.color255);
      }
    }
    cached.colorTableAddress = geometry[world].colorTable;
    cached.colorTableFingerprint = colorTableFingerprint;
    cached.materialTableAddress = geometry[world].materialTable;
    cached.staticMaterialByteCount = staticMaterialBytes;
    cached.materialRemapAddress = geometry[world].materialRemap;
    cached.materialRemapByteCount = materialRemapRecords[world] * 4u;
    cached.materialNearestShift = nearestTextureShift;
    cached.staticMaterialFingerprint = staticMaterialFingerprint;
    cached.constructedLighting = cameraWorkspace.construction.twoPointLighting;
  }
  for (uint32_t topologyOrdinal = 0u;
       topologyOrdinal < g_residentSceneryTopology.faces.size();
       ++topologyOrdinal) {
    const ResidentSceneryFace &face =
        g_residentSceneryTopology.faces[topologyOrdinal];
    if (face.world >= worldCount || geometryValidity[face.world] == 0u)
      return false;
    if (gpuTopologyReusable[face.world] != 0u)
      continue;
    const ResidentPresentationResource &resource =
        snapshot.resources[face.world];
    const uint32_t resourceFaceOrdinal =
        face.half == 0u ? face.packed & 0x7ffu
                        : resource.triangleCount + (face.packed & 0x7ffu) * 2u +
                              static_cast<uint32_t>(face.half - 1u);
    currentResourceCache[face.world].gpuTopology.push_back(
        {resourceFaceOrdinal, topologyOrdinal, face.packed, face.half,
         face.indices, face.completeIndices, face.descriptorA,
         face.descriptorB});
  }
  for (const ResidentSceneryFace &face : g_residentSceneryTopology.faces) {
    if (presentationFaceCacheReusable[face.world] == 0u)
      continue;
    const ResidentPresentationResource &resource =
        snapshot.resources[face.world];
    const uint32_t resourceFaceOrdinal =
        face.half == 0u ? face.packed & 0x7ffu
                        : resource.triangleCount + (face.packed & 0x7ffu) * 2u +
                              static_cast<uint32_t>(face.half - 1u);
    const auto &faces = currentResourceCache[face.world].faces;
    if (resourceFaceOrdinal >= faces.size() ||
        faces[resourceFaceOrdinal].presentation.resourceId !=
            resource.resourceId ||
        faces[resourceFaceOrdinal].presentation.resourceFaceOrdinal !=
            resourceFaceOrdinal ||
        faces[resourceFaceOrdinal].indices != face.indices ||
        faces[resourceFaceOrdinal].completeIndices != face.completeIndices) {
      presentationFaceCacheReusable[face.world] = 0u;
      currentResourceCache[face.world].faces.clear();
    }
  }
  const bool staticOpaqueFrameActive =
      compactRawOpaquePermitted && staticOpaquePlanPermitted &&
      !cameraWorkspace.construction.twoPointLighting &&
      validGeometryWorlds == worldCount &&
      std::all_of(presentationFaceCacheReusable.begin(),
                  presentationFaceCacheReusable.begin() + worldCount,
                  [](uint8_t reusable) { return reusable != 0u; });
  const auto hashTopologyValue = [](uint64_t &hash, uint32_t value) {
    constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);
    for (uint32_t shift = 0; shift < 32u; shift += 8u) {
      hash ^= (value >> shift) & 0xffu;
      hash *= kFnvPrime;
    }
  };
  snapshot.topologyFingerprint = UINT64_C(14695981039346656037);
  hashTopologyValue(snapshot.topologyFingerprint, worldCount);
  for (uint32_t world = 0; world < worldCount; ++world) {
    hashTopologyValue(snapshot.topologyFingerprint,
                      geometry[world].trianglePivot);
    hashTopologyValue(snapshot.topologyFingerprint, geometry[world].quadStart);
    hashTopologyValue(snapshot.topologyFingerprint,
                      geometry[world].vertexPivot);
    hashTopologyValue(snapshot.topologyFingerprint, geometry[world].colorTable);
    hashTopologyValue(snapshot.topologyFingerprint,
                      geometry[world].materialRemap);
    hashTopologyValue(snapshot.topologyFingerprint,
                      geometry[world].materialTable);
    hashTopologyValue(snapshot.topologyFingerprint,
                      geometry[world].vertexCount);
    hashTopologyValue(snapshot.topologyFingerprint,
                      worlds[world].triangleCount);
    hashTopologyValue(snapshot.topologyFingerprint, worlds[world].quadCount);
  }
  finishCaptureStage("host.crash3.resident_sidecar.capture_resource_cache_us");
  const uint64_t constructionFrame = metrics::traceFrame();
  snapshot.frameNumber =
      constructionFrame == std::numeric_limits<uint64_t>::max()
          ? constructionFrame
          : constructionFrame + 1u;
  snapshot.listAddress = listAddress;
  snapshot.sourceEntryCount = static_cast<uint32_t>(source.size());
  snapshot.sourceFaceCount = sceneryFaceCount(source);
  snapshot.residentFaceCount = residentFaceCount;
  std::size_t staticOpaqueReserveElision = 0u;
  if (staticOpaqueFrameActive) {
    for (const CachedResidentResource &resource : currentResourceCache) {
      if (resource.descriptor.flags != 1u && resource.gpuPayload != nullptr)
        staticOpaqueReserveElision +=
            resource.gpuPayload->staticOpaqueFaces.size();
    }
  }
  if (staticOpaqueReserveElision > g_residentSceneryTopology.faces.size())
    return false;
  const std::size_t residualFaceCapacity =
      g_residentSceneryTopology.faces.size() - staticOpaqueReserveElision;
  snapshot.faces.reserve(residualFaceCapacity);
  std::vector<ResidentGpuPresentationFace> gpuPresentationFaces;
  gpuPresentationFaces.reserve(residualFaceCapacity);

  bool valid = true;
  const double projectionPlane =
      static_cast<double>(cameraWorkspace.transform.projectionPlane);
  const double horizontalScale = horizontalProjectionScale();
  // First unsaturated GTE projection depth, expressed in the raw 4.12 camera
  // domain used by SceneryCameraVertex. Clipping here replaces the PS1's
  // divider saturation with a geometric near plane only in the host sidecar.
  const double nearZ = projectionPlane * 2048.0 + 1.0;
  const auto projectVertex = [&](const SceneryNearClipVertex &camera,
                                 ResidentPresentationVertex &output) {
    const double z = camera.z;
    const double projectiveW = z / 4096.0;
    if (!std::isfinite(camera.x) || !std::isfinite(camera.y) ||
        !std::isfinite(z) ||
        !gpu::replay::vulkan_abi::isProjectiveWInDepthDomain(projectiveW)) {
      return false;
    }
    output = {256.0 + horizontalScale * projectionPlane * camera.x / z,
              128.0 + projectionPlane * camera.y / z, projectiveW};
    return std::isfinite(output.x) && std::isfinite(output.y);
  };
  uint32_t sourceFacesInFrustum = 0u;
  uint32_t missingFacesInFrustum = 0u;
  uint32_t unresolvedMaterialsInFrustum = 0u;
  uint32_t unresolvedMaterialsOutsideFrustum = 0u;
  uint32_t gpuOrderingIndependentFaces = 0u;
  uint32_t staticOpaqueFacesReused = 0u;
  struct QuadCaptureReuse {
    bool valid = false;
    uint8_t world = 0u;
    uint16_t polygonId = 0u;
    uint32_t depthMode = 0u;
    std::array<SceneryCameraVertex, 4> cameraVertices{};
    std::optional<uint32_t> averageDepth;
    ResidentPresentationMaterial material{};
  } quadReuse;
  for (uint32_t topologyOrdinal = 0u;
       topologyOrdinal < g_residentSceneryTopology.faces.size();
       ++topologyOrdinal) {
    const ResidentSceneryFace &face =
        g_residentSceneryTopology.faces[topologyOrdinal];
    // Retain this exact test as an audit witness only. The host reconstruction
    // transforms the complete resident topology and lets Vulkan's homogeneous
    // clipping plus the real page scissor decide whether it contributes a
    // sample. A second CPU admission boundary would recreate the very class
    // of marginal disappearances this sidecar is meant to remove.
    bool intersects = false;
    if (auditPresentationFrustum) {
      intersects = sceneryFaceIntersectsPresentationFrustum(
          memory, geometry[face.world], face.world, cameraWorkspace, stats,
          face.indices, valid, nullptr, nullptr, nullptr, 0u);
      if (!valid)
        return false;
      if (intersects)
        ++snapshot.presentationFrustumFaces;
      else
        ++snapshot.outsidePresentationFrustumFaces;
    }

    ResidentPresentationFace projected;
    const ResidentPresentationResource &resource =
        snapshot.resources[face.world];
    projected.resourceId = resource.resourceId;
    projected.resourceGeometryFingerprint = resource.geometryFingerprint;
    projected.resourceFaceOrdinal =
        face.half == 0u ? face.packed & 0x7ffu
                        : resource.triangleCount + (face.packed & 0x7ffu) * 2u +
                              static_cast<uint32_t>(face.half - 1u);
    projected.resourceFlags = resource.flags;
    projected.packed = face.packed;
    projected.world = face.world;
    projected.half = face.half;
    projected.topologyOrdinal = topologyOrdinal;
    const uint32_t traversalOrdinal =
        sourceTraversalOrdinals[sceneryEntryIdentity(face.packed) * 3u +
                                face.half];
    if (traversalOrdinal != kInvalidTraversalOrdinal) {
      projected.sourceTraversalOrdinal = traversalOrdinal;
      projected.sourceTraversalOrdinalValid = true;
    }
    const uint8_t sourceState = sourceStates[sceneryEntryIdentity(face.packed)];
    projected.sourceSelectionState = sourceState;
    projected.sourceSelected = face.half == 0u
                                   ? (sourceState & kTrianglePresent) != 0u
                                   : (sourceState & face.half) != 0u;
    // A partially selected source quad is one AVSZ3 primitive. Use that exact
    // selected half's distance decision for both reconstructed halves so host
    // completion cannot introduce a material seam. State 3 and a wholly
    // absent quad use the canonical complete-quad AVSZ4 decision.
    uint32_t depthMode = face.half;
    if (face.half != 0u)
      depthMode = sourceState == 1u || sourceState == 2u ? sourceState : 3u;
    const uint16_t polygonId = face.packed & 0x7ffu;
    const uint16_t materialToken = static_cast<uint16_t>(
        ((face.descriptorA & 0xffu) << 4u) | (face.descriptorB & 0x0fu));
    CachedResidentResource &mutableCachedResource =
        currentResourceCache[face.world];
    CachedResidentFace *reusableCachedFace = nullptr;
    if (presentationFaceCacheReusable[face.world] != 0u &&
        projected.resourceFaceOrdinal < mutableCachedResource.faces.size()) {
      CachedResidentFace &candidate =
          mutableCachedResource.faces[projected.resourceFaceOrdinal];
      if (candidate.presentation.resourceFaceOrdinal ==
              projected.resourceFaceOrdinal &&
          candidate.presentation.resourceId == projected.resourceId &&
          candidate.indices == face.indices &&
          candidate.completeIndices == face.completeIndices) {
        reusableCachedFace = &candidate;
      }
    }
    const bool reuseStaticOpaque =
        staticOpaqueFrameActive && projected.resourceFlags != 1u &&
        reusableCachedFace != nullptr && (materialToken & 0x800u) == 0u &&
        reusableCachedFace->presentation.material.decoded &&
        reusableCachedFace->presentation.material.sourceToken ==
            materialToken &&
        !reusableCachedFace->presentation.material.dynamicTexture &&
        !reusableCachedFace->presentation.material.semiTransparent &&
        reusableCachedFace->presentation.colorDecoded;
    if (reuseStaticOpaque) {
      const ResidentPresentationFace &cachedPresentation =
          reusableCachedFace->presentation;
      projected.material = cachedPresentation.material;
      projected.colorDecoded = true;
      projected.color255 = cachedPresentation.color255;
      projected.modelPosition = cachedPresentation.modelPosition;
      projected.sourceVertexIndex = face.indices;
      projected.orderingTableBucket = 0u;
      projected.orderingTableBucketValid = false;
      projected.gpuRawOnly = true;

      // Preserve the last complete geometric oracle for transactional
      // handoff, updating only the current SLST witness. A later retained
      // resource is reprojected from raw cached vertices, never from these
      // deliberately omitted current-frame coordinates.
      ResidentPresentationFace &cachedWitness =
          reusableCachedFace->presentation;
      cachedWitness.sourceSelectionState = projected.sourceSelectionState;
      cachedWitness.sourceTraversalOrdinal = projected.sourceTraversalOrdinal;
      cachedWitness.sourceTraversalOrdinalValid =
          projected.sourceTraversalOrdinalValid;
      cachedWitness.sourceSelected = projected.sourceSelected;
      cachedWitness.world = projected.world;
      cachedWitness.resourceFlags = projected.resourceFlags;

      ++snapshot.decodedMaterialFaces;
      if (projected.material.textured)
        ++snapshot.decodedTexturedFaces;
      ++snapshot.decodedColorFaces;
      ++gpuOrderingIndependentFaces;
      ++staticOpaqueFacesReused;

      continue;
    }
    const bool reuseQuadHalf =
        face.half == 2u && quadReuse.valid && quadReuse.world == face.world &&
        quadReuse.polygonId == polygonId && quadReuse.depthMode == depthMode;

    std::array<SceneryCameraVertex, 3> cameraVertices{};
    std::array<SceneryCameraVertex, 4> completeCameraVertices{};
    const CachedResidentResource &cachedResource = mutableCachedResource;
    if (face.half == 0u) {
      for (std::size_t corner = 0u; corner < face.indices.size(); ++corner) {
        if (!readCurrentCachedSceneryCameraVertex(
                memory, geometry[face.world], face.world, face.indices[corner],
                cachedResource, cameraWorkspace, stats,
                cameraVertices[corner])) {
          return false;
        }
      }
    } else if (reuseQuadHalf) {
      completeCameraVertices = quadReuse.cameraVertices;
    } else {
      for (std::size_t corner = 0u; corner < face.completeIndices.size();
           ++corner) {
        if (!readCurrentCachedSceneryCameraVertex(
                memory, geometry[face.world], face.world,
                face.completeIndices[corner], cachedResource, cameraWorkspace,
                stats, completeCameraVertices[corner])) {
          return false;
        }
      }
    }
    if (face.half != 0u) {
      const std::array<uint8_t, 3> halfCorners =
          face.half == 1u ? std::array<uint8_t, 3>{{1u, 0u, 2u}}
                          : std::array<uint8_t, 3>{{0u, 2u, 3u}};
      for (std::size_t corner = 0u; corner < cameraVertices.size(); ++corner)
        cameraVertices[corner] = completeCameraVertices[halfCorners[corner]];
    }
    const std::optional<uint32_t> averageDepth =
        reuseQuadHalf
            ? quadReuse.averageDepth
            : sceneryFaceAverageDepthFromCameraVertices(
                  cameraVertices, completeCameraVertices, face.half, depthMode);
    if (averageDepth.has_value()) {
      constexpr uint32_t kOrderingTablePivot = 1904u;
      projected.orderingTableBucket =
          static_cast<uint16_t>(*averageDepth < kOrderingTablePivot
                                    ? kOrderingTablePivot - *averageDepth
                                    : 0u);
      projected.orderingTableBucketValid = true;
      ++snapshot.decodedOrderingTableFaces;
    } else {
      ++snapshot.unresolvedOrderingTableFaces;
    }
    bool decodedMaterial = false;
    if (reuseQuadHalf) {
      projected.material = quadReuse.material;
      for (std::size_t corner = 0u; corner < projected.material.uv.size();
           ++corner) {
        projected.material.uv[corner] =
            projected.material.packetUv[corner + 1u];
      }
      if (projected.material.selectorFromScratchTail)
        ++stats.dynamicMaterialScratchTailReads;
      decodedMaterial = projected.material.decoded;
    } else {
      decodedMaterial = decodeSceneryFaceMaterial(
          memory, geometry[face.world], face.world, face.half, depthMode,
          face.descriptorA, face.descriptorB, cameraWorkspace, stats,
          materialAnimationState, materialLodDepthShift, averageDepth,
          projected.material);
      if (decodedMaterial && face.half != 0u) {
        decodeSceneryModernUvOffset(completeCameraVertices, projectVertex,
                                    projected.material);
      }
      if (decodedMaterial && face.half == 1u) {
        quadReuse.valid = true;
        quadReuse.world = face.world;
        quadReuse.polygonId = polygonId;
        quadReuse.depthMode = depthMode;
        quadReuse.cameraVertices = completeCameraVertices;
        quadReuse.averageDepth = averageDepth;
        quadReuse.material = projected.material;
      }
    }
    projected.colorDecoded = true;
    for (std::size_t corner = 0; corner < cameraVertices.size(); ++corner) {
      projected.modelPosition[corner] = cameraVertices[corner].model;
      projected.colorDecoded &= cameraVertices[corner].colorDecoded;
      projected.color255[corner] = cameraVertices[corner].color255;
    }
    const auto unconstructedMaterial = projected.material;
    constructSceneryLighting(cameraWorkspace.construction, cachedResource,
                             face.indices, face.completeIndices,
                             cameraVertices, completeCameraVertices, depthMode,
                             projected);
    if (decodedMaterial) {
      ++snapshot.decodedMaterialFaces;
      if (projected.material.textured)
        ++snapshot.decodedTexturedFaces;
      if (projected.material.dynamicTexture)
        ++snapshot.decodedDynamicMaterialFaces;
      if (projected.material.selectorFromScratchTail)
        ++snapshot.decodedScratchTailDynamicMaterialFaces;
      if (projected.material.semiTransparent)
        ++snapshot.decodedSemiTransparentFaces;
    } else {
      ++snapshot.unresolvedMaterialFaces;
      if (auditPresentationFrustum && intersects)
        ++unresolvedMaterialsInFrustum;
      else if (auditPresentationFrustum)
        ++unresolvedMaterialsOutsideFrustum;
    }

    std::optional<ResidentPresentationMaterial> highestMaterial;
    if (nearestTextureDetail && decodedMaterial &&
        projected.material.dynamicTexture) {
      ResidentPresentationMaterial nearest;
      SceneryGeometryStats detailStats;
      // Evaluate the nearest *authored* distance-table band, not token zero
      // or an inferred largest texture. The animation clock stays identical.
      const uint16_t detailKey =
          static_cast<uint16_t>((materialToken << 2u) | face.half);
      auto [record, inserted] =
          highestMaterialRecords[face.world].try_emplace(detailKey);
      if (inserted) {
        if (decodeSceneryFaceMaterial(
                memory, geometry[face.world], face.world, face.half, depthMode,
                face.descriptorA, face.descriptorB, cameraWorkspace,
                detailStats, materialAnimationState, materialLodDepthShift,
                averageDepth, nearest, nearestTextureShift))
          record->second = nearest;
        metrics::count(
            "host.crash3.resident_sidecar.highest_material_record_decoded");
      } else {
        metrics::count(
            "host.crash3.resident_sidecar.highest_material_record_reused");
      }
      if (record->second.has_value()) {
        nearest = *record->second;
        if (face.half != 0u)
          decodeSceneryModernUvOffset(completeCameraVertices, projectVertex,
                                      nearest);
        highestMaterial = nearest;
        mutableCachedResource.highestMaterials.push_back(
            {projected.resourceId, projected.resourceGeometryFingerprint,
             projected.resourceFaceOrdinal, nearest});
      } else {
        metrics::count(
            "host.crash3.resident_sidecar.highest_material_unavailable");
        if (environmentSwitchEnabled("PS1_TRACE_CRASH3_SCENERY_DETAIL"))
          metrics::trace("game.crash3.scenery_detail_unavailable",
                         {{"resource_id", projected.resourceId},
                          {"face", projected.resourceFaceOrdinal},
                          {"source_token", projected.material.sourceToken},
                          {"native_token", projected.material.resolvedToken},
                          {"nearest_token", nearest.resolvedToken}});
      }
    }
    if (!projected.material.textured)
      highestMaterial.reset();
    if (projected.colorDecoded)
      ++snapshot.decodedColorFaces;
    else
      ++snapshot.unresolvedColorFaces;
    projected.sourceVertexIndex = face.indices;
    const bool requiresNearClip = std::any_of(
        cameraVertices.begin(), cameraVertices.end(), [&](const auto &vertex) {
          return static_cast<double>(vertex.z) < nearZ;
        });
    projected.gpuRawOnly =
        compactRawOpaquePermitted && !cameraWorkspace.construction.twoPointLighting &&
        projected.resourceFlags != 1u &&
        decodedMaterial && !projected.material.semiTransparent &&
        (!highestMaterial.has_value() || !highestMaterial->semiTransparent) &&
        projected.colorDecoded && (!requiresNearClip || gpuNearClipRequested);
    ResidentGpuPresentationFace gpuPresentation;
    gpuPresentation.resourceIndex = face.world;
    gpuPresentation.resourceFaceOrdinal = projected.resourceFaceOrdinal;
    gpuPresentation.topologyOrdinal = topologyOrdinal;
    gpuPresentation.packed = face.packed;
    gpuPresentation.half = face.half;
    gpuPresentation.sourceSelectionState = projected.sourceSelectionState;
    gpuPresentation.sourceTraversalOrdinal = projected.sourceTraversalOrdinal;
    gpuPresentation.orderingTableBucket = projected.orderingTableBucket;
    gpuPresentation.sourceTraversalOrdinalValid =
        projected.sourceTraversalOrdinalValid;
    gpuPresentation.orderingTableBucketValid =
        projected.orderingTableBucketValid;
    gpuPresentation.sourceSelected = projected.sourceSelected;
    gpuPresentation.requiresProjectedFallback =
        requiresNearClip && !projected.gpuRawOnly;
    gpuPresentation.colorDecoded = projected.colorDecoded;
    gpuPresentation.gpuRawOnly = projected.gpuRawOnly;
    gpuPresentation.material = projected.material;
    gpuPresentationFaces.push_back(std::move(gpuPresentation));
    CachedResidentFace cachedFace;
    cachedFace.unconstructedMaterial = unconstructedMaterial;
    cachedFace.indices = face.indices;
    cachedFace.completeIndices = face.completeIndices;
    cachedFace.presentation = projected;
    if (reusableCachedFace != nullptr)
      *reusableCachedFace = std::move(cachedFace);
    else
      currentResourceCache[face.world].faces.push_back(std::move(cachedFace));
    if (auditPresentationFrustum && intersects) {
      if (projected.sourceSelected)
        ++sourceFacesInFrustum;
      else
        ++missingFacesInFrustum;
    }

    if (projected.gpuRawOnly) {
      if (requiresNearClip) {
        ++snapshot.nearPlaneClippedFaces;
        metrics::count("host.crash3.resident_sidecar.gpu_near_clip_face");
      }
      if (highestMaterial.has_value())
        appendSceneryDetailMaterial(
            textureDetails, projected,
            static_cast<uint32_t>(snapshot.faces.size()), *highestMaterial);
      snapshot.faces.push_back(std::move(projected));
      continue;
    }
    if (!requiresNearClip) {
      bool projectedValid = true;
      for (std::size_t corner = 0; corner < cameraVertices.size(); ++corner) {
        projectedValid &=
            projectVertex({static_cast<double>(cameraVertices[corner].x),
                           static_cast<double>(cameraVertices[corner].y),
                           static_cast<double>(cameraVertices[corner].z)},
                          projected.vertices[corner]);
      }
      if (!projectedValid)
        return false;
      if (highestMaterial.has_value())
        appendSceneryDetailMaterial(
            textureDetails, projected,
            static_cast<uint32_t>(snapshot.faces.size()), *highestMaterial);
      snapshot.faces.push_back(std::move(projected));
      continue;
    }

    ++snapshot.nearPlaneClippedFaces;
    const SceneryNearClipPolygon clipped =
        clipSceneryTriangleToNearPlane(cameraVertices, nearZ);
    if (clipped.count < 3u) {
      ++snapshot.nearPlaneRejectedFaces;
      continue;
    }
    for (uint8_t triangle = 1u; triangle + 1u < clipped.count; ++triangle) {
      ResidentPresentationFace output = projected;
      output.nearPlaneClipped = true;
      const std::array<uint8_t, 3> indices{
          {0u, triangle, static_cast<uint8_t>(triangle + 1u)}};
      bool projectedValid = true;
      for (std::size_t corner = 0; corner < indices.size(); ++corner) {
        const SceneryNearClipVertex &clippedVertex =
            clipped.vertices[indices[corner]];
        projectedValid &= projectVertex(clippedVertex, output.vertices[corner]);
        for (std::size_t component = 0; component < 2u; ++component) {
          output.material.uv[corner][component] = 0.0f;
          for (std::size_t source = 0;
               source < clippedVertex.barycentric.size(); ++source) {
            output.material.uv[corner][component] +=
                static_cast<float>(clippedVertex.barycentric[source] *
                                   projected.material.uv[source][component]);
          }
        }
        for (std::size_t component = 0; component < 3u; ++component) {
          output.color255[corner][component] = 0.0f;
          for (std::size_t source = 0;
               source < clippedVertex.barycentric.size(); ++source) {
            output.color255[corner][component] +=
                static_cast<float>(clippedVertex.barycentric[source] *
                                   projected.color255[source][component]);
          }
        }
      }
      if (!projectedValid) {
        ++snapshot.nearPlaneDeferredFaces;
        continue;
      }
      if (highestMaterial.has_value()) {
        SceneryClipWeights weights;
        for (std::size_t corner = 0u; corner < indices.size(); ++corner)
          weights[corner] = clipped.vertices[indices[corner]].barycentric;
        appendSceneryDetailMaterial(
            textureDetails, output,
            static_cast<uint32_t>(snapshot.faces.size()),
            clipSceneryDetailMaterial(*highestMaterial, weights));
      }
      snapshot.faces.push_back(std::move(output));
      ++snapshot.nearPlaneGeneratedFaces;
    }
  }
  finishCaptureStage("host.crash3.resident_sidecar.capture_face_plan_us");

  const uint64_t targetFrame = snapshot.frameNumber;
  const uint32_t frustumFaces = snapshot.presentationFrustumFaces;
  const uint32_t outsideFrustumFaces = snapshot.outsidePresentationFrustumFaces;
  const uint32_t nearClipped = snapshot.nearPlaneClippedFaces;
  const uint32_t nearGenerated = snapshot.nearPlaneGeneratedFaces;
  const uint32_t nearRejected = snapshot.nearPlaneRejectedFaces;
  const uint32_t nearDeferred = snapshot.nearPlaneDeferredFaces;
  const uint32_t decodedMaterials = snapshot.decodedMaterialFaces;
  const uint32_t unresolvedMaterials = snapshot.unresolvedMaterialFaces;
  const uint32_t decodedTextured = snapshot.decodedTexturedFaces;
  const uint32_t decodedDynamic = snapshot.decodedDynamicMaterialFaces;
  const uint32_t decodedScratchTailDynamic =
      snapshot.decodedScratchTailDynamicMaterialFaces;
  const uint32_t decodedSemi = snapshot.decodedSemiTransparentFaces;
  const uint32_t decodedOrderingTable = snapshot.decodedOrderingTableFaces;
  const uint32_t unresolvedOrderingTable =
      snapshot.unresolvedOrderingTableFaces;
  const uint32_t decodedColors = snapshot.decodedColorFaces;
  const uint32_t unresolvedColors = snapshot.unresolvedColorFaces;
  snapshot.resourceSetValidated =
      validGeometryWorlds == worldCount && resourceIdentitiesValid &&
      sourceEntriesValid && snapshot.placementValidated &&
      snapshot.decodedMaterialFaces == snapshot.residentFaceCount &&
      snapshot.unresolvedMaterialFaces == 0u &&
      snapshot.decodedOrderingTableFaces + gpuOrderingIndependentFaces ==
          snapshot.residentFaceCount &&
      snapshot.unresolvedOrderingTableFaces == 0u &&
      snapshot.decodedColorFaces == snapshot.residentFaceCount &&
      snapshot.unresolvedColorFaces == 0u;
  traceSceneryVariantPath(source, snapshot);
  selectSceneryResourceVariants(memory, source, snapshot, currentResourceCache,
                                textureDetails);
  if (!prepareResidentGpuSceneCapture(
          memory, cameraWorkspace, geometry, geometryValidity, worldCount,
          currentResourceCache, std::move(gpuPresentationFaces),
          staticOpaqueFrameActive, snapshot)) {
    snapshot.gpuScene.reset();
  }
  observeResidentSceneResources(snapshot, currentResourceCache);
  observeResidentCohort(snapshot, std::move(currentResourceCache));
  snapshot.sceneCacheResourceCount =
      static_cast<uint32_t>(g_residentCohortJournal.activeCache.size() +
                            g_residentCohortJournal.sceneCache.size());
  const auto cachedFaceCount = [](const auto &cache) {
    return std::accumulate(
        cache.begin(), cache.end(), uint64_t{0},
        [](uint64_t total, const CachedResidentResource &resource) {
          return total + resource.faces.size();
        });
  };
  snapshot.sceneCacheFaces = static_cast<uint32_t>(std::min<uint64_t>(
      cachedFaceCount(g_residentCohortJournal.activeCache) +
          cachedFaceCount(g_residentCohortJournal.sceneCache),
      std::numeric_limits<uint32_t>::max()));
  const std::vector<ResidentResourceKey> sceneKeys = [&] {
    std::vector<ResidentResourceKey> keys =
        g_residentCohortJournal.activeResources;
    keys.reserve(keys.size() + g_residentCohortJournal.sceneCache.size());
    for (const CachedResidentResource &resource :
         g_residentCohortJournal.sceneCache) {
      keys.push_back(residentResourceKey(resource.descriptor));
    }
    std::sort(keys.begin(), keys.end());
    return keys;
  }();
  for (ResidentPresentationResource &resource : snapshot.resources) {
    resource.sceneCacheMember = std::binary_search(
        sceneKeys.begin(), sceneKeys.end(), residentResourceKey(resource));
  }
  const bool cohortPresentationSucceeded = applyResidentCohortPresentation(
      memory, ctx, source, snapshot, textureDetails);
  if (snapshot.gpuScene.has_value()) {
    // The first ABI milestone carries the exact current resident table. During
    // a transactional handoff the final CPU oracle may additionally contain
    // retained or reprojected resources; keep that frame fail-closed until the
    // GPU capture itself contains the same complete cohort.
    snapshot.gpuScene->completeForPresentation &=
        cohortPresentationSucceeded && !snapshot.cohortPresentationApplied &&
        !snapshot.activeCohortReprojected &&
        snapshot.retainedResourceCount == 0u;
  }
  const bool gpuCohortRecovered =
      recoverResidentGpuSceneFromCertifiedCohort(snapshot);
  const uint32_t capturedFaces = static_cast<uint32_t>(snapshot.faces.size());
  const uint32_t retainedResources = snapshot.retainedResourceCount;
  const uint32_t retainedResourceFaces = snapshot.retainedResourceFaces;
  const uint32_t sceneCacheResources = snapshot.sceneCacheResourceCount;
  const uint32_t sceneCacheFaces = snapshot.sceneCacheFaces;
  const uint32_t individuallyValidatedResources =
      snapshot.individuallyValidatedResources;
  const uint32_t individuallyAdmittedResources =
      snapshot.individuallyAdmittedResources;
  const uint32_t individuallyRevokedResources =
      snapshot.individuallyRevokedResources;
  const uint64_t resourceSetFingerprint = snapshot.resourceSetFingerprint;
  const uint64_t activeCohortFingerprint = snapshot.activeCohortFingerprint;
  const uint32_t activeCohortGeneration = snapshot.activeCohortGeneration;
  const uint32_t candidateObservations = snapshot.candidateObservations;
  const bool resourceSetValidated = snapshot.resourceSetValidated;
  const bool placementValidated = snapshot.placementValidated;
  const bool resourceIdentityConflict = snapshot.resourceIdentityConflict;
  const bool cohortCandidate = snapshot.cohortCandidate;
  const bool cohortDisjoint = snapshot.cohortDisjoint;
  const bool cohortCommitted = snapshot.cohortCommitted;
  const bool activeCohortReprojected = snapshot.activeCohortReprojected;
  const bool candidateCohortPresented = snapshot.candidateCohortPresented;
  const bool cohortPresentationValid = snapshot.cohortPresentationValid;
  const bool cohortPresentationApplied = snapshot.cohortPresentationApplied;
  const uint32_t sharedActiveResources = snapshot.sharedActiveResourceCount;
  const uint32_t reprojectedActiveResources =
      snapshot.reprojectedActiveResourceCount;
  const uint32_t reprojectedActiveFaces =
      snapshot.reprojectedActiveResourceFaces;
  const bool gpuSceneAvailable = snapshot.gpuScene.has_value();
  const bool gpuSceneComplete =
      gpuSceneAvailable && snapshot.gpuScene->completeForPresentation;
  const uint32_t gpuRawVertices =
      gpuSceneAvailable ? snapshot.gpuScene->rawVertexCount : 0u;
  const uint32_t gpuRawFaces =
      gpuSceneAvailable ? snapshot.gpuScene->rawFaceCount : 0u;
  const uint32_t gpuReusedPayloads =
      gpuSceneAvailable ? snapshot.gpuScene->reusedPayloadCount : 0u;
  const uint32_t gpuRebuiltPayloads =
      gpuSceneAvailable ? snapshot.gpuScene->rebuiltPayloadCount : 0u;
  prepareResidentOrderingTableProof(snapshot, ctx);
  traceResidentProjectionCatalog(snapshot, memory, worlds, geometry,
                                 worldCount);
  metrics::setState("host.crash3.resident_sidecar.nearest_texture_overrides",
                    textureDetails.materials.size());
  metrics::setState("host.crash3.scenery_variants.selected_lower_faces",
                    std::accumulate(textureDetails.variants.begin(),
                                    textureDetails.variants.end(), uint64_t{0},
                                    [](uint64_t count, const auto &plan) {
                                      return count + plan->replacements.size();
                                    }));
  metrics::setState("host.crash3.scenery_variants.selected_refinement_faces",
                    std::accumulate(textureDetails.variants.begin(),
                                    textureDetails.variants.end(), uint64_t{0},
                                    [](uint64_t count, const auto &plan) {
                                      for (const auto &family :
                                           plan->refinements)
                                        count += family.lowerFaces.size();
                                      return count;
                                    }));
  publishResidentPresentationSnapshot(std::move(snapshot), std::move(textureDetails));
  finishCaptureStage("host.crash3.resident_sidecar.capture_cohort_publish_us");
  metrics::setState("host.crash3.resident_sidecar.target_frame",
                    static_cast<int64_t>(std::min<uint64_t>(
                        targetFrame, std::numeric_limits<int64_t>::max())));
  metrics::setState("host.crash3.resident_sidecar.capture_id", captureId);
  metrics::setState("host.crash3.resident_sidecar.resource_set_fingerprint",
                    std::bit_cast<int64_t>(resourceSetFingerprint));
  metrics::setState("host.crash3.resident_sidecar.active_cohort_fingerprint",
                    std::bit_cast<int64_t>(activeCohortFingerprint));
  metrics::setState("host.crash3.resident_sidecar.active_cohort_generation",
                    activeCohortGeneration);
  metrics::setState("host.crash3.resident_sidecar.candidate_observations",
                    candidateObservations);
  metrics::setState("host.crash3.resident_sidecar.shared_active_resources",
                    sharedActiveResources);
  metrics::setState("host.crash3.resident_sidecar.resource_set_validated",
                    resourceSetValidated ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.placement_validated",
                    placementValidated ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.resource_identity_conflict",
                    resourceIdentityConflict ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.cohort_candidate",
                    cohortCandidate ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.cohort_disjoint",
                    cohortDisjoint ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.cohort_committed",
                    cohortCommitted ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.cohort_presentation_applied",
                    cohortPresentationApplied ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.cohort_presentation_valid",
                    cohortPresentationValid ? 1 : 0);
  metrics::setState(
      "host.crash3.resident_sidecar.cohort_presentation_succeeded",
      cohortPresentationSucceeded ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.active_cohort_reprojected",
                    activeCohortReprojected ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.candidate_cohort_presented",
                    candidateCohortPresented ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.retained_resources",
                    retainedResources);
  metrics::setState("host.crash3.resident_sidecar.retained_resource_faces",
                    retainedResourceFaces);
  metrics::setState("host.crash3.resident_sidecar.scene_cache_resources",
                    sceneCacheResources);
  metrics::setState("host.crash3.resident_sidecar.scene_cache_faces",
                    sceneCacheFaces);
  metrics::setState(
      "host.crash3.resident_sidecar.individually_validated_resources",
      individuallyValidatedResources);
  metrics::setState(
      "host.crash3.resident_sidecar.individually_admitted_resources",
      individuallyAdmittedResources);
  metrics::setState(
      "host.crash3.resident_sidecar.individually_revoked_resources",
      individuallyRevokedResources);
  metrics::setState("host.crash3.resident_sidecar.reprojected_active_resources",
                    reprojectedActiveResources);
  metrics::setState("host.crash3.resident_sidecar.reprojected_active_faces",
                    reprojectedActiveFaces);
  metrics::setState("host.crash3.resident_sidecar.gpu_scene_available",
                    gpuSceneAvailable ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.gpu_scene_complete",
                    gpuSceneComplete ? 1 : 0);
  metrics::setState(
      "host.crash3.resident_sidecar.gpu_certified_cohort_recovery",
      gpuCohortRecovered ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.gpu_raw_vertices",
                    gpuRawVertices);
  metrics::setState("host.crash3.resident_sidecar.gpu_raw_faces", gpuRawFaces);
  metrics::setState("host.crash3.resident_sidecar.gpu_payloads_reused",
                    gpuReusedPayloads);
  metrics::setState("host.crash3.resident_sidecar.gpu_payloads_rebuilt",
                    gpuRebuiltPayloads);
  metrics::setState("host.crash3.resident_sidecar.static_opaque_plan_requested",
                    staticOpaquePlanRequested ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.static_opaque_plan_permitted",
                    staticOpaquePlanPermitted ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.static_opaque_faces_reused",
                    staticOpaqueFacesReused);
  metrics::setState(
      "host.crash3.resident_sidecar.gpu_ordering_independent_faces",
      gpuOrderingIndependentFaces);
  if (staticOpaqueFacesReused != 0u) {
    metrics::count(
        "host.crash3.resident_sidecar.static_opaque_face_cpu_work_elided",
        staticOpaqueFacesReused);
  }
  metrics::setState("host.crash3.resident_sidecar.faces", capturedFaces);
  metrics::setState("host.crash3.resident_sidecar.vertex_deformation_active",
                    vertexDeformationActive ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.triangle_postprocessor",
                    cameraWorkspace.deformation.trianglePostprocessor);
  metrics::setState("host.crash3.resident_sidecar.quad_postprocessor",
                    cameraWorkspace.deformation.quadPostprocessor);
  metrics::setState("host.crash3.resident_sidecar.distance_cue_active",
                    cameraWorkspace.deformation.distanceCued ? 1 : 0);
  metrics::setState("host.crash3.resident_sidecar.deformed_vertices",
                    stats.deformedVertices);
  metrics::setState("host.crash3.resident_sidecar.presentation_frustum_faces",
                    frustumFaces);
  metrics::setState(
      "host.crash3.resident_sidecar.outside_presentation_frustum_faces",
      outsideFrustumFaces);
  metrics::setState("host.crash3.resident_sidecar.missing_faces",
                    missingFacesInFrustum);
  metrics::setState("host.crash3.resident_sidecar.near_clipped_faces",
                    nearClipped);
  metrics::setState("host.crash3.resident_sidecar.near_generated_faces",
                    nearGenerated);
  metrics::setState("host.crash3.resident_sidecar.near_rejected_faces",
                    nearRejected);
  metrics::setState("host.crash3.resident_sidecar.near_deferred_faces",
                    nearDeferred);
  metrics::setState("host.crash3.resident_sidecar.material_decoded_faces",
                    decodedMaterials);
  metrics::setState("host.crash3.resident_sidecar.material_unresolved_faces",
                    unresolvedMaterials);
  metrics::setState(
      "host.crash3.resident_sidecar.material_unresolved_presentation_"
      "frustum_faces",
      unresolvedMaterialsInFrustum);
  metrics::setState("host.crash3.resident_sidecar.material_unresolved_outside_"
                    "presentation_frustum_faces",
                    unresolvedMaterialsOutsideFrustum);
  metrics::setState(
      "host.crash3.resident_sidecar.dynamic_material_missing_lod_shift",
      stats.dynamicMaterialMissingLodShift);
  metrics::setState(
      "host.crash3.resident_sidecar.dynamic_material_depth_unavailable",
      stats.dynamicMaterialDepthUnavailable);
  metrics::setState(
      "host.crash3.resident_sidecar.dynamic_material_lod_index_out_of_range",
      stats.dynamicMaterialLodIndexOutOfRange);
  metrics::setState(
      "host.crash3.resident_sidecar.dynamic_material_scratch_tail_reads",
      stats.dynamicMaterialScratchTailReads);
  metrics::setState(
      "host.crash3.resident_sidecar.dynamic_material_maximum_lod_index",
      stats.dynamicMaterialMaximumLodIndex);
  metrics::setState(
      "host.crash3.resident_sidecar.dynamic_material_missing_remap",
      stats.dynamicMaterialMissingRemap);
  metrics::setState(
      "host.crash3.resident_sidecar.dynamic_material_descriptor_unavailable",
      stats.dynamicMaterialDescriptorUnavailable);
  metrics::setState("host.crash3.resident_sidecar.material_textured_faces",
                    decodedTextured);
  metrics::setState("host.crash3.resident_sidecar.material_dynamic_faces",
                    decodedDynamic);
  metrics::setState(
      "host.crash3.resident_sidecar.material_dynamic_scratch_tail_faces",
      decodedScratchTailDynamic);
  metrics::setState("host.crash3.resident_sidecar.material_semi_faces",
                    decodedSemi);
  metrics::setState("host.crash3.resident_sidecar.ot_bucket_decoded_faces",
                    decodedOrderingTable);
  metrics::setState("host.crash3.resident_sidecar.ot_bucket_unresolved_faces",
                    unresolvedOrderingTable);
  metrics::setState("host.crash3.resident_sidecar.color_decoded_faces",
                    decodedColors);
  metrics::setState("host.crash3.resident_sidecar.color_unresolved_faces",
                    unresolvedColors);
  if (metrics::traceEnabled()) {
    metrics::trace(
        "game.crash3.resident_presentation_snapshot",
        {{"construction_frame",
          static_cast<int64_t>(std::min<uint64_t>(
              constructionFrame, std::numeric_limits<int64_t>::max()))},
         {"target_frame",
          static_cast<int64_t>(std::min<uint64_t>(
              targetFrame, std::numeric_limits<int64_t>::max()))},
         {"capture_id", captureId},
         {"list_address", listAddress},
         {"source_entries", static_cast<int64_t>(source.size())},
         {"source_faces", sceneryFaceCount(source)},
         {"resident_faces", residentFaceCount},
         {"captured_faces", capturedFaces},
         {"triangle_postprocessor",
          cameraWorkspace.deformation.trianglePostprocessor},
         {"quad_postprocessor", cameraWorkspace.deformation.quadPostprocessor},
         {"triangle_constructor", cameraWorkspace.construction.triangleProgram},
         {"quad_constructor", cameraWorkspace.construction.quadProgram},
         {"primitive_linker", memory.read32(kRendererScratchBase + 112u)},
         {"constructor_lighting_active", cameraWorkspace.construction.twoPointLighting},
         {"light_0_xy", cameraWorkspace.construction.points[0].packedXy},
         {"light_0_z", cameraWorkspace.construction.points[0].z},
         {"light_1_xy", cameraWorkspace.construction.points[1].packedXy},
         {"light_1_z", cameraWorkspace.construction.points[1].z},
         {"far_red", cameraWorkspace.farColor[0]},
         {"far_green", cameraWorkspace.farColor[1]},
         {"far_blue", cameraWorkspace.farColor[2]},
         {"distance_cue_active",
          cameraWorkspace.deformation.distanceCued ? 1u : 0u},
         {"vertex_deformation_active", vertexDeformationActive ? 1u : 0u},
         {"deformed_vertices", stats.deformedVertices},
         {"presentation_frustum_faces", frustumFaces},
         {"outside_presentation_frustum_faces", outsideFrustumFaces},
         {"source_faces_in_frustum", sourceFacesInFrustum},
         {"missing_faces_in_frustum", missingFacesInFrustum},
         {"near_plane_clipped_faces", nearClipped},
         {"near_plane_generated_faces", nearGenerated},
         {"near_plane_rejected_faces", nearRejected},
         {"near_plane_deferred_faces", nearDeferred},
         {"material_decoded_faces", decodedMaterials},
         {"material_unresolved_faces", unresolvedMaterials},
         {"material_unresolved_presentation_frustum_faces",
          unresolvedMaterialsInFrustum},
         {"material_unresolved_outside_presentation_frustum_faces",
          unresolvedMaterialsOutsideFrustum},
         {"dynamic_material_missing_lod_shift",
          stats.dynamicMaterialMissingLodShift},
         {"dynamic_material_depth_unavailable",
          stats.dynamicMaterialDepthUnavailable},
         {"dynamic_material_lod_index_out_of_range",
          stats.dynamicMaterialLodIndexOutOfRange},
         {"dynamic_material_scratch_tail_reads",
          stats.dynamicMaterialScratchTailReads},
         {"dynamic_material_maximum_lod_index",
          stats.dynamicMaterialMaximumLodIndex},
         {"dynamic_material_missing_remap", stats.dynamicMaterialMissingRemap},
         {"dynamic_material_descriptor_unavailable",
          stats.dynamicMaterialDescriptorUnavailable},
         {"material_textured_faces", decodedTextured},
         {"material_dynamic_faces", decodedDynamic},
         {"material_dynamic_scratch_tail_faces", decodedScratchTailDynamic},
         {"material_semi_faces", decodedSemi},
         {"ot_bucket_decoded_faces", decodedOrderingTable},
         {"ot_bucket_unresolved_faces", unresolvedOrderingTable},
         {"color_decoded_faces", decodedColors},
         {"color_unresolved_faces", unresolvedColors}});
  }
  finishCaptureStage("host.crash3.resident_sidecar.capture_metrics_publish_us");
  return true;
}

bool filterSceneryUnionByPresentationFrustum(
    const Memory &memory, const CPUContext &ctx, uint32_t listAddress,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    uint32_t worldCount, bool includeResidentGeometry,
    SceneryGeometryStats &stats) {
  stats.cullGuardPixels = sceneryCullGuardPixels();
  std::vector<uint16_t> current;
  if (!readDecodedSceneryList(memory, listAddress, current))
    return false;
  stats.sourceUnionEntries =
      static_cast<uint32_t>(g_scenerySelection.entries.size());

  std::array<ResidentSceneryGeometry, kMaxSceneryWorlds> geometry{};
  if (!readResidentSceneryGeometry(memory, worlds, worldCount, geometry, stats))
    return false;
  static SceneryCameraWorkspace cameraWorkspace;
  prepareSceneryCameraWorkspace(ctx, geometry, worldCount, cameraWorkspace);

  constexpr uint8_t kTrianglePresent = 4u;
  std::vector<uint8_t> originalFaces(kSceneryIdentityCount);
  for (const uint16_t packed : current) {
    const uint32_t identity = sceneryEntryIdentity(packed);
    const uint32_t state = (packed >> 11u) & 3u;
    originalFaces[identity] =
        static_cast<uint8_t>(state == 0u ? kTrianglePresent : state);
  }
  std::vector<uint8_t> sourceFaces(kSceneryIdentityCount);
  for (const uint16_t packed : g_scenerySelection.entries) {
    const uint32_t identity = sceneryEntryIdentity(packed);
    const uint32_t state = (packed >> 11u) & 3u;
    sourceFaces[identity] =
        static_cast<uint8_t>(state == 0u ? kTrianglePresent : state);
  }

  std::vector<uint16_t> filtered;
  filtered.reserve(g_scenerySelection.entries.size());
  bool valid = true;
  static const bool auditOrientation = [] {
    const char *value = std::getenv("PS1_TRACE_CRASH3_SCENERY_ORIENTATION");
    return value != nullptr && *value != '\0' && *value != '0';
  }();
  const auto countOrientation = [](int32_t orientation, uint32_t &positive,
                                   uint32_t &negative, uint32_t &degenerate) {
    if (orientation > 0)
      ++positive;
    else if (orientation < 0)
      ++negative;
    else
      ++degenerate;
  };
  for (const uint16_t packed : g_scenerySelection.entries) {
    const uint32_t world = packed >> 13u;
    const uint32_t state = (packed >> 11u) & 3u;
    const uint32_t polygonId = packed & 0x7ffu;
    if (world >= worldCount) {
      stats.failure = 6u;
      stats.failureWorld = world;
      valid = false;
      break;
    }
    const uint32_t identity = sceneryEntryIdentity(packed);
    const uint8_t original = originalFaces[identity];
    if (state == 0u) {
      if ((original & kTrianglePresent) != 0u) {
        if (auditOrientation) {
          std::array<uint32_t, 3> indices{};
          sceneryFaceIndices(memory, geometry[world], polygonId, 0u, indices);
          int32_t orientation = 0;
          (void)sceneryFaceIntersectsPresentationFrustum(
              memory, geometry[world], world, cameraWorkspace, stats, indices,
              valid, &orientation);
          countOrientation(orientation, stats.originalPositive,
                           stats.originalNegative, stats.originalDegenerate);
          if (!valid) {
            stats.failure = 7u;
            break;
          }
        }
        filtered.push_back(packed);
        continue;
      }
      ++stats.testedFaces;
      std::array<uint32_t, 3> indices{};
      sceneryFaceIndices(memory, geometry[world], polygonId, 0u, indices);
      int32_t orientation = 0;
      if (sceneryFaceIntersectsPresentationFrustum(
              memory, geometry[world], world, cameraWorkspace, stats, indices,
              valid, auditOrientation ? &orientation : nullptr)) {
        filtered.push_back(packed);
        ++stats.retainedFaces;
        if (orientation > 0)
          ++stats.retainedPositive;
        else if (orientation < 0)
          ++stats.retainedNegative;
      } else {
        ++stats.rejectedFaces;
      }
      if (auditOrientation)
        countOrientation(orientation, stats.addedPositive, stats.addedNegative,
                         stats.addedDegenerate);
      if (!valid)
        stats.failure = 7u;
      if (!valid)
        break;
      continue;
    }

    uint32_t retainedState = original & 3u;
    const uint32_t candidateState = includeResidentGeometry ? 3u : state;
    for (uint32_t half = 1u; half <= 2u; ++half) {
      if ((candidateState & half) == 0u)
        continue;
      const bool residentOnly = (state & half) == 0u;
      if ((retainedState & half) != 0u) {
        if (auditOrientation) {
          std::array<uint32_t, 3> indices{};
          sceneryFaceIndices(memory, geometry[world], polygonId, half, indices);
          int32_t orientation = 0;
          (void)sceneryFaceIntersectsPresentationFrustum(
              memory, geometry[world], world, cameraWorkspace, stats, indices,
              valid, &orientation);
          countOrientation(orientation, stats.originalPositive,
                           stats.originalNegative, stats.originalDegenerate);
          if (!valid) {
            stats.failure = 7u;
            break;
          }
        }
        continue;
      }
      ++stats.testedFaces;
      if (residentOnly)
        ++stats.residentQuadHalvesTested;
      std::array<uint32_t, 3> indices{};
      sceneryFaceIndices(memory, geometry[world], polygonId, half, indices);
      int32_t orientation = 0;
      if (sceneryFaceIntersectsPresentationFrustum(
              memory, geometry[world], world, cameraWorkspace, stats, indices,
              valid, auditOrientation ? &orientation : nullptr)) {
        retainedState |= half;
        ++stats.retainedFaces;
        if (residentOnly)
          ++stats.residentQuadHalvesRetained;
        if (orientation > 0)
          ++stats.retainedPositive;
        else if (orientation < 0)
          ++stats.retainedNegative;
      } else {
        ++stats.rejectedFaces;
        if (residentOnly)
          ++stats.residentQuadHalvesRejected;
      }
      if (auditOrientation)
        countOrientation(orientation, stats.addedPositive, stats.addedNegative,
                         stats.addedDegenerate);
      if (!valid)
        stats.failure = 7u;
      if (!valid)
        break;
    }
    if (!valid)
      break;
    if (retainedState != 0u) {
      filtered.push_back(static_cast<uint16_t>((packed & ~(3u << 11u)) |
                                               (retainedState << 11u)));
    }
  }
  if (!valid)
    return false;

  if (includeResidentGeometry) {
    if (!ensureResidentSceneryTopology(memory, worlds, geometry, worldCount,
                                       stats)) {
      stats.failure = 9u;
      return false;
    }

    const auto &residentFaces = g_residentSceneryTopology.faces;
    constexpr uint32_t kNoComponent = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> parent(residentFaces.size(), kNoComponent);
    std::vector<uint8_t> rank(residentFaces.size(), 0u);
    std::vector<uint8_t> touchesSideBand(residentFaces.size(), 0u);
    std::vector<uint8_t> insideGuardOnly(residentFaces.size(), 0u);
    std::vector<uint8_t> testedIdentities(kSceneryIdentityCount, 0u);
    const bool retainAllFrustumComponents = [] {
      const char *value =
          std::getenv("PS1_CRASH3_RESIDENT_ALL_FRUSTUM_COMPONENTS");
      return value != nullptr && *value != '\0' && *value != '0';
    }();
    stats.residentAllFrustum = retainAllFrustumComponents ? 1u : 0u;

    // Candidate components are induced only by source-absent faces that are
    // inside the current presentation frustum. A selected source face or a
    // face outside the frustum is therefore a hard boundary: connectivity can
    // complete a side object or floor strip, but cannot flood through the
    // already-authoritative centre of the scene or around the back of a world.
    for (uint32_t faceIndex = 0; faceIndex < residentFaces.size();
         ++faceIndex) {
      const ResidentSceneryFace &face = residentFaces[faceIndex];
      const uint32_t identity = sceneryEntryIdentity(face.packed);
      if (sourceFaces[identity] != 0u)
        continue;
      if (testedIdentities[identity] == 0u) {
        testedIdentities[identity] = 1u;
        if (face.half == 0u)
          ++stats.residentTrianglesTested;
        else
          ++stats.residentQuadsTested;
      }
      ++stats.testedFaces;
      if (face.half != 0u)
        ++stats.residentAbsentQuadHalvesTested;

      bool touchesNativeHorizontalBoundary = false;
      bool insideHorizontalGuardOnly = false;
      const bool intersectsPresentation =
          sceneryFaceIntersectsPresentationFrustum(
              memory, geometry[face.world], face.world, cameraWorkspace, stats,
              face.indices, valid, nullptr, &touchesNativeHorizontalBoundary,
              &insideHorizontalGuardOnly);
      if (!valid) {
        stats.failure = 7u;
        return false;
      }
      if (!intersectsPresentation)
        continue;
      ++stats.residentFrustumFaces;
      if (insideHorizontalGuardOnly)
        ++stats.residentGuardBandFaces;
      parent[faceIndex] = faceIndex;
      touchesSideBand[faceIndex] = touchesNativeHorizontalBoundary ? 1u : 0u;
      insideGuardOnly[faceIndex] = insideHorizontalGuardOnly ? 1u : 0u;
    }

    const auto findRoot = [&](uint32_t value) {
      uint32_t root = value;
      while (parent[root] != root)
        root = parent[root];
      while (parent[value] != value) {
        const uint32_t next = parent[value];
        parent[value] = root;
        value = next;
      }
      return root;
    };
    const auto join = [&](uint32_t left, uint32_t right) {
      uint32_t leftRoot = findRoot(left);
      uint32_t rightRoot = findRoot(right);
      if (leftRoot == rightRoot)
        return;
      if (rank[leftRoot] < rank[rightRoot])
        std::swap(leftRoot, rightRoot);
      parent[rightRoot] = leftRoot;
      if (rank[leftRoot] == rank[rightRoot])
        ++rank[leftRoot];
    };
    for (const auto &edge : g_residentSceneryTopology.sharedEdges) {
      if (parent[edge[0]] != kNoComponent && parent[edge[1]] != kNoComponent)
        join(edge[0], edge[1]);
    }

    std::vector<uint8_t> componentHasSideBand(residentFaces.size(), 0u);
    for (uint32_t faceIndex = 0; faceIndex < residentFaces.size();
         ++faceIndex) {
      if (parent[faceIndex] != kNoComponent &&
          (touchesSideBand[faceIndex] != 0u || retainAllFrustumComponents))
        componentHasSideBand[findRoot(faceIndex)] = 1u;
    }
    std::vector<uint8_t> componentSeen(residentFaces.size(), 0u);
    std::vector<uint8_t> admittedStates(kSceneryIdentityCount, 0u);
    static const bool traceResidentFaces = [] {
      const char *value = std::getenv("PS1_TRACE_CRASH3_SCENERY_FACE_CATALOG");
      return value != nullptr && *value != '\0' && *value != '0';
    }();
    for (uint32_t faceIndex = 0; faceIndex < residentFaces.size();
         ++faceIndex) {
      const ResidentSceneryFace &face = residentFaces[faceIndex];
      const uint32_t identity = sceneryEntryIdentity(face.packed);
      const bool sourceEmitted = sourceFaces[identity] != 0u;

      bool retained = false;
      if (!sourceEmitted && parent[faceIndex] != kNoComponent) {
        const uint32_t root = findRoot(faceIndex);
        if (componentSeen[root] == 0u) {
          componentSeen[root] = 1u;
          ++stats.residentCandidateComponents;
          if (componentHasSideBand[root] != 0u)
            ++stats.residentRetainedComponents;
        }
        retained = componentHasSideBand[root] != 0u;
      }
      if (traceResidentFaces && metrics::traceEnabled()) {
        const SceneryCameraVertex &v0 =
            cameraWorkspace.vertices[face.world][face.indices[0]];
        const SceneryCameraVertex &v1 =
            cameraWorkspace.vertices[face.world][face.indices[1]];
        const SceneryCameraVertex &v2 =
            cameraWorkspace.vertices[face.world][face.indices[2]];
        metrics::trace(
            "game.crash3.scenery_resident_face",
            {{"face_index", faceIndex},
             {"packed", face.packed},
             {"world", face.world},
             {"state", face.half},
             {"polygon_id", face.packed & 0x7ffu},
             {"v0_index", face.indices[0]},
             {"v1_index", face.indices[1]},
             {"v2_index", face.indices[2]},
             {"source_emitted", sourceEmitted ? 1u : 0u},
             {"in_presentation_frustum",
              parent[faceIndex] != kNoComponent ? 1u : 0u},
             {"in_horizontal_guard_only", insideGuardOnly[faceIndex]},
             {"touches_native_horizontal_boundary", touchesSideBand[faceIndex]},
             {"component_retained", retained ? 1u : 0u},
             {"projection_plane", cameraWorkspace.transform.projectionPlane},
             {"v0_x", v0.x},
             {"v0_y", v0.y},
             {"v0_z", v0.z},
             {"v1_x", v1.x},
             {"v1_y", v1.y},
             {"v1_z", v1.z},
             {"v2_x", v2.x},
             {"v2_y", v2.y},
             {"v2_z", v2.z}});
      }
      if (sourceEmitted)
        continue;
      if (retained) {
        admittedStates[identity] |=
            face.half == 0u ? kTrianglePresent : face.half;
        ++stats.retainedFaces;
        if (touchesSideBand[faceIndex] != 0u)
          ++stats.residentSeedFaces;
        else
          ++stats.residentClosureFaces;
        if (face.half == 0u)
          ++stats.residentTrianglesRetained;
        else
          ++stats.residentAbsentQuadHalvesRetained;
      } else {
        ++stats.rejectedFaces;
        if (face.half == 0u)
          ++stats.residentTrianglesRejected;
        else
          ++stats.residentAbsentQuadHalvesRejected;
      }
    }

    std::vector<uint16_t> residentAdditions;
    // Store additions in the same reverse world/class/id traversal used by
    // the checked total-resident builder. func_8004103C consumes the list in
    // reverse, while its normal AVSZ/OT path still owns final packet order.
    for (uint32_t world = worldCount; world-- > 0u;) {
      for (uint32_t quad = worlds[world].quadCount; quad-- > 0u;) {
        const uint16_t identityEntry =
            static_cast<uint16_t>((world << 13u) | (1u << 11u) | quad);
        if (sourceFaces[sceneryEntryIdentity(identityEntry)] != 0u)
          continue;
        const uint32_t identity = sceneryEntryIdentity(identityEntry);
        const uint32_t retainedState = admittedStates[identity] & 3u;
        if (retainedState != 0u) {
          residentAdditions.push_back(static_cast<uint16_t>(
              (world << 13u) | (retainedState << 11u) | quad));
          ++stats.residentQuadsRetained;
        } else {
          ++stats.residentQuadsRejected;
        }
      }

      for (uint32_t triangle = worlds[world].triangleCount; triangle-- > 0u;) {
        const uint16_t packed =
            static_cast<uint16_t>((world << 13u) | triangle);
        if (sourceFaces[sceneryEntryIdentity(packed)] != 0u)
          continue;
        if ((admittedStates[sceneryEntryIdentity(packed)] & kTrianglePresent) !=
            0u) {
          residentAdditions.push_back(packed);
        }
      }
    }

    const uint64_t extendedCount =
        static_cast<uint64_t>(filtered.size()) + residentAdditions.size();
    if (extendedCount > kMaxDecodedSceneryEntries) {
      stats.failure = 8u;
      stats.expected = kMaxDecodedSceneryEntries;
      stats.observed = static_cast<uint32_t>(extendedCount);
      return false;
    }
    stats.residentEntriesAdded =
        static_cast<uint32_t>(residentAdditions.size());
    // The guest consumes this stored list backwards and prepends each packet
    // to its AVSZ-selected OT bucket. Keeping resident-only extensions before
    // the source list therefore makes them enter a tied bucket last and draw
    // first; source-authored geometry remains authoritative in front. Appending
    // here reverses that tie and lets an inferred background face overwrite
    // an original foreground face despite equal coarse PS1 depth.
    filtered.insert(filtered.begin(), residentAdditions.begin(),
                    residentAdditions.end());
  }

  stats.retainedEntries = static_cast<uint32_t>(filtered.size());
  g_scenerySelection.entries = std::move(filtered);
  return g_scenerySelection.entries != current;
}

void traceSceneryCoverage(
    const Memory &memory, uint32_t listAddress, int32_t originalCount,
    ScenerySelectionMode mode,
    const std::array<ResidentSceneryWorld, kMaxSceneryWorlds> &worlds,
    uint32_t worldCount, uint32_t residentEntryCount,
    uint32_t residentFaceCount, bool residentValid) {
  if (!metrics::traceEnabled())
    return;

  uint64_t listHash = UINT64_C(1469598103934665603);
  hash32(listHash, static_cast<uint32_t>(originalCount));
  uint32_t invalidEntries = 0;
  uint32_t selectedFaces = 0;
  std::array<std::vector<uint8_t>, kMaxSceneryWorlds> triangles;
  std::array<std::vector<uint8_t>, kMaxSceneryWorlds> quads;
  if (residentValid) {
    for (uint32_t world = 0; world < worldCount; ++world) {
      triangles[world].resize(worlds[world].triangleCount);
      quads[world].resize(worlds[world].quadCount);
    }
  }

  const bool listValid =
      originalCount >= 0 &&
      isRamRange(listAddress, 4u + static_cast<uint32_t>(originalCount) * 2u);
  if (listValid) {
    for (int32_t index = 0; index < originalCount; ++index) {
      const uint16_t packed =
          memory.read16(listAddress + 4u + static_cast<uint32_t>(index) * 2u);
      hash32(listHash, packed);
      const uint32_t world = packed >> 13u;
      const uint32_t state = (packed >> 11u) & 3u;
      const uint32_t id = packed & 0x7ffu;
      if (!residentValid || world >= worldCount) {
        ++invalidEntries;
      } else if (state == 0) {
        if (id < triangles[world].size())
          triangles[world][id] = 1u;
        else
          ++invalidEntries;
      } else if (id < quads[world].size()) {
        quads[world][id] |= static_cast<uint8_t>(state);
      } else {
        ++invalidEntries;
      }
    }
  } else {
    ++invalidEntries;
  }

  if (residentValid) {
    for (uint32_t world = 0; world < worldCount; ++world) {
      uint32_t worldSelectedFaces = 0;
      for (const uint8_t selected : triangles[world])
        worldSelectedFaces += selected != 0;
      for (const uint8_t selected : quads[world])
        worldSelectedFaces +=
            std::popcount(static_cast<unsigned>(selected & 3u));
      selectedFaces += worldSelectedFaces;
      metrics::trace("game.crash3.scenery_world_coverage",
                     {{"world", world},
                      {"info_address", worlds[world].infoAddress},
                      {"resident_triangles", worlds[world].triangleCount},
                      {"resident_quads", worlds[world].quadCount},
                      {"resident_faces", worlds[world].triangleCount +
                                             2u * worlds[world].quadCount},
                      {"slst_selected_faces", worldSelectedFaces},
                      {"slst_missing_faces", worlds[world].triangleCount +
                                                 2u * worlds[world].quadCount -
                                                 worldSelectedFaces}});
    }
  }

  metrics::trace(
      "game.crash3.scenery_selection",
      {{"mode", mode == ScenerySelectionMode::Bypass              ? 2u
                : mode == ScenerySelectionMode::Adjacent          ? 3u
                : mode == ScenerySelectionMode::Symmetric         ? 4u
                : mode == ScenerySelectionMode::Geometric         ? 5u
                : mode == ScenerySelectionMode::GeometricResident ? 6u
                                                                  : 1u},
       {"list_address", listAddress},
       {"list_hash", static_cast<int64_t>(listHash)},
       {"slst_entries", originalCount},
       {"resident_valid", residentValid ? 1u : 0u},
       {"resident_worlds", worldCount},
       {"resident_entries", residentEntryCount},
       {"resident_faces", residentFaceCount},
       {"slst_selected_faces", selectedFaces},
       {"slst_missing_faces",
        residentValid ? residentFaceCount - selectedFaces : 0u},
       {"invalid_entries", invalidEntries}});
}

void traceSceneryEntryCatalog(const Memory &memory, uint32_t listAddress,
                              int32_t originalCount, bool virtualActive) {
  const char *requested = std::getenv("PS1_TRACE_CRASH3_SCENERY_ENTRY_CATALOG");
  if (!metrics::traceEnabled() || requested == nullptr || *requested == '\0' ||
      *requested == '0') {
    return;
  }

  std::vector<uint16_t> original;
  if (!readDecodedSceneryList(memory, listAddress, original))
    return;
  std::vector<uint8_t> originalStates(kSceneryIdentityCount, 0u);
  for (const uint16_t packed : original) {
    const uint32_t identity = sceneryEntryIdentity(packed);
    const uint32_t state = (packed >> 11u) & 3u;
    originalStates[identity] = static_cast<uint8_t>(state == 0u ? 4u : state);
  }

  const std::vector<uint16_t> &entries =
      virtualActive ? g_scenerySelection.entries : original;
  metrics::trace("game.crash3.scenery_entry_catalog",
                 {{"list_address", listAddress},
                  {"original_entries", originalCount},
                  {"presented_entries", static_cast<int64_t>(entries.size())},
                  {"virtual_active", virtualActive ? 1u : 0u}});
  for (std::size_t storedIndex = 0; storedIndex < entries.size();
       ++storedIndex) {
    const uint16_t packed = entries[storedIndex];
    const uint32_t state = (packed >> 11u) & 3u;
    const uint32_t identity = sceneryEntryIdentity(packed);
    metrics::trace(
        "game.crash3.scenery_entry",
        {{"list_address", listAddress},
         {"stored_index", static_cast<int64_t>(storedIndex)},
         {"decoder_ordinal",
          static_cast<int64_t>(entries.size() - storedIndex - 1u)},
         {"packed", packed},
         {"world", packed >> 13u},
         {"polygon_class", state == 0u ? 0u : 1u},
         {"state", state},
         {"polygon_id", packed & 0x7ffu},
         {"original_state", originalStates[identity]},
         {"resident_added", originalStates[identity] == 0u ? 1u : 0u}});
  }
}

} // namespace

void configure(const ProfileOptions &options) noexcept {
  g_options = options;
  constexpr uint32_t kMaxSceneryCullGuardPixels = 2048u;
  g_options.sceneryCullGuardPixels =
      std::min(g_options.sceneryCullGuardPixels, kMaxSceneryCullGuardPixels);
  if (const char *value = std::getenv("PS1_CRASH3_SCENERY_GUARD_PIXELS");
      value != nullptr && *value != '\0') {
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 0);
    if (end != value && *end == '\0') {
      g_options.sceneryCullGuardPixels = static_cast<uint32_t>(
          std::min<unsigned long>(parsed, kMaxSceneryCullGuardPixels));
    }
  }
  g_sceneryOcclusionMode = configuredSceneryOcclusionMode();
  g_residentPresentationMode = configuredResidentPresentationMode();
  if (const char *value = std::getenv("PS1_CRASH3_RESIDENT_COHORT_PRESENT")) {
    g_residentCohortPresentationEnabled = *value != '\0' && *value != '0' &&
                                          std::strcmp(value, "off") != 0 &&
                                          std::strcmp(value, "audit") != 0;
  } else {
    g_residentCohortPresentationEnabled = true;
  }
  g_objectPresentationMode = configuredObjectPresentationMode();
  g_presentationCullStrategy = configuredPresentationCullStrategy();
  const char *traceObjects = std::getenv("PS1_TRACE_CRASH3_OBJECT_CATALOG");
  g_traceObjectPresentation =
      traceObjects != nullptr && *traceObjects != '\0' && *traceObjects != '0';
  metrics::setState("host.crash3.presentation_cull.strategy",
                    static_cast<int64_t>(g_presentationCullStrategy));
  metrics::setState("host.crash3.presentation_cull.guard_pixels",
                    presentationCullGuardPixels());
  metrics::setState("host.crash3.object_presentation.mode",
                    static_cast<int64_t>(g_objectPresentationMode));
  metrics::setState("host.crash3.resident_sidecar.cohort_presentation_enabled",
                    g_residentCohortPresentationEnabled ? 1 : 0);
  g_rendererScaleShift = -1;
  g_rendererObjectSource = 0;
  g_rendererObjectCaptureId = 0;
  g_rendererLogicalObject = 0;
  g_rendererOrderingTableBasesCaptured = false;
  g_rendererRotationCaptureNext = kNoRendererRotationCapture;
  g_demoLevelLoadGeneration.store(0u, std::memory_order_relaxed);
  g_demoLevelLoadOrdinal.store(0u, std::memory_order_relaxed);
  g_demoLevelCode.store(0u, std::memory_order_relaxed);
  clearProjectedBillboardCentres();
  g_scenerySelection = {};
  g_sceneryDeltaHistory = {};
  g_sceneryPathCache = {};
  g_sceneryProjectionIdentity = {};
  g_pendingResidentPresentationCapture = {};
  g_residentSceneryTopology = {};
  g_residentCohortJournal = {};
  g_residentCurrentCacheScratch.clear();
  g_sceneryVariantResources.clear();
  clearResidentPresentationSnapshots();
  {
    std::scoped_lock lock(g_residentOrderingTableProofMutex);
    g_residentOrderingTableProof = {};
    g_residentOrderingTableProofCaptures.clear();
  }
  clearObjectPresentationSnapshots();
}

const ProfileOptions &options() noexcept { return g_options; }

bool active() noexcept { return g_options.enabled; }

bool widescreen16x9() noexcept {
  return g_options.enabled && g_options.widescreen16x9;
}

bool presentationDepthCorrections() noexcept {
  return g_options.enabled && g_options.presentationDepthCorrections;
}

uint32_t sceneryCullGuardPixels() noexcept {
  // The ordinary resident sidecar remains observational. The explicit guard
  // strategy is different: it widens only short-lived visibility witnesses
  // after packet-bound values have been preserved. Neither mode changes the
  // architectural GTE FIFO, camera, final coordinates or Vulkan viewport.
  const bool guestRendererGuard =
      g_residentPresentationMode == ResidentPresentationMode::Off;
  const bool hostAdmissionGuard =
      g_residentPresentationMode != ResidentPresentationMode::Off &&
      g_presentationCullStrategy == PresentationCullStrategy::GuardBand;
  return widescreen16x9() && (guestRendererGuard || hostAdmissionGuard)
             ? g_options.sceneryCullGuardPixels
             : 0u;
}

ResidentPresentationMode residentPresentationMode() noexcept {
  return g_residentPresentationMode;
}

void captureResidentPresentationAtProjection(const CPUContext &ctx) noexcept {
  // func_8004103C calls scenerySelectionCount before its renderer setup
  // callback installs the GTE transform.  0x800417AC is the first RTPT after
  // that callback and before any scenery depth/material decision, making it
  // the exact and unique boundary at which to complete the pending snapshot.
  if (!g_pendingResidentPresentationCapture.active ||
      ctx.pc != UINT32_C(0x800417AC) ||
      g_pendingResidentPresentationCapture.memory == nullptr)
    return;

  const PendingResidentPresentationCapture pending =
      g_pendingResidentPresentationCapture;
  g_pendingResidentPresentationCapture = {};
  const bool captured = captureResidentPresentationSnapshot(
      *pending.memory, ctx, pending.listAddress, pending.worlds,
      pending.worldCount, pending.residentFaceCount, pending.captureId,
      pending.materialLodDepthShift, pending.materialAnimationState);
  metrics::setState("host.crash3.resident_sidecar.capture_pending", 0);
  metrics::setState("host.crash3.resident_sidecar.capture_valid",
                    captured ? 1 : 0);
  if (!captured)
    metrics::count("host.crash3.resident_sidecar.capture_rejected");
}

void observeResidentPresentationProjectionInputs(
    const CPUContext &ctx) noexcept {
  if (g_residentPresentationMode == ResidentPresentationMode::Off ||
      ctx.pc != UINT32_C(0x800417AC) || !g_sceneryProjectionIdentity.enabled ||
      !g_sceneryProjectionIdentity.currentValid) {
    return;
  }

  const uint16_t packed = g_sceneryProjectionIdentity.current;
  const uint32_t state = (packed >> 11u) & 3u;
  // RTPT consumes the triangle, state-1/state-3 first half, or the state-2
  // second half. A state-3 fourth corner follows through RTPS and therefore
  // deliberately does not participate in this three-input proof.
  const uint8_t half = state == 0u ? 0u : state == 2u ? 2u : 1u;
  const uint32_t slot = sceneryEntryIdentity(packed) * 3u + half;

  std::scoped_lock lock(g_residentOrderingTableProofMutex);
  const ResidentOrderingTableProofState &proof = g_residentOrderingTableProof;
  if (proof.captureId != g_sceneryProjectionIdentity.captureId ||
      slot >= proof.expectedModelInputGenerations.size() ||
      proof.expectedModelInputGenerations[slot] != proof.modelInputGeneration) {
    return;
  }

  bool transformMatches = proof.expectedProjectionControlsValid;
  for (std::size_t index = 0u; index < proof.expectedProjectionControls.size();
       ++index) {
    transformMatches &=
        proof.expectedProjectionControls[index] == ctx.cop2c[index];
  }
  if (transformMatches) {
    metrics::count("host.crash3.resident_sidecar.projection_transform_match");
  } else {
    metrics::count(
        "host.crash3.resident_sidecar.projection_transform_mismatch");
    metrics::trace("game.crash3.scenery_projection_transform_mismatch",
                   {{"capture_id", proof.captureId},
                    {"packed", packed},
                    {"state", state},
                    {"expected_rt0", proof.expectedProjectionControls[0]},
                    {"expected_rt1", proof.expectedProjectionControls[1]},
                    {"expected_rt2", proof.expectedProjectionControls[2]},
                    {"expected_rt3", proof.expectedProjectionControls[3]},
                    {"expected_rt4", proof.expectedProjectionControls[4]},
                    {"expected_trx", proof.expectedProjectionControls[5]},
                    {"expected_try", proof.expectedProjectionControls[6]},
                    {"expected_trz", proof.expectedProjectionControls[7]},
                    {"actual_rt0", ctx.cop2c[GTE_RT11RT12]},
                    {"actual_rt1", ctx.cop2c[GTE_RT13RT21]},
                    {"actual_rt2", ctx.cop2c[GTE_RT22RT23]},
                    {"actual_rt3", ctx.cop2c[GTE_RT31RT32]},
                    {"actual_rt4", ctx.cop2c[GTE_RT33]},
                    {"actual_trx", ctx.cop2c[GTE_TRX]},
                    {"actual_try", ctx.cop2c[GTE_TRY]},
                    {"actual_trz", ctx.cop2c[GTE_TRZ]}});
  }

  const auto &expected = proof.expectedModelInputs[slot];
  bool matches = true;
  std::array<uint32_t, 3> expectedVxy{};
  std::array<uint32_t, 3> expectedVz{};
  for (std::size_t corner = 0u; corner < expected.size(); ++corner) {
    expectedVxy[corner] =
        static_cast<uint16_t>(expected[corner][0]) |
        (static_cast<uint32_t>(static_cast<uint16_t>(expected[corner][1]))
         << 16u);
    expectedVz[corner] =
        static_cast<uint32_t>(static_cast<int32_t>(expected[corner][2]));
    matches &= expectedVxy[corner] == ctx.cop2d[GTE_VXY0 + corner * 2u] &&
               expectedVz[corner] == ctx.cop2d[GTE_VZ0 + corner * 2u];
  }
  if (matches) {
    metrics::count("host.crash3.resident_sidecar.projection_input_match");
    return;
  }

  metrics::count("host.crash3.resident_sidecar.projection_input_mismatch");
  metrics::trace("game.crash3.scenery_projection_input_mismatch",
                 {{"capture_id", proof.captureId},
                  {"packed", packed},
                  {"state", state},
                  {"half", half},
                  {"expected_vxy0", expectedVxy[0]},
                  {"expected_vz0", expectedVz[0]},
                  {"expected_vxy1", expectedVxy[1]},
                  {"expected_vz1", expectedVz[1]},
                  {"expected_vxy2", expectedVxy[2]},
                  {"expected_vz2", expectedVz[2]},
                  {"actual_vxy0", ctx.cop2d[GTE_VXY0]},
                  {"actual_vz0", ctx.cop2d[GTE_VZ0]},
                  {"actual_vxy1", ctx.cop2d[GTE_VXY1]},
                  {"actual_vz1", ctx.cop2d[GTE_VZ1]},
                  {"actual_vxy2", ctx.cop2d[GTE_VXY2]},
                  {"actual_vz2", ctx.cop2d[GTE_VZ2]}});
}

std::optional<ResidentPresentationSnapshot>
residentPresentationSnapshot(uint64_t frameNumber) noexcept {
  std::scoped_lock lock(g_residentPresentationMutex);
  const auto snapshot = std::find_if(
      g_residentPresentationSnapshots.begin(),
      g_residentPresentationSnapshots.end(), [&](const auto &candidate) {
        return candidate->frameNumber == frameNumber;
      });
  if (snapshot == g_residentPresentationSnapshots.end()) {
    metrics::count("host.crash3.resident_sidecar.frame_miss");
    return std::nullopt;
  }
  metrics::count("host.crash3.resident_sidecar.frame_hit");
  return **snapshot;
}

std::vector<ResidentPresentationSnapshot>
residentPresentationSnapshots() noexcept {
  std::scoped_lock lock(g_residentPresentationMutex);
  std::vector<ResidentPresentationSnapshot> result;
  result.reserve(g_residentPresentationSnapshots.size());
  for (const auto &snapshot : g_residentPresentationSnapshots)
    result.push_back(*snapshot);
  return result;
}

std::shared_ptr<const ResidentPresentationSnapshot>
residentPresentationSnapshotByCapture(uint32_t captureId) noexcept {
  if (captureId == 0u)
    return {};
  std::scoped_lock lock(g_residentPresentationMutex);
  const auto snapshot = std::find_if(
      g_residentPresentationSnapshots.begin(),
      g_residentPresentationSnapshots.end(),
      [&](const auto &candidate) { return candidate->captureId == captureId; });
  return snapshot != g_residentPresentationSnapshots.end() ? *snapshot
                                                           : nullptr;
}

ResidentPresentationCheckpointSignal
residentPresentationCheckpointSignal() noexcept {
  std::scoped_lock lock(g_residentPresentationMutex);
  if (g_residentPresentationSnapshots.empty())
    return {};
  const ResidentPresentationSnapshot &snapshot =
      *g_residentPresentationSnapshots.back();
  return {
      snapshot.frameNumber,
      snapshot.resourceSetFingerprint,
      snapshot.activeCohortFingerprint,
      snapshot.captureId,
      snapshot.activeCohortGeneration,
      snapshot.activeCohortFingerprint != 0u && snapshot.resourceSetValidated &&
          snapshot.resourceSetFingerprint == snapshot.activeCohortFingerprint,
      snapshot.cohortCommitted,
      snapshot.cohortDisjoint,
  };
}

void observeDebugPrintf(const CPUContext &ctx,
                        uint32_t formatAddress) noexcept {
  if (!active() || formatAddress != kDemoLevelPrintfFormatAddress)
    return;
  g_demoLevelLoadOrdinal.store(ctx.r[A1], std::memory_order_relaxed);
  g_demoLevelCode.store(static_cast<uint8_t>(ctx.r[A2] & 0xffu),
                        std::memory_order_relaxed);
  g_demoLevelLoadGeneration.fetch_add(1u, std::memory_order_release);
}

DemoLevelLoadSignal demoLevelLoadSignal() noexcept {
  const uint64_t generation =
      g_demoLevelLoadGeneration.load(std::memory_order_acquire);
  const uint8_t levelCode = g_demoLevelCode.load(std::memory_order_relaxed);
  return {
      generation,
      g_demoLevelLoadOrdinal.load(std::memory_order_relaxed),
      levelCode,
      levelCode == static_cast<uint8_t>('Y'),
  };
}

void observeSceneryOrderingTableAverage(const CPUContext &ctx,
                                        uint32_t averageDepth,
                                        uint8_t vertexCount) noexcept {
  if (!active() ||
      g_residentPresentationMode == ResidentPresentationMode::Off ||
      ctx.r[3] != kRendererScratchBase ||
      !g_sceneryProjectionIdentity.enabled ||
      !g_sceneryProjectionIdentity.currentValid) {
    return;
  }
  const bool avsz3Instruction =
      vertexCount == 3u && ctx.pc == UINT32_C(0x8004184C);
  const bool avsz4Instruction =
      vertexCount == 4u && ctx.pc == UINT32_C(0x80041B20);
  if (!avsz3Instruction && !avsz4Instruction)
    return;

  constexpr uint32_t kOrderingTablePivot = 1904u;
  const uint16_t observedBucket = static_cast<uint16_t>(
      averageDepth < kOrderingTablePivot ? kOrderingTablePivot - averageDepth
                                         : 0u);
  const uint16_t packed = g_sceneryProjectionIdentity.current;
  const uint32_t state = (packed >> 11u) & 3u;

  std::scoped_lock lock(g_residentOrderingTableProofMutex);
  ResidentOrderingTableProofState &proof = g_residentOrderingTableProof;
  if (proof.captureId == 0u ||
      proof.captureId != g_sceneryProjectionIdentity.captureId) {
    return;
  }
  // Triangles and half-quads both execute AVSZ3; only a complete state-3
  // source quad reaches AVSZ4.  Treating every quad identity as AVSZ4 made the
  // proof unavailable precisely at the intrinsic half-quad boundary.
  if ((avsz3Instruction && state == 3u) || (avsz4Instruction && state != 3u)) {
    ++proof.unavailable;
    publishResidentOrderingTableProofCaptureLocked(proof);
    return;
  }

  const auto compareHalf = [&](uint8_t half) {
    const uint32_t slot = sceneryEntryIdentity(packed) * 3u + half;
    if (slot >= proof.expectedBuckets.size() ||
        proof.expectedValid[slot] == 0u) {
      ++proof.unavailable;
      return;
    }
    if (proof.expectedBuckets[slot] == observedBucket) {
      ++proof.matches;
    } else {
      ++proof.mismatches;
      const uint32_t prefetchedSlot =
          sceneryEntryIdentity(g_sceneryProjectionIdentity.prefetched) * 3u +
          half;
      const bool prefetchedExpectedValid =
          g_sceneryProjectionIdentity.prefetchedValid &&
          prefetchedSlot < proof.expectedBuckets.size() &&
          proof.expectedValid[prefetchedSlot] != 0u;
      const uint16_t prefetchedExpected =
          prefetchedExpectedValid ? proof.expectedBuckets[prefetchedSlot] : 0u;
      metrics::trace(
          "game.crash3.scenery_local_ot_mismatch",
          {{"capture_id", proof.captureId},
           {"packed", packed},
           {"state", state},
           {"half", half},
           {"expected", proof.expectedBuckets[slot]},
           {"observed", observedBucket},
           {"average", averageDepth},
           {"prefetched_valid",
            g_sceneryProjectionIdentity.prefetchedValid ? 1u : 0u},
           {"prefetched", g_sceneryProjectionIdentity.prefetched},
           {"prefetched_expected_valid", prefetchedExpectedValid ? 1u : 0u},
           {"prefetched_expected", prefetchedExpected},
           {"actual_sz0", ctx.cop2d[GTE_SZ0]},
           {"actual_sz1", ctx.cop2d[GTE_SZ1]},
           {"actual_sz2", ctx.cop2d[GTE_SZ2]},
           {"actual_sz3", ctx.cop2d[GTE_SZ3]},
           {"last_cursor", g_sceneryProjectionIdentity.lastCursor}});
      if (proof.mismatchSamples < 16u) {
        ++proof.mismatchSamples;
        fmt::print(
            stderr,
            "[Crash3] local OT mismatch capture={} packed=0x{:04X} "
            "state={} half={} expected={} observed={} average={} "
            "sz=({},{},{},{}) zsf={}\n",
            proof.captureId, packed, state, half, proof.expectedBuckets[slot],
            observedBucket, averageDepth, ctx.cop2d[GTE_SZ0],
            ctx.cop2d[GTE_SZ1], ctx.cop2d[GTE_SZ2], ctx.cop2d[GTE_SZ3],
            vertexCount == 3u ? static_cast<int16_t>(ctx.cop2c[GTE_ZSF3])
                              : static_cast<int16_t>(ctx.cop2c[GTE_ZSF4]));
      }
    }
  };
  if (state == 0u) {
    compareHalf(0u);
  } else {
    if ((state & 1u) != 0u)
      compareHalf(1u);
    if ((state & 2u) != 0u)
      compareHalf(2u);
  }
  publishResidentOrderingTableProofCaptureLocked(proof);
}

ResidentOrderingTableProof
residentOrderingTableProof(uint32_t captureId) noexcept {
  std::scoped_lock lock(g_residentOrderingTableProofMutex);
  if (captureId != 0u) {
    const auto capture = std::find_if(
        g_residentOrderingTableProofCaptures.rbegin(),
        g_residentOrderingTableProofCaptures.rend(),
        [&](const auto &entry) { return entry.captureId == captureId; });
    if (capture != g_residentOrderingTableProofCaptures.rend())
      return capture->proof;
    return {};
  }
  return {g_residentOrderingTableProof.topologyFingerprint,
          g_residentOrderingTableProof.matches,
          g_residentOrderingTableProof.mismatches,
          g_residentOrderingTableProof.unavailable};
}

ObjectPresentationMode objectPresentationMode() noexcept {
  return g_objectPresentationMode;
}

void observeObjectPrePresentation(recomp_context *ctx) noexcept {
  if (!active() || g_objectPresentationMode == ObjectPresentationMode::Off ||
      ctx == nullptr || ctx->mem == nullptr) {
    return;
  }

  const Memory &memory = *ctx->mem;
  const uint32_t address = ctx->r[4];
  const uint64_t frame = metrics::traceFrame();
  std::scoped_lock lock(g_objectPresentationMutex);
  if (g_objectPresentationBuildingActive &&
      g_objectPresentationBuilding.frameNumber != frame) {
    publishObjectPresentationBuildingLocked();
  }
  if (!g_objectPresentationBuildingActive) {
    g_objectPresentationBuilding = {};
    g_objectPresentationBuilding.captureId = nextObjectPresentationCaptureId();
    g_objectPresentationBuilding.frameNumber = frame;
    g_objectPresentationBuildingActive = true;
  }

  if (!isCrash3PresentationObject(memory, address)) {
    ++g_objectPresentationBuilding.rejectedObjects;
    metrics::count("host.crash3.object_sidecar.rejected_object");
    if (g_traceObjectPresentation) {
      metrics::trace("game.crash3.object_catalog_reject",
                     {{"capture_id", g_objectPresentationBuilding.captureId},
                      {"object", address},
                      {"type", isCanonicalGuestRamRange(address, 4u, 4u)
                                   ? memory.read32(address)
                                   : 0u}});
    }
    return;
  }

  ObjectPresentationObject object = captureObjectPresentation(memory, address);
  auto existing = std::find_if(g_objectPresentationBuilding.objects.begin(),
                               g_objectPresentationBuilding.objects.end(),
                               [&](const ObjectPresentationObject &candidate) {
                                 return candidate.address == address;
                               });
  if (existing == g_objectPresentationBuilding.objects.end()) {
    g_objectPresentationBuilding.objects.push_back(std::move(object));
    existing = std::prev(g_objectPresentationBuilding.objects.end());
    metrics::count("host.crash3.object_sidecar.object");
  } else {
    // A repeated logical visit refreshes mutable state without discarding an
    // exact renderer-node binding already observed in this same interval.
    object.rendererNodes = std::move(existing->rendererNodes);
    *existing = std::move(object);
    metrics::count("host.crash3.object_sidecar.duplicate_object_visit");
  }

  if (g_traceObjectPresentation) {
    metrics::trace(
        "game.crash3.object_catalog_object",
        {{"capture_id", g_objectPresentationBuilding.captureId},
         {"object", existing->address},
         {"model_resource", existing->modelResource},
         {"global_resource", existing->globalResource},
         {"animation_sequence", existing->animationSequence},
         {"animation_frame", existing->animationFrame},
         {"model_type", existing->modelType},
         {"supported_model_type", existing->supportedModelType ? 1u : 0u},
         {"trans_x", existing->translation[0]},
         {"trans_y", existing->translation[1]},
         {"trans_z", existing->translation[2]},
         {"scale_x", existing->scale[0]},
         {"scale_y", existing->scale[1]},
         {"scale_z", existing->scale[2]},
         {"status_a", existing->statusA},
         {"status_b", existing->statusB},
         {"routine_flags", existing->routineFlags},
         {"display_mode", existing->displayMode},
         {"render_selector", existing->renderSelector}});
  }
}

void observeObjectRendererStreams(recomp_context *ctx) noexcept {
  if (!active() || g_objectPresentationMode == ObjectPresentationMode::Off ||
      ctx == nullptr || ctx->mem == nullptr ||
      ctx->r[3] != kRendererScratchBase ||
      ctx->r[29] != g_rendererObjectSource || g_rendererObjectCaptureId == 0u ||
      !isCanonicalGuestRamRange(g_rendererObjectSource,
                                kCrash3RendererNodeBytes, 4u)) {
    return;
  }

  const Memory &memory = *ctx->mem;
  std::scoped_lock lock(g_objectPresentationMutex);
  if (!g_objectPresentationBuildingActive ||
      g_objectPresentationBuilding.captureId != g_rendererObjectCaptureId) {
    return;
  }
  auto object = std::find_if(
      g_objectPresentationBuilding.objects.begin(),
      g_objectPresentationBuilding.objects.end(), [&](const auto &candidate) {
        return candidate.address == g_rendererLogicalObject;
      });
  if (object == g_objectPresentationBuilding.objects.end() ||
      object->modelType != 1u) {
    return;
  }
  auto node =
      std::find_if(object->rendererNodes.begin(), object->rendererNodes.end(),
                   [&](const auto &candidate) {
                     return candidate.address == g_rendererObjectSource;
                   });
  if (node == object->rendererNodes.end())
    return;

  // This hook executes at 0x8003F76C: the source pointers have reached their
  // final values and the colour prefix has just been copied to scratch+320,
  // but no type-1 command has yet overwritten that aliased storage.
  for (uint32_t index = 0u; index < node->byteCount / sizeof(uint32_t);
       ++index) {
    node->words[index] =
        memory.read32(g_rendererObjectSource + index * sizeof(uint32_t));
  }
  captureType1RendererStreams(*node, memory, kRendererScratchBase + 320u);
  metrics::count("host.crash3.object_sidecar.type1_consumption_capture");
  if (metrics::traceEnabled()) {
    metrics::trace(
        "game.crash3.object_renderer_stream_capture",
        {{"capture_id", g_rendererObjectCaptureId},
         {"renderer_node", g_rendererObjectSource},
         {"object", g_rendererLogicalObject},
         {"command_table", ctx->r[18]},
         {"color_table", ctx->r[22]},
         {"streams_captured", node->type1StreamsCaptured ? 1u : 0u},
         {"command_words",
          static_cast<int64_t>(node->type1CommandWords.size())},
         {"source_color_words",
          static_cast<int64_t>(node->type1ColorWords.size())},
         {"scratch_color_words",
          static_cast<int64_t>(node->type1ScratchColorWords.size())},
         {"source_color_0",
          node->type1ColorWords.empty() ? 0u : node->type1ColorWords[0]},
         {"scratch_color_0", node->type1ScratchColorWords.empty()
                                 ? 0u
                                 : node->type1ScratchColorWords[0]}});
  }
}

std::vector<ObjectPresentationSnapshot> objectPresentationSnapshots() noexcept {
  std::scoped_lock lock(g_objectPresentationMutex);
  std::vector<ObjectPresentationSnapshot> result{
      g_objectPresentationSnapshots.begin(),
      g_objectPresentationSnapshots.end()};
  if (g_objectPresentationBuildingActive)
    result.push_back(g_objectPresentationBuilding);
  return result;
}

double horizontalProjectionScale() noexcept {
  return widescreen16x9() ? 0.75 : 1.0;
}

void observeProjectedBillboardCentre(const CPUContext &ctx, uint8_t dataReg,
                                     uint32_t packedSxy,
                                     const PGXPValue &precision) noexcept {
  // Exact SCES-01420 construction boundary: func_80045638 reads the RTPS
  // centre into r5 here, with r23 already naming the 40-byte POLY_FT4 packet.
  // No opcode, level, frame content or screen-space fit participates.
  if (!active() || ctx.pc != UINT32_C(0x800457BC) ||
      (dataReg & 31u) != GTE_SXY2 ||
      !precision.hasSignedProjection(packedSxy) ||
      !isCanonicalGuestRamRange(ctx.r[23], 40u, 4u)) {
    return;
  }

  const uint32_t packetBase = Memory::toPhysical(ctx.r[23]);
  const uint64_t frame = metrics::traceFrame();
  const ProjectedBillboardCentreRecord record{packedSxy, precision, frame};
  std::scoped_lock lock(g_projectedBillboardMutex);
  if (frame != g_projectedBillboardPruneFrame) {
    for (auto it = g_projectedBillboardCentres.begin();
         it != g_projectedBillboardCentres.end();) {
      const uint64_t captured = it->second.frame;
      if (frame < captured || frame - captured > kProjectedBillboardFrameWindow)
        it = g_projectedBillboardCentres.erase(it);
      else
        ++it;
    }
    g_projectedBillboardPruneFrame = frame;
  }
  for (uint32_t offset : {8u, 16u, 24u, 32u})
    g_projectedBillboardCentres[packetBase + offset] = record;
  metrics::count("host.crash3.pgxp.billboard_centre_captured");
}

bool lookupProjectedBillboardPrecision(uint32_t address, uint32_t packedSxy,
                                       PGXPValue &precision) noexcept {
  if (!active())
    return false;

  ProjectedBillboardCentreRecord record;
  {
    std::scoped_lock lock(g_projectedBillboardMutex);
    const auto found =
        g_projectedBillboardCentres.find(Memory::toPhysical(address));
    if (found == g_projectedBillboardCentres.end())
      return false;
    const uint64_t frame = metrics::traceFrame();
    if (frame < found->second.frame ||
        frame - found->second.frame > kProjectedBillboardFrameWindow)
      return false;
    record = found->second;
  }

  if (!derivePGXPProjectedScreenOffset(record.precision, record.packedSxy,
                                       packedSxy, precision)) {
    metrics::count("host.crash3.pgxp.billboard_vertex_rejected");
    return false;
  }
  // The captured centre already uses the wide projection. The quad's CPU
  // offsets are still native PS1 pixels, so they need the same horizontal
  // scale; translating an unscaled rectangle to a wide centre stretches it.
  // This changes only presentation X/QX, never the packet or its depth/UV.
  const double scale = horizontalProjectionScale();
  if (scale != 1.0) {
    const int32_t deltaX =
        static_cast<int32_t>(static_cast<int16_t>(packedSxy)) -
        static_cast<int32_t>(static_cast<int16_t>(record.packedSxy));
    precision.x = record.precision.x + static_cast<float>(deltaX * scale);
    precision.qx = record.precision.qx + deltaX * scale * precision.z;
    metrics::count("host.crash3.pgxp.billboard_wide_extent_corrected");
  }
  metrics::count("host.crash3.pgxp.billboard_vertex_derived");
  return true;
}

uint32_t viewportCullPresentationSxy(uint32_t packedSxy) noexcept {
  if (!widescreen16x9() || !presentationCullReadsEnabled())
    return packedSxy;

  const uint32_t presented =
      mapViewportCullPresentationSxy(packedSxy, presentationCullGuardPixels());
  const int32_t nativeX = static_cast<int16_t>(packedSxy);
  const int32_t presentedX = static_cast<int16_t>(presented);
  metrics::count("host.crash3.presentation_cull.bbox_witness");
  if ((nativeX < 0 || nativeX > 511) && presentedX >= 0 && presentedX <= 511) {
    metrics::count("host.crash3.presentation_cull.bbox_edge_recovered");
  }
  // This public entry is called only by Crash's instruction override at
  // 0x8003E168. Renderer read sites use the private mapper above, so the trace
  // is an exact inventory of the top-level object bounding-box witnesses and
  // cannot be polluted by per-face or packet SXY traffic.
  static const bool traceBboxReads = [] {
    const char *value = std::getenv("PS1_TRACE_CRASH3_BBOX_READS");
    return value != nullptr && *value != '\0' && *value != '0';
  }();
  if (traceBboxReads) {
    metrics::trace("game.crash3.bbox_sxy_read",
                   {{"packed_sxy", packedSxy},
                    {"presented_sxy", presented},
                    {"native_x", static_cast<int16_t>(packedSxy)},
                    {"native_y", static_cast<int16_t>(packedSxy >> 16u)},
                    {"presented_x", static_cast<int16_t>(presented)}});
  }
  return presented;
}

bool rendererPresentationSxyIsCullOnlyGuard(
    uint32_t pc, uint8_t dataReg, int32_t projectionOriginXQ16) noexcept {
  dataReg &= 31u;
  if (dataReg == GTE_SXYP)
    dataReg = GTE_SXY2;
  constexpr int32_t kMainProjectionOriginXQ16 = 256 << 16;
  if (!widescreen16x9() || sceneryCullGuardPixels() == 0u ||
      projectionOriginXQ16 != kMainProjectionOriginXQ16)
    return false;

  if (pc == UINT32_C(0x80041B14) && dataReg == GTE_SXY2)
    return true;
  return false;
}

uint32_t rendererPresentationSxy(uint32_t pc, uint8_t dataReg,
                                 uint32_t packedSxy,
                                 int32_t projectionOriginXQ16) noexcept {
  dataReg &= 31u;
  if (dataReg == GTE_SXYP)
    dataReg = GTE_SXY2;
  if (!widescreen16x9() || !presentationCullReadsEnabled() ||
      dataReg < GTE_SXY0 || dataReg > GTE_SXY2)
    return packedSxy;

  // The warp-room trace proves that this executable interleaves its main
  // OFX=256 projection with a 64x64 off-screen pass at OFX=32 through the
  // same native-world code. That auxiliary texture is later composed into the
  // main scene and must retain its own native coordinates/admission domain.
  constexpr int32_t kMainProjectionOriginXQ16 = 256 << 16;
  const bool mainProjection = projectionOriginXQ16 == kMainProjectionOriginXQ16;

  // 0x8003CECC..D4 is the packed three-corner viewport reject in the native
  // world renderer used by the warp room. 0x8003DF14..1C is its second,
  // byte-for-byte equivalent material path. Only their MFC2 witnesses are
  // widened: the subsequent architectural NCLIP and the emission helpers keep
  // the native FIFO/packet coordinates, while PGXP carries the final 16:9
  // positions. This admits edge faces without changing the game's camera or
  // gameplay-visible GTE state.
  const bool nativeWorldViewportReject =
      (pc >= UINT32_C(0x8003CECC) && pc <= UINT32_C(0x8003CED4)) ||
      (pc >= UINT32_C(0x8003DF14) && pc <= UINT32_C(0x8003DF1C));

  // This is the complete SXY MFC2/SWC2 interval in Crash's shared,
  // scratchpad-backed polygon renderer. GTE commands in the same region keep
  // reading the native cop2d FIFO directly, so NCLIP/backface decisions and
  // gameplay-visible architectural state remain untouched. Both selections
  // are code-derived and executable-specific; neither is a generic PGXP
  // policy.
  const bool sharedPolygonRenderer =
      pc >= UINT32_C(0x80041838) && pc <= UINT32_C(0x80041CCC);
  const bool sharedPolygonCullRead =
      pc == UINT32_C(0x80041838) || pc == UINT32_C(0x8004183C) ||
      pc == UINT32_C(0x80041840) || pc == UINT32_C(0x80041ACC) ||
      pc == UINT32_C(0x80041AD0) || pc == UINT32_C(0x80041AD4) ||
      pc == UINT32_C(0x80041B14);

  // func_800434D0 projects one point at 0x80043648, rejects it at 0x8004367C
  // against the same packed 512x256 bounds and then copies that exact SXY into
  // a one-pixel GP0 point packet. The recomp's native-vs-widened edge traces
  // prove that this path admits additional warp-room stars/effects at 16:9;
  // donor emulators remain the 4:3 correctness oracle, not a widescreen claim.
  const bool pointSpriteViewportReject = pc == UINT32_C(0x8004367C);
  const bool cullOnlyGuard =
      rendererPresentationSxyIsCullOnlyGuard(pc, dataReg, projectionOriginXQ16);
  const bool selected =
      mainProjection && (nativeWorldViewportReject || sharedPolygonRenderer ||
                         pointSpriteViewportReject);
  // Every packet-visible read gets the true 16:9 coordinate. Wider admission
  // is applied later to branch-only GPR witnesses by
  // prepareSceneryCullGuardForCommand(), once any packet-bound value has been
  // preserved. Compressing here would make the admitted geometry itself use
  // the guard coordinate and would detach its PGXP lineage.
  const uint32_t activeGuardPixels = selected ? sceneryCullGuardPixels() : 0u;
  uint32_t presented =
      selected ? mapViewportCullPresentationSxy(packedSxy) : packedSxy;
  if (selected) {
    const int32_t nativeX = static_cast<int16_t>(packedSxy);
    const int32_t presentedX = static_cast<int16_t>(presented);
    metrics::count("host.crash3.presentation_cull.renderer_witness");
    if ((nativeX < 0 || nativeX > 511) && presentedX >= 0 &&
        presentedX <= 511) {
      metrics::count("host.crash3.presentation_cull.renderer_edge_recovered");
    }
  }
  // The fourth-corner MFC2 is consumed only by the quad's packed viewport
  // comparison. Its packet coordinate is written later from the untouched GTE
  // FIFO, so this is the one safe read-site counterpart to the command hook
  // below. Earlier three-corner values are shifted after the packet-bound first
  // corner has been stored.
  if (selected && sharedPolygonCullRead && cullOnlyGuard)
    presented = translateSceneryCullGuardSxy(presented);

  // One-run selector for closing Crash's remaining 16:9 admission sites.
  // It is intentionally profile-local and dormant unless both the ordinary
  // JSONL trace and this explicit switch are enabled. The record describes a
  // guest SXY read; it never changes which PCs are selected above.
  static const bool traceReads = [] {
    const char *value = std::getenv("PS1_TRACE_CRASH3_SXY_READS");
    return value != nullptr && *value != '\0' && *value != '0';
  }();
  if (traceReads) {
    const auto &identity = g_sceneryProjectionIdentity;
    metrics::trace(
        "game.crash3.sxy_read",
        {{"pc", pc},
         {"data_reg", dataReg},
         {"selected", selected ? 1u : 0u},
         {"guard_pixels", activeGuardPixels},
         {"main_projection", mainProjection ? 1u : 0u},
         {"projection_origin_x_q16", projectionOriginXQ16},
         {"packed_sxy", packedSxy},
         {"presented_sxy", presented},
         {"scenery_entry_valid", identity.currentValid ? 1u : 0u},
         {"scenery_entry", identity.current},
         {"scenery_prefetched_valid", identity.prefetchedValid ? 1u : 0u},
         {"scenery_prefetched_entry", identity.prefetched}});
  }
  return presented;
}

void prepareSceneryCullGuardForCommand(CPUContext &ctx) noexcept {
  const uint32_t guard = sceneryCullGuardPixels();
  constexpr int32_t kMainProjectionOriginXQ16 = 256 << 16;
  const bool terrainForward =
      ctx.pc == UINT32_C(0x800439CC) || ctx.pc == UINT32_C(0x80043A80);
  const bool terrainReverse =
      ctx.pc == UINT32_C(0x80043C28) || ctx.pc == UINT32_C(0x80043CDC);
  const bool waterForward =
      ctx.pc == UINT32_C(0x80044438) || ctx.pc == UINT32_C(0x800444F4);
  const bool waterReverse =
      ctx.pc == UINT32_C(0x800446A8) || ctx.pc == UINT32_C(0x80044764);
  const bool proceduralSurface =
      terrainForward || terrainReverse || waterForward || waterReverse;
  if (proceduralSurface && widescreen16x9() && presentationCullReadsEnabled() &&
      static_cast<int32_t>(ctx.cop2c[GTE_OFX]) == kMainProjectionOriginXQ16) {
    // Terrain and water each have forward/reverse walkers, each with separate
    // search and strip-continuation tests. All eight AVSZ4 sites use the same
    // four-corner branch state. Omitting a direction leaves a native-width
    // hole whenever the camera selects it, even though the opposite direction
    // reports recovered edge faces. At each AVSZ4 the first packed
    // corner has already been subtracted from the 512x257 bounds, the next
    // three remain raw, and r6 contains the first partial outside test. The
    // packet-bound first corner is also cached in scratch RAM, so changing the
    // earlier SWC2 read distorts emitted polygons. Reconstruct all four here,
    // after packet state is safe, and rebuild only the branch-local witnesses
    // in one internally consistent guard domain.
    constexpr int32_t kExpectedRight = 512;
    constexpr int32_t kExpectedBottom = 257;
    const uint32_t bounds = ctx.r[2];
    if (static_cast<int16_t>(bounds) != kExpectedRight ||
        static_cast<int16_t>(bounds >> 16u) != kExpectedBottom) {
      metrics::count(
          "game.crash3.procedural_surface_cull_guard.bound_mismatch");
      return;
    }

    const std::array<uint32_t, 4> original{{
        ctx.r[24] + bounds,
        ctx.r[25],
        ctx.r[4],
        ctx.r[5],
    }};
    std::array<uint32_t, 4> presented{};
    bool recoveredEdge = false;
    for (std::size_t index = 0; index < original.size(); ++index) {
      const uint32_t exact = mapViewportCullPresentationSxy(original[index]);
      presented[index] = mapViewportCullPresentationSxy(original[index], guard);
      const int32_t exactX = static_cast<int16_t>(exact);
      const int32_t guardedX = static_cast<int16_t>(presented[index]);
      recoveredEdge |= (exactX < 0 || exactX >= kExpectedRight) &&
                       guardedX >= 0 && guardedX < kExpectedRight;
    }

    ctx.r[24] = presented[0] - bounds;
    ctx.r[25] = presented[1];
    ctx.r[4] = presented[2];
    ctx.r[5] = presented[3];
    ctx.r[6] = presented[0] & presented[1];
    for (const uint8_t reg :
         {uint8_t{24}, uint8_t{25}, uint8_t{4}, uint8_t{5}, uint8_t{6}}) {
      ctx.pgxpGpr[reg].invalidate();
    }
    metrics::count("host.crash3.presentation_cull.procedural_surface_prepared");
    metrics::count(
        terrainForward   ? "host.crash3.presentation_cull.terrain_forward"
        : terrainReverse ? "host.crash3.presentation_cull.terrain_reverse"
        : waterForward   ? "host.crash3.presentation_cull.water_forward"
                         : "host.crash3.presentation_cull.water_reverse");
    if (recoveredEdge) {
      metrics::count(
          "host.crash3.presentation_cull.procedural_surface_edge_recovered");
    }
    return;
  }
  if (guard == 0u)
    return;

  // These four command sites are after the relevant exact SXY reads but before
  // their packed viewport comparisons. In the shared quad path the first
  // packet coordinate has already been stored at 0x80041AD8. Consequently
  // the listed GPRs are visibility witnesses only: shifting them cannot move
  // a final vertex or mutate the architectural GTE FIFO.
  const bool nativeWorld =
      ctx.pc == UINT32_C(0x8003CEE0) || ctx.pc == UINT32_C(0x8003DF28);
  const bool sharedPolygon =
      ctx.pc == UINT32_C(0x8004184C) || ctx.pc == UINT32_C(0x80041AE0);
  if (!nativeWorld && !sharedPolygon)
    return;

  constexpr int32_t kExpectedRight = 512;
  // Crash uses 257 for the exclusive bottom comparison while X uses the
  // corresponding packed 512 boundary.
  constexpr int32_t kExpectedBottom = 257;
  const uint8_t boundsReg = nativeWorld ? uint8_t{24} : uint8_t{31};
  const uint32_t bounds = ctx.r[boundsReg];
  const int32_t right = static_cast<int16_t>(bounds);
  const int32_t bottom = static_cast<int16_t>(bounds >> 16u);
  if (right != kExpectedRight || bottom != kExpectedBottom) {
    metrics::count("game.crash3.scenery_cull_guard.bound_mismatch");
    return;
  }

  const std::array<uint8_t, 3> witnesses =
      nativeWorld ? std::array<uint8_t, 3>{{25u, 4u, 5u}}
                  : std::array<uint8_t, 3>{{24u, 25u, 2u}};
  const uint32_t widenedRight =
      static_cast<uint32_t>(kExpectedRight) + 2u * guard;
  bool recoveredEdge = false;
  for (const uint8_t reg : witnesses) {
    const int32_t originalX = static_cast<int16_t>(ctx.r[reg]);
    ctx.r[reg] = translateSceneryCullGuardSxy(ctx.r[reg]);
    ctx.pgxpGpr[reg].invalidate();
    const int32_t guardedX = static_cast<int16_t>(ctx.r[reg]);
    recoveredEdge |= (originalX < 0 || originalX >= kExpectedRight) &&
                     guardedX >= 0 &&
                     static_cast<uint32_t>(guardedX) < widenedRight;
  }
  ctx.r[boundsReg] =
      (bounds & UINT32_C(0xffff0000)) | static_cast<uint16_t>(widenedRight);
  ctx.pgxpGpr[boundsReg].invalidate();
  metrics::count("game.crash3.scenery_cull_guard.prepared");
  metrics::count(nativeWorld
                     ? "host.crash3.presentation_cull.native_world_prepared"
                     : "host.crash3.presentation_cull.shared_prepared");
  if (recoveredEdge) {
    metrics::count("host.crash3.presentation_cull.command_edge_recovered");
  }
}

void observeSceneryDelta(recomp_context *ctx) noexcept {
  const ScenerySelectionMode mode = scenerySelectionMode();
  const char *pathTrace = std::getenv("PS1_TRACE_CRASH3_SCENERY_PATHS");
  const bool detailPathAudit =
      pathTrace != nullptr && *pathTrace == '1' && metrics::traceEnabled();
  const bool modernVariants = sceneryVariantsEnabled();
  if (ctx == nullptr || ctx->mem == nullptr ||
      (!detailPathAudit && !modernVariants &&
       mode != ScenerySelectionMode::Audit &&
       mode != ScenerySelectionMode::Adjacent &&
       mode != ScenerySelectionMode::Symmetric &&
       mode != ScenerySelectionMode::Geometric &&
       mode != ScenerySelectionMode::GeometricResident))
    return;

  const Memory &memory = *ctx->mem;
  const uint32_t source = ctx->r[4];
  const uint32_t previous = ctx->r[5];
  const uint32_t destination = ctx->r[6];
  const uint32_t direction = ctx->r[7] != 0u ? 1u : 0u;
  const uint32_t returnAddress = ctx->r[31];
  const bool knownCallsite = returnAddress == UINT32_C(0x80021968) ||
                             returnAddress == UINT32_C(0x800219B8) ||
                             returnAddress == UINT32_C(0x80021A44) ||
                             returnAddress == UINT32_C(0x80021A8C);
  if (!knownCallsite || !isRamRange(source, 4u) ||
      !isRamRange(destination, 4u)) {
    g_sceneryDeltaHistory = {};
    g_sceneryPathCache = {};
    return;
  }

  const uint32_t sourceType = memory.read16(source + 2u);
  const uint32_t stateCount = ctx->r[23];
  const uint32_t targetIndex =
      stateCount == 0u ? 0u : std::min(ctx->r[30] >> 8u, stateCount - 1u);
  uint32_t decodedIndex = targetIndex;
  if (returnAddress == UINT32_C(0x80021968)) {
    decodedIndex = 0u;
  } else if (returnAddress == UINT32_C(0x800219B8)) {
    decodedIndex = stateCount == 0u ? 0u : stateCount - 1u;
  } else if (returnAddress == UINT32_C(0x80021A44)) {
    decodedIndex =
        stateCount == 0u ? 0u : std::min(ctx->r[17], stateCount - 1u);
  } else if (returnAddress == UINT32_C(0x80021A8C)) {
    decodedIndex = ctx->r[17] == 0u ? 0u : ctx->r[17] - 1u;
  }

  uint32_t entryBase = 0u;
  const uint64_t entrySlot = static_cast<uint64_t>(ctx->r[29]) + 68u;
  const bool entryBaseValid =
      entrySlot <= std::numeric_limits<uint32_t>::max() &&
      isRamRange(static_cast<uint32_t>(entrySlot), 4u) &&
      ((entryBase = memory.read32(static_cast<uint32_t>(entrySlot))),
       isRamRange(entryBase, 20u));
  bool cacheRebuilt = false;
  if (detailPathAudit || modernVariants ||
      mode == ScenerySelectionMode::Symmetric ||
      mode == ScenerySelectionMode::Geometric ||
      mode == ScenerySelectionMode::GeometricResident) {
    const bool fullSource = (sourceType & 1u) == 0u;
    if (entryBaseValid && (fullSource || !g_sceneryPathCache.valid ||
                           g_sceneryPathCache.entryBase != entryBase ||
                           g_sceneryPathCache.stateCount != stateCount)) {
      cacheRebuilt = buildSceneryPathCache(memory, entryBase, stateCount);
    }
    if (g_sceneryPathCache.valid && g_sceneryPathCache.entryBase == entryBase &&
        decodedIndex < g_sceneryPathCache.states.size()) {
      g_sceneryPathCache.currentIndex = decodedIndex;
    }
  }

  if (metrics::traceEnabled()) {
    metrics::trace("game.crash3.scenery_selector_step",
                   {{"source", source},
                    {"previous", previous},
                    {"destination", destination},
                    {"entry_base", entryBaseValid ? entryBase : 0u},
                    {"table_cursor", ctx->r[19]},
                    {"return_address", returnAddress},
                    {"direction", direction},
                    {"source_type", sourceType},
                    {"selector_q8", static_cast<int32_t>(ctx->r[30])},
                    {"target_index", targetIndex},
                    {"decoder_index_before", ctx->r[17]},
                    {"decoder_index_after", decodedIndex},
                    {"remaining_steps", static_cast<int32_t>(ctx->r[18])},
                    {"state_count", stateCount},
                    {"change_flags", ctx->r[21]},
                    {"section", ctx->r[22]},
                    {"cache_rebuilt", cacheRebuilt ? 1u : 0u},
                    {"cache_valid", g_sceneryPathCache.valid ? 1u : 0u}});
  }

  if (mode != ScenerySelectionMode::Adjacent)
    return;

  if ((sourceType & 1u) == 0u) {
    // A full SLST establishes a new chain. The destination is not populated
    // until the original function returns, so only remember its identity.
    g_sceneryDeltaHistory = {};
    g_sceneryDeltaHistory.lastDestination = destination;
    if (metrics::traceEnabled()) {
      metrics::trace("game.crash3.scenery_delta_window",
                     {{"source", source},
                      {"previous", previous},
                      {"destination", destination},
                      {"table_cursor", ctx->r[19]},
                      {"return_address", returnAddress},
                      {"direction", direction},
                      {"source_type", sourceType},
                      {"captured_entries", 0u},
                      {"history_size", 0u},
                      {"continuous", 0u}});
    }
    return;
  }

  std::vector<uint16_t> entries;
  if (!readDecodedSceneryList(memory, previous, entries)) {
    g_sceneryDeltaHistory = {};
    return;
  }
  const bool continuous = g_sceneryDeltaHistory.lastDestination == 0u ||
                          g_sceneryDeltaHistory.lastDestination == previous;
  if (!continuous)
    g_sceneryDeltaHistory.preceding.clear();

  if (g_sceneryDeltaHistory.preceding.empty() ||
      g_sceneryDeltaHistory.preceding.back().entries != entries) {
    g_sceneryDeltaHistory.preceding.push_back(
        {source, ctx->r[19], direction != 0u, std::move(entries)});
  }
  const uint32_t retained = adjacentSceneryDeltaCount();
  if (g_sceneryDeltaHistory.preceding.size() > retained) {
    const std::size_t excess =
        g_sceneryDeltaHistory.preceding.size() - retained;
    g_sceneryDeltaHistory.preceding.erase(
        g_sceneryDeltaHistory.preceding.begin(),
        g_sceneryDeltaHistory.preceding.begin() + excess);
  }
  g_sceneryDeltaHistory.lastDestination = destination;

  if (metrics::traceEnabled()) {
    const auto &latest = g_sceneryDeltaHistory.preceding.back();
    metrics::trace(
        "game.crash3.scenery_delta_window",
        {{"source", source},
         {"previous", previous},
         {"destination", destination},
         {"table_cursor", ctx->r[19]},
         {"return_address", returnAddress},
         {"direction", direction},
         {"source_type", sourceType},
         {"captured_entries", static_cast<int64_t>(latest.entries.size())},
         {"history_size",
          static_cast<int64_t>(g_sceneryDeltaHistory.preceding.size())},
         {"continuous", continuous ? 1u : 0u}});
  }
}

int32_t scenerySelectionCount(recomp_context *ctx,
                              uint32_t listAddress) noexcept {
  if (ctx == nullptr || ctx->mem == nullptr)
    return 0;
  const Memory &memory = *ctx->mem;
  // func_8004103C is shared by dynamic type-1 models and resident scenery.
  // For a dynamic model its list argument is intrinsically node+0x38; once a
  // different list arrives, the prior object's projection identity has ended
  // and must not leak into scenery using the same scratchpad RTPT PCs.
  const bool contextNodeValid =
      isCanonicalGuestRamRange(g_rendererObjectSource, 0x48u, 4u);
  const uint32_t objectCommandList =
      contextNodeValid ? memory.read32(g_rendererObjectSource + 0x38u) : 0u;
  const bool currentObjectList = g_rendererObjectCaptureId != 0u &&
                                 contextNodeValid &&
                                 objectCommandList == listAddress;
  static const bool traceObjectPresenterBoundary = [] {
    const char *value = std::getenv("PS1_TRACE_CRASH3_OBJECT_PRESENTER");
    return value != nullptr && *value != '\0' && *value != '0';
  }();
  if (traceObjectPresenterBoundary && g_rendererObjectCaptureId != 0u) {
    metrics::trace(
        "game.crash3.object_presenter_list_boundary",
        {{"capture_id", g_rendererObjectCaptureId},
         {"renderer_node", g_rendererObjectSource},
         {"list_argument", listAddress},
         {"node_word_30", contextNodeValid
                              ? memory.read32(g_rendererObjectSource + 0x30u)
                              : 0u},
         {"node_word_34", contextNodeValid
                              ? memory.read32(g_rendererObjectSource + 0x34u)
                              : 0u},
         {"node_word_38", objectCommandList},
         {"node_word_3c", contextNodeValid
                              ? memory.read32(g_rendererObjectSource + 0x3cu)
                              : 0u},
         {"node_word_40", contextNodeValid
                              ? memory.read32(g_rendererObjectSource + 0x40u)
                              : 0u},
         {"node_word_44", contextNodeValid
                              ? memory.read32(g_rendererObjectSource + 0x44u)
                              : 0u},
         {"retained_object_context", currentObjectList ? 1u : 0u}});
  }
  if (!currentObjectList) {
    g_rendererScaleShift = -1;
    g_rendererObjectSource = 0u;
    g_rendererObjectCaptureId = 0u;
    g_rendererLogicalObject = 0u;
    g_rendererOrderingTableBasesCaptured = false;
    g_rendererRotationCaptureNext = kNoRendererRotationCapture;
  }
  const int32_t originalCount =
      static_cast<int16_t>(memory.read16(listAddress));
  const ScenerySelectionMode mode = scenerySelectionMode();
  g_scenerySelection.listAddress = listAddress;
  g_scenerySelection.mode = mode;
  g_scenerySelection.resolved = true;
  g_scenerySelection.virtualActive = false;
  g_scenerySelection.entries.clear();
  g_sceneryProjectionIdentity = {};
  // A prior empty/aborted renderer may never have reached its first RTPT.
  // A new list is an exact construction boundary and supersedes that pending
  // host-only capture without touching guest state.
  g_pendingResidentPresentationCapture = {};
  const char *traceIdentity =
      std::getenv("PS1_TRACE_CRASH3_SCENERY_PROJECTION_IDENTITY");
  const bool identityTrace = traceIdentity != nullptr &&
                             *traceIdentity != '\0' && *traceIdentity != '0';
  const bool occlusionIdentity =
      mode == ScenerySelectionMode::GeometricResident &&
      presentationDepthCorrections() &&
      g_sceneryOcclusionMode != SceneryOcclusionMode::Off;
  g_sceneryProjectionIdentity.enabled =
      active() && (identityTrace || occlusionIdentity ||
                   g_residentPresentationMode != ResidentPresentationMode::Off);
  if (g_sceneryProjectionIdentity.enabled) {
    g_sceneryProjectionIdentity.traceEnabled = metrics::traceEnabled();
    g_sceneryProjectionIdentity.captureId = nextResidentPresentationCaptureId();
  }

  const bool captureResident =
      g_residentPresentationMode != ResidentPresentationMode::Off &&
      widescreen16x9() && mainSceneryProjection(*ctx);
  if (mode == ScenerySelectionMode::Original && !captureResident)
    return originalCount;

  std::array<ResidentSceneryWorld, kMaxSceneryWorlds> worlds{};
  uint32_t worldCount = 0;
  uint32_t residentEntryCount = 0;
  uint32_t residentFaceCount = 0;
  bool residentValid = readResidentScenery(
      memory, worlds, worldCount, residentEntryCount, residentFaceCount);
  bool residentPartial = false;
  if (!residentValid && captureResident &&
      mode == ScenerySelectionMode::Original) {
    worlds = {};
    worldCount = 0u;
    residentEntryCount = 0u;
    residentFaceCount = 0u;
    residentValid = readResidentSceneryPartial(
        memory, worlds, worldCount, residentEntryCount, residentFaceCount);
    residentPartial = residentValid;
    if (residentPartial)
      metrics::count("host.crash3.resident_sidecar.partial_table_capture");
  }
  if (mode != ScenerySelectionMode::Original) {
    traceSceneryCoverage(memory, listAddress, originalCount, mode, worlds,
                         worldCount, residentEntryCount, residentFaceCount,
                         residentValid);
  }
  if (captureResident) {
    if (residentValid) {
      g_pendingResidentPresentationCapture.active = true;
      g_pendingResidentPresentationCapture.memory = &memory;
      g_pendingResidentPresentationCapture.listAddress = listAddress;
      g_pendingResidentPresentationCapture.worlds = worlds;
      g_pendingResidentPresentationCapture.worldCount = worldCount;
      g_pendingResidentPresentationCapture.residentFaceCount =
          residentFaceCount;
      g_pendingResidentPresentationCapture.captureId =
          g_sceneryProjectionIdentity.captureId;
      constexpr uint32_t kRendererMaterialLodDepthShiftOffset = 0x30u;
      if (ctx->r[6] <= UINT32_MAX - kRendererMaterialLodDepthShiftOffset) {
        const uint32_t address =
            ctx->r[6] + kRendererMaterialLodDepthShiftOffset;
        if (isRamRange(address, sizeof(uint32_t))) {
          g_pendingResidentPresentationCapture.materialLodDepthShift =
              memory.read32(address);
        }
      }
      g_pendingResidentPresentationCapture.materialAnimationState = ctx->r[7];
      metrics::setState("host.crash3.resident_sidecar.capture_pending", 1);
      metrics::setState("host.crash3.resident_sidecar.capture_valid", 0);
      metrics::setState("host.crash3.resident_sidecar.capture_partial",
                        residentPartial ? 1 : 0);
    } else {
      metrics::setState("host.crash3.resident_sidecar.capture_pending", 0);
      metrics::setState("host.crash3.resident_sidecar.capture_valid", 0);
      metrics::setState("host.crash3.resident_sidecar.capture_partial", 0);
      metrics::count("host.crash3.resident_sidecar.capture_rejected");
    }
  }

  if (mode == ScenerySelectionMode::Original)
    return originalCount;

  if (mode == ScenerySelectionMode::Adjacent ||
      mode == ScenerySelectionMode::Symmetric ||
      mode == ScenerySelectionMode::Geometric ||
      mode == ScenerySelectionMode::GeometricResident) {
    AdjacentMergeStats stats;
    const bool geometric = mode == ScenerySelectionMode::Geometric ||
                           mode == ScenerySelectionMode::GeometricResident;
    const bool includeResidentGeometry =
        mode == ScenerySelectionMode::GeometricResident;
    const bool directResident = mode == ScenerySelectionMode::GeometricResident;
    const bool sourcePathUnion = mode == ScenerySelectionMode::Symmetric ||
                                 mode == ScenerySelectionMode::Geometric;
    uint32_t leftUsed = 0u;
    uint32_t rightUsed = 0u;
    const uint32_t radius =
        mode == ScenerySelectionMode::Geometric
            ? static_cast<uint32_t>(g_sceneryPathCache.states.size())
        : directResident ? 0u
                         : adjacentSceneryDeltaCount();
    const bool traceTiming = metrics::traceEnabled();
    const auto sourceBuildStart = traceTiming
                                      ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    const bool unionMerged =
        residentValid &&
        (directResident ? buildCurrentSceneryList(memory, listAddress, worlds,
                                                  worldCount, stats)
         : sourcePathUnion
             ? buildSymmetricSceneryList(memory, listAddress, worlds,
                                         worldCount, stats, leftUsed, rightUsed,
                                         radius)
             : buildAdjacentSceneryList(memory, listAddress, worlds, worldCount,
                                        stats));
    const auto sourceBuildEnd = traceTiming
                                    ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    SceneryGeometryStats geometryStats;
    const auto geometryFilterStart = sourceBuildEnd;
    const bool merged =
        unionMerged &&
        (!geometric ||
         (widescreen16x9() && filterSceneryUnionByPresentationFrustum(
                                  memory, *ctx, listAddress, worlds, worldCount,
                                  includeResidentGeometry, geometryStats)));
    const auto geometryFilterEnd =
        traceTiming ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
    const auto elapsedUs = [](auto begin, auto end) -> int64_t {
      return std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
          .count();
    };
    const int64_t sourceBuildUs =
        traceTiming ? elapsedUs(sourceBuildStart, sourceBuildEnd) : 0;
    const int64_t geometryFilterUs =
        traceTiming ? elapsedUs(geometryFilterStart, geometryFilterEnd) : 0;
    if (geometric && unionMerged && !merged)
      g_scenerySelection.entries.clear();
    g_scenerySelection.virtualActive = merged;
    traceSceneryEntryCatalog(memory, listAddress, originalCount, merged);
    if (traceTiming) {
      std::vector<uint16_t> current;
      const bool currentValid =
          readDecodedSceneryList(memory, listAddress, current);
      const uint32_t unionEntries =
          merged ? static_cast<uint32_t>(g_scenerySelection.entries.size())
                 : static_cast<uint32_t>(std::max(originalCount, 0));
      const uint32_t unionFaces =
          merged         ? sceneryFaceCount(g_scenerySelection.entries)
          : currentValid ? sceneryFaceCount(current)
                         : 0u;
      const SceneryListSnapshot *latest =
          sourcePathUnion || directResident ||
                  g_sceneryDeltaHistory.preceding.empty()
              ? nullptr
              : &g_sceneryDeltaHistory.preceding.back();
      metrics::trace(
          includeResidentGeometry
              ? "game.crash3.scenery_geometric_resident_window"
          : geometric       ? "game.crash3.scenery_geometric_window"
          : sourcePathUnion ? "game.crash3.scenery_symmetric_window"
                            : "game.crash3.scenery_adjacent_window",
          {{"list_address", listAddress},
           {"requested_deltas", radius},
           {"direct_resident", directResident ? 1u : 0u},
           {"source_build_us", sourceBuildUs},
           {"geometry_filter_us", geometryFilterUs},
           {"available_deltas",
            static_cast<int64_t>(
                sourcePathUnion  ? g_sceneryPathCache.states.size()
                : directResident ? 0u
                                 : g_sceneryDeltaHistory.preceding.size())},
           {"used_deltas", stats.snapshotsUsed},
           {"left_used", leftUsed},
           {"right_used", rightUsed},
           {"current_index",
            sourcePathUnion ? g_sceneryPathCache.currentIndex : 0u},
           {"state_count",
            sourcePathUnion ? g_sceneryPathCache.stateCount : 0u},
           {"original_entries", originalCount},
           {"union_entries", unionEntries},
           {"entries_added", stats.entriesAdded},
           {"quad_states_upgraded", stats.quadStatesUpgraded},
           {"reordered_anchors", stats.reorderedAnchors},
           {"original_faces", currentValid ? sceneryFaceCount(current) : 0u},
           {"union_faces", unionFaces},
           {"failure", static_cast<uint32_t>(stats.failure)},
           {"active", merged ? 1u : 0u},
           {"latest_source", latest != nullptr ? latest->sourceAddress : 0u},
           {"latest_table_cursor",
            latest != nullptr ? latest->tableCursor : 0u},
           {"latest_direction",
            latest != nullptr && latest->forward ? 1u : 0u}});
      if (geometric) {
        metrics::trace(
            includeResidentGeometry
                ? "game.crash3.scenery_geometric_resident_filter"
                : "game.crash3.scenery_geometric_filter",
            {{"list_address", listAddress},
             {"source_union_entries", geometryStats.sourceUnionEntries},
             {"tested_faces", geometryStats.testedFaces},
             {"retained_faces", geometryStats.retainedFaces},
             {"rejected_faces", geometryStats.rejectedFaces},
             {"retained_entries", geometryStats.retainedEntries},
             {"failure", geometryStats.failure},
             {"failure_world", geometryStats.failureWorld},
             {"expected", geometryStats.expected},
             {"observed", geometryStats.observed},
             {"original_positive", geometryStats.originalPositive},
             {"original_negative", geometryStats.originalNegative},
             {"original_degenerate", geometryStats.originalDegenerate},
             {"added_positive", geometryStats.addedPositive},
             {"added_negative", geometryStats.addedNegative},
             {"added_degenerate", geometryStats.addedDegenerate},
             {"retained_positive", geometryStats.retainedPositive},
             {"retained_negative", geometryStats.retainedNegative},
             {"resident_quad_halves_tested",
              geometryStats.residentQuadHalvesTested},
             {"resident_quad_halves_retained",
              geometryStats.residentQuadHalvesRetained},
             {"resident_quad_halves_rejected",
              geometryStats.residentQuadHalvesRejected},
             {"resident_triangles_tested",
              geometryStats.residentTrianglesTested},
             {"resident_triangles_retained",
              geometryStats.residentTrianglesRetained},
             {"resident_triangles_rejected",
              geometryStats.residentTrianglesRejected},
             {"resident_quads_tested", geometryStats.residentQuadsTested},
             {"resident_quads_retained", geometryStats.residentQuadsRetained},
             {"resident_quads_rejected", geometryStats.residentQuadsRejected},
             {"resident_absent_quad_halves_tested",
              geometryStats.residentAbsentQuadHalvesTested},
             {"resident_absent_quad_halves_retained",
              geometryStats.residentAbsentQuadHalvesRetained},
             {"resident_absent_quad_halves_rejected",
              geometryStats.residentAbsentQuadHalvesRejected},
             {"resident_entries_added", geometryStats.residentEntriesAdded},
             {"transformed_vertices", geometryStats.transformedVertices},
             {"vertex_cache_hits", geometryStats.vertexCacheHits},
             {"resident_seed_faces", geometryStats.residentSeedFaces},
             {"resident_closure_faces", geometryStats.residentClosureFaces},
             {"resident_candidate_components",
              geometryStats.residentCandidateComponents},
             {"resident_retained_components",
              geometryStats.residentRetainedComponents},
             {"resident_frustum_faces", geometryStats.residentFrustumFaces},
             {"resident_guard_band_faces",
              geometryStats.residentGuardBandFaces},
             {"resident_all_frustum", geometryStats.residentAllFrustum},
             {"scenery_cull_guard_pixels", geometryStats.cullGuardPixels},
             {"topology_faces", geometryStats.topologyFaces},
             {"topology_shared_edges", geometryStats.topologySharedEdges},
             {"topology_rebuilt", geometryStats.topologyRebuilt},
             {"active", merged ? 1u : 0u},
             {"widescreen", widescreen16x9() ? 1u : 0u}});
      }
    }
    return merged ? static_cast<int32_t>(g_scenerySelection.entries.size())
                  : originalCount;
  }

  if (mode != ScenerySelectionMode::Bypass || !residentValid)
    return originalCount;
  if (residentEntryCount > kMaxDecodedSceneryEntries) {
    if (metrics::traceEnabled()) {
      metrics::trace("game.crash3.scenery_bypass_rejected",
                     {{"resident_entries", residentEntryCount},
                      {"decoded_list_capacity", kMaxDecodedSceneryEntries}});
    }
    return originalCount;
  }
  buildResidentSceneryList(worlds, worldCount, residentEntryCount);
  g_scenerySelection.virtualActive = true;
  return static_cast<int32_t>(g_scenerySelection.entries.size());
}

uint32_t scenerySelectionEntry(recomp_context *ctx,
                               uint32_t cursorAddress) noexcept {
  if (ctx == nullptr || ctx->mem == nullptr)
    return 0;
  const Memory &memory = *ctx->mem;
  const auto observedEntry = [&](uint32_t value) {
    return observeSceneryProjectionEntry(ctx->pc, cursorAddress, value);
  };
  // scenerySelectionCount resolves the process environment once per list.
  // Avoid getenv/strcmp at every one of the decoder's five threaded entry
  // loads: a busy warp-room frame executes this hook thousands of times.
  const ScenerySelectionMode mode = g_scenerySelection.listAddress != 0u
                                        ? g_scenerySelection.mode
                                        : scenerySelectionMode();
  if (mode != ScenerySelectionMode::Bypass &&
      mode != ScenerySelectionMode::Adjacent &&
      mode != ScenerySelectionMode::Symmetric &&
      mode != ScenerySelectionMode::Geometric &&
      mode != ScenerySelectionMode::GeometricResident)
    return observedEntry(memory.read16(cursorAddress));

  // A savestate can resume inside the decoder after host-only state was reset.
  // Reconstruct the deterministic resident list from the guest's saved list
  // start rather than serialising an implementation detail.
  if ((mode == ScenerySelectionMode::Bypass ||
       mode == ScenerySelectionMode::GeometricResident) &&
      !g_scenerySelection.resolved) {
    const uint32_t listStart = memory.read32(kSceneryDecoderListStartAddress);
    if (listStart < 4u)
      return observedEntry(memory.read16(cursorAddress));
    scenerySelectionCount(ctx, listStart - 4u);
  }
  if (!g_scenerySelection.virtualActive)
    return observedEntry(memory.read16(cursorAddress));

  const uint32_t first = g_scenerySelection.listAddress + 4u;
  if (cursorAddress < first || ((cursorAddress - first) & 1u) != 0)
    return observedEntry(memory.read16(cursorAddress));
  const uint32_t index = (cursorAddress - first) / 2u;
  if (index >= g_scenerySelection.entries.size())
    return observedEntry(memory.read16(cursorAddress));
  return observedEntry(g_scenerySelection.entries[index]);
}

int32_t screen2DPresentationX(int32_t x, int32_t drawAreaLeft,
                              int32_t drawAreaRight,
                              int32_t packetAnchorX) noexcept {
  if (!widescreen16x9())
    return x;

  // Crash double-buffers two 512-wide pages in the 1024-wide VRAM. Prefer
  // the draw area as the page witness and use the packet anchor only when an
  // unusually broad draw area spans both pages.
  int32_t center = 256;
  if (drawAreaLeft >= 512)
    center = 768;
  else if (drawAreaRight > 511 && packetAnchorX >= 512)
    center = 768;

  const int64_t delta = static_cast<int64_t>(x) - center;
  const int64_t magnitude = delta < 0 ? -delta : delta;
  const int64_t scaledMagnitude = (magnitude * 3 + 2) / 4;
  const int64_t scaledDelta = delta < 0 ? -scaledMagnitude : scaledMagnitude;
  return static_cast<int32_t>(static_cast<int64_t>(center) + scaledDelta);
}

void attachProjectionTag(const CPUContext &ctx, uint8_t vectorIndex,
                         PGXPValue &projection) noexcept {
  if (!active())
    return;

  static const bool traceObjectPresenterProjection = [] {
    const char *value = std::getenv("PS1_TRACE_CRASH3_OBJECT_PRESENTER");
    return value != nullptr && *value != '\0' && *value != '0';
  }();
  if (traceObjectPresenterProjection && g_rendererObjectCaptureId != 0u &&
      vectorIndex == 0u) {
    metrics::trace(
        "game.crash3.object_projection_candidate",
        {{"capture_id", g_rendererObjectCaptureId},
         {"renderer_node", g_rendererObjectSource},
         {"gte_pc", ctx.pc},
         {"scratch_renderer", ctx.r[3] == kRendererScratchBase ? 1u : 0u},
         {"scale_shift", g_rendererScaleShift}});
  }

  // Crash's assembly polygon renderer repurposes $ra as a temporary, so it is
  // explicitly excluded. For the scaled object renderer the identity instead
  // carries the linked render-list node captured before those registers are
  // destroyed. It is an instance discriminator, not yet a presentation rule.
  const bool scaledObjectProjection = ctx.r[3] == kRendererScratchBase &&
                                      g_rendererScaleShift >= 0 &&
                                      isDynamicObjectRendererProjection(ctx.pc);
  if (scaledObjectProjection && vectorIndex == 0u &&
      !g_rendererOrderingTableBasesCaptured &&
      g_objectPresentationMode != ObjectPresentationMode::Off &&
      g_rendererObjectCaptureId != 0u) {
    const auto *recomp = static_cast<const recomp_context *>(&ctx);
    if (recomp->mem != nullptr) {
      const uint32_t positive =
          recomp->mem->read32(kRendererScratchBase + 244u);
      const uint32_t nonPositive =
          recomp->mem->read32(kRendererScratchBase + 248u);
      std::scoped_lock lock(g_objectPresentationMutex);
      if (g_objectPresentationBuildingActive &&
          g_objectPresentationBuilding.captureId == g_rendererObjectCaptureId) {
        uint8_t modelType = 0;
        if (auto *node = currentObjectRendererNodeLocked(&modelType)) {
          // Refresh the final compact node manifest at its first intrinsic
          // projection. The mutable command, source-colour and pre-overwrite
          // scratch streams were already detached at 0x8003F76C.
          for (uint32_t index = 0u; index < node->byteCount / sizeof(uint32_t);
               ++index) {
            node->words[index] = recomp->mem->read32(g_rendererObjectSource +
                                                     index * sizeof(uint32_t));
          }
          node->orderingTableBasesCaptured = true;
          // Per-object setup can install lighting after the common projection
          // transform. Freeze the consumed light/far-colour controls at the
          // first RTPS/RTPT, not the preceding node's lighting state.
          std::copy_n(ctx.cop2c + 8u, 16u, node->gteControlWords.begin() + 8u);
          // 8003E054 projects the authored bounding box before deciding
          // whether to call the polygon stream at all. Prepare the exact
          // 8003F73C prefix copy on the host at that boundary: otherwise a
          // complete off-4:3 object never acquires a presentation source.
          // No guest scratch, selector, stream cursor or culling bit changes.
          // If the native stream runs, its later 8003F76C capture supersedes
          // this preparation with the actually consumed mutable sources.
          const uint32_t primitive = node->words[0x2cu / 4u];
          static const bool legacyConstruction = [] {
            const char *control = std::getenv("PS1_CRASH3_OBJECT_CONSTRUCTION");
            return control != nullptr && std::strcmp(control, "legacy") == 0;
          }();
          if (!legacyConstruction && !node->type1StreamsCaptured &&
              ctx.pc == UINT32_C(0x8003E094) && modelType == 1u &&
              (primitive == UINT32_C(0x80040C1C) ||
               primitive == UINT32_C(0x80040C00) ||
               (primitive == UINT32_C(0x8003FC00) &&
                node->words[0x28u / 4u] == UINT32_C(0x8003D248)) ||
               primitive == UINT32_C(0x8003DC00))) {
            captureType1RendererStreams(*node, *recomp->mem,
                                        kRendererScratchBase + 320u, false);
            metrics::count(
                node->type1StreamsCaptured
                    ? "host.crash3.object_source_preflight_prepared"
                    : "host.crash3.object_source_preflight_rejected");
          }
          node->orderingTableBasePositive = positive;
          node->orderingTableBaseNonPositive = nonPositive;
          g_rendererOrderingTableBasesCaptured = true;
          metrics::count(
              "host.crash3.object_sidecar.ordering_table_bases_captured");
          if (traceObjectPresenterProjection) {
            metrics::trace(
                "game.crash3.object_ordering_table_bases",
                {{"capture_id", g_rendererObjectCaptureId},
                 {"renderer_node", g_rendererObjectSource},
                 {"gte_pc", ctx.pc},
                 {"type1_streams_captured",
                  node->type1StreamsCaptured ? 1u : 0u},
                 {"type1_command_words",
                  static_cast<int64_t>(node->type1CommandWords.size())},
                 {"type1_color_words",
                  static_cast<int64_t>(node->type1ColorWords.size())},
                 {"positive", positive},
                 {"non_positive", nonPositive}});
          }
        }
      }
    }
  }
  const bool residentSceneryProjection =
      ctx.r[3] == kRendererScratchBase &&
      isResidentSceneryRendererProjection(ctx.pc);
  const uint32_t scaleToken =
      scaledObjectProjection ? static_cast<uint32_t>(g_rendererScaleShift) + 1u
                             : 0u;
  const uint32_t objectSource =
      scaledObjectProjection ? g_rendererObjectSource : 0u;
  const bool extendedObjectIdentity =
      scaledObjectProjection && g_rendererObjectCaptureId != 0u &&
      isCanonicalGuestRamRange(objectSource, kCrash3RendererNodeBytes, 4u);
  const uint32_t encodedObjectSource =
      extendedObjectIdentity
          ? ((g_rendererObjectCaptureId & kObjectCaptureMask) << 21u) |
                (Memory::toPhysical(objectSource) & kObjectSourcePhysicalMask)
          : objectSource;

  // Low word: 24-bit SCES-01420 executable offset, six-bit scale token, then
  // RTPT lane. In the intrinsic object schema the high word contains the
  // node's exact 21-bit RAM address plus an 11-bit capture identity. Capture
  // values that could form a canonical KSEG pointer are skipped, making the
  // schema self-identifying without stealing a scale bit. The legacy/off high
  // word remains the original full node address. No projected coordinate or
  // observed visibility participates.
  projection.gameProfileCallsite =
      (static_cast<uint64_t>(encodedObjectSource) << 32u) |
      (static_cast<uint64_t>(ctx.pc & UINT32_C(0x00FFFFFF)) << 8u) |
      (static_cast<uint64_t>(scaleToken) << 2u) |
      static_cast<uint64_t>(vectorIndex & UINT8_C(3));
  if (extendedObjectIdentity) {
    // projectionId is already the exact process-local join carried to Vulkan.
    // Keep the command out of PGXPValue/projectionContext: RTPT's lanes can
    // later be combined with older FIFO projections and must retain their
    // original context equality contract. One atomic word makes overwrite and
    // lookup indivisible; the 2^18-event horizon greatly exceeds the bounded
    // eight-frame presentation queue.
    const uint64_t journalValue =
        (static_cast<uint64_t>(static_cast<uint32_t>(projection.projectionId))
         << 32u) |
        ctx.r[8];
    g_objectProjectionCommandJournal[static_cast<std::size_t>(
                                         projection.projectionId) &
                                     kObjectProjectionCommandJournalMask]
        .store(journalValue, std::memory_order_relaxed);
    const uint64_t objectJournalValue =
        (static_cast<uint64_t>(static_cast<uint32_t>(projection.projectionId))
         << 32u) |
        g_rendererLogicalObject;
    g_objectProjectionLogicalObjectJournal[static_cast<std::size_t>(
                                               projection.projectionId) &
                                           kObjectProjectionCommandJournalMask]
        .store(objectJournalValue, std::memory_order_relaxed);
  }

  if (residentSceneryProjection && projection.modelVertexSource == 0 &&
      g_sceneryProjectionIdentity.enabled &&
      g_sceneryProjectionIdentity.currentValid &&
      g_sceneryProjectionIdentity.captureId != 0u) {
    // Payload layout: six-bit schema, exact 16-bit SLST entry and 26-bit
    // host-only capture identity. The GTE PC/lane already travels in
    // gameProfileCallsite; using this payload for the capture joins the
    // sidecar to its packet cohort without guessing a frame offset.
    const uint64_t payload =
        kSceneryProjectionIdentityFlag |
        (static_cast<uint64_t>(g_sceneryProjectionIdentity.current) << 26u) |
        g_sceneryProjectionIdentity.captureId;
    projection.modelVertexSource = kTagMarker | payload;
    return;
  }

  if (projection.modelVertexSource == 0u) {
    if (const auto identity = waterProjectionIdentity(ctx)) {
      projection.modelVertexSource =
          kTagMarker | kProceduralProjectionIdentityFlag | *identity;
      return;
    }
  }

  if (projection.modelVertexSource == 0u) {
    if (const auto identity = terrainProjectionIdentity(ctx)) {
      projection.modelVertexSource =
          kTagMarker | kProceduralProjectionIdentityFlag | *identity;
      return;
    }
  }

  // Keep the original dynamic carrier during migration: the existing 47
  // title/intro corrections were certified with it. New demo work consumes
  // gameProfileCallsite instead.
  if (projection.modelVertexSource != 0)
    return;

  uint64_t hash = UINT64_C(1469598103934665603);
  hash32(hash, ctx.pc);
  // These are Crash 3's renderer work/stream pointers at its GTE call-sites.
  // They describe the construction lane, not the projected numeric vertex.
  hash32(hash, ctx.r[3]);
  for (uint8_t reg = 16; reg <= 22; ++reg)
    hash32(hash, ctx.r[reg]);
  uint64_t payload = hash & kTagPayloadMask;
  if (payload == 0)
    payload = 1;
  projection.modelVertexSource = kTagMarker | payload;
}

std::optional<uint64_t> decodeProjectionTag(uint64_t carrier) noexcept {
  if ((carrier & kTagMarkerMask) != kTagMarker)
    return std::nullopt;
  return carrier & kTagPayloadMask;
}

std::optional<uint64_t>
proceduralProjectionOrdinal(uint64_t gameProfileTag) noexcept {
  return proceduralProjectionOrdinalFromTag(gameProfileTag);
}

std::optional<uint64_t>
proceduralProjectionIdentity(uint64_t gameProfileTag) noexcept {
  return proceduralProjectionIdentityFromTag(gameProfileTag);
}

std::optional<uint64_t>
proceduralProjectionIdentity(uint64_t gameProfileCallsite,
                             uint64_t gameProfileTag) noexcept {
  const std::optional<uint64_t> identity =
      proceduralProjectionIdentityFromTag(gameProfileTag);
  if (!identity.has_value())
    return std::nullopt;
  const uint32_t pc = projectionCallsiteGtePc(gameProfileCallsite);
  const uint64_t schema = *identity & ~kProceduralProjectionDataMask;
  if ((pc == UINT32_C(0x80043E00) &&
       schema == kProceduralProjectionTerrainGridSchema) ||
      (pc == UINT32_C(0x80044938) &&
       schema == kProceduralProjectionWaterGridSchema)) {
    return identity;
  }
  return std::nullopt;
}

void observeGteControlWrite(const CPUContext &ctx, uint8_t reg) noexcept {
  uint32_t writtenValue = ctx.cop2c[reg & 0x1fu];
  if (ctx.pc == UINT32_C(0x80040F94) && (reg & 0x1fu) == GTE_TRX)
    writtenValue = ctx.r[24];
  else if (ctx.pc == UINT32_C(0x80040F98) && (reg & 0x1fu) == GTE_TRY)
    writtenValue = ctx.r[5];
  else if (ctx.pc == UINT32_C(0x80040FA0) && (reg & 0x1fu) == GTE_TRZ)
    writtenValue = ctx.r[6];
  observeGteControlWrite(ctx, reg, writtenValue);
}

void observeGteControlWrite(const CPUContext &ctx, uint8_t reg,
                            uint32_t writtenValue) noexcept {
  // func_80040E38 stores r4 as the renderer's normalization exponent and then
  // writes shifted TRX at this exact instruction. Capture the categorical
  // exponent before later scratchpad renderer code repurposes r4 and $ra.
  if (!active())
    return;
  const uint8_t controlReg = reg & 0x1fu;
  const bool rendererRotation =
      controlReg <= GTE_RT33 &&
      ctx.pc == UINT32_C(0x80042120) +
                    static_cast<uint32_t>(controlReg) * sizeof(uint32_t);
  if (rendererRotation) {
    if (g_rendererRotationCaptureNext == kNoRendererRotationCapture)
      return;
    if (controlReg != g_rendererRotationCaptureNext) {
      g_rendererRotationCaptureNext = kNoRendererRotationCapture;
      metrics::count(
          "host.crash3.object_sidecar.gte_rotation_sequence_mismatch");
      return;
    }
    if (g_objectPresentationMode == ObjectPresentationMode::Off ||
        g_rendererObjectCaptureId == 0u) {
      g_rendererRotationCaptureNext = kNoRendererRotationCapture;
      return;
    }
    const uint32_t canonicalValue =
        controlReg == GTE_RT33 ? static_cast<uint32_t>(static_cast<int32_t>(
                                     static_cast<int16_t>(writtenValue)))
                               : writtenValue;
    std::scoped_lock lock(g_objectPresentationMutex);
    if (!g_objectPresentationBuildingActive ||
        g_objectPresentationBuilding.captureId != g_rendererObjectCaptureId) {
      g_rendererRotationCaptureNext = kNoRendererRotationCapture;
      return;
    }
    if (auto *node = currentObjectRendererNodeLocked()) {
      node->gteControlWords[controlReg] = canonicalValue;
      metrics::count("host.crash3.object_sidecar.gte_rotation_word");
      if (controlReg == GTE_RT33) {
        g_rendererRotationCaptureNext = kNoRendererRotationCapture;
        metrics::count(
            "host.crash3.object_sidecar.gte_rotation_sequence_complete");
      } else {
        g_rendererRotationCaptureNext = controlReg + 1u;
      }
      return;
    }
    g_rendererRotationCaptureNext = kNoRendererRotationCapture;
    return;
  }
  const bool rendererTry =
      ctx.pc == UINT32_C(0x80040F98) && controlReg == GTE_TRY;
  const bool rendererTrz =
      ctx.pc == UINT32_C(0x80040FA0) && controlReg == GTE_TRZ;
  if (rendererTry || rendererTrz) {
    if (g_objectPresentationMode == ObjectPresentationMode::Off ||
        g_rendererObjectCaptureId == 0u) {
      return;
    }
    std::scoped_lock lock(g_objectPresentationMutex);
    if (!g_objectPresentationBuildingActive ||
        g_objectPresentationBuilding.captureId != g_rendererObjectCaptureId) {
      return;
    }
    if (auto *node = currentObjectRendererNodeLocked()) {
      node->gteControlWords[controlReg] = writtenValue;
      metrics::count("host.crash3.object_sidecar.gte_translation_word");
      if (rendererTrz) {
        g_rendererRotationCaptureNext = g_rendererScaleShift > 0
                                            ? GTE_RT11RT12
                                            : kNoRendererRotationCapture;
        if (g_rendererRotationCaptureNext != kNoRendererRotationCapture) {
          metrics::count(
              "host.crash3.object_sidecar.gte_rotation_sequence_armed");
        }
      }
      return;
    }
    if (rendererTrz)
      g_rendererRotationCaptureNext = kNoRendererRotationCapture;
    return;
  }
  if (ctx.pc != UINT32_C(0x80040F94) || controlReg != GTE_TRX)
    return;
  g_rendererScaleShift = ctx.r[4] <= 31u ? static_cast<int32_t>(ctx.r[4]) : -1;
  g_rendererObjectSource = ctx.r[29];
  g_rendererObjectCaptureId = 0u;
  g_rendererLogicalObject = 0u;
  g_rendererOrderingTableBasesCaptured = false;
  g_rendererRotationCaptureNext = kNoRendererRotationCapture;
  uint32_t logicalObject = 0u;

  const auto *recomp = static_cast<const recomp_context *>(&ctx);
  if (g_objectPresentationMode != ObjectPresentationMode::Off &&
      recomp->mem != nullptr) {
    const Memory &memory = *recomp->mem;
    const bool validNode = isCanonicalGuestRamRange(
        g_rendererObjectSource, kCrash3RendererNodeBytes, 4u);
    if (validNode) {
      logicalObject = memory.read32(g_rendererObjectSource +
                                    kCrash3RendererNodeObjectOffset);
    }
    const bool validObject =
        validNode && isCrash3PresentationObject(memory, logicalObject);
    std::scoped_lock lock(g_objectPresentationMutex);
    if (g_objectPresentationBuildingActive)
      captureCompleteObjectRendererListLocked(memory);
    if (validObject && g_objectPresentationBuildingActive) {
      auto object =
          std::find_if(g_objectPresentationBuilding.objects.begin(),
                       g_objectPresentationBuilding.objects.end(),
                       [&](const ObjectPresentationObject &candidate) {
                         return candidate.address == logicalObject;
                       });
      if (object != g_objectPresentationBuilding.objects.end()) {
        const uint32_t nodeBytes =
            rendererNodeBytesForModelType(object->modelType);
        auto existingNode = std::find_if(
            object->rendererNodes.begin(), object->rendererNodes.end(),
            [&](const ObjectPresentationRendererNode &candidate) {
              return candidate.address == g_rendererObjectSource;
            });
        bool nodeCaptured = existingNode != object->rendererNodes.end();
        uint32_t changedNodeWordMask = 0u;
        if (!nodeCaptured &&
            !g_objectPresentationBuilding.rendererListComplete &&
            isCanonicalGuestRamRange(g_rendererObjectSource, nodeBytes, 4u)) {
          object->rendererNodes.push_back(captureObjectRendererNode(
              memory, g_rendererObjectSource, object->modelType));
          object->rendererNodes.back().directBindingDiscovered = true;
          ++g_objectPresentationBuilding.rendererNodeBindings;
          metrics::count("host.crash3.object_sidecar.renderer_node_binding");
          existingNode = std::prev(object->rendererNodes.end());
          nodeCaptured = true;
        }
        if (nodeCaptured) {
          // The complete-list scan necessarily precedes every node's setup.
          // Keep an exact diagnostic of fields which the setup phase mutates
          // before installing the node's GTE transform.  This distinguishes
          // a stale source manifest from a projection/matching error without
          // changing either guest execution or host presentation.
          if (g_traceObjectPresentation) {
            for (uint32_t index = 0u; index < nodeBytes / sizeof(uint32_t);
                 ++index) {
              if (existingNode->words[index] !=
                  memory.read32(g_rendererObjectSource + index * 4u)) {
                changedNodeWordMask |= uint32_t{1} << index;
              }
            }
          }
          // Nodes are linked before their per-object setup has necessarily
          // finished.  Refresh the compact manifest at the exact common
          // transform boundary, then detach type-1's transient command and
          // colour sources from the shared guest scratch lifetime.  The
          // frame-wide RAM image intentionally remains the immutable base for
          // persistent assets; these node-local streams are the pieces which
          // the game can rewrite again later in the very same frame.
          for (uint32_t index = 0u; index < nodeBytes / sizeof(uint32_t);
               ++index) {
            existingNode->words[index] =
                memory.read32(g_rendererObjectSource + index * 4u);
          }
          const bool firstTransform = !existingNode->transformInstalled;
          installObjectRendererTransform(*existingNode, ctx, writtenValue);
          if (firstTransform) {
            ++g_objectPresentationBuilding.rendererTransformBindings;
            metrics::count(
                "host.crash3.object_sidecar.renderer_transform_binding");
          }
          g_rendererObjectCaptureId = g_objectPresentationBuilding.captureId;
          g_rendererLogicalObject = logicalObject;
        }
        if (nodeCaptured && g_traceObjectPresentation) {
          metrics::trace(
              "game.crash3.object_renderer_binding",
              {{"capture_id", g_rendererObjectCaptureId},
               {"renderer_node", g_rendererObjectSource},
               {"object", logicalObject},
               {"model_type", object->modelType},
               {"list_discovered", existingNode->listDiscovered ? 1u : 0u},
               {"transform_installed", 1u},
               {"changed_node_word_mask", changedNodeWordMask},
               {"type1_streams_captured",
                existingNode->type1StreamsCaptured ? 1u : 0u},
               {"type1_command_words",
                static_cast<int64_t>(existingNode->type1CommandWords.size())},
               {"type1_color_words",
                static_cast<int64_t>(existingNode->type1ColorWords.size())},
               {"captured_word_30", existingNode->words[0x30u / 4u]},
               {"live_word_30", memory.read32(g_rendererObjectSource + 0x30u)},
               {"captured_word_34", existingNode->words[0x34u / 4u]},
               {"live_word_34", memory.read32(g_rendererObjectSource + 0x34u)},
               {"captured_word_38", existingNode->words[0x38u / 4u]},
               {"live_word_38", memory.read32(g_rendererObjectSource + 0x38u)},
               {"captured_word_3c", existingNode->words[0x3cu / 4u]},
               {"live_word_3c", memory.read32(g_rendererObjectSource + 0x3cu)},
               {"captured_word_40", existingNode->words[0x40u / 4u]},
               {"live_word_40", memory.read32(g_rendererObjectSource + 0x40u)},
               {"captured_word_44", existingNode->words[0x44u / 4u]},
               {"live_word_44", memory.read32(g_rendererObjectSource + 0x44u)},
               {"captured_word_60", existingNode->words[0x60u / 4u]},
               {"live_word_60", memory.read32(g_rendererObjectSource + 0x60u)},
               {"ordering_table_base_positive",
                existingNode->orderingTableBasePositive},
               {"ordering_table_base_non_positive",
                existingNode->orderingTableBaseNonPositive},
               {"setup_function", ctx.r[22]},
               {"render_function", ctx.r[23]}});
        } else if (!nodeCaptured) {
          ++g_objectPresentationBuilding.rejectedRendererNodes;
          metrics::count("host.crash3.object_sidecar.invalid_renderer_node");
        }
      } else {
        ++g_objectPresentationBuilding.rejectedRendererNodes;
        metrics::count("host.crash3.object_sidecar.uncatalogued_renderer_node");
      }
    } else {
      if (g_objectPresentationBuildingActive)
        ++g_objectPresentationBuilding.rejectedRendererNodes;
      metrics::count("host.crash3.object_sidecar.invalid_renderer_node");
    }
  }
  metrics::trace(
      "crash3.renderer_scale_context",
      {{"scale_shift", g_rendererScaleShift},
       {"object_source", static_cast<int64_t>(g_rendererObjectSource)},
       {"object_capture", g_rendererObjectCaptureId},
       {"logical_object", logicalObject},
       {"setup_function", static_cast<int64_t>(ctx.r[22])},
       {"render_function", static_cast<int64_t>(ctx.r[23])},
       {"r16", static_cast<int64_t>(ctx.r[16])},
       {"r17", static_cast<int64_t>(ctx.r[17])},
       {"r18", static_cast<int64_t>(ctx.r[18])},
       {"r19", static_cast<int64_t>(ctx.r[19])},
       {"r20", static_cast<int64_t>(ctx.r[20])},
       {"r21", static_cast<int64_t>(ctx.r[21])},
       {"r28", static_cast<int64_t>(ctx.r[28])},
       {"r30", static_cast<int64_t>(ctx.r[30])},
       {"shifted_trx", static_cast<int64_t>(ctx.r[24])},
       {"shifted_try", static_cast<int64_t>(ctx.r[5])},
       {"shifted_trz", static_cast<int64_t>(ctx.r[6])}});
}

void observeObjectPacketBinding(const CPUContext &ctx) noexcept {
  if (!active() || g_objectPresentationMode == ObjectPresentationMode::Off ||
      (ctx.pc != UINT32_C(0x80040C7C) && ctx.pc != UINT32_C(0x80040CB4) &&
       ctx.pc != UINT32_C(0x8003EC60) && ctx.pc != UINT32_C(0x8003ECEC) &&
       ctx.pc != UINT32_C(0x8003FD34)) ||
      g_rendererObjectCaptureId == 0u || g_rendererObjectSource == 0u ||
      !isCanonicalGuestRamRange(ctx.r23, 40u, 4u))
    return;
  std::scoped_lock lock(g_objectPresentationMutex);
  if (!g_objectPresentationBuildingActive ||
      g_objectPresentationBuilding.captureId != g_rendererObjectCaptureId)
    return;
  auto *capturedNode = currentObjectRendererNodeLocked();
  if (capturedNode == nullptr)
    return;
  auto &node = *capturedNode;
  if (ctx.pc == UINT32_C(0x8003FD34)) {
    const std::array<int32_t, 3> farColor{
        {static_cast<int32_t>(ctx.cop2c[GTE_RFC]),
         static_cast<int32_t>(ctx.cop2c[GTE_GFC]),
         static_cast<int32_t>(ctx.cop2c[GTE_BFC])}};
    if (node.perVertexFadeCaptured && node.perVertexFadeFarColor != farColor)
      node.perVertexFadeConsistent = false;
    node.perVertexFadeCaptured = true;
    node.perVertexFadeFarColor = farColor;
  }
  const uint32_t table = node.words[0x38u / sizeof(uint32_t)];
  // The stream has a leading colour count; r18 addresses the NEXT word.
  // This identifies repeated commands and screen-coincident faces too.
  if (!node.type1StreamsCaptured || ctx.r18 < table || ctx.r18 - table < 8u ||
      ((ctx.r18 - table) & 3u) != 0u)
    return;
  const uint32_t index = (ctx.r18 - table) / 4u - 2u;
  if (index >= node.type1CommandWords.size() ||
      node.type1CommandWords[index] == UINT32_MAX ||
      (node.type1CommandWords[index] >> 16u) == 0u ||
      node.packetBindings.size() >= 65536u) {
    metrics::count("host.crash3.object_sidecar.packet_binding_invalid");
    return;
  }
  node.packetBindings.push_back({ctx.r23, index, ctx.r8});
  metrics::count("host.crash3.object_sidecar.packet_binding");
  if (g_traceObjectPresentation) {
    metrics::trace("game.crash3.object_packet_binding",
                   {{"capture_id", g_rendererObjectCaptureId},
                    {"renderer_node", node.address},
                    {"packet", ctx.r23},
                    {"constructor_pc", ctx.pc},
                    {"command_index", index},
                    {"draw_command", ctx.r8},
                    {"source_rgb0", ctx.r13},
                    {"source_rgb1", ctx.r14},
                    {"source_rgb2", ctx.r15},
                    {"renderer_r7_low16", static_cast<int16_t>(ctx.r7)},
                    {"rfc", static_cast<int32_t>(ctx.cop2c[GTE_RFC])},
                    {"gfc", static_cast<int32_t>(ctx.cop2c[GTE_GFC])},
                    {"bfc", static_cast<int32_t>(ctx.cop2c[GTE_BFC])}});
  }
}

void observeObjectDistanceCue(const CPUContext &ctx) noexcept {
  // These type-1 primitive lanes load the three source colours, copy
  // renderer scratch+232 to IR0 and reach DPCT at 0x8003CCD4 (specialized)
  // or 0x80040C70/0x8003EC54 (common/semtransparent lanes). The node/capture
  // pair was established by the exact func_80040E38 transform sequence and is
  // also carried by every resulting PGXP owner.  Other DPCT callsites are
  // excluded.
  const bool objectDpct = ctx.pc == UINT32_C(0x8003CCD4) ||
                          ctx.pc == UINT32_C(0x80040C70) ||
                          ctx.pc == UINT32_C(0x8003EC54);
  if (!active() || g_objectPresentationMode == ObjectPresentationMode::Off ||
      !objectDpct || g_rendererObjectCaptureId == 0u ||
      g_rendererObjectSource == 0u) {
    return;
  }

  const int32_t ir0 =
      static_cast<int32_t>(static_cast<int16_t>(ctx.cop2d[GTE_IR0]));
  const std::array<int32_t, 3> farColor{{
      static_cast<int32_t>(ctx.cop2c[GTE_RFC]),
      static_cast<int32_t>(ctx.cop2c[GTE_GFC]),
      static_cast<int32_t>(ctx.cop2c[GTE_BFC]),
  }};

  std::scoped_lock lock(g_objectPresentationMutex);
  if (!g_objectPresentationBuildingActive ||
      g_objectPresentationBuilding.captureId != g_rendererObjectCaptureId) {
    return;
  }
  if (auto *node = currentObjectRendererNodeLocked()) {
    if (!node->distanceCueCaptured) {
      node->distanceCueCaptured = true;
      node->distanceCueIr0 = ir0;
      node->distanceCueFarColor = farColor;
    } else if (node->distanceCueIr0 != ir0 ||
               node->distanceCueFarColor != farColor) {
      node->distanceCueConsistent = false;
      metrics::count("host.crash3.object_sidecar.distance_cue_conflict");
    }
    ++node->distanceCueProofs;
    metrics::count("host.crash3.object_sidecar.distance_cue_proof");
    if (g_traceObjectPresentation) {
      metrics::trace("game.crash3.object_distance_cue_triangle",
                     {{"capture_id", g_rendererObjectCaptureId},
                      {"renderer_node", g_rendererObjectSource},
                      {"proof", node->distanceCueProofs},
                      {"ir0", ir0},
                      {"rfc", farColor[0]},
                      {"gfc", farColor[1]},
                      {"bfc", farColor[2]},
                      {"rgb0", ctx.cop2d[GTE_RGB0]},
                      {"rgb1", ctx.cop2d[GTE_RGB1]},
                      {"rgb2", ctx.cop2d[GTE_RGB2]}});
    }
    if (g_traceObjectPresentation && node->distanceCueProofs == 1u) {
      metrics::trace("game.crash3.object_distance_cue",
                     {{"capture_id", g_rendererObjectCaptureId},
                      {"renderer_node", g_rendererObjectSource},
                      {"ir0", ir0},
                      {"rfc", farColor[0]},
                      {"gfc", farColor[1]},
                      {"bfc", farColor[2]}});
    }
    return;
  }
}

void restoreTransientState(const Memory &memory) noexcept {
  // Checkpoints rendezvous outside the render loop. Scratchpad retains the
  // last exponent, but no render-list node is active until func_80040E38 runs.
  g_rendererObjectSource = 0;
  g_rendererObjectCaptureId = 0;
  g_rendererLogicalObject = 0;
  g_rendererOrderingTableBasesCaptured = false;
  g_rendererRotationCaptureNext = kNoRendererRotationCapture;
  clearProjectedBillboardCentres();
  g_scenerySelection = {};
  g_sceneryDeltaHistory = {};
  g_sceneryPathCache = {};
  g_sceneryProjectionIdentity = {};
  g_residentSceneryTopology = {};
  g_residentCohortJournal = {};
  g_residentCurrentCacheScratch.clear();
  g_sceneryVariantResources.clear();
  clearResidentPresentationSnapshots();
  {
    std::scoped_lock lock(g_residentOrderingTableProofMutex);
    g_residentOrderingTableProof = {};
    g_residentOrderingTableProofCaptures.clear();
  }
  clearObjectPresentationSnapshots();
  if (!active()) {
    g_rendererScaleShift = -1;
    return;
  }
  const uint32_t shift = memory.read32(kRendererScaleShiftAddress);
  g_rendererScaleShift = shift <= 31u ? static_cast<int32_t>(shift) : -1;
}

uint32_t projectionCallsiteGtePc(uint64_t callsite) noexcept {
  if (callsite == 0)
    return 0;
  return UINT32_C(0x80000000) | (static_cast<uint32_t>(callsite) >> 8u);
}

int32_t projectionCallsiteScaleShift(uint64_t callsite) noexcept {
  const uint32_t token = static_cast<uint32_t>(callsite >> 2u) & 0x3fu;
  return token == 0 ? -1 : static_cast<int32_t>(token - 1u);
}

uint8_t projectionCallsiteVectorIndex(uint64_t callsite) noexcept {
  return static_cast<uint8_t>(callsite & UINT64_C(3));
}

uint32_t projectionCallsiteObjectSource(uint64_t callsite) noexcept {
  const uint32_t encoded = static_cast<uint32_t>(callsite >> 32u);
  if (!hasExtendedObjectCallsiteIdentity(callsite))
    return encoded;
  return UINT32_C(0x80000000) | (encoded & kObjectSourcePhysicalMask);
}

uint32_t projectionCallsiteObjectCapture(uint64_t callsite) noexcept {
  if (!hasExtendedObjectCallsiteIdentity(callsite))
    return 0u;
  return (static_cast<uint32_t>(callsite >> 32u) >> 21u) & kObjectCaptureMask;
}

std::optional<uint32_t>
objectProjectionCommand(uint64_t projectionId) noexcept {
  if (projectionId == 0u)
    return std::nullopt;
  const uint64_t journalValue =
      g_objectProjectionCommandJournal[static_cast<std::size_t>(projectionId) &
                                       kObjectProjectionCommandJournalMask]
          .load(std::memory_order_relaxed);
  if (static_cast<uint32_t>(journalValue >> 32u) !=
      static_cast<uint32_t>(projectionId)) {
    return std::nullopt;
  }
  const uint32_t command = static_cast<uint32_t>(journalValue);
  // Object vertex commands have a non-zero upper half. Zero therefore remains
  // an unambiguous unavailable value if a future projection family does not
  // keep r8 live across its GTE instruction.
  if ((command >> 16u) == 0u)
    return std::nullopt;
  return command;
}

std::optional<uint32_t>
objectProjectionLogicalObject(uint64_t projectionId) noexcept {
  if (projectionId == 0u)
    return std::nullopt;
  const uint64_t journalValue = g_objectProjectionLogicalObjectJournal
                                    [static_cast<std::size_t>(projectionId) &
                                     kObjectProjectionCommandJournalMask]
                                        .load(std::memory_order_relaxed);
  if (static_cast<uint32_t>(journalValue >> 32u) !=
      static_cast<uint32_t>(projectionId)) {
    return std::nullopt;
  }
  const uint32_t object = static_cast<uint32_t>(journalValue);
  return object != 0u ? std::optional<uint32_t>{object} : std::nullopt;
}

PresentationSurfaceDecision planPresentedSurfaceDepth(
    std::span<const PresentationSurfaceTriangle> triangles) noexcept {
  using gpu::replay::PacketRoute;
  using gpu::replay::ReplayVertexOrderingTableAnchorExact;

  if (!presentationDepthCorrections())
    return {PresentationSurfaceStatus::Disabled};
  if (triangles.empty() || triangles.size() > 2u)
    return {PresentationSurfaceStatus::IneligiblePacket};

  constexpr uint32_t kOrderingTableEntries = 2048u;
  constexpr double kWorldUnitsPerBucket = 32.0;
  // Depth has no meaningful absolute world origin. Keep one conservative
  // 256-bucket guard band before the nearest OT bucket so a packet whose
  // continuous surface straddles its mean remains positive without clamping.
  constexpr double kPresentationNearGuard = 8192.0;
  std::array<float, 4> uniqueW{};
  std::array<bool, 4> hasUniqueW{};
  uint64_t commonListSource = 0;
  uint64_t commonAnchorSource = 0;
  PresentationProjectionScale commonScale =
      PresentationProjectionScale::Unsupported;
  int32_t commonNormalizationShift = -1;
  uint32_t commonObjectSource = 0;
  std::optional<uint32_t> commonObjectCapture;

  for (const PresentationSurfaceTriangle &triangle : triangles) {
    if (triangle.owner == nullptr)
      return {PresentationSurfaceStatus::IneligiblePacket};
    const gpu::replay::ReplayVisibilityAuditOwner &owner = *triangle.owner;
    if (owner.route != PacketRoute::Projected3D || !owner.fullyPrecise ||
        !owner.authenticDepth || !owner.provenance.validTopology) {
      return {PresentationSurfaceStatus::IneligiblePacket};
    }

    for (std::size_t corner = 0; corner < 3u; ++corner) {
      const gpu::replay::ReplayVertexProvenance &source =
          owner.provenance.vertices[corner];
      if (!source.has(ReplayVertexOrderingTableAnchorExact) ||
          source.dmaListSource == 0 ||
          source.dmaOrderingTableAnchorSource == 0) {
        return {PresentationSurfaceStatus::MissingOrderingTableAnchor};
      }
      if ((commonListSource != 0 && source.dmaListSource != commonListSource) ||
          (commonAnchorSource != 0 &&
           source.dmaOrderingTableAnchorSource != commonAnchorSource)) {
        return {PresentationSurfaceStatus::IncoherentOrderingTableAnchor};
      }
      commonListSource = source.dmaListSource;
      commonAnchorSource = source.dmaOrderingTableAnchorSource;

      const uint32_t pc = projectionCallsiteGtePc(source.gameProfileCallsite);
      const PresentationProjectionScale scale = presentationProjectionScale(pc);
      if (scale == PresentationProjectionScale::Unsupported ||
          (commonScale != PresentationProjectionScale::Unsupported &&
           scale != commonScale)) {
        metrics::trace("crash3.presented_surface_unknown_projection",
                       {{"gte_pc", static_cast<int64_t>(pc)},
                        {"existing_scale", static_cast<int64_t>(commonScale)},
                        {"observed_scale", static_cast<int64_t>(scale)},
                        {"triangles", static_cast<int64_t>(triangles.size())}});
        return {PresentationSurfaceStatus::UnsupportedProjectionFamily};
      }
      commonScale = scale;

      if (isDynamicObjectRendererProjection(pc)) {
        const int32_t shift =
            projectionCallsiteScaleShift(source.gameProfileCallsite);
        const uint32_t objectSource =
            projectionCallsiteObjectSource(source.gameProfileCallsite);
        const uint32_t objectCapture =
            projectionCallsiteObjectCapture(source.gameProfileCallsite);
        if (shift < 0 || shift > 29 || objectSource == 0u ||
            (commonNormalizationShift >= 0 &&
             shift != commonNormalizationShift) ||
            (commonObjectSource != 0u && objectSource != commonObjectSource) ||
            (commonObjectCapture.has_value() &&
             objectCapture != *commonObjectCapture)) {
          return {PresentationSurfaceStatus::UnsupportedProjectionFamily};
        }
        commonNormalizationShift = shift;
        commonObjectSource = objectSource;
        commonObjectCapture = objectCapture;
      }

      const uint8_t vertexIndex = owner.provenance.vertexIndices[corner];
      const float w = owner.effectiveProjectiveW[corner];
      if (vertexIndex >= uniqueW.size() || !std::isfinite(w) || w <= 0.0f)
        return {PresentationSurfaceStatus::ProjectiveWOutOfDomain};
      if (hasUniqueW[vertexIndex] &&
          std::bit_cast<uint32_t>(uniqueW[vertexIndex]) !=
              std::bit_cast<uint32_t>(w)) {
        return {PresentationSurfaceStatus::IneligiblePacket};
      }
      uniqueW[vertexIndex] = w;
      hasUniqueW[vertexIndex] = true;
    }
  }

  const uint32_t listAddress =
      static_cast<uint32_t>((commonListSource - 1u) & 0x1fffffu);
  const uint32_t anchorAddress =
      static_cast<uint32_t>((commonAnchorSource - 1u) & 0x1fffffu);
  if (anchorAddress < listAddress ||
      ((anchorAddress - listAddress) & 3u) != 0u) {
    return {PresentationSurfaceStatus::IncoherentOrderingTableAnchor};
  }
  const uint32_t bucket = (anchorAddress - listAddress) / 4u;
  if (bucket >= kOrderingTableEntries)
    return {PresentationSurfaceStatus::IncoherentOrderingTableAnchor};

  double multiplier =
      commonScale == PresentationProjectionScale::QuarterScalePresentation
          ? 4.0
          : 1.0;
  if (commonNormalizationShift >= 0)
    multiplier = std::ldexp(multiplier, commonNormalizationShift);
  if (commonScale == PresentationProjectionScale::FirstEndpointLine)
    multiplier = 0.5; // 32 presentation units / 64 SZ units per OT cell

  double scaledSum = 0.0;
  uint8_t uniqueVertexCount = 0;
  for (std::size_t index = 0; index < uniqueW.size(); ++index) {
    if (!hasUniqueW[index])
      continue;
    scaledSum += static_cast<double>(uniqueW[index]) * multiplier;
    ++uniqueVertexCount;
  }
  if (uniqueVertexCount < 3u)
    return {PresentationSurfaceStatus::IneligiblePacket};

  if (commonScale == PresentationProjectionScale::CameraBackgroundPoint) {
    return {PresentationSurfaceStatus::Apply, 0.0f,
            std::bit_cast<float>(CAMERA_BACKGROUND_DEPTH_BITS),
            static_cast<uint16_t>(bucket), uniqueVertexCount};
  }

  // Bucket zero is the far end of Crash's forward-linked 2048-entry OT;
  // bucket 2047 is nearest. Thirty-two keeps the native world renderer's
  // camera-unit quotient, while the 8-unit sprite/object families are lifted
  // by the 4x multiplier above. Anchoring the packet mean preserves explicit
  // game z-biases without fitting an observed visibility result.
  const double targetMean =
      kPresentationNearGuard +
      static_cast<double>(kOrderingTableEntries - bucket) *
          kWorldUnitsPerBucket;
  // 0x80039F04 reads SZ1 and >>6 before constructing the line's OT address;
  // its second endpoint does not determine the anchor. A lowered ribbon has
  // two copies of each endpoint, but must preserve that construction rule.
  const double anchorW =
      commonScale == PresentationProjectionScale::FirstEndpointLine
          ? static_cast<double>(uniqueW[0]) * multiplier
          : scaledSum / uniqueVertexCount;
  const double bias = targetMean - anchorW;
  if (!std::isfinite(multiplier) || !std::isfinite(bias))
    return {PresentationSurfaceStatus::ProjectiveWOutOfDomain};
  for (std::size_t index = 0; index < uniqueW.size(); ++index) {
    if (!hasUniqueW[index])
      continue;
    const double corrected =
        static_cast<double>(uniqueW[index]) * multiplier + bias;
    if (!gpu::replay::vulkan_abi::isProjectiveWInDepthDomain(corrected))
      return {PresentationSurfaceStatus::ProjectiveWOutOfDomain};
  }

  return {PresentationSurfaceStatus::Apply, static_cast<float>(multiplier),
          static_cast<float>(bias), static_cast<uint16_t>(bucket),
          uniqueVertexCount};
}

struct CoherentSceneryProjection {
  uint16_t entry = 0;
  uint32_t captureId = 0;
};

static std::optional<CoherentSceneryProjection> coherentSceneryProjection(
    const gpu::replay::ReplayVisibilityAuditOwner &owner) noexcept {
  std::optional<uint16_t> commonEntry;
  std::optional<uint32_t> commonCapture;
  for (const gpu::replay::ReplayVertexProvenance &source :
       owner.provenance.vertices) {
    const std::optional<uint16_t> entry =
        sceneryEntryFromProjectionTag(source.gameProfileTag);
    const std::optional<uint32_t> capture =
        sceneryCaptureFromProjectionTag(source.gameProfileTag);
    const uint32_t callsitePc =
        projectionCallsiteGtePc(source.gameProfileCallsite);
    if (!entry.has_value() || !capture.has_value() ||
        !isResidentSceneryRendererProjection(callsitePc)) {
      return std::nullopt;
    }
    if ((commonEntry.has_value() && *commonEntry != *entry) ||
        (commonCapture.has_value() && *commonCapture != *capture)) {
      return std::nullopt;
    }
    commonEntry = entry;
    commonCapture = capture;
  }
  if (!commonEntry.has_value() || !commonCapture.has_value())
    return std::nullopt;
  return CoherentSceneryProjection{*commonEntry, *commonCapture};
}

SceneryOcclusionClass classifySceneryOcclusion(
    const gpu::replay::ReplayVisibilityAuditOwner &owner) noexcept {
  if (g_sceneryOcclusionMode == SceneryOcclusionMode::Off &&
      g_residentPresentationMode != ResidentPresentationMode::Debug)
    return SceneryOcclusionClass::None;
  const auto projection = coherentSceneryProjection(owner);
  if (!projection.has_value())
    return SceneryOcclusionClass::None;
  if (owner.opaque &&
      (g_residentPresentationMode == ResidentPresentationMode::Debug ||
       g_sceneryOcclusionMode == SceneryOcclusionMode::StrictScenery ||
       certifiedWarpEdgeRecoveredEntry(projection->entry))) {
    return SceneryOcclusionClass::Recovered;
  }
  return SceneryOcclusionClass::Cohort;
}

std::optional<uint16_t> sceneryProjectionEntry(
    const gpu::replay::ReplayVisibilityAuditOwner &owner) noexcept {
  const auto projection = coherentSceneryProjection(owner);
  return projection.has_value() ? std::optional<uint16_t>{projection->entry}
                                : std::nullopt;
}

std::optional<uint32_t> sceneryProjectionCapture(
    const gpu::replay::ReplayVisibilityAuditOwner &owner) noexcept {
  const auto projection = coherentSceneryProjection(owner);
  return projection.has_value() ? std::optional<uint32_t>{projection->captureId}
                                : std::nullopt;
}

PresentationDepthDecision planPresentationDepth(
    const gpu::replay::ReplayVisibilityAuditOwner &owner,
    std::span<const gpu::replay::vulkan_abi::Vertex> vertices) noexcept {
  using gpu::replay::ReplayVertexDmaTraversalExact;
  using gpu::replay::ReplayVertexGp0WordSourceExact;

  if (!presentationDepthCorrections())
    return {PresentationDepthStatus::Disabled, 1.0f};
  if (owner.route != gpu::replay::PacketRoute::Projected3D ||
      !owner.fullyPrecise || !owner.authenticDepth ||
      !owner.provenance.validTopology) {
    return {PresentationDepthStatus::IneligibleOwner, 1.0f};
  }
  if (vertices.size() != 3u)
    return {PresentationDepthStatus::InvalidVertexSpan, 1.0f};

  const auto normalizedObjectDepth =
      [&](PresentationDepthStatus success) -> PresentationDepthDecision {
    int32_t commonShift = -1;
    uint32_t commonObject = 0;
    for (const gpu::replay::ReplayVertexProvenance &source :
         owner.provenance.vertices) {
      const int32_t shift =
          projectionCallsiteScaleShift(source.gameProfileCallsite);
      const uint32_t object =
          projectionCallsiteObjectSource(source.gameProfileCallsite);
      if (shift < 0 || object == 0 ||
          (commonShift >= 0 && shift != commonShift) ||
          (commonObject != 0 && object != commonObject)) {
        return {PresentationDepthStatus::UnmappedOwner, 1.0f};
      }
      commonShift = shift;
      commonObject = object;
    }

    const float multiplier = std::ldexp(1.0f, commonShift);
    for (const gpu::replay::vulkan_abi::Vertex &vertex : vertices) {
      const double corrected = static_cast<double>(vertex.projectiveW) *
                               static_cast<double>(multiplier);
      if (!gpu::replay::vulkan_abi::isProjectiveWInDepthDomain(corrected)) {
        return {PresentationDepthStatus::ProjectiveWOutOfDomain, 1.0f};
      }
    }
    return {success, multiplier};
  };

  // Resident scenery is projected by a separate shared renderer family. It
  // has no dynamic-object node or func_80040E38 scale token: its exact 4x
  // quotient is established by the code-derived GTE-site family and the OT
  // bucket relation. Treating it as the last dynamic object would reintroduce
  // the stale-context contamination this identity split is designed to stop.
  if (coherentSceneryProjection(owner).has_value()) {
    constexpr float kResidentSceneryMultiplier = 4.0f;
    for (const gpu::replay::vulkan_abi::Vertex &vertex : vertices) {
      const double corrected =
          static_cast<double>(vertex.projectiveW) * kResidentSceneryMultiplier;
      if (!gpu::replay::vulkan_abi::isProjectiveWInDepthDomain(corrected)) {
        return {PresentationDepthStatus::ProjectiveWOutOfDomain, 1.0f};
      }
    }
    return {PresentationDepthStatus::ApplySurfaceOnly,
            kResidentSceneryMultiplier};
  }

  // Retain the already cross-validated intro/title table as the stronger
  // color-occlusion correction. It deliberately remains exact and narrow.
  if (owner.opaque && owner.provenance.coherentDmaTraversal() &&
      owner.triangleIndex == 0u &&
      owner.provenance.vertexIndices == std::array<uint8_t, 3>{0u, 1u, 2u}) {
    PresentationDepthEntry lookup{};
    bool lookupComplete = true;
    for (std::size_t index = 0; index < lookup.tags.size(); ++index) {
      const gpu::replay::ReplayVertexProvenance &source =
          owner.provenance.vertices[index];
      constexpr uint8_t required =
          ReplayVertexGp0WordSourceExact | ReplayVertexDmaTraversalExact;
      if (!source.has(required) || source.gameProfileTag == 0 ||
          source.gp0WordSource == 0 || source.dmaNodeSource == 0) {
        lookupComplete = false;
        break;
      }
      lookup.tags[index] = source.gameProfileTag;
      const uint32_t wordAddress =
          static_cast<uint32_t>((source.gp0WordSource - 1u) & 0x1fffffu);
      const uint32_t nodeAddress =
          static_cast<uint32_t>((source.dmaNodeSource - 1u) & 0x1fffffu);
      lookup.gp0NodeOffsets[index] = (wordAddress - nodeAddress) & 0x1fffffu;
    }

    if (lookupComplete) {
      const auto found =
          std::lower_bound(kPresentationDepthEntries.begin(),
                           kPresentationDepthEntries.end(), lookup.tags,
                           [](const PresentationDepthEntry &entry,
                              const std::array<uint64_t, 3> &tags) {
                             return entry.tags < tags;
                           });
      if (found != kPresentationDepthEntries.end() &&
          found->tags == lookup.tags &&
          found->gp0NodeOffsets == lookup.gp0NodeOffsets) {
        for (const gpu::replay::vulkan_abi::Vertex &vertex : vertices) {
          const double corrected = static_cast<double>(vertex.projectiveW) *
                                   static_cast<double>(found->multiplier);
          if (!gpu::replay::vulkan_abi::isProjectiveWInDepthDomain(corrected)) {
            return {PresentationDepthStatus::ProjectiveWOutOfDomain, 1.0f};
          }
        }
        return {PresentationDepthStatus::Apply, found->multiplier};
      }
    }
  }

  // func_80040E38 right-shifts both model coordinates and translation by this
  // exponent. Reversing the power-of-two normalization is exact for the
  // presented geometric surface, including translucent packets. It does not
  // alone prove cross-object z ordering, so color and D32 retain OT behavior.
  return normalizedObjectDepth(PresentationDepthStatus::ApplySurfaceOnly);
}

} // namespace ps1::games::crash3
