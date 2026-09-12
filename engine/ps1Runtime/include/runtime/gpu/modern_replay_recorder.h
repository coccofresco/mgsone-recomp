#pragma once

#include "runtime/gpu/modern_replay.h"
#include "runtime/gpu/native_color.h"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace ps1::gpu::replay {

struct ReplayRecorderConfig {
  bool enabled = false;
  uint32_t internalScale = 1;
  bool pgxp = false;
  bool trueColor = false;
  ReplayDepthMode trueDepthMode = ReplayDepthMode::Off;
  // Performance experiment only.  A captured Projected3D packet targeting a
  // full presentation-sized draw area is still submitted to Vulkan, but its
  // duplicate 1x CPU raster is omitted. Smaller off-screen targets remain
  // native-authoritative for render-to-texture chains. This deliberately
  // weakens the native-M0/guest-read safety net and must stay opt-in until
  // framebuffer feedback and guest reads have a Vulkan-owned or lazily
  // materialized authoritative path.
  bool omitProjectedPresentationNativeRasterDiagnostic = false;

  bool operator==(const ReplayRecorderConfig &) const = default;
};

struct OpcodeCoverage {
  uint64_t seen = 0;
  uint64_t supported = 0;
  // Number of complete GP0 packets represented by one or more IR operations.
  // An A0 transfer counts once even when VBlank divides it into many chunks.
  uint64_t recorded = 0;
  uint64_t unsupported = 0;

  bool operator==(const OpcodeCoverage &) const = default;
};

// Single-writer recorder placed between decoded GP0 packets and Vulkan.
//
// GPU integration order is:
//   observeOpcode -> record operation (and pre-read uploads) -> native 1x draw
//   -> markNativeWrite.  All methods except mailbox consumption are intended
// for the serialized GPU timeline. CrashReplayMailbox supplies the only
// producer/consumer synchronization required by the render thread.
class CrashReplayRecorder final {
public:
  static constexpr uint32_t NATIVE_WIDTH = 1024;
  static constexpr uint32_t NATIVE_HEIGHT = 512;
  static constexpr uint32_t TILE_WIDTH = CRASH_REPLAY_NATIVE_TILE_WIDTH;
  static constexpr uint32_t TILE_HEIGHT = CRASH_REPLAY_NATIVE_TILE_HEIGHT;
  static constexpr uint32_t TILES_X = NATIVE_WIDTH / TILE_WIDTH;
  static constexpr uint32_t TILES_Y = NATIVE_HEIGHT / TILE_HEIGHT;
  static constexpr uint32_t TILE_COUNT = TILES_X * TILES_Y;
  static constexpr uint32_t NATIVE_READ_CACHE_LIMIT = 2048;
  static constexpr std::size_t NATIVE_PIXEL_COUNT =
      static_cast<std::size_t>(NATIVE_WIDTH) * NATIVE_HEIGHT;

  explicit CrashReplayRecorder(ReplayRecorderConfig config = {});

  // Reconfiguration starts a new capture timeline. It is accepted only before
  // this recorder has published its first latch and while no A0 transfer or
  // unpublished mutation exists. Publication makes the policy permanently
  // sticky even after the consumer drains the mailbox.
  void configure(ReplayRecorderConfig config);
  const ReplayRecorderConfig &config() const { return config_; }
  ReplayPolicyState policy() const;
  bool enabled() const { return config_.enabled; }

  static bool isCrashReplayV1OpcodeSupported(uint8_t opcode);
  static bool opcodeRequiresReplayOperation(uint8_t opcode);

  // Observe exactly once after a complete GP0 packet has been decoded. The
  // returned monotonically increasing sequence belongs in DrawPacket.
  uint64_t observeOpcode(uint8_t opcode);

  // Resolve a normally supported packet which cannot be represented by this
  // capture. The enclosing FrameLatch becomes incomplete without leaving a
  // dangling expected-operation count.
  void markPacketUnsupported(uint8_t opcode);

  const OpcodeCoverage &opcodeCoverage(uint8_t opcode) const {
    return opcodeCoverage_[opcode];
  }
  const std::array<OpcodeCoverage, 256> &opcodeCoverage() const {
    return opcodeCoverage_;
  }

