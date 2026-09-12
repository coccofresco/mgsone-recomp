#pragma once
/**
 * @file gpu.h
 * @brief PS1 GPU command processor, 1 MB VRAM, and host renderer surfaces.
 *
 * `ps1::gpu::GPU` ingests the GP0 (rendering) and GP1 (display control)
 * command streams written by the game (directly or via DMA channel 2), keeps
 * the 1024x512 VRAM in sync, and exposes the active display region for the
 * SDL2/Vulkan host to upload as a native-format texture.
 *
 * Threading: the game thread writes GP0/GP1 and DMA; the SDL render thread
 * reads `getDisplayVRAM()` after `snapshotDisplayBuffer()` is called from
 * the VBlank tick on the game thread.  Internal state is single-writer.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <runtime/gpu/enhanced_surface.h>
#include <runtime/gpu/modern_replay_recorder.h>
#include <runtime/gpu/native_color.h>
#include <runtime/pgxp.h>
#include <unordered_map>
#include <vector>

namespace ps1 {
class Memory;
}

namespace ps1::gpu {

// Exact linked-list transport attached to one GP0 word. Addresses are the
// physical DMA values; exact disambiguates the valid zero address from direct
// MMIO or non-OT HLE submission. Commands may legally cross node boundaries,
// so this is queued per word rather than guessed once per packet.
struct GP0DmaWordProvenance {
  uint32_t listStart = 0;
  uint32_t nodeAddress = 0;
  uint32_t traversalOrdinal = 0;
  // Last zero-word link visited before this packet. In a canonical reverse
  // ordering table this is the exact bucket which introduced the primitive
  // chain. The transport layer deliberately records the address without
  // assigning it a depth meaning; game/generic presentation policy may do so
  // later. Zero is a valid PS1 RAM address, hence the separate exact bit.
  uint32_t orderingTableAnchorAddress = 0;
  bool orderingTableAnchorExact = false;
  bool exact = false;
};

// Represents a 2D point/vertex
struct Vertex {
  int32_t x;
  int32_t y;
  double preciseX = 0.0;
  double preciseY = 0.0;
  double depth = 0.0;
  // Homogeneous screen coordinates retained by PGXP before the GTE divider
  // saturates.  projectiveX / depth and projectiveY / depth are the modern
  // screen position.  Keeping the numerators finite also represents vertices
  // on or behind the camera, which the companion renderer clips without ever
  // changing authoritative PS1 packet/raster behaviour.
  double projectiveX = 0.0;
  double projectiveY = 0.0;
  // Unmodified homogeneous numerators from the GTE sidecar. projectiveX/Y
  // above may deliberately be reconciled to packed SXY for donor rendering;
  // these retain the genuine source needed by coverage and future clipping.
  double rawProjectiveX = 0.0;
  double rawProjectiveY = 0.0;
  double nearDepth = 0.0;
  uint64_t projectiveId = 0;
  uint64_t projectiveContext = 0;
  uint64_t projectiveGameProfileCallsite = 0;
  uint64_t projectiveTransformValueFingerprint = 0;
  uint64_t projectiveTransformLineage = 0;
  uint64_t projectiveModelVertexSource = 0;
  uint64_t projectiveModelValueFlowSourceFingerprint = 0;
  uint64_t projectiveModelValueFlowProgramFingerprint = 0;
  // Reuses PGXPValue::VALID_INTRINSIC_PROVENANCE bits. The GP0 token is kept
  // separately because it proves the submitted operand address even for a
  // Screen2D packet which must ignore accidental projection metadata.
  uint64_t gp0WordSource = 0;
  uint64_t dmaListSource = 0;
  uint64_t dmaNodeSource = 0;
  uint64_t dmaOrderingTableAnchorSource = 0;
  uint32_t dmaTraversalOrdinal = 0;
  bool dmaProvenance = false;
  bool dmaOrderingTableAnchorProvenance = false;
  uint8_t projectiveProvenance = 0;
  // Floating attributes are populated only for the modern clip/raster pass.
  // Native PS1 interpolation continues to consume the original integer
  // TexCoord and Color24 values.
  float modernU = 0.0f;
  float modernV = 0.0f;
  float modernR = 0.0f;
  float modernG = 0.0f;
  float modernB = 0.0f;
  bool precise = false;
  bool authenticPreciseX = false;
  bool authenticPreciseY = false;
  bool reconciledX = false;
  bool reconciledY = false;
  bool homogeneousProjection = false;
  // Complete finite W provenance. The donor-compatible GTE saturation lane is
  // valid depth; geometry/UV precision does not depend on visibility policy.
  bool depthValid = false;
};

enum class TrueDepthMode : uint8_t {
  Off,
  // Completeness-first donor policy. The PS1 ordering table remains the
  // visibility authority between packets, while every accepted fully-valid
  // opaque Projected3D sample stores genuine GTE W in D32. In other words the
  // depth attachment is real scene data, but it cannot erase OT-later work.
  // This is the original-game default and the safe basis for future proven
  // intra-scope LEQUAL classification; guest/native state remains untouched.
  Hybrid,
  // Research/reference mode retaining the former universal opaque depth test.
  // Invalid-W packets also write normalized far depth here; CrashReplay's
  // donor-safe execution planner deliberately rejects that diagnostic state.
  Strict,
};

// Represents RGB color
struct Color24 {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

// Texture coordinates
struct TexCoord {
  uint8_t u;
  uint8_t v;
};

// Packet-level texture domain used by the modern PGXP replay. Keeping the
// corrected coordinates wider than eight bits is intentional: perspective
// UVs are clamped to this domain before the PS1 sampler applies its 8-bit
// wrap and texture window.
struct ModernUVInput {
  // SwanStation performs its flipped-quad test on the final PGXP BatchVertex
  // floats, not on the packed native coordinate. Keeping that representation
  // here is observable when a fractional vertex turns an otherwise-zero
  // derivative into a non-zero one.
  float x = 0.0f;
  float y = 0.0f;
  float w = 1.0f;
  uint16_t u = 0;
  uint16_t v = 0;
};

struct ModernUVDomain {
  double minU = 0.0;
  double maxU = 255.0;
  double minV = 0.0;
  double maxV = 255.0;
  uint16_t offsetU = 0;
  uint16_t offsetV = 0;
  bool enabled = false;
  bool flippedU = false;
  bool flippedV = false;
};

// Donor-compatible UV preparation shared by software and future accelerated
// modern backends. `quad` enables the SwanStation/Beetle flipped-sprite
// correction above native resolution when vertices 0--2 have uniform
// effective W; the fourth W deliberately does not enter the donor heuristic.
ModernUVDomain prepareModernUVDomain(const ModernUVInput *inputs,
                                     std::size_t inputCount,
                                     uint32_t resolutionScale, bool quad);

/**
 * @brief PS1 GPU state machine + 1024x512 VRAM + draw/display environment.
 *
 * Owns the entire GPU side of the system: GP0/GP1 command parsing, primitive
 * rasterisation into VRAM, drawing-area and texture-window registers, and
 * the per-frame display-region snapshot consumed by the Vulkan renderer.
 */
