#include "runtime/games/mgs_mesh_replay.h"
#include "runtime/cpu_context.h"
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
  for (uint32_t a = 0; a < cache->section(AssetCache::Assets).count; ++a) {
    if (cache->word(AssetCache::Assets, a, 8) != 'k')
      continue;
    const auto source =
        bytes.subspan(cache->section(AssetCache::Source).offset +
                          cache->word(AssetCache::Assets, a, 12),
                      cache->word(AssetCache::Assets, a, 13));
    if (source.size() < 32)
      continue;
    uint64_t header = 0, base = 0;
    if (u32(source.data()) == 0x61444d4b) {
      base = 16ull + u32(source.data() + 8);
      uint64_t cursor = 16;
      for (uint32_t k = 0; k < u32(source.data() + 4); ++k) {
        if (cursor + 36 > source.size())
          break;
        if (u32(source.data() + cursor) ==
            cache->word(AssetCache::Assets, a, 15)) {
          header = cursor + 4;
          break;
        }
        cursor += 36ull + uint64_t(u32(source.data() + cursor + 8)) * 88;
      }
      if (!header)
        continue;
    }
    const uint32_t first = cache->word(AssetCache::Assets, a, 9),
                   total = cache->word(AssetCache::Assets, a, 10);
    if (header + 32ull + uint64_t(total) * 88 > source.size())
      continue;
    for (uint32_t m = 0; m < total; ++m) {
      const auto *def = source.data() + header + 32 + m * 88;
      const uint32_t count = u32(def + 52);
      const uint64_t offset = base + u32(def + 56);
      if (offset + uint64_t(count) * 8 > source.size())
        continue;
      const uint64_t indices = base + u32(def + 60);
      const uint64_t corners = uint64_t(u32(def + 4)) * 4;
      if (indices + corners > source.size())
        continue;
      sourceCorners[first + m].assign(source.begin() + indices,
                                      source.begin() + indices + corners);
      auto &out = sourceVertices[first + m];
      out.reserve(count);
      for (uint32_t v = 0; v < count; ++v) {
        const auto *p = source.data() + offset + v * 8;
        out.push_back({{double(int16_t(u16(p))), double(int16_t(u16(p + 2))),
                        double(int16_t(u16(p + 4)))},
                       int16_t(u16(p + 6)),
                       uint32_t(p - bytes.data()) | 0x80000000u});
      }
    }
  }
}
// The oracle retains GTE divide/IR/SXY saturation; the cache projection does
// not. Billboard rectangles perform two projections in DG_MakePrimsRectangle.
std::array<double, 2> referenceProjection(const Point &p, double h) {
  const double ratio = std::min(131071.0 / 65536.0, h / std::max(1.0, p[2]));
  return {
      std::clamp(std::clamp(p[0], -32768.0, 32767.0) * ratio, -1024.0, 1023.0),
      std::clamp(std::clamp(p[1], -32768.0, 32767.0) * ratio, -1024.0, 1023.0)};
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
}
void setMeshReplayCache(const AssetCache *value) {
  cache = value;
  const char *mode = std::getenv("PS1_MGS_MESH_REPLAY");
  enabled = cache && mode && std::string_view(mode) == "1";
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
      if (!packet)
        continue;
      for (uint32_t m = root; m != ~0u;) {
        const uint32_t global = first + m;
        const uint32_t corners = cache->word(AssetCache::Models, global, 4);
        const uint32_t begin = cache->word(AssetCache::Models, global, 3);
        const uint32_t obj = instance.models[m].guestAddress;
        if (!range(obj, 92) || u16(ptr(obj + 82)) != corners / 4 ||
            !range(packet, uint64_t(corners / 4) * 52))
          break;
        for (uint32_t f = 0; f < corners / 4; ++f, packet += 52) {
          const auto *p = ptr(packet);
          if ((p[7] & 0xfcu) != 0x3cu)
            continue;
          Face face;
          face.sequence = scene->sequence;
          face.assetModel = global;
          face.precisePose = instance.pose.source == ScenePoseSource::Rotations ||
                             instance.pose.source == ScenePoseSource::Movements ||
                             instance.pose.inheritedRoot;
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
          bool complete = true;
          for (unsigned k = 0; k < 4; ++k) {
            const uint32_t corner = begin + f * 4 + order[k];
            Point position{};
            for (unsigned axis = 0; axis < 3; ++axis)
              position[axis] = std::bit_cast<float>(
                  cache->word(AssetCache::Vertices, corner, axis));
            int32_t seam = int32_t(std::bit_cast<float>(
                cache->word(AssetCache::Vertices, corner, 3)));
            uint32_t model = m;
            if (f * 4 + order[k] >= sourceCorners[global].size()) {
              complete = false;
              break;
            }
            uint32_t indexedVertex = sourceCorners[global][f * 4 + order[k]];
            uint32_t pointByte =
                cache->section(AssetCache::Vertices).offset + corner * 48;
            for (uint32_t step = 0; seam >= 0 && step <= total; ++step) {
              const uint32_t parent =
                  cache->word(AssetCache::Models, first + model, 1);
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
              if (indexedVertex >= count ||
                  !range(liveVertices, uint64_t(count) * 8)) {
                complete = false;
                break;
              }
              for (unsigned axis = 0; axis < 3; ++axis) {
                const double live = int16_t(
                    u16(ptr(liveVertices + indexedVertex * 8 + axis * 2)));
                displacement[axis] = live - position[axis];
              }
            }
            face.geometry.pointBytes[k] = pointByte;
            const auto &world = instance.models[owners[model]].presentationWorld;
            for (unsigned row = 0; row < 3; ++row) {
              auto &out = face.geometry.viewRows[k * 3 + row];
              for (unsigned col = 0; col < 4; ++col) {
                double value = col == 3 ? scene->view[12 + row] : 0;
                for (unsigned axis = 0; axis < 3; ++axis)
                  value += double(scene->view[axis * 4 + row]) *
                           world[col * 4 + axis];
                // USA DG_AdjustOverscan scales the entire Y row by 58/64.
                // Preserve its framing in float, alongside the 16:9 X scale.
                const double scale =
                    row == 1 ? 58.0 / 64.0
                             : (row == 0 && scene->widescreen ? 0.75 : 1.0);
                out[col] = float(value * scale);
              }
            }
            // Dynamic deformation is a per-corner displacement of the cached
            // original, transformed as a vector. No reference screen positions
            // are uploaded as replacement geometry.
            for (unsigned row = 0; row < 3; ++row)
              for (unsigned axis = 0; axis < 3; ++axis)
                face.geometry.viewRows[k * 3 + row][3] +=
                    face.geometry.viewRows[k * 3 + row][axis] *
                    displacement[axis];
            for (unsigned axis = 0; axis < 3; ++axis)
              position[axis] += displacement[axis];
            face.view[k] = transform(scene->view, transform(world, position));
            face.nativeView[k] = transform(
                scene->view, transform(instance.models[owners[model]].world, position));
            if (scene->widescreen) {
              face.view[k][0] *= 0.75;
              face.nativeView[k][0] *= 0.75;
            }
            face.view[k][1] *= 58.0 / 64.0;
            face.nativeView[k][1] *= 58.0 / 64.0;
            face.reference[k] =
                referenceProjection(face.nativeView[k], face.projection);
            face.xy[k] = {int16_t(u16(p + 8 + k * 12)),
                          int16_t(u16(p + 10 + k * 12))};
            face.colors[k] = u32(p + 4 + k * 12) & 0xffffffu;
            face.uv[k] = u16(p + 12 + k * 12);
          }
          if (complete) {
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
      metrics::trace("mgs.mesh.reject",
                     {{"packet", address},
                      {"model", face->assetModel},
                      {"procedural", face->procedural},
                      {"reason", 5},
                      {"corner", k},
                      {"x_milli", int64_t(projected[k][0] * 1000)},
                      {"y_milli", int64_t(projected[k][1] * 1000)},
                      {"nx", packet.vertices[k].nativeX},
                      {"ny", packet.vertices[k].nativeY},
                      {"offset_x", packet.state.drawOffset.x},
                      {"offset_y", packet.state.drawOffset.y},
                      {"z", int64_t(face->view[k][2])}});
    return false;
  }
  packet.originalMesh = face->geometry;
  packet.originalMesh->projection = {
      float(face->projection), float(packet.state.drawOffset.x),
      float(packet.state.drawOffset.y), face->procedural ? 2.0f : 1.0f};
  for (unsigned k = 0; k < 4; ++k) {
    auto &v = packet.vertices[k];
    v.x = projected[k][0];
    v.y = projected[k][1];
    v.w = projected[k][2];
    v.rawDepth = face->view[k][2];
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
