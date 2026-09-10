#include "runtime/games/crash3_scenery_detail.h"
#include "runtime/memory.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <set>

namespace ps1::games::crash3 {
namespace {
std::mutex mutex;
struct Capture {
  std::shared_ptr<const SceneryDetailSnapshot> detail;
  std::weak_ptr<const ResidentPresentationSnapshot> owner;
};
std::deque<Capture> captures;

bool ramRange(uint32_t address, uint32_t size) {
  return (address & 0x1fffffffu) < 0x200000u &&
         size <= 0x200000u - (address & 0x1fffffffu) && address != 0u;
}

struct FirstProperty {
  uint32_t data = 0;
  uint32_t count = 0;
  uint8_t type = 0;
  uint8_t stride = 0;
};
// The callers request index zero in mode 2; no time interpolation is involved.
std::optional<FirstProperty> firstOwnProperty(const Memory &memory,
                                              uint32_t section, uint16_t id) {
  if (!ramRange(section, 16u))
    return std::nullopt;
  const int16_t fields = memory.read16(section + 12u);
  if (fields < 0 || fields > 1024 ||
      !ramRange(section, 16u + static_cast<uint32_t>(fields) * 8u))
    return std::nullopt;
  int32_t left = 0, right = fields - 1;
  while (left <= right) {
    const int32_t mid = (left + right) / 2;
    const uint32_t field = section + 16u + static_cast<uint32_t>(mid) * 8u;
    const uint16_t key = memory.read16(field);
    if (key != id) {
      if (key < id)
        left = mid + 1;
      else
        right = mid - 1;
      continue;
    }
    const uint32_t start = section + 12u + memory.read16(field + 2u);
    const uint8_t flags = memory.read8(field + 4u);
    const uint8_t stride = memory.read8(field + 5u);
    const uint32_t rows = memory.read16(field + 6u);
    if (rows == 0u || stride == 0u || !ramRange(start, 2u))
      return std::nullopt;
    uint32_t data = start + ((flags & 0x40u) ? rows * 2u : 2u);
    if (flags & 0x20u)
      data += rows * 2u;
    data = (data + 3u) & ~3u;
    const uint32_t count = memory.read16(start);
    if (count == 0u || !ramRange(data, count * stride))
      return std::nullopt;
    return FirstProperty{data, count, static_cast<uint8_t>(flags & 31u),
                         stride};
  }
  return FirstProperty{}; // Valid table, requested property is absent.
}

bool strictlySharper(const ResidentPresentationMaterial &high,
                     const ResidentPresentationMaterial &low) {
  if (!high.decoded || !low.decoded || !high.textured || !low.textured ||
      high.dynamicTexture || low.dynamicTexture || high.sourceToken == 0u ||
      low.sourceToken == 0u || high.sourceToken >= 0x800u ||
      low.sourceToken >= 0x800u ||
      high.semiTransparent != low.semiTransparent ||
      (high.texturePage & 0x1e0u) != (low.texturePage & 0x1e0u) ||
      high.packetVertexCount != low.packetVertexCount ||
      (high.packetVertexCount != 3u && high.packetVertexCount != 4u))
    return false;
  bool strictlyGreater = false;
  for (uint32_t axis = 0u; axis < 2u; ++axis) {
    float highMin = 255.0f, lowMin = 255.0f, highMax = 0.0f, lowMax = 0.0f;
    for (uint32_t corner = 0u; corner < high.packetVertexCount; ++corner) {
      const float h = high.packetUv[corner][axis];
      const float l = low.packetUv[corner][axis];
      if (!std::isfinite(h) || !std::isfinite(l) || h < 0.0f || l < 0.0f ||
          h > 255.0f || l > 255.0f || std::trunc(h) != h || std::trunc(l) != l)
        return false;
      highMin = std::min(highMin, h);
      lowMin = std::min(lowMin, l);
      highMax = std::max(highMax, h);
      lowMax = std::max(lowMax, l);
    }
    const float highSpan = highMax - highMin, lowSpan = lowMax - lowMin;
    if (lowSpan <= 0.0f || highSpan < lowSpan)
      return false;
    strictlyGreater |= highSpan > lowSpan;
    for (uint32_t corner = 0u; corner < high.packetVertexCount; ++corner) {
      // Exact small-integer products, not a floating screen-space tolerance.
      if ((high.packetUv[corner][axis] - highMin) * lowSpan !=
          (low.packetUv[corner][axis] - lowMin) * highSpan)
        return false;
    }
  }
  return strictlyGreater;
}
} // namespace

std::optional<std::vector<ScenerySourcePath>>
readSceneryZonePaths(const Memory &memory, uint32_t zoneEntry) {
  if (!ramRange(zoneEntry, 20u) || memory.read32(zoneEntry) != 0x0100ffffu ||
      memory.read32(zoneEntry + 8u) != 7u)
    return std::nullopt;
  const uint32_t items = memory.read32(zoneEntry + 12u);
  if (items < 3u || items > 1024u || !ramRange(zoneEntry, 16u + items * 4u))
    return std::nullopt;
  std::vector<ScenerySourcePath> result;
  for (uint32_t item = 2u; item < items; ++item) {
    const uint32_t section = memory.read32(zoneEntry + 16u + item * 4u);
    if (!ramRange(section, 16u))
      return std::nullopt;
    // Root section programs own their path; inherited child records do not
    // introduce another path. No assumption about a fixed items-per-section.
    if (memory.read32(section + 4u) != zoneEntry ||
        memory.read32(section + 8u) != 0u)
      continue;
    const auto reference = firstOwnProperty(memory, section, 259u);
    if (!reference.has_value())
      return std::nullopt; // Malformed is not the same as an absent path.
    if (reference->data == 0u)
      continue;
    const auto length = firstOwnProperty(memory, section, 639u);
    if (!length.has_value() || length->data == 0u || reference->type != 4u ||
        reference->stride != 4u || length->count > 4096u)
      return std::nullopt;
    uint32_t pair = memory.read32(reference->data);
    if (pair & 1u) {
      const uint32_t eid = pair;
      const uint32_t table = memory.read32(0x80061198u);
      if (!ramRange(table, 1024u))
        return std::nullopt;
      pair = memory.read32(table + ((eid >> 13u) & 0x3fcu));
      uint32_t probes = 0u;
      for (; probes < 8192u && ramRange(pair, 8u); ++probes, pair += 8u)
        if (memory.read32(pair + 4u) == eid)
          break;
      if (probes == 8192u || !ramRange(pair, 8u) ||
          memory.read32(pair + 4u) != eid)
        return std::nullopt;
    }
    if (!ramRange(pair, 8u))
      return std::nullopt;
    const uint32_t entry = memory.read32(pair);
    const uint32_t eid = memory.read32(pair + 4u);
    if ((entry & 1u) || !ramRange(entry, 20u) || !(eid & 1u) ||
        memory.read32(entry) != 0x0100ffffu ||
        memory.read32(entry + 4u) != eid || memory.read32(entry + 8u) != 4u ||
        memory.read32(entry + 12u) != length->count + 1u)
      return std::nullopt; // Never page data in behind the game's back.
    result.push_back({section, entry, length->count, eid});
  }
  return result.empty() ? std::nullopt : std::optional{std::move(result)};
}

std::vector<SceneryBoundSourcePath> readRegisteredSceneryPaths(
    const Memory &memory, uint32_t currentZone,
    std::span<const ResidentPresentationResource> resources) {
  std::vector<SceneryBoundSourcePath> result;
  if (!sceneryVariantBindingsReady(resources))
    return result;
  const uint32_t registry = memory.read32(0x80061198u);
  if (!ramRange(registry, 1024u))
    return result;
  std::set<uint32_t> visited;
  uint32_t probes = 0;
  for (uint32_t bucket = 0; bucket < 1024; bucket += 4) {
    uint32_t pair = memory.read32(registry + bucket);
    for (; ramRange(pair, 8u) && probes < 8192; pair += 8u) {
      ++probes;
      const uint32_t eid = memory.read32(pair + 4u);
      // The resolver walks contiguous pairs. Stop at the next hash bucket,
      // not a guessed sentinel or a scan past the registry's last valid pair.
      if (!(eid & 1u) || ((eid >> 13u) & 0x3fcu) != bucket)
        break;
      const uint32_t entry = memory.read32(pair);
      if (entry == currentZone || (entry & 1u) || !ramRange(entry, 20u) ||
          memory.read32(entry) != 0x0100ffffu ||
          memory.read32(entry + 4u) != eid || memory.read32(entry + 8u) != 7u ||
          !visited.insert(entry).second)
        continue;
      const uint32_t table = memory.read32(entry + 16u);
      if (!ramRange(table, 4u))
        continue;
      SceneryBoundSourcePath bound;
      bound.worldCount = memory.read32(table);
      bound.worlds.fill(-1);
      if (bound.worldCount == 0 || bound.worldCount > 8 ||
          !ramRange(table, 4u + 48u * bound.worldCount))
        continue;
      bool valid = true, relevant = false;
      std::set<uint32_t> identities;
      for (uint32_t world = 0; world < bound.worldCount; ++world) {
        const uint32_t reference = memory.read32(table + 4u + 48u * world);
        if (!(reference & 1u) && !ramRange(reference, 8u)) {
          valid = false;
          break;
        }
        const uint32_t id =
            reference & 1u ? reference : memory.read32(reference + 4u);
        if (!(id & 1u) || !identities.insert(id).second) {
          valid = false;
          break;
        }
        for (uint32_t index = 0; index < resources.size(); ++index)
          if (resources[index].resourceId == id) {
            bound.worlds[world] = static_cast<int8_t>(index);
            relevant = true;
          }
      }
      if (!valid || !relevant)
        continue;
      if (const auto paths = readSceneryZonePaths(memory, entry))
        for (const auto &path : *paths) {
          bound.source = path;
          result.push_back(bound);
        }
    }
  }
  return result;
}

std::vector<SceneryVariantPair> findSceneryVariantPairs(
    std::span<const ResidentGpuTopologyFace> topology,
    std::span<const ResidentPresentationMaterial> materials) {
  // Index equality preserves not only exact geometry/winding, but also the
  // lighting indices and deformation flags in the native packed vertices.
  using Key = std::array<uint32_t, 8>;
  std::map<Key, std::vector<uint32_t>> groups;
  for (uint32_t ordinal = 0u; ordinal < topology.size(); ++ordinal) {
    const auto &face = topology[ordinal];
    if (face.resourceFaceOrdinal != ordinal || ordinal >= materials.size())
      return {};
    Key key{face.half,
            face.sourceVertexIndex[0],
            face.sourceVertexIndex[1],
            face.sourceVertexIndex[2],
            face.completeSourceVertexIndex[0],
            face.completeSourceVertexIndex[1],
            face.completeSourceVertexIndex[2],
            face.completeSourceVertexIndex[3]};
    if (face.sourceVertexIndex[0] == face.sourceVertexIndex[1] ||
        face.sourceVertexIndex[0] == face.sourceVertexIndex[2] ||
        face.sourceVertexIndex[1] == face.sourceVertexIndex[2])
      continue;
    groups[key].push_back(ordinal);
  }
  std::vector<SceneryVariantPair> result;
  for (const auto &[key, members] : groups) {
    if (members.size() != 2u)
      continue; // Multi-layer/family semantics not established by this policy.
    const auto a = members[0], b = members[1];
    if (strictlySharper(materials[a], materials[b]))
      result.push_back({b, a});
    else if (strictlySharper(materials[b], materials[a]))
      result.push_back({a, b});
  }
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return a.lowerFace < b.lowerFace;
  });
  return result;
}