  // Establish an immutable complete native baseline for the Vulkan shadow and
  // modern color/mask surface. This is used at power-on and save-state load,
  // not for GP1(00), which should record Reset instead.
  void recordSurfaceSeed(std::span<const Color16> nativeVram,
                         uint64_t depthEpoch = 1);
  void recordReset(Reset operation);
  void recordClearDepth(ClearDepth operation);

  // Textured draws conservatively synchronize a complete texture page and
  // CLUT before the DrawPacket. The returned dependency is also baked into the
  // packet. Native VRAM is copied immediately, so later CPU writes cannot
  // mutate an already-recorded tile upload.
  NativeReadDependency prepareNativeRead(const TexturePageState &texturePage,
                                         const ClutState &clut,
                                         std::span<const Color16> nativeVram);
  // CopyRect uses the same proof for its complete wrapped source rectangle.
  // recordCopy calls this automatically; it is public for capture diagnostics
  // and producer/consumer conformance tests.
  NativeReadDependency prepareNativeRectRead(NativeRect source,
                                             std::span<const Color16> nativeVram);
  void recordDraw(DrawPacket packet, std::span<const Color16> nativeVram);
  // Host-owned source geometry has its own sequence slot, but must not alter
  // observed/required/recorded GP0 coverage or the authoritative native raster.
  void recordPresentationDraw(DrawPacket packet,
                              std::span<const Color16> nativeVram);

  // Resolve a decoded command whose native semantics deliberately produce no
  // mutation (for example GP0(02) with a zero dimension or a clipped GP0(68)).
  // It still counts as represented coverage and forms an ordering barrier for
  // coalesced point writes, but emits no replay operation.
  void recordNoOp(uint8_t opcode);
  void recordFill(uint8_t opcode, FillRect operation);
  void recordCopy(uint8_t opcode, CopyRect operation,
                  std::span<const Color16> nativeVram);
  // PaintPixels is captured after the authoritative native raster has made
  // transparency, blend and E6 acceptance decisions. Recording therefore
  // also advances precisely the native tiles touched by its accepted output;
  // callers must not mark those writes a second time.
  void recordPaintPixels(uint8_t opcode, PaintPixels operation);
  void recordPaintRect(uint8_t opcode, PaintRect operation);

  // Mark guest-authoritative native writes after the CPU operation completes.
  // Rectangles use PS1 VRAM wrap semantics. One call advances the global native
  // revision once and assigns it to every intersecting tile.
  uint64_t markNativeWrite(NativeRect destination);
  uint64_t markNativeWrite(NativePoint destination);
  uint64_t nativeRevision() const { return nativeRevision_; }
  uint64_t nativeTileRevision(uint32_t tileIndex) const;
  uint64_t queuedTileRevision(uint32_t tileIndex) const;

  // A0 is the only visual GP0 operation which mutates VRAM after its header
  // packet has executed. Chunks retain full transfer geometry and a logical
  // pixel offset, so VBlank may flush in the middle of a row without padding.
  void beginCpuUpload(uint8_t opcode, uint64_t transactionId,
                      NativeRect transfer, MaskState mask);
  void appendCpuUploadPixel(uint16_t sourcePixel, bool accepted);
  bool flushCpuUpload();
  void finishCpuUpload();
  void abortCpuUpload();
  bool cpuUploadActive() const { return cpuUpload_.active; }
  uint64_t cpuUploadConsumedPixels() const { return cpuUpload_.consumed; }

  // Flushes a partial A0 chunk and pending PaintRect, seals the exact sequence
  // range, publishes it to the FIFO mailbox, and immediately opens the next
  // frame builder. Caller-supplied incompleteness is preserved.
  CrashReplayFramePtr sealAndPublish(FrameLatch latch);
  CrashReplayFramePtr tryConsume() { return mailbox_.tryConsume(); }
  CrashReplayMailbox &mailbox() { return mailbox_; }
  const CrashReplayMailbox &mailbox() const { return mailbox_; }
  void close() { mailbox_.close(); }

