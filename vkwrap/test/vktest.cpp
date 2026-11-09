// vktest.cpp - XeSS' Vulkan path through vkwrap (vulkan-1.dll next to this program) and the layer: a flat colour in,
// low-resolution motion vectors and depth, 60 executes; the output must be that colour. Results in vktest.txt next to
// the program (Proton drops a console program's stdout); exit code 0 when passed.
// Needs libxess.dll next to it, WINEDLLOVERRIDES=vulkan-1=n,b and the layer (VK_LOADER_LAYERS_ENABLE=VK_LAYER_XESS_xmx).
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../ffxfg/inc/xess/xess/xess.h"
#include "../../ffxfg/inc/xess/xess/xess_vk.h"

#define VKF(X)                                                                                                         \
   X(CreateInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceQueueFamilyProperties) X(CreateDevice)            \
   X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceProperties)
#define VKD(X)                                                                                                         \
   X(GetDeviceQueue) X(CreateImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(BindImageMemory)                \
   X(CreateImageView) X(CreateBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory)                 \
   X(CreateCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier)     \
   X(CmdClearColorImage) X(CmdCopyImageToBuffer) X(QueueSubmit) X(QueueWaitIdle) X(ResetCommandBuffer)

static PFN_vkGetInstanceProcAddr gipa;
#define X(n) static PFN_vk##n vk##n;
VKF(X) VKD(X)
#undef X

#define CHECK(x)                                                                                                       \
   do {                                                                                                                \
      VkResult r_ = (x);                                                                                               \
      if (r_ != VK_SUCCESS) {                                                                                          \
         printf("FAIL %s: %d\n", #x, (int)r_);                                                                         \
         return 1;                                                                                                     \
      }                                                                                                                \
   } while (0)
#define XCHECK(x)                                                                                                      \
   do {                                                                                                                \
      xess_result_t r_ = (x);                                                                                          \
      if (r_ != XESS_RESULT_SUCCESS) {                                                                                 \
         printf("FAIL %s: %d\n", #x, (int)r_);                                                                         \
         return 1;                                                                                                     \
      }                                                                                                                \
      printf("ok %s\n", #x);                                                                                           \
   } while (0)

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties memp;

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memp.memoryTypeCount; i++)
      if ((bits >> i & 1) && (memp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   return 0;
}

struct Img {
   VkImage image;
   VkImageView view;
   VkFormat format;
   uint32_t w, h;
};

static bool make_image(Img &m, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage)
{
   m.w = w;
   m.h = h;
   m.format = f;
   VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
   ci.imageType = VK_IMAGE_TYPE_2D;
   ci.format = f;
   ci.extent = {w, h, 1};
   ci.mipLevels = 1;
   ci.arrayLayers = 1;
   ci.samples = VK_SAMPLE_COUNT_1_BIT;
   ci.tiling = VK_IMAGE_TILING_OPTIMAL;
   ci.usage = usage;
   if (vkCreateImage(dev, &ci, nullptr, &m.image))
      return false;
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, m.image, &mr);
   VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size,
                              memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
   VkDeviceMemory mem;
   if (vkAllocateMemory(dev, &ai, nullptr, &mem) || vkBindImageMemory(dev, m.image, mem, 0))
      return false;
   VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
   vi.image = m.image;
   vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
   vi.format = f;
   vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   return vkCreateImageView(dev, &vi, nullptr, &m.view) == VK_SUCCESS;
}

static xess_vk_image_view_info info(const Img &m)
{
   xess_vk_image_view_info i = {};
   i.imageView = m.view;
   i.image = m.image;
   i.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   i.format = m.format;
   i.width = m.w;
   i.height = m.h;
   return i;
}

static void barrier(VkCommandBuffer cb, VkImage im, VkImageLayout from, VkImageLayout to, VkAccessFlags sa,
                    VkAccessFlags da)
{
   VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
   b.srcAccessMask = sa;
   b.dstAccessMask = da;
   b.oldLayout = from;
   b.newLayout = to;
   b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.image = im;
   b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                        nullptr, 1, &b);
}

int main()
{
   freopen("vktest.txt", "w", stdout);
   setvbuf(stdout, nullptr, _IONBF, 0);
   HMODULE vk = LoadLibraryA("vulkan-1.dll");
   HMODULE xm = LoadLibraryA("libxess.dll");
   if (!vk || !xm) {
      printf("FAIL vulkan-1.dll %p libxess.dll %p\n", (void *)vk, (void *)xm);
      return 1;
   }
   gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
#define XL(n) auto n = (decltype(&::n))GetProcAddress(xm, #n);
   XL(xessVKGetRequiredInstanceExtensions)
   XL(xessVKGetRequiredDeviceExtensions)
   XL(xessVKGetRequiredDeviceFeatures)
   XL(xessVKCreateContext)
   XL(xessVKInit)
   XL(xessVKExecute)
   XL(xessGetInputResolution)
   XL(xessDestroyContext)
#undef XL
   vkCreateInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");

   uint32_t nie = 0, api = 0;
   const char *const *ie = nullptr;
   XCHECK(xessVKGetRequiredInstanceExtensions(&nie, &ie, &api));
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "vktest", 1, "vktest", 1,
                            api > VK_API_VERSION_1_3 ? api : VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, nie, ie};
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, nullptr, &inst));
#define X(n) vk##n = (PFN_vk##n)gipa(inst, "vk" #n);
   VKF(X)
