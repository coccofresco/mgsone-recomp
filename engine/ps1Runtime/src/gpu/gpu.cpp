#include "runtime/gpu/gpu.h"
#include "runtime/games/crash3_profile.h"
#include "runtime/games/crash3_screen_composition.h"
#include "runtime/games/mgs_mesh_replay.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/games/mgs_screen_effects.h"
#include "runtime/gpu/display_geometry.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace ps1::gpu {

namespace {

class ScopedGpuMetricTimer {
public:
  explicit ScopedGpuMetricTimer(std::string_view metric) noexcept
      : metric_(metric), enabled_(metrics::enabled()) {
    if (enabled_)
      start_ = Clock::now();
  }

  ~ScopedGpuMetricTimer() {
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

uint64_t checkedReplayDepthEpochIncrement(uint64_t epoch) {
  if (epoch == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("CrashReplay depth epoch overflowed");
  return epoch + 1u;
}

bool traceFrameHashesEnabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("PS1_TRACE_FRAME_HASHES");
    return value != nullptr && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}

enum class ModernCoverageDivergence : uint8_t {
  NearPlaneDiscard = 1,
  PreciseDegenerate = 2,
  OutsideDrawArea = 3,
};

int64_t traceQ16(double value) {
  if (std::isnan(value))
    return std::numeric_limits<int64_t>::min();
  const long double scaled = static_cast<long double>(value) * 65536.0L;
  if (scaled >= static_cast<long double>(std::numeric_limits<int64_t>::max()))
    return std::numeric_limits<int64_t>::max();
  if (scaled <=
      static_cast<long double>(std::numeric_limits<int64_t>::min() + 1))
    return std::numeric_limits<int64_t>::min() + 1;
  return static_cast<int64_t>(std::llround(scaled));
}

struct DoublePair {
  double first = 0.0;
  double second = 0.0;
};

struct VramTransferExtent {
  uint32_t width = 0;
  uint32_t height = 0;
};

// GP0(80h-DFh) exposes only ten width bits and nine height bits. In this
// command family an all-zero field denotes the complete corresponding VRAM
// dimension; bits above those fields are ignored by the hardware.
constexpr VramTransferExtent decodeVramTransferExtent(uint32_t size) {
  const uint32_t encodedWidth = size & 0x3FFu;
  const uint32_t encodedHeight = (size >> 16) & 0x1FFu;
  return {encodedWidth == 0 ? GPU::VRAM_WIDTH : encodedWidth,
          encodedHeight == 0 ? GPU::VRAM_HEIGHT : encodedHeight};
}

inline DoublePair divideDoublePair(double first, double second,
                                   double denominator) {
#if defined(__SSE2__)
  // Perspective U and V share one denominator. Two-lane IEEE division keeps
  // the result of each lane identical to scalar double division while using
  // one packed divide on x86-64. Other hosts retain the exact scalar oracle.
  const __m128d numerator = _mm_set_pd(second, first);
  const __m128d divisor = _mm_set1_pd(denominator);
  const __m128d quotient = _mm_div_pd(numerator, divisor);
  alignas(16) double lanes[2];
  _mm_store_pd(lanes, quotient);
  return DoublePair{lanes[0], lanes[1]};
#else
  return DoublePair{first / denominator, second / denominator};
#endif
}

} // namespace

ModernUVDomain prepareModernUVDomain(const ModernUVInput *inputs,
                                     std::size_t inputCount,
                                     uint32_t resolutionScale, bool quad) {
  ModernUVDomain domain;
  if (inputs == nullptr || inputCount < 3 || inputCount > 4)
    return domain;

  // Match SwanStation's BatchVertex calculation exactly: final PGXP float XY,
  // the first triangle's plane derivatives, and float sign/zero tests after
  // division by its area. Packed integer XY is observably different for a
  // fractional vertex near an axis-aligned sprite.
  if (quad && inputCount == 4 && resolutionScale > 1) {
    const float abx = inputs[1].x - inputs[0].x;
    const float aby = inputs[1].y - inputs[0].y;
    const float bcx = inputs[2].x - inputs[1].x;
    const float bcy = inputs[2].y - inputs[1].y;
    const float cax = inputs[0].x - inputs[2].x;
    const float cay = inputs[0].y - inputs[2].y;
    const float dudx = -aby * static_cast<float>(inputs[2].u) -
                       bcy * static_cast<float>(inputs[0].u) -
                       cay * static_cast<float>(inputs[1].u);
    const float dvdx = -aby * static_cast<float>(inputs[2].v) -
                       bcy * static_cast<float>(inputs[0].v) -
                       cay * static_cast<float>(inputs[1].v);
    const float dudy = abx * static_cast<float>(inputs[2].u) +
                       bcx * static_cast<float>(inputs[0].u) +
                       cax * static_cast<float>(inputs[1].u);
    const float dvdy = abx * static_cast<float>(inputs[2].v) +
                       bcx * static_cast<float>(inputs[0].v) +
                       cax * static_cast<float>(inputs[1].v);
    const float area = bcx * cay - bcy * cax;
    const int32_t textureArea =
        (static_cast<int32_t>(inputs[1].u) - inputs[0].u) *
            (static_cast<int32_t>(inputs[2].v) - inputs[0].v) -
        (static_cast<int32_t>(inputs[2].u) - inputs[0].u) *
            (static_cast<int32_t>(inputs[1].v) - inputs[0].v);
    // The donor deliberately tests only the first triangle's W. The fourth
    // vertex does not participate in this sprite heuristic.
    const bool is3D = inputs[0].w != inputs[1].w || inputs[0].w != inputs[2].w;

    if (area != 0.0f && textureArea != 0 && !is3D) {
      const float reciprocalArea = 1.0f / area;
      const float dudxArea = dudx * reciprocalArea;
      const float dudyArea = dudy * reciprocalArea;
      const float dvdxArea = dvdx * reciprocalArea;
      const float dvdyArea = dvdy * reciprocalArea;
      domain.flippedU = ((dudxArea < 0.0f) && dudyArea == 0.0f) ||
                        ((dudyArea < 0.0f) && dudxArea == 0.0f);
      domain.flippedV = ((dvdxArea < 0.0f) && dvdyArea == 0.0f) ||
                        ((dvdyArea < 0.0f) && dvdxArea == 0.0f);
      domain.offsetU = domain.flippedU ? 1u : 0u;
      domain.offsetV = domain.flippedV ? 1u : 0u;
    }
  }

  uint32_t minU = static_cast<uint32_t>(inputs[0].u) + domain.offsetU;
  uint32_t maxU = minU;
  uint32_t minV = static_cast<uint32_t>(inputs[0].v) + domain.offsetV;
  uint32_t maxV = minV;
  for (std::size_t i = 1; i < inputCount; ++i) {
    const uint32_t u = static_cast<uint32_t>(inputs[i].u) + domain.offsetU;
    const uint32_t v = static_cast<uint32_t>(inputs[i].v) + domain.offsetV;
    minU = std::min(minU, u);
    maxU = std::max(maxU, u);
    minV = std::min(minV, v);
    maxV = std::max(maxV, v);
  }

  // Modern pixel centers approach the upper endpoint without reproducing the
  // PS1's top-left interpolant. Both donors exclude that endpoint whenever
  // the corresponding texture axis is non-constant.
  if (minU != maxU)
    --maxU;
  if (minV != maxV)
    --maxV;
  domain.minU = static_cast<double>(minU);
  domain.maxU = static_cast<double>(maxU);
  domain.minV = static_cast<double>(minV);
  domain.maxV = static_cast<double>(maxV);
  domain.enabled = true;
  return domain;
}

GPU::GPU() {
  vram_.resize(VRAM_WIDTH * VRAM_HEIGHT);
  displayVram_.resize(VRAM_WIDTH * VRAM_HEIGHT);
  constexpr std::size_t kOrdinaryGp0PacketWords = 16u;
  commandQueue_.reserve(kOrdinaryGp0PacketWords);
  commandSourceQueue_.reserve(kOrdinaryGp0PacketWords);
  commandPrecisionQueue_.reserve(kOrdinaryGp0PacketWords);
  commandDmaProvenanceQueue_.reserve(kOrdinaryGp0PacketWords);
  reset();
}

GPU::~GPU() {}

void GPU::setEnhancedRaster(bool enabled) {
  std::lock_guard<std::recursive_mutex> timelineLock(gpuTimelineMutex_);
  ++mutationSerial_;
  enhancedRasterEnabled_ = enabled;
  clearDepthBuffer();
  depthEpochHasMean_ = false;
  depthEpochDrawAreaDirty_ = false;
  depthEpochWrittenBoundsValid_ = false;
  ++depthEpochId_;
}

void GPU::setProjectiveNearClip(bool enabled) {
  std::lock_guard<std::recursive_mutex> timelineLock(gpuTimelineMutex_);
  ++mutationSerial_;
  projectiveNearClipEnabled_ = enabled;
  clearDepthBuffer();
  depthEpochHasMean_ = false;
  depthEpochDrawAreaDirty_ = false;
  ++depthEpochId_;
}

void GPU::setTrueDepth(bool enabled) {
  setTrueDepthMode(enabled ? TrueDepthMode::Strict : TrueDepthMode::Off);
}

void GPU::setTrueDepthMode(TrueDepthMode mode) {
  std::lock_guard<std::recursive_mutex> timelineLock(gpuTimelineMutex_);
  ++mutationSerial_;
  trueDepthMode_ = mode;
  clearDepthBuffer();
  depthEpochHasMean_ = false;
  depthEpochDrawAreaDirty_ = false;
  ++depthEpochId_;
}

void GPU::configureEnhancedPresentation(uint32_t scale, bool trueColor,
                                        bool useProgrammedDisplayRange) {
  std::lock_guard<std::recursive_mutex> timelineLock(gpuTimelineMutex_);
  ++mutationSerial_;
  trueColorEnabled_ = trueColor;
  useProgrammedDisplayRange_ = useProgrammedDisplayRange;
  if (scale <= 1 && !trueColor) {
    enhancedSurface_.configure(0, 0, 1);
  } else {
    enhancedSurface_.configure(VRAM_WIDTH, VRAM_HEIGHT, scale);
    enhancedSurface_.mirrorNativeBuffer(
        reinterpret_cast<const uint16_t *>(vram_.data()), vram_.size());
  }
  depthEpochHasMean_ = false;
  depthEpochDrawAreaDirty_ = false;
  depthEpochWrittenBoundsValid_ = false;
  ++depthEpochId_;
  std::lock_guard<std::mutex> displayLock(displayMutex_);
  displayRgba8_.clear();
  displayRgba8Width_ = 0;
  displayRgba8Height_ = 0;
}

void GPU::configureReplay(ReplayConfig config) {
  std::lock_guard<std::recursive_mutex> timelineLock(gpuTimelineMutex_);
  config.internalScale = std::max(config.internalScale, 1u);
  replayRecorder_.configure(config);
  activeReplayPacketSequence_ = replay::UNASSIGNED_PACKET_SEQUENCE;
  activeReplayPacketUnsupported_ = false;
  replayPaintCaptureActive_ = false;
  replayPaintCapture_.clear();
  replayDepthDrawAreaDirty_ = false;
  replayDepthHasMean_ = false;
  replayDepthLastMean_ = 0.0;
  replayDepthEpoch_ = 1;
  if (replayRecorder_.enabled())
    replayRecorder_.recordSurfaceSeed(replayNativeVram(), replayDepthEpoch_);
}

replay::CrashReplayFramePtr GPU::tryConsumeReplay() {
  return replayRecorder_.tryConsume();
}

void GPU::clearDepthBuffer() {
  if (enhancedSurface_.enabled())
    enhancedSurface_.clearDepth();
  depthEpochWrittenBoundsValid_ = false;
}

void GPU::reset() { resetState(true); }

void GPU::resetState(bool clearVideoMemory) {
  ++mutationSerial_;
  // A GP1 reset can interrupt the data phase of GP0(A0). Preserve every
  // source pixel already accepted by native VRAM before resetting either
  // command-parser state or the surface baseline.
  if (replayRecorder_.enabled() && replayRecorder_.cpuUploadActive())
    replayRecorder_.abortCpuUpload();
  // 0x14802000: post-reset hardware default, bit 23 = display disabled.
  // The real BIOS sends GP1(0x03,0) to enable display after booting.
  // main_host.cpp does this explicitly after initialization.
  gpuStat_ = 0x14802000;
  gpuRead_ = 0;
  expectedCommandWords_ = 0;
  isCommandExecuting_ = false;
  polylineCommand_ = false;
  replayPaintCaptureActive_ = false;
  replayPaintCapture_.clear();
  commandQueue_.clear();
  commandSourceQueue_.clear();
  commandPrecisionQueue_.clear();
  commandDmaProvenanceQueue_.clear();
  primitivePathHistory_.clear();
  framePreciseTriangles_ = 0;
  frameCompatibilityTriangles_ = 0;
  frameScreen2DTriangles_ = 0;
  framePartialPrecisionPackets_ = 0;
  frameBoundsRejects_ = 0;
  framePathToggles_ = 0;
  frameModernCoveredSamples_ = 0;
  frameModernInvalidDepthSamples_ = 0;
  frameModernMaskRejectedSamples_ = 0;
  frameModernDepthRejectedSamples_ = 0;
  frameModernTransparentSamples_ = 0;
  frameModernAcceptedSamples_ = 0;
  frameModernEmptyTriangles_ = 0;
  frameModernEmptyDepthOnly_ = 0;
  frameModernEmptyTransparentOnly_ = 0;
  frameModernEmptyMaskOnly_ = 0;
  frameModernEmptyInvalidDepthOnly_ = 0;
  frameModernEmptyMixed_ = 0;
  frameModernNativeBoundsSkips_ = 0;
  frameModernProjectiveInvalid_ = 0;
  frameModernContextMismatches_ = 0;
  frameModernNearClipDiscarded_ = 0;
  frameModernNearClipDiscardedNativeCoverage_ = 0;
  frameModernNearClipDiscardedNativeEmpty_ = 0;
  frameModernNearClipPolygons_ = 0;
  frameModernNearClipOutputTriangles_ = 0;
  frameModernNearSaturatedTriangles_ = 0;
  frameModernNearSaturatedVertices_ = 0;
  frameModernPreciseDegenerate_ = 0;
  frameModernPreciseDegenerateNativeCoverage_ = 0;
  frameModernPreciseDegenerateNativeEmpty_ = 0;
  frameModernOutsideDrawArea_ = 0;
  frameModernOutsideDrawAreaNativeCoverage_ = 0;
  frameModernOutsideDrawAreaNativeEmpty_ = 0;
  frameModernDepthEligiblePackets_ = 0;
  frameModernDepthInvalidPackets_ = 0;
  frameModernDepthBlendVisibilityBoundaryPackets_ = 0;
  frameModernDepthVisibilityBoundarySamples_ = 0;
  frameModernDepthEpochDrawAreaClears_ = 0;
  frameModernDepthEpochZJumpClears_ = 0;
  frameModernUVLimitedPrimitives_ = 0;
  frameModernUVFlippedUPrimitives_ = 0;
  frameModernUVFlippedVPrimitives_ = 0;
  frameModernUVClampedSamples_ = 0;
  frameModernUVSampleCacheHits_ = 0;
  frameModernUVSampleCacheMisses_ = 0;

  depthEpochDrawAreaDirty_ = false;
  depthEpochHasMean_ = false;
  depthEpochLastMean_ = 0.0;
  depthEpochWrittenBoundsValid_ = false;
  ++depthEpochId_;
  replayDepthDrawAreaDirty_ = false;
  replayDepthHasMean_ = false;
  replayDepthLastMean_ = 0.0;
  replayDepthEpoch_ = checkedReplayDepthEpochIncrement(replayDepthEpoch_);

  drawOffsetX_ = 0;
  drawOffsetY_ = 0;
  drawAreaX1_ = 0;
  drawAreaY1_ = 0;
  drawAreaX2_ = 1023;
  drawAreaY2_ = 511;
  ditherEnable_ = false;
  blendMode_ = 0;
  setMaskWhileDrawing_ = false;
  checkMaskBeforeDraw_ = false;
  currentTexpage_ = 0;
  textureRectFlipX_ = false;
  textureRectFlipY_ = false;
  allowTextureDisable_ = false;
  textureDisable_ = false;
  texWindowMaskX_ = 0;
  texWindowMaskY_ = 0;
  texWindowOffsetX_ = 0;
  texWindowOffsetY_ = 0;
  displayModeSet_ = false;
  displayAreaSet_ = false;
  horizontalDisplayRangeSet_ = false;
  verticalDisplayRangeSet_ = false;

  displayVRAMXStart_ = 0;
  displayVRAMYStart_ = 0;
  displayX1_ = 0;
  displayX2_ = 256;
  displayY1_ = 0;
  displayY2_ = 240;
  displaySnapshot_ = DisplayState{displayVRAMXStart_,
                                  displayVRAMYStart_,
                                  displayX1_,
                                  displayX2_,
                                  displayY1_,
                                  displayY2_,
                                  gpuStat_,
                                  displayModeSet_,
                                  displayAreaSet_,
                                  horizontalDisplayRangeSet_,
                                  verticalDisplayRangeSet_,
                                  ps1::metrics::traceFrame()};

  vramTransfer_.transferWordsRemaining = 0;
  vramTransfer_.transferPixelsRemaining = 0;
  vramTransfer_.isWritingToVRAM = false;
  vramTransfer_.isReadingFromVRAM = false;
  vramTransfer_.isCopyingVRAM = false;

  if (clearVideoMemory) {
    std::fill(vram_.begin(), vram_.end(), Color16{0});
    std::fill(displayVram_.begin(), displayVram_.end(), Color16{0});
    displayRgba8_.clear();
    displayRgba8Width_ = 0;
    displayRgba8Height_ = 0;
    if (enhancedSurface_.enabled())
      enhancedSurface_.clear();
  }
  clearDepthBuffer();
  if (replayRecorder_.enabled()) {
    if (clearVideoMemory) {
      replayRecorder_.recordSurfaceSeed(replayNativeVram(), replayDepthEpoch_);
    } else {
      replay::Reset replayReset;
      replayReset.nextDepthEpoch = replayDepthEpoch_;
      replayRecorder_.recordReset(replayReset);
    }
  }
}

void GPU::loadVram(const uint8_t *data) {
  std::lock_guard<std::recursive_mutex> timelineLock(gpuTimelineMutex_);
  ++mutationSerial_;
  // Finish the observable prefix of an interrupted A0 before the save-state
  // establishes a new immutable baseline. The not-yet-received suffix never
  // touched native VRAM and therefore must not appear in the replay stream.
  if (replayRecorder_.enabled() && replayRecorder_.cpuUploadActive())
    replayRecorder_.abortCpuUpload();
  // Reinterpret the raw bytes as 16-bit pixels (little-endian matches PS1)
  std::memcpy(vram_.data(), data, VRAM_WIDTH * VRAM_HEIGHT * sizeof(Color16));
  if (enhancedSurface_.enabled())
    enhancedSurface_.mirrorNativeBuffer(
        reinterpret_cast<const uint16_t *>(vram_.data()), vram_.size());

  // A save-state replaces the current GPU timeline.  Command words, their
  // PGXP provenance and any half-finished image transfer are transient and
  // are deliberately not serialized; carrying them across the load could
  // combine vertices/depth from two different timelines.
  expectedCommandWords_ = 0;
  isCommandExecuting_ = false;
  polylineCommand_ = false;
  replayPaintCaptureActive_ = false;
  replayPaintCapture_.clear();
  commandQueue_.clear();
  commandSourceQueue_.clear();
  commandPrecisionQueue_.clear();
  commandDmaProvenanceQueue_.clear();
  vramTransfer_ = {};
  clearDepthBuffer();
  depthEpochDrawAreaDirty_ = false;
  depthEpochHasMean_ = false;
  depthEpochWrittenBoundsValid_ = false;
  ++depthEpochId_;
  replayDepthDrawAreaDirty_ = false;
  replayDepthHasMean_ = false;
  replayDepthLastMean_ = 0.0;
  replayDepthEpoch_ = checkedReplayDepthEpochIncrement(replayDepthEpoch_);
  if (replayRecorder_.enabled())
    replayRecorder_.recordSurfaceSeed(replayNativeVram(), replayDepthEpoch_);
}

GPU::GuestState GPU::guestState() const {
  std::lock_guard<std::recursive_mutex> timelineLock(gpuTimelineMutex_);
  GuestState state;
  state.gpuStat = gpuStat_;
  state.gpuRead = gpuRead_;
  state.drawOffsetX = drawOffsetX_;
  state.drawOffsetY = drawOffsetY_;
  state.drawAreaX1 = drawAreaX1_;
  state.drawAreaY1 = drawAreaY1_;
  state.drawAreaX2 = drawAreaX2_;
  state.drawAreaY2 = drawAreaY2_;
  state.ditherEnable = ditherEnable_;
  state.blendMode = blendMode_;
  state.setMaskWhileDrawing = setMaskWhileDrawing_;
  state.checkMaskBeforeDraw = checkMaskBeforeDraw_;
  state.currentTexpage = currentTexpage_;
  state.textureRectFlipX = textureRectFlipX_;
  state.textureRectFlipY = textureRectFlipY_;
  state.allowTextureDisable = allowTextureDisable_;
  state.textureDisable = textureDisable_;
  state.displayModeSet = displayModeSet_;
  state.displayAreaSet = displayAreaSet_;
  state.horizontalDisplayRangeSet = horizontalDisplayRangeSet_;
  state.verticalDisplayRangeSet = verticalDisplayRangeSet_;
  state.displayVRAMXStart = displayVRAMXStart_;
  state.displayVRAMYStart = displayVRAMYStart_;
  state.displayX1 = displayX1_;
  state.displayX2 = displayX2_;
  state.displayY1 = displayY1_;
  state.displayY2 = displayY2_;
  state.texWindowMaskX = texWindowMaskX_;
  state.texWindowMaskY = texWindowMaskY_;
  state.texWindowOffsetX = texWindowOffsetX_;
  state.texWindowOffsetY = texWindowOffsetY_;
  return state;
}

void GPU::loadGuestState(const GuestState &state) {
  std::lock_guard<std::recursive_mutex> timelineLock(gpuTimelineMutex_);
  ++mutationSerial_;
  gpuStat_ = state.gpuStat;
  gpuRead_ = state.gpuRead;
  drawOffsetX_ = state.drawOffsetX;
  drawOffsetY_ = state.drawOffsetY;
  drawAreaX1_ = state.drawAreaX1;
  drawAreaY1_ = state.drawAreaY1;
  drawAreaX2_ = state.drawAreaX2;
  drawAreaY2_ = state.drawAreaY2;
  ditherEnable_ = state.ditherEnable;
  blendMode_ = state.blendMode;
  setMaskWhileDrawing_ = state.setMaskWhileDrawing;
  checkMaskBeforeDraw_ = state.checkMaskBeforeDraw;
  currentTexpage_ = state.currentTexpage;
  textureRectFlipX_ = state.textureRectFlipX;
  textureRectFlipY_ = state.textureRectFlipY;
  allowTextureDisable_ = state.allowTextureDisable;
  textureDisable_ = state.textureDisable;
  displayModeSet_ = state.displayModeSet;
  displayAreaSet_ = state.displayAreaSet;
  horizontalDisplayRangeSet_ = state.horizontalDisplayRangeSet;
  verticalDisplayRangeSet_ = state.verticalDisplayRangeSet;
  displayVRAMXStart_ = state.displayVRAMXStart;
  displayVRAMYStart_ = state.displayVRAMYStart;
  displayX1_ = state.displayX1;
  displayX2_ = state.displayX2;
  displayY1_ = state.displayY1;
  displayY2_ = state.displayY2;
  texWindowMaskX_ = state.texWindowMaskX;
  texWindowMaskY_ = state.texWindowMaskY;
  texWindowOffsetX_ = state.texWindowOffsetX;
  texWindowOffsetY_ = state.texWindowOffsetY;
}

void GPU::snapshotDisplayBuffer(uint64_t latchFrame) {
  const ScopedGpuMetricTimer snapshotTimer("host.gpu.snapshot_display_us");
  std::lock_guard<std::recursive_mutex> timelineLock(gpuTimelineMutex_);
  std::lock_guard<std::mutex> displayLock(displayMutex_);
  // Schema 5 distinguishes OT-authoritative real-W stores from schema 4's
  // cross-packet LEQUAL policy. Analysis scripts must branch on this state
  // instead of treating an absent legacy key as a measured zero.
  constexpr int64_t modernMetricsSchemaVersion = 5;
  ps1::metrics::setState("gpu.modern.metrics.schema_version",
                         modernMetricsSchemaVersion);
  if (latchFrame == UINT64_MAX)
    latchFrame = ps1::metrics::traceFrame();
  const bool reusedSnapshot = lastLatchedMutationSerial_ == mutationSerial_;
  const uint64_t mutationsSinceLastLatch =
      lastLatchedMutationSerial_ == UINT64_MAX
          ? mutationSerial_ + 1u
          : mutationSerial_ - lastLatchedMutationSerial_;
  // Copy both halves of the observable frame under one latch.  The game may
  // issue the next PutDispEnv immediately after VSync wakes it; keeping the
  // live display origin out of renderFrame prevents alternating old/new
  // buffer pairs.
  if (!reusedSnapshot)
    std::copy(vram_.begin(), vram_.end(), displayVram_.begin());
  displaySnapshot_ = DisplayState{displayVRAMXStart_,
                                  displayVRAMYStart_,
                                  displayX1_,
                                  displayX2_,
                                  displayY1_,
                                  displayY2_,
                                  gpuStat_,
                                  displayModeSet_,
                                  displayAreaSet_,
                                  horizontalDisplayRangeSet_,
                                  verticalDisplayRangeSet_,
                                  latchFrame,
                                  lastCompletedSubmissionId_,
                                  mutationSerial_};

  const DisplayAperture aperture = deriveDisplayAperture(
      displaySnapshot_, useProgrammedDisplayRange_
                            ? DisplayAreaPolicy::Programmed
                            : DisplayAreaPolicy::Nominal);
  if (enhancedSurface_.enabled() && !reusedSnapshot) {
    enhancedSurface_.copyNativeRegion(aperture.x, aperture.y, aperture.width,
                                      aperture.height, displayRgba8_);
    displayRgba8Width_ = aperture.width * enhancedSurface_.scale();
    displayRgba8Height_ = aperture.height * enhancedSurface_.scale();
  } else if (!enhancedSurface_.enabled()) {
    displayRgba8_.clear();
    displayRgba8Width_ = 0;
    displayRgba8Height_ = 0;
  }

  // A failed seal must never leave the previous latch's identity paired with
  // newly latched native pixels. This also clears the token while capture is
  // disabled. displayMutex_ protects both sides of the snapshot/token pair.
  displayReplayFrame_.reset();
  if (replayRecorder_.enabled()) {
    replay::FrameLatch replayLatch;
    replayLatch.frameNumber = latchFrame;
    replayLatch.contentRevision = mutationSerial_;
    replayLatch.completedSubmissionId = lastCompletedSubmissionId_;
    replayLatch.display.aperture = {static_cast<int32_t>(aperture.x),
                                    static_cast<int32_t>(aperture.y),
                                    aperture.width, aperture.height};
    replayLatch.display.enabled = aperture.enabled;
    replayLatch.display.pal = (gpuStat_ & (1u << 20)) != 0;
    replayLatch.display.interlaced = (gpuStat_ & (1u << 22)) != 0;
    replayLatch.display.oddField = (gpuStat_ & (1u << 31)) != 0;
    displayReplayFrame_ = replayRecorder_.sealAndPublish(replayLatch);
  }

  int displayWidth = 320;
  int displayHeight = 240;
  if (displayModeSet_) {
    displayHeight = (gpuStat_ & (1u << 19)) ? 480 : 240;
    const int hres = (gpuStat_ >> 17) & 3;
    displayWidth = (gpuStat_ & (1u << 16)) ? 368
                                           : (hres == 0   ? 256
                                              : hres == 1 ? 320
                                              : hres == 2 ? 512
                                                          : 640);
  }
  const bool overlap =
      static_cast<int32_t>(displayVRAMXStart_) <= drawAreaX2_ &&
      static_cast<int32_t>(displayVRAMXStart_) + displayWidth - 1 >=
          drawAreaX1_ &&
      static_cast<int32_t>(displayVRAMYStart_) <= drawAreaY2_ &&
      static_cast<int32_t>(displayVRAMYStart_) + displayHeight - 1 >=
          drawAreaY1_;
  const bool hashValid =
      ps1::metrics::traceEnabled() && traceFrameHashesEnabled();
  if (hashValid && !reusedSnapshot) {
    constexpr uint64_t fnvOffset = 1469598103934665603ull;
    constexpr uint64_t fnvPrime = 1099511628211ull;
    latchedNativeHash_ = fnvOffset;
    for (uint32_t y = 0; y < aperture.height; ++y) {
      const std::size_t row =
          static_cast<std::size_t>(aperture.y + y) * VRAM_WIDTH + aperture.x;
      for (uint32_t x = 0; x < aperture.width; ++x) {
        const uint16_t raw = displayVram_[row + x].raw;
        latchedNativeHash_ =
            (latchedNativeHash_ ^ static_cast<uint8_t>(raw)) * fnvPrime;
        latchedNativeHash_ =
            (latchedNativeHash_ ^ static_cast<uint8_t>(raw >> 8)) * fnvPrime;
      }
    }
    latchedModernHash_ = fnvOffset;
    latchedModernNonblack_ = 0;
    for (const Color32 color : displayRgba8_) {
      latchedModernHash_ = (latchedModernHash_ ^ color.r) * fnvPrime;
      latchedModernHash_ = (latchedModernHash_ ^ color.g) * fnvPrime;
      latchedModernHash_ = (latchedModernHash_ ^ color.b) * fnvPrime;
      latchedModernHash_ = (latchedModernHash_ ^ color.a) * fnvPrime;
      if (color.r != 0 || color.g != 0 || color.b != 0)
        ++latchedModernNonblack_;
    }
  }
  ps1::metrics::trace(
      "gpu.vblank_latch",
      {{"modern_metrics_schema", modernMetricsSchemaVersion},
       {"display_x", displayVRAMXStart_},
       {"display_y", displayVRAMYStart_},
       {"display_w", displayWidth},
       {"display_h", displayHeight},
       {"aperture_w", aperture.width},
       {"aperture_h", aperture.height},
       {"draw_x0", drawAreaX1_},
       {"draw_y0", drawAreaY1_},
       {"draw_x1", drawAreaX2_},
       {"draw_y1", drawAreaY2_},
       {"draw_offset_x", drawOffsetX_},
       {"draw_offset_y", drawOffsetY_},
       {"display_draw_overlap", overlap ? 1 : 0},
       {"latch_frame", static_cast<int64_t>(latchFrame)},
       {"completed_submission_id",
        static_cast<int64_t>(lastCompletedSubmissionId_)},
       {"submissions_since_last_latch",
        static_cast<int64_t>(lastCompletedSubmissionId_ -
                             lastLatchedSubmissionId_)},
       {"mutation_serial", static_cast<int64_t>(mutationSerial_)},
       {"mutations_since_last_latch",
        static_cast<int64_t>(mutationsSinceLastLatch)},
       {"snapshot_reused", reusedSnapshot ? 1 : 0},
       {"hash_valid", hashValid ? 1 : 0},
       {"native_hash", static_cast<int64_t>(latchedNativeHash_)},
       {"modern_hash", static_cast<int64_t>(latchedModernHash_)},
       {"modern_nonblack", static_cast<int64_t>(latchedModernNonblack_)},
       {"precise_triangles", static_cast<int64_t>(framePreciseTriangles_)},
       {"compatibility_triangles",
        static_cast<int64_t>(frameCompatibilityTriangles_)},
       {"screen_2d_triangles", static_cast<int64_t>(frameScreen2DTriangles_)},
       {"partial_precision_packets",
        static_cast<int64_t>(framePartialPrecisionPackets_)},
       {"bounds_rejects", static_cast<int64_t>(frameBoundsRejects_)},
       {"path_toggles", static_cast<int64_t>(framePathToggles_)},
       {"modern_covered_samples",
        static_cast<int64_t>(frameModernCoveredSamples_)},
       {"modern_invalid_depth_samples",
        static_cast<int64_t>(frameModernInvalidDepthSamples_)},
       {"modern_mask_rejected_samples",
        static_cast<int64_t>(frameModernMaskRejectedSamples_)},
       {"modern_depth_rejected_samples",
        static_cast<int64_t>(frameModernDepthRejectedSamples_)},
       {"modern_transparent_samples",
        static_cast<int64_t>(frameModernTransparentSamples_)},
       {"modern_accepted_samples",
        static_cast<int64_t>(frameModernAcceptedSamples_)},
       {"modern_empty_triangles",
        static_cast<int64_t>(frameModernEmptyTriangles_)},
       {"modern_empty_depth_only",
        static_cast<int64_t>(frameModernEmptyDepthOnly_)},
       {"modern_empty_transparent_only",
        static_cast<int64_t>(frameModernEmptyTransparentOnly_)},
       {"modern_empty_mask_only",
        static_cast<int64_t>(frameModernEmptyMaskOnly_)},
       {"modern_empty_invalid_depth_only",
        static_cast<int64_t>(frameModernEmptyInvalidDepthOnly_)},
       {"modern_empty_mixed", static_cast<int64_t>(frameModernEmptyMixed_)},
       {"modern_native_bounds_skips",
        static_cast<int64_t>(frameModernNativeBoundsSkips_)},
       {"modern_projective_invalid",
        static_cast<int64_t>(frameModernProjectiveInvalid_)},
       {"modern_context_mismatches",
        static_cast<int64_t>(frameModernContextMismatches_)},
       {"modern_near_clip_discarded",
        static_cast<int64_t>(frameModernNearClipDiscarded_)},
       {"modern_near_clip_discarded_native_coverage",
        static_cast<int64_t>(frameModernNearClipDiscardedNativeCoverage_)},
       {"modern_near_clip_discarded_native_empty",
        static_cast<int64_t>(frameModernNearClipDiscardedNativeEmpty_)},
       {"modern_near_clip_polygons",
        static_cast<int64_t>(frameModernNearClipPolygons_)},
       {"modern_near_clip_output_triangles",
        static_cast<int64_t>(frameModernNearClipOutputTriangles_)},
       {"modern_near_saturated_triangles",
        static_cast<int64_t>(frameModernNearSaturatedTriangles_)},
       {"modern_near_saturated_vertices",
        static_cast<int64_t>(frameModernNearSaturatedVertices_)},
       {"modern_precise_degenerate",
        static_cast<int64_t>(frameModernPreciseDegenerate_)},
       {"modern_precise_degenerate_native_coverage",
        static_cast<int64_t>(frameModernPreciseDegenerateNativeCoverage_)},
       {"modern_precise_degenerate_native_empty",
        static_cast<int64_t>(frameModernPreciseDegenerateNativeEmpty_)},
       {"modern_outside_draw_area",
        static_cast<int64_t>(frameModernOutsideDrawArea_)},
       {"modern_outside_draw_area_native_coverage",
        static_cast<int64_t>(frameModernOutsideDrawAreaNativeCoverage_)},
       {"modern_outside_draw_area_native_empty",
        static_cast<int64_t>(frameModernOutsideDrawAreaNativeEmpty_)},
       {"modern_depth_eligible_packets",
        static_cast<int64_t>(frameModernDepthEligiblePackets_)},
       {"modern_depth_invalid_packets",
        static_cast<int64_t>(frameModernDepthInvalidPackets_)},
       {"modern_depth_blend_visibility_boundary_packets",
        static_cast<int64_t>(frameModernDepthBlendVisibilityBoundaryPackets_)},
       {"modern_depth_visibility_boundary_samples",
        static_cast<int64_t>(frameModernDepthVisibilityBoundarySamples_)},
       {"modern_depth_epoch_draw_area_clears",
        static_cast<int64_t>(frameModernDepthEpochDrawAreaClears_)},
       {"modern_depth_epoch_z_jump_clears",
        static_cast<int64_t>(frameModernDepthEpochZJumpClears_)},
       {"modern_uv_limited_primitives",
        static_cast<int64_t>(frameModernUVLimitedPrimitives_)},
       {"modern_uv_flipped_u_primitives",
        static_cast<int64_t>(frameModernUVFlippedUPrimitives_)},
       {"modern_uv_flipped_v_primitives",
        static_cast<int64_t>(frameModernUVFlippedVPrimitives_)},
       {"modern_uv_clamped_samples",
        static_cast<int64_t>(frameModernUVClampedSamples_)},
       {"modern_uv_sample_cache_hits",
        static_cast<int64_t>(frameModernUVSampleCacheHits_)},
       {"modern_uv_sample_cache_misses",
        static_cast<int64_t>(frameModernUVSampleCacheMisses_)},
       {"modern_depth_epoch_id", static_cast<int64_t>(depthEpochId_)},
       {"modern_depth_epoch_has_mean", depthEpochHasMean_ ? 1 : 0},
       {"modern_depth_epoch_mean_q16",
        static_cast<int64_t>(std::llround(depthEpochLastMean_ * 65536.0))}});
  ps1::metrics::count("gpu.frame.precise_triangle", framePreciseTriangles_);
  ps1::metrics::count("gpu.frame.compatibility_triangle",
                      frameCompatibilityTriangles_);
  ps1::metrics::count("gpu.frame.screen_2d_triangle", frameScreen2DTriangles_);
  ps1::metrics::count("gpu.frame.partial_precision_packet",
                      framePartialPrecisionPackets_);
  ps1::metrics::count("gpu.frame.bounds_reject", frameBoundsRejects_);
  ps1::metrics::count("gpu.frame.path_toggle", framePathToggles_);
  ps1::metrics::count("gpu.modern.raster.covered", frameModernCoveredSamples_);
  ps1::metrics::count("gpu.modern.raster.reject.invalid_depth",
                      frameModernInvalidDepthSamples_);
  ps1::metrics::count("gpu.modern.raster.reject.mask",
                      frameModernMaskRejectedSamples_);
  ps1::metrics::count("gpu.modern.raster.reject.depth",
                      frameModernDepthRejectedSamples_);
  ps1::metrics::count("gpu.modern.raster.reject.transparent_texel",
                      frameModernTransparentSamples_);
  ps1::metrics::count("gpu.modern.sample", frameModernAcceptedSamples_);
  ps1::metrics::count("gpu.modern.raster.empty", frameModernEmptyTriangles_);
  ps1::metrics::count("gpu.modern.raster.empty.depth_only",
                      frameModernEmptyDepthOnly_);
  ps1::metrics::count("gpu.modern.raster.empty.transparent_only",
                      frameModernEmptyTransparentOnly_);
  ps1::metrics::count("gpu.modern.raster.empty.mask_only",
                      frameModernEmptyMaskOnly_);
  ps1::metrics::count("gpu.modern.raster.empty.invalid_depth_only",
                      frameModernEmptyInvalidDepthOnly_);
  ps1::metrics::count("gpu.modern.raster.empty.mixed", frameModernEmptyMixed_);
  ps1::metrics::count("gpu.modern.native_bounds_skip",
                      frameModernNativeBoundsSkips_);
  ps1::metrics::count("gpu.modern.projective_invalid",
                      frameModernProjectiveInvalid_);
  ps1::metrics::count("gpu.modern.projective_context_mismatch",
                      frameModernContextMismatches_);
  ps1::metrics::count("gpu.modern.near_clip.discarded",
                      frameModernNearClipDiscarded_);
  ps1::metrics::count("gpu.modern.near_clip.discarded.native_coverage",
                      frameModernNearClipDiscardedNativeCoverage_);
  ps1::metrics::count("gpu.modern.near_clip.discarded.native_empty",
                      frameModernNearClipDiscardedNativeEmpty_);
  ps1::metrics::count("gpu.modern.near_clip.polygon",
                      frameModernNearClipPolygons_);
  ps1::metrics::count("gpu.modern.near_clip.output_triangle",
                      frameModernNearClipOutputTriangles_);
  ps1::metrics::count("gpu.modern.near_saturated.triangle",
                      frameModernNearSaturatedTriangles_);
  ps1::metrics::count("gpu.modern.near_saturated.vertex",
                      frameModernNearSaturatedVertices_);
  ps1::metrics::count("gpu.modern.precise_degenerate",
                      frameModernPreciseDegenerate_);
  ps1::metrics::count("gpu.modern.precise_degenerate.native_coverage",
                      frameModernPreciseDegenerateNativeCoverage_);
  ps1::metrics::count("gpu.modern.precise_degenerate.native_empty",
                      frameModernPreciseDegenerateNativeEmpty_);
  ps1::metrics::count("gpu.modern.outside_draw_area",
                      frameModernOutsideDrawArea_);
  ps1::metrics::count("gpu.modern.outside_draw_area.native_coverage",
                      frameModernOutsideDrawAreaNativeCoverage_);
  ps1::metrics::count("gpu.modern.outside_draw_area.native_empty",
                      frameModernOutsideDrawAreaNativeEmpty_);
  ps1::metrics::count("gpu.modern.depth.packet.eligible",
                      frameModernDepthEligiblePackets_);
  ps1::metrics::count("gpu.modern.depth.packet.invalid_w",
                      frameModernDepthInvalidPackets_);
  ps1::metrics::count("gpu.modern.depth.packet.blend_visibility_boundary",
                      frameModernDepthBlendVisibilityBoundaryPackets_);
  ps1::metrics::count("gpu.modern.depth.sample.visibility_boundary",
                      frameModernDepthVisibilityBoundarySamples_);
  ps1::metrics::count("gpu.modern.depth.epoch.clear.draw_area",
                      frameModernDepthEpochDrawAreaClears_);
  ps1::metrics::count("gpu.modern.depth.epoch.clear.z_jump",
                      frameModernDepthEpochZJumpClears_);
  ps1::metrics::count("gpu.modern.uv.limit.primitive",
                      frameModernUVLimitedPrimitives_);
  ps1::metrics::count("gpu.modern.uv.flip_u.primitive",
                      frameModernUVFlippedUPrimitives_);
  ps1::metrics::count("gpu.modern.uv.flip_v.primitive",
                      frameModernUVFlippedVPrimitives_);
  ps1::metrics::count("gpu.modern.uv.clamp.sample",
                      frameModernUVClampedSamples_);
  ps1::metrics::count("gpu.modern.uv.sample_cache.hit",
                      frameModernUVSampleCacheHits_);
  ps1::metrics::count("gpu.modern.uv.sample_cache.miss",
                      frameModernUVSampleCacheMisses_);
  if (reusedSnapshot)
    ps1::metrics::count("gpu.frame.snapshot_reused");
  framePreciseTriangles_ = 0;
  frameCompatibilityTriangles_ = 0;
  frameScreen2DTriangles_ = 0;
  framePartialPrecisionPackets_ = 0;
  frameBoundsRejects_ = 0;
  framePathToggles_ = 0;
  frameModernCoveredSamples_ = 0;
  frameModernInvalidDepthSamples_ = 0;
  frameModernMaskRejectedSamples_ = 0;
  frameModernDepthRejectedSamples_ = 0;
  frameModernTransparentSamples_ = 0;
  frameModernAcceptedSamples_ = 0;
  frameModernEmptyTriangles_ = 0;
  frameModernEmptyDepthOnly_ = 0;
  frameModernEmptyTransparentOnly_ = 0;
  frameModernEmptyMaskOnly_ = 0;
  frameModernEmptyInvalidDepthOnly_ = 0;
  frameModernEmptyMixed_ = 0;
  frameModernNativeBoundsSkips_ = 0;
  frameModernProjectiveInvalid_ = 0;
  frameModernContextMismatches_ = 0;
  frameModernNearClipDiscarded_ = 0;
  frameModernNearClipDiscardedNativeCoverage_ = 0;
  frameModernNearClipDiscardedNativeEmpty_ = 0;
  frameModernNearClipPolygons_ = 0;
  frameModernNearClipOutputTriangles_ = 0;
  frameModernNearSaturatedTriangles_ = 0;
  frameModernNearSaturatedVertices_ = 0;
  frameModernPreciseDegenerate_ = 0;
  frameModernPreciseDegenerateNativeCoverage_ = 0;
  frameModernPreciseDegenerateNativeEmpty_ = 0;
  frameModernOutsideDrawArea_ = 0;
  frameModernOutsideDrawAreaNativeCoverage_ = 0;
  frameModernOutsideDrawAreaNativeEmpty_ = 0;
  frameModernDepthEligiblePackets_ = 0;
  frameModernDepthInvalidPackets_ = 0;
  frameModernDepthBlendVisibilityBoundaryPackets_ = 0;
  frameModernDepthVisibilityBoundarySamples_ = 0;
  frameModernDepthEpochDrawAreaClears_ = 0;
  frameModernDepthEpochZJumpClears_ = 0;
  frameModernUVLimitedPrimitives_ = 0;
  frameModernUVFlippedUPrimitives_ = 0;
  frameModernUVFlippedVPrimitives_ = 0;
  frameModernUVClampedSamples_ = 0;
  frameModernUVSampleCacheHits_ = 0;
  frameModernUVSampleCacheMisses_ = 0;
  lastLatchedSubmissionId_ = lastCompletedSubmissionId_;
  lastLatchedMutationSerial_ = mutationSerial_;
}

void GPU::beginCommandBatch() {
  gpuTimelineMutex_.lock();
  if (commandBatchDepth_++ == 0) {
    activeSubmissionId_ = ++nextSubmissionId_;
    submissionStats_ = {};
    submissionStats_.startVblank = ps1::metrics::traceFrame();
  }
}

void GPU::endCommandBatch() {
  if (commandBatchDepth_ == 0)
    return;
  if (--commandBatchDepth_ == 0) {
    ps1::metrics::trace(
        "gpu.submission_end",
        {{"submission_id", static_cast<int64_t>(activeSubmissionId_)},
         {"start_vblank", static_cast<int64_t>(submissionStats_.startVblank)},
         {"end_vblank", static_cast<int64_t>(ps1::metrics::traceFrame())},
         {"packet_count", static_cast<int64_t>(submissionStats_.packets)},
         {"precise_packets",
          static_cast<int64_t>(submissionStats_.precisePackets)},
         {"compatibility_packets",
          static_cast<int64_t>(submissionStats_.compatibilityPackets)},
         {"screen_2d_packets",
          static_cast<int64_t>(submissionStats_.screen2DPackets)},
         {"partial_precision_packets",
          static_cast<int64_t>(submissionStats_.partialPrecisionPackets)},
         {"bounds_rejects",
          static_cast<int64_t>(submissionStats_.boundsRejects)},
         {"path_toggles", static_cast<int64_t>(submissionStats_.pathToggles)}});
    ps1::metrics::finishGpuTraceSubmission(activeSubmissionId_);
    lastCompletedSubmissionId_ = activeSubmissionId_;
    activeSubmissionId_ = 0;
  }
  gpuTimelineMutex_.unlock();
}

void GPU::writeGP0BatchWord(uint32_t val, uint32_t sourceAddress,
                            const PGXPValue *precision,
                            PGXPSubmissionPath submissionPath,
                            GP0DmaWordProvenance dmaProvenance) {
  if (commandBatchDepth_ == 0) {
    writeGP0(val, sourceAddress, precision, submissionPath, dmaProvenance);
    return;
  }
  writeGP0Unlocked(val, sourceAddress, precision, submissionPath,
                   dmaProvenance);
}

uint64_t GPU::activeSubmissionId() const {
  std::lock_guard<std::recursive_mutex> lock(gpuTimelineMutex_);
  return activeSubmissionId_;
}

std::size_t
GPU::copyDisplayVRAM(Color16 *destination, std::size_t pixelCount,
                     DisplayState *state,
                     replay::CrashReplayFramePtr *replayFrame) const {
  if (destination == nullptr || pixelCount == 0)
    return 0;
  std::lock_guard<std::mutex> lock(displayMutex_);
  const std::size_t count = std::min(pixelCount, displayVram_.size());
  std::copy_n(displayVram_.begin(), count, destination);
  if (state != nullptr)
    *state = displaySnapshot_;
  if (replayFrame != nullptr)
    *replayFrame = displayReplayFrame_;
  return count;
}

GPU::DisplayCopyStatus GPU::copyDisplayVRAMIfChanged(
    std::vector<Color16> &destination, uint64_t knownContentRevision,
    DisplayState *state, replay::CrashReplayFramePtr *replayFrame) const {
  std::lock_guard<std::mutex> lock(displayMutex_);
  if (state != nullptr)
    *state = displaySnapshot_;
  if (replayFrame != nullptr)
    *replayFrame = displayReplayFrame_;
  if (knownContentRevision == displaySnapshot_.contentRevision)
    return DisplayCopyStatus::Unchanged;
  destination = displayVram_;
  return DisplayCopyStatus::Copied;
}

bool GPU::copyDisplayRGBA8(std::vector<Color32> &destination, uint32_t &width,
                           uint32_t &height, DisplayState *state) const {
  return copyDisplayRGBA8IfChanged(destination, width, height, UINT64_MAX,
                                   state) != DisplayCopyStatus::Unavailable;
}

GPU::DisplayCopyStatus GPU::copyDisplayRGBA8IfChanged(
    std::vector<Color32> &destination, uint32_t &width, uint32_t &height,
    uint64_t knownContentRevision, DisplayState *state) const {
  std::lock_guard<std::mutex> lock(displayMutex_);
  if (displayRgba8_.empty() || displayRgba8Width_ == 0 ||
      displayRgba8Height_ == 0) {
    destination.clear();
    width = 0;
    height = 0;
    return DisplayCopyStatus::Unavailable;
  }
  width = displayRgba8Width_;
  height = displayRgba8Height_;
  if (state != nullptr)
    *state = displaySnapshot_;
  if (knownContentRevision == displaySnapshot_.contentRevision)
    return DisplayCopyStatus::Unchanged;
  destination = displayRgba8_;
  return DisplayCopyStatus::Copied;
}

uint32_t GPU::readGPUSTAT() const {
  std::lock_guard<std::recursive_mutex> lock(gpuTimelineMutex_);
  // The transfer cursor is shared by CPU GPUREAD and DMA2. PsyQ _drs
  // waits for this bit before starting a VRAM read; a permanently clear
  // bit times out StoreImage2 before DMA can even start.
  return (gpuStat_ & ~(1u << 27)) |
         (vramTransfer_.isReadingFromVRAM ? (1u << 27) : 0u);
}

uint32_t GPU::readGPUREAD() {
  std::lock_guard<std::recursive_mutex> lock(gpuTimelineMutex_);
  if (vramTransfer_.isReadingFromVRAM) {
    auto takePixel = [&]() -> uint16_t {
      uint32_t x = vramTransfer_.currX;
      uint32_t y = vramTransfer_.currY;
      uint16_t p = vram_[y * VRAM_WIDTH + x].raw;

      vramTransfer_.currX++;
      if (vramTransfer_.currX >= vramTransfer_.srcX + vramTransfer_.width) {
        vramTransfer_.currX = vramTransfer_.srcX;
        vramTransfer_.currY++;
      }
      return p;
    };

    uint16_t p1 = takePixel();
    uint16_t p2 = takePixel();

    vramTransfer_.transferWordsRemaining--;
    if (vramTransfer_.transferWordsRemaining == 0) {
      vramTransfer_.isReadingFromVRAM = false;
    }

    return p1 | (p2 << 16);
  }
  return gpuRead_;
}

void GPU::writeGP0(uint32_t val, uint32_t sourceAddress,
                   const PGXPValue *precision,
                   PGXPSubmissionPath submissionPath,
                   GP0DmaWordProvenance dmaProvenance) {
  std::lock_guard<std::recursive_mutex> timelineLock(gpuTimelineMutex_);
  writeGP0Unlocked(val, sourceAddress, precision, submissionPath,
                   dmaProvenance);
}

void GPU::writeGP0Unlocked(uint32_t val, uint32_t sourceAddress,
                           const PGXPValue *precision,
                           PGXPSubmissionPath submissionPath,
                           GP0DmaWordProvenance dmaProvenance) {
  ++mutationSerial_;
  const PGXPRamLookup ramLookup =
      sourceAddress == UINT32_MAX ? PGXPRamLookup::NotApplicable
                                  : consumePGXPRamLookup(sourceAddress, val);
  if (ps1::metrics::gpuWordTraceEnabled()) {
    const int64_t traceSource =
        sourceAddress == UINT32_MAX ? -1 : static_cast<int64_t>(sourceAddress);
    const bool signedProjection =
        precision != nullptr && precision->hasSignedProjection(val);
    const uint8_t intrinsicProvenance =
        signedProjection
            ? precision->flags & PGXPValue::VALID_INTRINSIC_PROVENANCE
            : 0;
    if (signedProjection) {
      ps1::metrics::noteGpuProjectionForTrace(precision->projectionId,
                                              activeSubmissionId_);
    }
    ps1::metrics::trace(
        "gpu.gp0_word",
        {{"word", val},
         {"word_address", traceSource},
         {"path", static_cast<int64_t>(submissionPath)},
         {"ram_lookup", static_cast<int64_t>(ramLookup)},
         {"submission_id", static_cast<int64_t>(activeSubmissionId_)},
         {"command_word_index", static_cast<int64_t>(commandQueue_.size())},
         {"projection_id",
          signedProjection ? static_cast<int64_t>(precision->projectionId) : 0},
         {"projection_context",
          signedProjection ? static_cast<int64_t>(precision->projectionContext)
                           : 0},
         {"intrinsic_provenance", intrinsicProvenance},
         {"transform_lineage",
          signedProjection && (intrinsicProvenance &
                               PGXPValue::VALID_TRANSFORM_LINEAGE) != 0
              ? static_cast<int64_t>(precision->projectionTransformLineage)
              : 0},
         {"model_source",
          signedProjection &&
                  (intrinsicProvenance &
                   PGXPValue::VALID_MODEL_VERTEX_SOURCE) != 0 &&
                  precision->modelVertexSource != 0
              ? static_cast<int64_t>(precision->modelVertexSource - 1u)
              : -1},
         {"model_source_fingerprint",
          signedProjection &&
                  (intrinsicProvenance & PGXPValue::VALID_MODEL_VALUE_FLOW) != 0
              ? static_cast<int64_t>(precision->modelValueFlowSourceFingerprint)
              : 0},
         {"model_program_fingerprint",
          signedProjection &&
                  (intrinsicProvenance & PGXPValue::VALID_MODEL_VALUE_FLOW) != 0
              ? static_cast<int64_t>(
                    precision->modelValueFlowProgramFingerprint)
              : 0},
         {"dma_provenance", dmaProvenance.exact ? 1 : 0},
         {"dma_list_start", dmaProvenance.exact
                                ? static_cast<int64_t>(dmaProvenance.listStart)
                                : -1},
         {"dma_node_address",
          dmaProvenance.exact ? static_cast<int64_t>(dmaProvenance.nodeAddress)
                              : -1},
         {"dma_traversal_ordinal",
          dmaProvenance.exact
              ? static_cast<int64_t>(dmaProvenance.traversalOrdinal)
              : -1},
         {"ot_anchor_address",
          dmaProvenance.orderingTableAnchorExact
              ? static_cast<int64_t>(dmaProvenance.orderingTableAnchorAddress)
              : -1},
         {"ot_anchor_exact", dmaProvenance.orderingTableAnchorExact ? 1 : 0},
         {"signed_projection", signedProjection ? 1 : 0}});
  }

  if (ps1::metrics::enabled()) {
    gp0Words_.fetch_add(1, std::memory_order_relaxed);
    if (!vramTransfer_.isWritingToVRAM && !isCommandExecuting_) {
      gp0Hist_[val >> 24].fetch_add(1, std::memory_order_relaxed);
    }
  }
  {
    // This is a bounded startup diagnostic. The old unordered_map kept
    // hashing every GP0 word after its final report at word 5000.
    static uint32_t gp0Count = 0;
    static std::array<uint32_t, 256> opcodeHist{};
    if (gp0Count < 5000u) {
      ++gp0Count;
      if (!vramTransfer_.isWritingToVRAM && !isCommandExecuting_)
        ++opcodeHist[val >> 24];
      if (gp0Count == 500 || gp0Count == 2000 || gp0Count == 5000) {
        fmt::print(stderr, "[GPU] GP0 command histogram after {} calls:\n",
                   gp0Count);
        for (std::size_t op = 0; op < opcodeHist.size(); ++op) {
          if (opcodeHist[op] != 0u) {
            fmt::print(stderr, "  opcode 0x{:02X}: {} times\n", op,
                       opcodeHist[op]);
          }
        }
      }
    }
  }
  if (vramTransfer_.isWritingToVRAM) {
    uint16_t p1 = val & 0xFFFF;
    uint16_t p2 = (val >> 16) & 0xFFFF;

    auto putPixel = [&](uint16_t p) {
      if (vramTransfer_.transferPixelsRemaining == 0)
        return;
      uint32_t x = vramTransfer_.currX % VRAM_WIDTH;
      uint32_t y = vramTransfer_.currY % VRAM_HEIGHT;
      Color16 &destination = vram_[y * VRAM_WIDTH + x];
      const bool accepted =
          !checkMaskBeforeDraw_ || (destination.raw & 0x8000u) == 0;
      if (accepted) {
        destination.raw = p | (setMaskWhileDrawing_ ? 0x8000u : 0u);
        if (enhancedSurface_.enabled())
          enhancedSurface_.mirrorNativePixel(x, y, destination.raw);
      }
      // Record after the authoritative native decision/write. The raw source
      // halfword and exact acceptance bit let Vulkan reproduce both E6 rules;
      // rejected pixels still consume their transfer position.
      if (replayRecorder_.enabled())
        replayRecorder_.appendCpuUploadPixel(p, accepted);
      --vramTransfer_.transferPixelsRemaining;
      vramTransfer_.currX++;
      if (vramTransfer_.currX >= vramTransfer_.destX + vramTransfer_.width) {
        vramTransfer_.currX = vramTransfer_.destX;
        vramTransfer_.currY++;
      }
    };

    putPixel(p1);
    putPixel(p2);

    vramTransfer_.transferWordsRemaining--;
    if (vramTransfer_.transferWordsRemaining == 0) {
      vramTransfer_.isWritingToVRAM = false;
      if (replayRecorder_.enabled())
        replayRecorder_.finishCpuUpload();
    }
    return;
  }

  // Push word to command queue
  commandQueue_.push_back(val);
  commandSourceQueue_.push_back(sourceAddress);
  if (precision != nullptr || (ramLookup != PGXPRamLookup::NotApplicable &&
                               ramLookup != PGXPRamLookup::Unobserved)) {
    commandPrecisionQueue_.emplace_back(precision ? *precision : PGXPValue{});
    commandPrecisionQueue_.back()->ramLookup = ramLookup;
    commandPrecisionQueue_.back()->submissionPath = submissionPath;
  } else {
    commandPrecisionQueue_.emplace_back(std::nullopt);
  }
  commandDmaProvenanceQueue_.push_back(dmaProvenance);

  if (!isCommandExecuting_) {
    // Start decoding a new command
    uint32_t opcode = val >> 24;
    polylineCommand_ = false;
    switch (opcode) {
    case 0x00:
      expectedCommandWords_ = 1;
      break; // NOP
    case 0x01:
      expectedCommandWords_ = 1;
      break; // Clear Cache
    case 0x02:
      expectedCommandWords_ = 3;
      break; // Fill Rectangle in VRAM
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x23:
      expectedCommandWords_ = 4;
      break; // Monochrome 3-point polygon
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x27:
      expectedCommandWords_ = 7;
      break; // Textured 3-point polygon
    case 0x28:
    case 0x29:
    case 0x2A:
    case 0x2B:
      expectedCommandWords_ = 5;
      break; // Monochrome 4-point polygon
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
      expectedCommandWords_ = 9;
      break; // Textured 4-point polygon
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
      expectedCommandWords_ = 6;
      break; // Gouraud 3-point polygon
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x37:
      expectedCommandWords_ = 9;
      break; // Gouraud Textured 3-point polygon
    case 0x38:
    case 0x39:
    case 0x3A:
    case 0x3B:
      expectedCommandWords_ = 8;
      break; // Gouraud 4-point polygon
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
      expectedCommandWords_ = 12;
      break; // Gouraud Textured 4-point polygon
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47:
      expectedCommandWords_ = 3;
      break; // Monochrome Line
    case 0x48:
    case 0x49:
    case 0x4A:
    case 0x4B:
    case 0x4C:
    case 0x4D:
    case 0x4E:
    case 0x4F:
      expectedCommandWords_ = 3;
      polylineCommand_ = true;
      break;
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:
      expectedCommandWords_ = 4;
      break; // Gouraud Line
    case 0x58:
    case 0x59:
    case 0x5A:
    case 0x5B:
    case 0x5C:
    case 0x5D:
    case 0x5E:
    case 0x5F:
      expectedCommandWords_ = 4;
      polylineCommand_ = true;
      break;
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x63:
      expectedCommandWords_ = 3;
      break; // Variable Rect
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x67:
      expectedCommandWords_ = 4;
      break; // Variable Tex Rect
    case 0x68:
    case 0x69:
    case 0x6A:
    case 0x6B:
      expectedCommandWords_ = 2;
      break; // 1x1 Rect
    case 0x6C:
    case 0x6D:
    case 0x6E:
    case 0x6F:
      expectedCommandWords_ = 3;
      break; // 1x1 Tex Rect
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
      expectedCommandWords_ = 2;
      break; // 8x8 Rect
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
      expectedCommandWords_ = 3;
      break; // 8x8 Tex Rect
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7B:
      expectedCommandWords_ = 2;
      break; // 16x16 Rect
    case 0x7C:
    case 0x7D:
    case 0x7E:
    case 0x7F:
      expectedCommandWords_ = 3;
      break; // 16x16 Tex Rect
    case 0x80:
    case 0x81:
    case 0x82:
    case 0x83:
    case 0x84:
    case 0x85:
    case 0x86:
    case 0x87:
    case 0x88:
    case 0x89:
    case 0x8A:
    case 0x8B:
    case 0x8C:
    case 0x8D:
    case 0x8E:
    case 0x8F:
    case 0x90:
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97:
    case 0x98:
    case 0x99:
    case 0x9A:
    case 0x9B:
    case 0x9C:
    case 0x9D:
    case 0x9E:
    case 0x9F:
      expectedCommandWords_ = 4;
      break; // Copy Rectangle (VRAM to VRAM)
    case 0xA0:
    case 0xA1:
    case 0xA2:
    case 0xA3:
    case 0xA4:
    case 0xA5:
    case 0xA6:
    case 0xA7:
    case 0xA8:
    case 0xA9:
    case 0xAA:
    case 0xAB:
    case 0xAC:
    case 0xAD:
    case 0xAE:
    case 0xAF:
    case 0xB0:
    case 0xB1:
    case 0xB2:
    case 0xB3:
    case 0xB4:
    case 0xB5:
    case 0xB6:
    case 0xB7:
    case 0xB8:
    case 0xB9:
    case 0xBA:
    case 0xBB:
    case 0xBC:
    case 0xBD:
    case 0xBE:
    case 0xBF:
      expectedCommandWords_ = 3;
      break; // Copy Rectangle (CPU to VRAM)
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC4:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC8:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xCC:
    case 0xCD:
    case 0xCE:
    case 0xCF:
    case 0xD0:
    case 0xD1:
    case 0xD2:
    case 0xD3:
    case 0xD4:
    case 0xD5:
    case 0xD6:
    case 0xD7:
    case 0xD8:
    case 0xD9:
    case 0xDA:
    case 0xDB:
    case 0xDC:
    case 0xDD:
    case 0xDE:
    case 0xDF:
      expectedCommandWords_ = 3;
      break; // Copy Rectangle (VRAM to CPU)
    case 0xE1:
      expectedCommandWords_ = 1;
      break; // Draw Mode setting
    case 0xE2:
      expectedCommandWords_ = 1;
      break; // Texture Window setting
    case 0xE3:
      expectedCommandWords_ = 1;
      break; // Set Drawing Area top left
    case 0xE4:
      expectedCommandWords_ = 1;
      break; // Set Drawing Area bottom right
    case 0xE5:
      expectedCommandWords_ = 1;
      break; // Set Drawing Offset
    case 0xE6:
      expectedCommandWords_ = 1;
      break; // Mask Bit setting
    default:
      // Unknown or unimplemented command
      fmt::print("[GPU] WARNING: Unknown GP0 opcode 0x{:02X}\n", opcode);
      expectedCommandWords_ = 1;
      break;
    }
    isCommandExecuting_ = true;
  }

  if (polylineCommand_) {
    const uint8_t opcode = static_cast<uint8_t>(commandQueue_.front() >> 24);
    const bool gouraud = (opcode & 0x10u) != 0;
    const std::size_t terminatorIndex = commandQueue_.size() - 1u;
    const bool validTerminatorSlot = !gouraud || (terminatorIndex & 1u) == 0u;
    const bool hasMinimumVertices =
        commandQueue_.size() > expectedCommandWords_;
    const bool terminator =
        (val & UINT32_C(0xF000F000)) == UINT32_C(0x50005000);
    if (hasMinimumVertices && validTerminatorSlot && terminator) {
      // The terminator is framing, not a coordinate/color operand.
      commandQueue_.pop_back();
      commandSourceQueue_.pop_back();
      commandPrecisionQueue_.pop_back();
      commandDmaProvenanceQueue_.pop_back();
      executeGP0Command();
      commandQueue_.clear();
      commandSourceQueue_.clear();
      commandPrecisionQueue_.clear();
      commandDmaProvenanceQueue_.clear();
      isCommandExecuting_ = false;
      polylineCommand_ = false;
    }
    return;
  }

  if (commandQueue_.size() >= expectedCommandWords_) {
    executeGP0Command();
    commandQueue_.clear();
    commandSourceQueue_.clear();
    commandPrecisionQueue_.clear();
    commandDmaProvenanceQueue_.clear();
    isCommandExecuting_ = false;
    polylineCommand_ = false;
  }
}

void GPU::writeGP1(uint32_t val) {
  std::lock_guard<std::recursive_mutex> timelineLock(gpuTimelineMutex_);
  ++mutationSerial_;
  uint32_t opcode = val >> 24;
  switch (opcode) {
  case 0x00: // Reset GPU
    // Full hardware reset, but preserve display enable and area so we always
    // get a picture. On real hardware the BIOS re-sends GP1(0x03/0x05/...)
    // after reset; our HLE BIOS skips that, so the screen would go black
    // otherwise.
    {
      uint32_t savedStat = gpuStat_;
      uint32_t savedX = displayVRAMXStart_;
      uint32_t savedY = displayVRAMYStart_;
      uint32_t savedX1 = displayX1_;
      uint32_t savedX2 = displayX2_;
      uint32_t savedY1 = displayY1_;
      uint32_t savedY2 = displayY2_;
      bool savedModeSet = displayModeSet_;
      bool savedAreaSet = displayAreaSet_;
      bool savedHorizontalRangeSet = horizontalDisplayRangeSet_;
      bool savedVerticalRangeSet = verticalDisplayRangeSet_;
      // GP1(00h) resets command/control state, not VRAM. Keeping video memory
      // is required for framebuffer feedback and matches both SwanStation and
      // Beetle PSX; reset() remains the explicit power-on/reset API.
      resetState(false);
      // Restore display pipeline state so the renderer keeps showing frames.
      gpuStat_ = (gpuStat_ & ~(1u << 23)) | (savedStat & (1u << 23));
      displayVRAMXStart_ = savedX;
      displayVRAMYStart_ = savedY;
      displayX1_ = savedX1;
      displayX2_ = savedX2;
      displayY1_ = savedY1;
      displayY2_ = savedY2;
      displayModeSet_ = savedModeSet;
      displayAreaSet_ = savedAreaSet;
      horizontalDisplayRangeSet_ = savedHorizontalRangeSet;
      verticalDisplayRangeSet_ = savedVerticalRangeSet;
    }
    ps1::metrics::trace("gpu.reset", {{"word", val}});
    break;
  case 0x01: // Reset Command Buffer
    if (replayRecorder_.enabled() && replayRecorder_.cpuUploadActive())
      replayRecorder_.abortCpuUpload();
    commandQueue_.clear();
    commandSourceQueue_.clear();
    commandPrecisionQueue_.clear();
    commandDmaProvenanceQueue_.clear();
    isCommandExecuting_ = false;
    polylineCommand_ = false;
    replayPaintCaptureActive_ = false;
    replayPaintCapture_.clear();
    vramTransfer_ = {};
    break;
  case 0x03: // Display Enable
    // Bit 0: 0=On, 1=Off. Updates GPUSTAT bit 23.
    gpuStat_ = (gpuStat_ & ~(1 << 23)) | ((val & 1) << 23);
    ps1::metrics::trace("gpu.display_enable",
                        {{"enabled", (val & 1) == 0 ? 1 : 0}});
    break;
  case 0x04: // DMA Direction
    // Update GPUSTAT bits 29-30
    gpuStat_ = (gpuStat_ & ~(3 << 29)) | ((val & 3) << 29);
    ps1::metrics::trace("gpu.dma_direction", {{"direction", val & 3}});
    break;
  case 0x05: // Start of Display Area (in VRAM)
    displayVRAMXStart_ = val & 0x3FF;
    displayVRAMYStart_ = (val >> 10) & 0x1FF;
    displayAreaSet_ = true;
    {
      static int cnt = 0;
      if (cnt++ < 20)
        fmt::print(
            stderr,
            "[GPU] GP1(0x05) (sample, first 20): display area -> ({},{})\n",
            displayVRAMXStart_, displayVRAMYStart_);
    }
    ps1::metrics::count("gp1.display_area");
    ps1::metrics::setState("display.x",
                           static_cast<int64_t>(displayVRAMXStart_));
    ps1::metrics::setState("display.y",
                           static_cast<int64_t>(displayVRAMYStart_));
    ps1::metrics::trace(
        "gpu.display_start",
        {{"x", displayVRAMXStart_}, {"y", displayVRAMYStart_}, {"word", val}});
    break;
  case 0x06: // Horizontal Display Range
    displayX1_ = val & 0xFFF;
    displayX2_ = (val >> 12) & 0xFFF;
    horizontalDisplayRangeSet_ = true;
    ps1::metrics::trace(
        "gpu.display_range_h",
        {{"x0", displayX1_}, {"x1", displayX2_}, {"word", val}});
    break;
  case 0x07: // Vertical Display Range
    displayY1_ = val & 0x3FF;
    displayY2_ = (val >> 10) & 0x3FF;
    verticalDisplayRangeSet_ = true;
    ps1::metrics::trace(
        "gpu.display_range_v",
        {{"y0", displayY1_}, {"y1", displayY2_}, {"word", val}});
    break;
  case 0x08: // Display Mode
    // Bit 0-1 Horizontal Resolution 1 + 2
    // Bit 2   Vertical Resolution
    // Bit 3   Video Mode (NTSC/PAL)
    // Bit 4   Display Area Color Depth (0=15bit, 1=24bit)
    // Bit 5   Vertical Interlace
    // Bit 6   Horizontal Resolution 2
    // Bit 7   Reverseflag (0=Normal, 1=Distorted)
    // We update GPUSTAT bits 17-19 (Hres), 20 (Vres), etc.
    // GP1 bits 0..5 map to GPUSTAT 17..22.  HRes2 is the exception:
    // GP1 bit 6 maps to GPUSTAT bit 16, not bit 23 (display disable).
    gpuStat_ =
        (gpuStat_ & ~0x7F0000u) | ((val & 0x3Fu) << 17) | ((val & 0x40u) << 10);
    displayModeSet_ = true;
    ps1::metrics::trace("gpu.display_mode",
                        {{"mode", val & 0xFF}, {"word", val}});
    break;
  case 0x09: // Allow Texture Disable
    // This is a write mask, not draw mode itself. Like SwanStation, changing
    // it does not retroactively alter GPUSTAT bit 15; the next E1 or embedded
    // polygon tpage applies the gate while latching its draw-mode bit.
    allowTextureDisable_ = (val & 1u) != 0;
    ps1::metrics::trace(
        "gpu.texture_disable_gate",
        {{"allowed", allowTextureDisable_ ? 1 : 0}, {"word", val}});
    break;
  case 0x10: // Get GPU Info
  {
    uint32_t param = val & 0x0F; // Only bits 0-3 matter (mirrored 0-7)
    switch (param & 0x07) {
    case 2: // Texture Window
      gpuRead_ = ((uint32_t)texWindowMaskX_) |
                 ((uint32_t)texWindowMaskY_ << 5) |
                 ((uint32_t)texWindowOffsetX_ << 10) |
                 ((uint32_t)texWindowOffsetY_ << 15);
      break;
    case 3: // Draw Area Top Left
      gpuRead_ = ((uint32_t)drawAreaX1_) | ((uint32_t)drawAreaY1_ << 10);
      break;
    case 4: // Draw Area Bottom Right
      gpuRead_ = ((uint32_t)drawAreaX2_) | ((uint32_t)drawAreaY2_ << 10);
      break;
    case 5: // Draw Offset
      gpuRead_ = ((uint32_t)(drawOffsetX_ & 0x7FF)) |
                 ((uint32_t)(drawOffsetY_ & 0x7FF) << 11);
      break;
    case 7: // GPU Type (usually 2)
      gpuRead_ = 2;
      break;
    default:
      gpuRead_ = 0;
      break;
    }
    break;
  }
  default:
    fmt::print("[GPU] WARNING: Unknown GP1 opcode 0x{:02X}\n", opcode);
    break;
  }
}

std::function<void(uint32_t)> GPU::observePresentationList(uint32_t start,
                                                           const uint8_t *ram) {
  if (!replayRecorder_.enabled())
    return {};
  games::mgs::captureWaterEffects({ram, ps1::Memory::RAM_SIZE});
  std::shared_ptr<games::mgs::SceneRecoveryReplay> sceneRecovery =
      games::mgs::recoverSceneReplay(start, {ram, ps1::Memory::RAM_SIZE});
  if (!sceneRecovery)
    return {};
  return
      [this, sceneRecovery = std::move(sceneRecovery)](uint32_t currentAddr) {
        if (sceneRecovery && currentAddr > sceneRecovery->orderingTable &&
            currentAddr <= sceneRecovery->orderingTable + 256 * 4) {
          const uint32_t slot =
              (currentAddr - sceneRecovery->orderingTable) / 4 - 1;
          replay::BakedDrawState environment;
          environment.drawArea = {drawAreaX1_, drawAreaY1_, drawAreaX2_,
                                  drawAreaY2_};
          environment.drawOffset = {drawOffsetX_, drawOffsetY_};
          environment.texturePage.textureDisable = textureDisable_;
          environment.textureWindow.maskX = texWindowMaskX_;
          environment.textureWindow.maskY = texWindowMaskY_;
          environment.mask = {setMaskWhileDrawing_, checkMaskBeforeDraw_};
          environment.depth.epoch = replayDepthEpoch_;
          environment.dither = ditherEnable_;
          environment.drawToDisplayArea = (gpuStat_ & (1u << 10)) != 0;
          for (auto draw : sceneRecovery->buckets[slot]) {
            if (!games::mgs::bindRecoveredSceneDraw(draw, environment,
                                                    replayNativeVram())) {
              ps1::metrics::count("mgs.scene.recovery_material_rejected");
              continue;
            }
            draw.submissionId = activeSubmissionId_;
            replayRecorder_.recordPresentationDraw(std::move(draw),
                                                   replayNativeVram());
            ps1::metrics::count("mgs.scene.recovered_draws");
            ps1::metrics::trace("mgs.scene.recover",
                                {{"capture", int64_t(sceneRecovery->capture)},
                                 {"bucket", slot},
                                 {"ot", currentAddr}});
          }
        }
      };
}

void GPU::processLinkedList(uint32_t startAddr, const uint8_t *ram,
                            const ps1::Memory *memory) {
  const ScopedGpuMetricTimer linkedListTimer("host.gpu.process_linked_list_us");
  struct BatchGuard {
    GPU &gpu;
    explicit BatchGuard(GPU &owner) : gpu(owner) { gpu.beginCommandBatch(); }
    ~BatchGuard() { gpu.endCommandBatch(); }
  } batch(*this);
  const auto observeList = observePresentationList(startAddr, ram);
  uint32_t currentAddr = startAddr & 0x1FFFFC;
  uint32_t maxNodes = 0x10000; // safety net to prevent infinite loops
  static int llCallCount = 0;
  int nodeCount = 0;
  int totalWords = 0;
  uint32_t orderingTableAnchorAddress = 0;
  bool orderingTableAnchorExact = false;

  while (currentAddr != 0x00FFFFFF && currentAddr != 0xFFFFFF &&
         maxNodes-- > 0) {
    uint32_t header =
        (ram[currentAddr] | (ram[currentAddr + 1] << 8) |
         (ram[currentAddr + 2] << 16) | (ram[currentAddr + 3] << 24));

    uint32_t nextAddr = header & 0xFFFFFF;
    uint32_t numWords = header >> 24;

    // Canonical reverse OTs expose their bucket entry as a zero-word link
    // immediately before any primitive chain. Retain that exact transport
    // witness for subsequent data nodes; do not interpret its address here.
    if (numWords == 0) {
      orderingTableAnchorAddress = currentAddr;
      orderingTableAnchorExact = true;
      if (observeList)
        observeList(currentAddr);
    }

    if (ps1::metrics::gpuWordTraceEnabled()) {
      ps1::metrics::trace(
          "gpu.dma_node",
          {{"list_start", startAddr & 0x1FFFFCu},
           {"node_address", currentAddr},
           {"next_address", nextAddr},
           {"words", numWords},
           {"traversal_ordinal", nodeCount},
           {"ot_anchor_address",
            orderingTableAnchorExact
                ? static_cast<int64_t>(orderingTableAnchorAddress)
                : -1},
           {"ot_anchor_exact", orderingTableAnchorExact ? 1 : 0},
           {"submission_id", static_cast<int64_t>(activeSubmissionId_)}});
    }

    const GP0DmaWordProvenance dmaProvenance{startAddr & 0x1FFFFCu,
                                             currentAddr,
                                             static_cast<uint32_t>(nodeCount),
                                             orderingTableAnchorAddress,
                                             orderingTableAnchorExact,
                                             true};
    for (uint32_t i = 1; i <= numWords; ++i) {
      uint32_t wordAddr = (currentAddr + i * 4) & 0x1FFFFC;
      uint32_t word = (ram[wordAddr] | (ram[wordAddr + 1] << 8) |
                       (ram[wordAddr + 2] << 16) | (ram[wordAddr + 3] << 24));
      PGXPValue precision;
      const PGXPValue *precisionPtr =
          lookupPGXPSubmissionPrecision(memory, wordAddr, word, precision)
              ? &precision
              : nullptr;
      // BatchGuard owns gpuTimelineMutex_ for this complete ordering table;
      // avoid one redundant recursive lock/unlock pair per GP0 word.
      writeGP0Unlocked(word, wordAddr, precisionPtr,
                       PGXPSubmissionPath::DmaLinkedList, dmaProvenance);
      totalWords++;
    }
    nodeCount++;

    if (nextAddr == 0xFFFFFF || nextAddr == 0x00FFFFFF) {
      break;
    }
    currentAddr = nextAddr & 0x1FFFFC;
  }

  llCallCount++;
  if (llCallCount <= 10) {
    fmt::print(
        stderr,
        "[GPU] LinkedList #{}: startAddr=0x{:06X}, nodes={}, GP0 words={}\n",
        llCallCount, startAddr, nodeCount, totalWords);
  }
  ps1::metrics::trace("gpu.dma_list", {{"address", startAddr & 0xFFFFFFu},
                                       {"nodes", nodeCount},
                                       {"words", totalWords}});
}

void GPU::executeGP0Command() {
  if (commandQueue_.size() < expectedCommandWords_) {
    fmt::print(
        stderr,
        "[GPU] BUG: executeGP0Command called with queue={} < expected={}\n",
        commandQueue_.size(), expectedCommandWords_);
    return;
  }
  uint32_t cmd = commandQueue_.front();
  uint32_t opcode = cmd >> 24;

  activeReplayPacketSequence_ = replay::UNASSIGNED_PACKET_SEQUENCE;
  activeReplayPacketUnsupported_ = false;
  if (replayRecorder_.enabled()) {
    activeReplayPacketSequence_ =
        replayRecorder_.observeOpcode(static_cast<uint8_t>(opcode));
    const bool polygon = opcode >= 0x20 && opcode <= 0x3F;
    const bool representedByMutationHandler =
        opcode == 0x02 || (opcode >= 0x40 && opcode <= 0xBF);
    const bool representedWithoutOperation =
        opcode == 0x00 || opcode == 0x01 ||
        (opcode >= 0xC0 && opcode <= 0xDF) ||
        (opcode >= 0xE1 && opcode <= 0xE6);
    if (!polygon && !representedByMutationHandler &&
        !representedWithoutOperation)
      markActiveReplayPacketUnsupported();
  }

  // SwanStation consumes a coalesced E3/E4 change at the first following
  // render command, before it knows whether that draw is projected, 2D or a
  // compatibility fallback. Delaying this until the next precise polygon
  // leaves stale depth when UI/lines/rectangles separate two 3D areas.
  if (opcode >= 0x20 && opcode <= 0x7F) {
    consumeModernDepthDrawAreaEpoch();
    consumeReplayDepthDrawAreaEpoch();
  }

  switch (opcode) {
  case 0x00: // NOP
    break;
  case 0x01: // Clear Cache
    executeClearCache();
    break;
  case 0x02: // Fill Rect
    executeFillRect();
    break;

  // Polygons
  // Opcode bits: [27:25] = type, [24] = gouraud, [23] unused
  // Bit 0 = raw texture, Bit 1 = semi-transparent, Bit 2 = textured

  // Monochrome triangles (all combinations)
  case 0x20:
  case 0x21:
  case 0x22:
  case 0x23:
    executeMonochromePoly3();
    break;
  // Textured triangles
  case 0x24:
  case 0x25:
  case 0x26:
  case 0x27:
    executeTexturedPoly3();
    break;
  // Monochrome quads (all combinations)
  case 0x28:
  case 0x29:
  case 0x2A:
  case 0x2B:
    executeMonochromePoly4();
    break;
  // Textured quads
  case 0x2C:
  case 0x2D:
  case 0x2E:
  case 0x2F:
    executeTexturedPoly4();
    break;

  // Gouraud triangles
  case 0x30:
  case 0x31:
  case 0x32:
  case 0x33:
    executeGouraudPoly3();
    break;
  // Gouraud textured triangles
  case 0x34:
  case 0x35:
  case 0x36:
  case 0x37:
    executeGouraudTexturedPoly3();
    break;
  // Gouraud quads
  case 0x38:
  case 0x39:
  case 0x3A:
  case 0x3B:
    executeGouraudPoly4();
    break;
  // Gouraud textured quads
  case 0x3C:
  case 0x3D:
  case 0x3E:
  case 0x3F:
    executeGouraudTexturedPoly4();
    break;

  // Lines
  case 0x40:
  case 0x41:
  case 0x42:
  case 0x43: // Mono line
  case 0x44:
  case 0x45:
  case 0x46:
  case 0x47:
  case 0x48:
  case 0x49:
  case 0x4A:
  case 0x4B: // Mono poly-line
  case 0x4C:
  case 0x4D:
  case 0x4E:
  case 0x4F:
  case 0x50:
  case 0x51:
  case 0x52:
  case 0x53: // Gouraud line
  case 0x54:
  case 0x55:
  case 0x56:
  case 0x57:
  case 0x58:
  case 0x59:
  case 0x5A:
  case 0x5B: // Gouraud poly-line
  case 0x5C:
  case 0x5D:
  case 0x5E:
  case 0x5F:
    executeLine();
    break;

  // Rectangles
  case 0x60:
  case 0x61:
  case 0x62:
  case 0x63: // Variable size rect
  case 0x64:
  case 0x65:
  case 0x66:
  case 0x67: // Variable size textured rect
  case 0x68:
  case 0x69:
  case 0x6A:
  case 0x6B: // 1x1 dot
  case 0x6C:
  case 0x6D:
  case 0x6E:
  case 0x6F: // 1x1 textured
  case 0x70:
  case 0x71:
  case 0x72:
  case 0x73: // 8x8 rect
  case 0x74:
  case 0x75:
  case 0x76:
  case 0x77: // 8x8 textured rect
  case 0x78:
  case 0x79:
  case 0x7A:
  case 0x7B: // 16x16 rect
  case 0x7C:
  case 0x7D:
  case 0x7E:
  case 0x7F: // 16x16 textured rect
    executeRect();
    break;

  // VRAM Transfers
  case 0x80:
  case 0x81:
  case 0x82:
  case 0x83:
  case 0x84:
  case 0x85:
  case 0x86:
  case 0x87:
  case 0x88:
  case 0x89:
  case 0x8A:
  case 0x8B:
  case 0x8C:
  case 0x8D:
  case 0x8E:
  case 0x8F:
  case 0x90:
  case 0x91:
  case 0x92:
  case 0x93:
  case 0x94:
  case 0x95:
  case 0x96:
  case 0x97:
  case 0x98:
  case 0x99:
  case 0x9A:
  case 0x9B:
  case 0x9C:
  case 0x9D:
  case 0x9E:
  case 0x9F:
    executeCopyVRAM();
    break;
  case 0xA0:
  case 0xA1:
  case 0xA2:
  case 0xA3:
  case 0xA4:
  case 0xA5:
  case 0xA6:
  case 0xA7:
  case 0xA8:
  case 0xA9:
  case 0xAA:
  case 0xAB:
  case 0xAC:
  case 0xAD:
  case 0xAE:
  case 0xAF:
  case 0xB0:
  case 0xB1:
  case 0xB2:
  case 0xB3:
  case 0xB4:
  case 0xB5:
  case 0xB6:
  case 0xB7:
  case 0xB8:
  case 0xB9:
  case 0xBA:
  case 0xBB:
  case 0xBC:
  case 0xBD:
  case 0xBE:
  case 0xBF:
    executeCPUToVRAM();
    break;
  case 0xC0:
  case 0xC1:
  case 0xC2:
  case 0xC3:
  case 0xC4:
  case 0xC5:
  case 0xC6:
  case 0xC7:
  case 0xC8:
  case 0xC9:
  case 0xCA:
  case 0xCB:
  case 0xCC:
  case 0xCD:
  case 0xCE:
  case 0xCF:
  case 0xD0:
  case 0xD1:
  case 0xD2:
  case 0xD3:
  case 0xD4:
  case 0xD5:
  case 0xD6:
  case 0xD7:
  case 0xD8:
  case 0xD9:
  case 0xDA:
  case 0xDB:
  case 0xDC:
  case 0xDD:
  case 0xDE:
  case 0xDF:
    executeVRAMToCPU();
    break;

  // GPU Environment Commands
  case 0xE1: // Draw Mode / Texpage
    ditherEnable_ = (cmd & (1 << 9)) != 0;
    blendMode_ = (cmd >> 5) & 3;
    // Bits 0-8 (tex page X/Y base, semi-transparency, color depth) are the
    // current texture page used by textured sprites, which carry no tpage word.
    currentTexpage_ = cmd & 0x1FF;
    textureRectFlipX_ = (cmd & (1u << 12)) != 0;
    textureRectFlipY_ = (cmd & (1u << 13)) != 0;
    textureDisable_ = allowTextureDisable_ && (cmd & (1u << 11)) != 0;
    if (!allowTextureDisable_ && (cmd & (1u << 11)) != 0)
      ps1::metrics::count("gpu.texture_disable.ignored_e1");
    gpuStat_ = (gpuStat_ & ~(0x7FFu | (1u << 15u))) | (cmd & 0x7FFu) |
               (textureDisable_ ? (1u << 15u) : 0u);
    ps1::metrics::trace(
        "gpu.draw_mode",
        {{"tpage", currentTexpage_},
         {"dither", ditherEnable_ ? 1 : 0},
         {"blend", blendMode_},
         {"texture_disable_allowed", allowTextureDisable_ ? 1 : 0},
         {"texture_disable", textureDisable_ ? 1 : 0},
         {"word", cmd}});
    break;
  case 0xE2: // Set Texture Window
    executeTextureWindow();
    break;
  case 0xE3: // Set Drawing Area top left
  {
    const int32_t x = cmd & 0x3FF;
    const int32_t y = (cmd >> 10) & 0x1FF;
    depthEpochDrawAreaDirty_ |= x != drawAreaX1_ || y != drawAreaY1_;
    replayDepthDrawAreaDirty_ |= x != drawAreaX1_ || y != drawAreaY1_;
    drawAreaX1_ = x;
    drawAreaY1_ = y;
  }
    ps1::metrics::trace(
        "gpu.draw_area_start",
        {{"x", drawAreaX1_}, {"y", drawAreaY1_}, {"word", cmd}});
    break;
  case 0xE4: // Set Drawing Area bottom right
  {
    const int32_t x = cmd & 0x3FF;
    const int32_t y = (cmd >> 10) & 0x1FF;
    depthEpochDrawAreaDirty_ |= x != drawAreaX2_ || y != drawAreaY2_;
    replayDepthDrawAreaDirty_ |= x != drawAreaX2_ || y != drawAreaY2_;
    drawAreaX2_ = x;
    drawAreaY2_ = y;
  }
    ps1::metrics::trace(
        "gpu.draw_area_end",
        {{"x", drawAreaX2_}, {"y", drawAreaY2_}, {"word", cmd}});
    break;
  case 0xE5: // Set Drawing Offset
    drawOffsetX_ = cmd & 0x7FF;
    if (drawOffsetX_ & 0x400)
      drawOffsetX_ |= 0xFFFFF800; // Sign extend 11-bit
    drawOffsetY_ = (cmd >> 11) & 0x7FF;
    if (drawOffsetY_ & 0x400)
      drawOffsetY_ |= 0xFFFFF800; // Sign extend 11-bit
    ps1::metrics::trace(
        "gpu.draw_offset",
        {{"x", drawOffsetX_}, {"y", drawOffsetY_}, {"word", cmd}});
    break;
  case 0xE6: // Mask Bit
    // GP0(E6): bit 0 forces bit15 on every destination pixel; bit 1 rejects
    // writes where the destination already has bit15 set. GPUSTAT exposes
    // the same state in bits 11 and 12.
    setMaskWhileDrawing_ = (cmd & 1u) != 0;
    checkMaskBeforeDraw_ = (cmd & 2u) != 0;
    gpuStat_ = (gpuStat_ & ~((1u << 11) | (1u << 12))) |
               (setMaskWhileDrawing_ ? (1u << 11) : 0u) |
               (checkMaskBeforeDraw_ ? (1u << 12) : 0u);
    ps1::metrics::trace("gpu.mask_mode",
                        {{"set", setMaskWhileDrawing_ ? 1 : 0},
                         {"check", checkMaskBeforeDraw_ ? 1 : 0},
                         {"word", cmd}});
    break;

  default:
    fmt::print(stderr, "[GPU] Unknown GP0 opcode: 0x{:02X} (cmd=0x{:08X})\n",
               opcode, cmd);
    break;
  }

  // Unsupported visual commands still advance the native tile timeline, so
  // a later supported textured polygon cannot reuse a stale Vulkan shadow.
  // The whole-VRAM mark is conservative and only used while those command
  // families intentionally force whole-frame native fallback.
  if (replayRecorder_.enabled() && activeReplayPacketUnsupported_ &&
      (opcode == 0x02 || (opcode >= 0x40 && opcode <= 0xBF))) {
    replayRecorder_.markNativeWrite(
        replay::NativeRect{0, 0, VRAM_WIDTH, VRAM_HEIGHT});
  }
  activeReplayPacketSequence_ = replay::UNASSIGNED_PACKET_SEQUENCE;
  activeReplayPacketUnsupported_ = false;
}

void GPU::executeTextureWindow() {
  uint32_t cmd = commandQueue_.front();
  texWindowMaskX_ = (cmd & 0x1F) * 8;
  texWindowMaskY_ = ((cmd >> 5) & 0x1F) * 8;
  texWindowOffsetX_ = ((cmd >> 10) & 0x1F) * 8;
  texWindowOffsetY_ = ((cmd >> 15) & 0x1F) * 8;
}

void GPU::latchTexturePage(uint16_t tpage) {
  // A polygon's embedded tpage replaces draw-mode bits 0..8 and 11. It updates
  // the same state consumed by following sprites (which carry no tpage word)
  // and by untextured semi-transparent draws (which use the latched ABR mode).
  currentTexpage_ = tpage & 0x1FFu;
  blendMode_ = static_cast<uint8_t>((tpage >> 5) & 3u);
  textureDisable_ = allowTextureDisable_ && (tpage & (1u << 11u)) != 0;
  if (!allowTextureDisable_ && (tpage & (1u << 11u)) != 0)
    ps1::metrics::count("gpu.texture_disable.ignored_polygon_tpage");
  gpuStat_ = (gpuStat_ & ~(0x1FFu | (1u << 15u))) | currentTexpage_ |
             (textureDisable_ ? (1u << 15u) : 0u);
}

static PGXPPrimitive polygonPrimitive(uint8_t opcode) {
  switch (opcode & 0xfcu) {
  case 0x20:
    return PGXPPrimitive::FlatTriangle;
  case 0x24:
    return PGXPPrimitive::TexturedTriangle;
  case 0x28:
    return PGXPPrimitive::FlatQuad;
  case 0x2c:
    return PGXPPrimitive::TexturedQuad;
  case 0x30:
    return PGXPPrimitive::GouraudTriangle;
  case 0x34:
    return PGXPPrimitive::GouraudTexturedTriangle;
  case 0x38:
    return PGXPPrimitive::GouraudQuad;
  case 0x3c:
    return PGXPPrimitive::GouraudTexturedQuad;
  default:
    return PGXPPrimitive::FlatTriangle;
  }
}

static int32_t decodeSigned11(uint32_t value) {
  value &= 0x7FFu;
  return static_cast<int32_t>(value & 0x400u ? value | 0xFFFFF800u : value);
}

static bool withinPolygonDrawingLimits(const Vertex &v0, const Vertex &v1,
                                       const Vertex &v2) {
  const int32_t minX = std::min({v0.x, v1.x, v2.x});
  const int32_t maxX = std::max({v0.x, v1.x, v2.x});
  const int32_t minY = std::min({v0.y, v1.y, v2.y});
  const int32_t maxY = std::max({v0.y, v1.y, v2.y});
  return maxX - minX < 1024 && maxY - minY < 512;
}

PGXPSemanticDecision
GPU::classifyPolygonPacket(const std::array<std::size_t, 4> &wordIndices,
                           std::size_t vertexCount) const {
  if (commandQueue_.empty() || vertexCount == 0 || vertexCount > 4)
    return PGXPSemanticDecision::projected3D();

  PGXPPolygonSemanticInput input;
  input.opcode = static_cast<uint8_t>(commandQueue_.front() >> 24);
  input.vertexCount = static_cast<uint8_t>(vertexCount);
  input.drawOffsetX = drawOffsetX_;
  input.drawOffsetY = drawOffsetY_;
  input.drawX0 = drawAreaX1_;
  input.drawY0 = drawAreaY1_;
  input.drawX1 = drawAreaX2_;
  input.drawY1 = drawAreaY2_;
  input.programmedDisplayGeometry = displayModeSet_ && displayAreaSet_ &&
                                    horizontalDisplayRangeSet_ &&
                                    verticalDisplayRangeSet_;

  DisplayState display;
  display.xStart = displayVRAMXStart_;
  display.yStart = displayVRAMYStart_;
  display.displayX1 = displayX1_;
  display.displayX2 = displayX2_;
  display.displayY1 = displayY1_;
  display.displayY2 = displayY2_;
  display.gpuStat = gpuStat_;
  display.modeSet = displayModeSet_;
  display.areaSet = displayAreaSet_;
  display.horizontalRangeSet = horizontalDisplayRangeSet_;
  display.verticalRangeSet = verticalDisplayRangeSet_;
  const DisplayAperture aperture =
      deriveDisplayAperture(display, DisplayAreaPolicy::Programmed);
  input.apertureWidth = aperture.width;
  input.apertureHeight = aperture.height;

  for (std::size_t i = 0; i < vertexCount; ++i) {
    const std::size_t wordIndex = wordIndices[i];
    if (wordIndex >= commandQueue_.size())
      return PGXPSemanticDecision::projected3D();
    input.packedVertices[i] = commandQueue_[wordIndex];
    if (wordIndex < commandPrecisionQueue_.size() &&
        commandPrecisionQueue_[wordIndex].has_value()) {
      input.precision[i] = *commandPrecisionQueue_[wordIndex];
    }
  }
  return classifyPGXPPolygonSemantic(input);
}

Vertex GPU::decodeVertex(std::size_t wordIndex, PGXPSemanticDecision semantic) {
  const uint32_t word = commandQueue_[wordIndex];
  const uint8_t opcode = static_cast<uint8_t>(commandQueue_.front() >> 24);
  const uint32_t sourceAddress = wordIndex < commandSourceQueue_.size()
                                     ? commandSourceQueue_[wordIndex]
                                     : UINT32_MAX;
  const PGXPValue precision =
      wordIndex < commandPrecisionQueue_.size() &&
              commandPrecisionQueue_[wordIndex].has_value()
          ? *commandPrecisionQueue_[wordIndex]
          : PGXPValue{};
  const GP0DmaWordProvenance dmaProvenance =
      wordIndex < commandDmaProvenanceQueue_.size()
          ? commandDmaProvenanceQueue_[wordIndex]
          : GP0DmaWordProvenance{};
  const PGXPResolvedVertex resolved = resolvePGXPVertex(precision, word);
  recordPGXPVertex(opcode, static_cast<uint8_t>(wordIndex), sourceAddress,
                   polygonPrimitive(opcode), semantic, precision, word);
  Vertex vertex;
  vertex.gp0WordSource = sourceAddress == UINT32_MAX
                             ? 0
                             : static_cast<uint64_t>(sourceAddress) + 1u;
  if (dmaProvenance.exact) {
    vertex.dmaListSource = static_cast<uint64_t>(dmaProvenance.listStart) + 1u;
    vertex.dmaNodeSource =
        static_cast<uint64_t>(dmaProvenance.nodeAddress) + 1u;
    vertex.dmaTraversalOrdinal = dmaProvenance.traversalOrdinal;
    vertex.dmaProvenance = true;
    if (dmaProvenance.orderingTableAnchorExact) {
      vertex.dmaOrderingTableAnchorSource =
          static_cast<uint64_t>(dmaProvenance.orderingTableAnchorAddress) + 1u;
      vertex.dmaOrderingTableAnchorProvenance = true;
    }
  }
  vertex.x = decodeSigned11(word) + drawOffsetX_;
  vertex.y = decodeSigned11(word >> 16) + drawOffsetY_;
  vertex.preciseX = static_cast<double>(vertex.x);
  vertex.preciseY = static_cast<double>(vertex.y);
  vertex.authenticPreciseX = isAuthenticPGXPCoordinateSource(resolved.xSource);
  vertex.authenticPreciseY = isAuthenticPGXPCoordinateSource(resolved.ySource);
  vertex.reconciledX =
      resolved.xSource == PGXPCoordinateSource::NativeNonFinite ||
      resolved.xSource == PGXPCoordinateSource::NativeDelta ||
      resolved.xSource == PGXPCoordinateSource::NativeGuardBand;
  vertex.reconciledY =
      resolved.ySource == PGXPCoordinateSource::NativeNonFinite ||
      resolved.ySource == PGXPCoordinateSource::NativeDelta ||
      resolved.ySource == PGXPCoordinateSource::NativeGuardBand;

  if (resolved.hasSignedProjection) {
    vertex.depth = resolved.z;
    vertex.nearDepth = resolved.nearDepth;
    vertex.projectiveId = resolved.projectionId;
    vertex.projectiveContext = resolved.projectionContext;
    vertex.projectiveGameProfileCallsite = resolved.gameProfileCallsite;
    vertex.projectiveTransformValueFingerprint =
        resolved.projectionTransformValueFingerprint;
    vertex.projectiveTransformLineage = resolved.projectionTransformLineage;
    vertex.projectiveModelVertexSource = resolved.modelVertexSource;
    vertex.projectiveModelValueFlowSourceFingerprint =
        resolved.modelValueFlowSourceFingerprint;
    vertex.projectiveModelValueFlowProgramFingerprint =
        resolved.modelValueFlowProgramFingerprint;
    vertex.projectiveProvenance = resolved.intrinsicProvenance;
    vertex.depthValid = std::isfinite(resolved.z);
    // Keep the donor-compatible divider-saturated position beside the raw
    // homogeneous numerator. The selected near-plane policy decides which one
    // supplies geometry; neither representation needs to be reconstructed.
    vertex.preciseX = static_cast<double>(resolved.x) + drawOffsetX_;
    vertex.preciseY = static_cast<double>(resolved.y) + drawOffsetY_;
    vertex.rawProjectiveX =
        resolved.qx + static_cast<double>(drawOffsetX_) * vertex.depth;
    vertex.rawProjectiveY =
        resolved.qy + static_cast<double>(drawOffsetY_) * vertex.depth;
    vertex.projectiveX = vertex.rawProjectiveX;
    vertex.projectiveY = vertex.rawProjectiveY;
    vertex.homogeneousProjection = true;
    // Component reconciliation remains valid in front of the clipping plane.
    // Behind it the packed SXY value is divider-saturated and must never
    // replace the homogeneous numerator used to find the intersection.
    if (vertex.depth >= vertex.nearDepth &&
        !isAuthenticPGXPCoordinateSource(resolved.xSource))
      vertex.projectiveX =
          (static_cast<double>(resolved.x) + drawOffsetX_) * vertex.depth;
    if (vertex.depth >= vertex.nearDepth &&
        !isAuthenticPGXPCoordinateSource(resolved.ySource))
      vertex.projectiveY =
          (static_cast<double>(resolved.y) + drawOffsetY_) * vertex.depth;
    if (vertex.depth >= vertex.nearDepth && vertex.depth > 0.0f) {
      vertex.preciseX = vertex.projectiveX / vertex.depth;
      vertex.preciseY = vertex.projectiveY / vertex.depth;
    }
    vertex.precise = true;
    pgxpVertexHits_.fetch_add(1, std::memory_order_relaxed);
  } else if (resolved.hasDepth) {
    vertex.preciseX = static_cast<double>(resolved.x) + drawOffsetX_;
    vertex.preciseY = static_cast<double>(resolved.y) + drawOffsetY_;
    vertex.depth = resolved.z;
    vertex.projectiveX = vertex.preciseX * vertex.depth;
    vertex.projectiveY = vertex.preciseY * vertex.depth;
    vertex.rawProjectiveX = vertex.projectiveX;
    vertex.rawProjectiveY = vertex.projectiveY;
    vertex.nearDepth = 0.0f;
    vertex.precise = true;
    vertex.depthValid = true;
    pgxpVertexHits_.fetch_add(1, std::memory_order_relaxed);
  } else if (semantic.requiresXYZ() && resolved.hasScreenPosition()) {
    // Donor packet fallback: precise XY remains useful even when its W is
    // absent or invalid. Seed a finite projective representation so the
    // complete packet can stay on the modern path; prepareModernDepthPacket()
    // atomically replaces every vertex W with normalized 1.0.
    vertex.preciseX = static_cast<double>(resolved.x) + drawOffsetX_;
    vertex.preciseY = static_cast<double>(resolved.y) + drawOffsetY_;
    vertex.depth = 32768.0;
    vertex.projectiveX = vertex.preciseX * vertex.depth;
    vertex.projectiveY = vertex.preciseY * vertex.depth;
    vertex.rawProjectiveX = vertex.projectiveX;
    vertex.rawProjectiveY = vertex.projectiveY;
    vertex.nearDepth = 1.0;
    vertex.precise = true;
    vertex.depthValid = false;
    // `vertex_hit` remains a genuine XYZ/W coverage metric. Usable XY-only
    // provenance is visible through the precision-class counters, while this
    // vertex correctly remains an XYZ miss.
    pgxpVertexMisses_.fetch_add(1, std::memory_order_relaxed);
  } else if (semantic.requiresXYZ()) {
    // No usable sidecar at all is still a hardware-rendered packet in both
    // donors. GetPreciseVertex supplies native float XY and reports invalid W;
    // seed that same representation so 4x coverage remains vector-rasterized
    // instead of degrading to a block-expanded 1x compatibility mirror. The
    // precision miss counter still records that no PGXP XYZ was recovered.
    vertex.depth = 32768.0;
    vertex.projectiveX = vertex.preciseX * vertex.depth;
    vertex.projectiveY = vertex.preciseY * vertex.depth;
    vertex.rawProjectiveX = vertex.projectiveX;
    vertex.rawProjectiveY = vertex.projectiveY;
    vertex.nearDepth = 1.0;
    vertex.precise = true;
    vertex.depthValid = false;
    pgxpVertexMisses_.fetch_add(1, std::memory_order_relaxed);
  }
  return vertex;
}

void GPU::executeMonochromePoly3() {
  uint32_t c = commandQueue_[0] & 0xFFFFFF;
  Color24 color{static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8),
                static_cast<uint8_t>(c >> 16)};

