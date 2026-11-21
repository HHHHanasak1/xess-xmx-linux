#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

static void check(VkResult r, const char *what) {
  if (r != VK_SUCCESS) {
    std::fprintf(stderr, "%s failed: %d\n", what, r);
    std::exit(2);
  }
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instanceInfo.pApplicationInfo = &app;
  VkInstance instance;
  check(vkCreateInstance(&instanceInfo, nullptr, &instance), "instance");
  uint32_t n = 0;
  check(vkEnumeratePhysicalDevices(instance, &n, nullptr), "device count");
  std::vector<VkPhysicalDevice> physicals(n);
  check(vkEnumeratePhysicalDevices(instance, &n, physicals.data()), "devices");
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  for (auto p : physicals) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p, &props);
    if (props.vendorID == 0x8086 && props.deviceID >= 0x5690 &&
        props.deviceID <= 0x56ff) {
      physical = p;
      std::printf("Device: %s\n", props.deviceName);
    }
  }
  if (!physical)
    return 3;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
  std::vector<VkQueueFamilyProperties> families(n);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, families.data());
  uint32_t family = UINT32_MAX;
  for (uint32_t i = 0; i < n; i++)
    if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
        !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
      family = i;
      break;
    }
  if (family == UINT32_MAX) {
    std::fprintf(stderr,
                 "A dedicated compute family is required for this test\n");
    return 3;
  }
  std::printf("Dedicated compute family: %u\n", family);
  float priority = 1;
  VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queueInfo.queueFamilyIndex = family;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &priority;
  VkPhysicalDeviceVulkan13Features f13{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceFeatures2 features{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.pNext = &f13;
  vkGetPhysicalDeviceFeatures2(physical, &features);
  VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  deviceInfo.pNext = &features;
  deviceInfo.queueCreateInfoCount = 1;
  deviceInfo.pQueueCreateInfos = &queueInfo;
  VkDevice device;
  check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "device");
  VkQueue queue;
  vkGetDeviceQueue(device, family, 0, &queue);
  VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bufferInfo.size = 65536;
  bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  VkBuffer buffer;
  check(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer), "buffer");
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(device, buffer, &requirements);
  VkPhysicalDeviceMemoryProperties memoryProperties;
  vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
  uint32_t memoryType = UINT32_MAX;
  for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++)
    if ((requirements.memoryTypeBits & (1u << i)) &&
        ((memoryProperties.memoryTypes[i].propertyFlags &
          (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
      memoryType = i;
      break;
    }
  if (memoryType == UINT32_MAX)
    return 3;
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = memoryType;
  VkDeviceMemory memory;
  check(vkAllocateMemory(device, &allocation, nullptr, &memory), "allocate");
  check(vkBindBufferMemory(device, buffer, memory, 0), "bind");
  void *mapping;
  check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapping), "map");
  auto words = static_cast<uint32_t *>(mapping);
  words[15360] = 8;
  words[15361] = 1;
  words[15362] = 1;
  VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo setInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  setInfo.bindingCount = 1;
  setInfo.pBindings = &binding;
  VkDescriptorSetLayout setLayout;
  check(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout),
        "set layout");
  VkPipelineLayoutCreateInfo layoutInfo{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.setLayoutCount = 1;
  layoutInfo.pSetLayouts = &setLayout;
  VkPipelineLayout layout;
  check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout),
        "pipeline layout");
  VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo poolInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes = &poolSize;
  VkDescriptorPool pool;
  check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool),
        "descriptor pool");
  VkDescriptorSetAllocateInfo setAllocation{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  setAllocation.descriptorPool = pool;
  setAllocation.descriptorSetCount = 1;
  setAllocation.pSetLayouts = &setLayout;
  VkDescriptorSet set;
  check(vkAllocateDescriptorSets(device, &setAllocation, &set), "allocate set");
  VkDescriptorBufferInfo descriptorBuffer{buffer, 0, 65536};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &descriptorBuffer;
  vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
  if (!input)
    return 3;
  std::vector<uint32_t> spirv(size_t(input.tellg()) / 4);
  input.seekg(0);
  input.read(reinterpret_cast<char *>(spirv.data()), spirv.size() * 4);
  VkShaderModuleCreateInfo moduleInfo{
      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  moduleInfo.codeSize = spirv.size() * 4;
  moduleInfo.pCode = spirv.data();
  VkShaderModule module;
  check(vkCreateShaderModule(device, &moduleInfo, nullptr, &module), "module");
  VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
  subgroup.requiredSubgroupSize = 32;
  VkComputePipelineCreateInfo pipelineInfo{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipelineInfo.layout = layout;
  pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipelineInfo.stage.module = module;
  pipelineInfo.stage.pName = "main";
  pipelineInfo.stage.pNext = &subgroup;
  VkPipeline pipeline;
  check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                 nullptr, &pipeline),
        "pipeline");
  VkCommandPoolCreateInfo commandPoolInfo{
      VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  commandPoolInfo.queueFamilyIndex = family;
  VkCommandPool commandPool;
  check(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool),
        "command pool");
  VkCommandBufferAllocateInfo commandAllocation{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  commandAllocation.commandPool = commandPool;
  commandAllocation.commandBufferCount = 1;
  commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  VkCommandBuffer primary, secondary;
  check(vkAllocateCommandBuffers(device, &commandAllocation, &primary),
        "primary");
  commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
  check(vkAllocateCommandBuffers(device, &commandAllocation, &secondary),
        "secondary");
  auto bind = [&](VkCommandBuffer command) {
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0,
                            1, &set, 0, nullptr);
  };
  auto barrier = [&](VkCommandBuffer command) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
  };
  VkCommandBufferInheritanceInfo inheritance{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.pInheritanceInfo = &inheritance;
  check(vkBeginCommandBuffer(secondary, &begin), "begin secondary");
  bind(secondary);
  vkCmdDispatch(secondary, 8, 1, 1);
  check(vkEndCommandBuffer(secondary), "end secondary");
  begin.pInheritanceInfo = nullptr;
  check(vkBeginCommandBuffer(primary, &begin), "begin primary");
  vkCmdFillBuffer(primary, buffer, 0, 1024, 0xdeadbeef);
  barrier(primary);
  bind(primary);
  vkCmdDispatch(primary, 8, 1, 1);
  barrier(primary);
  vkCmdFillBuffer(primary, buffer, 0, 1024, 0xdeadbeef);
  barrier(primary);
  vkCmdExecuteCommands(primary, 1, &secondary);
  barrier(primary);
  vkCmdFillBuffer(primary, buffer, 0, 1024, 0xdeadbeef);
  barrier(primary);
  bind(primary);
  vkCmdDispatchIndirect(primary, buffer, 61440);
  VkMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  hostBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(primary, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostBarrier, 0,
                       nullptr, 0, nullptr);
  check(vkEndCommandBuffer(primary), "end primary");
  VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "fence");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &primary;
  for (unsigned repeat = 0; repeat < 16; repeat++) {
    for (uint32_t j = 0; j < 48; j++)
      for (uint32_t i = 0; i < 256; i++) {
        uint32_t k = 256 + i + j * 256;
        words[k] = (k * 22695477u + 1u) ^ 0x9e3779b9u;
      }
    check(vkResetFences(device, 1, &fence), "reset fence");
    check(vkQueueSubmit(queue, 1, &submit, fence), "submit");
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull),
          "wait 5s");
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t expected = 0;
      uint32_t neighbor = 256 + (i ^ 1u);
      uint32_t q = ((neighbor * 22695477u + 1u) ^ 0x9e3779b9u) ^ 0x13579bdfu;
      for (uint32_t j = 0; j < 48; j++) {
        uint32_t k = 256 + i + j * 256;
        uint32_t v = (k * 22695477u + 1u) ^ 0x9e3779b9u;
        expected ^= v * (q ^ (0x9e3779b9u + j * 0x45d9f3bu));
      }
      if (words[i] != expected) {
        std::fprintf(stderr,
                     "Mismatch repeat=%u index=%u actual=%08x expected=%08x\n",
                     repeat, i, words[i], expected);
        return 4;
      }
    }
  }
  std::printf(
      "PASS: 16 submissions, 48 dispatches, 4096 checked output words; direct, "
      "indirect, secondary, BLORP fills and mode transitions\n");
  vkDestroyFence(device, fence, nullptr);
  vkDestroyCommandPool(device, commandPool, nullptr);
  vkDestroyPipeline(device, pipeline, nullptr);
  vkDestroyShaderModule(device, module, nullptr);
  vkDestroyDescriptorPool(device, pool, nullptr);
  vkDestroyPipelineLayout(device, layout, nullptr);
  vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
  vkDestroyBuffer(device, buffer, nullptr);
  vkUnmapMemory(device, memory);
  vkFreeMemory(device, memory, nullptr);
  vkDestroyDevice(device, nullptr);
  vkDestroyInstance(instance, nullptr);
}
