#include "runtime/games/mgs_scene.h"
#include "runtime/cpu_context.h"
#include "runtime/games/mgs_mesh_replay.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include "runtime/psyq/psyq_state.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace ps1::games::mgs {
namespace {
const AssetCache *assets = nullptr;
std::unordered_map<uint32_t, std::vector<uint32_t>> assetNames;
std::mutex snapshotMutex;
std::shared_ptr<const SceneSnapshot> latest;
uint64_t sequence = 0;
bool precisePose = true;
bool ramRange(uint32_t address, uint64_t size) {
  const uint32_t physical = address & 0x1fffffffu;
  return physical < Memory::RAM_SIZE && size <= Memory::RAM_SIZE - physical;
}
const uint8_t *ram(const Memory &memory, uint32_t address) {
  return memory.ramPtr() + (address & 0x1fffffffu);
}
uint32_t u32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint16_t u16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
std::array<float, 16> matrix(const Memory &m, uint32_t address) {
  std::array<float, 16> result{};
  const uint8_t *p = ram(m, address);
  for (size_t row = 0; row < 3; ++row) {
    for (size_t col = 0; col < 3; ++col)
      result[col * 4 + row] = int16_t(u16(p + (row * 3 + col) * 2)) / 4096.0f;
    result[12 + row] = int32_t(u32(p + 20 + row * 4));
  }
  result[15] = 1;
  return result;
}
struct DeformationProof {
  uint32_t points = 0, axis = 0;
};
DeformationProof deformationProof(const Memory &m, uint32_t definition) {
  // USA GV_ACTOR_USER list. Follow live actors, never scan unowned heap data.
  uint32_t previous = 0x800AF4E4u;
  uint32_t actor = u32(ram(m, previous + 4));
  for (uint32_t n = 0; n < 1024 && actor; ++n) {
    if (!ramRange(actor, 32) || u32(ram(m, actor)) != previous)
      return {};
    const uint32_t name = u32(ram(m, actor + 20));
    if (ramRange(name, 11) &&
        std::memcmp(ram(m, name), "shakemdl.c", 11) == 0 &&
        ramRange(actor, 128) && u32(ram(m, actor + 56)) == definition) {
      const uint32_t axis = u32(ram(m, actor + 44));
      const uint32_t points = u32(ram(m, actor + 124));
      if (axis <= 2 && points)
        return {points, axis};
      return {};
    }
    previous = actor;
    actor = u32(ram(m, actor + 4));
  }
  return {};
}
bool match(const AssetCache &cache, uint32_t asset, const Memory &m,
           uint32_t definition, bool geometry = false,
           const DeformationProof *deform = nullptr, bool *changed = nullptr) {
  if (cache.word(AssetCache::Assets, asset, 8) != 'k') return false;
  const uint32_t sourceOffset = cache.word(AssetCache::Assets, asset, 12);
  const uint32_t sourceSize = cache.word(AssetCache::Assets, asset, 13);
  const auto bytes = cache.bytes().subspan(cache.section(AssetCache::Source).offset + sourceOffset, sourceSize);
  uint32_t header = 0, pointerBase = 0;
  if (bytes.size() < 32) return false;
  if (u32(bytes.data()) == 0x61444d4bu) {
    pointerBase = 16 + u32(bytes.data() + 8);
    uint32_t cursor = 16;
    const uint32_t wanted = cache.word(AssetCache::Assets, asset, 15);
    for (uint32_t i = 0; i < u32(bytes.data() + 4); ++i) {
      if (uint64_t(cursor) + 36 > bytes.size()) return false;
      if (u32(bytes.data() + cursor) == wanted) { header = cursor + 4; break; }
      const uint64_t next = uint64_t(cursor) + 36 + uint64_t(u32(bytes.data() + cursor + 8)) * 88;
      if (next > bytes.size()) return false;
      cursor = next;
    }
    if (!header) return false;
  }
  const uint32_t total = cache.word(AssetCache::Assets, asset, 10);
  if (!ramRange(definition, 32ull + uint64_t(total) * 88) ||
      uint64_t(header) + 32 + uint64_t(total) * 88 > bytes.size()) return false;
  if (std::memcmp(ram(m, definition), bytes.data() + header, 32) != 0) return false;
  uint32_t vertexBase = 0;
  for (uint32_t model = 0; model < total; ++model) {
    const auto *src = bytes.data() + header + 32 + model * 88;
    const auto *live = ram(m, definition + 32 + model * 88);
    // All semantic header fields and exact original streams must agree;
    // only the loader's pointer relocation and documented seam rewrite differ.
    if (std::memcmp(src + 4, live + 4, 52) || u32(src + 64) != u32(live + 64))
      return false;
    // Runtime shading can toggle bit 1 without changing topology.
    if (((u32(src) ^ u32(live)) & (geometry ? ~2u : ~0u)) != 0)
      return false;
    const uint32_t faces = u32(src + 4), vertices = u32(src + 52), normals = u32(src + 64);
    const uint32_t fields[] = {56, 60, 68, 72, 76, 80};
    const uint64_t sizes[] = {uint64_t(vertices)*8, uint64_t(faces)*4, uint64_t(normals)*8,
                             uint64_t(faces)*4, uint64_t(faces)*8, uint64_t(faces)*2};
    for (size_t stream = 0; stream < 6; ++stream) {
      const uint32_t relative = u32(src + fields[stream]);
      const uint32_t target = u32(live + fields[stream]);
      if (!relative && stream != 0) { if (target) return false; continue; }
      const uint64_t offset = uint64_t(relative) + pointerBase;
      if (offset + sizes[stream] > bytes.size() || !ramRange(target, sizes[stream])) return false;
      const uint8_t *a = bytes.data() + offset, *b = ram(m, target);
      if (stream == 0) {
        for (uint32_t v = 0; v < vertices; ++v) {
          for (uint32_t axis = 0; axis < 3; ++axis) {
            uint16_t original = u16(b + v * 8 + axis * 2);
            if (deform && axis == deform->axis) {
              const uint64_t offset = uint64_t(vertexBase + v) * 2;
              if (!ramRange(deform->points, offset + 2))
                return false;
              original = u16(ram(m, deform->points) + offset);
            }
            if (u16(a + v * 8 + axis * 2) != original)
              return false;
          }
          if (changed && std::memcmp(a + v * 8, b + v * 8, 6))
            *changed = true;
          const uint16_t seam = u16(a + v*8 + 6);
          if (seam == 0xffff) { if (u16(b + v*8 + 6) != seam) return false; continue; }
          const int32_t parent = int32_t(u32(src + 44));
          if (parent < 0 || uint32_t(parent) >= total) return false;
          const auto *parentModel = bytes.data() + header + 32 + parent * 88;
          const uint64_t indices = uint64_t(u32(parentModel + 60)) + pointerBase;
          const uint32_t count = u32(parentModel + 4) * 4;
          if (indices + count > bytes.size()) return false;
          uint32_t corner = 0;
          while (corner < count && (bytes[indices + corner] & 127u) != seam) ++corner;
          constexpr uint32_t order[] = {0,1,3,2};
          const uint16_t rewritten = uint16_t((corner / 4) * 52 + order[corner % 4] * 12 + 8);
          if (u16(b + v*8 + 6) != rewritten) return false;
        }
      } else if (stream == 1) {
        const uint64_t positions = uint64_t(u32(src + 56)) + pointerBase;
        if (positions + uint64_t(vertices)*8 > bytes.size()) return false;
        for (uint32_t corner = 0; corner < faces*4; ++corner) {
          if (a[corner] >= vertices) return false;
          const uint8_t expected = a[corner] | (u16(bytes.data() + positions + a[corner]*8 + 6) != 0xffff ? 128u : 0u);
          if (b[corner] != expected) return false;
        }
      } else if (!(geometry && stream == 5) && std::memcmp(a, b, sizes[stream]))
        return false;
    }
    vertexBase += vertices;
  }
  return true;
}
}
uint32_t matchOriginalDefinition(const AssetCache &cache, const Memory &memory, uint32_t definition) {
  if (!ramRange(definition, 32)) return ~0u;
  uint32_t found = ~0u;
  for (uint32_t a = 0; a < cache.section(AssetCache::Assets).count; ++a) {
    if (cache.word(AssetCache::Assets, a, 8) != 'k' ||
        cache.word(AssetCache::Assets, a, 10) != u32(ram(memory, definition) + 4)) continue;
    if (match(cache, a, memory, definition)) {
      // Multiple byte-distinct originals can become the same relocated image.
      // Do not choose one by cache order if provenance is ambiguous.
      if (found != ~0u) return ~0u;
      found = a;
    }
  }
  return found;
}
// USA GV cache: 128 (file ID, guest buffer) pairs. The file ID is only a
// candidate filter; the complete topology, normals, UVs and seam links must
// still match. This table is part of guest RAM and therefore survives F9.
uint32_t matchSceneDefinition(const AssetCache &cache, const Memory &memory,
                              uint32_t definition, bool &deformed) {
  deformed = false;
  if (!ramRange(definition, 32))
    return ~0u;
  const auto deformation = deformationProof(memory, definition);
  uint32_t fileId = ~0u;
  for (uint32_t i = 0; i < 128; ++i) {
    const auto *entry = ram(memory, 0x800AF668u + i * 8);
    if ((u32(entry) & 0xff0000u) == 0x0a0000u && u32(entry + 4) == definition) {
      if (fileId != ~0u && fileId != (u32(entry) & 0xffffu))
        return ~0u;
      fileId = u32(entry) & 0xffffu;
    }
  }
  std::vector<uint32_t> named;
  if (fileId != ~0u) {
    if (&cache == assets) {
      const auto it = assetNames.find(fileId);
      if (it != assetNames.end())
        named = it->second;
    } else {
      for (uint32_t i = 0; i < cache.section(AssetCache::Names).count; ++i)
        if (cache.word(AssetCache::Names, i, 0) == fileId)
          named.push_back(cache.word(AssetCache::Names, i, 1));
    }
  }
  std::vector<uint32_t> candidates = named;
  if (candidates.empty()) {
    for (uint32_t a = 0; a < cache.section(AssetCache::Assets).count; ++a)
      if (cache.word(AssetCache::Assets, a, 8) == 'k')
        candidates.push_back(a);
  }
  uint32_t found = ~0u;
  for (uint32_t a : candidates) {
    if (cache.word(AssetCache::Assets, a, 8) != 'k' ||
        cache.word(AssetCache::Assets, a, 10) !=
            u32(ram(memory, definition) + 4))
      continue;
    const bool provenance =
        std::find(named.begin(), named.end(), a) != named.end();
    if (!named.empty() && !provenance)
      continue;
    bool changed = false;
    if (!match(cache, a, memory, definition, true,
               provenance && deformation.points ? &deformation : nullptr,
               &changed))
      continue;
    if (found != ~0u) {
      // Aliases are interchangeable only when all rendered source geometry is
      // identical. File padding/unused streams do not create another mesh.
      const auto first = cache.word(AssetCache::Assets, found, 9);
      const auto other = cache.word(AssetCache::Assets, a, 9);
      const auto total = cache.word(AssetCache::Assets, a, 10);
      for (uint32_t m = 0; m < total; ++m) {
        const uint32_t count = cache.word(AssetCache::Models, first + m, 4);
        if (count != cache.word(AssetCache::Models, other + m, 4))
          return ~0u;
        for (uint32_t v = 0; v < count; ++v) {
          const uint32_t x = cache.word(AssetCache::Models, first + m, 3) + v;
          const uint32_t y = cache.word(AssetCache::Models, other + m, 3) + v;
          for (uint32_t w = 0; w <= 8; ++w)
            if (cache.word(AssetCache::Vertices, x, w) !=
                cache.word(AssetCache::Vertices, y, w))
              return ~0u;
        }
      }
    } else {
      found = a;
      deformed = changed;
    }
  }
  return found;
}
void setSceneAssetCache(const AssetCache *cache) {
  assets = cache;
  const char *poseMode = std::getenv("PS1_MGS_PRECISE_POSE");
  precisePose = !poseMode || std::string_view(poseMode) != "0";
  assetNames.clear();
  if (cache) {
    for (uint32_t i = 0; i < cache->section(AssetCache::Names).count; ++i)
      assetNames[cache->word(AssetCache::Names, i, 0)].push_back(
          cache->word(AssetCache::Names, i, 1));
  }
  setMeshReplayCache(cache);
  resetSceneCapture();
}
void resetSceneCapture() {
  std::lock_guard lock(snapshotMutex);
  latest.reset();
  resetMeshReplay();
}
std::shared_ptr<const SceneSnapshot> latestSceneSnapshot() {
  std::lock_guard lock(snapshotMutex); return latest;
}
void captureSceneBeforeBounds(recomp_context *ctx) {
  if (!assets || !active() || !ctx || !ctx->mem) return;
  const auto &m = *ctx->mem;
  const uint32_t channel = ctx->r4;
  if (!ramRange(channel, 100)) { resetSceneCapture(); return; }
  const auto *c = ram(m, channel);
  const uint32_t count = u16(c + 86), queue = u32(c + 88);
  if (count > 256 || !ramRange(queue, uint64_t(count)*4)) { resetSceneCapture(); return; }
  auto snap = std::make_shared<SceneSnapshot>();
  snap->sequence = ++sequence;
  snap->vblank = psyq::psyq_state().vsyncCounter.load();
  snap->channel = channel; snap->bufferIndex = ctx->r5;
  snap->view = matrix(m, channel + 16);
  snap->projection = int16_t(u16(c + 80));
  snap->widescreen = widescreen16x9();
  snap->currentGroup = u32(ram(m, 0x800AE0E0));
  for (size_t i = 0; i < 4; ++i) snap->clip[i] = int16_t(u16(c + 92 + i*2));
  std::unordered_map<uint32_t, std::pair<uint32_t, bool>> definitions;
  uint32_t matched = 0, visible = 0, modelCount = 0;
  uint32_t preciseInstances = 0, preciseModels = 0, invalidPoses = 0, inheritedRoots = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t address = u32(ram(m, queue) + i*4);
    if (!ramRange(address, 72)) { resetSceneCapture(); return; }
    const auto *o = ram(m, address);
    SceneInstance instance;
    instance.guestAddress = address; instance.definition = u32(o + 36);
    instance.rootAddress = u32(o + 32);
    instance.flags = u32(o + 40); instance.group = u16(o + 44);
    instance.visible = !(instance.flags & 0x80) && (!instance.group || (instance.group & snap->currentGroup));
    const uint32_t n = u16(o + 46);
    instance.primaryModels = n;
    if (n > 4096 || !ramRange(instance.definition, 32)) { resetSceneCapture(); return; }
    const uint32_t total = u32(ram(m, instance.definition) + 4);
    if (total > 4096 || n > total || !ramRange(address + 72, uint64_t(total)*92)) { resetSceneCapture(); return; }
    auto [it, inserted] =
        definitions.emplace(instance.definition, std::pair{~0u, false});
    if (inserted)
      it->second.first = matchSceneDefinition(*assets, m, instance.definition,
                                              it->second.second);
    instance.asset = it->second.first;
    instance.deformed = it->second.second;
    if (instance.asset != ~0u) ++matched;
    if (instance.visible) ++visible;
    for (uint32_t j = 0; j < total; ++j) {
      const uint32_t object = address + 72 + j*92;
      SceneModel model;
      model.guestAddress = object;
      model.assetModel = instance.asset == ~0u ? ~0u : assets->word(AssetCache::Assets, instance.asset, 9) + j;
      model.world = matrix(m, object);
      model.presentationWorld = model.world;
      const uint32_t descriptor = u32(ram(m, object + 64));
      if (!ramRange(descriptor, 88)) { resetSceneCapture(); return; }
      model.flags = u32(ram(m, descriptor));
      model.originalPackets = {u32(ram(m, object + 84)), u32(ram(m, object + 88))};
      instance.models.push_back(model); ++modelCount;
    }
    snap->instances.push_back(std::move(instance));
  }
  if (precisePose) {
    reconstructScenePoses(m, *snap);
    for (const auto &instance : snap->instances) {
      const auto &pose = instance.pose;
      if (pose.source == ScenePoseSource::Rotations ||
          pose.source == ScenePoseSource::Movements) {
        ++preciseInstances;
        preciseModels += pose.models;
        metrics::count("mgs.pose.articulated_instances");
        metrics::count("mgs.pose.articulated_models", pose.models);
      }
      if (pose.invalidInput) {
        ++invalidPoses;
        metrics::count("mgs.pose.invalid_inputs");
      }
      if (pose.inheritedRoot) {
        ++inheritedRoots;
        metrics::count("mgs.pose.inherited_roots");
      }
      metrics::trace("mgs.pose", {{"object", instance.guestAddress},
                                  {"source", int64_t(pose.source)},
                                  {"models", pose.models},
                                  {"invalid", pose.invalidInput},
                                  {"inherited_root", pose.inheritedRoot},
                                  {"rotation_error_millionths", int64_t(pose.maxRotationError * 1000000)},
                                  {"translation_error_milli", int64_t(pose.maxTranslationError * 1000)}});
    }
  }
  metrics::count("mgs.scene.captures");
  metrics::setState("mgs.scene.instances", count);
  metrics::setState("mgs.scene.visible_instances", visible);
  metrics::setState("mgs.scene.matched_instances", matched);
  metrics::setState("mgs.scene.models", modelCount);
  metrics::setState("mgs.pose.articulated_instances", preciseInstances);
  metrics::setState("mgs.pose.articulated_models", preciseModels);
  metrics::setState("mgs.pose.invalid_inputs", invalidPoses);
  metrics::setState("mgs.pose.inherited_roots", inheritedRoots);
  metrics::trace("mgs.scene", {{"channel", channel}, {"instances", count},
                               {"visible", visible}, {"matched", matched}, {"models", modelCount}});
  std::lock_guard lock(snapshotMutex); latest = std::move(snap);
}
}
void mgsCaptureSceneBeforeBounds(recomp_context *ctx) { ps1::games::mgs::captureSceneBeforeBounds(ctx); }