class GPU {
public:
  struct GuestState {
    uint32_t gpuStat = 0;
    uint32_t gpuRead = 0;
    int32_t drawOffsetX = 0;
    int32_t drawOffsetY = 0;
    int32_t drawAreaX1 = 0;
    int32_t drawAreaY1 = 0;
    int32_t drawAreaX2 = 0;
    int32_t drawAreaY2 = 0;
    bool ditherEnable = false;
    uint8_t blendMode = 0;
    bool setMaskWhileDrawing = false;
    bool checkMaskBeforeDraw = false;
    uint16_t currentTexpage = 0;
    bool textureRectFlipX = false;
    bool textureRectFlipY = false;
    bool allowTextureDisable = false;
    bool textureDisable = false;
    bool displayModeSet = false;
    bool displayAreaSet = false;
    bool horizontalDisplayRangeSet = false;
    bool verticalDisplayRangeSet = false;
    uint32_t displayVRAMXStart = 0;
    uint32_t displayVRAMYStart = 0;
    uint32_t displayX1 = 0;
    uint32_t displayX2 = 0;
    uint32_t displayY1 = 0;
    uint32_t displayY2 = 0;
    uint8_t texWindowMaskX = 0;
    uint8_t texWindowMaskY = 0;
    uint8_t texWindowOffsetX = 0;
    uint8_t texWindowOffsetY = 0;
  };

  struct DisplayState {
    uint32_t xStart = 0;
    uint32_t yStart = 0;
    uint32_t displayX1 = 0;
    uint32_t displayX2 = 0;
    uint32_t displayY1 = 0;
    uint32_t displayY2 = 0;
    uint32_t gpuStat = 0;
    bool modeSet = false;
    bool areaSet = false;
    bool horizontalRangeSet = false;
    bool verticalRangeSet = false;
    uint64_t latchFrame = 0;
    uint64_t completedSubmissionId = 0;
    // Changes only when the latched pixel content is materialized again.
    // latchFrame may advance while this stays constant, allowing consumers to
    // update temporal state without copying or uploading an identical image.
    uint64_t contentRevision = 0;
  };

