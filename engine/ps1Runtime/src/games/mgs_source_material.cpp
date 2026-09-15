#include "runtime/games/mgs_decomp.h"
#include "runtime/games/mgs_mesh_replay.h"
#include "runtime/games/mgs_scene.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include <algorithm>
#include <cmath>

namespace ps1::games::mgs {
namespace {
constexpr uint32_t order[] = {0, 1, 3, 2};
uint16_t u16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t u32(const uint8_t *p) {
  return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16;
}
bool range(uint32_t p, uint64_t n) {
  p &= 0x1fffffffu;
  return p < Memory::RAM_SIZE && n <= Memory::RAM_SIZE - p;
}
bool cinematicBodyRig(const AssetCache &assets, const SceneInstance &instance) {
  if (instance.pose.source != ScenePoseSource::Rotations ||
      instance.pose.invalidInput || instance.primaryModels < 10 ||
      instance.primaryModels > instance.models.size())
    return false;
  // Cinematic scenery also carries rotation arrays: the intro submarine is
  // a 13-part rig with independent roots. A body needs a connected skeleton,
  // not merely many animated parts. Original KMD parent indices establish it.
  for (uint32_t bone = 0; bone < instance.primaryModels; ++bone) {
    const auto source = assets.sourceModel(instance.models[bone].assetModel);
    if (!source ||
        (bone == 0 ? source->parent != -1
                   : source->parent < 0 || uint32_t(source->parent) >= bone))
      return false;
  }
  return true;
}
// optxtscn.c replaces its two placeholder PCXs with text assembled from the
// EF71 font by DR_MOVE commands in CC22. The KMD surface is still immutable;
// its material is a render target, not another static image to guess by pixels.
bool textScreenMaterial(const Memory &memory, uint16_t id, const uint8_t *tex,
                        uint16_t page, uint16_t clut) {
  if (!tex || (id != 0xa91d && id != 0xa91e))
    return false;
  const uint32_t target = findSourceTexture(memory, 0xcc22);
  const uint32_t font = findSourceTexture(memory, 0xef71);
  if (!target || !font)
    return false;
  const auto *dst = memory.ramPtr() + (target & 0x1fffffffu);
  const auto *src = memory.ramPtr() + (font & 0x1fffffffu);
  return (u16(dst + 4) & 0x180u) == 0 && dst[8] == 0 && dst[9] == 0 &&
         dst[10] == 255 && dst[11] == 255 && src[10] == 111 && src[11] == 6 &&
         page == uint16_t((u16(dst + 4) & ~0x60u) | 0x20u) &&
         clut == u16(src + 6) && u16(tex + 4) == page && u16(tex + 6) == clut &&
         tex[8] == 0 && tex[9] == (id == 0xa91d ? 0 : 128) && tex[10] == 127 &&
         tex[11] == 69;
}
// DG_ShadeChanl's light model evaluated from original normals and the float
// skeleton. No GTE, scratchpad, visibility test or native color saturation.
std::optional<gpu::replay::Rgb8>
shadeCorner(const AssetCache &assets, const Memory &memory,
            const SceneSnapshot &scene, const SceneInstance &instance,
            std::span<const uint32_t> owners, uint32_t model, uint32_t corner) {
  const uint32_t first = assets.word(AssetCache::Assets, instance.asset, 9);
  const auto *bytes = assets.bytes().data();
  for (size_t step = 0; step <= owners.size(); ++step) {
    if (model >= owners.size() || owners[model] >= instance.models.size())
      return {};
    const auto source = assets.sourceModel(first + model);
    if (!source || corner >= source->faces * 4 || !source->normalIndicesByte ||
        !source->normalsByte)
      return {};
    const uint32_t index = bytes[source->vertexIndicesByte + corner];
    if (index >= source->vertices)
      return {};
    const int16_t seam =
        int16_t(u16(bytes + source->positionsByte + index * 8 + 6));
    if ((source->flags & 0x10000u) && seam >= 0) {
      if (source->parent < 0 || uint32_t(source->parent) >= owners.size())
        return {};
      model = uint32_t(source->parent);
      const auto parent = assets.sourceModel(first + model);
      if (!parent)
        return {};
      corner = 0;
      while (corner < parent->faces * 4 &&
             bytes[parent->vertexIndicesByte + corner] != uint32_t(seam))
        ++corner;
      continue;
    }
    uint32_t normal = bytes[source->normalIndicesByte + corner];
    if (source->flags & 0x10000u)
      normal &= 127u;
    if (normal >= source->normals)
      return {};
    std::array<double, 3> worldNormal{}, illumination{}, color{};
    const auto &world = instance.models[owners[model]].presentationWorld;
    for (unsigned row = 0; row < 3; ++row)
      for (unsigned axis = 0; axis < 3; ++axis)
        worldNormal[row] +=
            world[axis * 4 + row] *
            (int16_t(u16(bytes + source->normalsByte + normal * 8 + axis * 2)) /
             4096.0);
    const auto *ram = memory.ramPtr();
    const uint32_t lights =
        u32(ram + ((instance.guestAddress + 52) & 0x1fffffffu));
    if (!lights || !range(lights, 64))
      return {};
    const auto *light = ram + (lights & 0x1fffffffu);
    for (unsigned row = 0; row < 3; ++row) {
      for (unsigned axis = 0; axis < 3; ++axis)
        illumination[row] += int16_t(u16(light + (row * 3 + axis) * 2)) /
                             4096.0 * worldNormal[axis];
      illumination[row] = std::max(0.0, illumination[row]);
    }
    for (unsigned row = 0; row < 3; ++row) {
      color[row] = instance.flags & 0x100u ? int32_t(u32(light + 20 + row * 4))
                                           : scene.ambient[row];
      for (unsigned axis = 0; axis < 3; ++axis)
        color[row] += int16_t(u16(light + 32 + (row * 3 + axis) * 2)) / 16.0 *
                      illumination[axis];
    }
    return gpu::replay::Rgb8{
        uint8_t(std::clamp(std::lround(color[0]), 0l, 255l)),
        uint8_t(std::clamp(std::lround(color[1]), 0l, 255l)),
        uint8_t(std::clamp(std::lround(color[2]), 0l, 255l))};
  }
  return {};
}
} // namespace

std::optional<gpu::replay::DrawPacket>
sourceMeshMaterial(const AssetCache &assets, const Memory &memory,
                   const SceneSnapshot &scene, const SceneInstance &instance,
                   uint32_t model, std::span<const uint32_t> owners,
                   uint32_t face, uint32_t nativePacket) {
  if (instance.asset == ~0u || model >= instance.models.size() ||
      !range(instance.guestAddress, 72))
    return {};
  const auto source = assets.sourceModel(instance.models[model].assetModel);
  if (!source || face >= source->faces || !source->uvByte ||
      !source->textureIdsByte)
    return {};
  const auto ptr = [&](uint32_t p) {
    return memory.ramPtr() + (p & 0x1fffffffu);
  };
  const uint32_t descriptor = instance.definition + 32 + model * 88;
  const uint32_t object = instance.models[model].guestAddress;
  if (!range(descriptor, 88) || !range(object, 92))
    return {};
  const uint32_t ids = u32(ptr(descriptor + 80));
  if (!ids || !range(ids, uint64_t(source->faces) * 2))
    return {};
  const uint16_t id = u16(ptr(ids + face * 2));
  const uint32_t texture = findSourceTexture(memory, id);
  const bool packetValid = nativePacket && range(nativePacket, 52) &&
                           (ptr(nativePacket)[7] & 0xfcu) == 0x3cu;
  // IRTEXTURE is eligibility for a palette swap, not an active material mode.
  // UpdateThermalTexture preserves normals and UVs even while thermal is on.
  const bool specialMaterial = (instance.flags & 0x400u) != 0;
  if ((!texture && id != 0 && !packetValid) ||
      (specialMaterial && !packetValid))
    return {};
  // The zero material in original KMDs is DG_NullTexture/EmptyTex, not a
  // missing PCX. DG_WriteObjPacketUV defines its zero page, CLUT and UVs even
  // if the model currently has no allocated packets.
  const std::array<uint8_t, 12> emptyTexture{};
  const uint8_t *tex = texture   ? ptr(texture)
                       : id == 0 ? emptyTexture.data()
                                 : nullptr;
  uint16_t page = tex ? u16(tex + 4) : 0, clut = tex ? u16(tex + 6) : 0;
  gpu::replay::DrawPacket draw;
  draw.opcode = (u32(ptr(descriptor)) & 2) ? 0x3e : 0x3c;
  if (packetValid) {
    draw.opcode = ptr(nativePacket)[7];
    page = u16(ptr(nativePacket + 26));
    clut = u16(ptr(nativePacket + 14));
  }
  draw.textured = draw.gouraud = true;
  draw.rawTexture = (draw.opcode & 1) != 0;
  draw.vertexCount = 4;
  draw.indexCount = 6;
  draw.triangleIndices = {0, 1, 2, 1, 2, 3};
  draw.state.texturePage = {page, uint16_t((page & 15) * 64),
                            uint16_t((page & 16) * 16),
                            gpu::replay::TextureDepth((page >> 7) & 3)};
  draw.state.clut = {clut, uint16_t((clut & 63) * 16), uint16_t(clut >> 6)};
  draw.state.blend.enabled = (draw.opcode & 2) != 0;
  draw.state.blend.texturedStpOnly = draw.state.blend.enabled;
  draw.state.blend.mode = gpu::replay::BlendMode((page >> 5) & 3);
  const uint32_t colors = u32(ptr(object + 68));
  const bool painted = colors && range(colors, uint64_t(source->faces) * 16);
  const bool shaded = (instance.flags & 8u) && !specialMaterial;
  for (unsigned k = 0; k < 4; ++k) {
    auto &vertex = draw.vertices[k];
    const auto *uv =
        assets.bytes().data() + source->uvByte + (face * 4 + order[k]) * 2;
    double u = tex ? double(uv[0]) * (uint32_t(tex[10]) + 1) / 256 + tex[8] : 0;
    double v = tex ? double(uv[1]) * (uint32_t(tex[11]) + 1) / 256 + tex[9] : 0;
    // UV animation, IR and environment mapping are actor material inputs.
    // Ordinary materials keep the fractional coordinates from the source.
    if (packetValid && (specialMaterial || !tex ||
                        (uint32_t(std::floor(u)) & 255) !=
                            ptr(nativePacket + 12 + k * 12)[0] ||
                        (uint32_t(std::floor(v)) & 255) !=
                            ptr(nativePacket + 12 + k * 12)[1])) {
      u = ptr(nativePacket + 12 + k * 12)[0];
      v = ptr(nativePacket + 12 + k * 12)[1];
    }
    vertex.u = std::fmod(u, 256.0);
    vertex.v = std::fmod(v, 256.0);
    uint32_t rgb = packetValid ? u32(ptr(nativePacket + 4 + k * 12))
                   : painted   ? u32(ptr(colors + (face * 4 + order[k]) * 4))
                               : 0x808080u;
    vertex.color = {uint8_t(rgb), uint8_t(rgb >> 8), uint8_t(rgb >> 16)};
    if (shaded) {
      const auto color = shadeCorner(assets, memory, scene, instance, owners,
                                     model, face * 4 + order[k]);
      if (!color)
        return {};
      vertex.color = *color;
    }
  }
  draw.originalMesh.emplace();
  // USA goggleir.c Act sets STATE_THERMG at 0x800adb3c. Cinematic actors are
  // created without the gameplay IRTEXTURE flag. Give connected body rigs an
  // authored warm response during demos; independently animated scenery is
  // excluded even when it has a rotation array and many parts.
  // This is an artistic rig classification, not simulated temperatures.
  const uint32_t gameStatus = u32(ptr(0x800adb3c));
  draw.originalMesh->thermalEmission =
      (gameStatus & 8u) &&
      ((instance.flags & 0x200u) ||
       ((gameStatus & 0x80000000u) && cinematicBodyRig(assets, instance)));
  if (draw.originalMesh->thermalEmission)
    metrics::count("mgs.source.thermal_emissive_faces");
  draw.originalMesh->textureId = id;
  draw.originalMesh->sourceTopology = draw.originalMesh->sourceRecovery = true;
  draw.originalMesh->runtimeTexture =
      textScreenMaterial(memory, id, tex, page, clut);
  if (id == 0 && page == 0 && clut == 0 &&
      std::all_of(draw.vertices.begin(), draw.vertices.begin() + 4,
                  [](const auto &v) {
                    return v.u >= 0 && v.u < 1 && v.v >= 0 && v.v < 1;
                  }))
    draw.originalMesh->runtimeTexture = true;
  if (shaded)
    metrics::count("mgs.source.shaded_faces");
  return draw;
}
} // namespace ps1::games::mgs
