#pragma once
#include "runtime/games/mgs_asset_cache.h"
#include "runtime/gpu/modern_replay.h"
#include "runtime/gpu/native_color.h"
#include <unordered_map>
struct recomp_context;
namespace ps1 {
class Memory;
}
namespace ps1::gpu::replay {
struct DrawPacket;
}
namespace ps1::games::mgs {
struct SceneSnapshot;
struct SceneInstance;
// Material adapter only. Geometry, normals and UV topology are read from the
// original resource; live state supplies lights, palette and actor overrides.
std::optional<gpu::replay::DrawPacket>
sourceMeshMaterial(const AssetCache &assets, const Memory &memory,
                   const SceneSnapshot &scene, const SceneInstance &instance,
                   uint32_t model, std::span<const uint32_t> owners,
                   uint32_t face, uint32_t nativePacket);
// Complete source faces for this exact native ordering table, including faces
// absent from its packets. Presentation operations never change guest RAM.
struct SceneRecoveryReplay {
  uint64_t capture = 0;
  uint32_t orderingTable = 0;
  std::array<std::vector<gpu::replay::DrawPacket>, 256> buckets;
  struct Mesh {
    gpu::replay::DrawPacket draw;
    bool visible = true, attempted = false, accepted = false;
  };
  // All native subdivision children share one original face. Publish that
  // face once at the first child, preserving its place among scene effects.
  std::vector<Mesh> meshes;
  std::unordered_map<uint32_t, uint32_t> meshNodes;
};
std::unique_ptr<SceneRecoveryReplay>
recoverSceneReplay(uint32_t listStart, std::span<const uint8_t> ram);
bool bindRecoveredSceneDraw(gpu::replay::DrawPacket &packet,
                            const gpu::replay::BakedDrawState &environment,
                            std::span<const gpu::Color16> vram);
// Producer-thread bridge. It changes the presentation packet only, never RAM,
// the GP0 command stream or the authoritative software raster.
void setMeshReplayCache(const AssetCache *cache);
void resetMeshReplay();
void prepareMeshReplay(recomp_context *ctx);
void beginMeshDivide(recomp_context *ctx);
void meshDivideGrid(recomp_context *ctx);
void meshDivideCopy(recomp_context *ctx);
void meshDividePacket(recomp_context *ctx);
void adaptMeshReplayPacket(gpu::replay::DrawPacket &packet);
void bindOriginalTexture(gpu::replay::DrawPacket &packet,
                         std::span<const gpu::Color16> vram);
bool replaceMeshReplayPacket(gpu::replay::DrawPacket &packet);
} // namespace ps1::games::mgs
void mgsPrepareMeshReplay(recomp_context *ctx);
