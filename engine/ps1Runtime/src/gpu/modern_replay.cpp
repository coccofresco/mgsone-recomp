#include "runtime/gpu/modern_replay.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace ps1::gpu::replay {
namespace {

constexpr uint32_t NATIVE_WIDTH = 1024;
constexpr uint32_t NATIVE_HEIGHT = 512;

std::size_t checkedPixelCount(const NativeRect &rect,
                              std::string_view operationName) {
  if (rect.width == 0 || rect.height == 0) {
    throw std::invalid_argument(std::string(operationName) +
                                " rectangle must not be empty");
  }
  if (rect.width > std::numeric_limits<std::size_t>::max() / rect.height) {
    throw std::invalid_argument(std::string(operationName) +
                                " rectangle pixel count overflows");
  }
  return static_cast<std::size_t>(rect.width) * rect.height;
}

std::size_t checkedNativeMutationPixelCount(const NativeRect &rect,
                                            std::string_view operationName) {
  const std::size_t pixels = checkedPixelCount(rect, operationName);
  if (rect.width > NATIVE_WIDTH || rect.height > NATIVE_HEIGHT) {
    throw std::invalid_argument(std::string(operationName) +
                                " exceeds native VRAM dimensions");
  }
  return pixels;
}

uint32_t wrapCoordinate(int64_t value, uint32_t extent) {
  int64_t wrapped = value % static_cast<int64_t>(extent);
  if (wrapped < 0)
    wrapped += extent;
  return static_cast<uint32_t>(wrapped);
}

void validate(const NativeTileUpload &operation) {
  const auto expected =
      checkedPixelCount(operation.destination, "NativeTileUpload");
  if (operation.pixels.size() != expected) {
    throw std::invalid_argument(
        "NativeTileUpload payload does not match its rectangle");
  }
}

void validate(const SurfaceSeed &operation) {
  const auto expected = checkedPixelCount(operation.destination, "SurfaceSeed");
  if (operation.pixels.size() != expected) {
    throw std::invalid_argument(
        "SurfaceSeed payload does not match its rectangle");
  }
  if (operation.depthEpoch == 0) {
    throw std::invalid_argument("SurfaceSeed depth epoch zero is reserved");
  }
}

void validate(const DrawPacket &operation) {
  if (operation.opcode == 0xFF) {
    throw std::invalid_argument("DrawPacket opcode is unassigned");
  }
  if (operation.packetSequence == UNASSIGNED_PACKET_SEQUENCE) {
    throw std::invalid_argument("DrawPacket packet sequence is unassigned");
  }
  const bool screenRectangle = (operation.opcode & 0xe0u) == 0x60u &&
                               operation.route == PacketRoute::Screen2D;
  const bool projectedPoint = operation.opcode == 0x68u && !screenRectangle;
  const bool sourceBillboard = operation.opcode==0x60u &&
      operation.surfaceEffect.kind==MgsSurfaceKind::Snow &&
      operation.surfaceEffect.actor>=0x10000u;
  const bool projectedLine = (operation.opcode & 0xecu) == 0x40u;
  const bool screenLine = (operation.opcode&0xe0u)==0x40u &&
                         operation.route==PacketRoute::Screen2D;
  if ((operation.opcode & 0xE0u) != 0x20u && !projectedPoint &&
      !projectedLine && !screenRectangle && !sourceBillboard && !screenLine) {
    throw std::invalid_argument("DrawPacket opcode is not a polygon command");
  }
  // Authenticated points and fixed lines lower to camera-facing quads.
  // Retain their real command identity; intrinsic 2D stays on native paint.
  if ((projectedPoint || (projectedLine && !screenLine) || sourceBillboard) &&
      (operation.route != PacketRoute::Projected3D ||
       operation.validMask != 0x0fu ||
       operation.fallbackReason != PacketFallbackReason::None)) {
    throw std::invalid_argument(
        "Projected point/line requires complete projection");
  }
  switch (operation.route) {
  case PacketRoute::Projected3D:
  case PacketRoute::Screen2D:
    break;
  default:
    throw std::invalid_argument("DrawPacket route is invalid");
  }
  switch (operation.semanticReason) {
  case PacketSemanticReason::PolygonDefault:
  case PacketSemanticReason::Intrinsic2DFamily:
  case PacketSemanticReason::DisplayAnchoredComposite:
    break;
  default:
    throw std::invalid_argument("DrawPacket semantic reason is invalid");
  }
  switch (operation.fallbackReason) {
  case PacketFallbackReason::None:
  case PacketFallbackReason::InvalidWPacketAtomic:
  case PacketFallbackReason::MissingPrecisionPacketAtomic:
  case PacketFallbackReason::ProjectiveContextMismatch:
  case PacketFallbackReason::RasterSetupRejected:
  case PacketFallbackReason::UnsupportedFeature:
    break;
  default:
    throw std::invalid_argument("DrawPacket fallback reason is invalid");
  }
  if (operation.vertexCount < 3 ||
      operation.vertexCount > DrawPacket::MAX_VERTICES) {
    throw std::invalid_argument("DrawPacket must contain 3 or 4 vertices");
  }
  if (operation.indexCount == 0 ||
      operation.indexCount > DrawPacket::MAX_INDICES ||
      (operation.indexCount % 3) != 0) {
    throw std::invalid_argument(
        "DrawPacket indices must encode one or two triangles");
  }
  // Bit 2 is not a texture flag in the GP0 line family. PsyQ's SetLineF4
  // uses alias 0x4c for Codec portrait borders; treating it as a polygon
  // texture flag rejects otherwise valid modern UI ribbons.
  const bool opcodeTextured = (operation.opcode & 0xe0u) != 0x40u &&
                              (operation.opcode & 0x04u) != 0;
  const bool opcodeQuad =
      (operation.opcode & 0x08u) != 0 || projectedLine || screenRectangle || sourceBillboard || screenLine;
  const bool opcodeGouraud =
      (operation.opcode & 0x10u) != 0 && !screenRectangle;
  const bool opcodeBlended = (operation.opcode & 0x02u) != 0;
  const bool effectiveTextureDisable =
      opcodeTextured && operation.state.texturePage.textureDisable;
  // `opcode` retains the encoded GP0 layout. Canonical capture lowers a
  // GP1(09)-enabled texture-disabled polygon to effective untextured state;
  // continue accepting the former textured+disabled representation so older
  // in-memory producers still fail closed in preflight through its dedicated
  // capability gate.
  const bool texturedLayoutMatches =
      operation.textured == opcodeTextured ||
      (effectiveTextureDisable && !operation.textured);
  const bool effectiveRawTexture =
      operation.textured && opcodeTextured && (operation.opcode & 0x01u);
  if (!texturedLayoutMatches || operation.gouraud != opcodeGouraud ||
      operation.rawTexture != effectiveRawTexture ||
      operation.state.blend.enabled != opcodeBlended ||
      operation.vertexCount != (opcodeQuad ? 4u : 3u) ||
      operation.indexCount != (opcodeQuad ? 6u : 3u)) {
    throw std::invalid_argument(
        "DrawPacket decoded flags/topology disagree with its opcode");
  }

  const TextureWindowState &window = operation.state.textureWindow;
  if (((window.maskX | window.maskY | window.offsetX | window.offsetY) & 7u) !=
      0) {
    throw std::invalid_argument(
        "DrawPacket texture window fields are not expanded GP0(E2) values");
  }

  const TextureUvDomainState &uvDomain = operation.state.textureUvDomain;
  if (!std::isfinite(uvDomain.minU) || !std::isfinite(uvDomain.minV) ||
      !std::isfinite(uvDomain.maxU) || !std::isfinite(uvDomain.maxV) ||
      uvDomain.minU > uvDomain.maxU || uvDomain.minV > uvDomain.maxV) {
    throw std::invalid_argument("DrawPacket UV domain is not finite/ordered");
  }
  if (uvDomain.offsetU > 1 || uvDomain.offsetV > 1 ||
      uvDomain.offsetU != static_cast<uint16_t>(uvDomain.flippedU) ||
      uvDomain.offsetV != static_cast<uint16_t>(uvDomain.flippedV)) {
    throw std::invalid_argument(
        "DrawPacket UV flip and offset metadata disagree");
  }
  if (operation.textured != uvDomain.enabled) {
    throw std::invalid_argument(
        "DrawPacket textured state and packet UV domain disagree");
  }

  const NativeReadDependency &read = operation.nativeRead;
  if (read.tileWidth != CRASH_REPLAY_NATIVE_TILE_WIDTH ||
      read.tileHeight != CRASH_REPLAY_NATIVE_TILE_HEIGHT) {
    throw std::invalid_argument(
        "DrawPacket native read dependency uses an unsupported tile size");
  }
  const bool needsNativeRead =
      operation.textured && !operation.state.texturePage.textureDisable;
  if (read.required != needsNativeRead ||
      (read.required && read.tileCount == 0) ||
      (!read.required && (read.tileCount != 0 ||
                          read.versionHash != CRASH_REPLAY_READ_HASH_OFFSET))) {
    throw std::invalid_argument(
        "DrawPacket native texture/CLUT dependency is incomplete");
  }
  for (std::size_t i = 0; i < operation.indexCount; ++i) {
    if (operation.triangleIndices[i] >= operation.vertexCount) {
      throw std::invalid_argument(
          "DrawPacket triangle index references a missing vertex");
    }
  }

  const uint8_t fullMask =
      static_cast<uint8_t>((1u << operation.vertexCount) - 1u);
  if ((operation.validMask & static_cast<uint8_t>(~fullMask)) != 0) {
    throw std::invalid_argument(
        "DrawPacket valid mask references a missing vertex");
  }

  for (std::size_t i = 0; i < operation.vertexCount; ++i) {
    const ReplayVertex &vertex = operation.vertices[i];
    if (vertex.coordinateSpace !=
        ReplayVertex::CoordinateSpace::PostDrawOffsetNativePixels) {
      throw std::invalid_argument(
          "DrawPacket vertex is not in post-drawOffset space");
    }
    if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
        !std::isfinite(vertex.u) || !std::isfinite(vertex.v) ||
        !std::isfinite(vertex.w) || vertex.w <= 0.0) {
      throw std::invalid_argument(
          "DrawPacket precise attributes need finite positive effective W");
    }

    const ReplayVertexProvenance &provenance = vertex.provenance;
    if ((provenance.evidence &
         static_cast<uint8_t>(~REPLAY_VERTEX_PROVENANCE_KNOWN_EVIDENCE)) != 0) {
      throw std::invalid_argument(
          "DrawPacket vertex provenance has unknown evidence bits");
    }
    const bool projectionEvent =
        provenance.has(ReplayVertexProjectionEventExact);
    const bool transformLineage =
        provenance.has(ReplayVertexTransformLineageExact);
    const bool modelSource = provenance.has(ReplayVertexModelSourceExact);
    const bool modelValueFlow = provenance.has(ReplayVertexModelValueFlowExact);
    const bool gp0Source = provenance.has(ReplayVertexGp0WordSourceExact);
    const bool dmaTraversal = provenance.has(ReplayVertexDmaTraversalExact);
    const bool orderingTableAnchor =
        provenance.has(ReplayVertexOrderingTableAnchorExact);
    if (!projectionEvent &&
        (provenance.projectionId != 0 || provenance.projectionContext != 0 ||
         provenance.transformValueFingerprint != 0 || transformLineage ||
         modelSource || modelValueFlow)) {
      throw std::invalid_argument(
          "DrawPacket vertex lineage lacks exact projection evidence");
    }
    if (projectionEvent &&
        (provenance.projectionId == 0 || !vertex.homogeneousQ)) {
      throw std::invalid_argument(
          "DrawPacket exact projection event lacks homogeneous provenance");
    }
    if ((!transformLineage && provenance.transformLineage != 0) ||
        (!modelSource && provenance.modelVertexSource != 0) ||
        (modelSource && provenance.modelVertexSource == 0) ||
        (!modelValueFlow &&
         (provenance.modelValueFlowSourceFingerprint != 0 ||
          provenance.modelValueFlowProgramFingerprint != 0)) ||
        (!gp0Source && provenance.gp0WordSource != 0) ||
        (gp0Source && provenance.gp0WordSource == 0)) {
      throw std::invalid_argument(
          "DrawPacket vertex provenance values disagree with evidence");
    }
    if ((!dmaTraversal &&
         (provenance.dmaListSource != 0 || provenance.dmaNodeSource != 0 ||
          provenance.dmaTraversalOrdinal != 0)) ||
        (dmaTraversal && (provenance.dmaListSource == 0 ||
                          provenance.dmaNodeSource == 0 || !gp0Source))) {
      throw std::invalid_argument(
          "DrawPacket vertex DMA provenance disagrees with evidence");
    }
    if ((!orderingTableAnchor &&
         provenance.dmaOrderingTableAnchorSource != 0) ||
        (orderingTableAnchor &&
         (provenance.dmaOrderingTableAnchorSource == 0 || !dmaTraversal))) {
      throw std::invalid_argument(
          "DrawPacket vertex OT-anchor provenance disagrees with evidence");
    }
  }

