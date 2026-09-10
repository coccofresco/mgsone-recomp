#include "runtime/games/crash3_object_presenter.h"
#include "runtime/gte_color_math.h"

#include "runtime/gte.h"
#include "runtime/gte_projection_math.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>

namespace ps1::games::crash3 {
namespace {

constexpr uint32_t kRamAddressMask = Memory::RAM_SIZE - 1u;
constexpr uint32_t kMaximumCommands = 65536u;
constexpr uint32_t kMaximumDecodedVertices = 16384u;
constexpr uint32_t kMaximumFaces = 32768u;
constexpr uint32_t kMaximumColorWords = 256u;
constexpr uint32_t kTemporalVertexRenderer = UINT32_C(0x8003F460);
constexpr uint32_t kNearPlaneVertexRenderer = UINT32_C(0x8003FDB4);

class SnapshotRam {
public:
  explicit SnapshotRam(
      const std::shared_ptr<const std::vector<uint8_t>> &storage) noexcept
      : storage_(storage.get()) {}

  bool valid() const noexcept {
    return storage_ != nullptr && storage_->size() == Memory::RAM_SIZE;
  }

  bool range(uint32_t address, uint32_t byteCount,
             uint32_t alignment = 1u) const noexcept {
    if (!valid() || address == 0u || byteCount == 0u || alignment == 0u ||
        (alignment & (alignment - 1u)) != 0u ||
        (address & (alignment - 1u)) != 0u || byteCount > Memory::RAM_SIZE)
      return false;
    const bool kuseg = address < UINT32_C(0x00800000);
    const bool kseg0 =
        address >= UINT32_C(0x80000000) && address < UINT32_C(0x80800000);
    const bool kseg1 =
        address >= UINT32_C(0xA0000000) && address < UINT32_C(0xA0800000);
    if (!kuseg && !kseg0 && !kseg1)
      return false;
    const uint32_t physical = address & kRamAddressMask;
    return physical <= Memory::RAM_SIZE - byteCount;
  }

  uint8_t read8(uint32_t address) const noexcept {
    return (*storage_)[address & kRamAddressMask];
  }

  uint16_t read16(uint32_t address) const noexcept {
    const uint32_t physical = address & kRamAddressMask;
    return static_cast<uint16_t>((*storage_)[physical]) |
           static_cast<uint16_t>((*storage_)[physical + 1u] << 8u);
  }

  uint32_t read32(uint32_t address) const noexcept {
    const uint32_t physical = address & kRamAddressMask;
    return static_cast<uint32_t>((*storage_)[physical]) |
           (static_cast<uint32_t>((*storage_)[physical + 1u]) << 8u) |
           (static_cast<uint32_t>((*storage_)[physical + 2u]) << 16u) |
           (static_cast<uint32_t>((*storage_)[physical + 3u]) << 24u);
  }

private:
  const std::vector<uint8_t> *storage_ = nullptr;
};

int32_t signExtend(uint32_t value, uint32_t bits) noexcept {
  const uint32_t shift = 32u - bits;
  return static_cast<int32_t>(value << shift) >> shift;
}

class MsbBitReader {
public:
  MsbBitReader(const SnapshotRam &ram, uint32_t address) noexcept
      : ram_(ram), address_(address) {}