  enum class DisplayCopyStatus : uint8_t {
    Unavailable,
    Unchanged,
    Copied,
  };

  GPU();
  ~GPU();

  // Reset the GPU state
  void reset();

  // Handle a GP0 (Rendering & VRAM) command
  void
  writeGP0(uint32_t val, uint32_t sourceAddress = UINT32_MAX,
           const PGXPValue *precision = nullptr,
           PGXPSubmissionPath submissionPath = PGXPSubmissionPath::DirectMmio,
           GP0DmaWordProvenance dmaProvenance = {});

  // Handle a GP1 (Display Control) command
  void writeGP1(uint32_t val);

  // Process a DMA linked list (Ordering Table) from main RAM
  void processLinkedList(uint32_t startAddr, const uint8_t *ram,
                         const ps1::Memory *memory = nullptr);

  // Read GPU response (GPUREAD)
  uint32_t readGPUREAD();

  // Read GPU Status Register (GPUSTAT)
  uint32_t readGPUSTAT() const;

  // Get a pointer to the 1024x512 VRAM framebuffer (live -- game thread writes
  // here)
  const Color16 *getVRAM() const { return vram_.data(); }

  /// Push accumulated GPU counters into ps1::metrics at shutdown.
  /// `ps1::metrics::count()` accumulates, so this publishes only what has
  /// accrued since the previous call: main_host reaches this from two
  /// shutdown paths (normal join and the forced `_Exit`), and a flat
  /// re-publish would double every `gp0.op.XX` and `gp0.words`.
  void publishMetrics() const;

  // Load 1024x512x2 bytes of VRAM from a save-state buffer.
  void loadVram(const uint8_t *data);
  GuestState guestState() const;
  void loadGuestState(const GuestState &state);

  // Get the display-safe snapshot (captured at VBlank).  The raw accessor is
  // retained for diagnostics that run after emulation has stopped.  A live
  // renderer must use copyDisplayVRAM() so the VBlank writer cannot race its
  // texture upload.
  const Color16 *getDisplayVRAM() const { return displayVram_.data(); }

  // Copy up to pixelCount pixels from the VBlank snapshot while holding the
  // same mutex as snapshotDisplayBuffer(). When requested, replayFrame is the
  // exact immutable frame sealed by that same latch. Returns the number
  // copied.
  std::size_t
  copyDisplayVRAM(Color16 *destination, std::size_t pixelCount,
                  DisplayState *state = nullptr,
                  replay::CrashReplayFramePtr *replayFrame = nullptr) const;

  // Vector variant for accelerated backends. The native 1x snapshot is left
  // untouched when knownContentRevision is current, avoiding both a CPU copy
  // and a Vulkan upload for reused VBlanks.
  DisplayCopyStatus copyDisplayVRAMIfChanged(
      std::vector<Color16> &destination, uint64_t knownContentRevision,
      DisplayState *state = nullptr,
      replay::CrashReplayFramePtr *replayFrame = nullptr) const;

  // Copy the VBlank-latched modern display aperture. Returns false while the
  // companion surface is disabled. Width/height are physical host samples,
  // already multiplied by internal resolution scale.
  bool copyDisplayRGBA8(std::vector<Color32> &destination, uint32_t &width,
                        uint32_t &height, DisplayState *state = nullptr) const;

  // As above, but leave destination untouched when the caller already owns
  // the current content revision. Dimensions and state are still refreshed so
  // presentation remains synchronized with the newest VBlank.
  DisplayCopyStatus
  copyDisplayRGBA8IfChanged(std::vector<Color32> &destination, uint32_t &width,
                            uint32_t &height, uint64_t knownContentRevision,
                            DisplayState *state = nullptr) const;

  // Snapshot the current display region into the display buffer.
  // Call this at VBlank time from the main thread.
  void snapshotDisplayBuffer(uint64_t latchFrame = UINT64_MAX);

  // Keep one logical GP0 list indivisible with respect to the VBlank latch.
  // Individual writes remain safe outside a batch; recursive locking lets
  // PsyQ/DMA list submitters bracket the full list without an unlocked parser.
  void beginCommandBatch();
  void endCommandBatch();
  // Submit one word while the calling thread owns beginCommandBatch(). The
  // fallback retains ordinary locking if a test/HLE caller omits the bracket.
  void writeGP0BatchWord(
      uint32_t val, uint32_t sourceAddress = UINT32_MAX,
      const PGXPValue *precision = nullptr,
      PGXPSubmissionPath submissionPath = PGXPSubmissionPath::DirectMmio,
      GP0DmaWordProvenance dmaProvenance = {});
  uint64_t activeSubmissionId() const;
  // Shared by DMA and HLE list traversal; caller holds the command batch.
  std::function<void(uint32_t)> observePresentationList(uint32_t start,
                                                        const uint8_t *ram);

