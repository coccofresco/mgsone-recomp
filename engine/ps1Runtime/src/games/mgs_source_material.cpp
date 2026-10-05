#include "runtime/games/mgs_decomp.h"
#include "runtime/games/mgs_mesh_replay.h"
#include "runtime/games/mgs_scene.h"
#include "runtime/games/mgs_surface_effects.h"
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
// Source normal transformed with the float pose. Lighting is evaluated only
// after perspective interpolation in Vulkan, including at rigid parent seams.
std::optional<std::array<float, 4>>
normalCorner(const AssetCache &assets, const SceneInstance &instance,
             std::span<const uint32_t> owners, uint32_t model,
             uint32_t corner) {
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
    std::array<double, 3> worldNormal{};
    const auto &world = instance.models[owners[model]].presentationWorld;
    // Inverse transpose, including non-uniform model scaling. The magnitude
    // of 1/determinant cancels at normalization; its sign must be retained.
    std::array<std::array<double, 3>, 3> cofactor{};
    for (unsigned axis = 0; axis < 3; ++axis)
      for (unsigned row = 0; row < 3; ++row)
        cofactor[axis][row] =
            double(world[((axis + 1) % 3) * 4 + (row + 1) % 3]) *
                world[((axis + 2) % 3) * 4 + (row + 2) % 3] -
            double(world[((axis + 1) % 3) * 4 + (row + 2) % 3]) *
                world[((axis + 2) % 3) * 4 + (row + 1) % 3];
    const double determinant = world[0] * cofactor[0][0] +
                               world[1] * cofactor[0][1] +
                               world[2] * cofactor[0][2];
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12)
      return {};
    for (unsigned axis = 0; axis < 3; ++axis)
      for (unsigned row = 0; row < 3; ++row)
        worldNormal[row] +=
            cofactor[axis][row] *
            int16_t(u16(bytes + source->normalsByte + normal * 8 + axis * 2)) *
            (determinant < 0 ? -1.0 : 1.0);
    const double length = std::sqrt(worldNormal[0] * worldNormal[0] +
                                    worldNormal[1] * worldNormal[1] +
                                    worldNormal[2] * worldNormal[2]);
    if (!std::isfinite(length) || length < 1e-8)
      return {};
    return std::array<float, 4>{float(worldNormal[0] / length),
                                float(worldNormal[1] / length),
                                float(worldNormal[2] / length), 0};
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
  draw.surfaceEffect=surfaceEffectForObject(instance.guestAddress);
  using SurfaceKind=gpu::replay::MgsSurfaceKind;
  draw.opcode = (u32(ptr(descriptor)) & 2) ? 0x3e : 0x3c;
  if (packetValid) {
    draw.opcode = ptr(nativePacket)[7];
    page = u16(ptr(nativePacket + 26));
    clut = u16(ptr(nativePacket + 14));
  }
  if(draw.surfaceEffect.kind==SurfaceKind::Camo &&
     (!packetValid || ((page>>7)&3)!=2)) {
    if(draw.surfaceEffect.parameters[2][2]!=0) {
      draw.surfaceEffect.kind=SurfaceKind::Mirror;
      draw.surfaceEffect.parameters[0][0]=float(SurfaceKind::Mirror);
    } else draw.surfaceEffect={};
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
  const bool stageShaded = instance.stageLighting && !(source->flags & 4u);
  const bool shaded = ((instance.flags & 8u) || stageShaded) &&
                      !specialMaterial && !draw.rawTexture && draw.surfaceEffect.kind!=SurfaceKind::Shadow;
  const bool opticalNormals=draw.surfaceEffect.kind==SurfaceKind::Environment ||
                             draw.surfaceEffect.kind==SurfaceKind::Camo || draw.surfaceEffect.kind==SurfaceKind::Afterimage ||
                             draw.surfaceEffect.kind==SurfaceKind::Energy;
  draw.originalMesh.emplace();
  auto &lighting = draw.originalMesh->lighting;
  if (model >= owners.size() || owners[model] >= instance.models.size())
    return {};
  const auto &world = instance.models[owners[model]].presentationWorld;
  if(draw.surfaceEffect.kind==SurfaceKind::Submarine) {
    auto &p=draw.surfaceEffect.parameters;
    const float cosine=p[2][0],sine=p[2][1];
    // SubEfct projects local X after the authored Z rotation, at 1/8 scale.
    // Express that projection in world space for continuous fragment UVs,
    // without the original per-vertex byte truncation/wrap interpolation.
    p[0][2]=128.f;
    for(unsigned row=0;row<3;++row) {
      p[2][row]=(cosine*world[row]-sine*world[4+row])*0.125f;
      p[0][2]-=p[2][row]*world[12+row];
    }
  }
  for (unsigned k = 0; k < 4; ++k) {
    const auto index =
        assets.bytes()[source->vertexIndicesByte + face * 4 + order[k]];
    if (index >= source->vertices)
      return {};
    const auto *p =
        assets.bytes().data() + source->positionsByte + index * 8;
    for (unsigned row = 0; row < 3; ++row) {
      lighting.positions[k][row] = world[12 + row];
      for (unsigned axis = 0; axis < 3; ++axis)
        lighting.positions[k][row] +=
            world[axis * 4 + row] * int16_t(u16(p + axis * 2));
    }
  }
  if (shaded) {
    const uint32_t lights =
        stageShaded ? 0x8009fad0 : u32(ptr(instance.guestAddress + 52));
    if (!lights || !range(lights, 64))
      return {};
    const auto *light = ptr(lights);
    for (unsigned row = 0; row < 3; ++row) {
      lighting.ambient[row] = instance.flags & 0x100u
                                  ? float(int32_t(u32(light + 20 + row * 4)))
                                  : scene.ambient[row];
      for (unsigned axis = 0; axis < 3; ++axis) {
        lighting.directions[row][axis] =
            int16_t(u16(light + (row * 3 + axis) * 2)) / 4096.f;
        lighting.colors[row][axis] =
            int16_t(u16(light + 32 + (row * 3 + axis) * 2)) / 16.f;
      }
    }
    lighting.ambient[3] = 1;
    if (stageShaded) {
      // Stage geometry's old RGB array is DG_MakePreshade's output, not
      // authored albedo. Recover the real .LIT sources instead of relighting
      // those already illuminated colors or inheriting the two-light limit.
      for (unsigned row = 0; row < 3; ++row) {
        lighting.ambient[row] = scene.ambient[row];
        lighting.directions[1][row] = lighting.directions[2][row] = 0;
        lighting.colors[row][1] = lighting.colors[row][2] = 0;
      }
      // Conservative world AABB culling has no count cap. Every light whose
      // authored influence can touch the face is evaluated per fragment.
      for (const auto &light : instance.pointLights) {
        bool touches = true;
        for (unsigned axis = 0; axis < 3; ++axis) {
          float low = lighting.positions[0][axis], high = low;
          for (unsigned k = 1; k < 4; ++k) {
            low = std::min(low, lighting.positions[k][axis]);
            high = std::max(high, lighting.positions[k][axis]);
          }
          touches &= light.positionRadius[axis] + light.colorExtent[3] >= low &&
                     light.positionRadius[axis] - light.colorExtent[3] <= high;
        }
        if (touches)
          lighting.pointLights.push_back(light);
      }
      metrics::count("mgs.lighting.stage_per_pixel_faces");
    }
  }
  for (unsigned k = 0; k < 4; ++k) {
    auto &vertex = draw.vertices[k];
    const auto *uv =
        assets.bytes().data() + source->uvByte + (face * 4 + order[k]) * 2;
    double u = tex ? double(uv[0]) * (uint32_t(tex[10]) + 1) / 256 + tex[8] : 0;
    double v = tex ? double(uv[1]) * (uint32_t(tex[11]) + 1) / 256 + tex[9] : 0;
    // UV animation, IR and environment mapping are actor material inputs.
    // Ordinary materials keep the fractional coordinates from the source.
    if (packetValid && draw.surfaceEffect.kind!=SurfaceKind::Submarine && (specialMaterial || !tex ||
                        (uint32_t(std::floor(u)) & 255) !=
                            ptr(nativePacket + 12 + k * 12)[0] ||
                        (uint32_t(std::floor(v)) & 255) !=
                            ptr(nativePacket + 12 + k * 12)[1])) {
      // Animation changes the integer tile/scroll phase. Preserve the
      // fractional coordinates of the source format underneath that phase.
      const auto *native=ptr(nativePacket + 12 + k * 12);
      u = native[0] + (tex && !specialMaterial ? u-std::floor(u) : 0.0);
      v = native[1] + (tex && !specialMaterial ? v-std::floor(v) : 0.0);
    }
    vertex.u = std::fmod(u, 256.0);
    vertex.v = std::fmod(v, 256.0);
    uint32_t rgb = packetValid ? u32(ptr(nativePacket + 4 + k * 12))
                   : painted   ? u32(ptr(colors + (face * 4 + order[k]) * 4))
                               : 0x808080u;
    vertex.color = {uint8_t(rgb), uint8_t(rgb >> 8), uint8_t(rgb >> 16)};
    if (shaded || opticalNormals) {
      const auto normal =
          normalCorner(assets, instance, owners, model, face * 4 + order[k]);
      if (!normal)
        return {};
      lighting.normals[k] = *normal;
      if(opticalNormals && draw.surfaceEffect.kind!=SurfaceKind::Afterimage) {
        for(unsigned row=0;row<3;++row) {
          lighting.normals[k][row]=0;
          for(unsigned axis=0;axis<3;++axis)
            lighting.normals[k][row]+=scene.view[axis*4+row]*(*normal)[axis];
        }
      }
      // Neutral albedo. Never bake or saturate illumination into vertex RGB.
      if(shaded) vertex.color = {128, 128, 128};
    }
  }
  if(draw.surfaceEffect.kind==SurfaceKind::Shadow) {
    std::array<float,4> bounds{1e9f,1e9f,-1e9f,-1e9f};
    for(const auto &v:draw.vertices) {
      bounds[0]=std::min(bounds[0],float(v.u));bounds[1]=std::min(bounds[1],float(v.v));
      bounds[2]=std::max(bounds[2],float(v.u));bounds[3]=std::max(bounds[3],float(v.v));
    }
    draw.surfaceEffect.parameters[1]=bounds;
  }
  if(draw.surfaceEffect.kind==SurfaceKind::Afterimage)
    for(unsigned axis=0;axis<3;++axis) draw.surfaceEffect.parameters[2][axis]=scene.view[axis*4+2];
  // USA goggleir.c also sets STATE_THERMG when DMO uses it for a red grade.
  // Only gameplay thermal mode receives the authored IR material response.
  // A cinematic skeleton is not evidence of a hot body: retain its shading.
  const uint32_t gameStatus = u32(ptr(0x800adb3c));
  draw.originalMesh->thermalEmission = (gameStatus & 8u) &&
                                       !(gameStatus & 0x80000000u) &&
                                       (instance.flags & 0x200u);
  if (draw.originalMesh->thermalEmission)
    metrics::count("mgs.source.thermal_emissive_faces");
  draw.originalMesh->textureId = id;
  if(draw.surfaceEffect.kind==SurfaceKind::Environment)
    draw.originalMesh->textureId=uint32_t(draw.surfaceEffect.parameters[2][0]);
  draw.originalMesh->sourceTopology = draw.originalMesh->sourceRecovery = true;
  draw.originalMesh->runtimeTexture =
      textScreenMaterial(memory, id, tex, page, clut);
  // Kogaku2 replaces each affected face's authored texture with the scene
  // framebuffer. The live owner and Direct15 page were checked above, including
  // the original reveal threshold and thermal exclusion. Declare that dynamic
  // material before submitting the source mesh; it has no static PCX to bind.
  if (draw.surfaceEffect.kind == SurfaceKind::Camo &&
      draw.state.texturePage.depth == gpu::replay::TextureDepth::Direct15) {
    draw.originalMesh->runtimeTexture = true;
    metrics::count("mgs.source.camouflage_faces");
  }
  if (id == 0 && page == 0 && clut == 0 &&
      std::all_of(draw.vertices.begin(), draw.vertices.begin() + 4,
                  [](const auto &v) {
                    return v.u >= 0 && v.u < 1 && v.v >= 0 && v.v < 1;
                  }))
    draw.originalMesh->runtimeTexture = true;
  if (shaded) {
    metrics::count("mgs.source.shaded_faces");
    metrics::count("mgs.lighting.per_pixel_faces");
  } else {
    // PAINT colors are authored/baked material inputs, not live lighting.
    metrics::count("mgs.lighting.authored_color_faces");
  }
  return draw;
}
} // namespace ps1::games::mgs
