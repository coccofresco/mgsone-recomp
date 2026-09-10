#include "runtime/gpu/mgs_asset_cache_vulkan.h"
#include "runtime/metrics.h"
#include <cstring>
#include <fmt/format.h>
#include <stdexcept>

namespace ps1::gpu {
namespace {
void checked(VkResult status, const char *what) {
  if (status != VK_SUCCESS) throw std::runtime_error(fmt::format("MGS GPU cache {}: {}", what, int(status)));
}
struct Buffer {
  VkDevice device;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void *mapped = nullptr;
  explicit Buffer(VkDevice d) : device(d) {}
  ~Buffer() {
    if (mapped) vkUnmapMemory(device, memory);
    if (buffer) vkDestroyBuffer(device, buffer, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
  }
  void create(VkPhysicalDevice physical, VkDeviceSize size, VkBufferUsageFlags usage,
              VkMemoryPropertyFlags properties) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    checked(vkCreateBuffer(device, &info, nullptr, &buffer), "create buffer");
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties types;
    vkGetPhysicalDeviceMemoryProperties(physical, &types);
    uint32_t selected = ~0u;
    for (uint32_t i = 0; i < types.memoryTypeCount; ++i)
      if ((requirements.memoryTypeBits & (1u << i)) &&
          (types.memoryTypes[i].propertyFlags & properties) == properties) { selected = i; break; }
    if (selected == ~0u) throw std::runtime_error("MGS GPU cache memory type unavailable");
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = selected;
    checked(vkAllocateMemory(device, &allocate, nullptr, &memory), "allocate buffer");
    checked(vkBindBufferMemory(device, buffer, memory, 0), "bind buffer");
  }
  void map() { checked(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map buffer"); }
};
struct Command {
  VkDevice device;
  VkCommandPool pool;
  VkCommandBuffer buffer = VK_NULL_HANDLE;
  ~Command() { if (buffer) vkFreeCommandBuffers(device, pool, 1, &buffer); }
};
}
MgsAssetGpuCache::~MgsAssetGpuCache() {
  if (buffer_) vkDestroyBuffer(device_, buffer_, nullptr);
  if (memory_) vkFreeMemory(device_, memory_, nullptr);
}
std::unique_ptr<MgsAssetGpuCache> MgsAssetGpuCache::upload(VkPhysicalDevice physical,
    VkDevice device, VkQueue queue, VkCommandPool pool,
    games::mgs::AssetCache cache, bool verifyReadback) {
  const VkDeviceSize size = cache.bytes().size();
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(physical, &props);
  if (size > props.limits.maxStorageBufferRange)
    throw std::runtime_error("MGS GPU cache exceeds device storage-buffer range");
  auto result = std::unique_ptr<MgsAssetGpuCache>(new MgsAssetGpuCache);
  result->device_ = device;
  Buffer staging(device), storage(device), readback(device);
  staging.create(physical, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  staging.map();
  std::memcpy(staging.mapped, cache.bytes().data(), size);
  storage.create(physical, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (verifyReadback) {
    readback.create(physical, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    readback.map();
  }
  Command command{device, pool};
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  checked(vkAllocateCommandBuffers(device, &allocate, &command.buffer), "allocate command");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  checked(vkBeginCommandBuffer(command.buffer, &begin), "begin upload");
  VkBufferCopy copy{0, 0, size};
  vkCmdCopyBuffer(command.buffer, staging.buffer, storage.buffer, 1, &copy);
  VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = storage.buffer;
  barrier.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(command.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 0, nullptr, 1, &barrier, 0, nullptr);
  if (verifyReadback) {
    vkCmdCopyBuffer(command.buffer, storage.buffer, readback.buffer, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.buffer = readback.buffer;
    vkCmdPipelineBarrier(command.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
  }
  checked(vkEndCommandBuffer(command.buffer), "end upload");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command.buffer;
  checked(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit upload");
  checked(vkQueueWaitIdle(queue), "wait upload");
  if (verifyReadback && std::memcmp(readback.mapped, cache.bytes().data(), size) != 0)
    throw std::runtime_error("MGS GPU cache readback differs from original upload");
  result->buffer_ = storage.buffer;
  result->memory_ = storage.memory;
  storage.buffer = VK_NULL_HANDLE;
  storage.memory = VK_NULL_HANDLE;
  result->cache_ = std::move(cache);
  metrics::count("mgs.asset_cache.uploads");
  metrics::setState("mgs.asset_cache.device_bytes", size);
  metrics::setState("mgs.asset_cache.readback_verified", verifyReadback);
  fmt::print("[MGS assets] {} assets, {} model parts, {} texture variants; {} bytes resident in Vulkan{}\n",
      result->cache_.section(games::mgs::AssetCache::Assets).count,
      result->cache_.section(games::mgs::AssetCache::Models).count,
      result->cache_.section(games::mgs::AssetCache::Textures).count, size,
      verifyReadback ? ", readback verified" : "");
  return result;
}
} // namespace ps1::gpu
