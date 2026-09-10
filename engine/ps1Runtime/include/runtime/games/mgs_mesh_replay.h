#pragma once
#include "runtime/games/mgs_asset_cache.h"
struct recomp_context;
namespace ps1::gpu::replay {
struct DrawPacket;
}
namespace ps1::games::mgs {
// Producer-thread bridge. It changes the presentation packet only, never RAM,
// the GP0 command stream or the authoritative software raster.
void setMeshReplayCache(const AssetCache *cache);
void resetMeshReplay();
void prepareMeshReplay(recomp_context *ctx);
void adaptMeshReplayPacket(gpu::replay::DrawPacket &packet);
bool replaceMeshReplayPacket(gpu::replay::DrawPacket &packet);
} // namespace ps1::games::mgs
void mgsPrepareMeshReplay(recomp_context *ctx);
