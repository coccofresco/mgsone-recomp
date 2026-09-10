#pragma once
#include "runtime/games/mgs_asset_cache.h"
#include <array>
#include <memory>
#include <vector>
struct recomp_context;
namespace ps1 { class Memory; }
namespace ps1::games::mgs {
struct SceneModel {
  uint32_t guestAddress = 0, assetModel = 0, flags = 0;
  std::array<float, 16> world{};
  std::array<uint32_t, 2> originalPackets{};
};
struct SceneInstance {
  uint32_t guestAddress = 0, definition = 0, flags = 0, group = 0;
  uint32_t asset = ~0u;
  bool visible = false;
  bool deformed = false;
  std::vector<SceneModel> models;
};
struct SceneSnapshot {
  uint64_t sequence = 0;
  uint32_t vblank = 0, channel = 0, bufferIndex = 0, currentGroup = 0;
  int32_t projection = 0;
  std::array<int16_t, 4> clip{};
  std::array<float, 16> view{};
  bool widescreen = false;
  std::vector<SceneInstance> instances;
};
// Set before starting the game thread; clear after that thread has stopped.
// A snapshot owns all dynamic data and never exposes RAM pointers to Vulkan.
void setSceneAssetCache(const AssetCache *cache);
void resetSceneCapture();
std::shared_ptr<const SceneSnapshot> latestSceneSnapshot();
uint32_t matchOriginalDefinition(const AssetCache &cache, const Memory &memory, uint32_t definition);
uint32_t matchSceneDefinition(const AssetCache &cache, const Memory &memory,
                              uint32_t definition, bool &deformed);
void captureSceneBeforeBounds(recomp_context *ctx);
}
// USA recipe instruction override, executed before any PSX bounding/culling.
void mgsCaptureSceneBeforeBounds(recomp_context *ctx);