bool observeSceneryVariantMembership(
    std::vector<SceneryVariantPair> &pairs, uint32_t world,
    uint32_t triangleCount, uint32_t quadCount,
    std::span<const std::vector<uint16_t>> states) {
  if (world >= 8u || triangleCount > 2048u || quadCount > 2048u ||
      states.empty() || states.size() > 4096u)
    return false;
  const uint32_t faceCount = triangleCount + quadCount * 2u;
  const uint32_t words = (static_cast<uint32_t>(states.size()) + 63u) / 64u;
  for (const auto &pair : pairs)
    if (pair.lowerFace >= faceCount || pair.higherFace >= faceCount ||
        pair.lowerFace == pair.higherFace)
      return false;
  std::vector<std::vector<uint64_t>> masks(faceCount,
                                           std::vector<uint64_t>(words));
  for (uint32_t state = 0u; state < states.size(); ++state) {
    for (const uint16_t packed : states[state]) {
      if ((packed >> 13u) != world)
        continue;
      const uint32_t polygon = packed & 0x7ffu, half = (packed >> 11u) & 3u;
      if (polygon >= (half == 0u ? triangleCount : quadCount))
        return false;
      const uint64_t bit = UINT64_C(1) << (state & 63u);
      if (half == 0u)
        masks[polygon][state / 64u] |= bit;
      else {
        const uint32_t base = triangleCount + polygon * 2u;
        if (half & 1u)
          masks[base][state / 64u] |= bit;
        if (half & 2u)
          masks[base + 1u][state / 64u] |= bit;
      }
    }
  }
  for (auto &pair : pairs) {
    bool lowSeen = false, highSeen = false, coSelected = false;
    for (uint32_t word = 0u; word < words; ++word) {
      const auto low = masks[pair.lowerFace][word];
      const auto high = masks[pair.higherFace][word];
      lowSeen |= low != 0u;
      highSeen |= high != 0u;
      coSelected |= (low & high) != 0u;
    }
    pair.coSelected |= coSelected;
    pair.exclusivePathWitness |= lowSeen && highSeen && !coSelected;
  }
  return true;
}

