/* probe.c - device check of the layer's pieces on an unpatched ANV (no GPU work is submitted):
 *   1. the layer's template (set 7: 32 MUTABLE surfaces + a sampler) exported as a shader object: parses, writes back
 *      byte-identical, and has an entry for every binding;
 *   2. the same template as a pipeline binary (VK_KHR_pipeline_binary): which leading bytes and alignment base make
 *      its data round-trip, and whether its payload equals the shader object's;
 *   3. real kernels through the layer's path (compile with -noLoadPayload, CML1, C prologue, template): the driver
 *      must accept the binary as a shader object and as a pipeline binary, and export it back unchanged.
 * probe <template.spv> <kernel folder> <dyn id>...      (environment as for the layer's compile: ANV_CM_HELPER,
 * XMX_IGC_DIR, XDG_CACHE_HOME, CM_LOG). Prints counts, sizes and results only. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/anvbin.h"
#include "../src/cmk.h"
#include "../src/compile.h"
#include "../src/gens.h"
#include "../src/layouts.h"
#include "../src/placeholder.h"
#include "../src/prologue.h"

static uint32_t LAYER_SET = 7;   /* PROBE_VKD3D=1: 2, after two vkd3d-proton-like heap sets */
#define NSURF_BINDINGS 32
#define SAMPLER_BINDING 32

#define CHECK_VK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static uint32_t dev_id;   /* PCI device id: the generation (gens.h) decides the GRF cap and the prologue */
#define FN(n) static PFN_##n n
FN(vkCreateInstance); FN(vkEnumeratePhysicalDevices); FN(vkGetPhysicalDeviceProperties2); FN(vkGetDeviceProcAddr);
FN(vkCreateDevice); FN(vkGetPhysicalDeviceFeatures2); FN(vkCreateDescriptorSetLayout); FN(vkCreatePipelineLayout);
FN(vkCreateShadersEXT); FN(vkGetShaderBinaryDataEXT); FN(vkDestroyShaderEXT); FN(vkCreateComputePipelines);
FN(vkDestroyPipeline); FN(vkCreatePipelineBinariesKHR); FN(vkGetPipelineBinaryDataKHR); FN(vkDestroyPipelineBinaryKHR);
FN(vkCreateShaderModule); FN(vkGetPipelineKeyKHR); FN(vkCreateSampler);