  // Check if display is enabled (GP1(0x03), GPUSTAT bit 23)
  bool isDisplayEnabled() const { return !(gpuStat_ & (1u << 23)); }

  // True once the game has sent GP1(0x08) to explicitly set display mode.
  // When false, the renderer uses safe 320x240 defaults instead of GPUSTAT.
  bool isDisplayModeSet() const { return displayModeSet_; }

  // True once the game has sent GP1(0x05) to explicitly set display area.
  // When false, the renderer may auto-detect the active framebuffer.
  bool isDisplayAreaSet() const { return displayAreaSet_; }

  // Dimensions of VRAM
  static constexpr uint32_t VRAM_WIDTH = 1024;
  static constexpr uint32_t VRAM_HEIGHT = 512;

  // Display State accessors for Renderer
  void getDisplayArea(uint32_t &xStart, uint32_t &yStart) const {
    xStart = displayVRAMXStart_;
    yStart = displayVRAMYStart_;
  }

  void getDisplayRange(uint32_t &x1, uint32_t &x2, uint32_t &y1,
                       uint32_t &y2) const {
    x1 = displayX1_;
    x2 = displayX2_;
    y1 = displayY1_;
    y2 = displayY2_;
  }

  // Helper methods
  Color16 applyBlend(Color16 fg, Color16 bg);

  // Opt-in host raster path.  The legacy PS1 rasterizer remains the default;
  // enhanced rendering is used only when every vertex of a triangle carries
  // valid PGXP screen/depth provenance.
  void setEnhancedRaster(bool enabled);
  bool isEnhancedRasterEnabled() const { return enhancedRasterEnabled_; }
  void setProjectiveNearClip(bool enabled);
  bool isProjectiveNearClipEnabled() const {
    return projectiveNearClipEnabled_;
  }
  // Legacy/test API: true selects strict mode. Runtime configurations should
  // choose an explicit mode through setTrueDepthMode().
  void setTrueDepth(bool enabled);
  void setTrueDepthMode(TrueDepthMode mode);
  TrueDepthMode trueDepthMode() const { return trueDepthMode_; }
  bool isTrueDepthEnabled() const {
    return trueDepthMode_ != TrueDepthMode::Off;
  }

  // Configure the game-independent companion render target. Native PS1 VRAM
  // remains authoritative regardless of these presentation settings.
  void configureEnhancedPresentation(uint32_t scale, bool trueColor,
                                     bool useProgrammedDisplayRange = true);
  uint32_t internalResolutionScale() const {
    return enhancedSurface_.enabled() ? enhancedSurface_.scale() : 1u;
  }
  bool isTrueColorEnabled() const { return trueColorEnabled_; }

  // Capture-only bridge to CrashReplayV1. Enabling it never enables the CPU
  // EnhancedSurface and never changes guest-authoritative native VRAM.
  using ReplayConfig = replay::ReplayRecorderConfig;
  void configureReplay(ReplayConfig config);
  bool isReplayCaptureEnabled() const { return replayRecorder_.enabled(); }
  replay::CrashReplayFramePtr tryConsumeReplay();

private:
  void resetState(bool clearVideoMemory);
  void writeGP0Unlocked(uint32_t val, uint32_t sourceAddress,
                        const PGXPValue *precision,
                        PGXPSubmissionPath submissionPath,
                        GP0DmaWordProvenance dmaProvenance);

  // VRAM buffer (live -- game thread writes here)
  std::vector<Color16> vram_;

  // Display snapshot buffer (read by renderer at frame time)
  std::vector<Color16> displayVram_;

  // High-resolution/true-color companion and its immutable VBlank aperture.
  EnhancedSurface enhancedSurface_;
  std::vector<Color32> displayRgba8_;
  uint32_t displayRgba8Width_ = 0;
  uint32_t displayRgba8Height_ = 0;
  bool trueColorEnabled_ = false;
  bool useProgrammedDisplayRange_ = true;

  // True depth belongs exclusively to EnhancedSurface. Native RGB555 VRAM is
  // always produced by the PS1 painter/affine path so guest reads, texture
  // feedback and save states cannot depend on PGXP availability.
  bool enhancedRasterEnabled_ = false;
  bool projectiveNearClipEnabled_ = false;
  TrueDepthMode trueDepthMode_ = TrueDepthMode::Off;
  bool depthEpochDrawAreaDirty_ = false;
  bool depthEpochHasMean_ = false;
  double depthEpochLastMean_ = 0.0;
  uint64_t depthEpochId_ = 0;
  // Native-space union of draw areas that may contain depth writes in the
  // current epoch. SwanStation invalidates the complete attachment; these
  // bounds are retained as trace evidence and to avoid empty epoch advances.
  bool depthEpochWrittenBoundsValid_ = false;
  int32_t depthEpochWrittenX1_ = 0;
  int32_t depthEpochWrittenY1_ = 0;
  int32_t depthEpochWrittenX2_ = 0;
  int32_t depthEpochWrittenY2_ = 0;