  const PGXPSemanticDecision semantic = classifyPolygonPacket({1, 2, 3, 0}, 3);
  Vertex v[3];
  for (int i = 0; i < 3; i++)
    v[i] = decodeVertex(1 + i, semantic);
  uint32_t opcode = commandQueue_[0] >> 24;
  bool isBlend = (opcode & 2) != 0;

  const uint8_t validMask = vertexPrecisionMask(v, 3);
  const bool precise =
      semantic.requiresXYZ() && shouldUseEnhancedRaster(v, 3, validMask);
  const PolygonRoute route = precise ? PolygonRoute::Precise3D
                             : semantic.requiresXYZ()
                                 ? PolygonRoute::Compatibility3D
                                 : PolygonRoute::Screen2D;
  if (!precise && validMask == 0x7u && enhancedRasterEnabled_)
    ps1::metrics::count("gpu.modern_setup_fallback");
  notePrimitivePath(route, semantic, validMask, 3, 1);
  const bool replayCaptured =
      capturePolygonPacket(v, nullptr, &color, 3, 0, currentTexpage_, semantic,
                           validMask, false, false, false, isBlend);
  if (precise) {
    const ModernDepthAction depthAction =
        prepareModernDepthPacket(v, 3, isBlend);
    rasterizeEnhancedSurfaceTriangle(v[0], v[1], v[2], {}, {}, {}, color, color,
                                     color, 0, 0, false, false, false, isBlend,
                                     {}, depthAction);
  }
  if (shouldRasterizeNativePolygon(semantic, replayCaptured)) {
    rasterizeTriangle(v[0], v[1], v[2], color, isBlend, precise);
    markCapturedPolygonNativeWrite(v, 3);
  }
}

void GPU::executeMonochromePoly4() {
  uint32_t c = commandQueue_[0] & 0xFFFFFF;
  Color24 color{static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8),
                static_cast<uint8_t>(c >> 16)};