bool compileSceneryDrawSelection(SceneryVariantPlan &plan, uint32_t faceCount) {
  if (faceCount > 6144u)
    return false;
  std::vector<source_selection::Rule> rules;
  std::vector<uint8_t> patchOwned(faceCount), singleAliases;
  for (const auto &family : plan.refinements) {
    for (uint32_t face : family.lowerFaces) {
      if (face >= faceCount || patchOwned[face])
        return false;
      patchOwned[face] = 1;
    }
    auto targets = family.higherFaces;
    targets.insert(targets.end(), family.supportFaces.begin(),
                   family.supportFaces.end());
    rules.push_back({family.lowerFaces, std::move(targets)});
    singleAliases.push_back(0);
  }
  for (const auto &pair : plan.replacements) {
    if (pair[0] >= faceCount)
      return false;
    // A proved many-face construction already owns this primitive. The old
    // draw code gave it the same precedence, separately in every GPU lane.
    if (patchOwned[pair[0]])
      continue;
    rules.push_back({{pair[0]}, {pair[1]}});
    singleAliases.push_back(1);
  }
  auto program = source_selection::compile(faceCount, rules, 6144u);
  if (!program)
    return false; // Preserve any previously published immutable plan.
  auto selection = std::make_shared<SceneryDrawSelection>();
  selection->program = std::move(*program);
  for (uint32_t i = 0; i < rules.size(); ++i) {
    ScenerySurfaceRefinement draw;
    draw.lowerFaces = rules[i].sources;
    // Shared supports remain separate when they have not themselves been
    // replaced. Chained obligations are expressed by their terminal sources.
    for (uint32_t target : selection->program.terminals[i]) {
      if (i < plan.refinements.size() &&
          std::find(plan.refinements[i].supportFaces.begin(),
                    plan.refinements[i].supportFaces.end(),
                    target) != plan.refinements[i].supportFaces.end())
        draw.supportFaces.push_back(target);
      else
        draw.higherFaces.push_back(target);
    }
    selection->draws.push_back(std::move(draw));
    selection->singleFaceAliases.push_back(
        singleAliases[i] && selection->program.terminals[i].size() == 1);
  }
  plan.selection = std::move(selection);
  return true;
}