  // Display registers latched at the same VBlank as displayVram_.  Reading
  // the live GP1 state after copying the latched pixels can pair frame N's
  // VRAM with frame N+1's display origin during a double-buffer flip.
  DisplayState displaySnapshot_;
  // Published while holding displayMutex_ after the native pixels and display
  // registers above have been latched. This pointer is an identity token, not
  // a value-derived lookup key: empty VBlanks can share every revision.
  replay::CrashReplayFramePtr displayReplayFrame_;

  // Mutex for snapshot buffer access
  mutable std::mutex displayMutex_;

  // Serializes GP0/GP1 against the VBlank latch. A recursive mutex lets one
  // complete ordering-table transaction own the timeline while writeGP0 keeps
  // its ordinary per-call safety for MMIO and tests.
  mutable std::recursive_mutex gpuTimelineMutex_;

  enum class PolygonRoute : uint8_t {
    Compatibility3D,
    Precise3D,
    Screen2D,
  };
  enum class ModernDepthAction : uint8_t {
    Preserve,
    // Accept in GP0/ordering-table order and store genuine W. This retains a
    // useful physical depth attachment without letting an unproven cross-
    // packet LEQUAL relation remove pixels from the authoritative PS1 image.
    Store,
    TestWrite,
    // Ordering-table painter work must bypass D32, but an accepted fragment
    // also ends the preceding opaque visibility epoch at that exact sample.
    // Publishing normalized far depth lets the next OT-later opaque fragment
    // establish a fresh real-Z owner without clearing unrelated pixels.
    InvalidateAcceptedFar,
    // Invalid PGXP provenance is packet-atomic: every vertex uses normalized
    // far W (1.0, represented here as raw W=32768) for geometry/UV. Hybrid
    // preserves D32; Strict additionally writes that far depth diagnostically.
    NormalizeFar,
    // Hybrid semi-transparency can need both packet-atomic W normalization and
    // the accepted-sample OT visibility boundary above.
    NormalizeAndInvalidateAcceptedFar,
  };