  uint64_t nextPacketSequence() const { return nextPacketSequence_; }
  bool surfaceSeeded() const { return surfaceSeeded_; }

private:
  struct CpuUploadState {
    bool active = false;
    uint8_t opcode = 0xA0;
    uint64_t transactionId = UNASSIGNED_PACKET_SEQUENCE;
    NativeRect transfer;
    MaskState mask;
    uint64_t totalPixels = 0;
    uint64_t consumed = 0;
    uint64_t chunkOffset = 0;
    std::vector<uint16_t> pixels;
    std::vector<uint64_t> acceptedBits;
    // Native versions advance once per flushed chunk, not once per pixel.
    // Rejected E6 mask-test samples never enter this set.
    // Membership keeps accepted-pixel insertion O(1); the companion sparse
    // index list avoids scanning every native tile when a chunk is flushed.
    std::bitset<TILE_COUNT> acceptedTileMembership;
    std::vector<uint32_t> acceptedTileIndices;
  };

  // Canonical ascending, duplicate-free tile indices. Per-command paths must
  // scale with the affected/read rectangle rather than all 8192 VRAM tiles.
  using TileSet = std::vector<uint32_t>;

  struct NativeReadCacheEntry {
    TileSet tiles;
    NativeReadDependency dependency;
    bool valid = false;
  };

  void requireEnabled() const;
  void requireNativeVram(std::span<const Color16> nativeVram) const;
  void resetTimeline();
  void markPacketRecorded(uint8_t opcode);
  uint64_t markNativeTiles(std::span<const uint32_t> tileIndices);
  void flushPendingPaintRect();
  bool canCoalesce(const PaintRect &left, const PaintRect &right) const;
  TileSet tilesForWrappedRect(NativeRect rect) const;
  TileSet nativeReadTiles(const TexturePageState &texturePage,
                          const ClutState &clut) const;
  NativeReadDependency prepareNativeTilesRead(
      std::span<const uint32_t> readTiles,
      std::span<const Color16> nativeVram);
  uint64_t nativeReadCacheKey(const TexturePageState &texturePage,
                              const ClutState &clut) const noexcept;
  void invalidateNativeReadCacheTiles(
      std::span<const uint32_t> tileIndices) noexcept;
  void clearNativeReadCache() noexcept;
  NativeTileUpload snapshotTile(uint32_t tileIndex, uint64_t revision,
                                std::span<const Color16> nativeVram) const;
  NativePoint cpuUploadDestination(uint64_t transferPixelOffset) const;
  uint64_t pendingRequiredCount() const;

  ReplayRecorderConfig config_;
  CrashReplayFrameBuilder builder_;
  CrashReplayMailbox mailbox_;
  std::array<OpcodeCoverage, 256> opcodeCoverage_{};
  // Single-writer high-water marks for low-overhead per-opcode coverage
  // publication at FrameLatch boundaries.
  std::array<OpcodeCoverage, 256> publishedOpcodeCoverage_{};
  std::array<uint64_t, 256> pendingRequiredByOpcode_{};
  std::array<uint64_t, TILE_COUNT> nativeTileRevision_{};
  std::array<uint64_t, TILE_COUNT> queuedTileRevision_{};
  // Texture pages and CLUTs are normally immutable for an entire frame, yet
  // a PS1 draw packet conservatively reads hundreds of 8x8 tiles. Cache the
  // already-proven dependency and invalidate it from the exact tiles written
  // later in the same serialized GPU timeline. Entries survive frame latches
  // because streamed levels commonly reuse immutable texture pages for many
  // minutes; an explicit cap bounds both storage and reverse subscriptions.
  std::unordered_map<uint64_t, uint32_t> nativeReadCacheIndex_;
  std::vector<NativeReadCacheEntry> nativeReadCacheEntries_;
  std::array<std::vector<uint32_t>, TILE_COUNT> nativeReadCacheSubscribers_;
  uint64_t frameNativeReadCacheHits_ = 0;
  uint64_t frameNativeReadCacheMisses_ = 0;
  uint64_t frameNativeReadCacheInvalidations_ = 0;
  uint64_t nativeRevision_ = 0;
  uint64_t nextPacketSequence_ = 0;
  uint64_t frameSequenceBegin_ = 0;
  uint64_t frameUnsupportedCount_ = 0;
  uint64_t frameUnrecoverableCount_ = 0;
  bool surfaceSeeded_ = false;
  bool hasPublishedLatch_ = false;
  CpuUploadState cpuUpload_;
  std::optional<PaintRect> pendingPaintRect_;
};

} // namespace ps1::gpu::replay