std::optional<uint32_t>
sceneryVariantReplacement(const SceneryVariantPlan &plan, uint32_t resourceId,
                          uint64_t geometryFingerprint, uint32_t faceOrdinal) {
  if (plan.resourceId != resourceId ||
      plan.geometryFingerprint != geometryFingerprint)
    return std::nullopt;
  if (plan.selection != nullptr) {
    const auto &selected = *plan.selection;
    if (faceOrdinal >= selected.program.owner.size())
      return std::nullopt;
    const uint32_t owner = selected.program.owner[faceOrdinal];
    return owner && selected.singleFaceAliases[owner - 1]
               ? std::optional{selected.program.terminals[owner - 1].front()}
               : std::nullopt;
  }
  const auto found = std::lower_bound(
      plan.replacements.begin(), plan.replacements.end(), faceOrdinal,
      [](const auto &pair, uint32_t face) { return pair[0] < face; });
  return found != plan.replacements.end() && (*found)[0] == faceOrdinal
             ? std::optional{(*found)[1]}
             : std::nullopt;
}

const ScenerySurfaceRefinement *
scenerySurfaceRefinement(const SceneryVariantPlan &plan, uint32_t resourceId,
                         uint64_t geometryFingerprint, uint32_t faceOrdinal) {
  if (plan.resourceId != resourceId ||
      plan.geometryFingerprint != geometryFingerprint)
    return nullptr;
  if (plan.selection != nullptr) {
    const auto &selected = *plan.selection;
    if (faceOrdinal >= selected.program.owner.size())
      return nullptr;
    const uint32_t owner = selected.program.owner[faceOrdinal];
    return owner && !selected.singleFaceAliases[owner - 1]
               ? &selected.draws[owner - 1]
               : nullptr;
  }
  auto upper = std::upper_bound(
      plan.refinements.begin(), plan.refinements.end(), faceOrdinal,
      [](uint32_t face, const auto &family) {
        // Producers sort nonempty, disjoint source families. Keep malformed
        // diagnostic plans safe as well; an empty family cannot own a face.
        return family.lowerFaces.empty() || face < family.lowerFaces.front();
      });
  if (upper == plan.refinements.begin())
    return nullptr;
  const auto &family = *--upper;
  return std::find(family.lowerFaces.begin(), family.lowerFaces.end(),
                   faceOrdinal) != family.lowerFaces.end()
             ? &family
             : nullptr;
}