  struct PrimitivePathState {
    uint64_t submission = 0;
    PolygonRoute route = PolygonRoute::Compatibility3D;
    uint8_t validMask = 0;
  };
  struct SubmissionStats {
    uint64_t startVblank = 0;
    uint64_t packets = 0;
    uint64_t precisePackets = 0;
    uint64_t compatibilityPackets = 0;
    uint64_t screen2DPackets = 0;
    uint64_t partialPrecisionPackets = 0;
    uint64_t boundsRejects = 0;
    uint64_t pathToggles = 0;
  };
  // Ordering-table storage is routinely reused for different GP0 packet
  // classes. Keying by opcode as well as source prevents that reuse from
  // looking like a temporal precise/compatibility route toggle.
  std::unordered_map<uint64_t, PrimitivePathState> primitivePathHistory_;
  uint32_t commandBatchDepth_ = 0;
  uint64_t nextSubmissionId_ = 0;
  uint64_t activeSubmissionId_ = 0;
  uint64_t lastCompletedSubmissionId_ = 0;
  uint64_t lastLatchedSubmissionId_ = 0;
  replay::CrashReplayRecorder replayRecorder_;
  uint64_t activeReplayPacketSequence_ = replay::UNASSIGNED_PACKET_SEQUENCE;
  bool activeReplayPacketUnsupported_ = false;
  bool replayPaintCaptureActive_ = false;
  std::vector<replay::PaintedPixel> replayPaintCapture_;
  bool replayDepthDrawAreaDirty_ = false;
  bool replayDepthHasMean_ = false;
  double replayDepthLastMean_ = 0.0;
  uint64_t replayDepthEpoch_ = 1;
  // Every GP0/GP1 write advances this serial, including writes outside an
  // explicit DMA/OT batch. Consecutive VBlanks with no mutation can reuse the
  // already-latched native and 4x buffers instead of copying tens of MiB
  // again while the host drains timing debt.
  uint64_t mutationSerial_ = 0;
  uint64_t lastLatchedMutationSerial_ = UINT64_MAX;
  uint64_t latchedNativeHash_ = 0;
  uint64_t latchedModernHash_ = 0;
  uint64_t latchedModernNonblack_ = 0;
  SubmissionStats submissionStats_;
  uint64_t framePreciseTriangles_ = 0;
  uint64_t frameCompatibilityTriangles_ = 0;
  uint64_t frameScreen2DTriangles_ = 0;
  uint64_t framePartialPrecisionPackets_ = 0;
  uint64_t frameBoundsRejects_ = 0;
  uint64_t framePathToggles_ = 0;
  uint64_t frameModernCoveredSamples_ = 0;
  uint64_t frameModernInvalidDepthSamples_ = 0;
  uint64_t frameModernMaskRejectedSamples_ = 0;
  uint64_t frameModernDepthRejectedSamples_ = 0;
  uint64_t frameModernTransparentSamples_ = 0;
  uint64_t frameModernAcceptedSamples_ = 0;
  uint64_t frameModernEmptyTriangles_ = 0;
  uint64_t frameModernEmptyDepthOnly_ = 0;
  uint64_t frameModernEmptyTransparentOnly_ = 0;
  uint64_t frameModernEmptyMaskOnly_ = 0;
  uint64_t frameModernEmptyInvalidDepthOnly_ = 0;
  uint64_t frameModernEmptyMixed_ = 0;
  uint64_t frameModernNativeBoundsSkips_ = 0;
  uint64_t frameModernProjectiveInvalid_ = 0;
  uint64_t frameModernContextMismatches_ = 0;
  uint64_t frameModernNearClipDiscarded_ = 0;
  uint64_t frameModernNearClipDiscardedNativeCoverage_ = 0;
  uint64_t frameModernNearClipDiscardedNativeEmpty_ = 0;
  uint64_t frameModernNearClipPolygons_ = 0;
  uint64_t frameModernNearClipOutputTriangles_ = 0;
  uint64_t frameModernNearSaturatedTriangles_ = 0;
  uint64_t frameModernNearSaturatedVertices_ = 0;
  uint64_t frameModernPreciseDegenerate_ = 0;
  uint64_t frameModernPreciseDegenerateNativeCoverage_ = 0;
  uint64_t frameModernPreciseDegenerateNativeEmpty_ = 0;
  uint64_t frameModernOutsideDrawArea_ = 0;
  uint64_t frameModernOutsideDrawAreaNativeCoverage_ = 0;
  uint64_t frameModernOutsideDrawAreaNativeEmpty_ = 0;
  uint64_t frameModernDepthEligiblePackets_ = 0;
  uint64_t frameModernDepthInvalidPackets_ = 0;
  uint64_t frameModernDepthBlendVisibilityBoundaryPackets_ = 0;
  uint64_t frameModernDepthVisibilityBoundarySamples_ = 0;
  uint64_t frameModernDepthEpochDrawAreaClears_ = 0;
  uint64_t frameModernDepthEpochZJumpClears_ = 0;
  uint64_t frameModernUVLimitedPrimitives_ = 0;
  uint64_t frameModernUVFlippedUPrimitives_ = 0;
  uint64_t frameModernUVFlippedVPrimitives_ = 0;
  uint64_t frameModernUVClampedSamples_ = 0;
  uint64_t frameModernUVSampleCacheHits_ = 0;
  uint64_t frameModernUVSampleCacheMisses_ = 0;

  // GPU status registers
  uint32_t gpuStat_;
  uint32_t gpuRead_;

  // GP0 opcode histogram.  Written on the game thread, read by the main
  // thread at shutdown, hence atomic.  Indexed by the command byte.
  mutable std::atomic<uint64_t> gp0Hist_[256]{};
  mutable std::atomic<uint64_t> gp0Words_{0};
  mutable std::atomic<uint64_t> pgxpVertexHits_{0};
  mutable std::atomic<uint64_t> pgxpVertexMisses_{0};

  // High-water marks of what publishMetrics() has already handed to
  // ps1::metrics.  Only ever touched by publishMetrics(), which runs on the
  // main thread at shutdown, so these need no synchronisation of their own.
  mutable uint64_t gp0HistPublished_[256]{};
  mutable uint64_t gp0WordsPublished_{0};
  mutable uint64_t pgxpVertexHitsPublished_{0};
  mutable uint64_t pgxpVertexMissesPublished_{0};

  // Command buffering
  // GP0 packets are normally 1--12 words. Persistent vectors avoid the
  // repeated block allocation/deallocation of four deques for every polygon;
  // optional precision also leaves non-coordinate words byte-light.
  std::vector<uint32_t> commandQueue_;
  std::vector<uint32_t> commandSourceQueue_;
  std::vector<std::optional<PGXPValue>> commandPrecisionQueue_;
  std::vector<GP0DmaWordProvenance> commandDmaProvenanceQueue_;
  uint32_t expectedCommandWords_;
  bool polylineCommand_ = false;

