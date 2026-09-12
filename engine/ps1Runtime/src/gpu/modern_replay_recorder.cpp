#include "runtime/gpu/modern_replay_recorder.h"

#include "runtime/metrics.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace ps1::gpu::replay {
namespace {

bool nativeSnapshotDominatesUnsupportedOpcode(uint8_t opcode) {
  return opcode == 0x02 || (opcode >= 0x20 && opcode <= 0x9F);
}

uint32_t wrapCoordinate(int64_t value, uint32_t limit) {
  int64_t wrapped = value % static_cast<int64_t>(limit);
  if (wrapped < 0)
    wrapped += limit;
  return static_cast<uint32_t>(wrapped);
}

uint64_t checkedIncrement(uint64_t value, const char *name) {
  if (value == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error(std::string(name) + " exhausted");
  return value + 1;
}

} // namespace

CrashReplayRecorder::CrashReplayRecorder(ReplayRecorderConfig config) {
  configure(config);
}

ReplayPolicyState CrashReplayRecorder::policy() const {
  return ReplayPolicyState{config_.internalScale, config_.pgxp,
                           config_.trueColor, config_.trueDepthMode};
}

void CrashReplayRecorder::configure(ReplayRecorderConfig config) {
  if (config.internalScale == 0 || config.internalScale > MAX_INTERNAL_SCALE)
    throw std::invalid_argument("CrashReplay internal scale must be 1..12");
  switch (config.trueDepthMode) {
  case ReplayDepthMode::Off:
  case ReplayDepthMode::Hybrid:
  case ReplayDepthMode::Strict:
    break;
  default:
    throw std::invalid_argument("CrashReplay depth mode is invalid");
  }
  if (hasPublishedLatch_)
    throw std::logic_error(
        "cannot reconfigure CrashReplay after publishing a frame latch");
  if (cpuUpload_.active)
    throw std::logic_error(
        "cannot reconfigure CrashReplay during a CPU upload");
  if (builder_.mutationCount() != 0 || mailbox_.pendingFrameCount() != 0)
    throw std::logic_error(
        "cannot reconfigure CrashReplay with unpublished/unread mutations");
  if (mailbox_.isClosed())
    throw std::logic_error("cannot reconfigure a closed CrashReplay mailbox");

  config_ = config;
  resetTimeline();
}

void CrashReplayRecorder::resetTimeline() {
  builder_ = CrashReplayFrameBuilder{};
  opcodeCoverage_.fill({});
  publishedOpcodeCoverage_.fill({});
  pendingRequiredByOpcode_.fill(0);
  nativeTileRevision_.fill(0);
  queuedTileRevision_.fill(0);
  clearNativeReadCache();
  nativeRevision_ = 0;
  nextPacketSequence_ = 0;
  frameSequenceBegin_ = 0;
  frameUnsupportedCount_ = 0;
  frameUnrecoverableCount_ = 0;
  surfaceSeeded_ = false;
  cpuUpload_ = {};
  pendingPaintRect_.reset();
}

void CrashReplayRecorder::requireEnabled() const {
  if (!enabled())
    throw std::logic_error("CrashReplay recorder is disabled");
}

void CrashReplayRecorder::requireNativeVram(
    std::span<const Color16> nativeVram) const {
  if (nativeVram.size() < NATIVE_PIXEL_COUNT)
    throw std::invalid_argument(
        "CrashReplay native VRAM span is smaller than 1024x512");
}

bool CrashReplayRecorder::isCrashReplayV1OpcodeSupported(uint8_t opcode) {
  // Polygon, VRAM copy and CPU-upload command lengths/semantics are selected
  // by the command family.  Keep every legal low-bit alias covered: those
  // bits encode raw-texture/semitransparency/primitive variants and are baked
  // into the recorded operation rather than being distinct replay handlers.
  if ((opcode >= 0x20 && opcode <= 0x7F) ||
      (opcode >= 0x80 && opcode <= 0x9F) ||
      (opcode >= 0xA0 && opcode <= 0xBF) ||
      (opcode >= 0xC0 && opcode <= 0xDF)) {
    return true;
  }
  switch (opcode) {
  case 0x00:
  case 0x01:
  case 0x02:
  case 0xE1:
  case 0xE2:
  case 0xE3:
  case 0xE4:
  case 0xE5:
  case 0xE6:
    return true;
  default:
    return false;
  }
}

bool CrashReplayRecorder::opcodeRequiresReplayOperation(uint8_t opcode) {
  if ((opcode >= 0x20 && opcode <= 0x7F) ||
      (opcode >= 0x80 && opcode <= 0x9F) ||
      (opcode >= 0xA0 && opcode <= 0xBF)) {
    return true;
  }
  switch (opcode) {
  case 0x02:
    return true;
  default:
    return false;
  }
}

uint64_t CrashReplayRecorder::observeOpcode(uint8_t opcode) {
  if (!enabled())
    return UNASSIGNED_PACKET_SEQUENCE;

  const uint64_t sequence = nextPacketSequence_;
  nextPacketSequence_ =
      checkedIncrement(nextPacketSequence_, "CrashReplay packet sequence");
  OpcodeCoverage &coverage = opcodeCoverage_[opcode];
  ++coverage.seen;
  if (!isCrashReplayV1OpcodeSupported(opcode)) {
    ++coverage.unsupported;
    ++frameUnsupportedCount_;
    if (!nativeSnapshotDominatesUnsupportedOpcode(opcode))
      ++frameUnrecoverableCount_;
    return sequence;
  }

  ++coverage.supported;
  if (opcodeRequiresReplayOperation(opcode))
    ++pendingRequiredByOpcode_[opcode];
  return sequence;
}

void CrashReplayRecorder::markPacketUnsupported(uint8_t opcode) {
  requireEnabled();
  if (!isCrashReplayV1OpcodeSupported(opcode)) {
    throw std::invalid_argument(
        "markPacketUnsupported expects a normally supported opcode");
  }
  OpcodeCoverage &coverage = opcodeCoverage_[opcode];
  if (coverage.seen == 0 || coverage.supported == 0)
    throw std::logic_error("unsupported packet was not observed first");
  if (pendingRequiredByOpcode_[opcode] != 0)
    --pendingRequiredByOpcode_[opcode];
  ++coverage.unsupported;
  ++frameUnsupportedCount_;
  if (!nativeSnapshotDominatesUnsupportedOpcode(opcode))
    ++frameUnrecoverableCount_;
}

void CrashReplayRecorder::markPacketRecorded(uint8_t opcode) {
  if (pendingRequiredByOpcode_[opcode] == 0)
    throw std::logic_error(
        "replay operation has no matching observed GP0 packet");
  --pendingRequiredByOpcode_[opcode];
  ++opcodeCoverage_[opcode].recorded;
}

void CrashReplayRecorder::recordSurfaceSeed(std::span<const Color16> nativeVram,
                                            uint64_t depthEpoch) {
  requireEnabled();
  requireNativeVram(nativeVram);
  if (depthEpoch == 0)
    throw std::invalid_argument("surface seed depth epoch zero is reserved");
  if (cpuUpload_.active)
    abortCpuUpload();
  flushPendingPaintRect();

  nativeRevision_ =
      checkedIncrement(nativeRevision_, "CrashReplay native revision");
  SurfaceSeed seed;
  seed.destination = {0, 0, NATIVE_WIDTH, NATIVE_HEIGHT};
  seed.pixels.resize(NATIVE_PIXEL_COUNT);
  std::transform(nativeVram.begin(), nativeVram.begin() + NATIVE_PIXEL_COUNT,
                 seed.pixels.begin(), [](Color16 pixel) { return pixel.raw; });
  seed.nativeRevision = nativeRevision_;
  seed.depthEpoch = depthEpoch;
  seed.replaceWholeSurface = true;
  builder_.record(std::move(seed));

  nativeTileRevision_.fill(nativeRevision_);
  queuedTileRevision_.fill(nativeRevision_);
  clearNativeReadCache();
  surfaceSeeded_ = true;
}

void CrashReplayRecorder::recordReset(Reset operation) {
  requireEnabled();
  if (cpuUpload_.active)
    abortCpuUpload();
  flushPendingPaintRect();
  builder_.record(std::move(operation));
}

void CrashReplayRecorder::recordClearDepth(ClearDepth operation) {
  requireEnabled();
  if (cpuUpload_.active)
    throw std::logic_error("depth clear cannot interleave a CPU upload");
  flushPendingPaintRect();
  builder_.record(std::move(operation));
}

CrashReplayRecorder::TileSet
CrashReplayRecorder::tilesForWrappedRect(NativeRect rect) const {
  TileSet result;
  if (rect.width == 0 || rect.height == 0)
    return result;

  struct Segment {
    uint32_t start = 0;
    uint32_t length = 0;
  };
  const auto segments = [](int32_t origin, uint32_t length, uint32_t limit) {
    std::array<Segment, 2> output{};
    if (length >= limit) {
      output[0] = {0, limit};
      return output;
    }
    const uint32_t start = wrapCoordinate(origin, limit);
    const uint32_t first = std::min(length, limit - start);
    output[0] = {start, first};
    if (first != length)
      output[1] = {0, length - first};
    return output;
  };

  auto xSegments = segments(rect.x, rect.width, NATIVE_WIDTH);
  auto ySegments = segments(rect.y, rect.height, NATIVE_HEIGHT);
  std::sort(
      xSegments.begin(), xSegments.end(),
      [](Segment left, Segment right) { return left.start < right.start; });
  std::sort(
      ySegments.begin(), ySegments.end(),
      [](Segment left, Segment right) { return left.start < right.start; });

  // Sorting the at-most-two wrapped axis segments makes the nested traversal
  // emit canonical row-major tile indices directly, without an 8192-bit scan.
  for (const Segment ys : ySegments) {
    if (ys.length == 0)
      continue;
    const uint32_t firstTileY = ys.start / TILE_HEIGHT;
    const uint32_t lastTileY = (ys.start + ys.length - 1u) / TILE_HEIGHT;
    for (uint32_t tileY = firstTileY; tileY <= lastTileY; ++tileY) {
      for (const Segment xs : xSegments) {
        if (xs.length == 0)
          continue;
        const uint32_t firstTileX = xs.start / TILE_WIDTH;
        const uint32_t lastTileX = (xs.start + xs.length - 1u) / TILE_WIDTH;
        for (uint32_t tileX = firstTileX; tileX <= lastTileX; ++tileX)
          result.push_back(tileY * TILES_X + tileX);
      }
    }
  }
  return result;
}

CrashReplayRecorder::TileSet
CrashReplayRecorder::nativeReadTiles(const TexturePageState &texturePage,
                                     const ClutState &clut) const {
  if (texturePage.textureDisable)
    return {};

  uint32_t textureWidth = 256;
  switch (texturePage.depth) {
  case TextureDepth::Indexed4:
    textureWidth = 64;
    break;
  case TextureDepth::Indexed8:
    textureWidth = 128;
    break;
  case TextureDepth::Direct15:
  case TextureDepth::Reserved:
    textureWidth = 256;
    break;
  }
  TileSet tiles = tilesForWrappedRect(
      {texturePage.baseX, texturePage.baseY, textureWidth, 256});

  TileSet clutTiles;
  if (texturePage.depth == TextureDepth::Indexed4) {
    clutTiles = tilesForWrappedRect({clut.x, clut.y, 16, 1});
  } else if (texturePage.depth == TextureDepth::Indexed8) {
    clutTiles = tilesForWrappedRect({clut.x, clut.y, 256, 1});
  }
  if (!clutTiles.empty()) {
    TileSet merged;
    merged.reserve(tiles.size() + clutTiles.size());
    std::set_union(tiles.begin(), tiles.end(), clutTiles.begin(),
                   clutTiles.end(), std::back_inserter(merged));
    tiles = std::move(merged);
  }
  return tiles;
}

NativeTileUpload
CrashReplayRecorder::snapshotTile(uint32_t tileIndex, uint64_t revision,
                                  std::span<const Color16> nativeVram) const {
  if (tileIndex >= TILE_COUNT)
    throw std::out_of_range("CrashReplay tile index is out of range");
  const uint32_t tileX = tileIndex % TILES_X;
  const uint32_t tileY = tileIndex / TILES_X;
  const uint32_t x = tileX * TILE_WIDTH;
  const uint32_t y = tileY * TILE_HEIGHT;

  NativeTileUpload upload;
  upload.destination = {static_cast<int32_t>(x), static_cast<int32_t>(y),
                        TILE_WIDTH, TILE_HEIGHT};
  upload.nativeRevision = revision;
  upload.pixels.resize(static_cast<std::size_t>(TILE_WIDTH) * TILE_HEIGHT);
  for (uint32_t row = 0; row < TILE_HEIGHT; ++row) {
    const std::size_t source =
        static_cast<std::size_t>(y + row) * NATIVE_WIDTH + x;
    const std::size_t destination = static_cast<std::size_t>(row) * TILE_WIDTH;
    std::transform(
        nativeVram.begin() + static_cast<std::ptrdiff_t>(source),
        nativeVram.begin() + static_cast<std::ptrdiff_t>(source + TILE_WIDTH),
        upload.pixels.begin() + static_cast<std::ptrdiff_t>(destination),
        [](Color16 pixel) { return pixel.raw; });
  }
  return upload;
}

NativeReadDependency
CrashReplayRecorder::prepareNativeRead(const TexturePageState &texturePage,
                                       const ClutState &clut,
                                       std::span<const Color16> nativeVram) {
  requireEnabled();
  requireNativeVram(nativeVram);
  if (!surfaceSeeded_)
    throw std::logic_error("native read requires a SurfaceSeed baseline");
  if (cpuUpload_.active)
    throw std::logic_error("native read cannot interleave a CPU upload");
  flushPendingPaintRect();

  if (texturePage.textureDisable)
    return {};

  const uint64_t key = nativeReadCacheKey(texturePage, clut);
  auto cached = nativeReadCacheIndex_.find(key);
  if (cached == nativeReadCacheIndex_.end()) {
    if (nativeReadCacheEntries_.size() >= NATIVE_READ_CACHE_LIMIT) {
      clearNativeReadCache();
      ps1::metrics::count("host.replay.capture.native_read_cache_capacity_reset");
    }
    const uint32_t index =
        static_cast<uint32_t>(nativeReadCacheEntries_.size());
    NativeReadCacheEntry entry;
    entry.tiles = nativeReadTiles(texturePage, clut);
    nativeReadCacheEntries_.push_back(std::move(entry));
    nativeReadCacheIndex_.emplace(key, index);
    for (const uint32_t tile : nativeReadCacheEntries_.back().tiles)
      nativeReadCacheSubscribers_[tile].push_back(index);
    cached = nativeReadCacheIndex_.find(key);
  }

  NativeReadCacheEntry &entry = nativeReadCacheEntries_[cached->second];
  if (entry.valid) {
    ++frameNativeReadCacheHits_;
    return entry.dependency;
  }
  ++frameNativeReadCacheMisses_;
  entry.dependency = prepareNativeTilesRead(entry.tiles, nativeVram);
  entry.valid = true;
  return entry.dependency;
}

uint64_t CrashReplayRecorder::nativeReadCacheKey(
    const TexturePageState &texturePage,
    const ClutState &clut) const noexcept {
  // Coordinates have native VRAM wrap semantics, so their low 10/9 bits are
  // the complete read-set identity. CLUT state is irrelevant to direct-color
  // pages and is normalized away to maximize exact reuse.
  const uint64_t baseX = texturePage.baseX & (NATIVE_WIDTH - 1u);
  const uint64_t baseY = texturePage.baseY & (NATIVE_HEIGHT - 1u);
  const uint64_t depth = static_cast<uint64_t>(texturePage.depth) & 3u;
  const bool indexed = texturePage.depth == TextureDepth::Indexed4 ||
                       texturePage.depth == TextureDepth::Indexed8;
  const uint64_t clutX = indexed ? clut.x & (NATIVE_WIDTH - 1u) : 0u;
  const uint64_t clutY = indexed ? clut.y & (NATIVE_HEIGHT - 1u) : 0u;
  return baseX | (baseY << 10u) | (depth << 19u) | (clutX << 21u) |
         (clutY << 31u);
}

void CrashReplayRecorder::invalidateNativeReadCacheTiles(
    std::span<const uint32_t> tileIndices) noexcept {
  for (const uint32_t tile : tileIndices) {
    if (tile >= TILE_COUNT)
      continue;
    for (const uint32_t cacheIndex : nativeReadCacheSubscribers_[tile]) {
      if (cacheIndex >= nativeReadCacheEntries_.size())
        continue;
      NativeReadCacheEntry &entry = nativeReadCacheEntries_[cacheIndex];
      if (entry.valid) {
        entry.valid = false;
        ++frameNativeReadCacheInvalidations_;
      }
    }
  }
}

void CrashReplayRecorder::clearNativeReadCache() noexcept {
  nativeReadCacheIndex_.clear();
  nativeReadCacheEntries_.clear();
  for (auto &subscribers : nativeReadCacheSubscribers_)
    subscribers.clear();
}

NativeReadDependency CrashReplayRecorder::prepareNativeRectRead(
    NativeRect source, std::span<const Color16> nativeVram) {
  requireEnabled();
  requireNativeVram(nativeVram);
  if (!surfaceSeeded_)
    throw std::logic_error("native read requires a SurfaceSeed baseline");
  if (cpuUpload_.active)
    throw std::logic_error("native read cannot interleave a CPU upload");
  if (source.width == 0 || source.height == 0 || source.width > NATIVE_WIDTH ||
      source.height > NATIVE_HEIGHT) {
    throw std::invalid_argument(
        "native rectangle read exceeds native VRAM dimensions");
  }
  flushPendingPaintRect();

  const TileSet readTiles = tilesForWrappedRect(source);
  return prepareNativeTilesRead(readTiles, nativeVram);
}

NativeReadDependency CrashReplayRecorder::prepareNativeTilesRead(
    std::span<const uint32_t> readTiles,
    std::span<const Color16> nativeVram) {
  if (readTiles.empty())
    throw std::logic_error("required native read has no source tiles");
  NativeReadDependency dependency;
  dependency.required = true;
  dependency.tileCount = static_cast<uint32_t>(readTiles.size());
  for (const uint32_t tileIndex : readTiles) {
    const uint64_t revision = nativeTileRevision_[tileIndex];
    if (queuedTileRevision_[tileIndex] != revision) {
      builder_.record(snapshotTile(tileIndex, revision, nativeVram));
      queuedTileRevision_[tileIndex] = revision;
    }
    dependency.versionHash = appendNativeReadDependencyHash(
        dependency.versionHash, tileIndex, revision);
  }
  return dependency;
}

void CrashReplayRecorder::recordDraw(DrawPacket packet,
                                     std::span<const Color16> nativeVram) {
  requireEnabled();
  if (!surfaceSeeded_)
    throw std::logic_error("draw requires a SurfaceSeed baseline");
  if (cpuUpload_.active)
    throw std::logic_error("draw cannot interleave a CPU upload");

  if (packet.textured && !packet.state.texturePage.textureDisable) {
    packet.nativeRead = prepareNativeRead(packet.state.texturePage,
                                          packet.state.clut, nativeVram);
  } else {
    flushPendingPaintRect();
    packet.nativeRead = {};
  }
  const uint8_t opcode = packet.opcode;
  builder_.record(std::move(packet));
  markPacketRecorded(opcode);
}

void CrashReplayRecorder::recordNoOp(uint8_t opcode) {
  requireEnabled();
  if (!surfaceSeeded_)
    throw std::logic_error("no-op requires a SurfaceSeed baseline");
  if (cpuUpload_.active)
    throw std::logic_error("no-op cannot interleave a CPU upload");
  flushPendingPaintRect();
  markPacketRecorded(opcode);
}

void CrashReplayRecorder::recordPresentationDraw(
    DrawPacket packet, std::span<const Color16> nativeVram) {
  requireEnabled();
  if (!surfaceSeeded_ || cpuUpload_.active)
    throw std::logic_error(
        "presentation draw requires an idle seeded timeline");
  if (!packet.originalMesh || !packet.originalTexture ||
      packet.vertexCount != 4 || packet.indexCount != 6 || !packet.textured ||
      packet.route != PacketRoute::Projected3D || packet.validMask != 15 ||
      packet.fallbackReason != PacketFallbackReason::None)
    throw std::invalid_argument(
        "presentation draw requires original source geometry and material");
  for (unsigned k = 0; k < packet.vertexCount; ++k)
    if (packet.vertices[k].provenance != ReplayVertexProvenance{})
      throw std::invalid_argument(
          "presentation draw cannot claim native command provenance");
  packet.packetSequence = nextPacketSequence_;
  packet.nativeRead = prepareNativeRead(packet.state.texturePage,
                                        packet.state.clut, nativeVram);
  builder_.record(std::move(packet));
  ++nextPacketSequence_;
  metrics::count("gpu.replay.presentation_draws");
}

void CrashReplayRecorder::recordFill(uint8_t opcode, FillRect operation) {
  requireEnabled();
  if (!surfaceSeeded_)
    throw std::logic_error("fill requires a SurfaceSeed baseline");
  if (cpuUpload_.active)
    throw std::logic_error("fill cannot interleave a CPU upload");
  flushPendingPaintRect();
  builder_.record(std::move(operation));
  markPacketRecorded(opcode);
}

void CrashReplayRecorder::recordCopy(uint8_t opcode, CopyRect operation,
                                     std::span<const Color16> nativeVram) {
  requireEnabled();
  if (!surfaceSeeded_)
    throw std::logic_error("copy requires a SurfaceSeed baseline");
  if (cpuUpload_.active)
    throw std::logic_error("copy cannot interleave a CPU upload");
  operation.nativeRead = prepareNativeRectRead(
      {operation.source.x, operation.source.y, operation.width, operation.height},
      nativeVram);
  builder_.record(std::move(operation));
  markPacketRecorded(opcode);
}

void CrashReplayRecorder::recordPaintPixels(uint8_t opcode,
                                            PaintPixels operation) {
  requireEnabled();
  if (!surfaceSeeded_)
    throw std::logic_error("paint requires a SurfaceSeed baseline");
  if (cpuUpload_.active)
    throw std::logic_error("paint cannot interleave a CPU upload");
  flushPendingPaintRect();
  std::bitset<TILE_COUNT> tileMembership;
  TileSet touchedTiles;
  touchedTiles.reserve(
      std::min<std::size_t>(operation.pixels.size(), TILE_COUNT));
  for (const PaintedPixel &pixel : operation.pixels) {
    const uint32_t x = wrapCoordinate(pixel.destination.x, NATIVE_WIDTH);
    const uint32_t y = wrapCoordinate(pixel.destination.y, NATIVE_HEIGHT);
    const uint32_t tileIndex = (y / TILE_HEIGHT) * TILES_X + x / TILE_WIDTH;
    if (!tileMembership.test(tileIndex)) {
      tileMembership.set(tileIndex);
      touchedTiles.push_back(tileIndex);
    }
  }
  builder_.record(std::move(operation));
  markPacketRecorded(opcode);
  markNativeTiles(touchedTiles);
}

bool CrashReplayRecorder::canCoalesce(const PaintRect &left,
                                      const PaintRect &right) const {
  if (left.destination.height != 1 || right.destination.height != 1 ||
      left.destination.y != right.destination.y ||
      left.destination.x + static_cast<int64_t>(left.destination.width) !=
          right.destination.x ||
      left.nativeColor != right.nativeColor || left.color != right.color ||
      left.mask != right.mask || left.depth != right.depth)
    return false;
  return left.destination.width <=
         std::numeric_limits<uint32_t>::max() - right.destination.width;
}

void CrashReplayRecorder::recordPaintRect(uint8_t opcode, PaintRect operation) {
  requireEnabled();
  if (!surfaceSeeded_)
    throw std::logic_error("paint requires a SurfaceSeed baseline");
  if (cpuUpload_.active)
    throw std::logic_error("paint cannot interleave a CPU upload");
  if (operation.destination.width == 0 || operation.destination.height == 0 ||
      operation.coalescedCommandCount !=
          static_cast<uint64_t>(operation.destination.width) *
              operation.destination.height ||
      !operation.depth.invalidateOnAccepted || operation.depth.testEnabled ||
      operation.depth.writeEnabled) {
    throw std::invalid_argument("PaintRect metadata is not replayable");
  }

  markPacketRecorded(opcode);
  if (pendingPaintRect_ && canCoalesce(*pendingPaintRect_, operation)) {
    pendingPaintRect_->destination.width += operation.destination.width;
    pendingPaintRect_->coalescedCommandCount += operation.coalescedCommandCount;
    return;
  }
  flushPendingPaintRect();
  pendingPaintRect_ = std::move(operation);
}

void CrashReplayRecorder::flushPendingPaintRect() {
  if (!pendingPaintRect_)
    return;
  builder_.record(std::move(*pendingPaintRect_));
  pendingPaintRect_.reset();
}

uint64_t CrashReplayRecorder::markNativeWrite(NativeRect destination) {
  requireEnabled();
  if (!surfaceSeeded_)
    throw std::logic_error("native write requires a SurfaceSeed baseline");
  const TileSet tiles = tilesForWrappedRect(destination);
  if (tiles.empty())
    return nativeRevision_;

  return markNativeTiles(tiles);
}

uint64_t CrashReplayRecorder::markNativeWrite(NativePoint destination) {
  requireEnabled();
  if (!surfaceSeeded_)
    throw std::logic_error("native write requires a SurfaceSeed baseline");
  const uint32_t x = wrapCoordinate(destination.x, NATIVE_WIDTH);
  const uint32_t y = wrapCoordinate(destination.y, NATIVE_HEIGHT);
  const uint32_t tileIndex = (y / TILE_HEIGHT) * TILES_X + (x / TILE_WIDTH);
  const std::array<uint32_t, 1> tiles{tileIndex};
  return markNativeTiles(tiles);
}

uint64_t CrashReplayRecorder::markNativeTiles(
    std::span<const uint32_t> tileIndices) {
  if (tileIndices.empty())
    return nativeRevision_;
  nativeRevision_ =
      checkedIncrement(nativeRevision_, "CrashReplay native revision");
  for (const uint32_t tileIndex : tileIndices) {
    if (tileIndex >= TILE_COUNT)
      throw std::out_of_range("CrashReplay native tile index is out of range");
    nativeTileRevision_[tileIndex] = nativeRevision_;
  }
  invalidateNativeReadCacheTiles(tileIndices);
  return nativeRevision_;
}

uint64_t CrashReplayRecorder::nativeTileRevision(uint32_t tileIndex) const {
  if (tileIndex >= TILE_COUNT)
    throw std::out_of_range("CrashReplay tile index is out of range");
  return nativeTileRevision_[tileIndex];
}

uint64_t CrashReplayRecorder::queuedTileRevision(uint32_t tileIndex) const {
  if (tileIndex >= TILE_COUNT)
    throw std::out_of_range("CrashReplay tile index is out of range");
  return queuedTileRevision_[tileIndex];
}

void CrashReplayRecorder::beginCpuUpload(uint8_t opcode, uint64_t transactionId,
                                         NativeRect transfer, MaskState mask) {
  requireEnabled();
  if (!surfaceSeeded_)
    throw std::logic_error("CPU upload requires a SurfaceSeed baseline");
  if (cpuUpload_.active)
    throw std::logic_error("a CPU upload is already active");
  if (transfer.width == 0 || transfer.height == 0)
    throw std::invalid_argument("CPU upload transfer must not be empty");
  if (transfer.width > NATIVE_WIDTH || transfer.height > NATIVE_HEIGHT)
    throw std::invalid_argument("CPU upload exceeds native VRAM dimensions");
  if (transactionId == UNASSIGNED_PACKET_SEQUENCE || nextPacketSequence_ == 0 ||
      transactionId != nextPacketSequence_ - 1u) {
    throw std::invalid_argument(
        "CPU upload transaction id is not the observed A0 header sequence");
  }
  flushPendingPaintRect();
  markPacketRecorded(opcode);

  cpuUpload_.active = true;
  cpuUpload_.opcode = opcode;
  cpuUpload_.transactionId = transactionId;
  cpuUpload_.transfer = transfer;
  cpuUpload_.mask = mask;
  cpuUpload_.totalPixels =
      static_cast<uint64_t>(transfer.width) * transfer.height;
  cpuUpload_.consumed = 0;
  cpuUpload_.chunkOffset = 0;
  cpuUpload_.pixels.clear();
  cpuUpload_.acceptedBits.clear();
  cpuUpload_.acceptedTileMembership.reset();
  cpuUpload_.acceptedTileIndices.clear();

  CpuUpload begin;
  begin.transactionId = transactionId;
  begin.phase = CpuUploadPhase::Begin;
  begin.transfer = transfer;
  begin.pixelOffset = 0;
  begin.mask = mask;
  builder_.record(std::move(begin));
}

NativePoint
CrashReplayRecorder::cpuUploadDestination(uint64_t transferPixelOffset) const {
  const uint64_t column = transferPixelOffset % cpuUpload_.transfer.width;
  const uint64_t row = transferPixelOffset / cpuUpload_.transfer.width;
  return {static_cast<int32_t>(
              wrapCoordinate(static_cast<int64_t>(cpuUpload_.transfer.x) +
                                 static_cast<int64_t>(column),
                             NATIVE_WIDTH)),
          static_cast<int32_t>(
              wrapCoordinate(static_cast<int64_t>(cpuUpload_.transfer.y) +
                                 static_cast<int64_t>(row),
                             NATIVE_HEIGHT))};
}

void CrashReplayRecorder::appendCpuUploadPixel(uint16_t sourcePixel,
                                               bool accepted) {
  requireEnabled();
  if (!cpuUpload_.active)
    throw std::logic_error("no CPU upload is active");
  if (cpuUpload_.consumed >= cpuUpload_.totalPixels)
    throw std::out_of_range("CPU upload received too many pixels");

  const std::size_t chunkIndex = cpuUpload_.pixels.size();
  if ((chunkIndex % 64u) == 0)
    cpuUpload_.acceptedBits.push_back(0);
  cpuUpload_.pixels.push_back(sourcePixel);
  if (accepted) {
    cpuUpload_.acceptedBits.back() |= uint64_t{1} << (chunkIndex % 64u);
    const NativePoint destination = cpuUploadDestination(cpuUpload_.consumed);
    const uint32_t tileX = static_cast<uint32_t>(destination.x) / TILE_WIDTH;
    const uint32_t tileY = static_cast<uint32_t>(destination.y) / TILE_HEIGHT;
    const uint32_t tileIndex = tileY * TILES_X + tileX;
    if (!cpuUpload_.acceptedTileMembership.test(tileIndex)) {
      cpuUpload_.acceptedTileMembership.set(tileIndex);
      cpuUpload_.acceptedTileIndices.push_back(tileIndex);
    }
  }
  ++cpuUpload_.consumed;
}

bool CrashReplayRecorder::flushCpuUpload() {
  requireEnabled();
  if (!cpuUpload_.active || cpuUpload_.pixels.empty())
    return false;

  CpuUpload operation;
  operation.transactionId = cpuUpload_.transactionId;
  operation.phase = CpuUploadPhase::Data;
  operation.transfer = cpuUpload_.transfer;
  operation.pixelOffset = cpuUpload_.chunkOffset;
  operation.pixels = std::move(cpuUpload_.pixels);
  operation.acceptedBits = std::move(cpuUpload_.acceptedBits);
  operation.mask = cpuUpload_.mask;
  builder_.record(std::move(operation));
  if (!cpuUpload_.acceptedTileIndices.empty()) {
    nativeRevision_ =
        checkedIncrement(nativeRevision_, "CrashReplay native revision");
    std::sort(cpuUpload_.acceptedTileIndices.begin(),
              cpuUpload_.acceptedTileIndices.end());
    for (const uint32_t tileIndex : cpuUpload_.acceptedTileIndices) {
      nativeTileRevision_[tileIndex] = nativeRevision_;
      cpuUpload_.acceptedTileMembership.reset(tileIndex);
    }
    invalidateNativeReadCacheTiles(cpuUpload_.acceptedTileIndices);
  }
  cpuUpload_.pixels.clear();
  cpuUpload_.acceptedBits.clear();
  cpuUpload_.acceptedTileIndices.clear();
  cpuUpload_.chunkOffset = cpuUpload_.consumed;
  return true;
}

void CrashReplayRecorder::finishCpuUpload() {
  requireEnabled();
  if (!cpuUpload_.active)
    throw std::logic_error("no CPU upload is active");
  if (cpuUpload_.consumed != cpuUpload_.totalPixels)
    throw std::logic_error("CPU upload finished before every pixel arrived");
  flushCpuUpload();
  CpuUpload complete;
  complete.transactionId = cpuUpload_.transactionId;
  complete.phase = CpuUploadPhase::Complete;
  complete.transfer = cpuUpload_.transfer;
  complete.pixelOffset = cpuUpload_.consumed;
  complete.mask = cpuUpload_.mask;
  builder_.record(std::move(complete));
  cpuUpload_.active = false;
}

void CrashReplayRecorder::abortCpuUpload() {
  requireEnabled();
  if (!cpuUpload_.active)
    return;
  flushCpuUpload();
  CpuUpload abort;
  abort.transactionId = cpuUpload_.transactionId;
  abort.phase = CpuUploadPhase::Abort;
  abort.transfer = cpuUpload_.transfer;
  abort.pixelOffset = cpuUpload_.consumed;
  abort.mask = cpuUpload_.mask;
  builder_.record(std::move(abort));
  cpuUpload_.active = false;
}

uint64_t CrashReplayRecorder::pendingRequiredCount() const {
  uint64_t total = 0;
  for (const uint64_t count : pendingRequiredByOpcode_) {
    if (count > std::numeric_limits<uint64_t>::max() - total)
      throw std::overflow_error("CrashReplay pending packet count overflowed");
    total += count;
  }
  return total;
}

CrashReplayFramePtr CrashReplayRecorder::sealAndPublish(FrameLatch latch) {
  requireEnabled();
  flushCpuUpload();
  flushPendingPaintRect();

  const uint64_t missing = pendingRequiredCount();
  // A required packet which reached the frame boundary without an operation
  // is a durable coverage failure, not just a transient latch statistic.
  // Preserve it in the per-opcode totals before opening the next builder.
  for (uint32_t opcode = 0; opcode < pendingRequiredByOpcode_.size();
       ++opcode) {
    const uint64_t count = pendingRequiredByOpcode_[opcode];
    if (count > std::numeric_limits<uint64_t>::max() -
                    opcodeCoverage_[opcode].unsupported) {
      throw std::overflow_error("CrashReplay opcode coverage overflowed");
    }
    opcodeCoverage_[opcode].unsupported += count;
    if (!nativeSnapshotDominatesUnsupportedOpcode(
            static_cast<uint8_t>(opcode))) {
      if (count > std::numeric_limits<uint64_t>::max() -
                      frameUnrecoverableCount_) {
        throw std::overflow_error(
            "CrashReplay unrecoverable packet count overflowed");
      }
      frameUnrecoverableCount_ += count;
    }
  }
  if (frameUnsupportedCount_ >
          std::numeric_limits<uint64_t>::max() - latch.unsupportedCount ||
      missing > std::numeric_limits<uint64_t>::max() - latch.unsupportedCount -
                    frameUnsupportedCount_) {
    throw std::overflow_error("CrashReplay frame unsupported count overflowed");
  }
  latch.unsupportedCount += frameUnsupportedCount_ + missing;
  if (frameUnrecoverableCount_ >
      std::numeric_limits<uint64_t>::max() - latch.unrecoverableCount) {
    throw std::overflow_error(
        "CrashReplay frame unrecoverable count overflowed");
  }
  latch.unrecoverableCount += frameUnrecoverableCount_;
  latch.complete = latch.complete && latch.unsupportedCount == 0;
  latch.sequenceBegin = frameSequenceBegin_;
  latch.sequenceEnd = nextPacketSequence_;
  latch.policy = policy();

  if (ps1::metrics::enabled()) {
    char name[64];
    const auto publishDelta = [&](uint32_t opcode, const char *field,
                                  uint64_t value, uint64_t previous) {
      if (value == previous)
        return;
      std::snprintf(name, sizeof(name),
                    "host.replay.capture.op.%02X.%s", opcode, field);
      ps1::metrics::count(name, value - previous);
    };
    for (uint32_t opcode = 0; opcode < opcodeCoverage_.size(); ++opcode) {
      const OpcodeCoverage &coverage = opcodeCoverage_[opcode];
      const OpcodeCoverage &published = publishedOpcodeCoverage_[opcode];
      publishDelta(opcode, "seen", coverage.seen, published.seen);
      publishDelta(opcode, "supported", coverage.supported,
                   published.supported);
      publishDelta(opcode, "recorded", coverage.recorded,
                   published.recorded);
      publishDelta(opcode, "unsupported", coverage.unsupported,
                   published.unsupported);
      publishedOpcodeCoverage_[opcode] = coverage;
    }
    ps1::metrics::count("host.replay.capture.frame");
    if (latch.unsupportedCount != 0) {
      ps1::metrics::count("host.replay.capture.frame_incomplete");
      ps1::metrics::count("host.replay.capture.unsupported_mutations",
                          latch.unsupportedCount);
    }
    if (latch.unrecoverableCount != 0) {
      ps1::metrics::count("host.replay.capture.frame_unrecoverable");
      ps1::metrics::count("host.replay.capture.unrecoverable_mutations",
                          latch.unrecoverableCount);
    }
    ps1::metrics::count("host.replay.capture.native_read_cache_hit",
                        frameNativeReadCacheHits_);
    ps1::metrics::count("host.replay.capture.native_read_cache_miss",
                        frameNativeReadCacheMisses_);
    ps1::metrics::count("host.replay.capture.native_read_cache_invalidation",
                        frameNativeReadCacheInvalidations_);
  }

  CrashReplayFramePtr frame = builder_.seal(latch);
  mailbox_.publish(frame);
  hasPublishedLatch_ = true;

  builder_ = CrashReplayFrameBuilder{};
  pendingRequiredByOpcode_.fill(0);
  frameUnsupportedCount_ = 0;
  frameUnrecoverableCount_ = 0;
  frameSequenceBegin_ = nextPacketSequence_;
  frameNativeReadCacheHits_ = 0;
  frameNativeReadCacheMisses_ = 0;
  frameNativeReadCacheInvalidations_ = 0;
  return frame;
}

} // namespace ps1::gpu::replay