void publishSceneryDetailSnapshot(
    SceneryDetailSnapshot snapshot,
    const std::shared_ptr<const ResidentPresentationSnapshot> &owner) {
  if (owner == nullptr || owner->captureId != snapshot.captureId)
    return;
  auto published =
      std::make_shared<const SceneryDetailSnapshot>(std::move(snapshot));
  std::scoped_lock lock(mutex);
  std::erase_if(captures, [&](const auto &old) {
    return old.owner.expired() || old.detail->captureId == published->captureId;
  });
  // Follow the actual immutable capture's lifetime. A fixed second eight-frame
  // journal could evict detail while a slower GPU still holds its source frame.
  captures.push_back({std::move(published), owner});
}

std::shared_ptr<const SceneryDetailSnapshot>
sceneryDetailSnapshot(uint32_t captureId) {
  std::scoped_lock lock(mutex);
  const auto found =
      std::find_if(captures.begin(), captures.end(), [&](const auto &snapshot) {
        return snapshot.detail->captureId == captureId &&
               !snapshot.owner.expired();
      });
  return found == captures.end() ? nullptr : found->detail;
}

void clearSceneryDetailSnapshots() {
  std::scoped_lock lock(mutex);
  captures.clear();
}

ResidentPresentationMaterial
clipSceneryDetailMaterial(const ResidentPresentationMaterial &material,
                          const SceneryClipWeights &weights) {
  auto result = material;
  for (uint32_t corner = 0u; corner < 3u; ++corner) {
    for (uint32_t component = 0u; component < 2u; ++component) {
      result.uv[corner][component] = 0.0f;
      for (uint32_t source = 0u; source < 3u; ++source)
        result.uv[corner][component] += static_cast<float>(
            weights[corner][source] * material.uv[source][component]);
    }
  }
  return result;
}

void appendSceneryDetailMaterial(SceneryDetailSnapshot &snapshot,
                                 const ResidentPresentationFace &face,
                                 uint32_t presentationFaceIndex,
                                 const ResidentPresentationMaterial &material) {
  if (face.material.resolvedToken == material.resolvedToken)
    return; // The native decision already names the detailed material.
  snapshot.materials.push_back({face.resourceId,
                                face.resourceGeometryFingerprint,
                                face.resourceFaceOrdinal, material,
                                presentationFaceIndex, face.nearPlaneClipped});
}

std::optional<ResidentPresentationMaterial>
applySceneryDetailMaterial(const ResidentPresentationFace &face,
                           const SceneryDetailOverride &detail,
                           uint32_t presentationFaceIndex) {
  if (presentationFaceIndex == UINT32_MAX ||
      presentationFaceIndex != detail.presentationFaceIndex ||
      face.nearPlaneClipped != detail.nearPlaneClipped ||
      face.resourceId != detail.resourceId ||
      face.resourceGeometryFingerprint != detail.geometryFingerprint ||
      face.resourceFaceOrdinal != detail.faceOrdinal ||
      !face.material.decoded || !detail.material.decoded)
    return std::nullopt;
  for (const auto &uv : detail.material.uv)
    for (const float value : uv)
      if (!std::isfinite(value))
        return std::nullopt;
  return detail.material;
}
} // namespace ps1::games::crash3