  const PGXPSemanticDecision semantic = classifyPolygonPacket({1, 2, 3, 4}, 4);
  Vertex v[4];
  for (int i = 0; i < 4; i++)
    v[i] = decodeVertex(1 + i, semantic);

  uint32_t opcode = commandQueue_[0] >> 24;
  bool isBlend = (opcode & 2) != 0;

  const uint8_t validMask = vertexPrecisionMask(v, 4);
  const bool precise =
      semantic.requiresXYZ() && shouldUseEnhancedRaster(v, 4, validMask);
  const PolygonRoute route = precise ? PolygonRoute::Precise3D
                             : semantic.requiresXYZ()
                                 ? PolygonRoute::Compatibility3D
                                 : PolygonRoute::Screen2D;
  if (!precise && validMask == 0xFu && enhancedRasterEnabled_)
    ps1::metrics::count("gpu.modern_setup_fallback");
  notePrimitivePath(route, semantic, validMask, 4, 2);
  const bool replayCaptured =
      capturePolygonPacket(v, nullptr, &color, 4, 0, currentTexpage_, semantic,
                           validMask, false, false, false, isBlend);
  const ModernDepthAction depthAction =
      precise ? prepareModernDepthPacket(v, 4, isBlend)
              : ModernDepthAction::Preserve;

