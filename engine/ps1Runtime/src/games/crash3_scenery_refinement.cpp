#include "runtime/games/crash3_scenery_refinement.h"
#include "runtime/source_surface_cover.h"
#include "runtime/source_surface_partition.h"
#include "runtime/source_surface_patch.h"

#include <cmath>
#include <map>
#include <set>

namespace ps1::games::crash3 {
namespace {
struct Polygon {
  surface_partition::Surface shape;
  std::vector<uint32_t> faces;
  uint32_t texels = 0;
  uint16_t format = 0;
};
struct SourcePatch {
  std::array<surface_partition::Point, 4> points{};
  uint32_t count = 0;
  std::vector<uint32_t> faces;
  uint32_t texels = 0;
  uint16_t format = 0;
  std::array<std::optional<surface_partition::Surface>, 3> projections;
};
uint32_t texels(const ResidentPresentationMaterial &material) {
  if (!material.decoded || !material.textured || material.semiTransparent ||
      material.dynamicTexture || material.sourceToken == 0 ||
      material.sourceToken >= 0x800 ||
      (material.packetVertexCount != 3 && material.packetVertexCount != 4))
    return 0;
  uint32_t result = 1;
  for (uint32_t axis = 0; axis < 2; ++axis) {
    float lo = 255, hi = 0;
    for (uint32_t i = 0; i < material.packetVertexCount; ++i) {
      const float value = material.packetUv[i][axis];
      if (!std::isfinite(value) || value < 0 || value > 255 ||
          std::trunc(value) != value)
        return 0;
      lo = std::min(lo, value);
      hi = std::max(hi, value);
    }
    result *= static_cast<uint32_t>(hi - lo + 1);
  }
  return result;
}
} // namespace

bool sceneryVariantBindingsReady(
    std::span<const ResidentPresentationResource> resources) {
  if (resources.empty() || resources.size() > 8)
    return false;
  std::set<uint32_t> identities;
  for (const auto &resource : resources)
    if (!resource.identityValid || !(resource.resourceId & 1u) ||
        resource.geometryFingerprint == 0 || resource.triangleCount > 2048 ||
        resource.quadCount > 2048 ||
        !identities.insert(resource.resourceId).second)
      return false;
  return true;
}

std::optional<ResidentPresentationMaterial> sceneryRefinementMaterial(
    const ResidentPresentationMaterial &native, uint32_t descriptor,
    const ResidentPresentationMaterial *nearest, uint32_t nearestShift) {
  if (!(native.sourceToken & 0x800u))
    return texels(native) && native.sourceToken == native.resolvedToken
               ? std::optional{native}
               : std::nullopt;
  if (!(descriptor & 0x80000000u) || nearest == nullptr || !native.decoded ||
      !native.dynamicTexture || !nearest->dynamicTexture ||
      nearest->sourceToken != native.sourceToken)
    return std::nullopt;
  const uint32_t expected =
      (descriptor & 0x7ffu) + ((descriptor >> (nearestShift & 31u)) & 3u);
  if (expected == 0 || expected >= 0x800 || nearest->resolvedToken != expected)
    return std::nullopt;
  auto result = *nearest;
  result.sourceToken = result.resolvedToken;
  result.dynamicTexture = false;
  return texels(result) ? std::optional{result} : std::nullopt;
}

std::vector<ScenerySurfaceRefinement> findScenerySurfaceRefinements(
    std::span<const ResidentGpuTopologyFace> topology,
    std::span<const ResidentGpuRawVertex> vertices,
    std::span<const ResidentPresentationMaterial> materials, uint32_t world,
    uint32_t triangleCount, uint32_t quadCount,
    std::span<const SceneryPathStates> paths, bool allowSharedSupports) {
  if (world >= 8 || triangleCount > 2048 || quadCount > 2048 ||
      topology.size() != triangleCount + 2 * quadCount ||
      materials.size() != topology.size() || paths.empty())
    return {};
  struct Membership {
    uint32_t states = 0;
    std::vector<std::vector<uint64_t>> masks;
  };
  std::vector<Membership> programs;
  for (auto path : paths) {
    if (path.empty() || path.size() > 4096)
      return {};
    Membership program{
        static_cast<uint32_t>(path.size()),
        std::vector<std::vector<uint64_t>>(
            topology.size(), std::vector<uint64_t>((path.size() + 63) / 64))};
    for (uint32_t state = 0; state < path.size(); ++state)
      for (uint16_t packed : path[state]) {
        if ((packed >> 13) != world)
          continue;
        const uint32_t half = (packed >> 11) & 3, index = packed & 0x7ff;
        if (index >= (half == 0 ? triangleCount : quadCount))
          return {};
        const auto bit = UINT64_C(1) << (state & 63);
        if (half == 0)
          program.masks[index][state / 64] |= bit;
        else {
          if (half & 1)
            program.masks[triangleCount + 2 * index][state / 64] |= bit;
          if (half & 2)
            program.masks[triangleCount + 2 * index + 1][state / 64] |= bit;
        }
      }
    programs.push_back(std::move(program));
  }
  const auto membership = [](const Membership &p, const Polygon &polygon,
                             uint32_t word, bool all) {
    uint64_t bits = all ? UINT64_MAX : 0;
    for (uint32_t face : polygon.faces) {
      if (all)
        bits &= p.masks[face][word];
      else
        bits |= p.masks[face][word];
    }
    return bits;
  };
  // Actual native triangles retain 3D seams, including shared bevels which
  // cannot belong to the convex coplanar polygon catalogue below.
  std::vector<surface_partition::Triangle> mesh(topology.size());
  std::vector<uint8_t> stableMesh(topology.size());
  for (uint32_t i = 0; i < topology.size(); ++i) {
    bool stable = texels(materials[i]) != 0;
    for (uint32_t j = 0; j < 3; ++j) {
      const auto index = topology[i].sourceVertexIndex[j];
      if (index >= vertices.size())
        return {};
      const auto &v = vertices[index];
      stable &= (v.rawXy & (1u << 18)) == 0;
      mesh[i][j] = {static_cast<int32_t>((v.rawXy & 0xfff0) >> 2),
                    static_cast<int32_t>((v.rawXy >> 18) & 0x3ffc),
                    static_cast<int32_t>((v.rawZ & 0xfff0) >> 2)};
    }
    stableMesh[i] = stable;
  }
  std::vector<Polygon> polygons;
  std::vector<SourcePatch> patches;
  for (uint32_t i = 0; i < topology.size(); ++i) {
    const auto &face = topology[i];
    if (face.resourceFaceOrdinal != i)
      return {};
    if (face.half == 2)
      continue;
    const uint32_t density = texels(materials[i]);
    if (!density)
      continue;
    std::array<uint32_t, 4> indices{};
    uint32_t count = 3;
    std::vector<uint32_t> members{i};
    if (face.half == 0)
      std::copy(face.sourceVertexIndex.begin(), face.sourceVertexIndex.end(),
                indices.begin());
    else {
      if (face.half != 1 || i + 1 >= topology.size() ||
          topology[i + 1].half != 2 ||
          topology[i + 1].completeSourceVertexIndex !=
              face.completeSourceVertexIndex)
        return {};
      count = 4;
      // Source cache stores {a,b,c,d}; native triangles are {b,a,c} and
      // {a,c,d}. Their convex perimeter is {b,a,d,c}, not a packet strip.
      indices = {
          face.completeSourceVertexIndex[1], face.completeSourceVertexIndex[0],
          face.completeSourceVertexIndex[3], face.completeSourceVertexIndex[2]};
      members.push_back(i + 1);
    }
    std::array<surface_partition::Point, 4> points{};
    bool stable = true;
    for (uint32_t corner = 0; corner < count; ++corner) {
      if (indices[corner] >= vertices.size())
        return {};
      const auto &v = vertices[indices[corner]];
      stable &= (v.rawXy & (1u << 18)) ==
                0; // Deforming rest planes are not certificates.
      points[corner] = {static_cast<int32_t>((v.rawXy & 0xfff0) >> 2),
                        static_cast<int32_t>((v.rawXy >> 18) & 0x3ffc),
                        static_cast<int32_t>((v.rawZ & 0xfff0) >> 2)};
    }
    const auto shape =
        surface_partition::makeSurface(std::span{points}.first(count));
    if (stable) {
      SourcePatch patch{
          points, count, members, density,
          static_cast<uint16_t>(materials[i].texturePage & 0x1e0)};
      for (uint32_t axis = 0; axis < 3; ++axis) {
        auto flattened = points;
        for (auto &point : flattened)
          point[axis] = 0;
        patch.projections[axis] =
            surface_partition::makeSurface(std::span{flattened}.first(count));
      }
      patches.push_back(std::move(patch));
    }
    if (stable && shape)
      polygons.push_back(
          {*shape, std::move(members), density,
           static_cast<uint16_t>(materials[i].texturePage & 0x1e0)});
  }
  std::map<std::array<int64_t, 4>, std::vector<uint32_t>> planes;
  for (uint32_t i = 0; i < polygons.size(); ++i)
    planes[polygons[i].shape.plane].push_back(i);
  std::vector<ScenerySurfaceRefinement> result;
  for (const auto &parent : polygons) {
    std::vector<const Polygon *> children;
    std::vector<const Polygon *> crossingChildren;
    std::vector<const Polygon *> coveringChildren;
    std::vector<surface_partition::Surface> shapes;
    for (uint32_t i : planes[parent.shape.plane]) {
      const auto &child = polygons[i];
      if (child.shape.twiceArea >= parent.shape.twiceArea ||
          child.format != parent.format ||
          int64_t{child.texels} * parent.shape.twiceArea <
              int64_t{parent.texels} * child.shape.twiceArea ||
          !surface_partition::interiorsOverlap(parent.shape, child.shape))
        continue;
      bool seen = false, coSelected = false;
      for (const auto &program : programs)
        for (uint32_t word = 0; word < (program.states + 63) / 64; ++word) {
          seen |= (membership(program, child, word, true) &
                   ~membership(program, parent, word, false)) != 0;
          coSelected |= (membership(program, parent, word, false) &
                         membership(program, child, word, false)) != 0;
        }
      if (seen) {
        coveringChildren.push_back(&child);
        // Shared 3D support proofs retain their stricter exclusive policy.
        if (!coSelected)
          crossingChildren.push_back(&child);
        if (surface_partition::contains(parent.shape, child.shape)) {
          children.push_back(&child);
          shapes.push_back(child.shape);
        }
      }
    }
    const auto hasBranches = [&](const auto &pieces) {
      bool lowSeen = false, highSeen = false, partialHandoff = false;
      for (const auto &program : programs) {
        for (uint32_t word = 0; word < (program.states + 63) / 64; ++word) {
          const auto anyLow = membership(program, parent, word, false);
          uint64_t high = pieces.empty() ? 0 : UINT64_MAX;
          uint64_t anyHigh = 0;
          for (const auto *child : pieces) {
            high &= membership(program, *child, word, true);
            anyHigh |= membership(program, *child, word, false);
          }
          // SLST's two quad bits are independently culled native halves. The
          // coarse branch may expose only one; its authored four-corner source
          // surface is still replaced by a COMPLETE finer partition below.
          lowSeen |= (anyLow & ~anyHigh) != 0;
          highSeen |= (high & ~anyLow) != 0;
          // Native section paths may submit BOTH alternatives at their handoff.
          // This is safe only with the ENTIRE exact finer opaque partition,
          // plus independently observed low-only and high-only branches.
          // Partial overlaps/layers still veto; no state-count tolerance.
          partialHandoff |= (anyLow & anyHigh & ~high) != 0;
        }
      }
      return lowSeen && highSeen && !partialHandoff;
    };
    if (hasBranches(children) &&
        surface_partition::certify(parent.shape, shapes)) {
      ScenerySurfaceRefinement refinement{parent.faces, {}, {}};
      for (const auto *child : children)
        refinement.higherFaces.insert(refinement.higherFaces.end(),
                                      child->faces.begin(), child->faces.end());
      result.push_back(std::move(refinement));
      continue;
    }
    // A native coarse triangle may cut THROUGH detailed quads. Requiring each
    // child to fit its diagonal rejects a complete exact alternative. The
    // reusable integer union proof clips against the parent footprint and
    // rejects every hole/overlap; all offered children remain obligations.
    shapes.clear();
    for (const auto *child : coveringChildren)
      shapes.push_back(child->shape);
    if (hasBranches(coveringChildren) &&
        surface_partition::certifyCover(parent.shape, shapes)) {
      ScenerySurfaceRefinement refinement{parent.faces, {}, {}};
      for (const auto *child : coveringChildren)
        refinement.higherFaces.insert(refinement.higherFaces.end(),
                                      child->faces.begin(), child->faces.end());
      result.push_back(std::move(refinement));
      continue;
    }
    if (!allowSharedSupports || crossingChildren.size() < 2)
      continue;
    std::vector<uint32_t> seeds;
    for (const auto *child : crossingChildren)
      seeds.insert(seeds.end(), child->faces.begin(), child->faces.end());
    for (const auto &program : programs) {
      const uint32_t words = (program.states + 63) / 64;
      bool lowSeen = false, highSeen = false;
      std::vector<uint64_t> relevant(words);
      for (uint32_t word = 0; word < words; ++word) {
        const uint64_t low = membership(program, parent, word, true);
        uint64_t high = UINT64_MAX;
        for (const auto *child : crossingChildren)
          high &= membership(program, *child, word, true);
        lowSeen |= low != 0;
        highSeen |= high != 0;
        relevant[word] = low | high;
      }
      if (!lowSeen || !highSeen)
        continue;
      std::vector<uint32_t> supports;
      for (uint32_t i = 0; i < mesh.size(); ++i) {
        if (!stableMesh[i])
          continue;
        bool common = true;
        for (uint32_t word = 0; word < words; ++word)
          common &= (program.masks[i][word] & relevant[word]) == relevant[word];
        // Exclusive parents/children cannot pass this all-low AND all-high
        // condition. Support geometry is unchanged in both native branches.
        if (common)
          supports.push_back(i);
      }
      if (auto closure = surface_partition::connectedCover(parent.shape, mesh,
                                                           seeds, supports)) {
        std::sort(closure->begin(), closure->end());
        result.push_back({parent.faces, seeds, std::move(*closure)});
        break; // A proof dependency, not selection of a different shared mesh.
      }
    }
  }
  // Recover nonplanar/partially handed-off alternatives as complete authored
  // patches, NOT the union of unrelated choices from different native states.
  // Projection data and exact adjacency are built once per immutable asset.
  using Point = surface_partition::Point;
  using Edge = std::array<Point, 2>;
  const auto edge = [](Point a, Point b) -> Edge {
    return a < b ? Edge{a, b} : Edge{b, a};
  };
  std::map<Edge, std::vector<uint32_t>> incidence;
  for (uint32_t i = 0; i < patches.size(); ++i)
    for (uint32_t j = 0; j < patches[i].count; ++j)
      incidence[edge(patches[i].points[j],
                     patches[i].points[(j + 1) % patches[i].count])]
          .push_back(i);
  for (const auto &parent : patches) {
    if (std::any_of(result.begin(), result.end(), [&](const auto &family) {
          return family.lowerFaces == parent.faces;
        }))
      continue;
    const auto reference =
        surface_partition::makeSurface(std::span{parent.points}.first(3));
    if (!reference)
      continue;
    uint32_t axis = 0;
    for (uint32_t i = 1; i < 3; ++i)
      if (std::abs(reference->plane[i]) > std::abs(reference->plane[axis]))
        axis = i;
    if (!parent.projections[axis])
      continue;
    const auto &footprint = *parent.projections[axis];
    std::vector<uint32_t> candidates;
    std::set<Point> possibleAnchors;
    for (uint32_t i = 0; i < patches.size(); ++i) {
      const auto &child = patches[i];
      const auto &shape = child.projections[axis];
      if (!shape || shape->twiceArea >= footprint.twiceArea ||
          child.format != parent.format ||
          int64_t{child.texels} * footprint.twiceArea <
              int64_t{parent.texels} * shape->twiceArea ||
          !surface_partition::interiorsOverlap(footprint, *shape))
        continue;
      candidates.push_back(i);
      for (uint32_t j = 0; j < child.count; ++j)
        possibleAnchors.insert(child.points[j]);
    }
    bool anchored = candidates.size() >= 2;
    for (uint32_t j = 0; j < parent.count; ++j)
      anchored &= possibleAnchors.contains(parent.points[j]);
    if (!anchored)
      continue;
    std::set<std::vector<uint32_t>> nativeChoices;
    for (const auto &program : programs)
      for (uint32_t state = 0; state < program.states; ++state) {
        const uint32_t word = state / 64;
        const uint64_t bit = UINT64_C(1) << (state % 64);
        bool low = false;
        for (uint32_t face : parent.faces)
          low |= (program.masks[face][word] & bit) != 0;
        if (low)
          continue;
        std::vector<uint32_t> choice;
        for (uint32_t i : candidates) {
          bool complete = true;
          for (uint32_t face : patches[i].faces)
            complete &= (program.masks[face][word] & bit) != 0;
          if (complete)
            choice.push_back(i);
        }
        if (choice.size() >= 2)
          nativeChoices.insert(std::move(choice));
      }
    std::vector<uint32_t> best;
    bool ambiguous = false;
    std::set<std::vector<uint32_t>> checked;
    for (const auto &choice : nativeChoices) {
      std::set<uint32_t> remaining(choice.begin(), choice.end());
      while (!remaining.empty()) {
        std::vector<uint32_t> pending{*remaining.begin()}, higher;
        remaining.erase(remaining.begin());
        while (!pending.empty()) {
          const uint32_t i = pending.back();
          pending.pop_back();
          const auto &child = patches[i];
          higher.insert(higher.end(), child.faces.begin(), child.faces.end());
          for (uint32_t j = 0; j < child.count; ++j)
            for (uint32_t neighbor : incidence[edge(
                     child.points[j], child.points[(j + 1) % child.count])])
              if (remaining.erase(neighbor))
                pending.push_back(neighbor);
        }
        std::sort(higher.begin(), higher.end());
        if (higher.size() < best.size() || !checked.insert(higher).second)
          continue;
        bool coarseBranch = false;
        for (const auto &program : programs)
          for (uint32_t word = 0; word < (program.states + 63) / 64; ++word) {
            uint64_t low = 0, high = 0;
            for (uint32_t face : parent.faces)
              low |= program.masks[face][word];
            for (uint32_t face : higher)
              high |= program.masks[face][word];
            coarseBranch |= (low & ~high) != 0;
          }
        if (!coarseBranch)
          continue;
        std::vector<surface_partition::Triangle> triangles;
        for (uint32_t face : higher)
          triangles.push_back(mesh[face]);
        if (!surface_partition::certifyAnchoredPatch(
                std::span{parent.points}.first(parent.count), triangles))
          continue;
        if (higher.size() > best.size()) {
          best = std::move(higher);
          ambiguous = false;
        } else if (higher != best)
          ambiguous =
              true; // No arbitrary tie between equally detailed branches.
      }
    }
    if (!best.empty() && !ambiguous)
      result.push_back({parent.faces, std::move(best), {}, true});
  }
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return a.lowerFaces < b.lowerFaces;
  });
  // Return established direct relations without erasing their ancestors.
  // Canonical source-program compilation resolves dependencies transitively,
  // and rejects cycles/ambiguous ownership before any draw plan is published.
  return result;
}

