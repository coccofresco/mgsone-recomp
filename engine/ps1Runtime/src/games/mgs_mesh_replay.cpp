#include "runtime/games/mgs_mesh_replay.h"
#include "runtime/cpu_context.h"
#include "runtime/games/mgs_decomp.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/games/mgs_scene.h"
#include "runtime/gpu/modern_replay.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace ps1::games::mgs {
namespace {
const AssetCache *cache = nullptr;
bool enabled = false;
bool recoveryEnabled = false;
using Point = std::array<double, 3>;
struct SourcePoint {
  Point p;
  int16_t seam;
  uint32_t pointByte;
};
std::vector<std::vector<SourcePoint>> sourceVertices;
std::vector<std::vector<uint8_t>> sourceCorners;
struct CachedTexture {
  uint32_t index, px, py, cx, cy, width, height, depth, rowWords;
  std::vector<gpu::Color16> pixels, palette;
  std::vector<uint32_t> names;
};
std::unordered_map<uint32_t, std::vector<CachedTexture>> textures;
struct Face {
  uint64_t sequence = 0;
  uint32_t assetModel = 0;
  uint64_t sourceFace = 0;
  std::array<Point, 4> view;
  std::array<Point, 4> nativeView{};
  std::array<std::array<double, 2>, 4> reference{};
  std::array<std::array<int16_t, 2>, 4> xy;
  std::array<uint32_t, 4> colors;
  std::array<uint16_t, 4> uv;
  uint16_t tpage = 0, clut = 0;
  uint8_t opcode = 0, step = 12;
  bool procedural = false;
  bool precisePose = false;
  double projection = 0;
  gpu::replay::OriginalMeshGeometry geometry;
};
std::array<std::unordered_map<uint32_t, Face>, 2> faces;
// One source geometry registry, independent of packet allocation/culling.
// The packet map above is only a native material/order/provenance adapter.
std::array<std::unordered_map<uint64_t, Face>, 2> sceneFaces;
struct RecoveryFace {
  uint64_t sourceFace = 0;
  uint32_t nativePacket = 0;
  uint8_t bucket = 0;
  gpu::replay::DrawPacket draw;
};
struct RecoverySource {
  uint64_t capture = 0;
  uint32_t orderingTable = 0;
  std::vector<RecoveryFace> faces;
};
std::array<RecoverySource, 2> recoverySources;
using Weights = std::array<float, 4>;
struct SubdividedFace {
  uint32_t parent;
  std::array<Weights, 4> weights;
};
std::array<std::unordered_map<uint32_t, SubdividedFace>, 2> subdivided;
std::unordered_map<uint32_t, Weights> vectorWeights;
uint32_t divideBuffer = 2;
uint32_t u32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
uint16_t u16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
void keepTransportProvenance(gpu::replay::ReplayVertex &vertex) {
  const auto old = vertex.provenance;
  vertex.provenance = {};
  auto &p = vertex.provenance;
  p.gp0WordSource = old.gp0WordSource;
  p.dmaListSource = old.dmaListSource;
  p.dmaNodeSource = old.dmaNodeSource;
  p.dmaTraversalOrdinal = old.dmaTraversalOrdinal;
  p.dmaOrderingTableAnchorSource = old.dmaOrderingTableAnchorSource;
  p.evidence =
      old.evidence & (gpu::replay::ReplayVertexGp0WordSourceExact |
                      gpu::replay::ReplayVertexDmaTraversalExact |
                      gpu::replay::ReplayVertexOrderingTableAnchorExact);
}
Point transform(const std::array<float, 16> &m, const Point &v) {
  Point out{};
  for (unsigned r = 0; r < 3; ++r)
    out[r] = m[12 + r] + m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2];
  return out;
}
bool range(uint32_t p, uint64_t n) {
  const uint32_t a = p & 0x1fffffffu;
  return a < Memory::RAM_SIZE && n <= Memory::RAM_SIZE - a;
}
uint64_t sourceFaceId(uint32_t instance, uint32_t corner) {
  return uint64_t(instance & 0x1fffffffu) << 32 | corner;
}
// Clip in homogeneous camera space. This never observes the native SXY,
// NCLIP area, bound_mode or packet allocation when deciding visibility.
bool sourceFaceVisible(const std::array<Point, 4> &eye,
                       const SceneSnapshot &scene, bool bothSides) {
  const auto &a = eye[0], &b = eye[1], &c = eye[2];
  const double determinant = a[0] * (b[1] * c[2] - b[2] * c[1]) -
                             a[1] * (b[0] * c[2] - b[2] * c[0]) +
                             a[2] * (b[0] * c[1] - b[1] * c[0]);
  if ((!bothSides && determinant <= 0) || determinant == 0)
    return false;
  std::vector<Point> polygon{eye[0], eye[1], eye[3], eye[2]};
  const double left = scene.clip[0], top = scene.clip[1];
  const double right = left + scene.clip[2], bottom = top + scene.clip[3];
  for (unsigned plane = 0; plane < 5; ++plane) {
    const auto distance = [&](const Point &p) {
      switch (plane) {
      case 0:
        return p[2] - 1;
      case 1:
        return p[0] * scene.projection - left * p[2];
      case 2:
        return right * p[2] - p[0] * scene.projection;
      case 3:
        return p[1] * scene.projection - top * p[2];
      default:
        return bottom * p[2] - p[1] * scene.projection;
      }
    };
    if (polygon.empty())
      return false;
    std::vector<Point> clipped;
    Point previous = polygon.back();
    double before = distance(previous);
    for (const auto &point : polygon) {
      const double after = distance(point);
      if ((before < 0) != (after < 0)) {
        Point intersection{};
        const double t = before / (before - after);
        for (unsigned axis = 0; axis < 3; ++axis)
          intersection[axis] =
              previous[axis] + t * (point[axis] - previous[axis]);
        clipped.push_back(intersection);
      }
      if (after >= 0)
        clipped.push_back(point);
      previous = point;
      before = after;
    }
    polygon = std::move(clipped);
  }
  return polygon.size() >= 3;
}
// The sole packet producer for indexed models, skinned/deformed models,
// procedural effects and source faces without a native command. Materials
// and ordering are adapters; geometry/projection/depth policy are shared.
void applySourceGeometry(gpu::replay::DrawPacket &packet, const Face &face) {
  packet.originalMesh = face.geometry;
  packet.originalMesh->projection = {
      float(face.projection), float(packet.state.drawOffset.x),
      float(packet.state.drawOffset.y), face.procedural ? 2.0f : 1.0f};
  for (unsigned k = 0; k < 4; ++k) {
    auto &v = packet.vertices[k];
    v.w = std::max(1.0, face.view[k][2]);
    v.x = face.view[k][0] * face.projection / v.w + packet.state.drawOffset.x;
    v.y = face.view[k][1] * face.projection / v.w + packet.state.drawOffset.y;
    v.rawDepth = face.view[k][2];
    v.qx = v.x * v.w;
    v.qy = v.y * v.w;
    v.nearDepth = 1;
    v.preciseUV = packet.textured;
    v.preciseXY = v.preciseX = v.preciseY = v.rawDepthValid = v.homogeneousQ =
        true;
    v.inventedW = v.reconciledX = v.reconciledY = false;
    v.nearSaturated = v.rawDepth < v.nearDepth;
    keepTransportProvenance(v);
  }
  packet.route = gpu::replay::PacketRoute::Projected3D;
  packet.validMask = 15;
  packet.fallbackReason = gpu::replay::PacketFallbackReason::None;
  packet.state.depth.testEnabled = false;
  packet.state.depth.writeEnabled = !packet.state.blend.enabled;
  packet.state.depth.compare = gpu::replay::DepthCompare::Always;
  packet.state.depth.invalidateOnAccepted = packet.state.blend.enabled;
}
void prepareRecoverySources(const SceneSnapshot &scene, const Memory &memory,
                            RecoverySource &out) {
  if (!recoveryEnabled || scene.bufferIndex > 1 || scene.projection <= 0 ||
      scene.clip[2] <= 0 || scene.clip[3] <= 0 || !range(scene.channel, 100))
    return;
  const auto ptr = [&](uint32_t p) {
    return memory.ramPtr() + (p & 0x1fffffffu);
  };
  // USA DG_Chanls[1] uses a 257-entry reverse ordering table. Keep that
  // composition boundary while removing geometric rejection/allocation limits.
  const uint32_t table = u32(ptr(scene.channel + scene.bufferIndex * 4));
  if (u16(ptr(scene.channel + 8)) != 8 || !table || !range(table, 257 * 4))
    return;
  out.capture = scene.sequence;
  out.orderingTable = table & 0x1fffffffu;
  const auto texture = [&](uint16_t id) -> const uint8_t * {
    const uint32_t address = findSourceTexture(memory, id);
    return address ? ptr(address) : nullptr;
  };
  constexpr uint32_t order[] = {0, 1, 3, 2};
  for (const auto &instance : scene.instances) {
    if (!instance.visible || instance.asset == ~0u ||
        (instance.flags & 0x07u) != 0x07u || (instance.flags & 0x608u) ||
        !range(instance.guestAddress, 72))
      continue;
    const uint32_t first = cache->word(AssetCache::Assets, instance.asset, 9);
    const uint32_t total = instance.models.size();
    if (instance.primaryModels > total)
      continue;
    std::vector<uint32_t> owners(total, ~0u), packets(total, 0);
    bool valid = true;
    for (uint32_t root = 0; root < instance.primaryModels && valid; ++root) {
      uint32_t packet = u32(
          ptr(instance.models[root].guestAddress + 84 + scene.bufferIndex * 4));
      for (uint32_t model = root; model != ~0u;) {
        if (model >= total || owners[model] != ~0u) {
          valid = false;
          break;
        }
        owners[model] = root;
        packets[model] = packet;
        if (packet)
          packet += cache->word(AssetCache::Models, first + model, 4) / 4 * 52;
        const uint32_t next = cache->word(AssetCache::Models, first + model, 2);
        model = next == ~0u ? ~0u : next - first;
      }
    }
    if (!valid)
      continue;
    for (uint32_t m = 0; m < total; ++m) {
      if (owners[m] == ~0u)
        continue;
      const uint32_t object = instance.models[m].guestAddress;
      const uint32_t root = instance.models[owners[m]].guestAddress;
      const uint32_t descriptor = instance.definition + 32 + m * 88;
      if (!range(descriptor, 88) || !range(object, 92))
        continue;
      const uint32_t flags = u32(ptr(descriptor));
      if (flags & (2u | 0x10000u))
        continue; // transparent/indirect require their own source path
      const uint32_t global = first + m;
      const uint32_t begin = cache->word(AssetCache::Models, global, 3);
      const uint32_t count = cache->word(AssetCache::Models, global, 4) / 4;
      const uint32_t colors = u32(ptr(object + 68));
      const uint32_t uv = u32(ptr(descriptor + 76)),
                     ids = u32(ptr(descriptor + 80));
      if (!colors || !uv || !ids || !range(colors, uint64_t(count) * 16) ||
          !range(uv, uint64_t(count) * 8) || !range(ids, uint64_t(count) * 2))
        continue;
      for (uint32_t f = 0; f < count; ++f) {
        const uint32_t packet = packets[m] ? packets[m] + f * 52 : 0;
        if (packet && !range(packet, 52))
          continue;
        // An accepted native face may have visible subdivision children. Leave
        // the whole face to that existing path; never cover its children twice.
        const bool nativeBound =
            u16(ptr(instance.guestAddress + 50)) && u16(ptr(root + 76));
        if (nativeBound && packet && (u32(ptr(packet)) & 0xffffu))
          continue;
        const uint16_t id = u16(ptr(ids + f * 2));
        const auto *tex = texture(id);
        if (!tex || ((u16(tex + 4) >> 7) & 3) > 1)
          continue;
        RecoveryFace face;
        face.sourceFace = sourceFaceId(instance.guestAddress, begin + f * 4);
        face.nativePacket = packet & 0x1fffffffu;
        auto &draw = face.draw;
        draw.opcode = 0x3c;
        draw.textured = draw.gouraud = true;
        draw.vertexCount = 4;
        draw.indexCount = 6;
        draw.validMask = 15;
        draw.triangleIndices = {0, 1, 2, 1, 2, 3};
        const auto source = sceneFaces[scene.bufferIndex].find(face.sourceFace);
        if (source == sceneFaces[scene.bufferIndex].end())
          continue;
        draw.originalMesh = source->second.geometry;
        draw.originalMesh->textureId = id;
        draw.originalMesh->sourceRecovery = true;
        const uint16_t page = u16(tex + 4), clut = u16(tex + 6);
        draw.state.texturePage = {page, uint16_t((page & 15) * 64),
                                  uint16_t((page & 16) * 16),
                                  gpu::replay::TextureDepth((page >> 7) & 3)};
        draw.state.clut = {clut, uint16_t((clut & 63) * 16),
                           uint16_t(clut >> 6)};
        const auto &eye = source->second.view;
        bool complete = true;
        for (uint32_t k = 0; k < 4 && complete; ++k) {
          auto &v = draw.vertices[k];
          const uint32_t color = u32(ptr(colors + (f * 4 + order[k]) * 4));
          if (color >> 24 != 0x3c) {
            complete = false;
            break;
          }
          v.color = {uint8_t(color), uint8_t(color >> 8), uint8_t(color >> 16)};
          v.u = uint8_t(ptr(uv + f * 8)[order[k] * 2] *
                            (uint32_t(tex[10]) + 1) / 256 +
                        tex[8]);
          v.v = uint8_t(ptr(uv + f * 8)[order[k] * 2 + 1] *
                            (uint32_t(tex[11]) + 1) / 256 +
                        tex[9]);
          // Present allocated packets must agree with the persistent material.
          // Actor-authored packet overrides require a dedicated source path.
          if (packet && (u32(ptr(packet + 4 + k * 12)) != color ||
                         u16(ptr(packet + 12 + k * 12)) !=
                             uint16_t(uint16_t(v.v) << 8 | uint16_t(v.u)) ||
                         u16(ptr(packet + 14)) != clut ||
                         u16(ptr(packet + 26)) != page)) {
            complete = false;
            break;
          }
        }
        if (!complete || !sourceFaceVisible(eye, scene, flags & 0x400))
          continue;
        applySourceGeometry(draw, source->second);
        draw.originalMesh->textureId = id;
        draw.originalMesh->sourceRecovery = true;
        // Match the channel's coarse compositing slot, not its geometric
        // clipping. Sorting/transparent composition remain native for now.
        const int32_t depth = int32_t((std::clamp(eye[0][2], 0.0, 65535.0) +
                                       std::clamp(eye[3][2], 0.0, 65535.0)) *
                                      .5);
        face.bucket = uint16_t(depth - int16_t(u16(ptr(root + 80)))) >> 8;
        out.faces.push_back(std::move(face));
      }
    }
  }
  metrics::count("mgs.scene.source_candidates", out.faces.size());
  metrics::setState("mgs.scene.source_candidates", out.faces.size());
}
// Read original indexed positions once. ZMD offsets are relative to its shared
// vertex area; ordinary KMD offsets are relative to the definition itself.
void indexSourceVertices() {
  sourceVertices.clear();
  sourceCorners.clear();
  if (!cache || !enabled)
    return;
  sourceVertices.resize(cache->section(AssetCache::Models).count);
  sourceCorners.resize(sourceVertices.size());
  const auto bytes = cache->bytes();
  uint32_t models = 0;
  for (uint32_t m = 0; m < sourceVertices.size(); ++m) {
    const auto view = cache->sourceModel(m);
    if (!view)
      continue;
    const auto *indices = bytes.data() + view->vertexIndicesByte;
    sourceCorners[m].assign(indices, indices + uint64_t(view->faces) * 4);
    auto &out = sourceVertices[m];
    out.reserve(view->vertices);
    for (uint32_t v = 0; v < view->vertices; ++v) {
      const uint32_t offset = view->positionsByte + v * 8;
      const auto *p = bytes.data() + offset;
      out.push_back({{double(int16_t(u16(p))), double(int16_t(u16(p + 2))),
                      double(int16_t(u16(p + 4)))},
                     int16_t(u16(p + 6)),
                     offset | 0x80000000u});
    }
    ++models;
  }
  metrics::setState("mgs.source.original_format_models", models);
}
// The oracle retains GTE divide/IR/SXY saturation; the cache projection does
// not. Billboard rectangles perform two projections in DG_MakePrimsRectangle.
std::array<double, 2> referenceProjection(const Point &p, double h) {
  const double ratio =
      std::min(131071.0 / 65536.0, h / std::clamp(p[2], 1.0, 65535.0));
  return {
      std::clamp(std::clamp(p[0], -32768.0, 32767.0) * ratio, -1024.0, 1023.0),
      std::clamp(std::clamp(p[1], -32768.0, 32767.0) * ratio, -1024.0, 1023.0)};
}
bool buildSourceFace(const SceneSnapshot &scene, const SceneInstance &instance,
                     const Memory &memory, uint32_t first, uint32_t m,
                     std::span<const uint32_t> owners, uint32_t f, Face &face) {
  const auto ptr = [&](uint32_t address) {
    return memory.ramPtr() + (address & 0x1fffffffu);
  };
  const uint32_t global = first + m, total = instance.models.size();
  const uint32_t begin = cache->word(AssetCache::Models, global, 3);
  face.sequence = scene.sequence;
  face.assetModel = global;
  face.sourceFace = sourceFaceId(instance.guestAddress, begin + f * 4);
  face.precisePose = instance.pose.source == ScenePoseSource::Rotations ||
                     instance.pose.source == ScenePoseSource::Movements ||
                     instance.pose.inheritedRoot;
  face.projection = scene.projection;
  bool complete = true;
  constexpr unsigned order[] = {0, 1, 3, 2};
  for (unsigned k = 0; k < 4; ++k) {
    const uint32_t corner = begin + f * 4 + order[k];
    uint32_t model = m;
    if (f * 4 + order[k] >= sourceCorners[global].size()) {
      complete = false;
      break;
    }
    uint32_t indexedVertex = sourceCorners[global][f * 4 + order[k]];
    if (indexedVertex >= sourceVertices[global].size()) {
      complete = false;
      break;
    }
    const auto &source = sourceVertices[global][indexedVertex];
    Point position = source.p;
    int32_t seam = source.seam;
    uint32_t pointByte = source.pointByte;
    for (uint32_t step = 0; seam >= 0 && step <= total; ++step) {
      const uint32_t parent = cache->word(AssetCache::Models, first + model, 1);
      if (parent < first || parent - first >= total ||
          uint32_t(seam) >= sourceVertices[parent].size()) {
        complete = false;
        break;
      }
      model = parent - first;
      indexedVertex = seam;
      const auto &v = sourceVertices[parent][seam];
      position = v.p;
      seam = v.seam;
      pointByte = v.pointByte;
    }
    if (!complete || seam >= 0 || owners[model] == ~0u) {
      complete = false;
      break;
    }
    Point displacement{};
    if (instance.deformed) {
      const uint32_t descriptor = instance.definition + 32 + model * 88;
      if (!range(descriptor, 88)) {
        complete = false;
        break;
      }
      const uint32_t liveVertices = u32(ptr(descriptor + 56));
      const uint32_t count = u32(ptr(descriptor + 52));
      if (indexedVertex >= count || !range(liveVertices, uint64_t(count) * 8)) {
        complete = false;
        break;
      }
      for (unsigned axis = 0; axis < 3; ++axis) {
        const double live =
            int16_t(u16(ptr(liveVertices + indexedVertex * 8 + axis * 2)));
        displacement[axis] = live - position[axis];
      }
    }
    face.geometry.pointBytes[k] = pointByte;
    const auto &world = instance.models[owners[model]].presentationWorld;
    for (unsigned row = 0; row < 3; ++row) {
      auto &out = face.geometry.viewRows[k * 3 + row];
      for (unsigned col = 0; col < 4; ++col) {
        double value = col == 3 ? scene.view[12 + row] : 0;
        for (unsigned axis = 0; axis < 3; ++axis)
          value += double(scene.view[axis * 4 + row]) * world[col * 4 + axis];
        // USA DG_AdjustOverscan scales the entire Y row by 58/64.
        // Preserve its framing in float, alongside the 16:9 X scale.
        const double scale = row == 1
                                 ? 58.0 / 64.0
                                 : (row == 0 && scene.widescreen ? 0.75 : 1.0);
        out[col] = float(value * scale);
      }
    }
    // Dynamic deformation is a per-corner displacement of the cached
    // original, transformed as a vector. No reference screen positions
    // are uploaded as replacement geometry.
    for (unsigned row = 0; row < 3; ++row)
      for (unsigned axis = 0; axis < 3; ++axis)
        face.geometry.viewRows[k * 3 + row][3] +=
            face.geometry.viewRows[k * 3 + row][axis] * displacement[axis];
    for (unsigned axis = 0; axis < 3; ++axis)
      position[axis] += displacement[axis];
    face.view[k] = transform(scene.view, transform(world, position));
    face.nativeView[k] = transform(
        scene.view, transform(instance.models[owners[model]].world, position));
    if (scene.widescreen) {
      face.view[k][0] *= 0.75;
      face.nativeView[k][0] *= 0.75;
    }
    face.view[k][1] *= 58.0 / 64.0;
    face.nativeView[k][1] *= 58.0 / 64.0;
    // DG_TransChanl projects the already composed DG_OBJ.screen matrix.
    // Re-composing eye/world in float is not its oracle: overscan and every
    // matrix product truncate to Q12, magnifying the difference near camera.
    // Unbounded source-only faces have no current native projection to check.
    const uint32_t owner = instance.models[owners[model]].guestAddress;
    if (range(owner, 92) && u16(ptr(owner + 76)) != 0) {
      const auto *screen = ptr(owner + 32);
      for (unsigned row = 0; row < 3; ++row) {
        double value = double(int32_t(u32(screen + 20 + row * 4))) * 4096;
        for (unsigned axis = 0; axis < 3; ++axis)
          value += int16_t(u16(screen + (row * 3 + axis) * 2)) * position[axis];
        face.nativeView[k][row] = std::floor(value / 4096.0);
      }
    }
    face.reference[k] =
        referenceProjection(face.nativeView[k], face.projection);
  }
  return complete;
}
void preparePrimitiveFaces(const SceneSnapshot &scene, const Memory &memory,
                           std::unordered_map<uint32_t, Face> &output) {
  if (cache->section(AssetCache::Templates).count != 4)
    return;
  const auto *ram = memory.ramPtr();
  const auto ptr = [&](uint32_t p) { return ram + (p & 0x1fffffffu); };
  const auto *c = ptr(scene.channel);
  const uint32_t size = u16(c + 82), first = u16(c + 84), queue = u32(c + 88);
  if (size > 256 || first > size || !range(queue, size * 4))
    return;
  const auto matrix = [&](uint32_t address) {
    std::array<float, 16> m{};
    m[15] = 1;
    for (uint32_t row = 0; row < 3; ++row) {
      for (uint32_t col = 0; col < 3; ++col)
        m[col * 4 + row] =
            int16_t(u16(ptr(address + (row * 3 + col) * 2))) / 4096.0f;
      m[12 + row] = int32_t(u32(ptr(address + 20 + row * 4)));
    }
    return m;
  };
  uint32_t prepared = 0;
  for (uint32_t i = first; i < size; ++i) {
    const uint32_t address = u32(ptr(queue + i * 4));
    if (!range(address, 84))
      continue;
    const auto *p = ptr(address);
    const uint32_t type = u32(p + 36), group = u16(p + 40), count = u16(p + 42);
    if ((type & 0x2900) || (group && !(group & scene.currentGroup)) ||
        count > 1024)
      continue;
    const bool flat = (type & 31) == 18, gouraud = (type & 31) == 20;
    if (!flat && !gouraud)
      continue;
    const uint32_t step = flat ? 8 : 12, packetSize = flat ? 40 : 52;
    if (u16(p + 48) != packetSize || u16(p + 50) != 4 || u16(p + 52) != 8 ||
        u16(p + 54) != step)
      continue;
    const uint32_t positions = u32(p + 56), rect = u32(p + 60),
                   packs = u32(p + 64 + scene.bufferIndex * 4);
    const bool rectangle = (type & 0x400) != 0, camera = (type & 0x200) != 0;
    if (!packs || !positions || !range(packs, uint64_t(count) * packetSize) ||
        !range(positions, uint64_t(count) * (rectangle ? 1 : 4) * 8) ||
        (rectangle && !range(rect, 8)))
      continue;
    const auto world = matrix(address);
    for (uint32_t n = 0; n < count; ++n) {
      const uint32_t packet = packs + n * packetSize;
      const auto *pack = ptr(packet);
      if ((pack[7] & 0xfc) != (flat ? 0x2c : 0x3c))
        continue;
      Face face;
      face.assetModel = ~0u;
      face.sourceFace = sourceFaceId(address, n | 0x80000000u);
      face.procedural = true;
      face.step = step;
      face.sequence = scene.sequence;
      face.projection = scene.projection;
      face.opcode = pack[7];
      face.clut = u16(pack + 14);
      face.tpage = u16(pack + 14 + step);
      for (uint32_t k = 0; k < 4; ++k) {
        const auto *v = ptr(
            positions + (n * (rectangle ? 1 : 4) + (rectangle ? 0 : k)) * 8);
        Point point{double(int16_t(u16(v))), double(int16_t(u16(v + 2))),
                    double(int16_t(u16(v + 4)))};
        if (!camera) {
          point = transform(scene.view, transform(world, point));
          // DG_PrimChanl has its own inline overscan, distinct from KMD.
          point[1] *= 58.0 / 64.0;
        }
        auto reference = referenceProjection(point, scene.projection);
        if (rectangle) {
          const double ratio = std::min(
              131071.0 / 65536.0, scene.projection / std::max(1.0, point[2]));
          const int16_t deltaX = -int16_t(u16(ptr(rect))) +
                                 int32_t(k & 1) * int16_t(u16(ptr(rect + 4)));
          const int16_t deltaY = -int16_t(u16(ptr(rect + 2))) +
                                 int32_t(k >> 1) * int16_t(u16(ptr(rect + 6)));
          reference[0] =
              std::clamp(reference[0] + deltaX * ratio, -1024.0, 1023.0);
          reference[1] =
              std::clamp(reference[1] + deltaY * ratio, -1024.0, 1023.0);
          point[0] += -int16_t(u16(ptr(rect))) +
                      int32_t(k & 1) * int16_t(u16(ptr(rect + 4)));
          point[1] += -int16_t(u16(ptr(rect + 2))) +
                      int32_t(k >> 1) * int16_t(u16(ptr(rect + 6)));
        }
        if (!camera && scene.widescreen) {
          point[0] *= 0.75;
          // Native DG_PrimChanl inlines overscan and bypasses the KMD wide
          // hook. Verify its unscaled X independently while aligning the
          // modern world-space effect with the widened model projection.
        }
        face.reference[k] = reference;
        face.view[k] = point;
        face.nativeView[k] = point;
        face.geometry.pointBytes[k] =
            cache->section(AssetCache::Templates).offset + k * 16;
        face.xy[k] = {int16_t(u16(pack + 8 + k * step)),
                      int16_t(u16(pack + 10 + k * step))};
        face.uv[k] = u16(pack + 12 + k * step);
        face.colors[k] = u32(pack + 4 + (gouraud ? k * step : 0)) & 0xffffff;
      }
      // A resident unit quad plus current actor shape. A non-planar fourth
      // corner is represented as deformation of that same canonical quad.
      for (uint32_t k = 0; k < 4; ++k) {
        for (uint32_t axis = 0; axis < 3; ++axis) {
          auto &row = face.geometry.viewRows[k * 3 + axis];
          row[0] = face.view[1][axis] - face.view[0][axis];
          row[1] = face.view[2][axis] - face.view[0][axis];
          row[3] = face.view[0][axis];
          if (k == 3)
            row[3] += face.view[3][axis] - face.view[2][axis] -
                      face.view[1][axis] + face.view[0][axis];
        }
      }
      sceneFaces[scene.bufferIndex].emplace(face.sourceFace, face);
      if (output.emplace(packet & 0x1fffffffu, face).second)
        ++prepared;
    }
  }
  metrics::setState("mgs.primitive.prepared_faces", prepared);
}
void indexTextures() {
  textures.clear();
  if (!cache || !enabled)
    return;
  std::unordered_map<uint32_t, std::vector<uint32_t>> names;
  for (uint32_t n = 0; n < cache->section(AssetCache::Names).count; ++n)
    names[cache->word(AssetCache::Names, n, 1)].push_back(
        cache->word(AssetCache::Names, n, 0));
  for (uint32_t t = 0; t < cache->section(AssetCache::Textures).count; ++t) {
    CachedTexture tex{};
    tex.index = t;
    tex.width = cache->word(AssetCache::Textures, t, 0);
    tex.height = cache->word(AssetCache::Textures, t, 1);
    tex.depth = cache->word(AssetCache::Textures, t, 6) & 1;
    const uint32_t asset = cache->word(AssetCache::Textures, t, 7);
    tex.names = names[asset];
    const uint32_t sourceSize = cache->word(AssetCache::Assets, asset, 13);
    if (sourceSize < 88)
      continue;
    const auto *pcx = cache->bytes().data() +
                      cache->section(AssetCache::Source).offset +
                      cache->word(AssetCache::Assets, asset, 12);
    tex.px = u16(pcx + 78);
    tex.py = u16(pcx + 80);
    tex.cx = u16(pcx + 82);
    tex.cy = u16(pcx + 84);
    const uint32_t ppw = tex.depth ? 2 : 4;
    if (tex.width % ppw || tex.px + tex.width / ppw > 1024 ||
        tex.py + tex.height > 512 || tex.cy >= 512 || tex.cx % 16)
      continue;
    tex.rowWords = tex.width / ppw;
    const uint32_t colors = cache->word(AssetCache::Textures, t, 5);
    if (tex.cx + colors > 1024)
      continue;
    tex.palette.resize(colors);
    for (uint32_t c = 0; c < colors; ++c)
      tex.palette[c].raw = cache->word(
          AssetCache::Palettes, cache->word(AssetCache::Textures, t, 4) + c, 0);
    const auto *indices = cache->bytes().data() +
                          cache->section(AssetCache::Indices).offset +
                          cache->word(AssetCache::Textures, t, 2);
    tex.pixels.resize(tex.rowWords * tex.height);
    bool valid = true;
    for (uint32_t i = 0; i < tex.width * tex.height; ++i) {
      if (indices[i] >= colors || indices[i] >= (tex.depth ? 256 : 16)) {
        valid = false;
        break;
      }
      tex.pixels[i / ppw].raw |= uint16_t(indices[i])
                                 << ((i % ppw) * (tex.depth ? 8 : 4));
    }
    if (!valid)
      continue;
    const uint32_t key = (tex.cy * 64 + tex.cx / 16) | (tex.depth << 16);
    textures[key].push_back(std::move(tex));
  }
}
} // namespace
void resetMeshReplay() {
  divideBuffer = 2;
  vectorWeights.clear();
  for (auto &buffer : subdivided)
    buffer.clear();
  for (auto &buffer : faces)
    buffer.clear();
  for (auto &source : recoverySources)
    source = {};
  for (auto &source : sceneFaces)
    source.clear();
}
void setMeshReplayCache(const AssetCache *value) {
  cache = value;
  const char *mode = std::getenv("PS1_MGS_MESH_REPLAY");
  enabled = cache && mode && std::string_view(mode) == "1";
  const char *recovery = std::getenv("PS1_MGS_SCENE_RECOVERY");
  recoveryEnabled = enabled && (!recovery || std::string_view(recovery) != "0");
  resetMeshReplay();
  indexSourceVertices();
  indexTextures();
}
void beginMeshDivide(recomp_context *ctx) {
  if (!enabled || !active() || !ctx || !ctx->mem)
    return;
  divideBuffer = ctx->r5;
  vectorWeights.clear();
  if (divideBuffer < 2)
    subdivided[divideBuffer].clear();
}
void meshDivideGrid(recomp_context *ctx) {
  if (!enabled || !active() || !ctx || !ctx->mem || divideBuffer > 1)
    return;
  const uint32_t grid = ctx->mem->read32(0x1f800034);
  if (grid < 0x1f800038 || grid > 0x1f80034c)
    return;
  constexpr uint32_t corners[] = {0, 2, 6, 8};
  if (grid == 0x1f800038) {
    vectorWeights.clear();
    for (uint32_t k = 0; k < 4; ++k) {
      Weights w{};
      w[k] = 1;
      vectorWeights[grid + corners[k] * 20] = w;
    }
  }
  for (uint32_t corner : corners)
    if (!vectorWeights.contains(grid + corner * 20))
      return;
  const auto average = [&](uint32_t a, uint32_t b, uint32_t c) {
    Weights w{};
    for (uint32_t k = 0; k < 4; ++k)
      w[k] = (vectorWeights.at(grid + a * 20)[k] +
              vectorWeights.at(grid + b * 20)[k]) *
             0.5f;
    vectorWeights[grid + c * 20] = w;
  };
  average(0, 2, 1);
  average(6, 8, 7);
  average(0, 6, 3);
  average(1, 7, 4);
  average(2, 8, 5);
}
void meshDivideCopy(recomp_context *ctx) {
  if (!enabled || !active() || !ctx || !ctx->mem || divideBuffer > 1)
    return;
  const uint32_t source = ctx->r4, target = ctx->mem->read32(0x1f800034);
  if (target < 0x1f800038 || target > 0x1f80034c)
    return;
  constexpr uint32_t input[] = {0, 1, 3, 4}, output[] = {0, 2, 6, 8};
  for (uint32_t k : input)
    if (!vectorWeights.contains(source + k * 20))
      return;
  for (uint32_t k = 0; k < 4; ++k)
    vectorWeights[target + output[k] * 20] =
        vectorWeights.at(source + input[k] * 20);
}
void meshDividePacket(recomp_context *ctx) {
  if (!enabled || !active() || !ctx || !ctx->mem || divideBuffer > 1)
    return;
  const uint32_t child = ctx->r7 & 0x1fffffffu;
  const uint32_t parent = ctx->mem->read32(0x1f80001c) & 0x1fffffffu;
  if (!range(child, 52) || !range(parent, 52))
    return;
  SubdividedFace face{};
  face.parent = parent;
  constexpr uint32_t input[] = {0, 1, 3, 4};
  for (uint32_t k = 0; k < 4; ++k) {
    const auto it = vectorWeights.find(ctx->r16 + input[k] * 20);
    if (it == vectorWeights.end())
      return;
    face.weights[k] = it->second;
  }
  subdivided[divideBuffer][child] = face;
}
void prepareMeshReplay(recomp_context *ctx) {
  if (!enabled || !active() || !ctx || !ctx->mem)
    return;
  const auto scene = latestSceneSnapshot();
  const uint32_t buffer = ctx->r5;
  if (buffer > 1) {
    resetMeshReplay();
    return;
  }
  auto &output = faces[buffer];
  output.clear();
  recoverySources[buffer] = {};
  sceneFaces[buffer].clear();
  std::unordered_set<uint32_t> ambiguous;
  if (!scene || scene->channel != ctx->r4 || scene->bufferIndex != buffer)
    return;
  const auto *ram = ctx->mem->ramPtr();
  const auto ptr = [&](uint32_t address) {
    return ram + (address & 0x1fffffffu);
  };
  constexpr unsigned order[] = {0, 1, 3, 2};
  for (const auto &instance : scene->instances) {
    if (!instance.visible || instance.asset == ~0u ||
        !range(instance.guestAddress, 72))
      continue;
    if (u32(ptr(instance.guestAddress + 36)) != instance.definition)
      continue;
    const uint32_t n = u16(ptr(instance.guestAddress + 46));
    const uint32_t first = cache->word(AssetCache::Assets, instance.asset, 9);
    const uint32_t total = instance.models.size();
    if (n > total)
      continue;
    // Extend parts use their primary owner's transform, not the uninitialized
    // MATRIX in the extra DG_OBJ. Reject shared/cyclic ownership.
    std::vector<uint32_t> owners(total, ~0u);
    bool valid = true;
    for (uint32_t root = 0; root < n && valid; ++root) {
      for (uint32_t m = root; m != ~0u;) {
        if (m >= total || owners[m] != ~0u) {
          valid = false;
          break;
        }
        owners[m] = root;
        const uint32_t next = cache->word(AssetCache::Models, first + m, 2);
        m = next == ~0u ? ~0u : next - first;
      }
    }
    if (!valid)
      continue;
    for (uint32_t root = 0; root < n; ++root) {
      const uint32_t object = instance.models[root].guestAddress;
      if (!range(object, 92))
        continue;
      uint32_t packet = u32(ptr(object + 84 + buffer * 4));
      for (uint32_t m = root; m != ~0u;) {
        const uint32_t global = first + m;
        const uint32_t corners = cache->word(AssetCache::Models, global, 4);
        const uint32_t begin = cache->word(AssetCache::Models, global, 3);
        const uint32_t obj = instance.models[m].guestAddress;
        if (!range(obj, 92) || u16(ptr(obj + 82)) != corners / 4 ||
            (packet && !range(packet, uint64_t(corners / 4) * 52)))
          break;
        for (uint32_t f = 0; f < corners / 4;
             ++f, packet = packet ? packet + 52 : 0) {
          Face face{};
          if (!buildSourceFace(*scene, instance, *ctx->mem, first, m, owners, f,
                               face))
            continue;
          sceneFaces[buffer].emplace(face.sourceFace, face);
          if (!packet)
            continue;
          const auto *p = ptr(packet);
          if ((p[7] & 0xfcu) != 0x3cu)
            continue;
          face.opcode = p[7];
          face.projection = scene->projection;
          face.clut = u16(p + 14);
          face.tpage = u16(p + 26);
          const uint32_t descriptor = u32(ptr(obj + 64));
          if (range(descriptor, 88)) {
            const uint32_t texids = u32(ptr(descriptor + 80));
            if (texids && range(texids, uint64_t(corners / 4) * 2))
              face.geometry.textureId = u16(ptr(texids + f * 2));
          }
          for (unsigned k = 0; k < 4; ++k) {
            face.xy[k] = {int16_t(u16(p + 8 + k * 12)),
                          int16_t(u16(p + 10 + k * 12))};
            face.colors[k] = u32(p + 4 + k * 12) & 0xffffffu;
            face.uv[k] = u16(p + 12 + k * 12);
          }
          {
            const uint32_t key = packet & 0x1fffffffu;
            if (ambiguous.contains(key))
              continue;
            if (!output.emplace(key, face).second) {
              output.erase(key);
              ambiguous.insert(key);
            }
          }
        }
        const uint32_t next = cache->word(AssetCache::Models, global, 2);
        m = next == ~0u ? ~0u : next - first;
      }
    }
  }
  preparePrimitiveFaces(*scene, *ctx->mem, output);
  uint32_t derived = 0;
  for (const auto &[child, link] : subdivided[buffer]) {
    const auto parent = output.find(link.parent);
    if (parent == output.end() || output.contains(child) || !range(child, 52))
      continue;
    Face face = parent->second;
    face.geometry.weights = link.weights;
    const auto *p = ptr(child);
    if (p[7] != face.opcode || u16(p + 14) != face.clut ||
        u16(p + 26) != face.tpage)
      continue;
    for (uint32_t k = 0; k < 4; ++k) {
      face.view[k] = {};
      face.nativeView[k] = {};
      for (uint32_t source = 0; source < 4; ++source)
        for (uint32_t axis = 0; axis < 3; ++axis) {
          face.view[k][axis] +=
              link.weights[k][source] * parent->second.view[source][axis];
          face.nativeView[k][axis] +=
              link.weights[k][source] * parent->second.nativeView[source][axis];
        }
      face.reference[k] = referenceProjection(face.nativeView[k], face.projection);
      face.xy[k] = {int16_t(u16(p + 8 + k * 12)),
                    int16_t(u16(p + 10 + k * 12))};
      face.colors[k] = u32(p + 4 + k * 12) & 0xffffff;
      face.uv[k] = u16(p + 12 + k * 12);
    }
    output.emplace(child, face);
    ++derived;
  }
  metrics::setState("mgs.mesh.subdivided_faces", derived);
  metrics::count("mgs.mesh.prepared_frames");
  metrics::setState("mgs.mesh.prepared_faces", output.size());
  metrics::setState("mgs.scene.source_geometry_faces",
                    sceneFaces[buffer].size());
  prepareRecoverySources(*scene, *ctx->mem, recoverySources[buffer]);
}
std::unique_ptr<SceneRecoveryReplay>
recoverSceneReplay(uint32_t listStart, std::span<const uint8_t> ram) {
  if (!recoveryEnabled || !active() || ram.size() != Memory::RAM_SIZE ||
      (recoverySources[0].faces.empty() && recoverySources[1].faces.empty()))
    return {};
  metrics::count("mgs.scene.recovery_list_attempts");
  std::unordered_set<uint32_t> nodes;
  uint32_t address = listStart & 0x1fffffffu;
  bool complete = false;
  for (uint32_t n = 0; n < 65536; ++n) {
    if (address & 3 || !range(address, 4) || !nodes.insert(address).second) {
      metrics::count("mgs.scene.recovery_invalid_list");
      return {};
    }
    const uint32_t tag = u32(ram.data() + address);
    if (!range(address, (uint64_t(tag >> 24) + 1) * 4)) {
      metrics::count("mgs.scene.recovery_invalid_list");
      return {};
    }
    address = tag & 0xffffffu;
    if (address == 0xffffffu) {
      complete = true;
      break;
    }
  }
  if (!complete)
    return {};
  uint32_t selected = 2;
  for (uint32_t buffer = 0; buffer < 2; ++buffer) {
    const auto &source = recoverySources[buffer];
    if (!source.capture || source.faces.empty())
      continue;
    bool found = true;
    for (uint32_t slot = 0; slot <= 256; ++slot) {
      const uint32_t node = source.orderingTable + slot * 4;
      if (!nodes.contains(node) || u32(ram.data() + node) >> 24) {
        metrics::trace("mgs.scene.recovery_ot_mismatch",
                       {{"table", source.orderingTable},
                        {"slot", slot},
                        {"present", nodes.contains(node)},
                        {"header", u32(ram.data() + node)},
                        {"start", listStart},
                        {"capture", int64_t(source.capture)}});
        found = false;
        break;
      }
    }
    if (found) {
      if (selected != 2)
        return {}; // ambiguous channel/buffer identity
      selected = buffer;
    }
  }
  if (selected == 2)
    return {};
  const auto &source = recoverySources[selected];
  std::unordered_set<uint64_t> represented;
  for (uint32_t node : nodes) {
    const auto face = faces[selected].find(node);
    if (face != faces[selected].end())
      represented.insert(face->second.sourceFace);
  }
  auto result = std::make_unique<SceneRecoveryReplay>();
  result->capture = source.capture;
  result->orderingTable = source.orderingTable;
  uint32_t recovered = 0;
  for (const auto &face : source.faces) {
    if (represented.contains(face.sourceFace) ||
        (face.nativePacket && nodes.contains(face.nativePacket))) {
      metrics::count("mgs.scene.recovery_duplicate_prevented");
      continue;
    }
    result->buckets[face.bucket].push_back(face.draw);
    ++recovered;
  }
  metrics::count("mgs.scene.recovery_lists");
  metrics::setState("mgs.scene.recovery_faces", recovered);
  return recovered ? std::move(result) : nullptr;
}
bool bindRecoveredSceneDraw(gpu::replay::DrawPacket &packet,
                            const gpu::replay::BakedDrawState &environment,
                            std::span<const gpu::Color16> vram) {
  if (!packet.originalMesh || environment.texturePage.textureDisable ||
      environment.mask.checkMaskBit || environment.mask.forceMaskBit ||
      environment.textureWindow.raw || environment.textureWindow.maskX ||
      environment.textureWindow.maskY || environment.depth.epoch == 0)
    return false;
  auto &state = packet.state;
  state.drawArea = environment.drawArea;
  state.drawOffset = environment.drawOffset;
  state.dither = environment.dither;
  state.drawToDisplayArea = environment.drawToDisplayArea;
  state.texturePage.raw = (state.texturePage.raw & ~0x600u) |
                          (state.dither ? 0x200u : 0u) |
                          (state.drawToDisplayArea ? 0x400u : 0u);
  // Even an opaque material retains its page's ABR selector. Keeping the
  // default mode here disagrees with raw tpage and rejects the entire frame
  // when a recovered face uses one of MGS's nonzero-ABR material pages.
  state.blend.mode = gpu::replay::BlendMode((state.texturePage.raw >> 5) & 3);
  state.depth = {false, true, gpu::replay::DepthCompare::Always,
                 environment.depth.epoch, false};
  state.textureUvDomain.enabled = true;
  double minU = 255, maxU = 0, minV = 255, maxV = 0;
  for (auto &v : packet.vertices) {
    v.x += state.drawOffset.x;
    v.y += state.drawOffset.y;
    v.qx = v.x * v.w;
    v.qy = v.y * v.w;
    v.nativeX = int16_t(std::clamp(v.x, -32768.0, 32767.0));
    v.nativeY = int16_t(std::clamp(v.y, -32768.0, 32767.0));
    minU = std::min(minU, v.u);
    maxU = std::max(maxU, v.u);
    minV = std::min(minV, v.v);
    maxV = std::max(maxV, v.v);
  }
  state.textureUvDomain.minU = minU;
  state.textureUvDomain.maxU = maxU;
  state.textureUvDomain.minV = minV;
  state.textureUvDomain.maxV = maxV;
  packet.originalMesh->projection[1] = state.drawOffset.x;
  packet.originalMesh->projection[2] = state.drawOffset.y;
  bindOriginalTexture(packet, vram);
  return packet.originalTexture.has_value();
}
void adaptMeshReplayPacket(gpu::replay::DrawPacket &packet) {
  if (!enabled || !active())
    return;
  if (replaceMeshReplayPacket(packet) ||
      packet.fallbackReason == gpu::replay::PacketFallbackReason::None)
    return;
  // Keep unresolved packets explicit and wholly native in screen space. No
  // partial PGXP coordinates or invented depth can certify an original mesh.
  packet.validMask = 0;
  packet.fallbackReason =
      gpu::replay::PacketFallbackReason::MissingPrecisionPacketAtomic;
  for (unsigned i = 0; i < packet.vertexCount; ++i) {
    auto &v = packet.vertices[i];
    v.x = v.nativeX;
    v.y = v.nativeY;
    v.w = gpu::replay::CRASH_REPLAY_FALLBACK_W;
    v.rawDepth = v.nearDepth = 0;
    v.qx = v.qy = 0;
    v.preciseXY = v.preciseX = v.preciseY = v.preciseUV = v.rawDepthValid =
        v.homogeneousQ = false;
    v.reconciledX = v.reconciledY = v.nearSaturated = false;
    v.inventedW = true;
    keepTransportProvenance(v);
  }
  packet.state.depth.testEnabled = packet.state.depth.writeEnabled = false;
  packet.state.depth.compare = gpu::replay::DepthCompare::Always;
  packet.state.depth.invalidateOnAccepted = packet.state.blend.enabled;
  metrics::count("mgs.mesh.native_projection_packets");
}
void bindOriginalTexture(gpu::replay::DrawPacket &packet,
                         std::span<const gpu::Color16> vram) {
  packet.originalTexture.reset();
  if (!enabled || !active() || !packet.textured || vram.size() != 1024u * 512u)
    return;
  metrics::count("mgs.texture.seen_packets");
  const uint32_t page = packet.state.texturePage.raw;
  const uint32_t depth = (page >> 7) & 3;
  const auto it = textures.find(packet.state.clut.raw | (depth << 16));
  if (it == textures.end()) {
    metrics::trace("mgs.texture.miss", {{"opcode", packet.opcode},
                                        {"page", page},
                                        {"clut", packet.state.clut.raw},
                                        {"reason", 1}});
    return;
  }
  const uint32_t ppw = depth ? 2 : 4;
  const int32_t pageX = (page & 15) * 64 * ppw, pageY = ((page >> 4) & 1) * 256;
  int32_t low[2] = {INT32_MAX, INT32_MAX}, high[2] = {INT32_MIN, INT32_MIN};
  for (uint32_t k = 0; k < packet.vertexCount; ++k) {
    const double uv[] = {packet.vertices[k].u, packet.vertices[k].v};
    for (uint32_t axis = 0; axis < 2; ++axis) {
      if (!std::isfinite(uv[axis]) || std::abs(uv[axis]) > 4096)
        return;
      low[axis] = std::min(low[axis], int32_t(uv[axis]));
      high[axis] = std::max(high[axis], int32_t(uv[axis]));
    }
  }
  if (packet.state.textureUvDomain.enabled) {
    const auto &domain = packet.state.textureUvDomain;
    low[0] = int32_t(domain.minU);
    high[0] = int32_t(domain.maxU);
    low[1] = int32_t(domain.minV);
    high[1] = int32_t(domain.maxV);
  }
  std::optional<gpu::replay::OriginalTexture> dynamicPalette, recovered;
  const CachedTexture *recoveredSource = nullptr;
  bool recoveryAmbiguous = false;
  for (const auto &tex : it->second) {
    const int32_t origin[] = {int32_t(tex.px * ppw) - pageX,
                              int32_t(tex.py) - pageY};
    bool covers = true;
    for (uint32_t axis = 0; axis < 2 && covers; ++axis) {
      for (int32_t q = low[axis]; q <= high[axis]; ++q) {
        const auto &window = packet.state.textureWindow;
        const uint32_t mask = axis ? window.maskY : window.maskX;
        const uint32_t offset = axis ? window.offsetY : window.offsetX;
        const int32_t u = ((uint32_t(q) & 255u) & ~mask) | (offset & mask);
        if (u < origin[axis] ||
            uint32_t(u - origin[axis]) >= (axis ? tex.height : tex.width)) {
          covers = false;
          break;
        }
      }
    }
    if (!covers)
      continue;
    const bool samePalette =
        !std::memcmp(vram.data() + tex.cy * 1024 + tex.cx, tex.palette.data(),
                     tex.palette.size() * sizeof(gpu::Color16));
    if (!samePalette && (depth != 0 || tex.palette.size() != 16))
      continue;
    bool matches = true, borrowed = false;
    const auto codecScratch = [](uint32_t x, uint32_t y) {
      return (x >= 768 && x < 1024 && y >= 256 && y < 260) ||
             (x >= 896 && x < 960 && y >= 336 && y < 432);
    };
    uint32_t intact = 0;
    const bool logicalMaterial =
        packet.originalMesh &&
        std::find(tex.names.begin(), tex.names.end(),
                  packet.originalMesh->textureId) != tex.names.end();
    for (uint32_t y = 0; y < tex.height && matches; ++y) {
      const auto *live = vram.data() + (tex.py + y) * 1024 + tex.px;
      const auto *original = tex.pixels.data() + y * tex.rowWords;
      if (!std::memcmp(live, original, tex.rowWords * sizeof(gpu::Color16))) {
        for (uint32_t x = 0; x < tex.rowWords; ++x)
          intact += !codecScratch(tex.px + x, tex.py + y);
        continue;
      }
      for (uint32_t x = 0; x < tex.rowWords; ++x) {
        if (live[x] == original[x]) {
          intact += !codecScratch(tex.px + x, tex.py + y);
          continue;
        }
        const uint32_t vx = tex.px + x, vy = tex.py + y;
        // radiotex.c temporarily borrows these exact rectangles. Old native
        // states can retain corruption from the former StoreImage stub. Only
        // a named KMD material plus an otherwise exact PCX may bypass it.
        const bool codec = codecScratch(vx, vy);
        if (!logicalMaterial || !codec) {
          matches = false;
          break;
        }
        borrowed = true;
      }
    }
    if (!matches || (borrowed && !intact))
      continue;
    gpu::replay::OriginalTexture binding;
    binding.data = {cache->section(AssetCache::Indices).offset +
                        cache->word(AssetCache::Textures, tex.index, 2),
                    cache->section(AssetCache::Palettes).offset +
                        4 * cache->word(AssetCache::Textures, tex.index, 4),
                    tex.width, tex.height};
    binding.origin = {origin[0], origin[1], 1, 0};
    if (!samePalette) {
      binding.origin[2] = 2;
      for (uint32_t c = 0; c < 16; ++c)
        binding.palette[c / 2] |= uint32_t(vram[tex.cy * 1024 + tex.cx + c].raw)
                                  << ((c & 1) * 16);
    }
    if (borrowed) {
      if (recoveredSource && (recoveredSource->pixels != tex.pixels ||
                              recoveredSource->palette != tex.palette ||
                              recovered->origin != binding.origin ||
                              recovered->data[2] != tex.width))
        recoveryAmbiguous = true;
      if (!recoveredSource) {
        recoveredSource = &tex;
        recovered = binding;
      }
      continue;
    }
    if (!samePalette) {
      if (!dynamicPalette)
        dynamicPalette = binding;
      continue;
    }
    packet.originalTexture = binding;
    metrics::count("mgs.texture.cached_packets");
    return;
  }
  if (dynamicPalette) {
    packet.originalTexture = *dynamicPalette;
    metrics::count("mgs.texture.cached_packets");
    metrics::count("mgs.texture.dynamic_palette_packets");
    return;
  }
  if (recovered && !recoveryAmbiguous) {
    packet.originalTexture = *recovered;
    metrics::count("mgs.texture.cached_packets");
    metrics::count("mgs.texture.codec_borrow_recovered_packets");
    return;
  }
  metrics::trace("mgs.texture.miss", {{"opcode", packet.opcode},
                                      {"page", page},
                                      {"clut", packet.state.clut.raw},
                                      {"u0", low[0]},
                                      {"u1", high[0]},
                                      {"v0", low[1]},
                                      {"v1", high[1]},
                                      {"reason", 2}});
}
bool replaceMeshReplayPacket(gpu::replay::DrawPacket &packet) {
  if (!enabled || !active() || packet.vertexCount != 4 ||
      ((packet.opcode & 0xfcu) != 0x3cu && (packet.opcode & 0xfcu) != 0x2cu))
    return false;
  metrics::count("mgs.mesh.seen_packets");
  const auto token = packet.vertices[0].provenance.dmaNodeSource;
  if (!token)
    return false;
  const uint32_t address = uint32_t(token - 1) & 0x1fffffffu;
  const Face *face = nullptr;
  for (const auto &buffer : faces) {
    const auto it = buffer.find(address);
    if (it == buffer.end())
      continue;
    if (face)
      return false;
    face = &it->second;
  }
  if (!face) {
    metrics::trace("mgs.mesh.unmapped", {{"packet", address},
                                         {"opcode", packet.opcode},
                                         {"x", packet.vertices[0].nativeX},
                                         {"y", packet.vertices[0].nativeY}});
    return false;
  }
  metrics::count("mgs.mesh.mapped_packets");
  if (packet.opcode != face->opcode)
    return false;
  if ((packet.state.texturePage.raw & 0x1ffu) != (face->tpage & 0x1ffu) ||
      packet.state.clut.raw != face->clut)
    return false;
  double maxError = 0;
  std::array<Point, 4> projected{};
  for (unsigned k = 0; k < 4; ++k) {
    const auto &v = packet.vertices[k];
    const auto &provenance = v.provenance;
    if (provenance.dmaNodeSource != token ||
        !provenance.has(gpu::replay::ReplayVertexDmaTraversalExact |
                        gpu::replay::ReplayVertexGp0WordSourceExact) ||
        ((provenance.gp0WordSource - 1) & 0x1fffffffu) !=
            address + 8 + k * face->step ||
        v.nativeX - packet.state.drawOffset.x != face->xy[k][0] ||
        v.nativeY - packet.state.drawOffset.y != face->xy[k][1] ||
        v.u !=
            double(face->uv[k] & 255u) + packet.state.textureUvDomain.offsetU ||
        v.v !=
            double(face->uv[k] >> 8) + packet.state.textureUvDomain.offsetV ||
        (uint32_t(v.color.r) | (uint32_t(v.color.g) << 8) |
         (uint32_t(v.color.b) << 16)) != face->colors[k])
      return false;
    const auto &p = face->view[k];
    // CPU packet metadata uses a conservative near-plane projection. The
    // shader fetches the original signed eye coordinates and clips in
    // homogeneous space at Z=1; it never renders these clamped metadata
    // coordinates.
    const double metadataW = std::max(1.0, p[2]);
    projected[k] = {
        p[0] * face->projection / metadataW + packet.state.drawOffset.x,
        p[1] * face->projection / metadataW + packet.state.drawOffset.y,
        metadataW};
    maxError = std::max({maxError,
                         std::abs(face->reference[k][0] +
                                  packet.state.drawOffset.x - v.nativeX),
                         std::abs(face->reference[k][1] +
                                  packet.state.drawOffset.y - v.nativeY)});
  }
  metrics::count("mgs.mesh.candidate_packets");
  // This is a diagnostic correctness gate, not an identity heuristic: the
  // complete source/pose/packet association above is required independently.
  if (!std::isfinite(maxError) || maxError > 4.0) {
    metrics::count("mgs.mesh.pose_rejected");
    for (uint32_t k = 0; k < 4; ++k)
      metrics::trace(
          "mgs.mesh.reject",
          {{"packet", address},
           {"model", face->assetModel},
           {"procedural", face->procedural},
           {"reason", 5},
           {"corner", k},
           {"x_milli", int64_t(projected[k][0] * 1000)},
           {"y_milli", int64_t(projected[k][1] * 1000)},
           {"nx", packet.vertices[k].nativeX},
           {"ny", packet.vertices[k].nativeY},
           {"reference_x_milli",
            int64_t((face->reference[k][0] + packet.state.drawOffset.x) *
                    1000)},
           {"reference_y_milli",
            int64_t((face->reference[k][1] + packet.state.drawOffset.y) *
                    1000)},
           {"offset_x", packet.state.drawOffset.x},
           {"offset_y", packet.state.drawOffset.y},
           {"z", int64_t(face->view[k][2])}});
    return false;
  }
  applySourceGeometry(packet, *face);
  metrics::count("mgs.mesh.replaced_packets");
  if (face->precisePose)
    metrics::count("mgs.mesh.precise_pose_packets");
  if (face->procedural)
    metrics::count("mgs.primitive.replaced_packets");
  metrics::trace("mgs.mesh.replace",
                 {{"packet", address},
                  {"model", face->assetModel},
                  {"capture", int64_t(face->sequence)},
                  {"error_milli", int64_t(maxError * 1000)}});
  return true;
}
} // namespace ps1::games::mgs
void mgsPrepareMeshReplay(recomp_context *ctx) {
  ps1::games::mgs::prepareMeshReplay(ctx);
}

void mgsBeginMeshDivide(recomp_context *ctx) {
  ps1::games::mgs::beginMeshDivide(ctx);
}
void mgsMeshDivideGrid(recomp_context *ctx) {
  ps1::games::mgs::meshDivideGrid(ctx);
}
void mgsMeshDivideCopy(recomp_context *ctx) {
  ps1::games::mgs::meshDivideCopy(ctx);
}
void mgsMeshDividePacket(recomp_context *ctx) {
  ps1::games::mgs::meshDividePacket(ctx);
}