  // Internal states
  bool isCommandExecuting_;

  // Drawing attributes
  int32_t drawOffsetX_;
  int32_t drawOffsetY_;
  int32_t drawAreaX1_;
  int32_t drawAreaY1_;
  int32_t drawAreaX2_;
  int32_t drawAreaY2_;
  bool ditherEnable_;
  uint8_t blendMode_; // 0: B/2+F/2, 1: B+F, 2: B-F, 3: B+F/4
  bool setMaskWhileDrawing_;
  bool checkMaskBeforeDraw_;
  // Current texture page from the last GP0(0xE1) draw-mode command. Sprites
  // (GP0 0x64-0x7F) carry no tpage word of their own and sample from this.
  uint16_t currentTexpage_;
  bool textureRectFlipX_;
  bool textureRectFlipY_;
  // GP1(09h) gates whether GP0(E1h).bit11 may disable texturing. Retail
  // software normally leaves this false; interpreting E1.11 unconditionally
  // turns ordinary draw-mode words (including Crash's 0x0A60) into flat
  // primitives.
  bool allowTextureDisable_;
  bool textureDisable_;

  // Set once GP1(0x08) is received; renderer uses defaults until then.
  bool displayModeSet_;

  // Set once GP1(0x05) is received; renderer uses auto-detect until then.
  bool displayAreaSet_;

  // Set independently because the reset values are valid numeric ranges but
  // do not mean that a title deliberately programmed GP1(06)/GP1(07).
  bool horizontalDisplayRangeSet_;
  bool verticalDisplayRangeSet_;

  // Display attributes
  uint32_t displayVRAMXStart_;
  uint32_t displayVRAMYStart_;
  uint32_t displayX1_, displayX2_;
  uint32_t displayY1_, displayY2_;

  // Texture Window attributes
  uint8_t texWindowMaskX_;
  uint8_t texWindowMaskY_;
  uint8_t texWindowOffsetX_;
  uint8_t texWindowOffsetY_;

  // VRAM copy parameters
  struct {
    uint32_t srcX, srcY;
    uint32_t destX, destY;
    uint32_t currX, currY;
    uint32_t width, height;
    uint32_t transferWordsRemaining;
    uint32_t transferPixelsRemaining;
    bool isWritingToVRAM;
    bool isReadingFromVRAM;
    bool isCopyingVRAM;
  } vramTransfer_;

  // Internal decoding helpers
  void executeGP0Command();
  void executeGP1Command(uint32_t val);

  // GP0 specific commands
  void executeClearCache();
  void executeFillRect();
  void executeCopyVRAM();
  void executeCPUToVRAM();
  void executeVRAMToCPU();
  void executeTextureWindow();
  void latchTexturePage(uint16_t tpage);

  void executeMonochromePoly3();
  void executeMonochromePoly4();

  void executeTexturedPoly3();
  void executeTexturedPoly4();

  void executeGouraudPoly3();
  void executeGouraudPoly4();
  void executeGouraudTexturedPoly3();
  void executeGouraudTexturedPoly4();

  void executeLine();
  void executeRect();
  void beginReplayPaintCapture();
  void captureReplayPaintedPixel(int x, int y, Color16 nativeColor,
                                 replay::Rgb8 color, bool sourceMaskBit);
  void finishReplayPaintCapture(uint8_t opcode);
  PGXPSemanticDecision
  classifyPolygonPacket(const std::array<std::size_t, 4> &wordIndices,
                        std::size_t vertexCount) const;
  Vertex decodeVertex(std::size_t wordIndex, PGXPSemanticDecision semantic);