#undef X
   uint32_t np = 1;
   VkPhysicalDevice phys;
   vkEnumeratePhysicalDevices(inst, &np, &phys);
   vkGetPhysicalDeviceMemoryProperties(phys, &memp);
   uint32_t nde = 0;
   const char *const *de = nullptr;
   XCHECK(xessVKGetRequiredDeviceExtensions(inst, phys, &nde, &de));
   VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
   void *features = &f2;
   XCHECK(xessVKGetRequiredDeviceFeatures(inst, phys, &features));
   uint32_t nq = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
   std::vector<VkQueueFamilyProperties> qf(nq);
   vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf.data());
   uint32_t fam = 0;
   while (fam < nq && !(qf[fam].queueFlags & VK_QUEUE_COMPUTE_BIT && qf[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      fam++;
   const float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, fam, 1, &prio};
   VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, features, 0, 1, &qci, 0, nullptr, nde, de, nullptr};
   CHECK(vkCreateDevice(phys, &dci, nullptr, &dev));
   auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define X(n) vk##n = (PFN_vk##n)gdpa(dev, "vk" #n);
   VKD(X)
#undef X
   VkQueue q;
   vkGetDeviceQueue(dev, fam, 0, &q);

   xess_context_handle_t x = nullptr;
   XCHECK(xessVKCreateContext(inst, phys, dev, &x));
   const xess_2d_t out = {1920, 1080};
   xess_2d_t in = {};
   XCHECK(xessGetInputResolution(x, &out, XESS_QUALITY_SETTING_PERFORMANCE, &in));
   xess_vk_init_params_t ip = {};
   ip.outputResolution = out;
   ip.qualitySetting = XESS_QUALITY_SETTING_PERFORMANCE;
   ip.initFlags = XESS_INIT_FLAG_NONE;
   XCHECK(xessVKInit(x, &ip));

   Img col, mv, dep, o;
   const VkImageUsageFlags in_use = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   if (!make_image(col, in.x, in.y, VK_FORMAT_R8G8B8A8_UNORM, in_use) ||
       !make_image(mv, in.x, in.y, VK_FORMAT_R16G16_SFLOAT, in_use) ||
       !make_image(dep, in.x, in.y, VK_FORMAT_R32_SFLOAT, in_use) ||
       !make_image(o, out.x, out.y, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      printf("FAIL images\n");
      return 1;
   }
   VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, (VkDeviceSize)out.x * out.y * 4,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   VkBuffer rb;
   CHECK(vkCreateBuffer(dev, &bci, nullptr, &rb));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, rb, &mr);
   VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size,
                               memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   VkDeviceMemory rbm;
   CHECK(vkAllocateMemory(dev, &mai, nullptr, &rbm));
   CHECK(vkBindBufferMemory(dev, rb, rbm, 0));

   VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                  VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, fam};
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
   VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                      VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
   VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cb};

   // inputs: a flat colour (R c0 G 80 B 30), zero motion, depth 0.5; output cleared to black
   CHECK(vkBeginCommandBuffer(cb, &bi));
   const VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   const Img *ins[] = {&col, &mv, &dep};
   VkClearColorValue cv[3] = {};
   cv[0].float32[0] = 0xc0 / 255.0f;
   cv[0].float32[1] = 0x80 / 255.0f;
   cv[0].float32[2] = 0x30 / 255.0f;
   cv[0].float32[3] = 1.0f;
   cv[2].float32[0] = 0.5f;
   for (int i = 0; i < 3; i++) {
      barrier(cb, ins[i]->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
              VK_ACCESS_TRANSFER_WRITE_BIT);
      vkCmdClearColorImage(cb, ins[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv[i], 1, &rng);
      barrier(cb, ins[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
   }
   VkClearColorValue black = {};
   barrier(cb, o.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
   vkCmdClearColorImage(cb, o.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rng);
   barrier(cb, o.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
   CHECK(vkEndCommandBuffer(cb));
   CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   static const float jit[8][2] = {{0.25f, -0.25f}, {-0.25f, 0.25f}, {0.125f, 0.375f}, {-0.375f, -0.125f},
                                   {0.375f, 0.125f}, {-0.125f, -0.375f}, {0.0625f, -0.4375f}, {-0.4375f, 0.0625f}};
   for (int f = 0; f < 60; f++) {
      vkResetCommandBuffer(cb, 0);
      CHECK(vkBeginCommandBuffer(cb, &bi));
      xess_vk_execute_params_t e = {};
      e.colorTexture = info(col);
      e.velocityTexture = info(mv);
      e.depthTexture = info(dep);
      e.outputTexture = info(o);
      e.jitterOffsetX = jit[f % 8][0];
      e.jitterOffsetY = jit[f % 8][1];
      e.exposureScale = 1.0f;
      e.resetHistory = f == 0;
      e.inputWidth = in.x;
      e.inputHeight = in.y;
      xess_result_t r = xessVKExecute(x, cb, &e);
      if (r != XESS_RESULT_SUCCESS) {
         printf("FAIL xessVKExecute frame %d: %d\n", f, (int)r);
         return 1;
      }
      barrier(cb, o.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
      if (f == 59) {
         VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {out.x, out.y, 1}};
         vkCmdCopyImageToBuffer(cb, o.image, VK_IMAGE_LAYOUT_GENERAL, rb, 1, &c);
      }
      CHECK(vkEndCommandBuffer(cb));
      CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
      CHECK(vkQueueWaitIdle(q));
   }
   printf("ok 60 executes\n");

   void *p = nullptr;
   CHECK(vkMapMemory(dev, rbm, 0, VK_WHOLE_SIZE, 0, &p));
   const uint32_t colour = 0xff3080c0;
   int bad = 0, n = 0;
   uint32_t worst = 0;
   for (uint32_t y = 8; y < out.y; y += 97)
      for (uint32_t xx = 8; xx < out.x; xx += 113) {
         const uint32_t v = ((const uint32_t *)p)[y * out.x + xx];
         n++;
         for (int ch = 0; ch < 3; ch++)
            if (abs((int)((v >> (8 * ch)) & 255) - (int)((colour >> (8 * ch)) & 255)) > 3) {
               bad++;
               worst = v;
               break;
            }
      }
   const uint32_t centre = ((const uint32_t *)p)[(out.y / 2) * out.x + out.x / 2];
   printf("%s output: %d of %d samples off the input colour 0x%08x (centre 0x%08x)\n", bad ? "FAIL" : "ok", bad, n,
          colour, centre);
   if (bad)
      printf("   e.g. 0x%08x\n", worst);
   xessDestroyContext(x);
   printf(bad ? "FAILED\n" : "PASS\n");
   return bad ? 1 : 0;
}