  // Both modern halves sample the same pre-packet native VRAM. Only after the
  // companion replay finishes do the two PS1 painter triangles update the
  // guest-authoritative surface.
  if (precise) {
    rasterizeEnhancedSurfaceTriangle(v[0], v[1], v[2], {}, {}, {}, color, color,
                                     color, 0, 0, false, false, false, isBlend,
                                     {}, depthAction);
    rasterizeEnhancedSurfaceTriangle(v[1], v[2], v[3], {}, {}, {}, color, color,
                                     color, 0, 0, false, false, false, isBlend,
                                     {}, depthAction);
  }
  if (shouldRasterizeNativePolygon(semantic, replayCaptured)) {
    rasterizeTriangle(v[0], v[1], v[2], color, isBlend, precise);
    rasterizeTriangle(v[1], v[2], v[3], color, isBlend, precise);
    markCapturedPolygonNativeWrite(v, 4);
  }
}

void GPU::executeTexturedPoly3() {
  uint32_t c = commandQueue_[0] & 0xFFFFFF;
  Color24 color{static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8),
                static_cast<uint8_t>(c >> 16)};

  const PGXPSemanticDecision semantic = classifyPolygonPacket({1, 3, 5, 0}, 3);
  Vertex v[3];
  TexCoord t[3];
  uint16_t clut, tpage;

  v[0] = decodeVertex(1, semantic);
  t[0].u = commandQueue_[2] & 0xFF;
  t[0].v = (commandQueue_[2] >> 8) & 0xFF;
  clut = (commandQueue_[2] >> 16) & 0xFFFF;

  v[1] = decodeVertex(3, semantic);
  t[1].u = commandQueue_[4] & 0xFF;
  t[1].v = (commandQueue_[4] >> 8) & 0xFF;
  tpage = (commandQueue_[4] >> 16) & 0xFFFF;
  latchTexturePage(tpage);

  v[2] = decodeVertex(5, semantic);
  t[2].u = commandQueue_[6] & 0xFF;
  t[2].v = (commandQueue_[6] >> 8) & 0xFF;

  uint32_t opcode = commandQueue_[0] >> 24;
  bool isRaw = (opcode & 1) != 0;
  bool isBlend = (opcode & 2) != 0;
  const bool effectiveTextured = !textureDisable_;

  const uint8_t validMask = vertexPrecisionMask(v, 3);
  const bool precise =
      semantic.requiresXYZ() && shouldUseEnhancedRaster(v, 3, validMask);
  const PolygonRoute route = precise ? PolygonRoute::Precise3D
                             : semantic.requiresXYZ()
                                 ? PolygonRoute::Compatibility3D
                                 : PolygonRoute::Screen2D;
  if (!precise && validMask == 0x7u && enhancedRasterEnabled_)
    ps1::metrics::count("gpu.modern_setup_fallback");
  notePrimitivePath(route, semantic, validMask, 3, 1);
  const bool replayCaptured =
      capturePolygonPacket(v, t, &color, 3, clut, tpage, semantic, validMask,
                           true, false, isRaw, isBlend);
  if (precise) {
    const ModernDepthAction depthAction =
        prepareModernDepthPacket(v, 3, isBlend);
    const ModernUVDomain uvDomain =
        effectiveTextured ? prepareModernUVDomainForPacket(
                                v, t, 3, false, enhancedSurface_.scale())
                          : ModernUVDomain{};
    rasterizeEnhancedSurfaceTriangle(
        v[0], v[1], v[2], t[0], t[1], t[2], color, color, color, clut, tpage,
        effectiveTextured, false, effectiveTextured && isRaw, isBlend, uvDomain,
        depthAction);
  }
  if (shouldRasterizeNativePolygon(semantic, replayCaptured)) {
    if (effectiveTextured) {
      rasterizeTexturedTriangle(v[0], v[1], v[2], t[0], t[1], t[2], color,
                                clut, tpage, isRaw, isBlend, precise);
    } else {
      rasterizeTriangle(v[0], v[1], v[2], color, isBlend, precise);
    }
    markCapturedPolygonNativeWrite(v, 3);
  }
}

void GPU::executeTexturedPoly4() {
  uint32_t c = commandQueue_[0] & 0xFFFFFF;
  Color24 color{static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8),
                static_cast<uint8_t>(c >> 16)};

  const PGXPSemanticDecision semantic = classifyPolygonPacket({1, 3, 5, 7}, 4);
  Vertex v[4];
  TexCoord t[4];
  uint16_t clut, tpage;

  v[0] = decodeVertex(1, semantic);
  t[0].u = commandQueue_[2] & 0xFF;
  t[0].v = (commandQueue_[2] >> 8) & 0xFF;
  clut = (commandQueue_[2] >> 16) & 0xFFFF;

  v[1] = decodeVertex(3, semantic);
  t[1].u = commandQueue_[4] & 0xFF;
  t[1].v = (commandQueue_[4] >> 8) & 0xFF;
  tpage = (commandQueue_[4] >> 16) & 0xFFFF;
  latchTexturePage(tpage);

  v[2] = decodeVertex(5, semantic);
  t[2].u = commandQueue_[6] & 0xFF;
  t[2].v = (commandQueue_[6] >> 8) & 0xFF;

  v[3] = decodeVertex(7, semantic);
  t[3].u = commandQueue_[8] & 0xFF;
  t[3].v = (commandQueue_[8] >> 8) & 0xFF;

  uint32_t opcode = commandQueue_[0] >> 24;
  bool isRaw = (opcode & 1) != 0;
  bool isBlend = (opcode & 2) != 0;
  const bool effectiveTextured = !textureDisable_;

  const uint8_t validMask = vertexPrecisionMask(v, 4);
  const bool precise =
      semantic.requiresXYZ() && shouldUseEnhancedRaster(v, 4, validMask);
  const PolygonRoute route = precise ? PolygonRoute::Precise3D
                             : semantic.requiresXYZ()
                                 ? PolygonRoute::Compatibility3D
                                 : PolygonRoute::Screen2D;
  if (!precise && validMask == 0xFu && enhancedRasterEnabled_)
    ps1::metrics::count("gpu.modern_setup_fallback");
  notePrimitivePath(route, semantic, validMask, 4, 2);
  const bool replayCaptured =
      capturePolygonPacket(v, t, &color, 4, clut, tpage, semantic, validMask,
                           true, false, isRaw, isBlend);
  const ModernDepthAction depthAction =
      precise ? prepareModernDepthPacket(v, 4, isBlend)
              : ModernDepthAction::Preserve;
  const ModernUVDomain uvDomain =
      precise && effectiveTextured
          ? prepareModernUVDomainForPacket(v, t, 4, true,
                                           enhancedSurface_.scale())
          : ModernUVDomain{};
  if (precise) {
    rasterizeEnhancedSurfaceTriangle(
        v[0], v[1], v[2], t[0], t[1], t[2], color, color, color, clut, tpage,
        effectiveTextured, false, effectiveTextured && isRaw, isBlend, uvDomain,
        depthAction);
    rasterizeEnhancedSurfaceTriangle(
        v[1], v[2], v[3], t[1], t[2], t[3], color, color, color, clut, tpage,
        effectiveTextured, false, effectiveTextured && isRaw, isBlend, uvDomain,
        depthAction);
  }
  if (shouldRasterizeNativePolygon(semantic, replayCaptured)) {
    if (effectiveTextured) {
      rasterizeTexturedTriangle(v[0], v[1], v[2], t[0], t[1], t[2], color,
                                clut, tpage, isRaw, isBlend, precise);
      rasterizeTexturedTriangle(v[1], v[2], v[3], t[1], t[2], t[3], color,
                                clut, tpage, isRaw, isBlend, precise);
    } else {
      rasterizeTriangle(v[0], v[1], v[2], color, isBlend, precise);
      rasterizeTriangle(v[1], v[2], v[3], color, isBlend, precise);
    }
    markCapturedPolygonNativeWrite(v, 4);
  }
}

static int edgeFunction(const Vertex &a, const Vertex &b, const Vertex &c) {
  return (c.x - a.x) * (b.y - a.y) - (c.y - a.y) * (b.x - a.x);
}

// PS1 polygons follow a top-left fill convention: a pixel exactly on a shared
// edge belongs to one of the two triangles, never both. With screen-space Y
// growing downwards and edgeFunction() positive on the normalized winding, an
// edge is inclusive when it points down, or left when horizontal.
static bool isTopLeftEdge(const Vertex &a, const Vertex &b) {
  const int dy = b.y - a.y;
  const int dx = b.x - a.x;
  return dy > 0 || (dy == 0 && dx < 0);
}

static bool edgeCovers(int value, bool inclusive) {
  return value > 0 || (value == 0 && inclusive);
}

// Exact affine interpolation without a hardware division at every covered
// pixel. For a positive triangle area, barycentric attribute numerators are
// affine in screen X/Y. Store their Euclidean quotient and remainder, then
// walk both using additions. At covered samples every weighted attribute is
// non-negative, so this is bit-identical to the old C++ integer division.
// advanceXBy() performs the same quotient/remainder carries in one operation
// when a scanline skips the empty prefix of a triangle's bounding box.
class AffineAttributeWalker {
public:
  AffineAttributeWalker(int64_t numerator, int64_t stepX, int64_t stepY,
                        int32_t divisor) noexcept
      : divisor_(divisor), value_(divide(numerator, divisor)),
        stepX_(divide(stepX, divisor)), stepY_(divide(stepY, divisor)) {}

  int32_t value() const noexcept {
    return static_cast<int32_t>(value_.quotient);
  }

  void advanceX() noexcept { advance(stepX_); }
  void advanceXBy(int32_t count) noexcept {
    if (count <= 0)
      return;
    value_.quotient += stepX_.quotient * count;
    const int64_t remainder = value_.remainder + stepX_.remainder * count;
    value_.quotient += remainder / divisor_;
    value_.remainder = remainder % divisor_;
  }
  void advanceY() noexcept { advance(stepY_); }

private:
  struct QuotientRemainder {
    int64_t quotient = 0;
    int64_t remainder = 0;
  };

  static QuotientRemainder divide(int64_t numerator, int64_t divisor) noexcept {
    QuotientRemainder result{numerator / divisor, numerator % divisor};
    if (result.remainder < 0) {
      result.remainder += divisor;
      --result.quotient;
    }
    return result;
  }

  void advance(const QuotientRemainder &step) noexcept {
    value_.quotient += step.quotient;
    value_.remainder += step.remainder;
    if (value_.remainder >= divisor_) {
      value_.remainder -= divisor_;
      ++value_.quotient;
    }
  }

  int64_t divisor_ = 1;
  QuotientRemainder value_{};
  QuotientRemainder stepX_{};
  QuotientRemainder stepY_{};
};

int64_t affineAttributeNumerator(int w0, int w1, int w2, int a0, int a1,
                                 int a2) noexcept {
  return static_cast<int64_t>(w0) * a0 + static_cast<int64_t>(w1) * a1 +
         static_cast<int64_t>(w2) * a2;
}

static bool
nativeTriangleHasGeometricCoverage(const std::array<int32_t, 6> &coordinates,
                                   int drawX0, int drawY0, int drawX1,
                                   int drawY1) {
  Vertex v0{coordinates[0], coordinates[1]};
  Vertex v1{coordinates[2], coordinates[3]};
  Vertex v2{coordinates[4], coordinates[5]};
  int area = edgeFunction(v0, v1, v2);
  if (area == 0)
    return false;
  if (area < 0)
    std::swap(v1, v2);

  const bool edge0Inclusive = isTopLeftEdge(v1, v2);
  const bool edge1Inclusive = isTopLeftEdge(v2, v0);
  const bool edge2Inclusive = isTopLeftEdge(v0, v1);
  const int minX = std::max(drawX0, std::min({v0.x, v1.x, v2.x}));
  const int minY = std::max(drawY0, std::min({v0.y, v1.y, v2.y}));
  const int maxX = std::min(drawX1, std::max({v0.x, v1.x, v2.x}));
  const int maxY = std::min(drawY1, std::max({v0.y, v1.y, v2.y}));
  if (minX > maxX || minY > maxY)
    return false;

  Vertex sample{};
  for (sample.y = minY; sample.y <= maxY; ++sample.y) {
    for (sample.x = minX; sample.x <= maxX; ++sample.x) {
      if (edgeCovers(edgeFunction(v1, v2, sample), edge0Inclusive) &&
          edgeCovers(edgeFunction(v2, v0, sample), edge1Inclusive) &&
          edgeCovers(edgeFunction(v0, v1, sample), edge2Inclusive))
        return true;
    }
  }
  return false;
}

namespace {

struct EnhancedTriangleSetup {
  Vertex v0;
  Vertex v1;
  Vertex v2;
  double area = 0.0;
  bool edge0Inclusive = false;
  bool edge1Inclusive = false;
  bool edge2Inclusive = false;
  bool swapped = false;
  int minX = 0;
  int minY = 0;
  int maxX = -1;
  int maxY = -1;
  bool valid = false;
};

double enhancedEdge(const Vertex &a, const Vertex &b, double x, double y) {
  return (x - static_cast<double>(a.preciseX)) *
             (static_cast<double>(b.preciseY) - a.preciseY) -
         (y - static_cast<double>(a.preciseY)) *
             (static_cast<double>(b.preciseX) - a.preciseX);
}

bool enhancedTopLeft(const Vertex &a, const Vertex &b) {
  const double dy = static_cast<double>(b.preciseY) - a.preciseY;
  const double dx = static_cast<double>(b.preciseX) - a.preciseX;
  return dy > 0.0 || (dy == 0.0 && dx < 0.0);
}

bool enhancedEdgeCovers(double value, bool inclusive) {
  return value > 0.0 || (value == 0.0 && inclusive);
}

EnhancedTriangleSetup prepareEnhancedTriangle(Vertex v0, Vertex v1, Vertex v2,
                                              int drawX0, int drawY0,
                                              int drawX1, int drawY1) {
  EnhancedTriangleSetup setup;
  setup.v0 = v0;
  setup.v1 = v1;
  setup.v2 = v2;
  setup.area =
      enhancedEdge(setup.v0, setup.v1, setup.v2.preciseX, setup.v2.preciseY);
  if (setup.area == 0.0 || !std::isfinite(setup.area))
    return setup;
  if (setup.area < 0.0) {
    std::swap(setup.v1, setup.v2);
    setup.area = -setup.area;
    setup.swapped = true;
  }

  setup.edge0Inclusive = enhancedTopLeft(setup.v1, setup.v2);
  setup.edge1Inclusive = enhancedTopLeft(setup.v2, setup.v0);
  setup.edge2Inclusive = enhancedTopLeft(setup.v0, setup.v1);

  const double minX = std::min({static_cast<double>(setup.v0.preciseX),
                                static_cast<double>(setup.v1.preciseX),
                                static_cast<double>(setup.v2.preciseX)});
  const double minY = std::min({static_cast<double>(setup.v0.preciseY),
                                static_cast<double>(setup.v1.preciseY),
                                static_cast<double>(setup.v2.preciseY)});
  const double maxX = std::max({static_cast<double>(setup.v0.preciseX),
                                static_cast<double>(setup.v1.preciseX),
                                static_cast<double>(setup.v2.preciseX)});
  const double maxY = std::max({static_cast<double>(setup.v0.preciseY),
                                static_cast<double>(setup.v1.preciseY),
                                static_cast<double>(setup.v2.preciseY)});

  // Enhanced vertices retain sub-pixel coordinates. Sample at host pixel
  // centres; the extra ceil endpoint is harmless (edge tests reject it) and
  // avoids losing a partially-covered final pixel.
  setup.minX = std::max(drawX0, static_cast<int>(std::floor(minX)));
  setup.minY = std::max(drawY0, static_cast<int>(std::floor(minY)));
  setup.maxX = std::min(drawX1, static_cast<int>(std::ceil(maxX)));
  setup.maxY = std::min(drawY1, static_cast<int>(std::ceil(maxY)));
  setup.valid = setup.minX <= setup.maxX && setup.minY <= setup.maxY;
  return setup;
}

double incrementalEdgeTolerance(double rowStart, double xStep, int span) {
  // Each scanline starts from the original edge equation. This conservative
  // forward-error bound identifies samples for which repeated addition could
  // change the sign/equality decision; only those samples pay for an exact
  // edge recomputation below.
  const double extent = static_cast<double>(std::max(span, 0));
  return 8.0 * std::numeric_limits<double>::epsilon() * (extent + 2.0) *
         (std::abs(rowStart) + extent * std::abs(xStep) + 1.0);
}

} // namespace

uint8_t GPU::vertexPrecisionMask(const Vertex *vertices,
                                 std::size_t vertexCount) const {
  auto valid = [](const Vertex &v) {
    return v.precise && v.depthValid && std::isfinite(v.projectiveX) &&
           std::isfinite(v.projectiveY) && std::isfinite(v.depth) &&
           std::isfinite(v.nearDepth) && v.nearDepth >= 0.0f;
  };
  uint8_t mask = 0;
  for (std::size_t i = 0; i < vertexCount; ++i) {
    if (valid(vertices[i]))
      mask |= static_cast<uint8_t>(1u << i);
  }
  return mask;
}

bool GPU::shouldUseEnhancedRaster(const Vertex *vertices,
                                  std::size_t vertexCount, uint8_t validMask) {
  if (!enhancedRasterEnabled_ || !enhancedSurface_.enabled() ||
      vertexCount < 3 || vertexCount > 4)
    return false;

  // Rasterability and genuine W provenance are deliberately separate masks.
  // Every projected packet has a finite donor-selected XY (PGXP when usable,
  // packed native otherwise), even though its trace validity mask must still
  // report missing XYZ/W rather than claiming 100% precision.
  for (std::size_t i = 0; i < vertexCount; ++i) {
    const Vertex &vertex = vertices[i];
    if (!vertex.precise || !std::isfinite(vertex.preciseX) ||
        !std::isfinite(vertex.preciseY) || !std::isfinite(vertex.projectiveX) ||
        !std::isfinite(vertex.projectiveY) || !std::isfinite(vertex.depth) ||
        !std::isfinite(vertex.nearDepth) || vertex.nearDepth < 0.0)
      return false;
  }
  const uint8_t fullMask = static_cast<uint8_t>((1u << vertexCount) - 1u);
  // SwanStation makes invalid W packet-atomic: retained PGXP XY (or its
  // native fallback) stays hardware rendered, while every W becomes 1.
  // Context and near-plane agreement are irrelevant for this affine fallback.
  if (validMask != fullMask)
    return true;
  const uint64_t context = vertices[0].projectiveContext;
  const double nearDepth = vertices[0].nearDepth;
  for (std::size_t i = 1; i < vertexCount; ++i) {
    if (vertices[i].projectiveContext != context ||
        vertices[i].nearDepth != nearDepth) {
      ++frameModernContextMismatches_;
      return false;
    }
  }
  // Geometry outcome is not a provenance decision.  A complete projective
  // packet remains on the modern path even when clipping removes it or its
  // high-precision area is exactly zero; switching to packed geometry for
  // one frame creates the quantized slivers observed as flicker.
  return true;
}

ModernUVDomain GPU::prepareModernUVDomainForPacket(const Vertex *vertices,
                                                   const TexCoord *texCoords,
                                                   std::size_t vertexCount,
                                                   bool quad,
                                                   uint32_t resolutionScale) {
  if (vertices == nullptr || texCoords == nullptr || vertexCount < 3 ||
      vertexCount > 4)
    return {};

  std::array<ModernUVInput, 4> inputs{};
  bool validW = true;
  for (std::size_t i = 0; i < vertexCount; ++i)
    validW = validW && vertices[i].depthValid;
  for (std::size_t i = 0; i < vertexCount; ++i) {
    const double effectiveDepth =
        std::max(vertices[i].depth, vertices[i].nearDepth);
    inputs[i] = ModernUVInput{
        static_cast<float>(vertices[i].preciseX),
        static_cast<float>(vertices[i].preciseY),
        validW ? static_cast<float>(effectiveDepth / 32768.0) : 1.0f,
        texCoords[i].u, texCoords[i].v};
  }
  ModernUVDomain domain =
      prepareModernUVDomain(inputs.data(), vertexCount, resolutionScale, quad);
  if (domain.enabled)
    ++frameModernUVLimitedPrimitives_;
  if (domain.flippedU)
    ++frameModernUVFlippedUPrimitives_;
  if (domain.flippedV)
    ++frameModernUVFlippedVPrimitives_;
  return domain;
}

std::span<const Color16> GPU::replayNativeVram() const { return vram_; }

void GPU::markActiveReplayPacketUnsupported() {
  if (!replayRecorder_.enabled() || activeReplayPacketUnsupported_ ||
      commandQueue_.empty())
    return;
  const uint8_t opcode = static_cast<uint8_t>(commandQueue_.front() >> 24);
  if (replay::CrashReplayRecorder::isCrashReplayV1OpcodeSupported(opcode))
    replayRecorder_.markPacketUnsupported(opcode);
  activeReplayPacketUnsupported_ = true;
}

void GPU::consumeReplayDepthDrawAreaEpoch() {
  if (!replayRecorder_.enabled() || !replayDepthDrawAreaDirty_)
    return;

  replayDepthDrawAreaDirty_ = false;
  // Hybrid stores W with an ALWAYS comparison, so draw-area changes do not
  // require the donor heuristic's whole-attachment clear for visibility.
  // Explicit native mutations still invalidate exactly the accepted samples.
  replayDepthHasMean_ = false;
}

replay::DepthState
GPU::prepareReplayDepthState(const Vertex *vertices, std::size_t vertexCount,
                             replay::PacketRoute route, uint8_t validMask,
                             replay::PacketFallbackReason fallbackReason,
                             bool blend) {
  replay::DepthState state;
  state.epoch = replayDepthEpoch_;
  if (route == replay::PacketRoute::Screen2D) {
    state.invalidateOnAccepted = true;
    return state;
  }

  const replay::ReplayDepthMode mode = replayRecorder_.config().trueDepthMode;
  if (mode == replay::ReplayDepthMode::Off)
    return state;

  // A semi-transparent packet is painter work in the PS1 ordering table. Its
  // composite color has no single geometric W, so accepted samples invalidate
  // D32 only after texture-zero and E6 rejection. A later Hybrid opaque packet
  // follows OT order unconditionally and stores its own genuine W.
  if (mode == replay::ReplayDepthMode::Hybrid &&
      replayRecorder_.config().pgxp && blend) {
    state.invalidateOnAccepted = true;
    return state;
  }

  const uint8_t fullMask = static_cast<uint8_t>((1u << vertexCount) - 1u);
  if (validMask != fullMask) {
    // W normalization is stored on the vertices independently of D32 state.
    // Hybrid keeps the fallback painter visible without poisoning depth.
    // Strict preserves the old far-write experiment, which the replay
    // execution planner rejects as a non-canonical donor state.
    if (mode == replay::ReplayDepthMode::Strict) {
      state.writeEnabled = true;
      state.compare = replay::DepthCompare::Always;
    }
    return state;
  }

  // A full XYZ/W mask is necessary but not sufficient for donor depth. A
  // mismatched projective context/near plane is an explicit packet fallback;
  // Hybrid keeps it painter ordered just like every other fallback. Strict
  // intentionally continues into its diagnostic universal-depth behavior.
  if (mode == replay::ReplayDepthMode::Hybrid &&
      fallbackReason != replay::PacketFallbackReason::None) {
    return state;
  }

  if (mode == replay::ReplayDepthMode::Strict) {
    if (!blend) {
      state.testEnabled = true;
      state.writeEnabled = true;
      state.compare = replay::DepthCompare::LessOrEqual;
    }
    return state;
  }

  if (blend)
    return state;

  // Cross-packet W is not a valid substitute for the PS1 ordering table:
  // games deliberately submit OT-later primitives whose physical W is
  // farther. ALWAYS preserves that presentation contract while still
  // publishing the genuine interpolated W to D32.
  state.testEnabled = false;
  state.writeEnabled = true;
  state.compare = replay::DepthCompare::Always;
  return state;
}