  const auto hasPacketAtomicW = [&] {
    for (std::size_t i = 0; i < operation.vertexCount; ++i) {
      if (operation.vertices[i].w != CRASH_REPLAY_FALLBACK_W)
        return false;
    }
    return true;
  };

  if ((operation.state.depth.testEnabled ||
       operation.state.depth.writeEnabled ||
       operation.state.depth.invalidateOnAccepted) &&
      operation.state.depth.epoch == 0) {
    throw std::invalid_argument(
        "DrawPacket active depth state requires a non-zero epoch");
  }

  if (operation.route == PacketRoute::Screen2D) {
    if (operation.semanticReason == PacketSemanticReason::PolygonDefault) {
      throw std::invalid_argument(
          "Screen2D DrawPacket requires an explicit semantic reason");
    }
    if (operation.fallbackReason != PacketFallbackReason::None) {
      throw std::invalid_argument(
          "semantic Screen2D DrawPacket must not be marked as a fallback");
    }
    if (operation.validMask != 0 || !hasPacketAtomicW()) {
      throw std::invalid_argument(
          "Screen2D DrawPacket must use affine fallback W without XYZ mask");
    }
    if (operation.state.depth.testEnabled ||
        operation.state.depth.writeEnabled ||
        !operation.state.depth.invalidateOnAccepted) {
      throw std::invalid_argument(
          "Screen2D DrawPacket must invalidate depth on accepted writes");
    }
    for (std::size_t i = 0; i < operation.vertexCount; ++i) {
      const ReplayVertexProvenance &provenance =
          operation.vertices[i].provenance;
      if ((provenance.evidence &
           (ReplayVertexProjectionEventExact |
            ReplayVertexTransformLineageExact | ReplayVertexModelSourceExact |
            ReplayVertexModelValueFlowExact)) != 0) {
        throw std::invalid_argument(
            "Screen2D DrawPacket must not publish GTE projection provenance");
      }
    }
    return;
  }

