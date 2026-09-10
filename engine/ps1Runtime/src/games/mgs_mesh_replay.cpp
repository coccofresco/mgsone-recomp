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
struct Face {
  uint64_t sequence = 0;
  uint32_t assetModel = 0;
  std::array<Point, 4> view;
  std::array<std::array<int16_t, 2>, 4> xy;
  std::array<uint32_t, 4> colors;
  std::array<uint16_t, 4> uv;
  uint16_t tpage = 0, clut = 0;
  uint8_t opcode = 0;
  double projection = 0;
  gpu::replay::OriginalMeshGeometry geometry;
};
std::array<std::unordered_map<uint32_t, Face>, 2> faces;
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
  if (!cache || !enabled)
    return;
  sourceVertices.resize(cache->section(AssetCache::Models).count);
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
} // namespace
void resetMeshReplay() {
  for (auto &buffer : faces)
    buffer.clear();
}
void setMeshReplayCache(const AssetCache *value) {
  cache = value;
  const char *mode = std::getenv("PS1_MGS_MESH_REPLAY");
  enabled = cache && mode && std::string_view(mode) == "1";
  resetMeshReplay();
  indexSourceVertices();
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
          face.opcode = p[7];
          face.projection = scene->projection;
          face.clut = u16(p + 14);
          face.tpage = u16(p + 26);
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
              const auto &v = sourceVertices[parent][seam];
              position = v.p;
              seam = v.seam;
              pointByte = v.pointByte;
            }
            if (!complete || seam >= 0 || owners[model] == ~0u) {
              complete = false;
              break;
            }
            face.geometry.pointBytes[k] = pointByte;
            const auto &world = instance.models[owners[model]].world;
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
            face.view[k] = transform(
                scene->view,
                transform(instance.models[owners[model]].world, position));
            if (scene->widescreen)
              face.view[k][0] *= 0.75;
            face.view[k][1] *= 58.0 / 64.0;
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
bool replaceMeshReplayPacket(gpu::replay::DrawPacket &packet) {
  if (!enabled || !active() || packet.vertexCount != 4 ||
      (packet.opcode & 0xfcu) != 0x3cu)
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
  if (!face)
    return false;
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
            address + 8 + k * 12 ||
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
    // Near-plane/subdivision and saturated packets keep the reference path.
    if (p[2] <= std::max(1.0, face->projection * 0.5))
      return false;
    projected[k] = {p[0] * face->projection / p[2] + packet.state.drawOffset.x,
                    p[1] * face->projection / p[2] + packet.state.drawOffset.y,
                    p[2]};
    maxError = std::max({maxError, std::abs(projected[k][0] - v.nativeX),
                         std::abs(projected[k][1] - v.nativeY)});
  }
  metrics::count("mgs.mesh.candidate_packets");
  // This is a diagnostic correctness gate, not an identity heuristic: the
  // complete source/pose/packet association above is required independently.
  if (!std::isfinite(maxError) || maxError > 4.0) {
    metrics::count("mgs.mesh.pose_rejected");
    return false;
  }
  packet.originalMesh = face->geometry;
  packet.originalMesh->projection = {float(face->projection),
                                     float(packet.state.drawOffset.x),
                                     float(packet.state.drawOffset.y), 1.0f};
  for (unsigned k = 0; k < 4; ++k) {
    auto &v = packet.vertices[k];
    v.x = projected[k][0];
    v.y = projected[k][1];
    v.w = v.rawDepth = projected[k][2];
    v.qx = v.x * v.w;
    v.qy = v.y * v.w;
    v.nearDepth = 0;
    v.preciseUV = packet.textured;
    v.preciseXY = v.preciseX = v.preciseY = v.rawDepthValid = v.homogeneousQ =
        true;
    v.inventedW = v.nearSaturated = v.reconciledX = v.reconciledY = false;
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