bool GPU::capturePolygonPacket(const Vertex *vertices,
                               const TexCoord *texCoords, const Color24 *colors,
                               std::size_t vertexCount, uint16_t clut,
                               uint16_t tpage, PGXPSemanticDecision semantic,
                               uint8_t validMask, bool textured, bool gouraud,
                               bool rawTexture, bool blend) {
  if (!replayRecorder_.enabled() || vertices == nullptr || colors == nullptr ||
      vertexCount < 3 || vertexCount > 4 ||
      activeReplayPacketSequence_ == replay::UNASSIGNED_PACKET_SEQUENCE)
    return false;

  // Textured polygon executors latch their embedded tpage before capture, so
  // this is already the GP1(09)-gated, packet-effective state.
  const bool textureDisabled = textured && textureDisable_;
  const bool effectiveTextured = textured && !textureDisabled;
  if (effectiveTextured && texCoords == nullptr) {
    markActiveReplayPacketUnsupported();
    return false;
  }

  // CrashReplayV1 does not yet retain homogeneous XY numerators, so a packet
  // which actually needs near-plane clipping must select whole-frame native
  // fallback instead of silently replaying divider-saturated geometry.
  if (projectiveNearClipEnabled_ && semantic.requiresXYZ()) {
    for (std::size_t i = 0; i < vertexCount; ++i) {
      if (vertices[i].depthValid && vertices[i].depth < vertices[i].nearDepth) {
        markActiveReplayPacketUnsupported();
        return false;
      }
    }
  }

  replay::DrawPacket packet;
  packet.opcode = static_cast<uint8_t>(commandQueue_.front() >> 24);
  packet.packetSequence = activeReplayPacketSequence_;
  packet.submissionId = activeSubmissionId_;
  packet.route = semantic.requiresXYZ() ? replay::PacketRoute::Projected3D
                                        : replay::PacketRoute::Screen2D;
  switch (semantic.reason) {
  case PGXPSemanticReason::Intrinsic2DFamily:
    packet.semanticReason = replay::PacketSemanticReason::Intrinsic2DFamily;
    break;
  case PGXPSemanticReason::DisplayAnchoredComposite:
    packet.semanticReason =
        replay::PacketSemanticReason::DisplayAnchoredComposite;
    break;
  case PGXPSemanticReason::PolygonDefault:
  default:
    packet.semanticReason = replay::PacketSemanticReason::PolygonDefault;
    break;
  }

  const uint8_t fullMask = static_cast<uint8_t>((1u << vertexCount) - 1u);
  packet.validMask = replayRecorder_.config().pgxp ? validMask : 0;
  if (packet.route == replay::PacketRoute::Screen2D)
    packet.validMask = 0;

  if (packet.route == replay::PacketRoute::Projected3D &&
      packet.validMask != fullMask) {
    packet.fallbackReason =
        packet.validMask == 0
            ? replay::PacketFallbackReason::MissingPrecisionPacketAtomic
            : replay::PacketFallbackReason::InvalidWPacketAtomic;
  } else if (packet.route == replay::PacketRoute::Projected3D) {
    const uint64_t context = vertices[0].projectiveContext;
    const double nearDepth = vertices[0].nearDepth;
    for (std::size_t i = 1; i < vertexCount; ++i) {
      if (vertices[i].projectiveContext != context ||
          vertices[i].nearDepth != nearDepth) {
        packet.fallbackReason =
            replay::PacketFallbackReason::ProjectiveContextMismatch;
        break;
      }
    }
  }

  packet.vertexCount = static_cast<uint8_t>(vertexCount);
  packet.indexCount = vertexCount == 4 ? 6 : 3;
  packet.triangleIndices = vertexCount == 4
                               ? std::array<uint8_t, 6>{0, 1, 2, 1, 2, 3}
                               : std::array<uint8_t, 6>{0, 1, 2, 0, 0, 0};
  // Keep the encoded polygon-family opcode for topology/layout provenance,
  // but publish the effective raster state. Under the GP1(09)-gated E1 bit a
  // textured packet is a flat/Gouraud primitive and must not acquire a native
  // texture dependency.
  packet.textured = effectiveTextured;
  packet.gouraud = gouraud;
  packet.rawTexture = effectiveTextured && rawTexture;

  const bool packetAtomicW = packet.validMask != fullMask;
  const int32_t screen2DAnchorX = vertices[0].x;
  // Recenter the ordinary 4:3 HUD, but leave an exact page-sized untextured
  // fade/flash on the complete host canvas. Compressing this polygon produced
  // vertical sky seams at 64/448 and left the title visible beside a fade.
  // This changes replay presentation only: original packet/raster state stays
  // byte-for-byte native, including its mask and blend semantics.
  const bool fullPageScreenOverlay =
      games::crash3::widescreen16x9() &&
      packet.route == replay::PacketRoute::Screen2D && !effectiveTextured &&
      vertexCount == 4u &&
      games::crash3::isFullPageScreenQuad({{{vertices[0].x, vertices[0].y},
                                            {vertices[1].x, vertices[1].y},
                                            {vertices[2].x, vertices[2].y},
                                            {vertices[3].x, vertices[3].y}}});
  if (fullPageScreenOverlay)
    metrics::count("host.crash3.presentation.full_page_screen_overlay");
  std::array<ModernUVInput, 4> uvInputs{};
  for (std::size_t i = 0; i < vertexCount; ++i) {
    replay::ReplayVertex &output = packet.vertices[i];
    output.nativeX =
        packet.route == replay::PacketRoute::Screen2D && !fullPageScreenOverlay
            ? games::crash3::screen2DPresentationX(vertices[i].x, drawAreaX1_,
                                                   drawAreaX2_, screen2DAnchorX)
            : vertices[i].x;
    output.nativeY = vertices[i].y;
    // Screen2D semantics are defined by the packed GPU rectangle. Never let
    // an accidental/stale sidecar move UI or a display-anchored composite.
    const bool useVectorXY = packet.route == replay::PacketRoute::Projected3D &&
                             replayRecorder_.config().pgxp &&
                             vertices[i].precise &&
                             std::isfinite(vertices[i].preciseX) &&
                             std::isfinite(vertices[i].preciseY);
    output.x = useVectorXY ? vertices[i].preciseX : output.nativeX;
    output.y = useVectorXY ? vertices[i].preciseY : vertices[i].y;
    output.w = packetAtomicW
                   ? replay::CRASH_REPLAY_FALLBACK_W
                   : std::max(vertices[i].depth, vertices[i].nearDepth);
    if (!std::isfinite(output.w) || output.w <= 0.0) {
      markActiveReplayPacketUnsupported();
      return false;
    }
    output.color = {colors[gouraud ? i : 0].r, colors[gouraud ? i : 0].g,
                    colors[gouraud ? i : 0].b};
    const bool projected = packet.route == replay::PacketRoute::Projected3D;
    if (vertices[i].gp0WordSource != 0) {
      output.provenance.gp0WordSource = vertices[i].gp0WordSource;
      output.provenance.evidence |= replay::ReplayVertexGp0WordSourceExact;
    }
    if (vertices[i].dmaProvenance) {
      output.provenance.dmaListSource = vertices[i].dmaListSource;
      output.provenance.dmaNodeSource = vertices[i].dmaNodeSource;
      output.provenance.dmaTraversalOrdinal = vertices[i].dmaTraversalOrdinal;
      output.provenance.evidence |= replay::ReplayVertexDmaTraversalExact;
    }
    if (vertices[i].dmaOrderingTableAnchorProvenance) {
      output.provenance.dmaOrderingTableAnchorSource =
          vertices[i].dmaOrderingTableAnchorSource;
      output.provenance.evidence |=
          replay::ReplayVertexOrderingTableAnchorExact;
    }
    if (projected && vertices[i].homogeneousProjection &&
        vertices[i].projectiveId != 0) {
      output.provenance.projectionId = vertices[i].projectiveId;
      output.provenance.projectionContext = vertices[i].projectiveContext;
      output.provenance.gameProfileCallsite =
          vertices[i].projectiveGameProfileCallsite;
      if ((vertices[i].projectiveProvenance &
           PGXPValue::VALID_MODEL_VERTEX_SOURCE) == 0) {
        if (const auto tag = games::crash3::decodeProjectionTag(
                vertices[i].projectiveModelVertexSource)) {
          output.provenance.gameProfileTag = *tag;
        }
      }
      output.provenance.transformValueFingerprint =
          vertices[i].projectiveTransformValueFingerprint;
      output.provenance.evidence |= replay::ReplayVertexProjectionEventExact;
      if ((vertices[i].projectiveProvenance &
           PGXPValue::VALID_TRANSFORM_LINEAGE) != 0) {
        output.provenance.transformLineage =
            vertices[i].projectiveTransformLineage;
        output.provenance.evidence |= replay::ReplayVertexTransformLineageExact;
      }
      if ((vertices[i].projectiveProvenance &
           PGXPValue::VALID_MODEL_VERTEX_SOURCE) != 0) {
        output.provenance.modelVertexSource =
            vertices[i].projectiveModelVertexSource;
        output.provenance.evidence |= replay::ReplayVertexModelSourceExact;
      }
      if ((vertices[i].projectiveProvenance &
           PGXPValue::VALID_MODEL_VALUE_FLOW) != 0) {
        output.provenance.modelValueFlowSourceFingerprint =
            vertices[i].projectiveModelValueFlowSourceFingerprint;
        output.provenance.modelValueFlowProgramFingerprint =
            vertices[i].projectiveModelValueFlowProgramFingerprint;
        output.provenance.evidence |= replay::ReplayVertexModelValueFlowExact;
      }
    }
    output.preciseX = projected && replayRecorder_.config().pgxp &&
                      vertices[i].authenticPreciseX;
    output.preciseY = projected && replayRecorder_.config().pgxp &&
                      vertices[i].authenticPreciseY;
    output.preciseXY = output.preciseX && output.preciseY;
    output.rawDepthValid = projected && (packet.validMask & (1u << i)) != 0;
    output.rawDepth = output.rawDepthValid ? vertices[i].depth : 0.0;
    output.nearDepth = output.rawDepthValid ? vertices[i].nearDepth : 0.0;
    output.qx = vertices[i].rawProjectiveX;
    output.qy = vertices[i].rawProjectiveY;
    output.homogeneousQ = projected && vertices[i].homogeneousProjection &&
                          std::isfinite(output.qx) && std::isfinite(output.qy);
    output.reconciledX = projected && vertices[i].reconciledX;
    output.reconciledY = projected && vertices[i].reconciledY;
    output.inventedW = projected && packetAtomicW;
    output.nearSaturated =
        output.rawDepthValid && vertices[i].depth < vertices[i].nearDepth;
    output.preciseUV =
        effectiveTextured && output.preciseXY && packet.validMask == fullMask;
    if (effectiveTextured) {
      uvInputs[i] = ModernUVInput{static_cast<float>(output.x),
                                  static_cast<float>(output.y),
                                  static_cast<float>(output.w / 32768.0),
                                  texCoords[i].u, texCoords[i].v};
    }
  }

  ModernUVDomain uvDomain;
  if (effectiveTextured) {
    uvDomain = prepareModernUVDomain(uvInputs.data(), vertexCount,
                                     replayRecorder_.config().internalScale,
                                     vertexCount == 4);
    for (std::size_t i = 0; i < vertexCount; ++i) {
      packet.vertices[i].u =
          static_cast<double>(texCoords[i].u) + uvDomain.offsetU;
      packet.vertices[i].v =
          static_cast<double>(texCoords[i].v) + uvDomain.offsetV;
    }
  }

  if (effectiveTextured && (packet.opcode & 0xe0u) == 0x60u &&
      packet.route == replay::PacketRoute::Screen2D) {
    // SPRT endpoints are half-open and may cross the 8-bit UV boundary.
    // Keep them as floats instead of truncating the far edge through TexCoord.
    const double u = commandQueue_[2] & 255u;
    const double v = (commandQueue_[2] >> 8) & 255u;
    const double width = vertices[1].x - vertices[0].x;
    const double height = vertices[2].y - vertices[0].y;
    const double u0 = u + (textureRectFlipX_ ? 1 : 0);
    const double v0 = v + (textureRectFlipY_ ? 1 : 0);
    const double u1 = u0 + (textureRectFlipX_ ? -width : width);
    const double v1 = v0 + (textureRectFlipY_ ? -height : height);
    // Equivalent positive UV periods keep a flipped sprite crossing zero
    // correct with the polygon shader's truncate-toward-zero conversion.
    const double shiftU =
        -std::min(0.0, std::floor(std::min(u0, u1) / 256)) * 256;
    const double shiftV =
        -std::min(0.0, std::floor(std::min(v0, v1) / 256)) * 256;
    for (uint32_t i = 0; i < 4; ++i) {
      packet.vertices[i].u = ((i & 1) ? u1 : u0) + shiftU;
      packet.vertices[i].v = ((i & 2) ? v1 : v0) + shiftV;
    }
    uvDomain.minU = std::min(u0, u1) + shiftU;
    uvDomain.maxU = std::max(u0, u1) - 1 + shiftU;
    uvDomain.minV = std::min(v0, v1) + shiftV;
    uvDomain.maxV = std::max(v0, v1) - 1 + shiftV;
    uvDomain.offsetU = uvDomain.offsetV = 0;
    uvDomain.enabled = true;
  }

  packet.state.drawArea = {drawAreaX1_, drawAreaY1_, drawAreaX2_, drawAreaY2_};
  packet.state.drawOffset = {drawOffsetX_, drawOffsetY_};
  const uint16_t effectiveTpage = static_cast<uint16_t>(
      (tpage & 0x1FFu) | (ditherEnable_ ? (1u << 9) : 0u) |
      ((gpuStat_ & (1u << 10)) != 0 ? (1u << 10) : 0u) |
      (textureDisabled ? (1u << 11) : 0u));
  packet.state.texturePage.raw = effectiveTpage;
  packet.state.texturePage.baseX = (effectiveTpage & 0xFu) * 64u;
  packet.state.texturePage.baseY = ((effectiveTpage >> 4) & 1u) * 256u;
  switch ((effectiveTpage >> 7) & 3u) {
  case 0:
    packet.state.texturePage.depth = replay::TextureDepth::Indexed4;
    break;
  case 1:
    packet.state.texturePage.depth = replay::TextureDepth::Indexed8;
    break;
  case 2:
    packet.state.texturePage.depth = replay::TextureDepth::Direct15;
    break;
  default:
    packet.state.texturePage.depth = replay::TextureDepth::Reserved;
    break;
  }
  // GP0(E1) bits 12/13 affect rectangles/sprites only. Polygon UVs must not
  // inherit that addressing mode even though the mutable GPU register keeps
  // the bits for the next rectangle command.
  packet.state.texturePage.xFlip = false;
  packet.state.texturePage.yFlip = false;
  packet.state.texturePage.textureDisable = textureDisabled;
  packet.state.clut = {clut, static_cast<uint16_t>((clut & 0x3Fu) * 16u),
                       static_cast<uint16_t>((clut >> 6) & 0x1FFu)};
  packet.state.textureWindow = {
      static_cast<uint32_t>(
          (texWindowMaskX_ / 8u) | ((texWindowMaskY_ / 8u) << 5) |
          ((texWindowOffsetX_ / 8u) << 10) | ((texWindowOffsetY_ / 8u) << 15)),
      texWindowMaskX_, texWindowMaskY_, texWindowOffsetX_, texWindowOffsetY_};
  packet.state.textureUvDomain = {
      uvDomain.minU,    uvDomain.minV,     uvDomain.maxU,
      uvDomain.maxV,    uvDomain.offsetU,  uvDomain.offsetV,
      uvDomain.enabled, uvDomain.flippedU, uvDomain.flippedV};
  switch (blendMode_ & 3u) {
  case 0:
    packet.state.blend.mode =
        replay::BlendMode::HalfBackgroundPlusHalfForeground;
    break;
  case 1:
    packet.state.blend.mode = replay::BlendMode::BackgroundPlusForeground;
    break;
  case 2:
    packet.state.blend.mode = replay::BlendMode::BackgroundMinusForeground;
    break;
  case 3:
    packet.state.blend.mode =
        replay::BlendMode::BackgroundPlusQuarterForeground;
    break;
  }
  packet.state.blend.enabled = blend;
  packet.state.blend.texturedStpOnly = effectiveTextured && blend;
  packet.state.mask = {setMaskWhileDrawing_, checkMaskBeforeDraw_};
  packet.state.depth =
      prepareReplayDepthState(vertices, vertexCount, packet.route,
                              packet.validMask, packet.fallbackReason, blend);
  packet.state.dither = ditherEnable_;
  packet.state.drawToDisplayArea = (gpuStat_ & (1u << 10)) != 0;

  games::mgs::adaptMeshReplayPacket(packet);
  games::mgs::bindOriginalTexture(packet, replayNativeVram());
  games::mgs::bindWaterEffect(packet, commandSourceQueue_.empty()
                                          ? UINT32_MAX
                                          : commandSourceQueue_.front());
  replayRecorder_.recordDraw(std::move(packet), replayNativeVram());
  return true;
}

bool GPU::shouldRasterizeNativePolygon(PGXPSemanticDecision semantic,
                                       bool replayCaptured) const {
  if (!replayCaptured || !semantic.requiresXYZ() ||
      !replayRecorder_.config()
           .omitProjectedPresentationNativeRasterDiagnostic) {
    return true;
  }

  DisplayState display;
  display.xStart = displayVRAMXStart_;
  display.yStart = displayVRAMYStart_;
  display.displayX1 = displayX1_;
  display.displayX2 = displayX2_;
  display.displayY1 = displayY1_;
  display.displayY2 = displayY2_;
  display.gpuStat = gpuStat_;
  display.modeSet = displayModeSet_;
  display.areaSet = displayAreaSet_;
  display.horizontalRangeSet = horizontalDisplayRangeSet_;
  display.verticalRangeSet = verticalDisplayRangeSet_;
  const DisplayAperture aperture =
      deriveDisplayAperture(display, DisplayAreaPolicy::Programmed);
  const uint32_t drawWidth =
      drawAreaX2_ >= drawAreaX1_
          ? static_cast<uint32_t>(drawAreaX2_ - drawAreaX1_ + 1)
          : 0u;
  const uint32_t drawHeight =
      drawAreaY2_ >= drawAreaY1_
          ? static_cast<uint32_t>(drawAreaY2_ - drawAreaY1_ + 1)
          : 0u;
  const bool presentationSizedTarget =
      aperture.enabled && drawWidth >= aperture.width &&
      drawHeight >= aperture.height;
  ps1::metrics::count(
      presentationSizedTarget
          ? "gpu.native_raster_diagnostic.projected_presentation_omitted"
          : "gpu.native_raster_diagnostic.projected_offscreen_retained");
  return !presentationSizedTarget;
}

void GPU::markCapturedPolygonNativeWrite(const Vertex *vertices,
                                         std::size_t vertexCount) {
  if (!replayRecorder_.enabled() || vertices == nullptr || vertexCount == 0)
    return;
  int32_t minX = vertices[0].x;
  int32_t minY = vertices[0].y;
  int32_t maxX = vertices[0].x;
  int32_t maxY = vertices[0].y;
  for (std::size_t i = 1; i < vertexCount; ++i) {
    minX = std::min(minX, vertices[i].x);
    minY = std::min(minY, vertices[i].y);
    maxX = std::max(maxX, vertices[i].x);
    maxY = std::max(maxY, vertices[i].y);
  }
  minX = std::max({minX, drawAreaX1_, 0});
  minY = std::max({minY, drawAreaY1_, 0});
  maxX = std::min({maxX, drawAreaX2_, static_cast<int32_t>(VRAM_WIDTH) - 1});
  maxY = std::min({maxY, drawAreaY2_, static_cast<int32_t>(VRAM_HEIGHT) - 1});
  if (maxX < minX || maxY < minY)
    return;
  replayRecorder_.markNativeWrite({minX, minY,
                                   static_cast<uint32_t>(maxX - minX + 1),
                                   static_cast<uint32_t>(maxY - minY + 1)});
}

void GPU::consumeModernDepthDrawAreaEpoch() {
  if (!depthEpochDrawAreaDirty_)
    return;

  // E3/E4 is still consumed at the same command boundary for deterministic
  // telemetry, but Hybrid painter-write visibility needs no attachment clear.
  depthEpochDrawAreaDirty_ = false;
  depthEpochHasMean_ = false;
}

GPU::ModernDepthAction GPU::prepareModernDepthPacket(const Vertex *vertices,
                                                     std::size_t vertexCount,
                                                     bool blend) {
  if (vertices == nullptr || vertexCount < 3)
    return ModernDepthAction::Preserve;

  bool normalizeFar = false;
  for (std::size_t i = 0; i < vertexCount; ++i)
    normalizeFar = normalizeFar || !vertices[i].depthValid;
  if (normalizeFar)
    ++frameModernDepthInvalidPackets_;

  if (trueDepthMode_ == TrueDepthMode::Off)
    return normalizeFar ? ModernDepthAction::NormalizeFar
                        : ModernDepthAction::Preserve;

  // Strict mode intentionally preserves the former experiment: every precise
  // opaque packet tests W after the selected near-plane policy has made it
  // rasterizable. Unit tests use this mode to isolate raw Z-buffer behavior.
  if (trueDepthMode_ == TrueDepthMode::Strict) {
    if (normalizeFar)
      return ModernDepthAction::NormalizeFar;
    return blend ? ModernDepthAction::Preserve : ModernDepthAction::TestWrite;
  }

  if (blend) {
    // Painter color ignores the previous D32 owner. Accepted fragments write
    // the far sentinel so the next OT-later opaque draw can establish a fresh
    // owner; transparent texels and E6 rejects leave the old owner untouched.
    ++frameModernDepthBlendVisibilityBoundaryPackets_;
    return normalizeFar ? ModernDepthAction::NormalizeAndInvalidateAcceptedFar
                        : ModernDepthAction::InvalidateAcceptedFar;
  }

  if (normalizeFar)
    return ModernDepthAction::NormalizeFar;
  ++frameModernDepthEligiblePackets_;
  return ModernDepthAction::Store;
}

void GPU::notePrimitivePath(PolygonRoute route, PGXPSemanticDecision semantic,
                            uint8_t validMask, uint8_t vertexCount,
                            uint8_t triangleCount) {
  const bool precise = route == PolygonRoute::Precise3D;
  if (precise)
    framePreciseTriangles_ += triangleCount;
  else if (route == PolygonRoute::Compatibility3D)
    frameCompatibilityTriangles_ += triangleCount;
  else
    frameScreen2DTriangles_ += triangleCount;

  const uint8_t fullMask = static_cast<uint8_t>((1u << vertexCount) - 1u);
  const bool partial = route != PolygonRoute::Screen2D && validMask != fullMask;
  if (partial)
    ++framePartialPrecisionPackets_;
  if (activeSubmissionId_ != 0) {
    ++submissionStats_.packets;
    if (precise)
      ++submissionStats_.precisePackets;
    else if (route == PolygonRoute::Compatibility3D)
      ++submissionStats_.compatibilityPackets;
    else
      ++submissionStats_.screen2DPackets;
    if (partial)
      ++submissionStats_.partialPrecisionPackets;
  }

  if (commandSourceQueue_.empty())
    return;
  const uint32_t guestSource = commandSourceQueue_.front();
  if (guestSource == UINT32_MAX)
    return;

  const uint8_t opcode =
      commandQueue_.empty() ? 0
                            : static_cast<uint8_t>(commandQueue_.front() >> 24);

  uint32_t source = guestSource;
  if (source >= 0x80000000u)
    source &= 0x1fffffffu;
  if (source < 0x00800000u)
    source &= 0x1ffffcu;
  else
    source &= ~3u;

  const uint64_t submission = activeSubmissionId_ != 0
                                  ? activeSubmissionId_
                                  : ps1::metrics::traceFrame();
  const uint64_t historyKey = (static_cast<uint64_t>(opcode) << 32) | source;
  auto [it, inserted] = primitivePathHistory_.try_emplace(
      historyKey, PrimitivePathState{submission, route, validMask});
  if (!inserted && it->second.submission != submission &&
      (it->second.route != route || it->second.validMask != validMask)) {
    ++framePathToggles_;
    if (activeSubmissionId_ != 0)
      ++submissionStats_.pathToggles;
    ps1::metrics::trace(
        "gpu.primitive_path_toggle",
        {{"word_address", guestSource},
         {"opcode", opcode},
         {"previous_submission", static_cast<int64_t>(it->second.submission)},
         {"current_submission", static_cast<int64_t>(submission)},
         {"previous_precise",
          it->second.route == PolygonRoute::Precise3D ? 1 : 0},
         {"current_precise", precise ? 1 : 0},
         {"previous_route", static_cast<int64_t>(it->second.route)},
         {"current_route", static_cast<int64_t>(route)},
         {"previous_valid_mask", it->second.validMask},
         {"current_valid_mask", validMask}});
  }
  it->second = PrimitivePathState{submission, route, validMask};

  if (!precise) {
    ps1::metrics::trace(
        "gpu.polygon_route",
        {{"submission_id", static_cast<int64_t>(activeSubmissionId_)},
         {"word_address", guestSource},
         {"opcode", opcode},
         {"vertex_count", vertexCount},
         {"valid_mask", validMask},
         {"full_precision", validMask == fullMask ? 1 : 0},
         {"setup_fallback",
          route == PolygonRoute::Compatibility3D && validMask == fullMask ? 1
                                                                          : 0},
         {"semantic_domain", static_cast<int64_t>(semantic.domain)},
         {"semantic_reason", static_cast<int64_t>(semantic.reason)},
         {"route", static_cast<int64_t>(route)}});
  }
}

void GPU::noteBoundsReject() {
  ++frameBoundsRejects_;
  if (activeSubmissionId_ != 0)
    ++submissionStats_.boundsRejects;
  ps1::metrics::count("gpu.primitive_reject.bounds");
}

void GPU::traceModernCoverageDivergence(
    uint8_t outcome, const std::array<int32_t, 6> &nativeCoordinates,
    const Vertex &v0, const Vertex &v1, const Vertex &v2, bool postNearClip,
    int fanIndex, int outputTriangleCount) const {
  if (!ps1::metrics::modernRejectTraceEnabled())
    return;

  const uint32_t wordAddress =
      commandSourceQueue_.empty() ? UINT32_MAX : commandSourceQueue_.front();
  const uint8_t opcode =
      commandQueue_.empty() ? 0
                            : static_cast<uint8_t>(commandQueue_.front() >> 24);
  ps1::metrics::trace(
      "gpu.modern_coverage_divergence",
      {{"submission_id", static_cast<int64_t>(activeSubmissionId_)},
       {"word_address",
        wordAddress == UINT32_MAX ? -1 : static_cast<int64_t>(wordAddress)},
       {"opcode", opcode},
       {"outcome", outcome},
       {"post_near_clip", postNearClip ? 1 : 0},
       {"fan_index", fanIndex},
       {"output_triangle_count", outputTriangleCount},
       {"native_x0", nativeCoordinates[0]},
       {"native_y0", nativeCoordinates[1]},
       {"native_x1", nativeCoordinates[2]},
       {"native_y1", nativeCoordinates[3]},
       {"native_x2", nativeCoordinates[4]},
       {"native_y2", nativeCoordinates[5]},
       {"precise_x0_q16", traceQ16(v0.preciseX)},
       {"precise_y0_q16", traceQ16(v0.preciseY)},
       {"precise_x1_q16", traceQ16(v1.preciseX)},
       {"precise_y1_q16", traceQ16(v1.preciseY)},
       {"precise_x2_q16", traceQ16(v2.preciseX)},
       {"precise_y2_q16", traceQ16(v2.preciseY)},
       {"depth0_q16", traceQ16(v0.depth)},
       {"depth1_q16", traceQ16(v1.depth)},
       {"depth2_q16", traceQ16(v2.depth)},
       {"near_depth_q16", traceQ16(v0.nearDepth)},
       {"projective_context", static_cast<int64_t>(v0.projectiveContext)},
       {"draw_x0", drawAreaX1_},
       {"draw_y0", drawAreaY1_},
       {"draw_x1", drawAreaX2_},
       {"draw_y1", drawAreaY2_},
       {"draw_offset_x", drawOffsetX_},
       {"draw_offset_y", drawOffsetY_}});
}

Color16 GPU::applyDither(Color16 baseColor, int x, int y) {
  return packColor((baseColor.raw & 0x1F) << 3,
                   ((baseColor.raw >> 5) & 0x1F) << 3,
                   ((baseColor.raw >> 10) & 0x1F) << 3,
                   (baseColor.raw & 0x8000u) != 0, x, y, ditherEnable_);
}

Color16 GPU::applyBlend(Color16 fg, Color16 bg) {
  return applyBlendMode(fg, bg, blendMode_);
}

[[gnu::always_inline]] inline Color16
GPU::applyBlendMode(Color16 fg, Color16 bg, uint8_t mode) const {
  const int fr = fg.raw & 0x1F;
  const int fg5 = (fg.raw >> 5) & 0x1F;
  const int fb = (fg.raw >> 10) & 0x1F;
  const int br = bg.raw & 0x1F;
  const int bg5 = (bg.raw >> 5) & 0x1F;
  const int bb = (bg.raw >> 10) & 0x1F;
  auto channel = [mode](int foreground, int background) {
    switch (mode & 3u) {
    case 0:
      return (background + foreground) >> 1;
    case 1:
      return std::min(31, background + foreground);
    case 2:
      return std::max(0, background - foreground);
    default:
      return std::min(31, background + (foreground >> 2));
    }
  };

  return Color16{
      static_cast<uint16_t>(channel(fr, br) | (channel(fg5, bg5) << 5) |
                            (channel(fb, bb) << 10) | (fg.raw & 0x8000u))};
}