  if (operation.semanticReason != PacketSemanticReason::PolygonDefault) {
    throw std::invalid_argument(
        "Projected3D DrawPacket has a Screen2D semantic reason");
  }
  const bool incompleteW = operation.validMask != fullMask;
  const bool wFallbackReason =
      operation.fallbackReason == PacketFallbackReason::InvalidWPacketAtomic ||
      operation.fallbackReason ==
          PacketFallbackReason::MissingPrecisionPacketAtomic;
  if (incompleteW && (!hasPacketAtomicW() || !wFallbackReason)) {
    throw std::invalid_argument(
        "incomplete Projected3D W must use packet-atomic raw 32768 fallback");
  }
  if (!incompleteW && wFallbackReason) {
    throw std::invalid_argument(
        "fully valid Projected3D packet has an invalid-W fallback reason");
  }
}

void validate(const FillRect &operation) {
  (void)checkedNativeMutationPixelCount(operation.destination, "FillRect");
}

void validate(const CopyRect &operation) {
  if (operation.width == 0 || operation.height == 0) {
    throw std::invalid_argument("CopyRect rectangle must not be empty");
  }
  if (operation.width > NATIVE_WIDTH || operation.height > NATIVE_HEIGHT) {
    throw std::invalid_argument("CopyRect exceeds native VRAM dimensions");
  }
  const NativeReadDependency &read = operation.nativeRead;
  if (read.tileWidth != CRASH_REPLAY_NATIVE_TILE_WIDTH ||
      read.tileHeight != CRASH_REPLAY_NATIVE_TILE_HEIGHT) {
    throw std::invalid_argument(
        "CopyRect native read dependency uses an unsupported tile size");
  }
  if (!read.required || read.tileCount == 0) {
    throw std::invalid_argument(
        "CopyRect native source dependency is incomplete");
  }
}

