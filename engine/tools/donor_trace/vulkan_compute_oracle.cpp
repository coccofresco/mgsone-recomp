// Reusable diagnostic runner: one in-place std430 storage buffer, one compute
// shader, one dispatch. Binary stdin/stdout; diagnostics only on stderr.
// No game, renderer, surface/window, descriptor convention beyond binding 0,
// or donor dependency. Not linked into the runtime.
#include <vulkan/vulkan.h>

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
void check(VkResult result) {
  if (result != VK_SUCCESS)
    throw std::runtime_error("Vulkan error " + std::to_string(result));
}
struct Context {
  VkInstance instance{};
  VkDevice device{};
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  VkShaderModule shader{};
  VkDescriptorSetLayout setLayout{};
  VkDescriptorPool descriptors{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{};
  VkCommandPool commands{};
  void *mapped = nullptr;
  ~Context() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (mapped)
        vkUnmapMemory(device, memory);
      vkDestroyCommandPool(device, commands, nullptr);
      vkDestroyPipeline(device, pipeline, nullptr);
      vkDestroyPipelineLayout(device, layout, nullptr);
      vkDestroyDescriptorPool(device, descriptors, nullptr);
      vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
      vkDestroyShaderModule(device, shader, nullptr);
      vkDestroyBuffer(device, buffer, nullptr);
      vkFreeMemory(device, memory, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (instance)
      vkDestroyInstance(instance, nullptr);
  }
};
} // namespace