[[gnu::always_inline]] inline Color16
GPU::packColor(int r, int g, int b, bool mask, int x, int y,
               bool dither) const {
  static constexpr int8_t matrix[4][4] = {
      {-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
  const int offset = dither ? matrix[y & 3][x & 3] : 0;
  r = std::clamp(r + offset, 0, 255);
  g = std::clamp(g + offset, 0, 255);
  b = std::clamp(b + offset, 0, 255);
  return Color16{
      static_cast<uint16_t>(((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) |
                            (((b >> 3) & 0x1F) << 10) | (mask ? 0x8000u : 0u))};
}

[[gnu::always_inline]] inline Color16
GPU::modulateTexel(Color16 texel, Color24 color, int x, int y) const {
  // PS1 modulation uses 0x80 as identity and saturates before conversion to
  // the 15-bit framebuffer. Preserve STP: it controls per-texel blending.
  const int r = std::min(255, ((texel.raw & 0x1F) * color.r) >> 4);
  const int g = std::min(255, (((texel.raw >> 5) & 0x1F) * color.g) >> 4);
  const int b = std::min(255, (((texel.raw >> 10) & 0x1F) * color.b) >> 4);
  return packColor(r, g, b, (texel.raw & 0x8000u) != 0, x, y, ditherEnable_);
}

[[gnu::always_inline]] inline bool
GPU::writePixel(int x, int y, Color16 color, bool mirrorToEnhanced) {
  if (x < 0 || y < 0 || x >= static_cast<int>(VRAM_WIDTH) ||
      y >= static_cast<int>(VRAM_HEIGHT))
    return false;
  Color16 &destination =
      vram_[static_cast<uint32_t>(y) * VRAM_WIDTH + static_cast<uint32_t>(x)];
  if (checkMaskBeforeDraw_ && (destination.raw & 0x8000u))
    return false;
  if (setMaskWhileDrawing_)
    color.raw |= 0x8000u;
  destination = color;
  if (mirrorToEnhanced && enhancedSurface_.enabled())
    enhancedSurface_.mirrorNativePixel(static_cast<uint32_t>(x),
                                       static_cast<uint32_t>(y), color.raw);
  return true;
}

Color32 GPU::packModernColor(int r, int g, int b, bool mask, int x,
                             int y) const {
  if (trueColorEnabled_) {
    return Color32{static_cast<uint8_t>(std::clamp(r, 0, 255)),
                   static_cast<uint8_t>(std::clamp(g, 0, 255)),
                   static_cast<uint8_t>(std::clamp(b, 0, 255)), 255};
  }

  const int scale = static_cast<int>(enhancedSurface_.scale());
  const Color16 quantized =
      packColor(r, g, b, mask, x / scale, y / scale, ditherEnable_);
  return EnhancedSurface::expandNativeColor(quantized.raw);
}

Color32 GPU::modulateModernTexel(Color16 texel, Color24 color, int x,
                                 int y) const {
  const int r = std::min(255, ((texel.raw & 0x1F) * color.r) >> 4);
  const int g = std::min(255, (((texel.raw >> 5) & 0x1F) * color.g) >> 4);
  const int b = std::min(255, (((texel.raw >> 10) & 0x1F) * color.b) >> 4);
  return packModernColor(r, g, b, (texel.raw & 0x8000u) != 0, x, y);
}

Color32 GPU::applyModernBlendMode(Color32 foreground, Color32 background,
                                  uint8_t mode) const {
  if (!trueColorEnabled_) {
    const Color16 foreground16 =
        packColor(foreground.r, foreground.g, foreground.b, false, 0, 0, false);
    const Color16 background16 =
        packColor(background.r, background.g, background.b, false, 0, 0, false);
    return EnhancedSurface::expandNativeColor(
        applyBlendMode(foreground16, background16, mode).raw);
  }

  auto channel = [mode](int foregroundChannel, int backgroundChannel) {
    switch (mode & 3u) {
    case 0:
      return (backgroundChannel + foregroundChannel) >> 1;
    case 1:
      return std::min(255, backgroundChannel + foregroundChannel);
    case 2:
      return std::max(0, backgroundChannel - foregroundChannel);
    default:
      return std::min(255, backgroundChannel + (foregroundChannel >> 2));
    }
  };
  return Color32{static_cast<uint8_t>(channel(foreground.r, background.r)),
                 static_cast<uint8_t>(channel(foreground.g, background.g)),
                 static_cast<uint8_t>(channel(foreground.b, background.b)),
                 255};
}

void GPU::writeModernBlock(int x, int y, Color32 foreground, bool sourceMask,
                           bool blend, uint8_t mode) {
  if (!enhancedSurface_.enabled() || x < 0 || y < 0 ||
      x >= static_cast<int>(VRAM_WIDTH) || y >= static_cast<int>(VRAM_HEIGHT))
    return;

  const uint32_t scale = enhancedSurface_.scale();
  const uint32_t startX = static_cast<uint32_t>(x) * scale;
  const uint32_t startY = static_cast<uint32_t>(y) * scale;
  for (uint32_t sy = 0; sy < scale; ++sy) {
    for (uint32_t sx = 0; sx < scale; ++sx) {
      const uint32_t destinationX = startX + sx;
      const uint32_t destinationY = startY + sy;
      const Color32 output =
          blend
              ? applyModernBlendMode(
                    foreground,
                    enhancedSurface_.colorAt(destinationX, destinationY), mode)
              : foreground;
      enhancedSurface_.writeSample(destinationX, destinationY, output,
                                   sourceMask, setMaskWhileDrawing_,
                                   checkMaskBeforeDraw_, 0.0f, false, true);
    }
  }
}

void GPU::rasterizeEnhancedSurfaceTriangle(
    Vertex v0, Vertex v1, Vertex v2, TexCoord t0, TexCoord t1, TexCoord t2,
    Color24 c0, Color24 c1, Color24 c2, uint16_t clut, uint16_t tpage,
    bool textured, bool gouraud, bool rawTexture, bool blend,
    const ModernUVDomain &uvDomain, ModernDepthAction depthAction) {
  if (!enhancedSurface_.enabled())
    return;
  // The PS1 rejects oversized polygon spans before rasterization.  The modern
  // companion must share that visibility decision even though it uses
  // sub-pixel coordinates, otherwise it can display geometry absent from the
  // authoritative VRAM.
  if (!withinPolygonDrawingLimits(v0, v1, v2)) {
    ++frameModernNativeBoundsSkips_;
    return;
  }
  const std::array<int32_t, 6> nativeCoordinates = {
      v0.x, v0.y, v1.x, v1.y, v2.x, v2.y,
  };

  auto initializeAttributes = [&uvDomain](Vertex &vertex, TexCoord texCoord,
                                          Color24 color) {
    vertex.modernU = static_cast<float>(static_cast<uint16_t>(texCoord.u) +
                                        uvDomain.offsetU);
    vertex.modernV = static_cast<float>(static_cast<uint16_t>(texCoord.v) +
                                        uvDomain.offsetV);
    vertex.modernR = static_cast<float>(color.r);
    vertex.modernG = static_cast<float>(color.g);
    vertex.modernB = static_cast<float>(color.b);
  };
  initializeAttributes(v0, t0, c0);
  initializeAttributes(v1, t1, c1);
  initializeAttributes(v2, t2, c2);

  const bool normalizePacketW =
      depthAction == ModernDepthAction::NormalizeFar ||
      depthAction == ModernDepthAction::NormalizeAndInvalidateAcceptedFar;
  if (normalizePacketW) {
    // Replace every invalid packet vertex W with normalized 1.0 before the
    // vertex stage. Keep screen XY, but use raw W=32768 for clipping and
    // perspective interpolation. Only Strict also publishes it to D32.
    auto forceFarDepth = [](Vertex &vertex) {
      vertex.depth = 32768.0;
      vertex.nearDepth = 1.0;
      vertex.projectiveX = vertex.preciseX * vertex.depth;
      vertex.projectiveY = vertex.preciseY * vertex.depth;
    };
    forceFarDepth(v0);
    forceFarDepth(v1);
    forceFarDepth(v2);
  }

  const double nearDepth = std::max({v0.nearDepth, v1.nearDepth, v2.nearDepth,
                                     std::numeric_limits<double>::min()});
  auto finiteProjective = [](const Vertex &vertex) {
    return vertex.precise && std::isfinite(vertex.projectiveX) &&
           std::isfinite(vertex.projectiveY) && std::isfinite(vertex.depth) &&
           std::isfinite(vertex.nearDepth);
  };
  if (!finiteProjective(v0) || !finiteProjective(v1) || !finiteProjective(v2)) {
    ++frameModernProjectiveInvalid_;
    return;
  }

  if (!projectiveNearClipEnabled_) {
    uint64_t saturatedVertices = 0;
    auto saturate = [nearDepth, &saturatedVertices](Vertex &vertex) {
      if (vertex.depth >= nearDepth)
        return;
      // preciseX/Y were produced with the architectural maximum divider ratio
      // by the GTE sidecar. Clamp only the visibility depth so the companion
      // can retain true-Z ordering without dividing by zero/negative W.
      vertex.depth = nearDepth;
      vertex.nearDepth = nearDepth;
      vertex.projectiveX = vertex.preciseX * vertex.depth;
      vertex.projectiveY = vertex.preciseY * vertex.depth;
      ++saturatedVertices;
    };
    saturate(v0);
    saturate(v1);
    saturate(v2);
    if (saturatedVertices != 0) {
      ++frameModernNearSaturatedTriangles_;
      frameModernNearSaturatedVertices_ += saturatedVertices;
    }
    rasterizeEnhancedClippedTriangle(v0, v1, v2, clut, tpage, textured, gouraud,
                                     rawTexture, blend, nativeCoordinates,
                                     false, 0, 1, uvDomain, depthAction);
    return;
  }

  // Sutherland-Hodgman clipping in homogeneous screen space.  projectiveX/Y
  // are x_screen*z and y_screen*z, so interpolation remains finite even when
  // an endpoint lies on or behind the camera.  Native PS1 rasterization still
  // consumes the original saturated packed coordinates after this companion
  // pass.
  auto inside = [nearDepth](const Vertex &vertex) {
    return vertex.depth >= nearDepth;
  };
  const bool v0Inside = inside(v0);
  const bool v1Inside = inside(v1);
  const bool v2Inside = inside(v2);
  if (v0Inside && v1Inside && v2Inside) {
    // The common path already has Q/W from decodeVertex. Avoid copying two
    // clip arrays and dividing all three vertices again for >99.9% of the
    // reference workload.
    rasterizeEnhancedClippedTriangle(v0, v1, v2, clut, tpage, textured, gouraud,
                                     rawTexture, blend, nativeCoordinates,
                                     false, 0, 1, uvDomain, depthAction);
    return;
  }
  if (!v0Inside && !v1Inside && !v2Inside) {
    ++frameModernNearClipDiscarded_;
    if (nativeTriangleHasGeometricCoverage(nativeCoordinates, drawAreaX1_,
                                           drawAreaY1_, drawAreaX2_,
                                           drawAreaY2_)) {
      ++frameModernNearClipDiscardedNativeCoverage_;
      traceModernCoverageDivergence(
          static_cast<uint8_t>(ModernCoverageDivergence::NearPlaneDiscard),
          nativeCoordinates, v0, v1, v2, true, -1, 0);
    } else {
      ++frameModernNearClipDiscardedNativeEmpty_;
    }
    return;
  }
  std::array<Vertex, 4> input{v0, v1, v2, {}};
  std::array<Vertex, 4> output{};
  std::size_t inputCount = 3;
  std::size_t outputCount = 0;
  auto intersection = [nearDepth](const Vertex &from, const Vertex &to) {
    // Canonicalize an edge before interpolation. Both native quad halves may
    // cross the same diagonal in opposite directions; using one endpoint
    // order guarantees a bit-identical clipped vertex and prevents cracks or
    // double blending along that generated edge.
    const Vertex *a = &from;
    const Vertex *b = &to;
    // IDs prove provenance when split guest words are merged, but they are
    // intentionally not an edge-order key: adjacent packets may re-project
    // the same endpoints and receive different IDs. Homogeneous geometry is
    // stable across those packets and therefore provides the canonical key.
    const bool reverse =
        a->depth > b->depth ||
        (a->depth == b->depth && (a->projectiveX > b->projectiveX ||
                                  (a->projectiveX == b->projectiveX &&
                                   a->projectiveY > b->projectiveY)));
    if (reverse)
      std::swap(a, b);
    const double denominator =
        static_cast<double>(b->depth) - static_cast<double>(a->depth);
    const double rawT =
        denominator == 0.0
            ? 0.0
            : (static_cast<double>(nearDepth) - a->depth) / denominator;
    const double t = std::clamp(rawT, 0.0, 1.0);
    auto mixDouble = [t](double left, double right) {
      return left + (right - left) * t;
    };
    auto mixFloat = [t](float left, float right) {
      return static_cast<float>(static_cast<double>(left) +
                                (static_cast<double>(right) - left) * t);
    };
    Vertex clipped = *a;
    clipped.depth = nearDepth;
    clipped.projectiveX = mixDouble(a->projectiveX, b->projectiveX);
    clipped.projectiveY = mixDouble(a->projectiveY, b->projectiveY);
    clipped.nearDepth = nearDepth;
    clipped.modernU = mixFloat(a->modernU, b->modernU);
    clipped.modernV = mixFloat(a->modernV, b->modernV);
    clipped.modernR = mixFloat(a->modernR, b->modernR);
    clipped.modernG = mixFloat(a->modernG, b->modernG);
    clipped.modernB = mixFloat(a->modernB, b->modernB);
    clipped.projectiveId = std::min(a->projectiveId, b->projectiveId);
    clipped.precise = true;
    return clipped;
  };

  Vertex previous = input[inputCount - 1];
  bool previousInside = inside(previous);
  for (std::size_t i = 0; i < inputCount; ++i) {
    const Vertex current = input[i];
    const bool currentInside = inside(current);
    if (currentInside != previousInside && previous.depth != nearDepth &&
        current.depth != nearDepth)
      output[outputCount++] = intersection(previous, current);
    if (currentInside)
      output[outputCount++] = current;
    previous = current;
    previousInside = currentInside;
  }

  if (outputCount < 3) {
    ++frameModernNearClipDiscarded_;
    if (nativeTriangleHasGeometricCoverage(nativeCoordinates, drawAreaX1_,
                                           drawAreaY1_, drawAreaX2_,
                                           drawAreaY2_)) {
      ++frameModernNearClipDiscardedNativeCoverage_;
      traceModernCoverageDivergence(
          static_cast<uint8_t>(ModernCoverageDivergence::NearPlaneDiscard),
          nativeCoordinates, v0, v1, v2, true, -1, 0);
    } else {
      ++frameModernNearClipDiscardedNativeEmpty_;
    }
    return;
  }

  for (std::size_t i = 0; i < outputCount; ++i) {
    Vertex &vertex = output[i];
    vertex.preciseX = vertex.projectiveX / vertex.depth;
    vertex.preciseY = vertex.projectiveY / vertex.depth;
    if (!std::isfinite(vertex.preciseX) || !std::isfinite(vertex.preciseY)) {
      ++frameModernProjectiveInvalid_;
      return;
    }
  }

  ++frameModernNearClipPolygons_;
  frameModernNearClipOutputTriangles_ += outputCount - 2;
  for (std::size_t i = 1; i + 1 < outputCount; ++i)
    rasterizeEnhancedClippedTriangle(
        output[0], output[i], output[i + 1], clut, tpage, textured, gouraud,
        rawTexture, blend, nativeCoordinates, true, static_cast<int>(i - 1),
        static_cast<int>(outputCount - 2), uvDomain, depthAction);
}

void GPU::rasterizeEnhancedClippedTriangle(
    Vertex v0, Vertex v1, Vertex v2, uint16_t clut, uint16_t tpage,
    bool textured, bool gouraud, bool rawTexture, bool blend,
    const std::array<int32_t, 6> &nativeCoordinates, bool postNearClip,
    int fanIndex, int outputTriangleCount, const ModernUVDomain &uvDomain,
    ModernDepthAction depthAction) {
  if (!enhancedSurface_.enabled())
    return;

  const int scale = static_cast<int>(enhancedSurface_.scale());
  v0.preciseX *= scale;
  v0.preciseY *= scale;
  v1.preciseX *= scale;
  v1.preciseY *= scale;
  v2.preciseX *= scale;
  v2.preciseY *= scale;

  const int drawX0 = drawAreaX1_ * scale;
  const int drawY0 = drawAreaY1_ * scale;
  const int drawX1 = std::min(static_cast<int>(enhancedSurface_.width()) - 1,
                              (drawAreaX2_ + 1) * scale - 1);
  const int drawY1 = std::min(static_cast<int>(enhancedSurface_.height()) - 1,
                              (drawAreaY2_ + 1) * scale - 1);
  const double projectedArea = enhancedEdge(v0, v1, v2.preciseX, v2.preciseY);
  if (!std::isfinite(projectedArea) || std::abs(projectedArea) <= 1e-12) {
    ++frameModernPreciseDegenerate_;
    if (nativeTriangleHasGeometricCoverage(nativeCoordinates, drawAreaX1_,
                                           drawAreaY1_, drawAreaX2_,
                                           drawAreaY2_)) {
      ++frameModernPreciseDegenerateNativeCoverage_;
      traceModernCoverageDivergence(
          static_cast<uint8_t>(ModernCoverageDivergence::PreciseDegenerate),
          nativeCoordinates, v0, v1, v2, postNearClip, fanIndex,
          outputTriangleCount);
    } else {
      ++frameModernPreciseDegenerateNativeEmpty_;
    }
    return;
  }
  const auto setup =
      prepareEnhancedTriangle(v0, v1, v2, drawX0, drawY0, drawX1, drawY1);
  if (!setup.valid) {
    ++frameModernOutsideDrawArea_;
    if (nativeTriangleHasGeometricCoverage(nativeCoordinates, drawAreaX1_,
                                           drawAreaY1_, drawAreaX2_,
                                           drawAreaY2_)) {
      ++frameModernOutsideDrawAreaNativeCoverage_;
      traceModernCoverageDivergence(
          static_cast<uint8_t>(ModernCoverageDivergence::OutsideDrawArea),
          nativeCoordinates, v0, v1, v2, postNearClip, fanIndex,
          outputTriangleCount);
    } else {
      ++frameModernOutsideDrawAreaNativeEmpty_;
    }
    return;
  }

  auto rasterizeKernel = [&](auto texturedTag) {
    constexpr bool Textured = decltype(texturedTag)::value;
    const double edge0StepX = static_cast<double>(setup.v2.preciseY) -
                              static_cast<double>(setup.v1.preciseY);
    const double edge1StepX = static_cast<double>(setup.v0.preciseY) -
                              static_cast<double>(setup.v2.preciseY);
    const double edge2StepX = static_cast<double>(setup.v1.preciseY) -
                              static_cast<double>(setup.v0.preciseY);
    const double l0StepX = edge0StepX / setup.area;
    const double l1StepX = edge1StepX / setup.area;
    const double l2StepX = edge2StepX / setup.area;
    const double inverseDepthStepX = Textured ? l0StepX / setup.v0.depth +
                                                    l1StepX / setup.v1.depth +
                                                    l2StepX / setup.v2.depth
                                              : 0.0;
    // SwanStation puts PGXP W in clip-space Z and lets the host rasterizer
    // interpolate window depth affinely. It is deliberately separate from the
    // reciprocal interpolation used for perspective-correct UVs.
    const double affineDepthStepX = l0StepX * setup.v0.depth +
                                    l1StepX * setup.v1.depth +
                                    l2StepX * setup.v2.depth;
    double uOverDepthStepX = 0.0;
    double vOverDepthStepX = 0.0;
    if constexpr (Textured) {
      const DoublePair uv0 =
          divideDoublePair(l0StepX * setup.v0.modernU,
                           l0StepX * setup.v0.modernV, setup.v0.depth);
      const DoublePair uv1 =
          divideDoublePair(l1StepX * setup.v1.modernU,
                           l1StepX * setup.v1.modernV, setup.v1.depth);
      const DoublePair uv2 =
          divideDoublePair(l2StepX * setup.v2.modernU,
                           l2StepX * setup.v2.modernV, setup.v2.depth);
      uOverDepthStepX = uv0.first + uv1.first + uv2.first;
      vOverDepthStepX = uv0.second + uv1.second + uv2.second;
    }
    const double shadeRStepX = gouraud ? l0StepX * setup.v0.modernR +
                                             l1StepX * setup.v1.modernR +
                                             l2StepX * setup.v2.modernR
                                       : 0.0;
    const double shadeGStepX = gouraud ? l0StepX * setup.v0.modernG +
                                             l1StepX * setup.v1.modernG +
                                             l2StepX * setup.v2.modernG
                                       : 0.0;
    const double shadeBStepX = gouraud ? l0StepX * setup.v0.modernB +
                                             l1StepX * setup.v1.modernB +
                                             l2StepX * setup.v2.modernB
                                       : 0.0;
    const int scanlineSpan = setup.maxX - setup.minX;
    // STP is a blend selector only for semi-transparent textured packets.
    // Hybrid Store follows GP0 order and publishes genuine W without a reject;
    // Strict TestWrite retains diagnostic LEQUAL. Painter composites write far
    // only after all fragment rejection checks. Invalid-W still forces
    // packet-atomic W=1 for geometry/UV.
    const bool testDepth = depthAction == ModernDepthAction::TestWrite;
    const bool storeDepth = depthAction == ModernDepthAction::Store;
    const bool invalidateAcceptedFar =
        depthAction == ModernDepthAction::InvalidateAcceptedFar ||
        depthAction == ModernDepthAction::NormalizeAndInvalidateAcceptedFar;
    const bool writeFarDepth = depthAction == ModernDepthAction::NormalizeFar &&
                               trueDepthMode_ == TrueDepthMode::Strict;
    const bool writeDepth = testDepth || storeDepth || writeFarDepth;

    uint64_t invalidDepthSamples = 0;
    uint64_t maskRejectedSamples = 0;
    uint64_t depthRejectedSamples = 0;
    uint64_t transparentSamples = 0;
    uint64_t acceptedSamples = 0;
    uint64_t uvClampedSamples = 0;
    uint64_t uvSampleCacheMisses = 0;
    bool uvSampleCacheValid = false;
    int uvSampleCacheU = 0;
    int uvSampleCacheV = 0;
    Color16 uvSampleCacheTexel{};
    Color32 uvSampleCacheOutput{};
    bool uvSampleCacheOutputValid = false;
    for (int y = setup.minY; y <= setup.maxY; ++y) {
      const auto row = enhancedSurface_.mutableRow(static_cast<uint32_t>(y));
      if (!row)
        continue;

      const double px = static_cast<double>(setup.minX) + 0.5;
      const double py = static_cast<double>(y) + 0.5;
      double w0 = enhancedEdge(setup.v1, setup.v2, px, py);
      double w1 = enhancedEdge(setup.v2, setup.v0, px, py);
      double w2 = enhancedEdge(setup.v0, setup.v1, px, py);
      const double l0 = w0 / setup.area;
      const double l1 = w1 / setup.area;
      const double l2 = w2 / setup.area;
      double inverseDepth =
          Textured
              ? l0 / setup.v0.depth + l1 / setup.v1.depth + l2 / setup.v2.depth
              : 1.0;
      double affineDepth =
          l0 * setup.v0.depth + l1 * setup.v1.depth + l2 * setup.v2.depth;
      double uOverDepth = 0.0;
      double vOverDepth = 0.0;
      if constexpr (Textured) {
        const DoublePair uv0 = divideDoublePair(
            l0 * setup.v0.modernU, l0 * setup.v0.modernV, setup.v0.depth);
        const DoublePair uv1 = divideDoublePair(
            l1 * setup.v1.modernU, l1 * setup.v1.modernV, setup.v1.depth);
        const DoublePair uv2 = divideDoublePair(
            l2 * setup.v2.modernU, l2 * setup.v2.modernV, setup.v2.depth);
        uOverDepth = uv0.first + uv1.first + uv2.first;
        vOverDepth = uv0.second + uv1.second + uv2.second;
      }
      double shadeR = gouraud ? l0 * setup.v0.modernR + l1 * setup.v1.modernR +
                                    l2 * setup.v2.modernR
                              : setup.v0.modernR;
      double shadeG = gouraud ? l0 * setup.v0.modernG + l1 * setup.v1.modernG +
                                    l2 * setup.v2.modernG
                              : setup.v0.modernG;
      double shadeB = gouraud ? l0 * setup.v0.modernB + l1 * setup.v1.modernB +
                                    l2 * setup.v2.modernB
                              : setup.v0.modernB;

      const double edge0Tolerance =
          incrementalEdgeTolerance(w0, edge0StepX, scanlineSpan);
      const double edge1Tolerance =
          incrementalEdgeTolerance(w1, edge1StepX, scanlineSpan);
      const double edge2Tolerance =
          incrementalEdgeTolerance(w2, edge2StepX, scanlineSpan);

      bool scanlineEnteredTriangle = false;
      for (int x = setup.minX; x <= setup.maxX; ++x) {
        double sampleW0 = w0;
        double sampleW1 = w1;
        double sampleW2 = w2;
        double sampleInverseDepth = inverseDepth;
        double sampleAffineDepth = affineDepth;
        double sampleUOverDepth = uOverDepth;
        double sampleVOverDepth = vOverDepth;
        double sampleShadeR = shadeR;
        double sampleShadeG = shadeG;
        double sampleShadeB = shadeB;

        w0 += edge0StepX;
        w1 += edge1StepX;
        w2 += edge2StepX;
        if constexpr (Textured)
          inverseDepth += inverseDepthStepX;
        affineDepth += affineDepthStepX;
        uOverDepth += uOverDepthStepX;
        vOverDepth += vOverDepthStepX;
        shadeR += shadeRStepX;
        shadeG += shadeGStepX;
        shadeB += shadeBStepX;

        const double sampleX = static_cast<double>(x) + 0.5;
        const bool edge0Near = std::abs(sampleW0) <= edge0Tolerance;
        const bool edge1Near = std::abs(sampleW1) <= edge1Tolerance;
        const bool edge2Near = std::abs(sampleW2) <= edge2Tolerance;
        const bool nearEdge = edge0Near || edge1Near || edge2Near;
        if (edge0Near)
          sampleW0 = enhancedEdge(setup.v1, setup.v2, sampleX, py);
        if (edge1Near)
          sampleW1 = enhancedEdge(setup.v2, setup.v0, sampleX, py);
        if (edge2Near)
          sampleW2 = enhancedEdge(setup.v0, setup.v1, sampleX, py);
        if (!enhancedEdgeCovers(sampleW0, setup.edge0Inclusive) ||
            !enhancedEdgeCovers(sampleW1, setup.edge1Inclusive) ||
            !enhancedEdgeCovers(sampleW2, setup.edge2Inclusive)) {
          // A convex triangle intersects a scanline in at most one interval.
          // Once that interval has ended, the remainder of the bounding box
          // cannot re-enter and none of its interpolants are observable.
          if (scanlineEnteredTriangle)
            break;
          continue;
        }
        scanlineEnteredTriangle = true;
        // Preserve the exact barycentric result on shared/top-left edges. Away
        // from that narrow numerical band all interpolants stay incremental.
        if (nearEdge) {
          const double sampleL0 = sampleW0 / setup.area;
          const double sampleL1 = sampleW1 / setup.area;
          const double sampleL2 = sampleW2 / setup.area;
          sampleAffineDepth = sampleL0 * setup.v0.depth +
                              sampleL1 * setup.v1.depth +
                              sampleL2 * setup.v2.depth;
          if constexpr (Textured) {
            sampleInverseDepth = sampleL0 / setup.v0.depth +
                                 sampleL1 / setup.v1.depth +
                                 sampleL2 / setup.v2.depth;
            const DoublePair uv0 =
                divideDoublePair(sampleL0 * setup.v0.modernU,
                                 sampleL0 * setup.v0.modernV, setup.v0.depth);
            const DoublePair uv1 =
                divideDoublePair(sampleL1 * setup.v1.modernU,
                                 sampleL1 * setup.v1.modernV, setup.v1.depth);
            const DoublePair uv2 =
                divideDoublePair(sampleL2 * setup.v2.modernU,
                                 sampleL2 * setup.v2.modernV, setup.v2.depth);
            sampleUOverDepth = uv0.first + uv1.first + uv2.first;
            sampleVOverDepth = uv0.second + uv1.second + uv2.second;
          }
          if (gouraud) {
            sampleShadeR = sampleL0 * setup.v0.modernR +
                           sampleL1 * setup.v1.modernR +
                           sampleL2 * setup.v2.modernR;
            sampleShadeG = sampleL0 * setup.v0.modernG +
                           sampleL1 * setup.v1.modernG +
                           sampleL2 * setup.v2.modernG;
            sampleShadeB = sampleL0 * setup.v0.modernB +
                           sampleL1 * setup.v1.modernB +
                           sampleL2 * setup.v2.modernB;
          }
        }

        if constexpr (Textured) {
          if (!(sampleInverseDepth > 0.0) ||
              !std::isfinite(sampleInverseDepth)) {
            ++invalidDepthSamples;
            continue;
          }
        }
        float sampleDepth = 0.0f;
        if (writeDepth) {
          sampleDepth =
              writeFarDepth ? 32768.0f : static_cast<float>(sampleAffineDepth);
          if (!std::isfinite(sampleDepth) || !(sampleDepth > 0.0f)) {
            ++invalidDepthSamples;
            continue;
          }
        }

        const uint32_t destinationX = static_cast<uint32_t>(x);
        if (checkMaskBeforeDraw_ && row.mask[destinationX] != 0) {
          ++maskRejectedSamples;
          continue;
        }
        if (testDepth && row.depthEpoch[destinationX] == row.activeDepthEpoch &&
            sampleDepth > row.depth[destinationX]) {
          ++depthRejectedSamples;
          continue;
        }

        Color24 shade{static_cast<uint8_t>(std::clamp(
                          static_cast<int>(setup.v0.modernR), 0, 255)),
                      static_cast<uint8_t>(std::clamp(
                          static_cast<int>(setup.v0.modernG), 0, 255)),
                      static_cast<uint8_t>(std::clamp(
                          static_cast<int>(setup.v0.modernB), 0, 255))};
        if (gouraud) {
          shade.r = static_cast<uint8_t>(
              std::clamp(static_cast<int>(sampleShadeR), 0, 255));
          shade.g = static_cast<uint8_t>(
              std::clamp(static_cast<int>(sampleShadeG), 0, 255));
          shade.b = static_cast<uint8_t>(
              std::clamp(static_cast<int>(sampleShadeB), 0, 255));
        }

        Color16 texel{};
        bool sourceMask = false;
        Color32 output;
        if constexpr (Textured) {
          const DoublePair reconstructed = divideDoublePair(
              sampleUOverDepth, sampleVOverDepth, sampleInverseDepth);
          const double reconstructedU = reconstructed.first;
          const double reconstructedV = reconstructed.second;
          const double clampedU =
              uvDomain.enabled
                  ? std::clamp(reconstructedU, uvDomain.minU, uvDomain.maxU)
                  : reconstructedU;
          const double clampedV =
              uvDomain.enabled
                  ? std::clamp(reconstructedV, uvDomain.minV, uvDomain.maxV)
                  : reconstructedV;
          if (clampedU != reconstructedU || clampedV != reconstructedV)
            ++uvClampedSamples;
          // Truncate only after the packet domain clamp. sampleTexel() then
          // performs the architectural 8-bit wrap and E2 texture window.
          const int u = static_cast<int>(clampedU);
          const int v = static_cast<int>(clampedV);
          const bool cacheHit =
              uvSampleCacheValid && u == uvSampleCacheU && v == uvSampleCacheV;
          if (cacheHit) {
            texel = uvSampleCacheTexel;
          } else {
            texel = sampleTexel(u, v, tpage, clut);
            uvSampleCacheValid = true;
            uvSampleCacheU = u;
            uvSampleCacheV = v;
            uvSampleCacheTexel = texel;
            uvSampleCacheOutputValid = false;
            ++uvSampleCacheMisses;
          }
          if (texel.raw == 0) {
            ++transparentSamples;
            continue;
          }
          sourceMask = (texel.raw & 0x8000u) != 0;
          if (cacheHit && uvSampleCacheOutputValid) {
            output = uvSampleCacheOutput;
          } else {
            output = rawTexture ? EnhancedSurface::expandNativeColor(texel.raw)
                                : modulateModernTexel(texel, shade, x, y);
            // Raw texels never depend on shade. Flat modulation is also stable
            // across identical UV samples in true-color mode; RGB555+dither and
            // Gouraud output retain their per-sample calculation.
            if (rawTexture || (!gouraud && trueColorEnabled_)) {
              uvSampleCacheOutput = output;
              uvSampleCacheOutputValid = true;
            }
          }
          if (blend && sourceMask)
            output =
                applyModernBlendMode(output, row.color[destinationX],
                                     static_cast<uint8_t>((tpage >> 5) & 3u));
        } else {
          output = packModernColor(shade.r, shade.g, shade.b, false, x, y);
          if (blend)
            output = applyModernBlendMode(output, row.color[destinationX],
                                          blendMode_);
        }

        if (invalidateAcceptedFar) {
          // Epoch mismatch is the scalar surface's exact infinity/far
          // sentinel. Do not store raw W=32768 here: replay normalizes raw W
          // by 2^-32, whereas Vulkan's accepted-invalidate path writes D32
          // 1.0. Clearing ownership makes both domains agree bit-for-bit.
          row.depthEpoch[destinationX] = 0;
        } else if (writeDepth) {
          row.depth[destinationX] = sampleDepth;
          row.depthEpoch[destinationX] = row.activeDepthEpoch;
        }
        output.a = 255;
        row.color[destinationX] = output;
        row.mask[destinationX] = sourceMask || setMaskWhileDrawing_ ? 1u : 0u;
        ++acceptedSamples;
      }
    }
    // Every covered sample terminates in exactly one outcome. Deriving the
    // total removes one hot-loop increment per covered 4x sample (4.98 billion
    // increments in the reference soak) without changing telemetry semantics.
    const uint64_t coveredSamples = invalidDepthSamples + maskRejectedSamples +
                                    depthRejectedSamples + transparentSamples +
                                    acceptedSamples;
    frameModernCoveredSamples_ += coveredSamples;
    frameModernInvalidDepthSamples_ += invalidDepthSamples;
    frameModernMaskRejectedSamples_ += maskRejectedSamples;
    frameModernDepthRejectedSamples_ += depthRejectedSamples;
    frameModernTransparentSamples_ += transparentSamples;
    frameModernAcceptedSamples_ += acceptedSamples;
    if (invalidateAcceptedFar)
      frameModernDepthVisibilityBoundarySamples_ += acceptedSamples;
    frameModernUVClampedSamples_ += uvClampedSamples;
    const uint64_t uvSampleCacheHits =
        Textured ? acceptedSamples + transparentSamples - uvSampleCacheMisses
                 : 0;
    frameModernUVSampleCacheHits_ += uvSampleCacheHits;
    frameModernUVSampleCacheMisses_ += uvSampleCacheMisses;
    if (coveredSamples != 0 && acceptedSamples == 0) {
      ++frameModernEmptyTriangles_;
      if (depthRejectedSamples == coveredSamples)
        ++frameModernEmptyDepthOnly_;
      else if (transparentSamples == coveredSamples)
        ++frameModernEmptyTransparentOnly_;
      else if (maskRejectedSamples == coveredSamples)
        ++frameModernEmptyMaskOnly_;
      else if (invalidDepthSamples == coveredSamples)
        ++frameModernEmptyInvalidDepthOnly_;
      else
        ++frameModernEmptyMixed_;
    }
  };

  if (textured)
    rasterizeKernel(std::true_type{});
  else
    rasterizeKernel(std::false_type{});
}

void GPU::rasterizeTriangle(Vertex v0, Vertex v1, Vertex v2, Color24 color,
                            bool blend, bool precisePacket) {
  if (!withinPolygonDrawingLimits(v0, v1, v2)) {
    noteBoundsReject();
    return;
  }
  // Check winding and swap if necessary so we have CCW
  int area = edgeFunction(v0, v1, v2);
  if (area == 0)
    return; // Degenerate
  if (area < 0) {
    std::swap(v1, v2); // Make CCW
  }
  const bool edge0Inclusive = isTopLeftEdge(v1, v2);
  const bool edge1Inclusive = isTopLeftEdge(v2, v0);
  const bool edge2Inclusive = isTopLeftEdge(v0, v1);

  // Bounding box
  int minX = std::min({v0.x, v1.x, v2.x});
  int minY = std::min({v0.y, v1.y, v2.y});
  int maxX = std::max({v0.x, v1.x, v2.x});
  int maxY = std::max({v0.y, v1.y, v2.y});

  // Clip against draw area (DrawAreaX1, Y1 / X2, Y2)
  minX = std::max(minX, drawAreaX1_);
  minY = std::max(minY, drawAreaY1_);
  maxX = std::min(maxX, drawAreaX2_);
  maxY = std::min(maxY, drawAreaY2_);

  // Edge equations are affine. Evaluate them once at the clipped origin and
  // walk them with integer additions; this is bit-identical to calling
  // edgeFunction() for every native pixel and removes six multiplies from the
  // hot loop.
  const Vertex rasterOrigin{minX, minY};
  int rowW0 = edgeFunction(v1, v2, rasterOrigin);
  int rowW1 = edgeFunction(v2, v0, rasterOrigin);
  int rowW2 = edgeFunction(v0, v1, rasterOrigin);
  const int stepX0 = v2.y - v1.y;
  const int stepX1 = v0.y - v2.y;
  const int stepX2 = v1.y - v0.y;
  const int stepY0 = v1.x - v2.x;
  const int stepY1 = v2.x - v0.x;
  const int stepY2 = v0.x - v1.x;
  for (int y = minY; y <= maxY; ++y) {
    int w0 = rowW0;
    int w1 = rowW1;
    int w2 = rowW2;
    for (int x = minX; x <= maxX; ++x) {

      if (edgeCovers(w0, edge0Inclusive) && edgeCovers(w1, edge1Inclusive) &&
          edgeCovers(w2, edge2Inclusive)) {
        // Draw pixel
        Color16 finalColor =
            packColor(color.r, color.g, color.b, false, x, y, ditherEnable_);

        uint32_t idx =
            static_cast<uint32_t>(y) * VRAM_WIDTH + static_cast<uint32_t>(x);
        if (blend) {
          Color16 bg = vram_[idx];
          finalColor = applyBlend(finalColor, bg);
        }
        const bool wrote =
            writePixel(x, y, finalColor, !precisePacket && !trueColorEnabled_);
        if (wrote && !precisePacket && trueColorEnabled_)
          writeModernBlock(
              x, y,
              packModernColor(color.r, color.g, color.b, false,
                              x * static_cast<int>(enhancedSurface_.scale()),
                              y * static_cast<int>(enhancedSurface_.scale())),
              false, blend, blendMode_);
      }
      w0 += stepX0;
      w1 += stepX1;
      w2 += stepX2;
    }
    rowW0 += stepY0;
    rowW1 += stepY1;
    rowW2 += stepY2;
  }
}

void GPU::rasterizeTexturedTriangle(Vertex v0, Vertex v1, Vertex v2,
                                    TexCoord t0, TexCoord t1, TexCoord t2,
                                    Color24 color, uint16_t clut,
                                    uint16_t tpage, bool isRaw, bool blend,
                                    bool precisePacket) {
  if (!withinPolygonDrawingLimits(v0, v1, v2)) {
    noteBoundsReject();
    return;
  }
  int area = edgeFunction(v0, v1, v2);
  if (area == 0)
    return;
  if (area < 0) {
    std::swap(v1, v2);
    std::swap(t1, t2);
    area = -area;
  }
  const bool edge0Inclusive = isTopLeftEdge(v1, v2);
  const bool edge1Inclusive = isTopLeftEdge(v2, v0);
  const bool edge2Inclusive = isTopLeftEdge(v0, v1);

  int minX = std::max({drawAreaX1_, std::min({v0.x, v1.x, v2.x})});
  int minY = std::max({drawAreaY1_, std::min({v0.y, v1.y, v2.y})});
  int maxX = std::min({drawAreaX2_, std::max({v0.x, v1.x, v2.x})});
  int maxY = std::min({drawAreaY2_, std::max({v0.y, v1.y, v2.y})});

  uint32_t tpX = (tpage & 0xF) * 64;
  uint32_t tpY = ((tpage >> 4) & 1) * 256;
  uint32_t depth = (tpage >> 7) & 3;

  uint32_t clutX = (clut & 0x3F) * 16;
  uint32_t clutY = (clut >> 6) & 0x1FF;

  const Vertex rasterOrigin{minX, minY};
  int rowW0 = edgeFunction(v1, v2, rasterOrigin);
  int rowW1 = edgeFunction(v2, v0, rasterOrigin);
  int rowW2 = edgeFunction(v0, v1, rasterOrigin);
  const int stepX0 = v2.y - v1.y;
  const int stepX1 = v0.y - v2.y;
  const int stepX2 = v1.y - v0.y;
  const int stepY0 = v1.x - v2.x;
  const int stepY1 = v2.x - v0.x;
  const int stepY2 = v0.x - v1.x;
  AffineAttributeWalker rowU(
      affineAttributeNumerator(rowW0, rowW1, rowW2, t0.u, t1.u, t2.u),
      affineAttributeNumerator(stepX0, stepX1, stepX2, t0.u, t1.u, t2.u),
      affineAttributeNumerator(stepY0, stepY1, stepY2, t0.u, t1.u, t2.u),
      area);
  AffineAttributeWalker rowV(
      affineAttributeNumerator(rowW0, rowW1, rowW2, t0.v, t1.v, t2.v),
      affineAttributeNumerator(stepX0, stepX1, stepX2, t0.v, t1.v, t2.v),
      affineAttributeNumerator(stepY0, stepY1, stepY2, t0.v, t1.v, t2.v),
      area);
  for (int y = minY; y <= maxY; ++y) {
    int w0 = rowW0;
    int w1 = rowW1;
    int w2 = rowW2;
    AffineAttributeWalker uWalker = rowU;
    AffineAttributeWalker vWalker = rowV;
    for (int x = minX; x <= maxX; ++x) {
      if (edgeCovers(w0, edge0Inclusive) && edgeCovers(w1, edge1Inclusive) &&
          edgeCovers(w2, edge2Inclusive)) {
        int u = uWalker.value();
        int v = vWalker.value();

        // Apply Texture Window (E2) masking and offsets
        if (texWindowMaskX_ != 0 || texWindowOffsetX_ != 0) {
          u = (u & ~(texWindowMaskX_)) | (texWindowOffsetX_ & texWindowMaskX_);
        }
        if (texWindowMaskY_ != 0 || texWindowOffsetY_ != 0) {
          v = (v & ~(texWindowMaskY_)) | (texWindowOffsetY_ & texWindowMaskY_);
        }

        Color16 texColor;
        if (depth == 0) { // 4-bit
          uint32_t tx = tpX + (u / 4);
          uint32_t ty = tpY + v;
          uint16_t block =
              vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)].raw;
          uint8_t index = (block >> ((u % 4) * 4)) & 0xF;
          texColor = vram_[(clutY % VRAM_HEIGHT) * VRAM_WIDTH +
                           ((clutX + index) % VRAM_WIDTH)];
        } else if (depth == 1) { // 8-bit
          uint32_t tx = tpX + (u / 2);
          uint32_t ty = tpY + v;
          uint16_t block =
              vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)].raw;
          uint8_t index = (block >> ((u % 2) * 8)) & 0xFF;
          texColor = vram_[(clutY % VRAM_HEIGHT) * VRAM_WIDTH +
                           ((clutX + index) % VRAM_WIDTH)];
        } else { // 15-bit
          uint32_t tx = tpX + u;
          uint32_t ty = tpY + v;
          texColor = vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)];
        }

        if (texColor.raw != 0) {
          uint32_t idx = (y % VRAM_HEIGHT) * VRAM_WIDTH + (x % VRAM_WIDTH);

          Color16 finalColor =
              isRaw ? texColor : modulateTexel(texColor, color, x, y);
          const bool sourceMask = (texColor.raw & 0x8000u) != 0;
          const bool modernBlend = blend && sourceMask;
          const uint8_t modernBlendMode =
              static_cast<uint8_t>((tpage >> 5) & 3u);
          // Textured semi-transparency is selected per texel by STP. Texels
          // without bit15 remain opaque even when the opcode is semi.
          if (modernBlend)
            finalColor =
                applyBlendMode(finalColor, vram_[idx], modernBlendMode);
          const bool wrote = writePixel(
              x, y, finalColor, !precisePacket && !trueColorEnabled_);
          if (wrote && !precisePacket && trueColorEnabled_) {
            const Color32 modernColor =
                isRaw ? EnhancedSurface::expandNativeColor(texColor.raw)
                      : modulateModernTexel(
                            texColor, color,
                            x * static_cast<int>(enhancedSurface_.scale()),
                            y * static_cast<int>(enhancedSurface_.scale()));
            writeModernBlock(x, y, modernColor, sourceMask, modernBlend,
                             modernBlendMode);
          }
        }
      }
      w0 += stepX0;
      w1 += stepX1;
      w2 += stepX2;
      uWalker.advanceX();
      vWalker.advanceX();
    }
    rowW0 += stepY0;
    rowW1 += stepY1;
    rowW2 += stepY2;
    rowU.advanceY();
    rowV.advanceY();
  }
}