void validate(const CpuUpload &operation) {
  const auto transferPixels =
      checkedNativeMutationPixelCount(operation.transfer, "CpuUpload transfer");
  if (operation.transactionId == UNASSIGNED_PACKET_SEQUENCE) {
    throw std::invalid_argument("CpuUpload transaction id is unassigned");
  }

  switch (operation.phase) {
  case CpuUploadPhase::Begin:
    if (operation.pixelOffset != 0 || !operation.pixels.empty() ||
        !operation.acceptedBits.empty()) {
      throw std::invalid_argument(
          "CpuUpload Begin must have an empty zero-offset payload");
    }
    return;
  case CpuUploadPhase::Complete:
    if (operation.pixelOffset != transferPixels || !operation.pixels.empty() ||
        !operation.acceptedBits.empty()) {
      throw std::invalid_argument(
          "CpuUpload Complete must terminate the complete transfer");
    }
    return;
  case CpuUploadPhase::Abort:
    if (operation.pixelOffset > transferPixels || !operation.pixels.empty() ||
        !operation.acceptedBits.empty()) {
      throw std::invalid_argument(
          "CpuUpload Abort must have an empty in-range payload");
    }
    return;
  case CpuUploadPhase::Data:
    break;
  default:
    throw std::invalid_argument("CpuUpload phase is invalid");
  }

  if (operation.pixels.empty()) {
    throw std::invalid_argument("CpuUpload Data payload must not be empty");
  }
  if (operation.pixelOffset > transferPixels ||
      operation.pixels.size() > transferPixels - operation.pixelOffset) {
    throw std::invalid_argument(
        "CpuUpload flush lies outside the declared A0 transfer");
  }
  if (operation.acceptedBits.size() !=
      CpuUpload::acceptedWordCount(operation.pixels.size())) {
    throw std::invalid_argument(
        "CpuUpload accepted bitset does not match its flush payload");
  }
  const std::size_t tailBits = operation.pixels.size() % 64u;
  if (tailBits != 0) {
    const uint64_t validBits = (uint64_t{1} << tailBits) - 1u;
    if ((operation.acceptedBits.back() & ~validBits) != 0) {
      throw std::invalid_argument(
          "CpuUpload accepted bitset has bits outside its payload");
    }
  }
}

