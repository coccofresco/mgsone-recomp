#pragma once
#include "runtime/games/mgs_asset_cache.h"
#include <vulkan/vulkan.h>
#include <memory>

namespace ps1::gpu {
// Device-local lifetime is independent of the swapchain, stage and savestate.
// This owns storage only. Draw ownership belongs to the future MGS scene adapter.
class MgsAssetGpuCache {
public:
  static std::unique_ptr<MgsAssetGpuCache> upload(VkPhysicalDevice physical,
      VkDevice device, VkQueue queue, VkCommandPool pool,
      games::mgs::AssetCache cache, bool verifyReadback);
  ~MgsAssetGpuCache();
  MgsAssetGpuCache(const MgsAssetGpuCache &) = delete;
  MgsAssetGpuCache &operator=(const MgsAssetGpuCache &) = delete;
  VkBuffer buffer() const { return buffer_; }
  const games::mgs::AssetCache &assets() const { return cache_; }
private:
  MgsAssetGpuCache() = default;
  VkDevice device_ = VK_NULL_HANDLE;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  games::mgs::AssetCache cache_;
};
} // namespace ps1::gpu