int main(int argc, char **argv) try {
  static_assert(std::endian::native == std::endian::little);
  if (argc != 3) {
    std::fprintf(stderr,
                 "usage: vulkan_compute_oracle shader.spv work-items\n");
    return 2;
  }
  char *end = nullptr;
  const unsigned long workItems = std::strtoul(argv[2], &end, 10);
  if (*argv[2] == '\0' || *end || workItems == 0 || workItems > 4194240)
    throw std::runtime_error("invalid work-item count");
  std::vector<char> bytes;
  std::array<char, 4096> chunk{};
  while (const auto count = std::fread(chunk.data(), 1, chunk.size(), stdin)) {
    bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
    if (bytes.size() > 64u * 1024u * 1024u)
      throw std::runtime_error("input exceeds 64 MiB");
  }
  if (std::ferror(stdin) || bytes.empty() || bytes.size() % 4u)
    throw std::runtime_error("invalid input buffer");
  std::ifstream codeFile(argv[1], std::ios::binary | std::ios::ate);
  const auto codeBytes = codeFile.tellg();
  if (!codeFile || codeBytes <= 0 || codeBytes % 4 != 0 || codeBytes > 1048576)
    throw std::runtime_error("invalid SPIR-V file");
  std::vector<uint32_t> code(static_cast<size_t>(codeBytes) / 4);
  codeFile.seekg(0);
  if (!codeFile.read(reinterpret_cast<char *>(code.data()), codeBytes))
    throw std::runtime_error("truncated SPIR-V file");
  Context c;
  VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  check(vkCreateInstance(&instanceInfo, nullptr, &c.instance));
  uint32_t count = 0;
  check(vkEnumeratePhysicalDevices(c.instance, &count, nullptr));
  std::vector<VkPhysicalDevice> devices(count);
  check(vkEnumeratePhysicalDevices(c.instance, &count, devices.data()));
  VkPhysicalDevice physical{};
  uint32_t family = 0;
  for (auto candidate : devices) {
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(candidate, &features);
    if (!features.shaderInt64)
      continue;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count,
                                             families.data());
    for (uint32_t index = 0; index < count; ++index)
      if (families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) {
        physical = candidate;
        family = index;
        break;
      }
    if (physical)
      break;
  }
  if (!physical)
    throw std::runtime_error("no compute device with shaderInt64");
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physical, &properties);
  if (bytes.size() > properties.limits.maxStorageBufferRange ||
      (workItems + 63) / 64 > properties.limits.maxComputeWorkGroupCount[0])
    throw std::runtime_error("device limits exceeded");
  std::fprintf(stderr, "compute device: %s\n", properties.deviceName);
  const float priority = 1;
  VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queueInfo.queueFamilyIndex = family;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &priority;
  VkPhysicalDeviceFeatures features{};
  features.shaderInt64 = VK_TRUE;
  VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  deviceInfo.queueCreateInfoCount = 1;
  deviceInfo.pQueueCreateInfos = &queueInfo;
  deviceInfo.pEnabledFeatures = &features;
  check(vkCreateDevice(physical, &deviceInfo, nullptr, &c.device));
  VkQueue queue{};
  vkGetDeviceQueue(c.device, family, 0, &queue);
  VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bufferInfo.size = bytes.size();
  bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  check(vkCreateBuffer(c.device, &bufferInfo, nullptr, &c.buffer));
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(c.device, c.buffer, &requirements);
  VkPhysicalDeviceMemoryProperties memoryProperties{};
  vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
  uint32_t memoryType = UINT32_MAX;
  for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
    if ((requirements.memoryTypeBits & (1u << index)) &&
        (memoryProperties.memoryTypes[index].propertyFlags &
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      memoryType = index;
      break;
    }
  if (memoryType == UINT32_MAX)
    throw std::runtime_error("no coherent host memory");
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = memoryType;
  check(vkAllocateMemory(c.device, &allocate, nullptr, &c.memory));
  check(vkBindBufferMemory(c.device, c.buffer, c.memory, 0));
  check(vkMapMemory(c.device, c.memory, 0, bytes.size(), 0, &c.mapped));
  std::memcpy(c.mapped, bytes.data(), bytes.size());
  VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo setInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  setInfo.bindingCount = 1;
  setInfo.pBindings = &binding;
  check(vkCreateDescriptorSetLayout(c.device, &setInfo, nullptr, &c.setLayout));
  VkPipelineLayoutCreateInfo layoutInfo{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.setLayoutCount = 1;
  layoutInfo.pSetLayouts = &c.setLayout;
  check(vkCreatePipelineLayout(c.device, &layoutInfo, nullptr, &c.layout));
  VkShaderModuleCreateInfo shaderInfo{
      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  shaderInfo.codeSize = code.size() * 4;
  shaderInfo.pCode = code.data();
  check(vkCreateShaderModule(c.device, &shaderInfo, nullptr, &c.shader));
  VkComputePipelineCreateInfo pipelineInfo{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipelineInfo.stage.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipelineInfo.stage.module = c.shader;
  pipelineInfo.stage.pName = "main";
  pipelineInfo.layout = c.layout;
  check(vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                 nullptr, &c.pipeline));
  VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo poolInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  poolInfo.maxSets = poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes = &poolSize;
  check(vkCreateDescriptorPool(c.device, &poolInfo, nullptr, &c.descriptors));
  VkDescriptorSetAllocateInfo setAllocate{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  setAllocate.descriptorPool = c.descriptors;
  setAllocate.descriptorSetCount = 1;
  setAllocate.pSetLayouts = &c.setLayout;
  VkDescriptorSet set{};
  check(vkAllocateDescriptorSets(c.device, &setAllocate, &set));
  VkDescriptorBufferInfo bufferDescriptor{c.buffer, 0, bytes.size()};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &bufferDescriptor;
  vkUpdateDescriptorSets(c.device, 1, &write, 0, nullptr);
  VkCommandPoolCreateInfo commandsInfo{
      VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  commandsInfo.queueFamilyIndex = family;
  check(vkCreateCommandPool(c.device, &commandsInfo, nullptr, &c.commands));
  VkCommandBufferAllocateInfo commandAllocate{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  commandAllocate.commandPool = c.commands;
  commandAllocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  commandAllocate.commandBufferCount = 1;
  VkCommandBuffer commands{};
  check(vkAllocateCommandBuffers(c.device, &commandAllocate, &commands));
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  check(vkBeginCommandBuffer(commands, &begin));
  vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipeline);
  vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, c.layout, 0,
                          1, &set, 0, nullptr);
  vkCmdDispatch(commands, (workItems + 63) / 64, 1, 1);
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr,
                       0, nullptr);
  check(vkEndCommandBuffer(commands));
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &commands;
  check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
  check(vkQueueWaitIdle(queue));
  return std::fwrite(c.mapped, 1, bytes.size(), stdout) == bytes.size() ? 0 : 2;
} catch (const std::exception &error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 2;
}