void validate(const PaintPixels &operation) {
  if (operation.pixels.empty()) {
    throw std::invalid_argument("PaintPixels payload must not be empty");
  }
  for (const PaintedPixel &pixel : operation.pixels) {
    if (pixel.destination.x < 0 || pixel.destination.y < 0 ||
        pixel.destination.x >= static_cast<int32_t>(NATIVE_WIDTH) ||
        pixel.destination.y >= static_cast<int32_t>(NATIVE_HEIGHT)) {
      throw std::invalid_argument(
          "PaintPixels accepted destination lies outside native VRAM");
    }
  }
  if (!operation.depth.invalidateOnAccepted || operation.depth.testEnabled ||
      operation.depth.writeEnabled) {
    throw std::invalid_argument(
        "PaintPixels must invalidate depth only on accepted writes");
  }
}

void validate(const PaintRect &operation) {
  const auto pixelCount =
      checkedNativeMutationPixelCount(operation.destination, "PaintRect");
  if (operation.coalescedCommandCount != pixelCount) {
    throw std::invalid_argument(
        "PaintRect command count must match its non-overlapping pixel area");
  }
  if (!operation.depth.invalidateOnAccepted || operation.depth.testEnabled ||
      operation.depth.writeEnabled) {
    throw std::invalid_argument(
        "PaintRect must invalidate depth only on accepted writes");
  }
}