  // Helper methods for rasterization
  Color16 applyDither(Color16 baseColor, int x, int y);
  Color16 applyBlendMode(Color16 fg, Color16 bg, uint8_t mode) const;
  Color16 modulateTexel(Color16 texel, Color24 color, int x, int y) const;
  Color16 packColor(int r, int g, int b, bool mask, int x, int y,
                    bool dither) const;
  bool writePixel(int x, int y, Color16 color, bool mirrorToEnhanced = true);
  void clearDepthBuffer();
  uint8_t vertexPrecisionMask(const Vertex *vertices,
                              std::size_t vertexCount) const;
  bool shouldUseEnhancedRaster(const Vertex *vertices, std::size_t vertexCount,
                               uint8_t validMask);
  void consumeModernDepthDrawAreaEpoch();
  ModernDepthAction prepareModernDepthPacket(const Vertex *vertices,
                                             std::size_t vertexCount,
                                             bool blend);
  ModernUVDomain prepareModernUVDomainForPacket(const Vertex *vertices,
                                                const TexCoord *texCoords,
                                                std::size_t vertexCount,
                                                bool quad,
                                                uint32_t resolutionScale);
  void notePrimitivePath(PolygonRoute route, PGXPSemanticDecision semantic,
                         uint8_t validMask, uint8_t vertexCount,
                         uint8_t triangleCount);
  bool capturePolygonPacket(const Vertex *vertices, const TexCoord *texCoords,
                            const Color24 *colors, std::size_t vertexCount,
                            uint16_t clut, uint16_t tpage,
                            PGXPSemanticDecision semantic, uint8_t validMask,
                            bool textured, bool gouraud, bool rawTexture,
                            bool blend);
  bool shouldRasterizeNativePolygon(PGXPSemanticDecision semantic,
                                    bool replayCaptured) const;
  void markCapturedPolygonNativeWrite(const Vertex *vertices,
                                      std::size_t vertexCount);
  void markActiveReplayPacketUnsupported();
  void consumeReplayDepthDrawAreaEpoch();
  replay::DepthState
  prepareReplayDepthState(const Vertex *vertices, std::size_t vertexCount,
                          replay::PacketRoute route, uint8_t validMask,
                          replay::PacketFallbackReason fallbackReason,
                          bool blend);
  std::span<const Color16> replayNativeVram() const;
  void noteBoundsReject();
  void traceModernCoverageDivergence(
      uint8_t outcome, const std::array<int32_t, 6> &nativeCoordinates,
      const Vertex &v0, const Vertex &v1, const Vertex &v2, bool postNearClip,
      int fanIndex, int outputTriangleCount) const;
  Color32 packModernColor(int r, int g, int b, bool mask, int x, int y) const;
  Color32 modulateModernTexel(Color16 texel, Color24 color, int x, int y) const;
  Color32 applyModernBlendMode(Color32 foreground, Color32 background,
                               uint8_t mode) const;
  void writeModernBlock(int x, int y, Color32 foreground, bool sourceMask,
                        bool blend, uint8_t mode);
  void rasterizeEnhancedSurfaceTriangle(
      Vertex v0, Vertex v1, Vertex v2, TexCoord t0, TexCoord t1, TexCoord t2,
      Color24 c0, Color24 c1, Color24 c2, uint16_t clut, uint16_t tpage,
      bool textured, bool gouraud, bool rawTexture, bool blend,
      const ModernUVDomain &uvDomain, ModernDepthAction depthAction);
  void rasterizeEnhancedClippedTriangle(
      Vertex v0, Vertex v1, Vertex v2, uint16_t clut, uint16_t tpage,
      bool textured, bool gouraud, bool rawTexture, bool blend,
      const std::array<int32_t, 6> &nativeCoordinates, bool postNearClip,
      int fanIndex, int outputTriangleCount, const ModernUVDomain &uvDomain,
      ModernDepthAction depthAction);
  void drawLineSegment(Vertex v0, Vertex v1, Color24 c0, Color24 c1,
                       bool gouraud, bool blend);

  // Rasterize a solid monochrome triangle
  void rasterizeTriangle(Vertex v0, Vertex v1, Vertex v2, Color24 c, bool blend,
                         bool precisePacket);

  // Sample a single texel from VRAM (texture page + CLUT). raw==0 =
  // transparent.
  Color16 sampleTexel(int u, int v, uint16_t tpage, uint16_t clut) const;

  // Rasterize a textured triangle
  void rasterizeTexturedTriangle(Vertex v0, Vertex v1, Vertex v2, TexCoord t0,
                                 TexCoord t1, TexCoord t2, Color24 color,
                                 uint16_t clut, uint16_t tpage, bool isRaw,
                                 bool blend, bool precisePacket);

  // Rasterize a gouraud-shaded triangle (per-vertex color interpolation)
  void rasterizeGouraudTriangle(Vertex v0, Vertex v1, Vertex v2, Color24 c0,
                                Color24 c1, Color24 c2, bool blend,
                                bool precisePacket);

  // Rasterize a gouraud-shaded textured triangle
  void rasterizeGouraudTexturedTriangle(Vertex v0, Vertex v1, Vertex v2,
                                        TexCoord t0, TexCoord t1, TexCoord t2,
                                        Color24 c0, Color24 c1, Color24 c2,
                                        uint16_t clut, uint16_t tpage,
                                        bool isRaw, bool blend,
                                        bool precisePacket);

  // Helper methods for rasterization (to be implemented)
  void drawSolidPoly(bool quad);
  void drawTexturedPoly(bool quad, bool blend, bool raw);
};

} // namespace ps1::gpu