void GPU::beginReplayPaintCapture() {
  replayPaintCapture_.clear();
  replayPaintCaptureActive_ = replayRecorder_.enabled();
}

void GPU::captureReplayPaintedPixel(int x, int y, Color16 nativeColor,
                                    replay::Rgb8 color, bool sourceMaskBit) {
  if (!replayPaintCaptureActive_)
    return;
  replayPaintCapture_.push_back(
      replay::PaintedPixel{{x, y}, nativeColor.raw, color, sourceMaskBit});
}

void GPU::finishReplayPaintCapture(uint8_t opcode) {
  const bool wasActive = replayPaintCaptureActive_;
  replayPaintCaptureActive_ = false;
  if (!wasActive)
    return;
  if (replayPaintCapture_.empty()) {
    replayRecorder_.recordNoOp(opcode);
    return;
  }

  replay::PaintPixels operation;
  operation.pixels = std::move(replayPaintCapture_);
  const bool textured = (opcode & 0x04u) != 0;
  const uint8_t blend =
      textured ? static_cast<uint8_t>((currentTexpage_ >> 5) & 3u) : blendMode_;
  switch (blend) {
  case 0:
    operation.blend.mode = replay::BlendMode::HalfBackgroundPlusHalfForeground;
    break;
  case 1:
    operation.blend.mode = replay::BlendMode::BackgroundPlusForeground;
    break;
  case 2:
    operation.blend.mode = replay::BlendMode::BackgroundMinusForeground;
    break;
  default:
    operation.blend.mode = replay::BlendMode::BackgroundPlusQuarterForeground;
    break;
  }
  operation.blend.enabled = (opcode & 0x02u) != 0;
  operation.blend.texturedStpOnly = textured;
  operation.mask = {setMaskWhileDrawing_, checkMaskBeforeDraw_};
  operation.depth.epoch = replayDepthEpoch_;
  operation.depth.invalidateOnAccepted = true;
  operation.dither = ditherEnable_ && (((opcode & 0x10u) != 0) || textured);
  ps1::metrics::count("gpu.replay.paint_pixels.operation");
  ps1::metrics::count("gpu.replay.paint_pixels.pixel", operation.pixels.size());
  if (operation.blend.enabled)
    ps1::metrics::count("gpu.replay.paint_pixels.blended_operation");
  if (operation.blend.texturedStpOnly)
    ps1::metrics::count("gpu.replay.paint_pixels.textured_stp_operation");
  if (operation.mask.checkMaskBit)
    ps1::metrics::count("gpu.replay.paint_pixels.check_mask_operation");
  if (operation.mask.forceMaskBit)
    ps1::metrics::count("gpu.replay.paint_pixels.force_mask_operation");
  if (operation.dither)
    ps1::metrics::count("gpu.replay.paint_pixels.dithered_operation");
  replayRecorder_.recordPaintPixels(opcode, std::move(operation));
}

void GPU::executeLine() {
  const uint32_t opcode = commandQueue_[0] >> 24;
  const bool gouraud = (opcode & 0x10u) != 0;
  const bool polyline = (opcode & 0x08u) != 0;
  const bool blend = (opcode & 0x02u) != 0;
  const PGXPPrimitive primitive =
      gouraud ? (polyline ? PGXPPrimitive::GouraudPolyline
                          : PGXPPrimitive::GouraudLine)
              : (polyline ? PGXPPrimitive::Polyline : PGXPPrimitive::Line);
  auto classifyScreenVertex = [&](std::size_t wordIndex) {
    const PGXPValue precision =
        wordIndex < commandPrecisionQueue_.size() &&
                commandPrecisionQueue_[wordIndex].has_value()
            ? *commandPrecisionQueue_[wordIndex]
            : PGXPValue{};
    const uint32_t sourceAddress = wordIndex < commandSourceQueue_.size()
                                       ? commandSourceQueue_[wordIndex]
                                       : UINT32_MAX;
    recordPGXPVertex(static_cast<uint8_t>(opcode),
                     static_cast<uint8_t>(wordIndex), sourceAddress, primitive,
                     PGXPSemanticDecision::intrinsicScreen2D(), precision,
                     commandQueue_[wordIndex]);
  };
  const std::size_t vertexCount =
      gouraud ? commandQueue_.size() / 2u : commandQueue_.size() - 1u;
  // Modern fixed lines are thin camera-facing ribbons when BOTH endpoints
  // have authenticated signed projections. CPU Bresenham remains untouched.
  // A polyline or an unproven endpoint retains atomic native PaintPixels.
  const std::size_t lastWord = gouraud ? 3u : 2u;
  bool projectedLine = replayRecorder_.enabled() &&
                       replayRecorder_.config().pgxp && !polyline &&
                       (opcode & 0xecu) == 0x40u && vertexCount == 2u;
  std::array<PGXPResolvedVertex, 2> resolved{};
  if (projectedLine) {
    for (std::size_t index = 0u; index != 2u; ++index) {
      const std::size_t word = index == 0u ? 1u : lastWord;
      if (word >= commandPrecisionQueue_.size() ||
          !commandPrecisionQueue_[word].has_value()) {
        projectedLine = false;
        break;
      }
      resolved[index] =
          resolvePGXPVertex(*commandPrecisionQueue_[word], commandQueue_[word]);
      projectedLine &=
          resolved[index].hasSignedProjection && resolved[index].z > 0.0 &&
          resolved[index].z >= resolved[index].nearDepth &&
          isAuthenticPGXPCoordinateSource(resolved[index].xSource) &&
          isAuthenticPGXPCoordinateSource(resolved[index].ySource);
    }
    projectedLine &=
        resolved[0].projectionContext == resolved[1].projectionContext &&
        resolved[0].nearDepth == resolved[1].nearDepth;
  }
  bool projectedLineCaptured = false;
  if (projectedLine) {
    const Vertex a = decodeVertex(1u, PGXPSemanticDecision{});
    const Vertex b = decodeVertex(lastWord, PGXPSemanticDecision{});
    const double scale = games::crash3::horizontalProjectionScale();
    const double dx = (b.preciseX - a.preciseX) / scale;
    const double dy = b.preciseY - a.preciseY;
    const double length = std::hypot(dx, dy);
    // A coincident pair is a square point rather than a normalized zero
    // vector. The same ribbon construction then remains finite.
    const double tx = length > 0.0 ? dx / length : 1.0;
    const double ty = length > 0.0 ? dy / length : 0.0;
    std::array<Vertex, 4> corners{a, a, b, b};
    std::array<Color24, 4> colors{};
    for (uint32_t corner = 0u; corner != 4u; ++corner) {
      const double end = corner < 2u ? -0.5 : 0.5;
      const double side = (corner & 1u) == 0u ? -0.5 : 0.5;
      const double ox = (0.5 + end * tx - side * ty) * scale;
      const double oy = 0.5 + end * ty + side * tx;
      auto &vertex = corners[corner];
      vertex.x += static_cast<int32_t>(std::lround(ox));
      vertex.y += static_cast<int32_t>(std::lround(oy));
      vertex.preciseX += ox;
      vertex.preciseY += oy;
      vertex.projectiveX += ox * vertex.depth;
      vertex.projectiveY += oy * vertex.depth;
      vertex.rawProjectiveX += ox * vertex.depth;
      vertex.rawProjectiveY += oy * vertex.depth;
      const uint32_t rgb = commandQueue_[gouraud && corner >= 2u ? 2u : 0u];
      colors[corner] = {static_cast<uint8_t>(rgb),
                        static_cast<uint8_t>(rgb >> 8u),
                        static_cast<uint8_t>(rgb >> 16u)};
    }
    projectedLineCaptured = capturePolygonPacket(
        corners.data(), nullptr, colors.data(), 4u, 0u, currentTexpage_,
        PGXPSemanticDecision{}, 0x0fu, false, gouraud, false, blend);
    if (projectedLineCaptured)
      metrics::count("host.gpu.projected_line_captured");
  }
  for (std::size_t vertexIndex = 0; !projectedLine && vertexIndex < vertexCount;
       ++vertexIndex) {
    const std::size_t wordIndex =
        gouraud ? vertexIndex * 2u + 1u : vertexIndex + 1u;
    classifyScreenVertex(wordIndex);
  }

  if (!projectedLineCaptured)
    beginReplayPaintCapture();
  for (std::size_t segment = 0; segment + 1u < vertexCount; ++segment) {
    const std::size_t v0Index = gouraud ? segment * 2u + 1u : segment + 1u;
    const std::size_t v1Index =
        gouraud ? (segment + 1u) * 2u + 1u : segment + 2u;
    const std::size_t c0Index = gouraud ? segment * 2u : 0u;
    const std::size_t c1Index = gouraud ? (segment + 1u) * 2u : 0u;
    const uint32_t v0word = commandQueue_[v0Index];
    const uint32_t v1word = commandQueue_[v1Index];
    const uint32_t c0word = commandQueue_[c0Index];
    const uint32_t c1word = commandQueue_[c1Index];
    const Vertex v0{decodeSigned11(v0word), decodeSigned11(v0word >> 16)};
    const Vertex v1{decodeSigned11(v1word), decodeSigned11(v1word >> 16)};
    const Color24 c0{static_cast<uint8_t>(c0word),
                     static_cast<uint8_t>(c0word >> 8),
                     static_cast<uint8_t>(c0word >> 16)};
    const Color24 c1{static_cast<uint8_t>(c1word),
                     static_cast<uint8_t>(c1word >> 8),
                     static_cast<uint8_t>(c1word >> 16)};
    drawLineSegment(Vertex{v0.x + drawOffsetX_, v0.y + drawOffsetY_},
                    Vertex{v1.x + drawOffsetX_, v1.y + drawOffsetY_}, c0, c1,
                    gouraud, blend);
  }
  if (!projectedLineCaptured)
    finishReplayPaintCapture(static_cast<uint8_t>(opcode));
}

void GPU::drawLineSegment(Vertex v0, Vertex v1, Color24 c0, Color24 c1,
                          bool gouraud, bool blend) {
  if (std::abs(v1.x - v0.x) >= 1024 || std::abs(v1.y - v0.y) >= 512) {
    noteBoundsReject();
    return;
  }
  int dx = std::abs(v1.x - v0.x);
  int sx = v0.x < v1.x ? 1 : -1;
  int dy = -std::abs(v1.y - v0.y);
  int sy = v0.y < v1.y ? 1 : -1;
  int err = dx + dy;
  const int steps = std::max(dx, -dy);
  int x = v0.x;
  int y = v0.y;
  int step = 0;
  while (true) {
    if (x >= drawAreaX1_ && x <= drawAreaX2_ && y >= drawAreaY1_ &&
        y <= drawAreaY2_) {
      const int r = gouraud && steps != 0
                        ? c0.r + (static_cast<int>(c1.r) - c0.r) * step / steps
                        : c0.r;
      const int g = gouraud && steps != 0
                        ? c0.g + (static_cast<int>(c1.g) - c0.g) * step / steps
                        : c0.g;
      const int b = gouraud && steps != 0
                        ? c0.b + (static_cast<int>(c1.b) - c0.b) * step / steps
                        : c0.b;
      Color16 finalColor =
          packColor(r, g, b, false, x, y, gouraud && ditherEnable_);
      const uint32_t idx =
          static_cast<uint32_t>(y) * VRAM_WIDTH + static_cast<uint32_t>(x);
      if (blend)
        finalColor = applyBlend(finalColor, vram_[idx]);
      const bool wrote = writePixel(x, y, finalColor, !trueColorEnabled_);
      if (wrote) {
        captureReplayPaintedPixel(x, y, vram_[idx],
                                  replay::Rgb8{static_cast<uint8_t>(r),
                                               static_cast<uint8_t>(g),
                                               static_cast<uint8_t>(b)},
                                  false);
      }
      if (wrote && trueColorEnabled_)
        writeModernBlock(
            x, y,
            packModernColor(r, g, b, false,
                            x * static_cast<int>(enhancedSurface_.scale()),
                            y * static_cast<int>(enhancedSurface_.scale())),
            false, blend, blendMode_);
    }
    if (x == v1.x && y == v1.y)
      break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y += sy;
    }
    ++step;
  }
}

void GPU::executeRect() {
  uint32_t c = commandQueue_[0] & 0xFFFFFF;
  uint32_t opcode = commandQueue_[0] >> 24;
  const Color24 commandColor{static_cast<uint8_t>(c),
                             static_cast<uint8_t>(c >> 8),
                             static_cast<uint8_t>(c >> 16)};

  Vertex v;
  v.x = decodeSigned11(commandQueue_[1]) + drawOffsetX_;
  v.y = decodeSigned11(commandQueue_[1] >> 16) + drawOffsetY_;

  int w = 0, h = 0;
  // The opcode controls packet length even when GP1(09)-enabled draw mode
  // disables texture sampling. Keep encoded and effective state separate so
  // UV/CLUT and size words are always consumed from their architectural slots.
  const bool encodedTextured = (opcode & 0x04) != 0;
  const bool isTextured = encodedTextured && !textureDisable_;
  const PGXPValue precision =
      commandPrecisionQueue_.size() > 1 && commandPrecisionQueue_[1].has_value()
          ? *commandPrecisionQueue_[1]
          : PGXPValue{};
  const PGXPResolvedVertex resolved =
      resolvePGXPVertex(precision, commandQueue_[1]);
  const bool projectedPoint =
      replayRecorder_.enabled() && replayRecorder_.config().pgxp &&
      opcode == 0x68u && resolved.hasSignedProjection && resolved.z > 0.0 &&
      resolved.z >= resolved.nearDepth &&
      isAuthenticPGXPCoordinateSource(resolved.xSource) &&
      isAuthenticPGXPCoordinateSource(resolved.ySource);
  if (!projectedPoint) {
    const uint32_t sourceAddress =
        commandSourceQueue_.size() > 1 ? commandSourceQueue_[1] : UINT32_MAX;
    recordPGXPVertex(
        static_cast<uint8_t>(opcode), 1, sourceAddress,
        encodedTextured ? PGXPPrimitive::Sprite : PGXPPrimitive::Rectangle,
        PGXPSemanticDecision::intrinsicScreen2D(), precision, commandQueue_[1]);
  }
  bool isBlend = (opcode & 0x02) != 0;
  bool isRaw = (opcode & 0x01) != 0;
  int sizeWordIdx = encodedTextured ? 3 : 2;

  switch ((opcode >> 3) & 3) {
  case 0: // Variable size
    w = commandQueue_[sizeWordIdx] & 0x3FF;
    h = (commandQueue_[sizeWordIdx] >> 16) & 0x1FF;
    break;
  case 1: // 1x1
    w = 1;
    h = 1;
    break;
  case 2: // 8x8
    w = 8;
    h = 8;
    break;
  case 3: // 16x16
    w = 16;
    h = 16;
    break;
  }

  // Textured sprites carry a UV base + CLUT in word[2] and sample from the
  // current texture page (GP0 0xE1). Untextured rects use the flat command
  // color. Prior to this, textured rects were filled with the command color
  // (0x808080 neutral -> gray), so title/HUD sprites rendered as gray blocks.
  int uBase = 0, vBase = 0;
  uint16_t clut = 0;
  if (encodedTextured) {
    uBase = commandQueue_[2] & 0xFF;
    vBase = (commandQueue_[2] >> 8) & 0xFF;
    clut = (commandQueue_[2] >> 16) & 0xFFFF;
  }

  // A GP0(68) point with an authenticated GTE projection is a 3D point, not
  // an intrinsic screen-space rectangle. Lower its subpixel anchor to a quad
  // only in the modern IR; native rasterization below still writes one pixel.
  // Unproven / screen-space points retain the ordered PaintRect operation.
  bool projectedPointCaptured = false;
  if (projectedPoint) {
    const Vertex anchor = decodeVertex(1u, PGXPSemanticDecision{});
    if (anchor.depthValid && anchor.depth > 0.0 && anchor.authenticPreciseX &&
        anchor.authenticPreciseY) {
      std::array<Vertex, 4> corners{anchor, anchor, anchor, anchor};
      const double horizontalScale = games::crash3::horizontalProjectionScale();
      for (uint32_t corner = 0u; corner < corners.size(); ++corner) {
        const int32_t dx = static_cast<int32_t>(corner & 1u);
        const int32_t dy = static_cast<int32_t>(corner >> 1u);
        auto &vertex = corners[corner];
        vertex.x += dx;
        vertex.y += dy;
        vertex.preciseX += dx * horizontalScale;
        vertex.preciseY += dy;
        vertex.projectiveX += dx * horizontalScale * vertex.depth;
        vertex.projectiveY += dy * vertex.depth;
        vertex.rawProjectiveX += dx * horizontalScale * vertex.depth;
        vertex.rawProjectiveY += dy * vertex.depth;
      }
      projectedPointCaptured = capturePolygonPacket(
          corners.data(), nullptr, &commandColor, 4u, 0u, currentTexpage_,
          PGXPSemanticDecision{}, 0x0fu, false, false, false, false);
      if (projectedPointCaptured)
        metrics::count("host.gpu.projected_point_captured");
    }
  }
  const bool capturePoint =
      replayRecorder_.enabled() && opcode == 0x68 && !projectedPointCaptured;
  if (capturePoint) {
    const bool insideDrawArea = v.x >= drawAreaX1_ && v.x <= drawAreaX2_ &&
                                v.y >= drawAreaY1_ && v.y <= drawAreaY2_ &&
                                v.x >= 0 && v.y >= 0 &&
                                v.x < static_cast<int32_t>(VRAM_WIDTH) &&
                                v.y < static_cast<int32_t>(VRAM_HEIGHT);
    if (!insideDrawArea) {
      replayRecorder_.recordNoOp(0x68);
    } else {
      const Color16 nativeColor =
          packColor(commandColor.r, commandColor.g, commandColor.b, false, v.x,
                    v.y, false);
      replay::PaintRect point;
      point.destination = {v.x, v.y, 1, 1};
      point.nativeColor = nativeColor.raw;
      point.color = {commandColor.r, commandColor.g, commandColor.b};
      point.mask = {setMaskWhileDrawing_, checkMaskBeforeDraw_};
      point.depth.epoch = replayDepthEpoch_;
      point.depth.invalidateOnAccepted = true;
      point.coalescedCommandCount = 1;
      replayRecorder_.recordPaintRect(0x68, point);
    }
  }

  bool modernRectangle = false;
  if (games::mgs::active() && replayRecorder_.enabled() && opcode != 0x68 &&
      w > 0 && h > 0) {
    std::array<Vertex, 4> corners{v, v, v, v};
    const std::array<TexCoord, 4> uv{};
    for (uint32_t i = 0; i < 4; ++i) {
      corners[i].x += (i & 1) ? w : 0;
      corners[i].y += (i & 2) ? h : 0;
    }
    modernRectangle = capturePolygonPacket(
        corners.data(), isTextured ? uv.data() : nullptr, &commandColor, 4,
        clut, currentTexpage_, PGXPSemanticDecision::intrinsicScreen2D(), 0,
        isTextured, false, isRaw, isBlend);
    if (modernRectangle)
      metrics::count("mgs.presentation.rectangle_draw");
  }
  const bool capturePixels =
      replayRecorder_.enabled() && opcode != 0x68 && !modernRectangle;
  if (capturePixels)
    beginReplayPaintCapture();

  for (int dy = 0; dy < h; ++dy) {
    for (int dx = 0; dx < w; ++dx) {
      int px = v.x + dx;
      int py = v.y + dy;
      if (px < drawAreaX1_ || px > drawAreaX2_ || py < drawAreaY1_ ||
          py > drawAreaY2_)
        continue;
      uint32_t idx = (py % VRAM_HEIGHT) * VRAM_WIDTH + (px % VRAM_WIDTH);
      Color16 finalColor;
      Color32 modernColor;
      replay::Rgb8 replayColor;
      bool modernBlend = isBlend;
      bool sourceMask = false;
      uint8_t modernBlendMode = blendMode_;
      if (isTextured) {
        const int sampleU = uBase + (textureRectFlipX_ ? -dx : dx);
        const int sampleV = vBase + (textureRectFlipY_ ? -dy : dy);
        Color16 texel = sampleTexel(sampleU, sampleV, currentTexpage_, clut);
        if (texel.raw == 0)
          continue; // fully-transparent texel
        sourceMask = (texel.raw & 0x8000u) != 0;
        finalColor = isRaw ? texel : modulateTexel(texel, commandColor, px, py);
        modernColor =
            isRaw ? EnhancedSurface::expandNativeColor(texel.raw)
                  : modulateModernTexel(
                        texel, commandColor,
                        px * static_cast<int>(enhancedSurface_.scale()),
                        py * static_cast<int>(enhancedSurface_.scale()));
        if (isRaw) {
          const Color32 expanded =
              EnhancedSurface::expandNativeColor(texel.raw);
          replayColor = {expanded.r, expanded.g, expanded.b};
        } else {
          replayColor = {
              static_cast<uint8_t>(
                  std::min(255, ((texel.raw & 0x1F) * commandColor.r) >> 4)),
              static_cast<uint8_t>(std::min(
                  255, (((texel.raw >> 5) & 0x1F) * commandColor.g) >> 4)),
              static_cast<uint8_t>(std::min(
                  255, (((texel.raw >> 10) & 0x1F) * commandColor.b) >> 4))};
        }
        modernBlend = isBlend && sourceMask;
        modernBlendMode = static_cast<uint8_t>((currentTexpage_ >> 5) & 3u);
        if (modernBlend)
          finalColor = applyBlendMode(finalColor, vram_[idx], modernBlendMode);
      } else {
        finalColor = packColor(commandColor.r, commandColor.g, commandColor.b,
                               false, px, py, false);
        modernColor = packModernColor(
            commandColor.r, commandColor.g, commandColor.b, false,
            px * static_cast<int>(enhancedSurface_.scale()),
            py * static_cast<int>(enhancedSurface_.scale()));
        replayColor = {commandColor.r, commandColor.g, commandColor.b};
        if (isBlend)
          finalColor = applyBlend(finalColor, vram_[idx]);
      }
      const bool wrote = writePixel(px, py, finalColor, !trueColorEnabled_);
      if (wrote)
        captureReplayPaintedPixel(px, py, vram_[idx], replayColor, sourceMask);
      if (wrote && trueColorEnabled_)
        writeModernBlock(px, py, modernColor, sourceMask, modernBlend,
                         modernBlendMode);
      if (capturePoint && wrote)
        replayRecorder_.markNativeWrite(replay::NativePoint{px, py});
    }
  }
  if (capturePixels)
    finishReplayPaintCapture(static_cast<uint8_t>(opcode));
  if (modernRectangle)
    replayRecorder_.markNativeWrite({v.x, v.y, uint32_t(w), uint32_t(h)});
}

// Samples a single 16-bit texel from VRAM for the given texture page and CLUT,
// honouring the E2 texture window. Returns raw==0 for a fully-transparent
// texel (caller skips it). Mirrors the sampler in rasterizeTexturedTriangle.
Color16 GPU::sampleTexel(int u, int v, uint16_t tpage, uint16_t clut) const {
  u &= 0xFF;
  v &= 0xFF;
  if (texWindowMaskX_ != 0 || texWindowOffsetX_ != 0)
    u = (u & ~texWindowMaskX_) | (texWindowOffsetX_ & texWindowMaskX_);
  if (texWindowMaskY_ != 0 || texWindowOffsetY_ != 0)
    v = (v & ~texWindowMaskY_) | (texWindowOffsetY_ & texWindowMaskY_);

  uint32_t tpX = (tpage & 0xF) * 64;
  uint32_t tpY = ((tpage >> 4) & 1) * 256;
  uint32_t depth = (tpage >> 7) & 3;
  uint32_t clutX = (clut & 0x3F) * 16;
  uint32_t clutY = (clut >> 6) & 0x1FF;

  if (depth == 0) { // 4-bit CLUT
    uint32_t tx = tpX + (u / 4), ty = tpY + v;
    uint16_t block =
        vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)].raw;
    uint8_t index = (block >> ((u % 4) * 4)) & 0xF;
    return vram_[(clutY % VRAM_HEIGHT) * VRAM_WIDTH +
                 ((clutX + index) % VRAM_WIDTH)];
  } else if (depth == 1) { // 8-bit CLUT
    uint32_t tx = tpX + (u / 2), ty = tpY + v;
    uint16_t block =
        vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)].raw;
    uint8_t index = (block >> ((u % 2) * 8)) & 0xFF;
    return vram_[(clutY % VRAM_HEIGHT) * VRAM_WIDTH +
                 ((clutX + index) % VRAM_WIDTH)];
  }
  // 15-bit direct
  uint32_t tx = tpX + u, ty = tpY + v;
  return vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)];
}

void GPU::executeClearCache() {
  // Clears the texture cache. NOP for our accurate software rasterizer since we
  // read directly from VRAM.
}

void GPU::executeFillRect() {
  uint32_t color = commandQueue_[0] & 0xFFFFFF;
  uint32_t pos = commandQueue_[1];
  uint32_t size = commandQueue_[2];

  uint32_t x = (pos & 0x3FF) & ~0xFu;
  uint32_t y = (pos >> 16) & 0x1FF;
  // GP0(02h) size semantics differ from the A0h/C0h transfer commands: there a
  // zero dimension means "the full 1024/512", here it means "fill nothing".
  // Width rounds up to a multiple of 16, height is masked to 9 bits (psx-spx,
  // Fill Rectangle in VRAM).
  //
  // Treating a zero height as 512 is what erased the game's texture pages: the
  // per-frame clear at (0,12)/(512,12) became 512 rows tall, wrapped past the
  // bottom of VRAM through the `% VRAM_HEIGHT` below, and wiped y=384..511 --
  // exactly where the two 256x128 texture pages are uploaded. Sprites then
  // sampled transparent texels and drew nothing.
  uint32_t w = size & 0x3FF;
  uint32_t h = (size >> 16) & 0x1FF;
  if (w == 0 || h == 0) {
    if (replayRecorder_.enabled())
      replayRecorder_.recordNoOp(0x02);
    ps1::metrics::count("fill_rect_zero");
    return;
  }
  w = (w + 0xFu) & ~0xFu;

  static int fillCount = 0;
  if (++fillCount <= 10)
    fmt::print(stderr,
               "[GPU] FillRect #{} (sample, first 10): color=0x{:06X} "
               "pos=({},{}) size={}x{}\n",
               fillCount, color, x, y, w, h);
  ps1::metrics::count("fill_rect");
  ps1::metrics::trace(
      "gpu.fast_fill",
      {{"x", x}, {"y", y}, {"w", w}, {"h", h}, {"color", color}});

  // Convert 24-bit RGB to 15-bit
  uint16_t r5 = (color & 0xFF) >> 3;
  uint16_t g5 = ((color >> 8) & 0xFF) >> 3;
  uint16_t b5 = ((color >> 16) & 0xFF) >> 3;
  uint16_t a1 = 0; // Fill commands don't set the mask bit usually

  Color16 c16;
  c16.raw = r5 | (g5 << 5) | (b5 << 10) | (a1 << 15);

  if (replayRecorder_.enabled()) {
    replay::FillRect fill;
    fill.destination = {static_cast<int32_t>(x), static_cast<int32_t>(y), w, h};
    fill.color = {static_cast<uint8_t>(color), static_cast<uint8_t>(color >> 8),
                  static_cast<uint8_t>(color >> 16)};
    fill.nativeColor = c16.raw;
    replayRecorder_.recordFill(0x02, fill);
  }

  for (uint32_t dy = 0; dy < h; ++dy) {
    for (uint32_t dx = 0; dx < w; ++dx) {
      uint32_t px = (x + dx) % VRAM_WIDTH;
      uint32_t py = (y + dy) % VRAM_HEIGHT;
      vram_[py * VRAM_WIDTH + px] = c16;
    }
  }
  if (enhancedSurface_.enabled()) {
    const Color32 modernColor =
        trueColorEnabled_
            ? Color32{static_cast<uint8_t>(color & 0xFFu),
                      static_cast<uint8_t>((color >> 8) & 0xFFu),
                      static_cast<uint8_t>((color >> 16) & 0xFFu), 255}
            : EnhancedSurface::expandNativeColor(c16.raw);
    enhancedSurface_.fillNativeRect(x, y, w, h, modernColor);
  }
  if (replayRecorder_.enabled())
    replayRecorder_.markNativeWrite(
        {static_cast<int32_t>(x), static_cast<int32_t>(y), w, h});
}