static uint8_t *
rd(const char *p, size_t *n)
{
   FILE *f = fopen(p, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint8_t *b = malloc(sz ? sz : 1);
   *n = fread(b, 1, sz, f);
   fclose(f);
   return b;
}

static VkDescriptorSetLayout set_layouts[8];
static const struct anv_layout *LT;   /* this driver's layout table */
static VkPipelineLayout pipe_layout;
static VkSampler sampler;

static void
init(void)
{
   void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
   if (!lib) { printf("FAIL no libvulkan\n"); exit(1); }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define LI(n) n = (PFN_##n)gipa(inst, #n)
   LI(vkCreateInstance);
   /* PROBE_ENGINE / PROBE_APP: the names the driver sees (its per-application settings key on them) */
   VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, getenv("PROBE_APP") ? getenv("PROBE_APP") : "xmx-probe",
                            1, getenv("PROBE_ENGINE"), getenv("PROBE_ENGINE_VER") ? atoi(getenv("PROBE_ENGINE_VER")) : 0,
                            VK_API_VERSION_1_3 };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai, 0, 0, 0, 0 };
   CHECK_VK(vkCreateInstance(&ici, 0, &inst));
   LI(vkEnumeratePhysicalDevices); LI(vkGetPhysicalDeviceProperties2); LI(vkGetDeviceProcAddr); LI(vkCreateDevice);
   LI(vkGetPhysicalDeviceFeatures2);
   uint32_t n = 1;
   vkEnumeratePhysicalDevices(inst, &n, &pd);
   VkPhysicalDeviceDriverProperties drv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
   VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv };
   vkGetPhysicalDeviceProperties2(pd, &p2);
   printf("device %s (0x%04x), driver %s %s\n", p2.properties.deviceName, p2.properties.deviceID, drv.driverName,
          drv.driverInfo);
   dev_id = p2.properties.deviceID;
   LT = anv_layout_for(drv.driverInfo);
   printf("layout table: %s\n", LT ? LT->version : "none");
   VkPhysicalDevicePipelineBinaryFeaturesKHR pbf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR };
   VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mdf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, &pbf };
   VkPhysicalDeviceShaderObjectFeaturesEXT sof = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, &mdf };
   VkPhysicalDeviceMaintenance5FeaturesKHR m5 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, &sof };
   VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &m5 };
   vkGetPhysicalDeviceFeatures2(pd, &f2);
   printf("features: pipelineBinaries %u shaderObject %u mutableDescriptorType %u maintenance5 %u\n",
          pbf.pipelineBinaries, sof.shaderObject, mdf.mutableDescriptorType, m5.maintenance5);
   if (!pbf.pipelineBinaries || !sof.shaderObject || !mdf.mutableDescriptorType || !m5.maintenance5) {
      printf("FAIL a needed feature is missing\n");
      exit(1);
   }
   const char *exts[] = { "VK_KHR_pipeline_binary", "VK_EXT_shader_object", "VK_EXT_mutable_descriptor_type",
                          "VK_KHR_maintenance5" };
   float prio = 1;
   VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
   VkPhysicalDevicePipelineBinaryFeaturesKHR pbf_on = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, 0, 1 };
   VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mdf_on = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, &pbf_on, 1 };
   VkPhysicalDeviceShaderObjectFeaturesEXT sof_on = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, &mdf_on, 1 };
   VkPhysicalDeviceMaintenance5FeaturesKHR m5_on = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, &sof_on, 1 };
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &m5_on, 0, 1, &q, 0, 0, 4, exts, 0 };
   CHECK_VK(vkCreateDevice(pd, &dci, 0, &dev));
