#include "runtime/gpu/modern_replay_preflight.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

namespace ps1::gpu::replay {
namespace {

using NativeTileSet = std::vector<uint32_t>;

struct AxisSegment {
  uint32_t first = 0;
  uint32_t last = 0;
};

struct AxisSegments {
  std::array<AxisSegment, 2> values{};
  uint32_t count = 0;
};

uint32_t wrapCoordinate(int32_t value, uint32_t extent) {
  int64_t wrapped = static_cast<int64_t>(value) % extent;
  if (wrapped < 0)
    wrapped += extent;
  return static_cast<uint32_t>(wrapped);
}

AxisSegments splitWrappedAxis(int32_t origin, uint32_t length,
                              uint32_t extent) {
  AxisSegments result;
  if (length == 0)
    return result;
  if (length >= extent) {
    result.values[0] = AxisSegment{0, extent - 1};
    result.count = 1;
    return result;
  }

  const uint32_t first = wrapCoordinate(origin, extent);
  const uint32_t beforeWrap = std::min(length, extent - first);
  result.values[result.count++] = AxisSegment{first, first + beforeWrap - 1};
  if (beforeWrap != length) {
    result.values[result.count++] = AxisSegment{0, length - beforeWrap - 1};
  }
  return result;
}

NativeTileSet wrappedRectTiles(const NativeRect &rect) {
  NativeTileSet tiles;
  AxisSegments xs =
      splitWrappedAxis(rect.x, rect.width, CRASH_REPLAY_NATIVE_WIDTH);
  AxisSegments ys =
      splitWrappedAxis(rect.y, rect.height, CRASH_REPLAY_NATIVE_HEIGHT);

  std::sort(xs.values.begin(), xs.values.begin() + xs.count,
            [](AxisSegment left, AxisSegment right) {
              return left.first < right.first;
            });
  std::sort(ys.values.begin(), ys.values.begin() + ys.count,
            [](AxisSegment left, AxisSegment right) {
              return left.first < right.first;
            });

  // Sorted wrapped segments emit unique row-major tile indices directly.
  // Runtime cost is proportional to the rectangle, never all native tiles.
  for (uint32_t yi = 0; yi < ys.count; ++yi) {
    const uint32_t firstTileY =
        ys.values[yi].first / CRASH_REPLAY_NATIVE_TILE_HEIGHT;
    const uint32_t lastTileY =
        ys.values[yi].last / CRASH_REPLAY_NATIVE_TILE_HEIGHT;
    for (uint32_t tileY = firstTileY; tileY <= lastTileY; ++tileY) {
      for (uint32_t xi = 0; xi < xs.count; ++xi) {
        const uint32_t firstTileX =
            xs.values[xi].first / CRASH_REPLAY_NATIVE_TILE_WIDTH;
        const uint32_t lastTileX =
            xs.values[xi].last / CRASH_REPLAY_NATIVE_TILE_WIDTH;
        for (uint32_t tileX = firstTileX; tileX <= lastTileX; ++tileX) {
          tiles.push_back(tileY * CRASH_REPLAY_NATIVE_TILE_COLUMNS + tileX);
        }
      }
    }
  }
  return tiles;
}

NativeTileSet conservativeNativeReadTiles(const DrawPacket &packet) {
  if (!packet.textured || packet.state.texturePage.textureDisable)
    return {};

  // These are the same canonical decoded fields used by the producer when it
  // snapshots dirty tiles. Raw-vs-decoded agreement is checked separately by
  // preflight before either representation can reach the renderer.
  const TexturePageState &page = packet.state.texturePage;
  uint32_t packedWidth = 256;
  switch (page.depth) {
  case TextureDepth::Indexed4:
    packedWidth = 64;
    break;
  case TextureDepth::Indexed8:
    packedWidth = 128;
    break;
  case TextureDepth::Direct15:
  case TextureDepth::Reserved:
    break;
  }

  NativeTileSet tiles =
      wrappedRectTiles(NativeRect{page.baseX, page.baseY, packedWidth, 256});
  if (page.depth == TextureDepth::Indexed4 ||
      page.depth == TextureDepth::Indexed8) {
    const uint32_t clutWidth =
        page.depth == TextureDepth::Indexed4 ? 16u : 256u;
    const NativeTileSet clutTiles = wrappedRectTiles(
        NativeRect{packet.state.clut.x, packet.state.clut.y, clutWidth, 1});
    NativeTileSet merged;
    merged.reserve(tiles.size() + clutTiles.size());
    std::set_union(tiles.begin(), tiles.end(), clutTiles.begin(),
                   clutTiles.end(), std::back_inserter(merged));
    tiles = std::move(merged);
  }
  return tiles;
}

bool isNativeRectBounded(const NativeRect &rect) {
  return rect.width != 0 && rect.height != 0 &&
         rect.width <= CRASH_REPLAY_NATIVE_WIDTH &&
         rect.height <= CRASH_REPLAY_NATIVE_HEIGHT;
}

bool isWholeNativeSurface(const NativeRect &rect) {
  return rect.x == 0 && rect.y == 0 &&
         rect.width == CRASH_REPLAY_NATIVE_WIDTH &&
         rect.height == CRASH_REPLAY_NATIVE_HEIGHT;
}

bool isTileAligned(const NativeRect &rect) {
  return isNativeRectBounded(rect) &&
         (wrapCoordinate(rect.x, CRASH_REPLAY_NATIVE_WIDTH) %
              CRASH_REPLAY_NATIVE_TILE_WIDTH ==
          0) &&
         (wrapCoordinate(rect.y, CRASH_REPLAY_NATIVE_HEIGHT) %
              CRASH_REPLAY_NATIVE_TILE_HEIGHT ==
          0) &&
         rect.width % CRASH_REPLAY_NATIVE_TILE_WIDTH == 0 &&
         rect.height % CRASH_REPLAY_NATIVE_TILE_HEIGHT == 0;
}

bool isValidAperture(const NativeRect &aperture) {
  if (aperture.x < 0 || aperture.y < 0 || aperture.width == 0 ||
      aperture.height == 0) {
    return false;
  }
  const uint64_t right = static_cast<uint64_t>(aperture.x) + aperture.width;
  const uint64_t bottom = static_cast<uint64_t>(aperture.y) + aperture.height;
  return right <= CRASH_REPLAY_NATIVE_WIDTH &&
         bottom <= CRASH_REPLAY_NATIVE_HEIGHT;
}

ReplayCapability operationCapability(const Operation &operation) {
  return std::visit(
      []<typename T>(const T &) -> ReplayCapability {
        if constexpr (std::is_same_v<T, NativeTileUpload>)
          return ReplayCapability::NativeTileUpload;
        else if constexpr (std::is_same_v<T, SurfaceSeed>)
          return ReplayCapability::SurfaceSeed;
        else if constexpr (std::is_same_v<T, DrawPacket>)
          return ReplayCapability::DrawPacket;
        else if constexpr (std::is_same_v<T, FillRect>)
          return ReplayCapability::FillRect;
        else if constexpr (std::is_same_v<T, CopyRect>)
          return ReplayCapability::CopyRect;
        else if constexpr (std::is_same_v<T, CpuUpload>)
          return ReplayCapability::CpuUpload;
        else if constexpr (std::is_same_v<T, PaintPixels>)
          return ReplayCapability::PaintPixels;
        else if constexpr (std::is_same_v<T, PaintRect>)
          return ReplayCapability::PaintRect;
        else if constexpr (std::is_same_v<T, ClearDepth>)
          return ReplayCapability::ClearDepth;
        else if constexpr (std::is_same_v<T, Reset>)
          return ReplayCapability::Reset;
        else
          return ReplayCapability::FrameLatch;
      },
      operation);
}

ReplayCapability fallbackCapability(PacketFallbackReason reason) {
  switch (reason) {
  case PacketFallbackReason::InvalidWPacketAtomic:
    return ReplayCapability::FallbackInvalidW;
  case PacketFallbackReason::MissingPrecisionPacketAtomic:
    return ReplayCapability::FallbackMissingPrecision;
  case PacketFallbackReason::ProjectiveContextMismatch:
    return ReplayCapability::FallbackProjectiveContext;
  case PacketFallbackReason::RasterSetupRejected:
    return ReplayCapability::FallbackRasterSetup;
  case PacketFallbackReason::UnsupportedFeature:
    return ReplayCapability::FallbackUnsupportedFeature;
  case PacketFallbackReason::None:
    break;
  }
  return ReplayCapability::DrawPacket;
}

ReplayCapability textureCapability(TextureDepth depth) {
  switch (depth) {
  case TextureDepth::Indexed4:
    return ReplayCapability::TextureIndexed4;
  case TextureDepth::Indexed8:
    return ReplayCapability::TextureIndexed8;
  case TextureDepth::Direct15:
    return ReplayCapability::TextureDirect15;
  case TextureDepth::Reserved:
    return ReplayCapability::TextureReserved;
  }
  return ReplayCapability::DrawPacket;
}

bool hasCapability(const ReplayPreflightRequest &request,
                   ReplayCapability capability) {
  return (request.capabilities & replayCapabilityBit(capability)) != 0;
}

bool requireCapability(ReplayPreflightResult &result,
                       const ReplayPreflightRequest &request,
                       ReplayCapability capability) {
  if (hasCapability(request, capability))
    return true;
  result.reason = ReplayPreflightReason::MissingCapability;
  result.missingCapabilities |= replayCapabilityBit(capability);
  return false;
}

bool decodedTextureStateMatchesRaw(const DrawPacket &packet) {
  const TexturePageState &page = packet.state.texturePage;
  const uint16_t rawPage = page.raw;
  if (page.baseX != static_cast<uint16_t>((rawPage & 0x0fu) * 64u) ||
      page.baseY != static_cast<uint16_t>(((rawPage >> 4u) & 1u) * 256u) ||
      page.depth != static_cast<TextureDepth>((rawPage >> 7u) & 3u) ||
      page.textureDisable != ((rawPage & (1u << 11u)) != 0) ||
      page.xFlip != ((rawPage & (1u << 12u)) != 0) ||
      page.yFlip != ((rawPage & (1u << 13u)) != 0)) {
    return false;
  }
  switch (packet.state.blend.mode) {
  case BlendMode::HalfBackgroundPlusHalfForeground:
  case BlendMode::BackgroundPlusForeground:
  case BlendMode::BackgroundMinusForeground:
  case BlendMode::BackgroundPlusQuarterForeground:
    break;
  default:
    return false;
  }
  if (packet.state.blend.mode != static_cast<BlendMode>((rawPage >> 5u) & 3u) ||
      packet.state.dither != ((rawPage & (1u << 9u)) != 0) ||
      packet.state.drawToDisplayArea != ((rawPage & (1u << 10u)) != 0) ||
      packet.state.blend.enabled != ((packet.opcode & 0x02u) != 0) ||
      packet.state.blend.texturedStpOnly !=
          (packet.textured && packet.state.blend.enabled)) {
    return false;
  }

  switch (packet.state.depth.compare) {
  case DepthCompare::Always:
  case DepthCompare::Less:
  case DepthCompare::LessOrEqual:
    break;
  default:
    return false;
  }

  const ClutState &clut = packet.state.clut;
  if (clut.x != static_cast<uint16_t>((clut.raw & 0x3fu) * 16u) ||
      clut.y != static_cast<uint16_t>((clut.raw >> 6u) & 0x1ffu)) {
    return false;
  }

  const TextureWindowState &window = packet.state.textureWindow;
  if (window.maskX != static_cast<uint8_t>((window.raw & 0x1fu) * 8u) ||
      window.maskY != static_cast<uint8_t>(((window.raw >> 5u) & 0x1fu) * 8u) ||
      window.offsetX !=
          static_cast<uint8_t>(((window.raw >> 10u) & 0x1fu) * 8u) ||
      window.offsetY !=
          static_cast<uint8_t>(((window.raw >> 15u) & 0x1fu) * 8u)) {
    return false;
  }
  return true;
}

bool validDepthCompare(DepthCompare compare) {
  switch (compare) {
  case DepthCompare::Always:
  case DepthCompare::Less:
  case DepthCompare::LessOrEqual:
    return true;
  }
  return false;
}

bool validReplayPolicy(const ReplayPolicyState &policy) {
  if (policy.internalScale == 0 || policy.internalScale > 8)
    return false;
  switch (policy.trueDepthMode) {
  case ReplayDepthMode::Off:
  case ReplayDepthMode::Hybrid:
  case ReplayDepthMode::Strict:
    return true;
  }
  return false;
}

bool validateDepthReference(const DepthState &depth,
                            const ReplayPreflightState &state,
                            ReplayPreflightResult &result) {
  if (!validDepthCompare(depth.compare)) {
    result.reason = ReplayPreflightReason::InvalidDepthCompare;
    return false;
  }
  if (!state.depthInitialized) {
    result.reason = ReplayPreflightReason::DepthNotInitialized;
    return false;
  }
  if (depth.epoch != state.depthEpoch) {
    result.reason = ReplayPreflightReason::DepthEpochMismatch;
    return false;
  }
  return true;
}

bool advanceDepthEpoch(uint64_t nextEpoch, ReplayPreflightState &state,
                       ReplayPreflightResult &result) {
  if (!state.depthInitialized) {
    result.reason = ReplayPreflightReason::DepthNotInitialized;
    return false;
  }
  if (state.depthEpoch == std::numeric_limits<uint64_t>::max()) {
    result.reason = ReplayPreflightReason::DepthEpochOverflow;
    return false;
  }
  if (nextEpoch != state.depthEpoch + 1u) {
    result.reason = ReplayPreflightReason::DepthEpochDiscontinuity;
    return false;
  }
  state.depthEpoch = nextEpoch;
  return true;
}

uint64_t cpuUploadPixelCount(const NativeRect &transfer) {
  return static_cast<uint64_t>(transfer.width) * transfer.height;
}

bool applyCpuUpload(const CpuUpload &upload, const FrameLatch &latch,
                    ReplayPreflightState &state,
                    ReplayPreflightResult &result) {
  ReplayCpuUploadState &transaction = state.cpuUpload;
  switch (upload.phase) {
  case CpuUploadPhase::Begin:
    if (transaction.active) {
      result.reason = ReplayPreflightReason::CpuUploadDuplicateBegin;
      return false;
    }
    if (upload.transactionId < latch.sequenceBegin ||
        upload.transactionId >= latch.sequenceEnd) {
      result.reason = ReplayPreflightReason::CpuUploadBeginOutOfRange;
      return false;
    }
    transaction.active = true;
    transaction.transactionId = upload.transactionId;
    transaction.transfer = upload.transfer;
    transaction.mask = upload.mask;
    transaction.nextOffset = 0;
    return true;

  case CpuUploadPhase::Data:
  case CpuUploadPhase::Complete:
  case CpuUploadPhase::Abort:
    if (!transaction.active) {
      result.reason = ReplayPreflightReason::CpuUploadMissingBegin;
      return false;
    }
    break;
  default:
    result.reason = ReplayPreflightReason::InvalidOperationLayout;
    return false;
  }

  if (upload.transactionId != transaction.transactionId) {
    result.reason = ReplayPreflightReason::CpuUploadTransactionMismatch;
    return false;
  }
  if (upload.transfer != transaction.transfer ||
      upload.mask != transaction.mask) {
    result.reason = ReplayPreflightReason::CpuUploadMetadataMismatch;
    return false;
  }
  if (upload.pixelOffset != transaction.nextOffset) {
    result.reason = ReplayPreflightReason::CpuUploadOffsetDiscontinuity;
    return false;
  }

  const uint64_t totalPixels = cpuUploadPixelCount(transaction.transfer);
  if (upload.phase == CpuUploadPhase::Data) {
    if (transaction.nextOffset > totalPixels ||
        upload.pixels.size() > totalPixels - transaction.nextOffset) {
      result.reason = ReplayPreflightReason::CpuUploadOffsetDiscontinuity;
      return false;
    }
    transaction.nextOffset += upload.pixels.size();
    return true;
  }

  const bool complete = upload.phase == CpuUploadPhase::Complete;
  if ((complete && transaction.nextOffset != totalPixels) ||
      transaction.nextOffset > totalPixels) {
    result.reason = ReplayPreflightReason::CpuUploadOffsetDiscontinuity;
    return false;
  }
  transaction = ReplayCpuUploadState{};
  return true;
}

bool validateNativeReadDependency(const NativeReadDependency &dependency,
                                  const NativeTileSet &tiles, bool required,
                                  const ReplayPreflightState &state,
                                  ReplayPreflightResult &result) {
  if (dependency.tileWidth != CRASH_REPLAY_NATIVE_TILE_WIDTH ||
      dependency.tileHeight != CRASH_REPLAY_NATIVE_TILE_HEIGHT ||
      dependency.required != required ||
      (!required &&
       (dependency.tileCount != 0 ||
        dependency.versionHash != CRASH_REPLAY_READ_HASH_OFFSET))) {
    result.reason = ReplayPreflightReason::NativeReadLayoutMismatch;
    return false;
  }
  if (!required)
    return true;

  ++result.counters.nativeReadPackets;
  result.counters.nativeReadTiles += tiles.size();
  if (dependency.tileCount != tiles.size()) {
    result.reason = ReplayPreflightReason::NativeReadCountMismatch;
    return false;
  }

  uint64_t hash = CRASH_REPLAY_READ_HASH_OFFSET;
  for (const uint32_t tile : tiles) {
    if (!state.knownNativeTiles.test(tile)) {
      result.reason = ReplayPreflightReason::NativeReadTileUnknown;
      return false;
    }
    hash = appendNativeReadDependencyHash(hash, tile,
                                          state.nativeTileRevisions[tile]);
  }
  if (hash != dependency.versionHash) {
    result.reason = ReplayPreflightReason::NativeReadHashMismatch;
    return false;
  }
  return true;
}

bool validateNativeRead(const DrawPacket &packet,
                        const ReplayPreflightState &state,
                        ReplayPreflightResult &result) {
  const bool required =
      packet.textured && !packet.state.texturePage.textureDisable;
  return validateNativeReadDependency(packet.nativeRead,
                                      conservativeNativeReadTiles(packet),
                                      required, state, result);
}

bool validateNativeRead(const CopyRect &copy,
                        const ReplayPreflightState &state,
                        ReplayPreflightResult &result) {
  const NativeTileSet tiles = wrappedRectTiles(
      {copy.source.x, copy.source.y, copy.width, copy.height});
  return validateNativeReadDependency(copy.nativeRead, tiles, true, state,
                                      result);
}

bool validateDrawCapabilities(const DrawPacket &packet,
                              const ReplayPreflightRequest &request,
                              ReplayPreflightResult &result) {
  switch (packet.route) {
  case PacketRoute::Projected3D: {
    if (!requireCapability(result, request, ReplayCapability::Projected3D))
      return false;
    if (!requireCapability(result, request,
                           ReplayCapability::NearPlaneSaturate))
      return false;
    break;
  }
  case PacketRoute::Screen2D:
    if (!requireCapability(result, request, ReplayCapability::Screen2D))
      return false;
    break;
  default:
    result.reason = ReplayPreflightReason::InvalidOperationLayout;
    return false;
  }

  if (packet.fallbackReason != PacketFallbackReason::None &&
      !requireCapability(result, request,
                         fallbackCapability(packet.fallbackReason))) {
    return false;
  }

  if (!requireCapability(result, request,
                         packet.state.blend.enabled
                             ? ReplayCapability::DrawSemiTransparent
                             : ReplayCapability::DrawOpaque)) {
    return false;
  }
  if (!requireCapability(result, request,
                         packet.textured ? ReplayCapability::DrawTextured
                                         : ReplayCapability::DrawUntextured)) {
    return false;
  }
  if (packet.state.blend.enabled &&
      !requireCapability(result, request,
                         ReplayCapability::DestinationColorFeedback)) {
    return false;
  }
  if (packet.state.mask.checkMaskBit &&
      !requireCapability(result, request,
                         ReplayCapability::DestinationMaskRead)) {
    return false;
  }
  if (packet.state.mask.forceMaskBit &&
      !requireCapability(result, request, ReplayCapability::ForceMaskWrite)) {
    return false;
  }

  const DepthState &depth = packet.state.depth;
  ReplayCapability depthCapability = ReplayCapability::DepthNonCanonical;
  if (depth.testEnabled && depth.writeEnabled &&
      depth.compare == DepthCompare::LessOrEqual &&
      !depth.invalidateOnAccepted) {
    depthCapability = ReplayCapability::DepthTestWriteLessEqual;
  } else if (!depth.testEnabled && depth.writeEnabled &&
             depth.compare == DepthCompare::Always &&
             !depth.invalidateOnAccepted) {
    depthCapability = ReplayCapability::DepthPainterWrite;
  } else if (!depth.testEnabled && !depth.writeEnabled &&
             !depth.invalidateOnAccepted) {
    depthCapability = ReplayCapability::DepthPainterOrdered;
  } else if (!depth.testEnabled && !depth.writeEnabled &&
             depth.invalidateOnAccepted) {
    depthCapability = ReplayCapability::DepthInvalidate;
  }
  if (!requireCapability(result, request, depthCapability))
    return false;

  if (packet.textured) {
    if (packet.state.texturePage.textureDisable) {
      if (!requireCapability(result, request,
                             ReplayCapability::TextureDisable)) {
        return false;
      }
    } else if (!requireCapability(
                   result, request,
                   textureCapability(packet.state.texturePage.depth))) {
      return false;
    }
  }
  return true;
}

bool applyNativeTileUpload(const NativeTileUpload &upload,
                           ReplayPreflightState &state,
                           ReplayPreflightResult &result) {
  if (!isTileAligned(upload.destination)) {
    result.reason = ReplayPreflightReason::NativeTileGeometryInvalid;
    return false;
  }
  const NativeTileSet tiles = wrappedRectTiles(upload.destination);
  for (const uint32_t tile : tiles) {
    if (state.knownNativeTiles.test(tile) &&
        upload.nativeRevision <= state.nativeTileRevisions[tile]) {
      result.reason = ReplayPreflightReason::NativeRevisionNotAdvanced;
      return false;
    }
  }
  for (const uint32_t tile : tiles) {
    state.nativeTileRevisions[tile] = upload.nativeRevision;
    state.knownNativeTiles.set(tile);
  }
  ++result.counters.nativeTileUploads;
  result.counters.nativeTilesTouched += tiles.size();
  return true;
}

bool applySurfaceSeed(const SurfaceSeed &seed, ReplayPreflightState &state,
                      ReplayPreflightResult &result) {
  // A seed is the one operation allowed to replace the revision timeline.  A
  // partial rectangle cannot prove the untouched words of an unknown tile,
  // so CrashReplayV1 only treats a full replacement as a valid re-anchor.
  if (!seed.replaceWholeSurface || !isWholeNativeSurface(seed.destination)) {
    result.reason = ReplayPreflightReason::NativeTileGeometryInvalid;
    return false;
  }
  state.nativeTileRevisions.fill(seed.nativeRevision);
  state.knownNativeTiles.set();
  state.surfaceInitialized = true;
  state.depthInitialized = true;
  state.depthEpoch = seed.depthEpoch;
  result.counters.nativeTilesTouched += CRASH_REPLAY_NATIVE_TILE_COUNT;
  return true;
}

bool operationNeedsInitializedSurface(const Operation &operation) {
  return std::holds_alternative<DrawPacket>(operation) ||
         std::holds_alternative<FillRect>(operation) ||
         std::holds_alternative<CopyRect>(operation) ||
         std::holds_alternative<CpuUpload>(operation) ||
         std::holds_alternative<PaintPixels>(operation) ||
         std::holds_alternative<PaintRect>(operation);
}

bool displayFlagsEqual(const DisplayLatchState &left,
                       const DisplayLatchState &right) {
  return left.enabled == right.enabled && left.pal == right.pal &&
         left.interlaced == right.interlaced && left.oddField == right.oddField;
}

} // namespace

std::vector<uint32_t>
conservativeNativeReadTileIndices(const DrawPacket &packet) {
  return conservativeNativeReadTiles(packet);
}

std::vector<uint32_t>
conservativeNativeReadTileIndices(const CopyRect &copy) {
  return wrappedRectTiles(
      {copy.source.x, copy.source.y, copy.width, copy.height});
}

namespace {

enum class ReplayEvaluationMode : uint8_t {
  StrictM1,
  NativeSnapshotAnchor,
};

ReplayPreflightResult
evaluateCrashReplay(std::span<const CrashReplayFramePtr> fifoFrames,
                    const ReplayPreflightRequest &request,
                    const ReplayPreflightState &confirmedState,
                    ReplayEvaluationMode mode,
                    const CrashReplayFrame *exactTargetFrame = nullptr,
                    bool allowIncompleteNativeReanchor = false) {
  const bool nativeSnapshotAnchor =
      mode == ReplayEvaluationMode::NativeSnapshotAnchor;
  const bool hardNativeReanchor = nativeSnapshotAnchor &&
                                  allowIncompleteNativeReanchor &&
                                  exactTargetFrame != nullptr;
  ReplayPreflightResult result;
  result.candidateState = confirmedState;
  result.counters.inputFrames = fifoFrames.size();

  if (request.schemaName != CRASH_REPLAY_SCHEMA_NAME) {
    result.reason = ReplayPreflightReason::SchemaNameMismatch;
    return result;
  }
  if (request.schemaVersion != CRASH_REPLAY_SCHEMA_VERSION) {
    result.reason = ReplayPreflightReason::SchemaVersionMismatch;
    return result;
  }
  if (!isValidAperture(request.expectedLatch.display.aperture)) {
    result.reason = ReplayPreflightReason::InvalidExpectedAperture;
    return result;
  }
  if (!validReplayPolicy(request.expectedPolicy)) {
    result.reason = ReplayPreflightReason::InvalidExpectedPolicy;
    return result;
  }
  switch (request.nearClipMode) {
  case ReplayNearClipMode::Saturate:
    break;
  case ReplayNearClipMode::GeometricClip:
  default:
    result.reason = ReplayPreflightReason::InvalidNearClipMode;
    return result;
  }
  bool foundTarget = false;
  for (const CrashReplayFramePtr &frame : fifoFrames) {
    if (!frame) {
      result.frames.push_back(frame);
      result.counters.retainedFrames = result.frames.size();
      result.reason = ReplayPreflightReason::NullFrame;
      return result;
    }
    result.frames.push_back(frame);
    if (exactTargetFrame != nullptr) {
      if (frame.get() == exactTargetFrame) {
        foundTarget = true;
        break;
      }
      continue;
    }
    const uint64_t frameNumber = frame->latch().frameNumber;
    if (frameNumber == request.expectedLatch.frameNumber) {
      foundTarget = true;
      break;
    }
    if (frameNumber > request.expectedLatch.frameNumber) {
      result.frames.pop_back();
      break;
    }
  }
  result.counters.retainedFrames = result.frames.size();
  if (!foundTarget) {
    result.reason = ReplayPreflightReason::TargetLatchUnavailable;
    return result;
  }

  ReplayPreflightState candidate = confirmedState;
  if (hardNativeReanchor) {
    // The exact native snapshot is a complete color/mask baseline even when
    // the visual IR prefix has a hole. Preserve known producer revision
    // labels and initialize only unknown tiles to zero so every later positive
    // NativeTileUpload revision can still advance monotonically.
    candidate.surfaceInitialized = true;
    candidate.depthInitialized = true;
    if (candidate.depthEpoch == 0)
      candidate.depthEpoch = 1;
    candidate.knownNativeTiles.set();
  }
  for (std::size_t frameIndex = 0; frameIndex < result.frames.size();
       ++frameIndex) {
    const CrashReplayFrame &frame = *result.frames[frameIndex];
    const std::span<const Operation> operations = frame.operations();
    ++result.counters.examinedFrames;
    if (operations.empty() ||
        !std::holds_alternative<FrameLatch>(operations.back())) {
      result.reason = ReplayPreflightReason::InvalidOperationLayout;
      return result;
    }
    for (std::size_t operation = 0; operation + 1 < operations.size();
         ++operation) {
      if (std::holds_alternative<FrameLatch>(operations[operation])) {
        result.reason = ReplayPreflightReason::InvalidOperationLayout;
        return result;
      }
    }

    const FrameLatch &latch = std::get<FrameLatch>(operations.back());
    if (hardNativeReanchor && latch.unrecoverableCount != 0) {
      result.reason = ReplayPreflightReason::UnsupportedMutationCount;
      return result;
    }
    if (!hardNativeReanchor && latch.unsupportedCount != 0) {
      result.reason = ReplayPreflightReason::UnsupportedMutationCount;
      return result;
    }
    if (!hardNativeReanchor && !latch.complete) {
      result.reason = ReplayPreflightReason::FrameIncomplete;
      return result;
    }
    if (!isValidAperture(latch.display.aperture)) {
      result.reason = ReplayPreflightReason::InvalidAperture;
      return result;
    }
    if (!validReplayPolicy(latch.policy)) {
      result.reason = ReplayPreflightReason::InvalidFramePolicy;
      return result;
    }
    if (latch.policy != request.expectedPolicy) {
      result.reason = ReplayPreflightReason::PolicyMismatch;
      return result;
    }
    if (latch.sequenceEnd < latch.sequenceBegin) {
      result.reason = ReplayPreflightReason::SequenceRangeMismatch;
      return result;
    }

    const bool hasTimelineAnchor = candidate.timelineInitialized;
    const uint64_t previousSubmission = candidate.completedSubmissionId;
    if (hasTimelineAnchor) {
      if (candidate.lastFrameNumber == std::numeric_limits<uint64_t>::max()) {
        result.reason = ReplayPreflightReason::FrameNumberOverflow;
        return result;
      }
      if (latch.frameNumber != candidate.lastFrameNumber + 1) {
        result.reason = ReplayPreflightReason::FrameNumberDiscontinuity;
        return result;
      }
      if (latch.contentRevision < candidate.contentRevision) {
        result.reason = ReplayPreflightReason::ContentRevisionRegression;
        return result;
      }
      if (latch.completedSubmissionId < candidate.completedSubmissionId) {
        result.reason = ReplayPreflightReason::SubmissionRegression;
        return result;
      }
      if (latch.sequenceBegin != candidate.nextPacketSequence) {
        result.reason = ReplayPreflightReason::SequenceRangeDiscontinuity;
        return result;
      }
    }

    // Draw packets and A0 headers share the producer's monotonically
    // increasing GP0 packet sequence. Data/terminal A0 phases deliberately
    // retain the header id across later frame latches and are not new packet
    // sequence events.
    bool hasPacketSequence = false;
    uint64_t lastPacketSequence = 0;
    bool hasPacketSubmission = false;
    uint64_t lastPacketSubmission = 0;
    for (std::size_t operationIndex = 0; operationIndex < operations.size();
         ++operationIndex) {
      const Operation &operation = operations[operationIndex];
      ++result.counters.operations;
      if (operationIndex + 1 < operations.size())
        ++result.counters.mutations;

      if (!requireCapability(result, request, operationCapability(operation))) {
        return result;
      }

      if (candidate.cpuUpload.active &&
          !std::holds_alternative<CpuUpload>(operation) &&
          !std::holds_alternative<FrameLatch>(operation)) {
        result.reason = ReplayPreflightReason::CpuUploadInterleavedOperation;
        return result;
      }

      if (operationNeedsInitializedSurface(operation) &&
          !candidate.surfaceInitialized) {
        result.reason = ReplayPreflightReason::SurfaceNotInitialized;
        return result;
      }

      if (const auto *upload = std::get_if<NativeTileUpload>(&operation)) {
        if (!applyNativeTileUpload(*upload, candidate, result))
          return result;
        continue;
      }
      if (const auto *seed = std::get_if<SurfaceSeed>(&operation)) {
        if (!applySurfaceSeed(*seed, candidate, result))
          return result;
        continue;
      }
      if (const auto *cpuUpload = std::get_if<CpuUpload>(&operation)) {
        if (!applyCpuUpload(*cpuUpload, latch, candidate, result))
          return result;
        if (cpuUpload->phase == CpuUploadPhase::Begin) {
          if (hasPacketSequence &&
              cpuUpload->transactionId <= lastPacketSequence) {
            result.reason = ReplayPreflightReason::PacketSequenceDiscontinuity;
            return result;
          }
          hasPacketSequence = true;
          lastPacketSequence = cpuUpload->transactionId;
        }
        continue;
      }
      if (const auto *clear = std::get_if<ClearDepth>(&operation)) {
        if (!advanceDepthEpoch(clear->nextEpoch, candidate, result))
          return result;
        continue;
      }
      if (const auto *reset = std::get_if<Reset>(&operation)) {
        if (!reset->clearDepth ||
            !advanceDepthEpoch(reset->nextDepthEpoch, candidate, result)) {
          if (!reset->clearDepth)
            result.reason = ReplayPreflightReason::DepthEpochDiscontinuity;
          return result;
        }
        continue;
      }
      if (const auto *paint = std::get_if<PaintPixels>(&operation)) {
        if (!validateDepthReference(paint->depth, candidate, result))
          return result;
        continue;
      }
      if (const auto *paint = std::get_if<PaintRect>(&operation)) {
        if (!validateDepthReference(paint->depth, candidate, result))
          return result;
        continue;
      }
      if (const auto *copy = std::get_if<CopyRect>(&operation)) {
        if (!validateNativeRead(*copy, candidate, result))
          return result;
        continue;
      }
      const auto *packet = std::get_if<DrawPacket>(&operation);
      if (packet == nullptr)
        continue;

      ++result.counters.drawPackets;
      if (!validateDepthReference(packet->state.depth, candidate, result))
        return result;
      // The latch range counts every observed GP0 command, while only polygon
      // variants carry packetSequence.  E1-E6, fill/copy/upload/paint and
      // other represented commands therefore create legitimate gaps.
      if (packet->packetSequence < latch.sequenceBegin ||
          packet->packetSequence >= latch.sequenceEnd ||
          (hasPacketSequence && packet->packetSequence <= lastPacketSequence)) {
        result.reason = ReplayPreflightReason::PacketSequenceDiscontinuity;
        return result;
      }
      hasPacketSequence = true;
      lastPacketSequence = packet->packetSequence;

      if (packet->submissionId != 0) {
        if ((hasTimelineAnchor && packet->submissionId <= previousSubmission) ||
            packet->submissionId > latch.completedSubmissionId ||
            (hasPacketSubmission &&
             packet->submissionId < lastPacketSubmission)) {
          result.reason = ReplayPreflightReason::PacketSubmissionOutOfRange;
          return result;
        }
        hasPacketSubmission = true;
        lastPacketSubmission = packet->submissionId;
      }

      if (request.requireCanonicalRawState &&
          !decodedTextureStateMatchesRaw(*packet)) {
        result.reason = ReplayPreflightReason::DecodedTextureStateMismatch;
        return result;
      }
      if (!validateDrawCapabilities(*packet, request, result))
        return result;
      if (!validateNativeRead(*packet, candidate, result))
        return result;
    }

    // While GP0(A0) is receiving data words it cannot decode another packet
    // header. Therefore an open transaction's header is necessarily the last
    // packet observed by this latch, even when that header belongs to an
    // earlier frame.
    if (candidate.cpuUpload.active &&
        (candidate.cpuUpload.transactionId == UNASSIGNED_PACKET_SEQUENCE ||
         latch.sequenceEnd != candidate.cpuUpload.transactionId + 1u)) {
      result.reason = ReplayPreflightReason::PacketSequenceDiscontinuity;
      return result;
    }

    const bool isTarget = frameIndex + 1 == result.frames.size();
    if (isTarget) {
      // A strong pointer selects the FIFO boundary, but every scalar copied
      // beside the native pixels remains part of that same atomic identity.
      // Never let a valid token mask a stale or altered frame number.
      if (latch.frameNumber != request.expectedLatch.frameNumber) {
        result.reason = ReplayPreflightReason::TargetLatchUnavailable;
        return result;
      }
      if (!hardNativeReanchor && request.expectedLatch.unsupportedCount != 0) {
        result.reason = ReplayPreflightReason::UnsupportedMutationCount;
        return result;
      }
      if (!hardNativeReanchor && !request.expectedLatch.complete) {
        result.reason = ReplayPreflightReason::FrameIncomplete;
        return result;
      }
      if (latch.contentRevision != request.expectedLatch.contentRevision) {
        result.reason = ReplayPreflightReason::ContentRevisionMismatch;
        return result;
      }
      if (latch.completedSubmissionId !=
          request.expectedLatch.completedSubmissionId) {
        result.reason = ReplayPreflightReason::SubmissionMismatch;
        return result;
      }
      if (!nativeSnapshotAnchor &&
          (latch.sequenceBegin != request.expectedLatch.sequenceBegin ||
           latch.sequenceEnd != request.expectedLatch.sequenceEnd)) {
        result.reason = ReplayPreflightReason::SequenceRangeMismatch;
        return result;
      }
      if (latch.display.aperture != request.expectedLatch.display.aperture) {
        result.reason = ReplayPreflightReason::ApertureMismatch;
        return result;
      }
      if (!displayFlagsEqual(latch.display, request.expectedLatch.display)) {
        result.reason = ReplayPreflightReason::DisplayStateMismatch;
        return result;
      }
    }

    candidate.timelineInitialized = true;
    candidate.lastFrameNumber = latch.frameNumber;
    candidate.contentRevision = latch.contentRevision;
    candidate.completedSubmissionId = latch.completedSubmissionId;
    candidate.nextPacketSequence = latch.sequenceEnd;
  }

  if (!candidate.surfaceInitialized) {
    result.reason = ReplayPreflightReason::SurfaceNotInitialized;
    return result;
  }
  if (!candidate.depthInitialized) {
    result.reason = ReplayPreflightReason::DepthNotInitialized;
    return result;
  }
  if (nativeSnapshotAnchor && !candidate.knownNativeTiles.all()) {
    result.reason = ReplayPreflightReason::NativeAnchorBaselineUnavailable;
    return result;
  }

  result.route = ReplayPreflightRoute::M1Replay;
  result.reason = ReplayPreflightReason::None;
  result.candidateState = candidate;
  return result;
}

} // namespace

ReplayPreflightResult
preflightCrashReplay(std::span<const CrashReplayFramePtr> fifoFrames,
                     const ReplayPreflightRequest &request,
                     const ReplayPreflightState &confirmedState) {
  return evaluateCrashReplay(fifoFrames, request, confirmedState,
                             ReplayEvaluationMode::StrictM1, nullptr);
}

ReplayNativeAnchorResult
deriveNativeSnapshotAnchor(std::span<const CrashReplayFramePtr> fifoFrames,
                           const ReplayNativeAnchorRequest &request,
                           const ReplayPreflightState &scheduledState) {
  ReplayPreflightRequest evaluationRequest;
  evaluationRequest.schemaVersion = request.schemaVersion;
  evaluationRequest.schemaName = request.schemaName;
  if (request.snapshot.sealedFrame)
    evaluationRequest.expectedLatch = request.snapshot.sealedFrame->latch();
  evaluationRequest.expectedLatch.frameNumber = request.snapshot.frameNumber;
  evaluationRequest.expectedLatch.contentRevision =
      request.snapshot.contentRevision;
  evaluationRequest.expectedLatch.completedSubmissionId =
      request.snapshot.completedSubmissionId;
  evaluationRequest.expectedLatch.display = request.snapshot.display;
  evaluationRequest.expectedPolicy = request.expectedPolicy;
  // M0Reanchor replaces every visual mutation with the authoritative native
  // snapshot. Giving the structural walker the complete schema capability set
  // therefore suppresses only executor capability misses; packet layout,
  // policy, native-read, A0 and depth validation remain unchanged.
  evaluationRequest.capabilities = CRASH_REPLAY_V1_ALL_CAPABILITIES;
  evaluationRequest.nearClipMode = ReplayNearClipMode::Saturate;
  evaluationRequest.requireCanonicalRawState = request.requireCanonicalRawState;

  ReplayPreflightResult evaluated =
      evaluateCrashReplay(fifoFrames, evaluationRequest, scheduledState,
                          ReplayEvaluationMode::NativeSnapshotAnchor,
                          request.snapshot.sealedFrame.get(),
                          request.allowIncompleteNativeReanchor);

  ReplayNativeAnchorResult result;
  result.reason = evaluated.reason;
  result.counters = evaluated.counters;
  result.frames = std::move(evaluated.frames);
  result.candidateState = std::move(evaluated.candidateState);
  if (evaluated.route == ReplayPreflightRoute::M1Replay) {
    result.route = ReplayNativeAnchorRoute::M0Reanchor;
  }
  return result;
}

std::string_view replayCapabilityName(ReplayCapability capability) {
  switch (capability) {
  case ReplayCapability::NativeTileUpload:
    return "native_tile_upload";
  case ReplayCapability::SurfaceSeed:
    return "surface_seed";
  case ReplayCapability::DrawPacket:
    return "draw_packet";
  case ReplayCapability::FillRect:
    return "fill_rect";
  case ReplayCapability::CopyRect:
    return "copy_rect";
  case ReplayCapability::CpuUpload:
    return "cpu_upload";
  case ReplayCapability::PaintPixels:
    return "paint_pixels";
  case ReplayCapability::PaintRect:
    return "paint_rect";
  case ReplayCapability::ClearDepth:
    return "clear_depth";
  case ReplayCapability::Reset:
    return "reset";
  case ReplayCapability::FrameLatch:
    return "frame_latch";
  case ReplayCapability::Projected3D:
    return "projected_3d";
  case ReplayCapability::Screen2D:
    return "screen_2d";
  case ReplayCapability::FallbackInvalidW:
    return "fallback_invalid_w";
  case ReplayCapability::FallbackMissingPrecision:
    return "fallback_missing_precision";
  case ReplayCapability::FallbackProjectiveContext:
    return "fallback_projective_context";
  case ReplayCapability::FallbackRasterSetup:
    return "fallback_raster_setup";
  case ReplayCapability::FallbackUnsupportedFeature:
    return "fallback_unsupported_feature";
  case ReplayCapability::NearPlaneSaturate:
    return "near_plane_saturate";
  case ReplayCapability::NearPlaneGeometricClip:
    return "near_plane_geometric_clip";
  case ReplayCapability::TextureDisable:
    return "texture_disable";
  case ReplayCapability::TextureIndexed4:
    return "texture_indexed_4";
  case ReplayCapability::TextureIndexed8:
    return "texture_indexed_8";
  case ReplayCapability::TextureDirect15:
    return "texture_direct_15";
  case ReplayCapability::TextureReserved:
    return "texture_reserved";
  case ReplayCapability::DrawOpaque:
    return "draw_opaque";
  case ReplayCapability::DrawSemiTransparent:
    return "draw_semi_transparent";
  case ReplayCapability::DrawUntextured:
    return "draw_untextured";
  case ReplayCapability::DrawTextured:
    return "draw_textured";
  case ReplayCapability::DestinationColorFeedback:
    return "destination_color_feedback";
  case ReplayCapability::DestinationMaskRead:
    return "destination_mask_read";
  case ReplayCapability::ForceMaskWrite:
    return "force_mask_write";
  case ReplayCapability::DepthTestWriteLessEqual:
    return "depth_test_write_less_equal";
  case ReplayCapability::DepthPainterOrdered:
    return "depth_painter_ordered";
  case ReplayCapability::DepthInvalidate:
    return "depth_invalidate";
  case ReplayCapability::DepthNonCanonical:
    return "depth_noncanonical";
  case ReplayCapability::DepthPainterWrite:
    return "depth_painter_write";
  }
  return "unknown";
}

std::string_view replayPreflightReasonName(ReplayPreflightReason reason) {
  switch (reason) {
  case ReplayPreflightReason::None:
    return "none";
  case ReplayPreflightReason::SchemaNameMismatch:
    return "schema_name_mismatch";
  case ReplayPreflightReason::SchemaVersionMismatch:
    return "schema_version_mismatch";
  case ReplayPreflightReason::InvalidExpectedAperture:
    return "invalid_expected_aperture";
  case ReplayPreflightReason::InvalidExpectedPolicy:
    return "invalid_expected_policy";
  case ReplayPreflightReason::InvalidFramePolicy:
    return "invalid_frame_policy";
  case ReplayPreflightReason::PolicyMismatch:
    return "policy_mismatch";
  case ReplayPreflightReason::InvalidNearClipMode:
    return "invalid_near_clip_mode";
  case ReplayPreflightReason::TargetLatchUnavailable:
    return "target_latch_unavailable";
  case ReplayPreflightReason::NullFrame:
    return "null_frame";
  case ReplayPreflightReason::InvalidOperationLayout:
    return "invalid_operation_layout";
  case ReplayPreflightReason::FrameNumberDiscontinuity:
    return "frame_number_discontinuity";
  case ReplayPreflightReason::FrameNumberOverflow:
    return "frame_number_overflow";
  case ReplayPreflightReason::ContentRevisionRegression:
    return "content_revision_regression";
  case ReplayPreflightReason::ContentRevisionMismatch:
    return "content_revision_mismatch";
  case ReplayPreflightReason::SubmissionRegression:
    return "submission_regression";
  case ReplayPreflightReason::SubmissionMismatch:
    return "submission_mismatch";
  case ReplayPreflightReason::PacketSubmissionOutOfRange:
    return "packet_submission_out_of_range";
  case ReplayPreflightReason::InvalidAperture:
    return "invalid_aperture";
  case ReplayPreflightReason::ApertureMismatch:
    return "aperture_mismatch";
  case ReplayPreflightReason::DisplayStateMismatch:
    return "display_state_mismatch";
  case ReplayPreflightReason::SequenceRangeDiscontinuity:
    return "sequence_range_discontinuity";
  case ReplayPreflightReason::SequenceRangeMismatch:
    return "sequence_range_mismatch";
  case ReplayPreflightReason::PacketSequenceDiscontinuity:
    return "packet_sequence_discontinuity";
  case ReplayPreflightReason::FrameIncomplete:
    return "frame_incomplete";
  case ReplayPreflightReason::UnsupportedMutationCount:
    return "unsupported_mutation_count";
  case ReplayPreflightReason::MissingCapability:
    return "missing_capability";
  case ReplayPreflightReason::DecodedTextureStateMismatch:
    return "decoded_texture_state_mismatch";
  case ReplayPreflightReason::NativeTileGeometryInvalid:
    return "native_tile_geometry_invalid";
  case ReplayPreflightReason::NativeRevisionNotAdvanced:
    return "native_revision_not_advanced";
  case ReplayPreflightReason::NativeReadLayoutMismatch:
    return "native_read_layout_mismatch";
  case ReplayPreflightReason::NativeReadCountMismatch:
    return "native_read_count_mismatch";
  case ReplayPreflightReason::NativeReadTileUnknown:
    return "native_read_tile_unknown";
  case ReplayPreflightReason::NativeReadHashMismatch:
    return "native_read_hash_mismatch";
  case ReplayPreflightReason::SurfaceNotInitialized:
    return "surface_not_initialized";
  case ReplayPreflightReason::NativeAnchorBaselineUnavailable:
    return "native_anchor_baseline_unavailable";
  case ReplayPreflightReason::CpuUploadDuplicateBegin:
    return "cpu_upload_duplicate_begin";
  case ReplayPreflightReason::CpuUploadMissingBegin:
    return "cpu_upload_missing_begin";
  case ReplayPreflightReason::CpuUploadTransactionMismatch:
    return "cpu_upload_transaction_mismatch";
  case ReplayPreflightReason::CpuUploadMetadataMismatch:
    return "cpu_upload_metadata_mismatch";
  case ReplayPreflightReason::CpuUploadBeginOutOfRange:
    return "cpu_upload_begin_out_of_range";
  case ReplayPreflightReason::CpuUploadOffsetDiscontinuity:
    return "cpu_upload_offset_discontinuity";
  case ReplayPreflightReason::CpuUploadInterleavedOperation:
    return "cpu_upload_interleaved_operation";
  case ReplayPreflightReason::DepthNotInitialized:
    return "depth_not_initialized";
  case ReplayPreflightReason::DepthEpochOverflow:
    return "depth_epoch_overflow";
  case ReplayPreflightReason::DepthEpochDiscontinuity:
    return "depth_epoch_discontinuity";
  case ReplayPreflightReason::DepthEpochMismatch:
    return "depth_epoch_mismatch";
  case ReplayPreflightReason::InvalidDepthCompare:
    return "invalid_depth_compare";
  case ReplayPreflightReason::PrecisionCoverageIncomplete:
    return "precision_coverage_incomplete";
  }
  return "unknown";
}

} // namespace ps1::gpu::replay