void GPU::executeCPUToVRAM() {
  uint32_t pos = commandQueue_[1];
  uint32_t size = commandQueue_[2];
  const VramTransferExtent extent = decodeVramTransferExtent(size);

  static int cpuVramCount = 0;
  if (++cpuVramCount <= 20) {
    fmt::print(stderr,
               "[GPU] CPU->VRAM #{} (sample, first 20): dest=({},{}) "
               "size={}x{} (opcode=0x{:02X})\n",
               cpuVramCount, pos & 0x3FF, (pos >> 16) & 0x1FF, extent.width,
               extent.height, commandQueue_[0] >> 24);
  }
  ps1::metrics::count("cpu_to_vram");

  vramTransfer_.destX = pos & 0x3FF;
  vramTransfer_.destY = (pos >> 16) & 0x1FF;

  const uint32_t w = extent.width;
  const uint32_t h = extent.height;

  ps1::metrics::trace("gpu.cpu_to_vram", {{"x", vramTransfer_.destX},
                                          {"y", vramTransfer_.destY},
                                          {"w", w},
                                          {"h", h}});

  vramTransfer_.width = w;
  vramTransfer_.height = h;

  vramTransfer_.currX = vramTransfer_.destX;
  vramTransfer_.currY = vramTransfer_.destY;

  uint32_t pixels = w * h;
  vramTransfer_.transferWordsRemaining = (pixels + 1) / 2;
  vramTransfer_.transferPixelsRemaining = pixels;
  if (replayRecorder_.enabled()) {
    replayRecorder_.beginCpuUpload(
        static_cast<uint8_t>(commandQueue_[0] >> 24),
        activeReplayPacketSequence_,
        {static_cast<int32_t>(vramTransfer_.destX),
         static_cast<int32_t>(vramTransfer_.destY), w, h},
        {setMaskWhileDrawing_, checkMaskBeforeDraw_});
  }
  vramTransfer_.isWritingToVRAM = true;
}

void GPU::executeCopyVRAM() {
  uint32_t srcPos = commandQueue_[1];
  uint32_t dstPos = commandQueue_[2];
  uint32_t size = commandQueue_[3];

  uint32_t sx = srcPos & 0x3FF;
  uint32_t sy = (srcPos >> 16) & 0x1FF;
  uint32_t dx = dstPos & 0x3FF;
  uint32_t dy = (dstPos >> 16) & 0x1FF;

  const VramTransferExtent extent = decodeVramTransferExtent(size);
  const uint32_t w = extent.width;
  const uint32_t h = extent.height;

  ps1::metrics::trace("gpu.vram_copy", {{"src_x", sx},
                                        {"src_y", sy},
                                        {"dst_x", dx},
                                        {"dst_y", dy},
                                        {"w", w},
                                        {"h", h}});

  // With no forced mask bit, copying a rectangle onto the same wrapped VRAM
  // origin cannot change any observable plane. In particular, do not mirror
  // it through EnhancedSurface (which would invalidate depth) or advance the
  // CrashReplay native tile revision. E6 bit 0 makes the command a real write.
  if (sx == dx && sy == dy && !setMaskWhileDrawing_) {
    if (replayRecorder_.enabled())
      replayRecorder_.recordNoOp(static_cast<uint8_t>(commandQueue_[0] >> 24));
    return;
  }

  if (replayRecorder_.enabled()) {
    replay::CopyRect copy;
    copy.source = {static_cast<int32_t>(sx), static_cast<int32_t>(sy)};
    copy.destination = {static_cast<int32_t>(dx), static_cast<int32_t>(dy)};
    copy.width = w;
    copy.height = h;
    copy.mask = {setMaskWhileDrawing_, checkMaskBeforeDraw_};
    replayRecorder_.recordCopy(static_cast<uint8_t>(commandQueue_[0] >> 24),
                               copy, replayNativeVram());
  }

  // The PS1 copies backwards when the source begins to the left of the
  // destination. This preserves overlapping horizontal moves. Mask rules also
  // apply to VRAM copies, just as they do to CPU uploads and primitives.
  const bool backwards =
      sx < dx || ((sx + w - 1) % VRAM_WIDTH) < ((dx + w - 1) % VRAM_WIDTH);
  bool acceptedAnyPixel = false;
  for (uint32_t py = 0; py < h; ++py) {
    for (uint32_t step = 0; step < w; ++step) {
      const uint32_t px = backwards ? (w - 1 - step) : step;
      uint32_t s_idx =
          ((sy + py) % VRAM_HEIGHT) * VRAM_WIDTH + ((sx + px) % VRAM_WIDTH);
      uint32_t d_idx =
          ((dy + py) % VRAM_HEIGHT) * VRAM_WIDTH + ((dx + px) % VRAM_WIDTH);
      const Color16 source = vram_[s_idx];
      if (!checkMaskBeforeDraw_ || (vram_[d_idx].raw & 0x8000u) == 0) {
        acceptedAnyPixel = true;
        vram_[d_idx].raw = source.raw | (setMaskWhileDrawing_ ? 0x8000u : 0u);
        if (enhancedSurface_.enabled())
          enhancedSurface_.copyNativePixel(
              (sx + px) % VRAM_WIDTH, (sy + py) % VRAM_HEIGHT,
              (dx + px) % VRAM_WIDTH, (dy + py) % VRAM_HEIGHT,
              setMaskWhileDrawing_);
      }
    }
  }
  if (replayRecorder_.enabled() && acceptedAnyPixel) {
    replayRecorder_.markNativeWrite(
        {static_cast<int32_t>(dx), static_cast<int32_t>(dy), w, h});
  }
}

void GPU::executeVRAMToCPU() {
  uint32_t pos = commandQueue_[1];
  uint32_t size = commandQueue_[2];

  vramTransfer_.srcX = pos & 0x3FF;
  vramTransfer_.srcY = (pos >> 16) & 0x1FF;

  const VramTransferExtent extent = decodeVramTransferExtent(size);
  const uint32_t w = extent.width;
  const uint32_t h = extent.height;

  vramTransfer_.width = w;
  vramTransfer_.height = h;

  vramTransfer_.currX = vramTransfer_.srcX;
  vramTransfer_.currY = vramTransfer_.srcY;

  uint32_t pixels = w * h;
  vramTransfer_.transferWordsRemaining = (pixels + 1) / 2;
  vramTransfer_.isReadingFromVRAM = true;
}

// Gouraud Shading Polygons

static Color24 extractColor24(uint32_t word) {
  Color24 c;
  c.r = word & 0xFF;
  c.g = (word >> 8) & 0xFF;
  c.b = (word >> 16) & 0xFF;
  return c;
}

void GPU::executeGouraudPoly3() {
  // Word layout: c0+cmd, v0, c1, v1, c2, v2
  const PGXPSemanticDecision semantic = classifyPolygonPacket({1, 3, 5, 0}, 3);
  Color24 c[3];
  Vertex v[3];
  c[0] = extractColor24(commandQueue_[0]);
  v[0] = decodeVertex(1, semantic);
  c[1] = extractColor24(commandQueue_[2]);
  v[1] = decodeVertex(3, semantic);
  c[2] = extractColor24(commandQueue_[4]);
  v[2] = decodeVertex(5, semantic);

  uint32_t opcode = commandQueue_[0] >> 24;
  bool isBlend = (opcode & 2) != 0;

  const uint8_t validMask = vertexPrecisionMask(v, 3);
  const bool precise =
      semantic.requiresXYZ() && shouldUseEnhancedRaster(v, 3, validMask);
  const PolygonRoute route = precise ? PolygonRoute::Precise3D
                             : semantic.requiresXYZ()
                                 ? PolygonRoute::Compatibility3D
                                 : PolygonRoute::Screen2D;
  if (!precise && validMask == 0x7u && enhancedRasterEnabled_)
    ps1::metrics::count("gpu.modern_setup_fallback");
  notePrimitivePath(route, semantic, validMask, 3, 1);
  const bool replayCaptured =
      capturePolygonPacket(v, nullptr, c, 3, 0, currentTexpage_, semantic,
                           validMask, false, true, false, isBlend);
  if (precise) {
    const ModernDepthAction depthAction =
        prepareModernDepthPacket(v, 3, isBlend);
    rasterizeEnhancedSurfaceTriangle(v[0], v[1], v[2], {}, {}, {}, c[0], c[1],
                                     c[2], 0, 0, false, true, false, isBlend,
                                     {}, depthAction);
  }
  if (shouldRasterizeNativePolygon(semantic, replayCaptured)) {
    rasterizeGouraudTriangle(v[0], v[1], v[2], c[0], c[1], c[2], isBlend,
                             precise);
    markCapturedPolygonNativeWrite(v, 3);
  }
}

void GPU::executeGouraudPoly4() {
  // Word layout: c0+cmd, v0, c1, v1, c2, v2, c3, v3
  const PGXPSemanticDecision semantic = classifyPolygonPacket({1, 3, 5, 7}, 4);
  Color24 c[4];
  Vertex v[4];
  c[0] = extractColor24(commandQueue_[0]);
  v[0] = decodeVertex(1, semantic);
  c[1] = extractColor24(commandQueue_[2]);
  v[1] = decodeVertex(3, semantic);
  c[2] = extractColor24(commandQueue_[4]);
  v[2] = decodeVertex(5, semantic);
  c[3] = extractColor24(commandQueue_[6]);
  v[3] = decodeVertex(7, semantic);

  uint32_t opcode = commandQueue_[0] >> 24;
  bool isBlend = (opcode & 2) != 0;

  const uint8_t validMask = vertexPrecisionMask(v, 4);
  const bool precise =
      semantic.requiresXYZ() && shouldUseEnhancedRaster(v, 4, validMask);
  const PolygonRoute route = precise ? PolygonRoute::Precise3D
                             : semantic.requiresXYZ()
                                 ? PolygonRoute::Compatibility3D
                                 : PolygonRoute::Screen2D;
  if (!precise && validMask == 0xFu && enhancedRasterEnabled_)
    ps1::metrics::count("gpu.modern_setup_fallback");
  notePrimitivePath(route, semantic, validMask, 4, 2);
  const bool replayCaptured =
      capturePolygonPacket(v, nullptr, c, 4, 0, currentTexpage_, semantic,
                           validMask, false, true, false, isBlend);
  const ModernDepthAction depthAction =
      precise ? prepareModernDepthPacket(v, 4, isBlend)
              : ModernDepthAction::Preserve;
  if (precise) {
    rasterizeEnhancedSurfaceTriangle(v[0], v[1], v[2], {}, {}, {}, c[0], c[1],
                                     c[2], 0, 0, false, true, false, isBlend,
                                     {}, depthAction);
    rasterizeEnhancedSurfaceTriangle(v[1], v[2], v[3], {}, {}, {}, c[1], c[2],
                                     c[3], 0, 0, false, true, false, isBlend,
                                     {}, depthAction);
  }
  if (shouldRasterizeNativePolygon(semantic, replayCaptured)) {
    rasterizeGouraudTriangle(v[0], v[1], v[2], c[0], c[1], c[2], isBlend,
                             precise);
    rasterizeGouraudTriangle(v[1], v[2], v[3], c[1], c[2], c[3], isBlend,
                             precise);
    markCapturedPolygonNativeWrite(v, 4);
  }
}

void GPU::executeGouraudTexturedPoly3() {
  // Word layout: c0+cmd, v0, uv0+clut, c1, v1, uv1+tpage, c2, v2, uv2
  const PGXPSemanticDecision semantic = classifyPolygonPacket({1, 4, 7, 0}, 3);
  Color24 c[3];
  Vertex v[3];
  TexCoord t[3];
  uint16_t clut, tpage;

  c[0] = extractColor24(commandQueue_[0]);
  v[0] = decodeVertex(1, semantic);
  t[0].u = commandQueue_[2] & 0xFF;
  t[0].v = (commandQueue_[2] >> 8) & 0xFF;
  clut = (commandQueue_[2] >> 16) & 0xFFFF;

  c[1] = extractColor24(commandQueue_[3]);
  v[1] = decodeVertex(4, semantic);
  t[1].u = commandQueue_[5] & 0xFF;
  t[1].v = (commandQueue_[5] >> 8) & 0xFF;
  tpage = (commandQueue_[5] >> 16) & 0xFFFF;
  latchTexturePage(tpage);

  c[2] = extractColor24(commandQueue_[6]);
  v[2] = decodeVertex(7, semantic);
  t[2].u = commandQueue_[8] & 0xFF;
  t[2].v = (commandQueue_[8] >> 8) & 0xFF;

  uint32_t opcode = commandQueue_[0] >> 24;
  bool isRaw = (opcode & 1) != 0;
  bool isBlend = (opcode & 2) != 0;
  const bool effectiveTextured = !textureDisable_;

  const uint8_t validMask = vertexPrecisionMask(v, 3);
  const bool precise =
      semantic.requiresXYZ() && shouldUseEnhancedRaster(v, 3, validMask);
  const PolygonRoute route = precise ? PolygonRoute::Precise3D
                             : semantic.requiresXYZ()
                                 ? PolygonRoute::Compatibility3D
                                 : PolygonRoute::Screen2D;
  if (!precise && validMask == 0x7u && enhancedRasterEnabled_)
    ps1::metrics::count("gpu.modern_setup_fallback");
  notePrimitivePath(route, semantic, validMask, 3, 1);
  const bool replayCaptured =
      capturePolygonPacket(v, t, c, 3, clut, tpage, semantic, validMask, true,
                           true, isRaw, isBlend);
  if (precise) {
    const ModernDepthAction depthAction =
        prepareModernDepthPacket(v, 3, isBlend);
    const ModernUVDomain uvDomain =
        effectiveTextured ? prepareModernUVDomainForPacket(
                                v, t, 3, false, enhancedSurface_.scale())
                          : ModernUVDomain{};
    rasterizeEnhancedSurfaceTriangle(v[0], v[1], v[2], t[0], t[1], t[2], c[0],
                                     c[1], c[2], clut, tpage, effectiveTextured,
                                     true, effectiveTextured && isRaw, isBlend,
                                     uvDomain, depthAction);
  }
  if (shouldRasterizeNativePolygon(semantic, replayCaptured)) {
    if (effectiveTextured) {
      rasterizeGouraudTexturedTriangle(v[0], v[1], v[2], t[0], t[1], t[2],
                                       c[0], c[1], c[2], clut, tpage, isRaw,
                                       isBlend, precise);
    } else {
      rasterizeGouraudTriangle(v[0], v[1], v[2], c[0], c[1], c[2], isBlend,
                               precise);
    }
    markCapturedPolygonNativeWrite(v, 3);
  }
}

void GPU::executeGouraudTexturedPoly4() {
  // Word layout: c0+cmd, v0, uv0+clut, c1, v1, uv1+tpage, c2, v2, uv2, c3,
  // v3, uv3
  const PGXPSemanticDecision semantic = classifyPolygonPacket({1, 4, 7, 10}, 4);
  Color24 c[4];
  Vertex v[4];
  TexCoord t[4];
  uint16_t clut, tpage;

  c[0] = extractColor24(commandQueue_[0]);
  v[0] = decodeVertex(1, semantic);
  t[0].u = commandQueue_[2] & 0xFF;
  t[0].v = (commandQueue_[2] >> 8) & 0xFF;
  clut = (commandQueue_[2] >> 16) & 0xFFFF;

  c[1] = extractColor24(commandQueue_[3]);
  v[1] = decodeVertex(4, semantic);
  t[1].u = commandQueue_[5] & 0xFF;
  t[1].v = (commandQueue_[5] >> 8) & 0xFF;
  tpage = (commandQueue_[5] >> 16) & 0xFFFF;
  latchTexturePage(tpage);

  c[2] = extractColor24(commandQueue_[6]);
  v[2] = decodeVertex(7, semantic);
  t[2].u = commandQueue_[8] & 0xFF;
  t[2].v = (commandQueue_[8] >> 8) & 0xFF;

  c[3] = extractColor24(commandQueue_[9]);
  v[3] = decodeVertex(10, semantic);
  t[3].u = commandQueue_[11] & 0xFF;
  t[3].v = (commandQueue_[11] >> 8) & 0xFF;

  uint32_t opcode = commandQueue_[0] >> 24;
  bool isRaw = (opcode & 1) != 0;
  bool isBlend = (opcode & 2) != 0;
  const bool effectiveTextured = !textureDisable_;

  const uint8_t validMask = vertexPrecisionMask(v, 4);
  const bool precise =
      semantic.requiresXYZ() && shouldUseEnhancedRaster(v, 4, validMask);
  const PolygonRoute route = precise ? PolygonRoute::Precise3D
                             : semantic.requiresXYZ()
                                 ? PolygonRoute::Compatibility3D
                                 : PolygonRoute::Screen2D;
  if (!precise && validMask == 0xFu && enhancedRasterEnabled_)
    ps1::metrics::count("gpu.modern_setup_fallback");
  notePrimitivePath(route, semantic, validMask, 4, 2);
  const bool replayCaptured =
      capturePolygonPacket(v, t, c, 4, clut, tpage, semantic, validMask, true,
                           true, isRaw, isBlend);
  const ModernDepthAction depthAction =
      precise ? prepareModernDepthPacket(v, 4, isBlend)
              : ModernDepthAction::Preserve;
  const ModernUVDomain uvDomain =
      precise && effectiveTextured
          ? prepareModernUVDomainForPacket(v, t, 4, true,
                                           enhancedSurface_.scale())
          : ModernUVDomain{};
  if (precise) {
    rasterizeEnhancedSurfaceTriangle(v[0], v[1], v[2], t[0], t[1], t[2], c[0],
                                     c[1], c[2], clut, tpage, effectiveTextured,
                                     true, effectiveTextured && isRaw, isBlend,
                                     uvDomain, depthAction);
    rasterizeEnhancedSurfaceTriangle(v[1], v[2], v[3], t[1], t[2], t[3], c[1],
                                     c[2], c[3], clut, tpage, effectiveTextured,
                                     true, effectiveTextured && isRaw, isBlend,
                                     uvDomain, depthAction);
  }
  if (shouldRasterizeNativePolygon(semantic, replayCaptured)) {
    if (effectiveTextured) {
      rasterizeGouraudTexturedTriangle(v[0], v[1], v[2], t[0], t[1], t[2],
                                       c[0], c[1], c[2], clut, tpage, isRaw,
                                       isBlend, precise);
      rasterizeGouraudTexturedTriangle(v[1], v[2], v[3], t[1], t[2], t[3],
                                       c[1], c[2], c[3], clut, tpage, isRaw,
                                       isBlend, precise);
    } else {
      rasterizeGouraudTriangle(v[0], v[1], v[2], c[0], c[1], c[2], isBlend,
                               precise);
      rasterizeGouraudTriangle(v[1], v[2], v[3], c[1], c[2], c[3], isBlend,
                               precise);
    }
    markCapturedPolygonNativeWrite(v, 4);
  }
}

void GPU::rasterizeGouraudTriangle(Vertex v0, Vertex v1, Vertex v2, Color24 c0,
                                   Color24 c1, Color24 c2, bool blend,
                                   bool precisePacket) {
  if (!withinPolygonDrawingLimits(v0, v1, v2)) {
    noteBoundsReject();
    return;
  }
  int area = edgeFunction(v0, v1, v2);
  if (area == 0)
    return;
  if (area < 0) {
    std::swap(v1, v2);
    std::swap(c1, c2);
    area = -area;
  }
  const bool edge0Inclusive = isTopLeftEdge(v1, v2);
  const bool edge1Inclusive = isTopLeftEdge(v2, v0);
  const bool edge2Inclusive = isTopLeftEdge(v0, v1);

  int minX = std::max(drawAreaX1_, std::min({v0.x, v1.x, v2.x}));
  int minY = std::max(drawAreaY1_, std::min({v0.y, v1.y, v2.y}));
  int maxX = std::min(drawAreaX2_, std::max({v0.x, v1.x, v2.x}));
  int maxY = std::min(drawAreaY2_, std::max({v0.y, v1.y, v2.y}));

  const Vertex rasterOrigin{minX, minY};
  int rowW0 = edgeFunction(v1, v2, rasterOrigin);
  int rowW1 = edgeFunction(v2, v0, rasterOrigin);
  int rowW2 = edgeFunction(v0, v1, rasterOrigin);
  const int stepX0 = v2.y - v1.y;
  const int stepX1 = v0.y - v2.y;
  const int stepX2 = v1.y - v0.y;
  const int stepY0 = v1.x - v2.x;
  const int stepY1 = v2.x - v0.x;
  const int stepY2 = v0.x - v1.x;
  AffineAttributeWalker rowR(
      affineAttributeNumerator(rowW0, rowW1, rowW2, c0.r, c1.r, c2.r),
      affineAttributeNumerator(stepX0, stepX1, stepX2, c0.r, c1.r, c2.r),
      affineAttributeNumerator(stepY0, stepY1, stepY2, c0.r, c1.r, c2.r), area);
  AffineAttributeWalker rowG(
      affineAttributeNumerator(rowW0, rowW1, rowW2, c0.g, c1.g, c2.g),
      affineAttributeNumerator(stepX0, stepX1, stepX2, c0.g, c1.g, c2.g),
      affineAttributeNumerator(stepY0, stepY1, stepY2, c0.g, c1.g, c2.g), area);
  AffineAttributeWalker rowB(
      affineAttributeNumerator(rowW0, rowW1, rowW2, c0.b, c1.b, c2.b),
      affineAttributeNumerator(stepX0, stepX1, stepX2, c0.b, c1.b, c2.b),
      affineAttributeNumerator(stepY0, stepY1, stepY2, c0.b, c1.b, c2.b), area);
  for (int y = minY; y <= maxY; ++y) {
    int w0 = rowW0;
    int w1 = rowW1;
    int w2 = rowW2;
    // The intersection of a convex triangle and one scanline is a single
    // contiguous span. Walk cheap edge equations through the empty prefix,
    // then advance the three exact attribute walkers to the first sample in
    // one quotient/remainder step instead of updating them outside coverage.
    int x = minX;
    while (x <= maxX &&
           !(edgeCovers(w0, edge0Inclusive) && edgeCovers(w1, edge1Inclusive) &&
             edgeCovers(w2, edge2Inclusive))) {
      w0 += stepX0;
      w1 += stepX1;
      w2 += stepX2;
      ++x;
    }
    if (x <= maxX) {
      AffineAttributeWalker rWalker = rowR;
      AffineAttributeWalker gWalker = rowG;
      AffineAttributeWalker bWalker = rowB;
      const int32_t skipped = x - minX;
      rWalker.advanceXBy(skipped);
      gWalker.advanceXBy(skipped);
      bWalker.advanceXBy(skipped);
      for (; x <= maxX; ++x) {
        if (!(edgeCovers(w0, edge0Inclusive) &&
              edgeCovers(w1, edge1Inclusive) && edgeCovers(w2, edge2Inclusive)))
          break;
        // Interpolate RGB per-vertex using barycentric coordinates
        int r = rWalker.value();
        int g = gWalker.value();
        int b = bWalker.value();

        // Clamp
        r = std::clamp(r, 0, 255);
        g = std::clamp(g, 0, 255);
        b = std::clamp(b, 0, 255);

        Color16 finalColor = packColor(r, g, b, false, x, y, ditherEnable_);

        uint32_t idx = (y % VRAM_HEIGHT) * VRAM_WIDTH + (x % VRAM_WIDTH);
        if (blend) {
          finalColor = applyBlend(finalColor, vram_[idx]);
        }
        const bool wrote =
            writePixel(x, y, finalColor, !precisePacket && !trueColorEnabled_);
        if (wrote && !precisePacket && trueColorEnabled_)
          writeModernBlock(
              x, y,
              packModernColor(r, g, b, false,
                              x * static_cast<int>(enhancedSurface_.scale()),
                              y * static_cast<int>(enhancedSurface_.scale())),
              false, blend, blendMode_);
        w0 += stepX0;
        w1 += stepX1;
        w2 += stepX2;
        rWalker.advanceX();
        gWalker.advanceX();
        bWalker.advanceX();
      }
    }
    rowW0 += stepY0;
    rowW1 += stepY1;
    rowW2 += stepY2;
    rowR.advanceY();
    rowG.advanceY();
    rowB.advanceY();
  }
}

void GPU::rasterizeGouraudTexturedTriangle(Vertex v0, Vertex v1, Vertex v2,
                                           TexCoord t0, TexCoord t1,
                                           TexCoord t2, Color24 c0, Color24 c1,
                                           Color24 c2, uint16_t clut,
                                           uint16_t tpage, bool isRaw,
                                           bool blend, bool precisePacket) {
  if (!withinPolygonDrawingLimits(v0, v1, v2)) {
    noteBoundsReject();
    return;
  }
  int area = edgeFunction(v0, v1, v2);
  if (area == 0)
    return;
  if (area < 0) {
    std::swap(v1, v2);
    std::swap(t1, t2);
    std::swap(c1, c2);
    area = -area;
  }
  const bool edge0Inclusive = isTopLeftEdge(v1, v2);
  const bool edge1Inclusive = isTopLeftEdge(v2, v0);
  const bool edge2Inclusive = isTopLeftEdge(v0, v1);

  int minX = std::max(drawAreaX1_, std::min({v0.x, v1.x, v2.x}));
  int minY = std::max(drawAreaY1_, std::min({v0.y, v1.y, v2.y}));
  int maxX = std::min(drawAreaX2_, std::max({v0.x, v1.x, v2.x}));
  int maxY = std::min(drawAreaY2_, std::max({v0.y, v1.y, v2.y}));

  uint32_t tpX = (tpage & 0xF) * 64;
  uint32_t tpY = ((tpage >> 4) & 1) * 256;
  uint32_t depth = (tpage >> 7) & 3;

  uint32_t clutX = (clut & 0x3F) * 16;
  uint32_t clutY = (clut >> 6) & 0x1FF;

  const Vertex rasterOrigin{minX, minY};
  int rowW0 = edgeFunction(v1, v2, rasterOrigin);
  int rowW1 = edgeFunction(v2, v0, rasterOrigin);
  int rowW2 = edgeFunction(v0, v1, rasterOrigin);
  const int stepX0 = v2.y - v1.y;
  const int stepX1 = v0.y - v2.y;
  const int stepX2 = v1.y - v0.y;
  const int stepY0 = v1.x - v2.x;
  const int stepY1 = v2.x - v0.x;
  const int stepY2 = v0.x - v1.x;
  AffineAttributeWalker rowU(
      affineAttributeNumerator(rowW0, rowW1, rowW2, t0.u, t1.u, t2.u),
      affineAttributeNumerator(stepX0, stepX1, stepX2, t0.u, t1.u, t2.u),
      affineAttributeNumerator(stepY0, stepY1, stepY2, t0.u, t1.u, t2.u), area);
  AffineAttributeWalker rowV(
      affineAttributeNumerator(rowW0, rowW1, rowW2, t0.v, t1.v, t2.v),
      affineAttributeNumerator(stepX0, stepX1, stepX2, t0.v, t1.v, t2.v),
      affineAttributeNumerator(stepY0, stepY1, stepY2, t0.v, t1.v, t2.v), area);
  AffineAttributeWalker rowR(
      affineAttributeNumerator(rowW0, rowW1, rowW2, c0.r, c1.r, c2.r),
      affineAttributeNumerator(stepX0, stepX1, stepX2, c0.r, c1.r, c2.r),
      affineAttributeNumerator(stepY0, stepY1, stepY2, c0.r, c1.r, c2.r), area);
  AffineAttributeWalker rowG(
      affineAttributeNumerator(rowW0, rowW1, rowW2, c0.g, c1.g, c2.g),
      affineAttributeNumerator(stepX0, stepX1, stepX2, c0.g, c1.g, c2.g),
      affineAttributeNumerator(stepY0, stepY1, stepY2, c0.g, c1.g, c2.g), area);
  AffineAttributeWalker rowB(
      affineAttributeNumerator(rowW0, rowW1, rowW2, c0.b, c1.b, c2.b),
      affineAttributeNumerator(stepX0, stepX1, stepX2, c0.b, c1.b, c2.b),
      affineAttributeNumerator(stepY0, stepY1, stepY2, c0.b, c1.b, c2.b), area);
  for (int y = minY; y <= maxY; ++y) {
    int w0 = rowW0;
    int w1 = rowW1;
    int w2 = rowW2;
    // As above, only edge equations cross the empty bounding-box prefix. The
    // five exact UV/RGB walkers jump to the first covered sample as a batch.
    int x = minX;
    while (x <= maxX &&
           !(edgeCovers(w0, edge0Inclusive) && edgeCovers(w1, edge1Inclusive) &&
             edgeCovers(w2, edge2Inclusive))) {
      w0 += stepX0;
      w1 += stepX1;
      w2 += stepX2;
      ++x;
    }
    if (x <= maxX) {
      AffineAttributeWalker uWalker = rowU;
      AffineAttributeWalker vWalker = rowV;
      AffineAttributeWalker rWalker = rowR;
      AffineAttributeWalker gWalker = rowG;
      AffineAttributeWalker bWalker = rowB;
      const int32_t skipped = x - minX;
      uWalker.advanceXBy(skipped);
      vWalker.advanceXBy(skipped);
      rWalker.advanceXBy(skipped);
      gWalker.advanceXBy(skipped);
      bWalker.advanceXBy(skipped);
      for (; x <= maxX; ++x) {
        if (!(edgeCovers(w0, edge0Inclusive) &&
              edgeCovers(w1, edge1Inclusive) && edgeCovers(w2, edge2Inclusive)))
          break;
        int u = uWalker.value();
        int v = vWalker.value();

        // Apply Texture Window
        if (texWindowMaskX_ != 0 || texWindowOffsetX_ != 0) {
          u = (u & ~(texWindowMaskX_)) | (texWindowOffsetX_ & texWindowMaskX_);
        }
        if (texWindowMaskY_ != 0 || texWindowOffsetY_ != 0) {
          v = (v & ~(texWindowMaskY_)) | (texWindowOffsetY_ & texWindowMaskY_);
        }

        // Texture lookup (same as flat-textured)
        Color16 texColor;
        if (depth == 0) { // 4-bit
          uint32_t tx = tpX + (u / 4);
          uint32_t ty = tpY + v;
          uint16_t block =
              vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)].raw;
          uint8_t index = (block >> ((u % 4) * 4)) & 0xF;
          texColor = vram_[(clutY % VRAM_HEIGHT) * VRAM_WIDTH +
                           ((clutX + index) % VRAM_WIDTH)];
        } else if (depth == 1) { // 8-bit
          uint32_t tx = tpX + (u / 2);
          uint32_t ty = tpY + v;
          uint16_t block =
              vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)].raw;
          uint8_t index = (block >> ((u % 2) * 8)) & 0xFF;
          texColor = vram_[(clutY % VRAM_HEIGHT) * VRAM_WIDTH +
                           ((clutX + index) % VRAM_WIDTH)];
        } else { // 15-bit
          uint32_t tx = tpX + u;
          uint32_t ty = tpY + v;
          texColor = vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)];
        }

        if (texColor.raw != 0) {
          uint32_t idx = (y % VRAM_HEIGHT) * VRAM_WIDTH + (x % VRAM_WIDTH);

          const Color24 shade{
              static_cast<uint8_t>(std::clamp(rWalker.value(), 0, 255)),
              static_cast<uint8_t>(std::clamp(gWalker.value(), 0, 255)),
              static_cast<uint8_t>(std::clamp(bWalker.value(), 0, 255))};
          Color16 finalColor =
              isRaw ? texColor : modulateTexel(texColor, shade, x, y);
          const bool sourceMask = (texColor.raw & 0x8000u) != 0;
          const bool modernBlend = blend && sourceMask;
          const uint8_t modernBlendMode =
              static_cast<uint8_t>((tpage >> 5) & 3u);
          if (modernBlend)
            finalColor =
                applyBlendMode(finalColor, vram_[idx], modernBlendMode);
          const bool wrote = writePixel(x, y, finalColor,
                                        !precisePacket && !trueColorEnabled_);
          if (wrote && !precisePacket && trueColorEnabled_) {
            const Color32 modernColor =
                isRaw ? EnhancedSurface::expandNativeColor(texColor.raw)
                      : modulateModernTexel(
                            texColor, shade,
                            x * static_cast<int>(enhancedSurface_.scale()),
                            y * static_cast<int>(enhancedSurface_.scale()));
            writeModernBlock(x, y, modernColor, sourceMask, modernBlend,
                             modernBlendMode);
          }
        }
        w0 += stepX0;
        w1 += stepX1;
        w2 += stepX2;
        uWalker.advanceX();
        vWalker.advanceX();
        rWalker.advanceX();
        gWalker.advanceX();
        bWalker.advanceX();
      }
    }
    rowW0 += stepY0;
    rowW1 += stepY1;
    rowW2 += stepY2;
    rowU.advanceY();
    rowV.advanceY();
    rowR.advanceY();
    rowG.advanceY();
    rowB.advanceY();
  }
}

void GPU::publishMetrics() const {
  // ps1::metrics::count() accumulates, so publish the delta since the last
  // call rather than the whole histogram: main_host calls this from both
  // shutdown paths, and re-publishing the totals doubled every gp0.op.XX.
  char name[16];
  for (int op = 0; op < 256; ++op) {
    const uint64_t n = gp0Hist_[op].load(std::memory_order_relaxed);
    const uint64_t delta = n - gp0HistPublished_[op];
    if (delta == 0)
      continue;
    gp0HistPublished_[op] = n;
    std::snprintf(name, sizeof(name), "gp0.op.%02X", op);
    ps1::metrics::count(name, delta);
  }
  const uint64_t words = gp0Words_.load(std::memory_order_relaxed);
  ps1::metrics::count("gp0.words", words - gp0WordsPublished_);
  gp0WordsPublished_ = words;

  const uint64_t pgxpHits = pgxpVertexHits_.load(std::memory_order_relaxed);
  const uint64_t pgxpMisses = pgxpVertexMisses_.load(std::memory_order_relaxed);
  ps1::metrics::count("pgxp.vertex_hit", pgxpHits - pgxpVertexHitsPublished_);
  ps1::metrics::count("pgxp.vertex_miss",
                      pgxpMisses - pgxpVertexMissesPublished_);
  pgxpVertexHitsPublished_ = pgxpHits;
  pgxpVertexMissesPublished_ = pgxpMisses;
}

} // namespace ps1::gpu