bool scenerySurfaceRefinementContradicted(
    const ScenerySurfaceRefinement &family, uint32_t world,
    uint32_t triangleCount, std::span<const SceneryPathStates> paths) {
  std::array<uint8_t, 6144> roles{};
  for (uint32_t face : family.lowerFaces) {
    if (face >= roles.size())
      return true;
    roles[face] |= 1;
  }
  for (uint32_t face : family.higherFaces) {
    if (face >= roles.size())
      return true;
    roles[face] |= 2;
  }
  for (auto path : paths)
    for (const auto &state : path) {
      uint8_t seen = 0;
      std::set<uint32_t> highFaces;
      for (uint16_t packed : state) {
        if ((packed >> 13) != world)
          continue;
        const uint32_t half = (packed >> 11) & 3, index = packed & 0x7ff;
        const auto visit = [&](uint32_t face) {
          if (face < roles.size()) {
            seen |= roles[face];
            if (roles[face] & 2)
              highFaces.insert(face);
          }
        };
        if (half == 0)
          visit(index);
        else {
          if (half & 1)
            visit(triangleCount + 2 * index);
          if (half & 2)
            visit(triangleCount + 2 * index + 1);
        }
      }
      if (seen == 3 && !family.anchoredSourcePatch &&
          (!family.supportFaces.empty() ||
           highFaces.size() != family.higherFaces.size()))
        return true;
    }
  return false;
}
} // namespace ps1::games::crash3