void validate(const ClearDepth &operation) {
  if (operation.nextEpoch == 0) {
    throw std::invalid_argument("ClearDepth epoch zero is reserved");
  }
  if (!operation.wholeSurface) {
    throw std::invalid_argument(
        "CrashReplay schema 7 supports only whole-surface depth clears");
  }
}

void validate(const Reset &operation) {
  if (!operation.resetDrawState && !operation.clearDepth) {
    throw std::invalid_argument("Reset must reset state or depth");
  }
  if (operation.clearDepth && operation.nextDepthEpoch == 0) {
    throw std::invalid_argument("Reset depth epoch zero is reserved");
  }
  if (!operation.clearDepth) {
    throw std::invalid_argument(
        "CrashReplay schema 7 Reset must establish a new depth epoch");
  }
}

void validate(const FrameLatch &latch) {
  if (latch.policy.internalScale == 0 ||
      latch.policy.internalScale > MAX_INTERNAL_SCALE) {
    throw std::invalid_argument("FrameLatch replay scale must be 1..12");
  }
  switch (latch.policy.trueDepthMode) {
  case ReplayDepthMode::Off:
  case ReplayDepthMode::Hybrid:
  case ReplayDepthMode::Strict:
    break;
  default:
    throw std::invalid_argument("FrameLatch replay depth mode is invalid");
  }
  if (latch.sequenceEnd < latch.sequenceBegin) {
    throw std::invalid_argument("FrameLatch sequence range is reversed");
  }
  if (latch.complete && latch.unsupportedCount != 0) {
    throw std::invalid_argument(
        "complete FrameLatch cannot report unsupported mutations");
  }
  if (latch.unrecoverableCount > latch.unsupportedCount) {
    throw std::invalid_argument(
        "FrameLatch unrecoverable count exceeds unsupported mutations");
  }
}

} // namespace

uint64_t appendNativeReadDependencyHash(uint64_t hash, uint32_t tileIndex,
                                        uint64_t nativeRevision) {
  // FNV-1a over a canonical little-endian byte stream keeps the proof stable
  // across host word size and endianness without allocating a temporary key.
  const auto append = [&hash](uint64_t value, uint32_t byteCount) {
    for (uint32_t byte = 0; byte < byteCount; ++byte) {
      hash ^= static_cast<uint8_t>(value >> (byte * 8u));
      hash *= UINT64_C(1099511628211);
    }
  };
  append(tileIndex, sizeof(tileIndex));
  append(nativeRevision, sizeof(nativeRevision));
  return hash;
}