#define LD(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
   LD(vkCreateDescriptorSetLayout); LD(vkCreatePipelineLayout); LD(vkCreateShadersEXT); LD(vkGetShaderBinaryDataEXT);
   LD(vkDestroyShaderEXT); LD(vkCreateComputePipelines); LD(vkDestroyPipeline); LD(vkCreatePipelineBinariesKHR);
   LD(vkGetPipelineBinaryDataKHR); LD(vkDestroyPipelineBinaryKHR); LD(vkCreateShaderModule); LD(vkGetPipelineKeyKHR);
   LD(vkCreateSampler);

   /* sets 0..6 empty (vkd3d-proton's sets in the real layout), set 7 = the layer's */
   VkDescriptorSetLayoutCreateInfo empty = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
   if (getenv("PROBE_VKD3D") && atoi(getenv("PROBE_VKD3D"))) {
      /* like vkd3d-proton: set 0 the resource heap (one huge MUTABLE binding, update after bind), set 1 the sampler heap */
      LAYER_SET = 2;
      static const VkDescriptorType ht[] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER };
      VkMutableDescriptorTypeListEXT hl = { 4, ht };
      VkMutableDescriptorTypeCreateInfoEXT hm = { VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT, 0, 1, &hl };
      const VkDescriptorBindingFlags bf = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
      VkDescriptorSetLayoutBindingFlagsCreateInfo hf = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, &hm, 1, &bf };
      VkDescriptorSetLayoutBinding hb = { 0, VK_DESCRIPTOR_TYPE_MUTABLE_EXT, 500000, VK_SHADER_STAGE_ALL, 0 };
      VkDescriptorSetLayoutCreateInfo hci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &hf,
                                              VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 1, &hb };
      CHECK_VK(vkCreateDescriptorSetLayout(dev, &hci, 0, &set_layouts[0]));
      VkDescriptorSetLayoutBindingFlagsCreateInfo sf = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, 0, 1, &bf };
      VkDescriptorSetLayoutBinding sb = { 0, VK_DESCRIPTOR_TYPE_SAMPLER, 2048, VK_SHADER_STAGE_ALL, 0 };
      VkDescriptorSetLayoutCreateInfo sci2 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &sf,
                                               VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 1, &sb };
      CHECK_VK(vkCreateDescriptorSetLayout(dev, &sci2, 0, &set_layouts[1]));
   } else {
      for (uint32_t s = 0; s < LAYER_SET; s++)
         CHECK_VK(vkCreateDescriptorSetLayout(dev, &empty, 0, &set_layouts[s]));
   }
   VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, 0, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                               VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
   CHECK_VK(vkCreateSampler(dev, &sci, 0, &sampler));
   /* PROBE_TYPES=6 adds the buffer types to the mutable list, PROBE_UAB=1 makes the set update-after-bind (the two
    * ways vkd3d-proton's heap can differ from the plain layout) */
   static const VkDescriptorType mut_types[] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                 VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                                 VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
                                                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
   const uint32_t ntypes = getenv("PROBE_TYPES") && atoi(getenv("PROBE_TYPES")) == 6 ? 6 : 4;
   const VkDescriptorSetLayoutCreateFlags lflags = getenv("PROBE_UAB") && atoi(getenv("PROBE_UAB")) ?
      VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
   printf("layer set: %u mutable types, flags %#x\n", ntypes, lflags);
   VkMutableDescriptorTypeListEXT lists[NSURF_BINDINGS + 1];
   VkDescriptorSetLayoutBinding b[NSURF_BINDINGS + 1];
   for (int i = 0; i < NSURF_BINDINGS; i++) {
      b[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_MUTABLE_EXT, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0 };
      lists[i] = (VkMutableDescriptorTypeListEXT){ ntypes, mut_types };
   }
   b[NSURF_BINDINGS] = (VkDescriptorSetLayoutBinding){ SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                                                       VK_SHADER_STAGE_COMPUTE_BIT, 0 };
   lists[NSURF_BINDINGS] = (VkMutableDescriptorTypeListEXT){ 0, 0 };
   VkMutableDescriptorTypeCreateInfoEXT mci = { VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT, 0,
                                                NSURF_BINDINGS + 1, lists };
   VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &mci, lflags,
                                           NSURF_BINDINGS + 1, b };
   CHECK_VK(vkCreateDescriptorSetLayout(dev, &lci, 0, &set_layouts[LAYER_SET]));
   VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, LAYER_SET + 1, set_layouts };
   CHECK_VK(vkCreatePipelineLayout(dev, &plci, 0, &pipe_layout));
}

/* ---- shader object ---- */
static uint8_t *
so_export(VkShaderCodeTypeEXT type, const void *code, size_t n, size_t *out_n, VkResult *res)
{
   VkShaderCreateInfoEXT ci = { VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                type, n, code, "main", LAYER_SET + 1, set_layouts, 0, 0, 0 };
   VkShaderEXT sh;
   *res = vkCreateShadersEXT(dev, 1, &ci, 0, &sh);
   if (*res)
      return NULL;
   CHECK_VK(vkGetShaderBinaryDataEXT(dev, sh, out_n, 0));
   uint8_t *d = malloc(*out_n);
   CHECK_VK(vkGetShaderBinaryDataEXT(dev, sh, out_n, d));
   vkDestroyShaderEXT(dev, sh, 0);
   return d;
}

/* ---- pipeline binary ---- */
static VkPipeline
pipe_from_spirv(const uint32_t *spv, size_t n)
{
   VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, n, spv };
   VkPipelineCreateFlags2CreateInfoKHR f2 = { VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, 0,
                                              VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR };
   VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, &f2, 0,
                                        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &smci, 0,
                                          VK_SHADER_STAGE_COMPUTE_BIT, VK_NULL_HANDLE, "main", 0 },
                                        pipe_layout, VK_NULL_HANDLE, -1 };
   VkPipeline p;
   CHECK_VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, 0, &p));
   return p;
}

