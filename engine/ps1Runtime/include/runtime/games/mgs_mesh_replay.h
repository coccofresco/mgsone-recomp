#pragma once
#include "runtime/games/mgs_asset_cache.h"
#include "runtime/gpu/modern_replay.h"
#include "runtime/gpu/native_color.h"
struct recomp_context;
namespace ps1::gpu::replay {
struct DrawPacket;
}
namespace ps1::games::mgs {
// Complete source faces absent from this exact native ordering table. These
// are presentation operations, never synthetic GP0 commands in guest RAM.
struct SceneRecoveryReplay {
  uint64_t capture = 0;
  uint32_t orderingTable = 0;
  std::array<std::vector<gpu::replay::DrawPacket>, 256> buckets;
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