  std::optional<int32_t> readSigned(uint32_t bits) noexcept {
    if (bits == 0u || bits > 32u || bitIndex_ > UINT32_MAX - bits)
      return std::nullopt;
    const uint32_t wordIndex = bitIndex_ >> 5u;
    const uint32_t intra = bitIndex_ & 31u;
    const uint64_t byteOffset = static_cast<uint64_t>(wordIndex) * 4u;
    if (byteOffset > UINT32_MAX)
      return std::nullopt;
    const uint32_t firstAddress = address_ + static_cast<uint32_t>(byteOffset);
    if (!ram_.range(firstAddress, sizeof(uint32_t), 1u))
      return std::nullopt;
    const uint32_t first = ram_.read32(firstAddress);
    const int32_t shift = static_cast<int32_t>(32u - bits - intra);
    uint32_t value = 0u;
    if (shift >= 0) {
      value = first >> static_cast<uint32_t>(shift);
    } else {
      if (!ram_.range(firstAddress + sizeof(uint32_t), sizeof(uint32_t), 1u))
        return std::nullopt;
      const uint32_t second = ram_.read32(firstAddress + sizeof(uint32_t));
      value = (first << static_cast<uint32_t>(-shift)) |
              (second >> static_cast<uint32_t>(shift & 31));
    }
    bitIndex_ += bits;
    return signExtend(value, bits);
  }

private:
  const SnapshotRam &ram_;
  uint32_t address_ = 0u;
  uint32_t bitIndex_ = 0u;
};

int32_t signedByte(int32_t value) noexcept {
  return static_cast<int8_t>(static_cast<uint8_t>(value));
}

int16_t lowSigned16(uint32_t value) noexcept {
  return static_cast<int16_t>(value & 0xffffu);
}

int16_t highSigned16(uint32_t value) noexcept {
  return static_cast<int16_t>(value >> 16u);
}

std::array<double, 3>
transformModelVertex(const ObjectPresentationRendererNode &node,
                     const std::array<double, 3> &model) noexcept {
  const auto &c = node.gteControlWords;
  const std::array<std::array<double, 3>, 3> rotation{{
      {{static_cast<double>(lowSigned16(c[0])),
        static_cast<double>(highSigned16(c[0])),
        static_cast<double>(lowSigned16(c[1]))}},
      {{static_cast<double>(highSigned16(c[1])),
        static_cast<double>(lowSigned16(c[2])),
        static_cast<double>(highSigned16(c[2]))}},
      {{static_cast<double>(lowSigned16(c[3])),
        static_cast<double>(highSigned16(c[3])),
        static_cast<double>(lowSigned16(c[4]))}},
  }};
  std::array<double, 3> result{};
  for (std::size_t row = 0; row < result.size(); ++row) {
    result[row] = static_cast<double>(std::bit_cast<int32_t>(c[5u + row]));
    for (std::size_t column = 0; column < model.size(); ++column)
      result[row] += rotation[row][column] * model[column] / 4096.0;
  }
  return result;
}

bool projectVertex(const ObjectPresentationRendererNode &node,
                   ObjectPresentationVertex &vertex) noexcept {
  vertex.camera = transformModelVertex(node, vertex.model);
  const double z = vertex.camera[2];
  const double h = static_cast<double>(node.gteControlWords[26] & 0xffffu);
  if (!std::isfinite(z) || !std::isfinite(h))
    return false;
  const double normalization =
      node.scaleShift >= 0 && node.scaleShift < 31
          ? static_cast<double>(uint32_t{1} << node.scaleShift)
          : 1.0;
  vertex.projectiveW = z * normalization;
  if (!std::isfinite(vertex.projectiveW))
    return false;
  if (z == 0.0) {
    vertex.screen = {{std::numeric_limits<double>::quiet_NaN(),
                      std::numeric_limits<double>::quiet_NaN()}};
    return true;
  }
  const double ofx =
      static_cast<double>(std::bit_cast<int32_t>(node.gteControlWords[24])) /
      65536.0;
  const double ofy =
      static_cast<double>(std::bit_cast<int32_t>(node.gteControlWords[25])) /
      65536.0;
  vertex.screen[0] = ofx + h * vertex.camera[0] / z;
  vertex.screen[1] = ofy + h * vertex.camera[1] / z;
  vertex.directProjectionValid =
      std::isfinite(vertex.screen[0]) && std::isfinite(vertex.screen[1]);
  return vertex.directProjectionValid;
}

std::array<float, 3> decodeColor(uint32_t packed) noexcept {
  return {{static_cast<float>(packed & 0xffu),
           static_cast<float>((packed >> 8u) & 0xffu),
           static_cast<float>((packed >> 16u) & 0xffu)}};
}

std::array<float, 3>
decodeDistanceCuedColor(const ObjectPresentationRendererNode &node,
                        uint32_t packed) noexcept {
  const std::array<float, 3> source = decodeColor(packed);
  // The primitive path copies node+0x1c to renderer scratch+232, writes its
  // low half to IR0, and executes DPCT when it is non-zero.  The same path
  // extracts RFC/GFC/BFC from node+0x20 immediately before emitting the
  // primitive; a GTE snapshot taken when the transform was installed is too
  // early because these three global registers are deliberately reused by
  // neighbouring objects.  Reproduce both bit extraction and the GTE's sf=1
  // fixed-point arithmetic exactly.  The high node+0x1c bits remain renderer
  // flags and never participate in the color operation.
  // Prefer the tuple sampled at the command which consumes it.  The packed
  // node words remain an exact fallback for synthetic/unit snapshots and for
  // the zero-cue path where Crash branches around DPCT entirely.
  const bool exactCue = node.distanceCueCaptured &&
                        node.distanceCueConsistent &&
                        node.distanceCueProofs != 0u;
  const int32_t ir0 = exactCue ? node.distanceCueIr0
                               : static_cast<int16_t>(node.words[0x1cu / 4u]);
  if (ir0 == 0)
    return source;

  std::array<int32_t, 3> farColor = node.distanceCueFarColor;
  if (!exactCue) {
    const uint32_t packedFarColor = node.words[0x20u / 4u];
    farColor = {{
        static_cast<int32_t>((packedFarColor >> 16u) & 0x0ff0u),
        static_cast<int32_t>((packedFarColor >> 8u) & 0x0ff0u),
        static_cast<int32_t>(packedFarColor & 0x0ff0u),
    }};
  }
  return gte_math::distanceCueColor(source, farColor,
                                    static_cast<int16_t>(ir0));
}

std::array<double, 3> unpackCachedGteVertex(uint32_t packedXY,
                                            uint32_t packedZ) noexcept {
  return {{static_cast<double>(lowSigned16(packedXY)),
           static_cast<double>(highSigned16(packedXY)),
           static_cast<double>(lowSigned16(packedZ))}};
}

struct DecodedStackVertex {
  std::array<double, 3> model{};
  uint32_t color = 0u;
  uint32_t command = 0u;
  uint8_t stackIndex = 0u;
  bool valid = false;
};

struct DecodeState {
  std::array<DecodedStackVertex, 256> stack{};
  std::array<DecodedStackVertex, 4> fifo{};
  std::vector<uint32_t> scratchColors;
  uint32_t vertexIndex = 0u;
  int32_t xAccum = 0;
  int32_t yAccum = 0;
  int32_t zAccum = 0;
  int32_t nextXAccum = 0;
  int32_t nextYAccum = 0;
  int32_t nextZAccum = 0;
  uint32_t stripLength = 0u;
  uint32_t stripCommand = 0u;
};

std::optional<int32_t> readDeltaComponent(MsbBitReader &reader, uint8_t bits,
                                          int32_t temporalBase,
                                          int32_t &accumulator) noexcept {
  const auto value = reader.readSigned(static_cast<uint32_t>(bits) + 1u);
  if (!value.has_value())
    return std::nullopt;
  accumulator = bits == 7u ? signedByte(*value)
                           : signedByte(accumulator + *value + temporalBase);
  return accumulator;
}

bool readFrameOriginAndStream(const SnapshotRam &ram, uint32_t frame,
                              std::array<int16_t, 3> &origin,
                              uint32_t &packedZOrigin,
                              uint32_t &stream) noexcept {
  if (!ram.range(frame, 0x1cu, 4u))
    return false;
  origin = {{static_cast<int16_t>(ram.read16(frame)),
             static_cast<int16_t>(ram.read16(frame + 2u)),
             static_cast<int16_t>(ram.read16(frame + 4u))}};
  packedZOrigin = ram.read32(frame + 4u);
  const uint32_t offset = ram.read32(frame + 0x18u);
  if (offset < 0x1cu || offset > Memory::RAM_SIZE)
    return false;
  stream = frame + offset;
  return ram.range(stream, sizeof(uint32_t), 1u);
}

int32_t averageSigned32(uint32_t left, uint32_t right) noexcept {
  return std::bit_cast<int32_t>(left + right) >> 1;
}

std::array<double, 3>
interpolatePackedGteVertex(const std::array<int16_t, 3> &origin,
                           const std::array<int16_t, 3> &nextOrigin,
                           uint32_t packedZOrigin, uint32_t nextPackedZOrigin,
                           uint32_t current, uint32_t next) noexcept {
  const int32_t originX = averageSigned32(
      static_cast<uint32_t>(static_cast<int32_t>(origin[0])),
      static_cast<uint32_t>(static_cast<int32_t>(nextOrigin[0])));
  const int32_t originY = averageSigned32(
      static_cast<uint32_t>(static_cast<int32_t>(origin[1])),
      static_cast<uint32_t>(static_cast<int32_t>(nextOrigin[1])));
  const uint32_t packedOriginXY =
      static_cast<uint16_t>(originX) |
      (static_cast<uint32_t>(static_cast<uint16_t>(originY)) << 16u);
  const uint32_t packedOriginZ =
      static_cast<uint32_t>(averageSigned32(packedZOrigin, nextPackedZOrigin));

  // Exact dataflow in func_8003E260: the two three-byte frame samples are
  // summed in their packed lanes, scaled by 16, then combined with the
  // already averaged frame origins. The 0xfffeffff mask removes the carry
  // between packed X/Y lanes. This is not equivalent to averaging abstract
  // model coordinates and multiplying them by four.
  constexpr uint32_t kPackedXYLanes = UINT32_C(0x00ff00ff);
  const uint32_t packedXY =
      ((((current & kPackedXYLanes) + (next & kPackedXYLanes)) << 4u) +
       packedOriginXY) &
      UINT32_C(0xfffeffff);
  const uint32_t packedZ =
      ((((current & UINT32_C(0x0000ff00)) + (next & UINT32_C(0x0000ff00))) >>
        4u) +
       packedOriginZ);
  return {{static_cast<double>(lowSigned16(packedXY)),
           static_cast<double>(highSigned16(packedXY)),
           static_cast<double>(lowSigned16(packedZ))}};
}

std::array<double, 3>
expandPackedGteVertex(const std::array<int16_t, 3> &origin,
                      uint32_t packedZOrigin, uint32_t current) noexcept {
  const uint32_t packedOriginXY =
      static_cast<uint16_t>(origin[0]) |
      (static_cast<uint32_t>(static_cast<uint16_t>(origin[1])) << 16u);

  // Exact dataflow in func_8003D260. The single three-byte sample is arranged
  // X/Y/Z in little-endian byte lanes, scaled by 32, then added to the frame
  // origins. The masks are the ones installed by func_8003F810 and prevent a
  // carry from crossing the signed packed X/Y lane boundary.
  const uint32_t scaled = current << 5u;
  const uint32_t packedXY =
      (((scaled & UINT32_C(0x1fe01fe0)) + packedOriginXY) &
       UINT32_C(0xfffeffff));
  const uint32_t packedZ =
      ((scaled >> 8u) & UINT32_C(0x00001fe0)) + packedZOrigin;
  return {{static_cast<double>(lowSigned16(packedXY)),
           static_cast<double>(highSigned16(packedXY)),
           static_cast<double>(lowSigned16(packedZ))}};
}

std::optional<uint32_t> indexedColor(const std::vector<uint32_t> &source,
                                     const std::vector<uint32_t> &scratch,
                                     uint32_t byteOffset,
                                     bool useScratch) noexcept {
  if ((byteOffset & 3u) != 0u)
    return std::nullopt;
  const uint32_t index = byteOffset / sizeof(uint32_t);
  if (useScratch) {
    if (index >= scratch.size())
      return std::nullopt;
    return scratch[index];
  }
  if (index >= source.size())
    return std::nullopt;
  return source[index];
}

std::optional<uint32_t> commandColor(const std::vector<uint32_t> &source,
                                     const std::vector<uint32_t> &scratch,
                                     uint32_t command) noexcept {
  return indexedColor(source, scratch, (command >> 7u) & 0x1fcu,
                      (command & UINT32_C(0x08000000)) != 0u);
}

bool applyColorCommand(const std::vector<uint32_t> &source, DecodeState &state,
                       uint32_t command) noexcept {
  const auto a = indexedColor(source, state.scratchColors,
                              (command >> 7u) & 0x1fcu, (command & 1u) != 0u);
  const auto b = indexedColor(source, state.scratchColors, command & 0x1fcu,
                              (command & 2u) != 0u);
  if (!a.has_value() || !b.has_value())
    return false;
  state.fifo[1].color = *a;
  state.fifo[2].color = *a;
  state.fifo[3].color = *b;
  return true;
}

uint32_t requiredSourceColorWordCount(const std::vector<uint32_t> &commands,
                                      uint32_t scratchWordCount) noexcept {
  uint32_t required = scratchWordCount;
  const auto include = [&](uint32_t byteOffset) {
    required = std::max(required, byteOffset / uint32_t{4} + uint32_t{1});
  };
  for (const uint32_t command : commands) {
    if (command == UINT32_MAX)
      break;
    if ((command >> 16u) == 0u) {
      if ((command & 1u) == 0u)
        include((command >> 7u) & 0x1fcu);
      if ((command & 2u) == 0u)
        include(command & 0x1fcu);
    } else if ((command & UINT32_C(0x08000000)) == 0u) {
      include((command >> 7u) & 0x1fcu);
    }
  }
  return required;
}

bool decodeVertex(const SnapshotRam &ram,
                  const ObjectPresentationRendererNode &node, uint32_t command,
                  const std::array<int16_t, 3> &origin,
                  const std::array<int16_t, 3> &nextOrigin,
                  uint32_t packedZOrigin, uint32_t nextPackedZOrigin,
                  uint32_t streamAddress, uint32_t nextStreamAddress,
                  MsbBitReader &stream, MsbBitReader *nextStream,
                  DecodeState &state, DecodedStackVertex &decoded) noexcept {
  const uint8_t stackIndex = static_cast<uint8_t>(command >> 16u);
  const uint8_t flags = static_cast<uint8_t>(command >> 24u);
  if ((flags & 4u) != 0u) {
    if (!state.stack[stackIndex].valid)
      return false;
    decoded = state.stack[stackIndex];
    return true;
  }
  if (state.vertexIndex >= kMaximumDecodedVertices)
    return false;

  uint32_t current = 0u;
  uint32_t next = 0u;
  const uint32_t deltaArray = node.words[0x60u / sizeof(uint32_t)];
  if (deltaArray != 0u) {
    const uint64_t deltaOffset =
        static_cast<uint64_t>(state.vertexIndex) * sizeof(uint32_t);
    if (deltaOffset > UINT32_MAX ||
        !ram.range(deltaArray + static_cast<uint32_t>(deltaOffset),
                   sizeof(uint32_t), 1u)) {
      return false;
    }
    const uint32_t temporal =
        ram.read32(deltaArray + static_cast<uint32_t>(deltaOffset));
    const uint8_t xBits = static_cast<uint8_t>((temporal >> 6u) & 7u);
    const uint8_t zBits = static_cast<uint8_t>((temporal >> 3u) & 7u);
    const uint8_t yBits = static_cast<uint8_t>(temporal & 7u);
    const int32_t baseX = signExtend(temporal >> 25u, 7u) << 1u;
    const int32_t baseZ = signExtend(temporal >> 17u, 8u);
    const int32_t baseY = signExtend(temporal >> 9u, 8u);
    const auto x = readDeltaComponent(stream, xBits, baseX, state.xAccum);
    const auto z = readDeltaComponent(stream, zBits, baseZ, state.yAccum);
    const auto y = readDeltaComponent(stream, yBits, baseY, state.zAccum);
    if (!x.has_value() || !z.has_value() || !y.has_value())
      return false;
    current = static_cast<uint8_t>(*x) |
              (static_cast<uint32_t>(static_cast<uint8_t>(*y)) << 8u) |
              (static_cast<uint32_t>(static_cast<uint8_t>(*z)) << 16u);

    if (nextStream != nullptr) {
      const auto nextX =
          readDeltaComponent(*nextStream, xBits, baseX, state.nextXAccum);
      const auto nextZ =
          readDeltaComponent(*nextStream, zBits, baseZ, state.nextYAccum);
      const auto nextY =
          readDeltaComponent(*nextStream, yBits, baseY, state.nextZAccum);
      if (!nextX.has_value() || !nextZ.has_value() || !nextY.has_value())
        return false;
      next = static_cast<uint8_t>(*nextX) |
             (static_cast<uint32_t>(static_cast<uint8_t>(*nextY)) << 8u) |
             (static_cast<uint32_t>(static_cast<uint8_t>(*nextZ)) << 16u);
    }
  } else {
    const uint64_t byteOffset = static_cast<uint64_t>(state.vertexIndex) * 3u;
    if (byteOffset > UINT32_MAX ||
        !ram.range(streamAddress + static_cast<uint32_t>(byteOffset), 3u, 1u)) {
      return false;
    }
    const uint32_t address = streamAddress + static_cast<uint32_t>(byteOffset);
    current = static_cast<uint32_t>(ram.read8(address)) |
              (static_cast<uint32_t>(ram.read8(address + 1u)) << 8u) |
              (static_cast<uint32_t>(ram.read8(address + 2u)) << 16u);
    if (nextStream != nullptr) {
      if (!ram.range(nextStreamAddress + static_cast<uint32_t>(byteOffset), 3u,
                     1u)) {
        return false;
      }
      const uint32_t nextAddress =
          nextStreamAddress + static_cast<uint32_t>(byteOffset);
      next = static_cast<uint32_t>(ram.read8(nextAddress)) |
             (static_cast<uint32_t>(ram.read8(nextAddress + 1u)) << 8u) |
             (static_cast<uint32_t>(ram.read8(nextAddress + 2u)) << 16u);
    }
  }

  decoded.model =
      nextStream != nullptr
          ? interpolatePackedGteVertex(origin, nextOrigin, packedZOrigin,
                                       nextPackedZOrigin, current, next)
          : expandPackedGteVertex(origin, packedZOrigin, current);
  decoded.stackIndex = stackIndex;
  decoded.valid = true;
  // func_8003F73C's scratch+320 area is genuinely aliased: the initial colour
  // words and the cached two-word VXY/Z slots are not parallel stores.  Each
  // newly decoded vertex writes its GTE input back through the latter view,
  // and a later colour command whose source bit selects scratch observes that
  // overwritten word.  Keeping only `stack` current made those commands read
  // the stale initial colour-table copy, which could poison certification for
  // an otherwise completely decoded mesh.
  const std::size_t scratchWord = static_cast<std::size_t>(stackIndex) * 2u;
  if (scratchWord + 1u < state.scratchColors.size()) {
    const int16_t modelX = static_cast<int16_t>(decoded.model[0]);
    const int16_t modelY = static_cast<int16_t>(decoded.model[1]);
    const int16_t modelZ = static_cast<int16_t>(decoded.model[2]);
    state.scratchColors[scratchWord] =
        static_cast<uint16_t>(modelX) |
        (static_cast<uint32_t>(static_cast<uint16_t>(modelY)) << 16u);
    state.scratchColors[scratchWord + 1u] =
        static_cast<uint32_t>(static_cast<int32_t>(modelZ));
  }
  state.stack[stackIndex] = decoded;
  ++state.vertexIndex;
  return true;
}

ObjectPresentationMaterial
decodeMaterial(const SnapshotRam &ram,
               const ObjectPresentationRendererNode &node,
               uint32_t command) noexcept {
  ObjectPresentationMaterial material;
  material.sourceToken = static_cast<uint16_t>(command & 0x1ffu);
  material.resolvedToken = material.sourceToken;
  if (material.sourceToken == 0u) {
    material.decoded = true;
    return material;
  }
  if ((material.sourceToken & 0x100u) != 0u) {
    material.dynamicTexture = true;
    // Exact dataflow at func_8003CC88: bit 8 selects the node-local animated
    // remap program, while the low byte is its first descriptor index. Each
    // descriptor derives a new 11-bit index from the captured animation
    // state at node+0x18; bit 11 chains to another descriptor. Bound malformed
    // programs even though the original renderer assumes authored data.
    const uint32_t remapTable = node.words[0x44u / sizeof(uint32_t)];
    const uint32_t animationState = node.words[0x18u / sizeof(uint32_t)];
    uint32_t index = material.sourceToken & 0xffu;
    constexpr uint32_t kMaximumRemapDescriptors = 32u;
    bool terminated = false;
    for (uint32_t step = 0u; step < kMaximumRemapDescriptors; ++step) {
      const uint64_t descriptorOffset =
          static_cast<uint64_t>(index) * sizeof(uint32_t);
      if (remapTable == 0u || descriptorOffset > UINT32_MAX ||
          descriptorOffset > UINT32_MAX - remapTable ||
          !ram.range(remapTable + static_cast<uint32_t>(descriptorOffset),
                     sizeof(uint32_t), 1u)) {
        return material;
      }
      const uint32_t descriptor =
          ram.read32(remapTable + static_cast<uint32_t>(descriptorOffset));
      const uint32_t shift = (descriptor >> 26u) & 0x1fu;
      const uint32_t phase = (animationState >> shift) + (descriptor >> 19u);
      const uint32_t selector = phase & ((descriptor >> 12u) & 0x7fu);
      index = (descriptor + selector) & 0x7ffu;
      if ((descriptor & 0x800u) == 0u) {
        terminated = true;
        break;
      }
    }
    if (!terminated)
      return material;
    material.resolvedToken = static_cast<uint16_t>(index);
    // A remap result of zero rejoins the renderer's untextured sentinel path.
    if (material.resolvedToken == 0u) {
      material.decoded = true;
      return material;
    }
  }
  const uint32_t table = node.words[0x40u / sizeof(uint32_t)];
  // Zero selects Crash's untextured packet path. Positive tokens are
  // one-based indices into the packed three-word texture table; treating the
  // token as a zero-based index silently borrowed the following face's UVs.
  const uint64_t offset =
      static_cast<uint64_t>(material.resolvedToken - 1u) * 12u;
  if (table == 0u || offset > UINT32_MAX ||
      !ram.range(table + static_cast<uint32_t>(offset), 12u, 1u)) {
    return material;
  }
  const uint32_t word0 = ram.read32(table + static_cast<uint32_t>(offset));
  const uint32_t word1 = ram.read32(table + static_cast<uint32_t>(offset) + 4u);
  const uint32_t word2 = ram.read32(table + static_cast<uint32_t>(offset) + 8u);
  const auto uv = [](uint16_t packed) {
    return std::array<float, 2>{
        {static_cast<float>(packed & 0xffu), static_cast<float>(packed >> 8u)}};
  };
  material.uv = {{uv(static_cast<uint16_t>(word0)),
                  uv(static_cast<uint16_t>(word1)),
                  uv(static_cast<uint16_t>(word2))}};
  float minU = material.uv[0][0];
  float minV = material.uv[0][1];
  float maxU = minU;
  float maxV = minV;
  for (std::size_t corner = 1u; corner < material.uv.size(); ++corner) {
    minU = std::min(minU, material.uv[corner][0]);
    minV = std::min(minV, material.uv[corner][1]);
    maxU = std::max(maxU, material.uv[corner][0]);
    maxV = std::max(maxV, material.uv[corner][1]);
  }
  if (minU != maxU)
    --maxU;
  if (minV != maxV)
    --maxV;
  material.decoded = true;
  material.textured = true;
  material.semiTransparent =
      (word1 & UINT32_C(0x00600000)) != UINT32_C(0x00600000);
  material.clut = static_cast<uint16_t>(word0 >> 16u);
  material.texturePage = static_cast<uint16_t>((word1 >> 16u) & 0x1ffu);
  material.uvDomain = {{minU, minV, maxU, maxV}};
  return material;
}

ObjectPresentationMesh
decodeType1Mesh(const SnapshotRam &ram, const ObjectPresentationObject &object,
                const ObjectPresentationRendererNode &node) {
  ObjectPresentationMesh mesh;
  mesh.objectAddress = object.address;
  mesh.rendererNodeAddress = node.address;
  mesh.logicalModelResource = object.modelResource;
  mesh.rendererModelResource = node.words[0x14u / sizeof(uint32_t)];
  mesh.animationSequence = object.animationSequence;
  mesh.animationFrame = object.animationFrame;
  mesh.renderSelector = object.renderSelector;
  mesh.vertexRenderer = node.words[0x0cu / sizeof(uint32_t)];
  mesh.primitiveConstructor = node.words[0x2cu / sizeof(uint32_t)];
  static const bool legacyConstructionControl = [] {
    const char *mode = std::getenv("PS1_CRASH3_OBJECT_CONSTRUCTION");
    return mode != nullptr && std::strcmp(mode, "legacy") == 0;
  }();
  if (legacyConstructionControl)
    mesh.primitiveConstructor = 0u;
  mesh.sourceConstructionCaptured =
      mesh.primitiveConstructor != 0u && hasIntrinsicObjectConstruction(node);
  mesh.scaleShift = node.scaleShift;
  std::copy_n(node.gteControlWords.begin(),
              mesh.projectionTransformWords.size(),
              mesh.projectionTransformWords.begin());
  mesh.projectionPlane =
      static_cast<uint16_t>(node.gteControlWords[26] & 0xffffu);
  mesh.projectionOriginFixed = {
      {std::bit_cast<int32_t>(node.gteControlWords[24]),
       std::bit_cast<int32_t>(node.gteControlWords[25])}};
  mesh.orderingTableBasesCaptured = node.orderingTableBasesCaptured;
  mesh.orderingTableBasePositive = node.orderingTableBasePositive;
  mesh.orderingTableBaseNonPositive = node.orderingTableBaseNonPositive;
  mesh.orderingTableZsf3 =
      static_cast<int16_t>(node.gteControlWords[ps1::GTE_ZSF3]);
  mesh.orderingTableZsf4 =
      static_cast<int16_t>(node.gteControlWords[ps1::GTE_ZSF4]);
  mesh.materialTableAddress = node.words[0x40u / sizeof(uint32_t)];
  if ((!node.listDiscovered && !node.directBindingDiscovered) ||
      object.modelType != 1u) {
    mesh.status = ObjectMeshDecodeStatus::UnsupportedModelType;
    return mesh;
  }
  // Type 1 selects one of two materially different vertex paths through the
  // node's final renderer callback. func_8003F460 feeds func_8003E260 with
  // both temporal frame streams. func_8003FDB4 instead feeds the near-plane
  // aware func_8003D260 path and node+0x34 is not necessarily a frame pointer.
  // Never let plausible executable bytes masquerade as a completed mesh.
  const uint32_t vertexRenderer = mesh.vertexRenderer;
  const bool temporalVertexPath = vertexRenderer == kTemporalVertexRenderer;
  const bool nearPlaneVertexPath = vertexRenderer == kNearPlaneVertexRenderer;
  if (!temporalVertexPath && !nearPlaneVertexPath) {
    mesh.status = ObjectMeshDecodeStatus::UnsupportedVertexDecoder;
    return mesh;
  }
  if (!node.transformInstalled || node.scaleShift < 0) {
    mesh.status = ObjectMeshDecodeStatus::MissingTransform;
    return mesh;
  }

  const uint32_t commandTable = node.words[0x38u / sizeof(uint32_t)];
  const uint32_t colorTable = node.words[0x3cu / sizeof(uint32_t)];
  const uint32_t currentFrame = node.words[0x30u / sizeof(uint32_t)];
  const uint32_t nextFrame = node.words[0x34u / sizeof(uint32_t)];
  mesh.currentFrameResource = currentFrame;
  mesh.nextFrameResource = nextFrame;
  mesh.commandTableResource = commandTable;
  mesh.packetBindings = node.packetBindings;
  const bool capturedStreams =
      node.type1StreamsCaptured && !node.type1CommandWords.empty() &&
      node.type1CommandWords.back() == UINT32_MAX &&
      node.type1ColorWordCount <= node.type1ColorWords.size() &&
      node.type1ScratchColorWords.size() == node.type1ColorWordCount;
  if (node.type1StreamsCaptured && !capturedStreams) {
    mesh.status = ObjectMeshDecodeStatus::InvalidSourceManifest;
    return mesh;
  }
  if ((!capturedStreams && (!ram.range(commandTable, sizeof(uint32_t), 4u) ||
                            !ram.range(colorTable, sizeof(uint32_t), 4u))) ||
      currentFrame == 0u || (temporalVertexPath && nextFrame == 0u)) {
    mesh.status = ObjectMeshDecodeStatus::InvalidSourceManifest;
    return mesh;
  }

  std::vector<uint32_t> snapshotCommands;
  const std::vector<uint32_t> *sourceCommands = &node.type1CommandWords;
  if (!capturedStreams) {
    snapshotCommands.reserve(1024u);
    bool terminated = false;
    for (uint32_t index = 0u; index < kMaximumCommands; ++index) {
      const uint64_t byteOffset =
          (static_cast<uint64_t>(index) + 1u) * sizeof(uint32_t);
      if (byteOffset > UINT32_MAX || byteOffset > UINT32_MAX - commandTable ||
          !ram.range(commandTable + static_cast<uint32_t>(byteOffset),
                     sizeof(uint32_t), 1u)) {
        break;
      }
      const uint32_t command =
          ram.read32(commandTable + static_cast<uint32_t>(byteOffset));
      snapshotCommands.push_back(command);
      if (command == UINT32_MAX) {
        terminated = true;
        break;
      }
    }
    if (!terminated) {
      mesh.status = ObjectMeshDecodeStatus::InvalidCommandStream;
      return mesh;
    }
    sourceCommands = &snapshotCommands;
  }

  const uint32_t colorCount =
      capturedStreams ? node.type1ColorWordCount : ram.read32(commandTable);
  const uint32_t sourceColorCount =
      requiredSourceColorWordCount(*sourceCommands, colorCount);
  if (colorCount > kMaximumColorWords ||
      sourceColorCount > kMaximumColorWords ||
      (capturedStreams && sourceColorCount > node.type1ColorWords.size()) ||
      (!capturedStreams &&
       !ram.range(colorTable, std::max(sourceColorCount, 1u) * sizeof(uint32_t),
                  1u))) {
    mesh.status = ObjectMeshDecodeStatus::InvalidColorTable;
    return mesh;
  }
  std::vector<uint32_t> snapshotColors;
  const std::vector<uint32_t> *sourceColors = &node.type1ColorWords;
  if (!capturedStreams) {
    snapshotColors.reserve(sourceColorCount);
    for (uint32_t index = 0u; index < sourceColorCount; ++index) {
      snapshotColors.push_back(
          colorTable == 0u ? 0u : ram.read32(colorTable + index * 4u));
    }
    sourceColors = &snapshotColors;
  }
  DecodeState state;
  if (capturedStreams) {
    state.scratchColors = node.type1ScratchColorWords;
  } else {
    state.scratchColors.assign(sourceColors->begin(),
                               sourceColors->begin() + colorCount);
  }
  // func_8003F73C copies these words verbatim to scratch+320 before parsing
  // commands. A cached-vertex command indexes the same storage as 8-byte
  // VXY/Z slots, while color commands index it as individual words. Seed only
  // complete copied pairs; subsequently decoded vertices overwrite their
  // indexed slot exactly as in the guest.
  for (uint32_t stackIndex = 0u;
       stackIndex * 2u + 1u < state.scratchColors.size(); ++stackIndex) {
    DecodedStackVertex &cached = state.stack[stackIndex];
    cached.model =
        unpackCachedGteVertex(state.scratchColors[stackIndex * 2u],
                              state.scratchColors[stackIndex * 2u + 1u]);
    cached.stackIndex = static_cast<uint8_t>(stackIndex);
    cached.valid = true;
  }

  std::array<int16_t, 3> origin{};
  std::array<int16_t, 3> nextOrigin{};
  uint32_t packedZOrigin = 0u;
  uint32_t nextPackedZOrigin = 0u;
  uint32_t vertexStream = 0u;
  uint32_t nextVertexStream = 0u;
  if (!readFrameOriginAndStream(ram, currentFrame, origin, packedZOrigin,
                                vertexStream) ||
      (temporalVertexPath &&
       !readFrameOriginAndStream(ram, nextFrame, nextOrigin, nextPackedZOrigin,
                                 nextVertexStream))) {
    mesh.status = ObjectMeshDecodeStatus::InvalidVertexStream;
    return mesh;
  }
  MsbBitReader bitStream(ram, vertexStream);
  MsbBitReader nextBitStream(ram, nextVertexStream);
  MsbBitReader *nextBits = temporalVertexPath ? &nextBitStream : nullptr;

  bool terminated = false;
  mesh.faces.reserve(1024u);
  std::array<uint8_t, 128> normalHighlights{};
  if (mesh.primitiveConstructor == UINT32_C(0x8003DC00)) {
    constexpr uint32_t highlightAddress = UINT32_C(0x8005D3CC);
    if (!ram.range(highlightAddress, normalHighlights.size(), 1u)) {
      mesh.status = ObjectMeshDecodeStatus::InvalidSourceManifest;
      return mesh;
    }
    for (uint32_t index = 0u; index < normalHighlights.size(); ++index)
      normalHighlights[index] = ram.read8(highlightAddress + index);
  }
  for (uint32_t commandIndex = 0u; commandIndex < kMaximumCommands;
       ++commandIndex) {
    if (commandIndex >= sourceCommands->size()) {
      mesh.status = ObjectMeshDecodeStatus::InvalidCommandStream;
      return mesh;
    }
    const uint32_t command = (*sourceCommands)[commandIndex];
    ++mesh.commandCount;
    if (command == UINT32_MAX) {
      terminated = true;
      break;
    }
    if ((command >> 16u) == 0u) {
      if (!applyColorCommand(*sourceColors, state, command)) {
        mesh.status = ObjectMeshDecodeStatus::InvalidColorTable;
        return mesh;
      }
      ++mesh.colorCommandCount;
      continue;
    }

    const uint8_t flags = static_cast<uint8_t>(command >> 24u);
    const uint8_t stackIndex = static_cast<uint8_t>(command >> 16u);
    DecodedStackVertex decoded;
    if (!decodeVertex(ram, node, command, origin, nextOrigin, packedZOrigin,
                      nextPackedZOrigin, vertexStream, nextVertexStream,
                      bitStream, nextBits, state, decoded)) {
      mesh.failureCommand = command;
      mesh.failureVertexIndex = state.vertexIndex;
      mesh.status = ObjectMeshDecodeStatus::InvalidVertexStream;
      return mesh;
    }
    const auto color =
        commandColor(*sourceColors, state.scratchColors, command);
    if (!color.has_value()) {
      mesh.status = ObjectMeshDecodeStatus::InvalidColorTable;
      return mesh;
    }
    decoded.color = *color;
    decoded.command = command;
    state.stack[stackIndex].color = *color;
    state.stack[stackIndex].command = command;
    if ((flags & 4u) != 0u)
      ++mesh.cachedVertexCount;
    else
      ++mesh.decodedVertexCount;

    state.fifo[0] = state.fifo[1];
    state.fifo[1] = state.fifo[2];
    state.fifo[2] = state.fifo[3];
    state.fifo[3] = decoded;
    // The native strip entry (8003CDEC / 8003F460) consumes three vertices
    // unconditionally. Bit 31 is a restart only in the continuation loop,
    // after the first triangle exists. Authored streams may set it on all
    // three seed commands; resetting on the second/third discarded faces.
    const bool startsNewStrip =
        state.stripLength >= 3u && (flags & 0x80u) != 0u;
    if (startsNewStrip || state.stripLength == 0u)
      state.stripCommand = command;
    if (startsNewStrip)
      state.stripLength = 0u;
    const bool reuseFirst = state.stripLength > 2u && (flags & 0x40u) != 0u;
    if (reuseFirst)
      state.fifo[1] = state.fifo[0];
    if (state.stripLength < 2u) {
      ++state.stripLength;
      continue;
    }
    if (mesh.faces.size() >= kMaximumFaces) {
      mesh.status = ObjectMeshDecodeStatus::ResourceLimit;
      return mesh;
    }

    // 8003CE2C restores scratch+268 after filling the initial GTE FIFO:
    // the first triangle takes its material/winding/OT class from the strip
    // head. Continuations use the completing command. commandIndex still
    // names the consumed stream cursor (third seed for the first triangle),
    // exactly as the packet-construction binding records it.
    const uint32_t drawCommand =
        state.stripLength == 2u ? state.stripCommand : command;
    ObjectPresentationFace face;
    face.objectAddress = object.address;
    face.rendererNodeAddress = node.address;
    face.ordinal = static_cast<uint32_t>(mesh.faces.size());
    face.commandIndex = commandIndex;
    face.command = drawCommand;
    face.stripHeadCommand = state.stripCommand;
    face.originalBackfaceTest = (drawCommand & UINT32_C(0x10000000)) != 0u;
    const bool initialNegative =
        std::bit_cast<int32_t>(node.words[0x1cu / sizeof(uint32_t)]) < 0;
    const bool commandReverses = (drawCommand & UINT32_C(0x20000000)) != 0u;
    face.acceptsNegativeWinding = initialNegative ^ commandReverses;
    face.material = decodeMaterial(ram, node, drawCommand);
    if (!face.material.decoded)
      ++mesh.unresolvedMaterialFaces;
    for (std::size_t corner = 0u; corner < face.vertices.size(); ++corner) {
      const DecodedStackVertex &source = state.fifo[corner + 1u];
      if (!source.valid) {
        mesh.status = ObjectMeshDecodeStatus::InvalidCommandStream;
        return mesh;
      }
      auto &destination = face.vertices[corner];
      destination.model = source.model;
      destination.packedSourceColor = source.color;
      destination.stackIndex = source.stackIndex;
      face.vertexCommands[corner] = source.command;
      if (!projectVertex(node, destination)) {
        mesh.status = ObjectMeshDecodeStatus::InvalidVertexStream;
        return mesh;
      }
      const bool sourceFadeConstructor =
          mesh.primitiveConstructor == UINT32_C(0x8003FC00) &&
          node.words[0x28u / 4u] == UINT32_C(0x8003D248);
      if (node.perVertexFadeCaptured || sourceFadeConstructor) {
        if (!node.perVertexFadeConsistent) {
          mesh.status = ObjectMeshDecodeStatus::InvalidColorTable;
          return mesh;
        }
        // Exact SZ1/2/3 semantics used by func_8003FC1C. Preserve high
        // precision geometry; only this guest colour calculation uses SZ's
        // integer floor/clamp. The fade is not a culling or visibility gate.
        const int32_t sz = static_cast<int32_t>(
            std::clamp(std::floor(destination.camera[2]), 0.0, 65535.0));
        if (sz <= 3072) {
          destination.color255 = {};
        } else if (sz > 5120) {
          destination.color255 = decodeColor(source.color);
        } else {
          const int32_t ir0 = (5120 - sz) * 2;
          const auto color = decodeColor(source.color);
          std::array<int32_t, 3> farColor{};
          for (std::size_t component = 0u; component < 3u; ++component)
            farColor[component] =
                node.perVertexFadeCaptured
                    ? node.perVertexFadeFarColor[component]
                    : std::bit_cast<int32_t>(
                          node.gteControlWords[GTE_RFC + component]);
          destination.color255 = gte_math::distanceCueColor(
              color, farColor, static_cast<int16_t>(ir0));
        }
      } else if ((mesh.primitiveConstructor == UINT32_C(0x80040C1C) ||
                  mesh.primitiveConstructor == UINT32_C(0x80040C00)) &&
                 (drawCommand & UINT32_C(0x01000000)) != 0u) {
        destination.color255 = decodeColor(source.color);
      } else {
        destination.color255 = decodeDistanceCuedColor(node, source.color);
      }
    }
    if (mesh.primitiveConstructor == UINT32_C(0x8003DC00)) {
      if (!applyObjectNormalConstructor(node, normalHighlights, face)) {
        metrics::count("host.crash3.object_normal_constructor_rejected");
        mesh.status = ObjectMeshDecodeStatus::InvalidSourceManifest;
        return mesh;
      }
    }
    if (mesh.primitiveConstructor == UINT32_C(0x8003FC00) &&
        (node.perVertexFadeCaptured ||
         node.words[0x28u / 4u] == UINT32_C(0x8003D248))) {
      // 8003FD18 always selects GT3 semi-transparency. 8003FD08 tests the
      // complete faded words, including RGBC's unchanged high byte outside
      // the hard-zero interval. A zero contribution must not write depth.
      face.material.semiTransparent = true;
      face.constructorEmitsFace = std::any_of(
          face.vertices.begin(), face.vertices.end(), [](const auto &vertex) {
            return std::floor(vertex.camera[2]) > 3072.0 &&
                   ((vertex.packedSourceColor & UINT32_C(0xff000000)) != 0u ||
                    std::any_of(vertex.color255.begin(), vertex.color255.end(),
                                [](float color) { return color != 0.0f; }));
          });
    }
    mesh.faces.push_back(std::move(face));
    ++state.stripLength;
  }

  mesh.status = terminated ? ObjectMeshDecodeStatus::Complete
                           : ObjectMeshDecodeStatus::InvalidCommandStream;
  if (terminated) {
    uint64_t fingerprint = UINT64_C(1469598103934665603);
    uint64_t geometryFingerprint = UINT64_C(1469598103934665603);
    const auto mixInto = [](uint64_t &hash, uint32_t word) {
      for (uint32_t shift = 0u; shift < 32u; shift += 8u) {
        hash ^= static_cast<uint8_t>(word >> shift);
        hash *= UINT64_C(1099511628211);
      }
    };
    mixInto(fingerprint, static_cast<uint32_t>(sourceCommands->size()));
    mixInto(fingerprint, mesh.primitiveConstructor);
    for (const uint32_t command : *sourceCommands)
      mixInto(fingerprint, command);
    mesh.rendererProgramFingerprint = fingerprint;
    mixInto(geometryFingerprint, static_cast<uint32_t>(mesh.faces.size()));
    for (const ObjectPresentationFace &face : mesh.faces) {
      mixInto(geometryFingerprint, face.ordinal);
      mixInto(geometryFingerprint, face.originalBackfaceTest ? 1u : 0u);
      mixInto(geometryFingerprint, face.acceptsNegativeWinding ? 1u : 0u);
      for (std::size_t corner = 0u; corner < face.vertices.size(); ++corner) {
        // The high word contains the strip/cache operation and stack slot.
        // The low word selects colour/material state and is intentionally
        // absent from geometric identity.
        mixInto(geometryFingerprint,
                face.vertexCommands[corner] & UINT32_C(0xffff0000));
        mixInto(geometryFingerprint, face.vertices[corner].stackIndex);
      }
    }
    mesh.rendererGeometryFingerprint = geometryFingerprint;
  }
  return mesh;
}

} // namespace

bool hasIntrinsicObjectConstruction(
    const ObjectPresentationRendererNode &node) noexcept {
  if ((!node.listDiscovered && !node.directBindingDiscovered) ||
      !node.transformInstalled || !node.orderingTableBasesCaptured ||
      !node.type1StreamsCaptured || node.type1CommandWords.empty() ||
      node.type1CommandWords.back() != UINT32_MAX || node.scaleShift < 0 ||
      node.scaleShift >= 31)
    return false;
  // These single-pass entries have completely decoded construction rules.
  // A list-only node, pool residue, custom multi-pass callback or unprepared
  // stream does not become presentable merely because its bounds are visible.
  const uint32_t primitive = node.words[0x2cu / 4u];
  return primitive == UINT32_C(0x80040C1C) ||
         primitive == UINT32_C(0x80040C00) ||
         (primitive == UINT32_C(0x8003FC00) &&
          node.words[0x28u / 4u] == UINT32_C(0x8003D248)) ||
         (primitive == UINT32_C(0x8003DC00) &&
          node.words[0x28u / 4u] == UINT32_C(0x8003E1A4));
}

bool applyObjectNormalConstructor(const ObjectPresentationRendererNode &node,
                                  std::span<const uint8_t, 128> highlightTable,
                                  ObjectPresentationFace &face) noexcept {
  if (node.words[0x2cu / 4u] != UINT32_C(0x8003DC00) ||
      node.words[0x28u / 4u] != UINT32_C(0x8003E1A4) ||
      !face.material.decoded || !face.material.textured)
    return false;
  const auto &control = node.gteControlWords;
  std::array<std::array<int32_t, 2>, 3> nativeScreen{};
  for (uint32_t corner = 0u; corner < 3u; ++corner) {
    const auto &camera = face.vertices[corner].camera;
    for (double component : camera)
      if (!std::isfinite(component) || component < INT32_MIN ||
          component > INT32_MAX)
        return false;
    const auto z =
        static_cast<uint16_t>(std::clamp(std::floor(camera[2]), 0.0, 65535.0));
    const uint32_t q =
        gte_math::divide(static_cast<uint16_t>(control[GTE_H]), z);
    for (uint32_t axis = 0u; axis < 2u; ++axis)
      nativeScreen[corner][axis] = gte_math::screenCoordinate(
          static_cast<int32_t>(std::floor(camera[axis])), q,
          std::bit_cast<int32_t>(control[GTE_OFX + axis]));
  }
  const auto &a = nativeScreen[0];
  const auto &b = nativeScreen[1];
  const auto &c = nativeScreen[2];
  const int64_t area = int64_t{b[0] - a[0]} * (c[1] - a[1]) -
                       int64_t{b[1] - a[1]} * (c[0] - a[0]);
  // Reproduce the native NCLIP branch, including zero-area PS1 triangles.
  // Geometry remains unquantized; only this authored two-sided material rule
  // consumes the integer orientation (as the original constructor does).
  face.constructorBackFacing = (area < 0) != face.acceptsNegativeWinding;
  const uint32_t normalWord = face.vertices[0].packedSourceColor;
  std::array<int32_t, 3> normal{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    normal[axis] = static_cast<int8_t>(normalWord >> (axis * 8u)) * 32;
  // MVMVA(light, IR, zero, sf=1, lm=0), including signed IR saturation.
  const std::array<int16_t, 3> row0{lowSigned16(control[8]),
                                    highSigned16(control[8]),
                                    lowSigned16(control[9])};
  const std::array<int16_t, 3> row2{lowSigned16(control[11]),
                                    highSigned16(control[11]),
                                    lowSigned16(control[12])};
  const auto dot = [&](const auto &row) {
    int64_t sum = 0;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      sum += static_cast<int64_t>(row[axis]) * normal[axis];
    return static_cast<int32_t>(
        std::clamp(sum >> 12u, INT64_C(-32768), INT64_C(32767)));
  };
  const uint32_t light = static_cast<uint32_t>(dot(row0) + 4096);
  uint32_t intensity = (light * 7u + 8192u) >> 4u;
  if (face.constructorBackFacing)
    intensity >>= 1u;
  const int32_t cue =
      std::bit_cast<int32_t>(intensity - (node.words[0x1cu / 4u] & 0xffffu));
  const int32_t ir0 = static_cast<int16_t>(std::max(cue, 0));
  const int32_t reflection = dot(row2) >> 3u;
  const int32_t highlightIndex = std::abs(reflection) - 384;
  uint32_t highlight =
      highlightIndex < 0 ? 0u : highlightTable[std::min(highlightIndex, 127)];
  if (face.constructorBackFacing)
    highlight >>= 3u;
  std::array<float, 3> color{};
  for (uint32_t channel = 0u; channel < 3u; ++channel) {
    // 8003E1A4 installs RGBC=0; DPCS blends towards the captured far colour.
    const auto shaded = gte_math::interpolateColorChannel(
        0, std::bit_cast<int32_t>(control[GTE_RFC + channel]),
        static_cast<int16_t>(ir0), true, false);
    color[channel] = static_cast<float>(
        std::min(255u, static_cast<uint32_t>(shaded.color) + highlight));
  }
  for (auto &vertex : face.vertices)
    vertex.color255 = color; // Native flat-shaded FT3, not Gouraud RGB normals.
  const uint16_t authoredBlend = face.material.texturePage & 0x60u;
  face.material.semiTransparent =
      authoredBlend == 0u && !face.constructorBackFacing;
  face.material.texturePage |= face.material.semiTransparent ? 0x20u : 0x60u;
  return true;
}

ObjectPresentationScene
decodeObjectPresentationScene(const ObjectPresentationSnapshot &snapshot) {
  ObjectPresentationScene scene;
  scene.captureId = snapshot.captureId;
  scene.frameNumber = snapshot.frameNumber;
  const SnapshotRam ram(snapshot.guestRam);
  if (!ram.valid()) {
    for (const ObjectPresentationObject &object : snapshot.objects) {
      for (const ObjectPresentationRendererNode &node : object.rendererNodes) {
        if (object.modelType != 1u ||
            (!node.listDiscovered && !node.directBindingDiscovered))
          continue;
        ObjectPresentationMesh mesh;
        mesh.objectAddress = object.address;
        mesh.rendererNodeAddress = node.address;
        mesh.status = ObjectMeshDecodeStatus::MissingRamSnapshot;
        scene.meshes.push_back(mesh);
        ++scene.attemptedNodes;
      }
    }
    return scene;
  }

  for (const ObjectPresentationObject &object : snapshot.objects) {
    for (const ObjectPresentationRendererNode &node : object.rendererNodes) {
      if (object.modelType != 1u ||
          (!node.listDiscovered && !node.directBindingDiscovered))
        continue;
      ++scene.attemptedNodes;
      scene.meshes.push_back(decodeType1Mesh(ram, object, node));
      const ObjectPresentationMesh &mesh = scene.meshes.back();
      if (mesh.status == ObjectMeshDecodeStatus::Complete)
        ++scene.completeNodes;
      scene.faceCount += static_cast<uint32_t>(mesh.faces.size());
      scene.unresolvedMaterialFaces += mesh.unresolvedMaterialFaces;
    }
  }
  return scene;
}

ObjectFaceProjection projectObjectPresentationFace(
    const ObjectPresentationMesh &mesh, const ObjectPresentationFace &face,
    ObjectPresentationProjectionMode mode, double cameraScale) noexcept {
  ObjectFaceProjection result;
  if (!face.constructorEmitsFace) {
    result.status = ObjectFaceProjectionStatus::ConstructorSuppressed;
    return result;
  }
  if (mesh.projectionPlane == 0u || mesh.scaleShift < 0 ||
      mesh.scaleShift >= 31 || !std::isfinite(cameraScale) ||
      cameraScale <= 0.0) {
    return result;
  }

  // GTE division saturates while H >= 2 * SZ. Packet comparisons retain that
  // limit. The modern sidecar instead clips at the first representable
  // positive 4.12 camera-space quantum: it has no reason to inherit the PS1
  // divider's artificial near plane.
  const double nearZ =
      mode == ObjectPresentationProjectionMode::PositiveCamera
          ? 1.0 / 4096.0
          : static_cast<double>(mesh.projectionPlane) * 0.5 + 1.0 / 4096.0;
  const double normalization = std::ldexp(1.0, mesh.scaleShift);
  const double appliedCameraScale =
      mode == ObjectPresentationProjectionMode::PositiveCamera ? cameraScale
                                                               : 1.0;
  const double originX =
      static_cast<double>(mesh.projectionOriginFixed[0]) / 65536.0;
  const double originY =
      static_cast<double>(mesh.projectionOriginFixed[1]) / 65536.0;
  if (!std::isfinite(nearZ) || !std::isfinite(normalization) ||
      !std::isfinite(originX) || !std::isfinite(originY)) {
    return result;
  }

  if (mode == ObjectPresentationProjectionMode::GteClampedApparent) {
    constexpr double kMaximumProjectionRatio =
        static_cast<double>(0x1ffffu) / 65536.0;
    ObjectProjectedTriangle &triangle = result.triangles[0];
    for (std::size_t corner = 0u; corner < face.vertices.size(); ++corner) {
      const ObjectPresentationVertex &source = face.vertices[corner];
      if (!std::isfinite(source.camera[0]) ||
          !std::isfinite(source.camera[1]) ||
          !std::isfinite(source.camera[2])) {
        return ObjectFaceProjection{};
      }
      const double ratio =
          static_cast<double>(mesh.projectionPlane) >= 2.0 * source.camera[2]
              ? kMaximumProjectionRatio
              : static_cast<double>(mesh.projectionPlane) / source.camera[2];
      const double apparentZ =
          static_cast<double>(mesh.projectionPlane) / ratio;
      ObjectProjectedVertex &destination = triangle.vertices[corner];
      destination.screen = {{originX + source.camera[0] * ratio,
                             originY + source.camera[1] * ratio}};
      destination.projectiveW = apparentZ * normalization;
      destination.uv = face.material.uv[corner];
      destination.color255 = source.color255;
      if (!std::isfinite(destination.screen[0]) ||
          !std::isfinite(destination.screen[1]) ||
          !std::isfinite(destination.projectiveW) ||
          destination.projectiveW <= 0.0) {
        return ObjectFaceProjection{};
      }
    }
    result.status = ObjectFaceProjectionStatus::Direct;
    result.triangleCount = 1u;
    return result;
  }

  bool requiresClip = false;
  bool anyInside = false;
  for (const ObjectPresentationVertex &vertex : face.vertices) {
    if (!std::isfinite(vertex.camera[0]) || !std::isfinite(vertex.camera[1]) ||
        !std::isfinite(vertex.camera[2])) {
      return result;
    }
    const bool inside = vertex.camera[2] * appliedCameraScale >= nearZ;
    anyInside |= inside;
    requiresClip |= !inside;
  }
  if (!anyInside) {
    result.status = ObjectFaceProjectionStatus::NearPlaneRejected;
    return result;
  }

  if (!requiresClip) {
    ObjectProjectedTriangle &triangle = result.triangles[0];
    for (std::size_t corner = 0u; corner < face.vertices.size(); ++corner) {
      const ObjectPresentationVertex &source = face.vertices[corner];
      if (mode == ObjectPresentationProjectionMode::PositiveCamera) {
        const double x = source.camera[0] * appliedCameraScale;
        const double y = source.camera[1] * appliedCameraScale;
        const double z = source.camera[2] * appliedCameraScale;
        ObjectProjectedVertex &destination = triangle.vertices[corner];
        destination.screen = {
            {originX + static_cast<double>(mesh.projectionPlane) * x / z,
             originY + static_cast<double>(mesh.projectionPlane) * y / z}};
        destination.projectiveW = z * normalization;
        destination.uv = face.material.uv[corner];
        destination.color255 = source.color255;
        if (!std::isfinite(destination.screen[0]) ||
            !std::isfinite(destination.screen[1]) ||
            !std::isfinite(destination.projectiveW) ||
            destination.projectiveW <= 0.0) {
          return ObjectFaceProjection{};
        }
      } else {
        if (!source.directProjectionValid || !std::isfinite(source.screen[0]) ||
            !std::isfinite(source.screen[1]) ||
            !std::isfinite(source.projectiveW) || source.projectiveW <= 0.0) {
          return result;
        }
        triangle.vertices[corner] = {source.screen, source.projectiveW,
                                     face.material.uv[corner], source.color255};
      }
    }
    result.status = ObjectFaceProjectionStatus::Direct;
    result.triangleCount = 1u;
    return result;
  }

  struct ClipVertex {
    std::array<double, 3> camera{};
    std::array<double, 3> barycentric{};
  };
  std::array<ClipVertex, 4> polygon{};
  uint8_t polygonCount = 0u;
  const auto sourceVertex = [&](std::size_t corner) {
    ClipVertex vertex;
    for (std::size_t component = 0u; component < vertex.camera.size();
         ++component) {
      vertex.camera[component] =
          face.vertices[corner].camera[component] * appliedCameraScale;
    }
    vertex.barycentric[corner] = 1.0;
    return vertex;
  };
  const auto append = [&](const ClipVertex &vertex) {
    if (polygonCount < polygon.size())
      polygon[polygonCount++] = vertex;
  };
  const auto intersection = [&](const ClipVertex &from, const ClipVertex &to) {
    const double denominator = to.camera[2] - from.camera[2];
    double t =
        denominator != 0.0 ? (nearZ - from.camera[2]) / denominator : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    ClipVertex clipped;
    for (std::size_t component = 0u; component < 3u; ++component) {
      clipped.camera[component] =
          from.camera[component] +
          (to.camera[component] - from.camera[component]) * t;
      clipped.barycentric[component] =
          from.barycentric[component] +
          (to.barycentric[component] - from.barycentric[component]) * t;
    }
    clipped.camera[2] = nearZ;
    return clipped;
  };

  ClipVertex previous = sourceVertex(face.vertices.size() - 1u);
  bool previousInside = previous.camera[2] >= nearZ;
  for (std::size_t corner = 0u; corner < face.vertices.size(); ++corner) {
    const ClipVertex current = sourceVertex(corner);
    const bool currentInside = current.camera[2] >= nearZ;
    if (currentInside != previousInside)
      append(intersection(previous, current));
    if (currentInside)
      append(current);
    previous = current;
    previousInside = currentInside;
  }
  if (polygonCount < 3u) {
    result.status = ObjectFaceProjectionStatus::NearPlaneRejected;
    return result;
  }

  const auto project = [&](const ClipVertex &source,
                           ObjectProjectedVertex &destination) {
    const double z = source.camera[2];
    destination.screen = {{originX + static_cast<double>(mesh.projectionPlane) *
                                         source.camera[0] / z,
                           originY + static_cast<double>(mesh.projectionPlane) *
                                         source.camera[1] / z}};
    destination.projectiveW = z * normalization;
    for (std::size_t component = 0u; component < 2u; ++component) {
      double value = 0.0;
      for (std::size_t corner = 0u; corner < face.vertices.size(); ++corner) {
        value += source.barycentric[corner] *
                 static_cast<double>(face.material.uv[corner][component]);
      }
      destination.uv[component] = static_cast<float>(value);
    }
    for (std::size_t component = 0u; component < 3u; ++component) {
      double value = 0.0;
      for (std::size_t corner = 0u; corner < face.vertices.size(); ++corner) {
        value += source.barycentric[corner] *
                 static_cast<double>(face.vertices[corner].color255[component]);
      }
      destination.color255[component] = static_cast<float>(value);
    }
    return std::isfinite(destination.screen[0]) &&
           std::isfinite(destination.screen[1]) &&
           std::isfinite(destination.projectiveW) &&
           destination.projectiveW > 0.0;
  };

  for (uint8_t triangleIndex = 1u; triangleIndex + 1u < polygonCount;
       ++triangleIndex) {
    if (result.triangleCount >= result.triangles.size())
      return ObjectFaceProjection{};
    const std::array<uint8_t, 3> indices{
        {0u, triangleIndex, static_cast<uint8_t>(triangleIndex + 1u)}};
    ObjectProjectedTriangle &triangle = result.triangles[result.triangleCount];
    for (std::size_t corner = 0u; corner < indices.size(); ++corner) {
      if (!project(polygon[indices[corner]], triangle.vertices[corner]))
        return ObjectFaceProjection{};
    }
    ++result.triangleCount;
  }
  if (result.triangleCount == 0u)
    return ObjectFaceProjection{};
  result.status = ObjectFaceProjectionStatus::NearPlaneClipped;
  return result;
}

bool objectPresentationTrianglePassesBackface(
    const ObjectPresentationFace &face,
    const ObjectProjectedTriangle &triangle) noexcept {
  if (!face.originalBackfaceTest)
    return true;
  const auto &a = triangle.vertices[0].screen;
  const auto &b = triangle.vertices[1].screen;
  const auto &c = triangle.vertices[2].screen;
  const double twiceArea =
      (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
  return std::isfinite(twiceArea) && twiceArea != 0.0 &&
         (twiceArea < 0.0) == face.acceptsNegativeWinding;
}

ObjectPresentationDepthTransform
objectPresentationDepthTransform(const ObjectPresentationMesh &mesh,
                                 const ObjectPresentationFace &face,
                                 uint8_t sourceVertexCount) noexcept {
  if (!mesh.orderingTableBasesCaptured || mesh.scaleShift < 0 ||
      mesh.scaleShift >= 31 ||
      (sourceVertexCount != 3u && sourceVertexCount != 4u))
    return {};

  // This is the exact signed `command << 6` branch at 0x80040c0c. Unsigned
  // shift plus bit_cast preserves the R3000A wrap semantics without relying
  // on undefined signed overflow.
  const int32_t commandClass = std::bit_cast<int32_t>(face.command << 6u);
  const bool normalConstructor =
      mesh.primitiveConstructor == UINT32_C(0x8003DC00);
  // 80040C1C enters AFTER the class selector at 80040C00. 8003F8B8 has
  // installed the positive base; the other scratch word may be unrelated.
  const bool fixedBase = normalConstructor ||
                         mesh.primitiveConstructor == UINT32_C(0x80040C1C) ||
                         mesh.primitiveConstructor == UINT32_C(0x8003FC00);
  const uint32_t base = fixedBase || commandClass > 0
                            ? mesh.orderingTableBasePositive
                            : mesh.orderingTableBaseNonPositive;
  // The game intentionally permits negative origins: unsigned subtraction
  // followed by a signed branch clamps those cohorts to the far OT bucket.
  // Preserve the raw scratch word and interpret it as the R3000A did.
  const int32_t signedBase = std::bit_cast<int32_t>(base);
  // 0x80040c28 tests the sign BEFORE the delay-slot SLL 2. For a
  // non-negative origin, that shift discards bit 30 before OT addressing:
  // e.g. 0x40000770 and 0x00000770 produce the same bucket for the live
  // (non-clamped) interval. Treating the raw word as a world-space origin
  // instead creates a huge negative depth and pins source-absent faces to
  // the near plane. Negative origins must retain their signed interpretation
  // because the earlier branch deliberately clamps them to the far end.
  const int32_t continuousBase =
      signedBase >= 0 ? static_cast<int32_t>(base & UINT32_C(0x3fffffff))
                      : signedBase;

  constexpr double kOrderingTableEntries = 2048.0;
  constexpr double kWorldUnitsPerBucket = 32.0;
  constexpr double kPresentationNearGuard = 8192.0;
  // Crash inserts MAC0>>17, where MAC0 is ZSF3/4 times the sum of SZ. The
  // presentation domain assigns 32 units to one OT cell, hence the exact
  // continuous slope is vertexCount*ZSF/4096 before undoing the node's
  // normalization. This also preserves the small, intentional ZSF3=1365
  // distinction from an ideal arithmetic average.
  const int16_t zsf =
      sourceVertexCount == 3u ? mesh.orderingTableZsf3 : mesh.orderingTableZsf4;
  if (zsf <= 0)
    return {};
  const double multiplier = std::ldexp(
      static_cast<double>(sourceVertexCount) * zsf / 4096.0, -mesh.scaleShift);
  const double bias =
      kPresentationNearGuard +
      (kOrderingTableEntries - static_cast<double>(continuousBase)) *
          kWorldUnitsPerBucket +
      (normalConstructor && face.constructorBackFacing
           ? 2.0 * kWorldUnitsPerBucket
           : 0.0);
  if (!std::isfinite(multiplier) || multiplier <= 0.0 || !std::isfinite(bias) ||
      std::abs(bias) > static_cast<double>(std::numeric_limits<float>::max()))
    return {};
  return {true, static_cast<float>(multiplier), static_cast<float>(bias), base};
}

} // namespace ps1::games::crash3