/* the one binary of a pipeline: key and data */
static uint8_t *
pb_data(VkPipeline p, VkPipelineBinaryKeyKHR *key, size_t *n)
{
   VkPipelineBinaryCreateInfoKHR bci = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, 0, 0, p, 0 };
   VkPipelineBinaryKHR bin[4];
   VkPipelineBinaryHandlesInfoKHR hi = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR, 0, 0, 0 };
   CHECK_VK(vkCreatePipelineBinariesKHR(dev, &bci, 0, &hi));
   if (hi.pipelineBinaryCount != 1) {
      printf("FAIL pipeline has %u binaries\n", hi.pipelineBinaryCount);
      exit(1);
   }
   hi.pPipelineBinaries = bin;
   CHECK_VK(vkCreatePipelineBinariesKHR(dev, &bci, 0, &hi));
   VkPipelineBinaryDataInfoKHR di = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR, 0, bin[0] };
   *key = (VkPipelineBinaryKeyKHR){ VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR };
   *n = 0;   /* the size query must not see a stale size */
   fflush(stdout);
   CHECK_VK(vkGetPipelineBinaryDataKHR(dev, &di, key, n, 0));
   uint8_t *d = malloc(*n);
   CHECK_VK(vkGetPipelineBinaryDataKHR(dev, &di, key, n, d));
   vkDestroyPipelineBinaryKHR(dev, bin[0], 0);
   return d;
}

/* a pipeline from (key, data); VK_SUCCESS or the error */
static VkResult
pipe_from_binary(const VkPipelineBinaryKeyKHR *key, const uint8_t *d, size_t n, VkPipeline *p)
{
   VkPipelineBinaryDataKHR bd = { n, (void *)d };
   VkPipelineBinaryKeysAndDataKHR kd = { 1, key, &bd };
   VkPipelineBinaryCreateInfoKHR bci = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, 0, &kd, 0, 0 };
   VkPipelineBinaryKHR bin;
   VkPipelineBinaryHandlesInfoKHR hi = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR, 0, 1, &bin };
   VkResult r = vkCreatePipelineBinariesKHR(dev, &bci, 0, &hi);
   if (r)
      return r;
   VkPipelineBinaryInfoKHR pbi = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, 0, 1, &bin };
   VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, &pbi, 0,
                                        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0,
                                          VK_SHADER_STAGE_COMPUTE_BIT, VK_NULL_HANDLE, "main", 0 },
                                        pipe_layout, VK_NULL_HANDLE, -1 };
   r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, 0, p);
   vkDestroyPipelineBinaryKHR(dev, bin, 0);
   return r;
}

static bool
known(const char *h, void *u)
{
   char p[700];
   snprintf(p, sizeof(p), "%s/%s.spv", (const char *)u, h);
   FILE *f = fopen(p, "rb");
   if (f)
      fclose(f);
   return f != NULL;
}