uint32_t shaderDrawFlags(const DrawPacket &packet) {
  uint32_t flags = 0;
  const auto set = [&flags](ReplayDrawFlag flag, bool enabled) {
    if (enabled)
      flags |= replayDrawFlagBit(flag);
  };
  set(ReplayDrawFlag::Textured, packet.textured);
  set(ReplayDrawFlag::RawTexture, packet.rawTexture);
  set(ReplayDrawFlag::SemiTransparent, packet.state.blend.enabled);
  set(ReplayDrawFlag::ForceMask, packet.state.mask.forceMaskBit);
  set(ReplayDrawFlag::CheckMask, packet.state.mask.checkMaskBit);
  set(ReplayDrawFlag::LimitUvDomain, packet.state.textureUvDomain.enabled);
  const uint8_t fullMask =
      packet.vertexCount >= 3 && packet.vertexCount <= DrawPacket::MAX_VERTICES
          ? static_cast<uint8_t>((1u << packet.vertexCount) - 1u)
          : 0u;
  set(ReplayDrawFlag::SurfaceDepthValid,
      packet.route == PacketRoute::Projected3D &&
          packet.fallbackReason == PacketFallbackReason::None &&
          fullMask != 0 && packet.validMask == fullMask);
  return flags;
}

ReplayTriangleProvenance
captureReplayTriangleProvenance(const DrawPacket &packet,
                                uint8_t triangleIndex) noexcept {
  ReplayTriangleProvenance result;
  const std::size_t first = static_cast<std::size_t>(triangleIndex) * 3u;
  if (first > packet.indexCount || packet.indexCount - first < 3u)
    return result;

  for (std::size_t corner = 0; corner < 3; ++corner) {
    const uint8_t vertexIndex = packet.triangleIndices[first + corner];
    if (vertexIndex >= packet.vertexCount)
      return {};
    result.vertexIndices[corner] = vertexIndex;
    result.vertices[corner] = packet.vertices[vertexIndex].provenance;
  }
  result.validTopology = true;
  return result;
}

std::size_t CpuUpload::acceptedWordCount(std::size_t pixelCount) {
  return pixelCount / 64u + (pixelCount % 64u != 0 ? 1u : 0u);
}

bool CpuUpload::acceptedAt(std::size_t chunkIndex) const {
  if (chunkIndex >= pixels.size()) {
    throw std::out_of_range("CpuUpload accepted index is outside its payload");
  }
  const uint64_t word = acceptedBits.at(chunkIndex / 64u);
  return ((word >> (chunkIndex % 64u)) & 1u) != 0;
}

NativePoint CpuUpload::destinationAt(std::size_t chunkIndex) const {
  if (chunkIndex >= pixels.size()) {
    throw std::out_of_range(
        "CpuUpload destination index is outside its payload");
  }
  if (transfer.width == 0) {
    throw std::logic_error("CpuUpload transfer width is zero");
  }
  const uint64_t linear = pixelOffset + chunkIndex;
  return {static_cast<int32_t>(
              wrapCoordinate(static_cast<int64_t>(transfer.x) +
                                 static_cast<int64_t>(linear % transfer.width),
                             NATIVE_WIDTH)),
          static_cast<int32_t>(
              wrapCoordinate(static_cast<int64_t>(transfer.y) +
                                 static_cast<int64_t>(linear / transfer.width),
                             NATIVE_HEIGHT))};
}

CrashReplayFrame::CrashReplayFrame(std::vector<Operation> operations)
    : operations_(std::move(operations)) {
  if (operations_.empty() ||
      !std::holds_alternative<FrameLatch>(operations_.back())) {
    throw std::invalid_argument("CrashReplayV1 frame must end in a FrameLatch");
  }
}

const FrameLatch &CrashReplayFrame::latch() const {
  return std::get<FrameLatch>(operations_.back());
}

void CrashReplayFrameBuilder::ensureOpen() const {
  if (sealed_) {
    throw std::logic_error("CrashReplayV1 frame is already sealed");
  }
}

void CrashReplayFrameBuilder::append(Operation operation) {
  ensureOpen();
  operations_.push_back(std::move(operation));
}

void CrashReplayFrameBuilder::record(NativeTileUpload operation) {
  ensureOpen();
  validate(operation);
  append(std::move(operation));
}

