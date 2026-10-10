#pragma once
#include "runtime/metrics.h"
#include <vulkan/vulkan.h>
#include <map>
#include <mutex>

namespace ps1::gpu {
// Tracks successful allocations owned by the renderer and MGS asset cache.
// Allocation sizes include Vulkan memory requirements/alignment, but exclude
// driver-private, swapchain-owned and ImGui backend allocations. This is NOT a
// device-wide VRAM usage figure. Disabled in ordinary player sessions.
struct VulkanMemoryLedger {
  std::mutex mutex;
  std::map<std::pair<VkDevice,VkDeviceMemory>,VkDeviceSize> allocations;
  uint64_t bytes=0, peak=0;
  void publish() {
    metrics::setState("host.vulkan_memory.live_allocations",allocations.size());
    metrics::setState("host.vulkan_memory.live_bytes",bytes);
    metrics::setState("host.vulkan_memory.peak_bytes",peak);
  }
};
inline VulkanMemoryLedger vulkanMemoryLedger;

inline VkResult allocateTrackedVulkanMemory(VkDevice device,const VkMemoryAllocateInfo* info,
                                            const VkAllocationCallbacks* callbacks,VkDeviceMemory* memory) {
  const auto result=vkAllocateMemory(device,info,callbacks,memory);
  if(metrics::enabled()) {
    if(result!=VK_SUCCESS) metrics::count("host.vulkan_memory.allocate_failed");
    else {
      std::lock_guard lock(vulkanMemoryLedger.mutex);
      const auto [it,inserted]=vulkanMemoryLedger.allocations.emplace(std::make_pair(device,*memory),info->allocationSize);
      if(inserted) {
        vulkanMemoryLedger.bytes+=info->allocationSize;
        if(vulkanMemoryLedger.bytes>vulkanMemoryLedger.peak) vulkanMemoryLedger.peak=vulkanMemoryLedger.bytes;
      } else metrics::count("host.vulkan_memory.duplicate_handle");
      vulkanMemoryLedger.publish();
    }
  }
  return result;
}
inline void freeTrackedVulkanMemory(VkDevice device,VkDeviceMemory memory,const VkAllocationCallbacks* callbacks) {
  if(metrics::enabled() && memory!=VK_NULL_HANDLE) {
    std::lock_guard lock(vulkanMemoryLedger.mutex);
    const auto it=vulkanMemoryLedger.allocations.find({device,memory});
    if(it!=vulkanMemoryLedger.allocations.end()) {
      vulkanMemoryLedger.bytes-=it->second;
      vulkanMemoryLedger.allocations.erase(it);
    } else metrics::count("host.vulkan_memory.untracked_free");
    // Remove ownership before the driver can recycle this handle on another
    // thread. A concurrent allocation publishes only after this lock releases.
    vkFreeMemory(device,memory,callbacks);
    vulkanMemoryLedger.publish();
  } else vkFreeMemory(device,memory,callbacks);
}
}