int
main(int argc, char **argv)
{
   if (argc >= 3 && !strcmp(argv[1], "pbsize")) {
      /* probe pbsize <spv>: vkGetPipelineBinaryDataKHR with pPipelineBinaryData = NULL and a non-zero size in
       * *pPipelineBinaryDataSize. The spec says the size is returned then; nothing is written to the data pointer. */
      init();
      size_t sn;
      uint32_t *spv = (uint32_t *)rd(argv[2], &sn);
      VkPipeline p = pipe_from_spirv(spv, sn);
      VkPipelineBinaryCreateInfoKHR bci = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, 0, 0, p, 0 };
      VkPipelineBinaryKHR bin[4];
      VkPipelineBinaryHandlesInfoKHR hi = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR, 0, 0, 0 };
      CHECK_VK(vkCreatePipelineBinariesKHR(dev, &bci, 0, &hi));
      hi.pPipelineBinaries = bin;
      CHECK_VK(vkCreatePipelineBinariesKHR(dev, &bci, 0, &hi));
      VkPipelineBinaryDataInfoKHR di = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR, 0, bin[0] };
      VkPipelineBinaryKeyKHR key = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR };
      size_t n0 = 0;
      VkResult r0 = vkGetPipelineBinaryDataKHR(dev, &di, &key, &n0, NULL);
      printf("size query with *size = 0: result %d, size %zu\n", r0, n0);
      fflush(stdout);
      size_t n1 = 1u << 20;
      VkResult r1 = vkGetPipelineBinaryDataKHR(dev, &di, &key, &n1, NULL);
      printf("size query with *size = %u: result %d, size %zu\n", 1u << 20, r1, n1);
      return 0;
   }
   if (argc >= 4 && !strcmp(argv[1], "export")) {
      /* probe export <out dir> <spv>...: each shader (descriptor set 7 = the layer's set) exported as a shader object
       * (<name>.so) and as a pipeline binary (<name>.pb), for working out the layout of another Mesa version */
      init();
      for (int i = 3; i < argc; i++) {
         size_t sn, n, pn;
         uint32_t *spv = (uint32_t *)rd(argv[i], &sn);
         if (!spv) { printf("FAIL no %s\n", argv[i]); return 1; }
         const char *base = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
         char out[700];
         VkResult r;
         uint8_t *d = so_export(VK_SHADER_CODE_TYPE_SPIRV_EXT, spv, sn, &n, &r);
         CHECK_VK(r);
         snprintf(out, sizeof(out), "%s/%.*s.so", argv[2], (int)(strlen(base) - 4), base);
         FILE *f = fopen(out, "wb");
         fwrite(d, 1, n, f);
         fclose(f);
         VkPipeline p = pipe_from_spirv(spv, sn);
         VkPipelineBinaryKeyKHR key;
         uint8_t *pd2 = pb_data(p, &key, &pn);
         vkDestroyPipeline(dev, p, 0);
         snprintf(out, sizeof(out), "%s/%.*s.pb", argv[2], (int)(strlen(base) - 4), base);
         f = fopen(out, "wb");
         fwrite(pd2, 1, pn, f);
         fclose(f);
         printf("%s: shader object %zu B, pipeline binary %zu B\n", base, n, pn);
         free(d);
         free(pd2);
         free(spv);
      }
      return 0;
   }
   if (argc < 3) {
      printf("usage: probe <template.spv> <kernel folder> <dyn id>...\n");
      return 2;
   }
   init();
   int fails = 0;
   char err[256];
   size_t sn;
   uint32_t *spv = (uint32_t *)rd(argv[1], &sn);
   if (!spv) { printf("FAIL no %s\n", argv[1]); return 1; }
   for (size_t i = 5; i < sn / 4;) {   /* OpDecorate <id> DescriptorSet <n> -> the layer's set */
      const uint32_t op = spv[i] & 0xffff, wc = spv[i] >> 16;
      if (!wc)
         break;
      if (op == 71 && wc >= 4 && spv[i + 2] == 34)
         spv[i + 3] = LAYER_SET;
      i += wc;
   }

   /* 1. template as a shader object */
   VkResult r;
   size_t tn;
   uint8_t *td = so_export(VK_SHADER_CODE_TYPE_SPIRV_EXT, spv, sn, &tn, &r);
   CHECK_VK(r);
   struct anvbin t;
   if (!LT || !anvbin_parse(LT, td, tn, 80, 0, 0, &t, err, sizeof(err))) { printf("FAIL template parse: %s\n", err); return 1; }
   size_t wn;
   uint8_t *w = anvbin_write(&t, &wn);
   const bool rt = w && wn == tn && !memcmp(w, td, tn);
   unsigned have = 0, so_min = ~0u, so_max = 0;
   for (unsigned k = 0; k < NSURF_BINDINGS; k++)
      for (unsigned i = 0; i < t.surface_count; i++) {
         const uint8_t *e = t.surf + i * 16;
         uint32_t bnd, so;
         memcpy(&bnd, e + BINDING_OFF_binding, 4);
         memcpy(&so, e + BINDING_OFF_set_offset, 4);
         if (e[BINDING_OFF_set] == LAYER_SET && bnd == k) {
            have++;
            so_min = so < so_min ? so : so_min;
            so_max = so > so_max ? so : so_max;
            break;
         }
      }
   unsigned samp = 0;
   for (unsigned i = 0; i < t.sampler_count; i++) {
      uint32_t bnd;
      memcpy(&bnd, t.samp + i * 16 + BINDING_OFF_binding, 4);
      samp += t.samp[i * 16 + BINDING_OFF_set] == LAYER_SET && bnd == SAMPLER_BINDING;
   }
   if (getenv("PROBE_DUMP"))
      for (unsigned i = 0; i < t.surface_count; i++) {
         const uint8_t *e = t.surf + i * 16;
         uint32_t idx, bnd, so;
         memcpy(&idx, e, 4); memcpy(&bnd, e + 4, 4); memcpy(&so, e + 8, 4);
         printf("   bti %2u: set %3u binding %2u index %u set_offset %u plane %u\n", i, e[12], bnd, idx, so, e[13]);
      }
   printf("1. template shader object: %zu bytes, round trip %s, layout %u, surfaces %u/%u found (set_offset %u..%u), "
          "sampler %s, used sets %#x/%#x, binding mask %#x\n", tn, rt ? "identical" : "DIFFERS", t.layout_type, have,
          NSURF_BINDINGS, so_min, so_max, samp ? "found" : "MISSING", t.used_surface_sets, t.used_sampler_sets,
          t.binding_mask);
   fails += !rt || have != NSURF_BINDINGS || !samp;

   /* 2. template as a pipeline binary */
   VkPipeline tp = pipe_from_spirv(spv, sn);
   VkPipelineBinaryKeyKHR tkey;
   size_t pn;
   uint8_t *pd_ = pb_data(tp, &tkey, &pn);
   vkDestroyPipeline(dev, tp, 0);
   const size_t payload = tn - 80;
   printf("2. template pipeline binary: %zu bytes (shader object payload %zu), key %u bytes\n", pn, payload,
          tkey.keySize);
   int pre = -1, base = -1;
   for (int p = 0; p <= 8 && pre < 0; p++)
      for (int a = 0; a <= 1 && pre < 0; a++) {
         const size_t ab = a ? (size_t)p : 0;
         struct anvbin pb;
         if (!anvbin_parse(LT, pd_, pn, 0, p, ab, &pb, err, sizeof(err)))
            continue;
         size_t qn;
         uint8_t *q = anvbin_write(&pb, &qn);
         if (q && qn == pn && !memcmp(q, pd_, pn)) {
            pre = p;
            base = (int)ab;
            printf("   round trip with %d leading byte(s) (value(s)", p);
            for (int i = 0; i < p; i++)
               printf(" %u", pd_[i]);
            printf("), alignment from %s; code %s the shader object's (code %u/%u B, surfaces %u/%u, samplers %u/%u, "
                   "used sets %#x/%#x)\n", a ? "the payload" : "the data start",
                   pb.code_len == t.code_len && !memcmp(pb.code, t.code, t.code_len) ? "equals" : "DIFFERS from",
                   pb.code_len, t.code_len, pb.surface_count, t.surface_count, pb.sampler_count, t.sampler_count,
                   pb.used_surface_sets, t.used_surface_sets);
         }
         free(q);
      }
   if (pre < 0) {
      printf("   FAIL: no wrapping round-trips\n");
      fails++;
   }
   /* the template's own data must make a pipeline again */
   VkPipeline again;
   r = pipe_from_binary(&tkey, pd_, pn, &again);
   printf("   pipeline from the template's own binary: %d\n", r);
   if (!r)
      vkDestroyPipeline(dev, again, 0);
   fails += r != 0;

   /* 3. kernels */
   /* the compile of the device's generation (gens.h); a GPU the layer does not serve compiles as the Xe3 B390 */
   const struct xmx_gen *gen = xmx_gen_of(dev_id);
   struct xmx_dev_info di = { gen ? dev_id : 0xb080, 64, 0, "xe3" };
   if (!gen)
      gen = xmx_gen_of(0xb080);
   di.gen = gen->name;
   di.max_grf = gen->max_grf;
   if (getenv("XMX_LAYER_FORCE_GRF") && !strcmp(getenv("XMX_LAYER_FORCE_GRF"), "128"))
      di.max_grf = 128;
   printf("3. kernels for %s (0x%04x), %u GRF cap\n", gen->name, di.pci_id, di.max_grf);
   for (int i = 3; i < argc; i++) {
      const int id = atoi(argv[i]);
      char hash[17], name[32];
      if (!xmx_map_lookup(argv[2], id, hash)) { printf("3. dyn_%d: not in map.txt\n", id); fails++; continue; }
      snprintf(name, sizeof(name), "dyn_%d", id);
      uint8_t *img;
      size_t in;
      if (!xmx_kernel_get(&di, argv[2], id, name, hash, &img, &in)) { printf("3. dyn_%d: compile FAILED\n", id); fails++; continue; }
      struct xmx_cml k;
      xmx_cml_parse(img, in, &k);
      size_t cn;
      uint8_t *code = xmx_full_code(&k, gen->isa, &cn);
      const bool smp = (xmx_rd32(k.hdr + XMX_H_BARRIERS) & 0x80000000u) != 0;
      struct anvbin_kernel kc = { &k, code, (uint32_t)cn, LAYER_SET, SAMPLER_BINDING, smp, 64 };
      struct anvbin kb;
      if (!code || !anvbin_kernel(&t, &kc, &kb, err, sizeof(err))) {
         printf("3. dyn_%d: binary FAILED: %s\n", id, code ? err : "no prologue");
         fails++;
         continue;
      }
      size_t bn, en;
      uint8_t *bd = anvbin_write(&kb, &bn);
      uint8_t *ed = so_export(VK_SHADER_CODE_TYPE_BINARY_EXT, bd, bn, &en, &r);
      const bool so_same = ed && en == bn && !memcmp(ed, bd, bn);
      printf("3. dyn_%d (%s): code %zu B (prologue %zu), %u GRF, surfaces %u, sampler %u, group_count %s; shader "
             "object %d, re-export %s", id, hash, cn, cn - k.code_size, xmx_rd32(k.hdr + XMX_H_GRF),
             xmx_rd32(k.hdr + XMX_H_NSURF), smp,
             k.gc_off == XMX_NO_GC ? "no" : "yes", r, so_same ? "identical" : "DIFFERS");
      fails += r != 0 || !so_same;
      free(ed);
      if (pre >= 0) {
         struct anvbin kp = kb;
         kp.hdr_len = 0;
         kp.pre_len = pre;
         kp.align_base = base;
         memcpy(kp.pre, pd_, pre);
         size_t kn;
         uint8_t *kd = anvbin_write(&kp, &kn);
         VkPipelineBinaryKeyKHR key = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR, 0, 32 };
         memset(key.key, 0, sizeof(key.key));
         memcpy(key.key, "XMXL", 4);
         memcpy(key.key + 4, hash, 16);
         VkPipeline kpipe;
         r = pipe_from_binary(&key, kd, kn, &kpipe);
         bool pb_same = false;
         if (!r) {
            VkPipelineBinaryKeyKHR k2;
            size_t n2;
            uint8_t *d2 = pb_data(kpipe, &k2, &n2);
            pb_same = n2 == kn && !memcmp(d2, kd, kn);
            free(d2);
            vkDestroyPipeline(dev, kpipe, 0);
         }
         printf("; pipeline binary %d, re-export %s\n", r, r ? "-" : pb_same ? "identical" : "DIFFERS");
         fails += r != 0 || !pb_same;
         free(kd);
      } else {
         printf("\n");
      }
      free(bd);
      free(code);
      free(img);
   }
   (void)known;
   printf("probe: %s\n", fails ? "FAILED" : "all passed");
   return fails != 0;
}