void CrashReplayFrameBuilder::record(SurfaceSeed operation) {
  ensureOpen();
  validate(operation);
  append(std::move(operation));
}

void CrashReplayFrameBuilder::record(DrawPacket operation) {
  ensureOpen();
  validate(operation);
  if (hasPacketSequence_ && operation.packetSequence <= lastPacketSequence_) {
    throw std::invalid_argument(
        "DrawPacket sequences must be strictly increasing within a frame");
  }
  const uint64_t packetSequence = operation.packetSequence;
  append(std::move(operation));
  if (!hasPacketSequence_) {
    firstPacketSequence_ = packetSequence;
    hasPacketSequence_ = true;
  }
  lastPacketSequence_ = packetSequence;
}

void CrashReplayFrameBuilder::record(FillRect operation) {
  ensureOpen();
  validate(operation);
  append(std::move(operation));
}

void CrashReplayFrameBuilder::record(CopyRect operation) {
  ensureOpen();
  validate(operation);
  append(std::move(operation));
}

void CrashReplayFrameBuilder::record(CpuUpload operation) {
  ensureOpen();
  validate(operation);
  append(std::move(operation));
}

void CrashReplayFrameBuilder::record(PaintPixels operation) {
  ensureOpen();
  validate(operation);
  append(std::move(operation));
}

void CrashReplayFrameBuilder::record(PaintRect operation) {
  ensureOpen();
  validate(operation);
  append(std::move(operation));
}

void CrashReplayFrameBuilder::record(ClearDepth operation) {
  ensureOpen();
  validate(operation);
  append(std::move(operation));
}

void CrashReplayFrameBuilder::record(Reset operation) {
  ensureOpen();
  validate(operation);
  append(std::move(operation));
}

CrashReplayFramePtr CrashReplayFrameBuilder::seal(FrameLatch latch) {
  ensureOpen();
  validate(latch);
  if (hasPacketSequence_ && (firstPacketSequence_ < latch.sequenceBegin ||
                             lastPacketSequence_ >= latch.sequenceEnd)) {
    throw std::invalid_argument(
        "FrameLatch sequence range does not contain its DrawPackets");
  }
  sealed_ = true;
  operations_.emplace_back(std::move(latch));
  return CrashReplayFramePtr(new CrashReplayFrame(std::move(operations_)));
}

void CrashReplayMailbox::publish(CrashReplayFramePtr frame) {
  if (!frame) {
    throw std::invalid_argument("cannot publish a null CrashReplayV1 frame");
  }
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      throw std::logic_error("cannot publish to a closed CrashReplay mailbox");
    }
    frames_.push_back(std::move(frame));
  }
  ready_.notify_one();
}

CrashReplayFramePtr CrashReplayMailbox::tryConsume() {
  std::lock_guard lock(mutex_);
  if (frames_.empty()) {
    return {};
  }
  auto frame = std::move(frames_.front());
  frames_.pop_front();
  return frame;
}

CrashReplayFramePtr CrashReplayMailbox::waitConsume() {
  std::unique_lock lock(mutex_);
  ready_.wait(lock, [this] { return closed_ || !frames_.empty(); });
  if (frames_.empty()) {
    return {};
  }
  auto frame = std::move(frames_.front());
  frames_.pop_front();
  return frame;
}

CrashReplayFramePtr
CrashReplayMailbox::waitConsumeFor(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  if (!ready_.wait_for(lock, timeout,
                       [this] { return closed_ || !frames_.empty(); })) {
    return {};
  }
  if (frames_.empty()) {
    return {};
  }
  auto frame = std::move(frames_.front());
  frames_.pop_front();
  return frame;
}

void CrashReplayMailbox::close() {
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
  }
  ready_.notify_all();
}

bool CrashReplayMailbox::isClosed() const {
  std::lock_guard lock(mutex_);
  return closed_;
}

std::size_t CrashReplayMailbox::pendingFrameCount() const {
  std::lock_guard lock(mutex_);
  return frames_.size();
}

} // namespace ps1::gpu::replay
