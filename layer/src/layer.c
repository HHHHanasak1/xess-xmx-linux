/* layer.c - VK_LAYER_XESS_xmx: the shim's XeSS kernels on XMX with an unpatched ANV (Mesa 26.1, 26.3 and 27.0-devel,
 * see layouts.h).
 *
 * What the patched driver does inside ANV, this layer does on top of it:
 *  - recognise the shim's placeholder compute shaders (placeholder.c) in the SPIR-V from vkd3d-proton;
 *  - for a placeholder pipeline, compile the kernel (-noLoadPayload, compile.c), put a prologue in front of it
 *    (prologue.c), wrap it into the driver's own serialized form taken from a template shader (anvbin.c), and create
 *    a real pipeline from that through VK_KHR_pipeline_binary; its layout is vkd3d's plus one set of the layer's;
 *  - at every dispatch of such a pipeline, fill the layer's set from vkd3d's descriptor heap - surface k is heap
 *    index push_constant[table k] + element - and bind it;
 *  - answer the shim's self-test placeholder (anv_canary) once its own check on this device has passed, so that the
 *    shim uses XeSS' XMX path only when the layer can serve it;
 *  - a Vulkan game with XeSS (vkwrap/, the package's vulkan-1.dll): the same placeholders, made by vkwrap from XeSS'
 *    CM shader modules, in pipeline layouts without sets; their descriptors come from XeSS' descriptor push, which
 *    vkwrap passes on as vkCmdPushDescriptorSetKHR with no layout and set XMX_VK_PUSH_SET (kept here, never passed
 *    on), surface k = the k-th pushed resource. The self-test also writes layer_canary, which vkwrap waits for.
 * On this project's patched driver (its prog_data is longer, layouts.h) the layer serves only the Vulkan path: the
 * driver injects a D3D12 game's kernels itself but has no use for XeSS' descriptor push.
 * Enabled with VK_LOADER_LAYERS_ENABLE=VK_LAYER_XESS_xmx (and VK_ADD_LAYER_PATH); log: XMX_LAYER_LOG=1 or a path. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <threads.h>
#include <unistd.h>

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include "../third_party/blake3/blake3.h"
#include "anvbin.h"
#include "cmk.h"
#include "compile.h"
#include "layouts.h"
#include "map.h"
#include "placeholder.h"
#include "prof.h"
#include "prologue.h"
#include "template_spv.h"

#define EXPORT __attribute__((visibility("default")))
#define NSURF_BINDINGS 32
#define SAMPLER_BINDING 32
#define SETS_PER_POOL 64
#define XMX_PC_BYTES 256   /* the push constants a command buffer keeps for the dispatches */

/* ---- log ---- */
static FILE *log_file;
static once_flag log_once = ONCE_FLAG_INIT;
static mtx_t log_mtx;
static void
log_init(void)
{
   mtx_init(&log_mtx, mtx_plain);
   const char *e = getenv("XMX_LAYER_LOG");
   if (e && *e && strcmp(e, "0"))
      log_file = strchr(e, '/') ? fopen(e, "a") : stderr;
}
static void __attribute__((format(printf, 1, 2)))
xlog(const char *fmt, ...)
{
   call_once(&log_once, log_init);
   if (!log_file)
      return;
   mtx_lock(&log_mtx);
   va_list ap;
   va_start(ap, fmt);
   fprintf(log_file, "xmx-layer[%d]: ", (int)getpid());
   vfprintf(log_file, fmt, ap);
   fputc('\n', log_file);
   fflush(log_file);
   va_end(ap);
   mtx_unlock(&log_mtx);
}

/* ---- objects ---- */
#define DKEY(h) ((uint64_t)(uintptr_t)(*(void **)(h)))
#define HKEY(h) ((uint64_t)(h))

static struct xmap instances, devices, physdevs, modules, set_layouts, pipe_layouts, pipelines, cmdbufs, cmdpools;
static once_flag maps_once = ONCE_FLAG_INIT;
static void
maps_init(void)
{
   xmap_init(&instances); xmap_init(&devices); xmap_init(&physdevs); xmap_init(&modules);
   xmap_init(&set_layouts); xmap_init(&pipe_layouts); xmap_init(&pipelines); xmap_init(&cmdbufs);
   xmap_init(&cmdpools);
}

struct xinstance {
   VkInstance handle;
   PFN_vkGetInstanceProcAddr gipa;
   PFN_vkDestroyInstance DestroyInstance;
   PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
   PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
   PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2;
   PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
};

#define DEVICE_FNS(X) \
   X(DestroyDevice) X(CreateShaderModule) X(DestroyShaderModule) X(CreateComputePipelines) X(DestroyPipeline) \
   X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
   X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(ResetCommandBuffer) \
   X(ResetCommandPool) X(DestroyCommandPool) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) \
   X(CmdDispatch) X(CmdDispatchBase) X(CmdDispatchIndirect) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
   X(ResetDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateSampler) X(DestroySampler) \
   X(CreatePipelineBinariesKHR) X(GetPipelineBinaryDataKHR) X(DestroyPipelineBinaryKHR) \
   X(CreateQueryPool) X(DestroyQueryPool) X(CmdResetQueryPool) X(CmdWriteTimestamp) X(GetQueryPoolResults) \
   X(QueuePresentKHR) X(EndCommandBuffer) X(CmdPushDescriptorSetKHR)

/* vkwrap (the vulkan-1.dll of a Vulkan game with XeSS) hands XeSS' vkCmdPushXeSSDescriptorsINTEL over as
 * vkCmdPushDescriptorSetKHR with no layout and this set number: the descriptors of the next dispatches, for the layer */
#define XMX_VK_PUSH_SET 0x58455353u   /* "XESS" */

/* XMX_LAYER_PROF: time stamps of a command buffer, two per dispatch of a replaced kernel */
#define PROF_QUERIES 256

struct xlayer_set;   /* the layer's descriptor set layout for one mutable type list + flags */
struct xtemplate;

struct xdevice {
   uint64_t key;                /* the loader's dispatch key of the device */
   VkDevice handle;
   VkPhysicalDevice phys;
   struct xinstance *inst;
   PFN_vkGetDeviceProcAddr gdpa;
#define X(n) PFN_vk##n n;
   DEVICE_FNS(X)
#undef X
   bool binaries;               /* pipeline binaries enabled by the layer */
   const struct anv_layout *L;  /* the serialized-shader layout of this driver (layouts.h), NULL if unknown */
   struct anv_layout Lp;        /* L with the patched driver's prog_data size, once a template showed it (L = &Lp) */
   bool ok;                     /* the self-test passed: placeholders are served */
   bool tested;
   mtx_t mtx;
   struct xmx_dev_info info;
   VkSampler default_sampler;   /* XeSS' own: linear, clamp, normalised */
   struct xlayer_set *lsets;    /* list */
   struct xtemplate *templates; /* list */
   char dir[512];               /* the prefix's kernel folder */
   bool prof;                   /* XMX_LAYER_PROF */
   int prof_mode;               /* 1 per dispatch, 2 per run (prof.h) */
   int prof_run;                /* the slot of the runs in mode 2 */
   double ts_period;            /* ns per time stamp tick */
   uint64_t ts_mask;            /* the valid bits of a time stamp */
};

struct xsetlayout {
   struct xdevice *dev;                    /* first in every per-handle struct: x_DestroyDevice purges by it */
   VkDescriptorSetLayoutCreateFlags flags;
   uint32_t heap_binding, heap_count;      /* the MUTABLE binding with >= 1000 descriptors, ~0 if none */
   VkDescriptorType mut_types[8];
   uint32_t nmut;
   uint32_t sampler_heap_binding;          /* SAMPLER binding with >= 1000 descriptors, ~0 if none */
   VkSampler static_sampler;               /* first immutable sampler, or 0 */
   unsigned nstatic;
};

struct xpipelayout {
   struct xdevice *dev;
   uint32_t nsets;
   VkDescriptorSetLayout sets[8];
   uint32_t npc;
   VkPushConstantRange pc[8];
   uint32_t push_size;
   VkPipelineLayout ext[1];   /* the layout with the layer's set appended (created on demand) */
   struct xlayer_set *ext_set;
};

struct xlayer_set {
   struct xlayer_set *next;
   VkDescriptorSetLayoutCreateFlags flags;
   VkDescriptorType mut_types[8];
   uint32_t nmut;
   VkDescriptorSetLayout layout;
};

struct xtemplate {
   struct xtemplate *next;
   VkDescriptorSetLayout set_layout;
   uint32_t set_index;
   uint8_t *data;
   size_t n;
   struct anvbin bin;
};

/* a replaced pipeline: what each dispatch needs */
struct xkernel {
   struct xdevice *dev;
   VkPipelineLayout layout;     /* vkd3d's layout + the layer's set */
   uint32_t set_index;          /* the layer's set */
   VkDescriptorSetLayout lset;
   struct xlayer_set *ls;
   uint32_t heap_set, heap_binding, heap_count;
   uint32_t nsurf, table_off[24], elem[24];
   bool sampler;
   bool vkpush;                 /* a Vulkan game's kernel: its descriptors come from the XeSS push (vkwrap), not a heap */
   int sampler_mode;            /* 0 static, 1 heap, 2 default, 3 the push's */
   VkSampler static_sampler;
   uint32_t sampler_heap_set, sampler_heap_binding, sampler_pc_off;
   char hash[17];
   int prof;                    /* its slot in prof.c, -1 if none */
};

/* the layer's command buffers of a command pool (the application synchronises the pool, so the list needs no lock) */
struct xcmdpool {
   struct xdevice *dev;
   VkCommandPool handle;
   struct xcmd *cmds;
   struct xcmdpool *next;       /* x_DestroyDevice's list */
};

struct xcmd {
   VkCommandBuffer handle;
   struct xcmdpool *pool;
   struct xcmd *prev, *next;    /* in pool->cmds */
   struct xdevice *dev;
   struct xkernel *bound;
   VkDescriptorSet sets[8];
   uint8_t pc[XMX_PC_BYTES];
   /* the layer's descriptor pools of this command buffer, reset when it is begun again */
   VkDescriptorPool pools[64];
   unsigned npools;
   uint64_t pools_full;         /* bit i: pools[i] ran out since the last reset */
   struct xlayer_set *pool_ls[64];
   /* the last XeSS descriptor push (Vulkan games, vkwrap): resources in kernel order, the first sampler */
   uint32_t vk_n;
   VkDescriptorType vk_type[NSURF_BINDINGS];
   VkDescriptorImageInfo vk_img[NSURF_BINDINGS];
   VkDescriptorBufferInfo vk_buf[NSURF_BINDINGS];
   VkSampler vk_sampler;
   bool vk_pushed;
   /* XMX_LAYER_PROF (primary command buffers): the time stamps recorded since the last begin */
   bool primary;
   VkQueryPool qp;
   uint32_t nq;
   unsigned qlost;
   int qk[PROF_QUERIES / 2];
   bool in_run;                 /* mode 2: a run's first time stamp is written, its last not yet */
};

/* The device of a handle. Called by every hooked command, so the last device found is kept: a game has one. */
static _Atomic(struct xdevice *) last_dev;
static struct xdevice *
dev_of(void *h)
{
   const uint64_t k = DKEY(h);
   struct xdevice *d = atomic_load_explicit(&last_dev, memory_order_acquire);
   if (d && d->key == k)
      return d;
   d = xmap_get(&devices, k);
   if (d)
      atomic_store_explicit(&last_dev, d, memory_order_release);
   return d;
}

/* The layer's state of a command buffer, also on every hooked command: each recording thread keeps the last one it
 * looked up. Freeing command buffers bumps cmd_epoch, which empties every thread's cache. */
static atomic_uint cmd_epoch;
static _Thread_local struct { VkCommandBuffer cb; struct xcmd *c; unsigned epoch; } cmd_cache;
static struct xcmd *
cmd_of(VkCommandBuffer cb)
{
   const unsigned e = atomic_load_explicit(&cmd_epoch, memory_order_acquire);
   if (cmd_cache.cb == cb && cmd_cache.epoch == e)
      return cmd_cache.c;
   struct xcmd *c = xmap_get(&cmdbufs, (uint64_t)(uintptr_t)cb);
   cmd_cache.cb = cb;
   cmd_cache.c = c;
   cmd_cache.epoch = e;
   return c;
}

/* ---- self-test and templates ---- */
static VkPipeline
create_from_binary(struct xdevice *d, const uint8_t *data, size_t n, VkPipelineLayout layout, VkResult *res)
{
   VkPipelineBinaryKeyKHR key = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR, 0, 32 };
   blake3_hasher bh;
   blake3_hasher_init(&bh);
   blake3_hasher_update(&bh, "xess-xmx layer", 14);
   blake3_hasher_update(&bh, data, n);
   blake3_hasher_finalize(&bh, key.key, 32);   /* unique by content */
   VkPipelineBinaryDataKHR bd = { n, (void *)data };
   VkPipelineBinaryKeysAndDataKHR kd = { 1, &key, &bd };
   VkPipelineBinaryCreateInfoKHR bci = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, 0, &kd, 0, 0 };
   VkPipelineBinaryKHR bin;
   VkPipelineBinaryHandlesInfoKHR hi = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR, 0, 1, &bin };
   *res = d->CreatePipelineBinariesKHR(d->handle, &bci, NULL, &hi);
   if (*res)
      return VK_NULL_HANDLE;
   VkPipelineBinaryInfoKHR pbi = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, 0, 1, &bin };
   VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, &pbi, 0,
                                      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0,
                                        VK_SHADER_STAGE_COMPUTE_BIT, VK_NULL_HANDLE, "main", 0 },
                                      layout, VK_NULL_HANDLE, -1 };
   VkPipeline p = VK_NULL_HANDLE;
   *res = d->CreateComputePipelines(d->handle, VK_NULL_HANDLE, 1, &ci, NULL, &p);
   d->DestroyPipelineBinaryKHR(d->handle, bin, NULL);
   return *res ? VK_NULL_HANDLE : p;
}

/* the layer's set layout for a heap's mutable type list and flags (d->mtx held) */
static struct xlayer_set *
layer_set(struct xdevice *d, const VkDescriptorType *types, uint32_t ntypes, VkDescriptorSetLayoutCreateFlags flags)
{
   flags &= VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
   for (struct xlayer_set *l = d->lsets; l; l = l->next)
      if (l->flags == flags && l->nmut == ntypes && !memcmp(l->mut_types, types, ntypes * sizeof(*types)))
         return l;
   struct xlayer_set *l = calloc(1, sizeof(*l));
   l->flags = flags;
   l->nmut = ntypes;
   memcpy(l->mut_types, types, ntypes * sizeof(*types));
   VkMutableDescriptorTypeListEXT lists[NSURF_BINDINGS + 1];
   VkDescriptorSetLayoutBinding b[NSURF_BINDINGS + 1];
   for (int i = 0; i < NSURF_BINDINGS; i++) {
      b[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_MUTABLE_EXT, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0 };
      lists[i] = (VkMutableDescriptorTypeListEXT){ ntypes, l->mut_types };
   }
   b[NSURF_BINDINGS] = (VkDescriptorSetLayoutBinding){ SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                                                       VK_SHADER_STAGE_COMPUTE_BIT, 0 };
   lists[NSURF_BINDINGS] = (VkMutableDescriptorTypeListEXT){ 0, 0 };
   VkMutableDescriptorTypeCreateInfoEXT mci = { VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT, 0,
                                                NSURF_BINDINGS + 1, lists };
   VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &mci, flags,
                                          NSURF_BINDINGS + 1, b };
   if (d->CreateDescriptorSetLayout(d->handle, &ci, NULL, &l->layout)) {
      free(l);
      return NULL;
   }
   l->next = d->lsets;
   d->lsets = l;
   return l;
}

/* the template SPIR-V with its descriptor set number changed to `set` */
static uint32_t *
template_for_set(uint32_t set, size_t *n)
{
   *n = sizeof(xmx_template_spv);
   uint32_t *w = malloc(*n);
   memcpy(w, xmx_template_spv, *n);
   for (size_t i = 5; i < *n / 4;) {
      const uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
      if (!wc)
         break;
      if (op == 71 && wc >= 4 && w[i + 2] == 34)   /* OpDecorate <id> DescriptorSet <n> */
         w[i + 3] = set;
      i += wc;
   }
   return w;
}

/* the template exported through a pipeline binary for (layout, set index) (d->mtx held) */
static struct xtemplate *
template_get(struct xdevice *d, VkPipelineLayout layout, struct xlayer_set *ls, uint32_t set_index)
{
   for (struct xtemplate *t = d->templates; t; t = t->next)
      if (t->set_layout == ls->layout && t->set_index == set_index)
         return t;
   size_t n;
   uint32_t *spv = template_for_set(set_index, &n);
   VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, n, spv };
   VkPipelineCreateFlags2CreateInfoKHR f2 = { VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, 0,
                                              VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR };
   VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, &f2, 0,
                                      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &smci, 0,
                                        VK_SHADER_STAGE_COMPUTE_BIT, VK_NULL_HANDLE, "main", 0 },
                                      layout, VK_NULL_HANDLE, -1 };
   VkPipeline p;
   VkResult r = d->CreateComputePipelines(d->handle, VK_NULL_HANDLE, 1, &ci, NULL, &p);
   free(spv);
   if (r) {
      xlog("template pipeline (set %u) failed: %d", set_index, r);
      return NULL;
   }
   VkPipelineBinaryCreateInfoKHR bci = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, 0, 0, p, 0 };
   VkPipelineBinaryKHR bin[2];
   VkPipelineBinaryHandlesInfoKHR hi = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR, 0, 0, 0 };
   r = d->CreatePipelineBinariesKHR(d->handle, &bci, NULL, &hi);
   if (r || hi.pipelineBinaryCount != 1) {
      d->DestroyPipeline(d->handle, p, NULL);
      xlog("template binaries: %d (%u)", r, hi.pipelineBinaryCount);
      return NULL;
   }
   hi.pPipelineBinaries = bin;
   r = d->CreatePipelineBinariesKHR(d->handle, &bci, NULL, &hi);
   d->DestroyPipeline(d->handle, p, NULL);
   if (r)
      return NULL;
   VkPipelineBinaryDataInfoKHR di = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR, 0, bin[0] };
   VkPipelineBinaryKeyKHR key = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR };
   size_t dn = 0;   /* the size query must not see a stale size (26.1.2 copies into a NULL buffer otherwise) */
   r = d->GetPipelineBinaryDataKHR(d->handle, &di, &key, &dn, NULL);
   uint8_t *data = r ? NULL : malloc(dn);
   if (data)
      r = d->GetPipelineBinaryDataKHR(d->handle, &di, &key, &dn, data);
   d->DestroyPipelineBinaryKHR(d->handle, bin[0], NULL);
   if (r || !data) {
      free(data);
      return NULL;
   }
   struct xtemplate *t = calloc(1, sizeof(*t));
   char err[200];
   /* 26.1.2: the data is the serialized shader with no leading bytes, aligned from its start (probe, stage 3) */
   bool parsed = anvbin_parse(d->L, data, dn, 0, 0, 0, &t->bin, err, sizeof(err));
   if (!parsed && d->L != &d->Lp && d->L->pd_size_patched) {
      /* this project's patched driver: the same layout, prog_data longer by its C-for-Metal fields (layouts.h). Its
       * own kernel injection only takes vkd3d's descriptor heap, so the layer serves XeSS' Vulkan path there too. */
      struct anv_layout lp = *d->L;
      lp.pd_size = lp.pd_size_patched;
      char err2[200];
      if (anvbin_parse(&lp, data, dn, 0, 0, 0, &t->bin, err2, sizeof(err2))) {
         d->Lp = lp;
         d->L = &d->Lp;
         t->bin.L = d->L;
         parsed = true;
         xlog("template (set %u): the patched driver's layout (prog_data %u bytes)", set_index, lp.pd_size);
      }
   }
   if (!parsed) {
      xlog("template (set %u) does not parse: %s", set_index, err);
      free(data);
      free(t);
      return NULL;
   }
   size_t wn;
   uint8_t *w = anvbin_write(&t->bin, &wn);
   const bool same = w && wn == dn && !memcmp(w, data, dn);
   free(w);
   if (!same) {
      xlog("template (set %u): re-serialization differs, layout table does not match this driver", set_index);
      free(data);
      free(t);
      return NULL;
   }
   {
      /* what the driver put into the template's binding table, per set */
      unsigned per_set[256] = { 0 }, minb = ~0u, maxb = 0;
      for (unsigned i = 0; i < t->bin.surface_count; i++) {
         const uint8_t *e = t->bin.surf + i * 16;
         uint32_t bnd;
         memcpy(&bnd, e + BINDING_OFF_binding, 4);
         per_set[e[BINDING_OFF_set]]++;
         if (e[BINDING_OFF_set] == set_index) {
            minb = bnd < minb ? bnd : minb;
            maxb = bnd > maxb ? bnd : maxb;
         }
      }
      char sets[256] = "";
      for (unsigned s = 0; s < 256; s++)
         if (per_set[s])
            snprintf(sets + strlen(sets), sizeof(sets) - strlen(sets), " set%u:%u", s, per_set[s]);
      xlog("template (set %u): %zu bytes, layout type %u, %u surfaces (%s), set %u bindings %u..%u, %u samplers, "
           "used sets %#x/%#x, mask %#x", set_index, dn, t->bin.layout_type, t->bin.surface_count, sets, set_index,
           minb, maxb, t->bin.sampler_count, t->bin.used_surface_sets, t->bin.used_sampler_sets, t->bin.binding_mask);
   }
   t->set_layout = ls->layout;
   t->set_index = set_index;
   t->data = data;
   t->n = dn;
   t->next = d->templates;
   d->templates = t;
   return t;
}

/* vkd3d's layout + the layer's set (d->mtx held) */
static VkPipelineLayout
ext_layout(struct xdevice *d, struct xpipelayout *pl, struct xlayer_set *ls)
{
   if (pl->ext[0] && pl->ext_set == ls)
      return pl->ext[0];
   if (pl->ext[0])
      return VK_NULL_HANDLE;   /* one layer set per layout: a second heap type list is not expected */
   VkDescriptorSetLayout sets[8];
   memcpy(sets, pl->sets, pl->nsets * sizeof(sets[0]));
   sets[pl->nsets] = ls->layout;
   VkPipelineLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, pl->nsets + 1, sets,
                                     pl->npc, pl->pc };
   if (d->CreatePipelineLayout(d->handle, &ci, NULL, &pl->ext[0]))
      return VK_NULL_HANDLE;
   pl->ext_set = ls;
   return pl->ext[0];
}

/* The self-test: this driver is one whose layout the layer knows, the template round-trips, and a pipeline can be
 * made from a binary the layer wrote. Only then is the shim's self-test answered. */
static void
self_test(struct xdevice *d)
{
   mtx_lock(&d->mtx);
   if (d->tested) {
      mtx_unlock(&d->mtx);
      return;
   }
   d->tested = true;
   VkPhysicalDeviceDriverProperties drv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
   VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv };
   d->inst->GetPhysicalDeviceProperties2(d->phys, &p2);
   d->L = drv.driverID == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA ? anv_layout_for(drv.driverInfo) : NULL;
   const bool known = d->L != NULL;
   const bool xe3 = (p2.properties.deviceID & 0xfff0) == 0xb080 || (p2.properties.deviceID & 0xfff0) == 0xfd80;
   if (!d->binaries || !known || !xe3) {
      xlog("self-test: not served (%s, %s 0x%04x, pipeline binaries %s)", drv.driverInfo,
           p2.properties.deviceName, p2.properties.deviceID, d->binaries ? "on" : "off");
      mtx_unlock(&d->mtx);
      return;
   }
   static const VkDescriptorType deftypes[] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER };
   struct xlayer_set *ls = layer_set(d, deftypes, 4, 0);
   VkPipelineLayout pl = VK_NULL_HANDLE;
   if (ls) {
      VkPipelineLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &ls->layout };
      d->CreatePipelineLayout(d->handle, &ci, NULL, &pl);
   }
   struct xtemplate *t = pl ? template_get(d, pl, ls, 0) : NULL;
   VkResult r = VK_ERROR_UNKNOWN;
   if (t) {
      VkPipeline p = create_from_binary(d, t->data, t->n, pl, &r);
      if (p)
         d->DestroyPipeline(d->handle, p, NULL);
   }
   if (pl)
      d->DestroyPipelineLayout(d->handle, pl, NULL);
   d->ok = t && r == VK_SUCCESS;
   xlog("self-test %s: %s, template %s, pipeline from binary %d", d->ok ? "passed" : "FAILED", drv.driverInfo,
        t ? "round-trips" : "unusable", r);
   mtx_unlock(&d->mtx);
}

/* ---- descriptor set layouts / pipeline layouts ---- */
static VKAPI_ATTR VkResult VKAPI_CALL
x_CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci,
                            const VkAllocationCallbacks *alloc, VkDescriptorSetLayout *out)
{
   struct xdevice *d = dev_of(device);
   VkResult r = d->CreateDescriptorSetLayout(device, ci, alloc, out);
   if (r)
      return r;
   struct xsetlayout *l = calloc(1, sizeof(*l));
   l->dev = d;
   l->flags = ci->flags;
   l->heap_binding = l->sampler_heap_binding = ~0u;
   const VkMutableDescriptorTypeCreateInfoEXT *mci = NULL;
   for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext)
      if (s->sType == VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT)
         mci = (const void *)s;
   for (uint32_t i = 0; i < ci->bindingCount; i++) {
      const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
      if (b->descriptorType == VK_DESCRIPTOR_TYPE_MUTABLE_EXT && b->descriptorCount >= 1000 &&
          l->heap_binding == ~0u) {
         l->heap_binding = b->binding;
         l->heap_count = b->descriptorCount;
         if (mci && i < mci->mutableDescriptorTypeListCount) {
            const VkMutableDescriptorTypeListEXT *ml = &mci->pMutableDescriptorTypeLists[i];
            l->nmut = ml->descriptorTypeCount < 8 ? ml->descriptorTypeCount : 8;
            memcpy(l->mut_types, ml->pDescriptorTypes, l->nmut * sizeof(VkDescriptorType));
         }
      }
      if (b->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER && b->descriptorCount >= 1000 && !b->pImmutableSamplers &&
          l->sampler_heap_binding == ~0u)
         l->sampler_heap_binding = b->binding;
      if (b->pImmutableSamplers && (b->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                                    b->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) {
         if (!l->nstatic)
            l->static_sampler = b->pImmutableSamplers[0];
         l->nstatic += b->descriptorCount;
      }
   }
   xmap_put(&set_layouts, HKEY(*out), l);
   return r;
}

static VKAPI_ATTR void VKAPI_CALL
x_DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout l, const VkAllocationCallbacks *alloc)
{
   struct xdevice *d = dev_of(device);
   if (l)
      free(xmap_take(&set_layouts, HKEY(l)));
   d->DestroyDescriptorSetLayout(device, l, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL
x_CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *alloc,
                       VkPipelineLayout *out)
{
   struct xdevice *d = dev_of(device);
   VkResult r = d->CreatePipelineLayout(device, ci, alloc, out);
   if (r || ci->setLayoutCount > 7 || ci->pushConstantRangeCount > 8)
      return r;
   struct xpipelayout *pl = calloc(1, sizeof(*pl));
   pl->dev = d;
   pl->nsets = ci->setLayoutCount;
   memcpy(pl->sets, ci->pSetLayouts, pl->nsets * sizeof(VkDescriptorSetLayout));
   pl->npc = ci->pushConstantRangeCount;
   memcpy(pl->pc, ci->pPushConstantRanges, pl->npc * sizeof(VkPushConstantRange));
   for (uint32_t i = 0; i < pl->npc; i++)
      if (pl->pc[i].offset + pl->pc[i].size > pl->push_size)
         pl->push_size = pl->pc[i].offset + pl->pc[i].size;
   xmap_put(&pipe_layouts, HKEY(*out), pl);
   return r;
}

static VKAPI_ATTR void VKAPI_CALL
x_DestroyPipelineLayout(VkDevice device, VkPipelineLayout l, const VkAllocationCallbacks *alloc)
{
   struct xdevice *d = dev_of(device);
   struct xpipelayout *pl = l ? xmap_take(&pipe_layouts, HKEY(l)) : NULL;
   /* the extended layout stays alive while replaced pipelines may still use it: released with the device */
   free(pl);
   d->DestroyPipelineLayout(device, l, alloc);
}

/* ---- shader modules ---- */
struct xmodule {
   struct xdevice *dev;
   struct xmx_placeholder ph;
};

static bool
hash_known(const char *hash, void *user)
{
   const struct xdevice *d = user;
   char p[700];
   snprintf(p, sizeof(p), "%s/%s.spv", d->dir, hash);
   return access(p, R_OK) == 0;
}

static void
classify_code(struct xdevice *d, const uint32_t *code, size_t size, struct xmx_placeholder *ph)
{
   struct xmx_spv_info info;
   memset(ph, 0, sizeof(*ph));
   if (!code || size < 20 || size > XMX_PH_MAX_SPV_BYTES || !xmx_spv_scan(code, size / 4, &info) || !info.has_magic)
      return;
   xmx_classify(&info, d->dir[0] ? d->dir : NULL, d->dir[0] ? hash_known : NULL, d, ph);
}

static VKAPI_ATTR VkResult VKAPI_CALL
x_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *alloc,
                     VkShaderModule *out)
{
   struct xdevice *d = dev_of(device);
   VkResult r = d->CreateShaderModule(device, ci, alloc, out);
   if (r)
      return r;
   struct xmx_placeholder ph;
   classify_code(d, ci->pCode, ci->codeSize, &ph);
   if (ph.kind != XMX_PH_NONE) {
      struct xmodule *m = malloc(sizeof(*m));
      m->dev = d;
      m->ph = ph;
      free(xmap_take(&modules, HKEY(*out)));
      xmap_put(&modules, HKEY(*out), m);
   } else if (xmap_get(&modules, HKEY(*out))) {
      free(xmap_take(&modules, HKEY(*out)));   /* a stale entry of a reused handle */
   }
   return r;
}

static VKAPI_ATTR void VKAPI_CALL
x_DestroyShaderModule(VkDevice device, VkShaderModule m, const VkAllocationCallbacks *alloc)
{
   struct xdevice *d = dev_of(device);
   if (m)
      free(xmap_take(&modules, HKEY(m)));
   d->DestroyShaderModule(device, m, alloc);
}

/* ---- pipelines ---- */
static atomic_uint replaced_count;   /* replaced pipelines alive: none, no lookup on a compute bind */

/* The replaced kernel of a pipeline (NULL for any other), on every compute bind: each recording thread keeps the
 * last lookups. Every change of `pipelines` bumps pipe_epoch after it, which empties every thread's cache, so a
 * destroyed pipeline's handle reused for another pipeline is looked up again. */
static atomic_uint pipe_epoch;
#define PIPE_CACHE 8
static _Thread_local struct { VkPipeline p; struct xkernel *k; unsigned epoch; } pipe_cache[PIPE_CACHE];
static struct xkernel *
kernel_of(VkPipeline p)
{
   const unsigned e = atomic_load_explicit(&pipe_epoch, memory_order_acquire);
   const unsigned s = (unsigned)((HKEY(p) * 0x9e3779b97f4a7c15ull) >> 61) & (PIPE_CACHE - 1);
   if (pipe_cache[s].p == p && pipe_cache[s].epoch == e)
      return pipe_cache[s].k;
   struct xkernel *k = xmap_get(&pipelines, HKEY(p));
   pipe_cache[s].p = p;
   pipe_cache[s].k = k;
   pipe_cache[s].epoch = e;
   return k;
}
/* The self-test placeholder's pipeline layout, as replace() needs it for a kernel: 2 = vkd3d-proton's classic heap (a
 * MUTABLE binding of >= 1000 descriptors) and its one descriptor table in the push constants (the shim's test root
 * signature: one table of one UAV); 1 = no sets and no push constants (vkwrap's test: XeSS' Vulkan push); 0 = neither,
 * e.g. vkd3d-proton on VK_EXT_descriptor_buffer or VK_EXT_descriptor_heap, whose kernels the layer could not bind. */
static int
canary_layout(const VkComputePipelineCreateInfo *ci)
{
   const struct xpipelayout *pl = xmap_get(&pipe_layouts, HKEY(ci->layout));
   if (!pl)
      return 0;
   if (pl->nsets == 0)
      return pl->push_size == 0 ? 1 : 0;
   for (uint32_t s = 0; s < pl->nsets; s++) {
      const struct xsetlayout *l = xmap_get(&set_layouts, HKEY(pl->sets[s]));
      if (l && l->heap_binding != ~0u)
         return pl->push_size >= 4 ? 2 : 0;
   }
   return 0;
}

static void
answer_canary(struct xdevice *d, const VkComputePipelineCreateInfo *ci, const struct xmx_placeholder *ph)
{
   self_test(d);
   if (!d->ok || !d->dir[0] || (ph->kind == XMX_PH_CANARY && !ph->canary_ok))
      return;
   const int lay = canary_layout(ci);
   if (!lay) {
      const struct xpipelayout *pl = xmap_get(&pipe_layouts, HKEY(ci->layout));
      xlog("self-test placeholder NOT answered: its pipeline layout (%d sets, %u bytes of push constants) has no "
           "vkd3d-style descriptor heap - VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer,VK_EXT_descriptor_heap "
           "set? XeSS uses its DP4a path", pl ? (int)pl->nsets : -1, pl ? pl->push_size : 0);
      return;
   }
   char mark[600];
   FILE *f;
   if (lay == 2) {   /* the shim (D3D12) reads this one; vkwrap's Vulkan test only gets the layer's own mark */
      snprintf(mark, sizeof(mark), "%s/anv_canary", d->dir);
      f = fopen(mark, "w");
      if (f) {
         fprintf(f, "xess-xmx layer recognised the self-test placeholder (unpatched driver)\n");
         fclose(f);
         xlog("self-test placeholder answered (%s)", mark);
      }
   }
   /* the layer's own mark: vkwrap (Vulkan games) needs the layer, whose descriptor push the patched driver lacks */
   snprintf(mark, sizeof(mark), "%s/layer_canary", d->dir);
   f = fopen(mark, "w");
   if (f) {
      fprintf(f, "xess-xmx layer: XeSS' Vulkan descriptor push served\n");
      fclose(f);
   }
}

/* a kernel the layer cannot serve: as with a failed compile, compile_failed.txt makes the shim use XeSS' generic
 * paths from its next launch on (once per kernel and process) */
static mtx_t failed_mtx;
static once_flag failed_once = ONCE_FLAG_INIT;
static void failed_init(void) { mtx_init(&failed_mtx, mtx_plain); }

static void
mark_failed(struct xdevice *d, const struct xmx_placeholder *ph)
{
   static char seen[128][17];
   static unsigned nseen;
   call_once(&failed_once, failed_init);
   mtx_lock(&failed_mtx);
   bool known = false;
   for (unsigned i = 0; i < nseen && !known; i++)
      known = !strcmp(seen[i], ph->hash);
   if (!known && nseen < 128)
      snprintf(seen[nseen++], 17, "%s", ph->hash);
   mtx_unlock(&failed_mtx);
   if (known || !d->dir[0])
      return;
   char mark[600];
   snprintf(mark, sizeof(mark), "%s/compile_failed.txt", d->dir);
   FILE *f = fopen(mark, "a");
   if (f) {
      fprintf(f, "%d %s\n", ph->kind == XMX_PH_LEGACY_KERNEL ? ph->dyn_id : -1, ph->hash);
      fclose(f);
   }
}

/* the replacement pipeline of a kernel placeholder; VK_NULL_HANDLE if it cannot be made (then the placeholder is
 * created as it is, and the kernel is marked failed for the shim's next launch). Not marked: a device the self-test
 * did not pass (the shim gets no self-test answer and does not use XMX at all) and the patched driver's own kernels. */
static VkPipeline
replace(struct xdevice *d, const VkComputePipelineCreateInfo *ci, const struct xmx_placeholder *ph)
{
   self_test(d);
   if (!d->ok)
      return VK_NULL_HANDLE;
   struct xpipelayout *pl = xmap_get(&pipe_layouts, HKEY(ci->layout));
   if (!pl) {
      xlog("kernel %s: unknown pipeline layout", ph->hash);
      mark_failed(d, ph);
      return VK_NULL_HANDLE;
   }
   if (d->L == &d->Lp && pl->nsets != 0)
      return VK_NULL_HANDLE;   /* patched driver: a D3D12 game's kernel, which the driver injects itself */
   char name[32];
   if (ph->kind == XMX_PH_LEGACY_KERNEL)
      snprintf(name, sizeof(name), "dyn_%d", ph->dyn_id);
   else
      snprintf(name, sizeof(name), "%s", ph->hash);
   uint8_t *img = NULL;
   size_t in;
   if (!xmx_kernel_get(&d->info, d->dir, ph->dyn_id, name, ph->hash, &img, &in)) {
      xlog("kernel %s: compile failed", ph->hash);
      return VK_NULL_HANDLE;
   }
   struct xmx_cml k;
   xmx_cml_parse(img, in, &k);
   struct xkernel *kr = calloc(1, sizeof(*kr));
   kr->dev = d;
   snprintf(kr->hash, sizeof(kr->hash), "%s", ph->hash);
   kr->prof = d->prof ? xprof_kernel(ph->hash) : -1;
   const uint32_t nsurf = xmx_rd32(k.hdr + XMX_H_NSURF);
   kr->sampler = (xmx_rd32(k.hdr + XMX_H_BARRIERS) & 0x80000000u) != 0;
   /* vkd3d-proton's heap and static sampler in the layout */
   const struct xsetlayout *heap = NULL;
   kr->heap_set = ~0u;
   unsigned nstatic = 0;
   kr->sampler_heap_set = ~0u;
   for (uint32_t s = 0; s < pl->nsets; s++) {
      const struct xsetlayout *l = xmap_get(&set_layouts, HKEY(pl->sets[s]));
      if (!l)
         continue;
      if (!heap && l->heap_binding != ~0u) {
         heap = l;
         kr->heap_set = s;
         kr->heap_binding = l->heap_binding;
         kr->heap_count = l->heap_count;
      }
      if (l->nstatic) {
         if (!nstatic)
            kr->static_sampler = l->static_sampler;
         nstatic += l->nstatic;
      }
      if (l->sampler_heap_binding != ~0u && kr->sampler_heap_set == ~0u) {
         kr->sampler_heap_set = s;
         kr->sampler_heap_binding = l->sampler_heap_binding;
      }
   }
   /* XeSS' Vulkan path (a Vulkan game, through vkwrap): a layout with no sets, the descriptors come from its push */
   const bool vkpush = pl->nsets == 0;
   kr->vkpush = vkpush;
   uint32_t base = ~0u;
   if (nsurf && pl->push_size >= 4 * nsurf)
      base = pl->push_size - 4 * nsurf;
   if (vkpush) {
      kr->sampler_mode = 3;
      if (nsurf > NSURF_BINDINGS) {
         xlog("kernel %s: %u surfaces, more than the layer's %u bindings", ph->hash, nsurf, NSURF_BINDINGS);
         mark_failed(d, ph);
         free(kr);
         free(img);
         return VK_NULL_HANDLE;
      }
   } else if (kr->sampler) {
      if (nstatic) {
         kr->sampler_mode = 0;
      } else if (kr->sampler_heap_set != ~0u && pl->push_size >= 4 * (nsurf + 1)) {
         /* a sampler descriptor table after the surfaces (patch 0005) */
         kr->sampler_mode = 1;
         base = pl->push_size - 4 * (nsurf + 1);
         kr->sampler_pc_off = pl->push_size - 4;
      } else {
         kr->sampler_mode = 2;
      }
   }
   if (!vkpush && nsurf && (!heap || base == ~0u || nsurf > 24)) {
      xlog("kernel %s: no vkd3d-style descriptor heap in the layout (%u sets) - VKD3D_DISABLE_EXTENSIONS="
           "VK_EXT_descriptor_buffer,VK_EXT_descriptor_heap set?", ph->hash, pl->nsets);
      mark_failed(d, ph);
      free(kr);
      free(img);
      return VK_NULL_HANDLE;
   }
   kr->nsurf = nsurf;
   bool pc_ok = kr->sampler_mode != 1 || kr->sampler_pc_off <= XMX_PC_BYTES - 4;
   for (uint32_t i = 0; i < nsurf && !vkpush; i++) {
      kr->table_off[i] = base + 4 * xmx_rd32(k.hdr + XMX_H_TABLE_IDX + 4 * i);
      kr->elem[i] = xmx_rd32(k.hdr + XMX_H_ELEM + 4 * i);
      pc_ok &= kr->table_off[i] <= XMX_PC_BYTES - 4;
   }
   if (!pc_ok) {
      /* prepare_dispatch reads the tables from the first XMX_PC_BYTES of push constants it keeps */
      xlog("kernel %s: descriptor tables beyond %u bytes of push constants (%u)", ph->hash, XMX_PC_BYTES,
           pl->push_size);
      mark_failed(d, ph);
      free(kr);
      free(img);
      return VK_NULL_HANDLE;
   }
   static const VkDescriptorType deftypes[] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER };
   mtx_lock(&d->mtx);
   /* the layer's mutable list: the four types the template uses, plus every type of vkd3d's heap list (a copy needs
    * the source's active type in the destination's list) */
   VkDescriptorType types[12];
   uint32_t ntypes = 4;
   memcpy(types, deftypes, sizeof(deftypes));
   if (vkpush) {   /* XeSS' push also has buffers */
      types[ntypes++] = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      types[ntypes++] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   }
   for (uint32_t i = 0; heap && i < heap->nmut; i++) {
      bool have = false;
      for (uint32_t j = 0; j < ntypes && !have; j++)
         have = types[j] == heap->mut_types[i];
      if (!have && ntypes < 8)
         types[ntypes++] = heap->mut_types[i];
   }
   struct xlayer_set *ls = layer_set(d, types, ntypes, heap ? heap->flags : 0);
   VkPipelineLayout el = ls ? ext_layout(d, pl, ls) : VK_NULL_HANDLE;
   struct xtemplate *t = el ? template_get(d, el, ls, pl->nsets) : NULL;
   mtx_unlock(&d->mtx);
   VkPipeline p = VK_NULL_HANDLE;
   char err[256] = "";
   size_t cn = 0;
   uint8_t *code = t ? xmx_full_code(&k, &cn) : NULL;
   if (code) {
      struct anvbin_kernel kc = { &k, code, (uint32_t)cn, pl->nsets, SAMPLER_BINDING, kr->sampler, 64 };
      struct anvbin b;
      if (anvbin_kernel(&t->bin, &kc, &b, err, sizeof(err))) {
         size_t bn;
         uint8_t *bd = anvbin_write(&b, &bn);
         VkResult r = VK_ERROR_UNKNOWN;
         if (bd)
            p = create_from_binary(d, bd, bn, el, &r);
         if (!p)
            snprintf(err, sizeof(err), "pipeline from binary: %d", r);
         free(bd);
      }
   }
   if (!p) {
      xlog("kernel %s: not replaced (%s); heap flags %#x, %u mutable types:", ph->hash,
           err[0] ? err : !t ? "no template" : "no prologue", heap ? heap->flags : 0, heap ? heap->nmut : 0);
      static int detailed;
      if (detailed++ < 2) {
         for (uint32_t i = 0; heap && i < heap->nmut; i++)
            xlog("   heap type %d", heap->mut_types[i]);
         for (uint32_t s = 0; s < pl->nsets; s++) {
            const struct xsetlayout *l = xmap_get(&set_layouts, HKEY(pl->sets[s]));
            xlog("   layout set %u: %s, flags %#x, heap binding %d (%u), sampler heap %d, static samplers %u", s,
                 l ? "known" : "unknown", l ? l->flags : 0, l ? (int)l->heap_binding : -2, l ? l->heap_count : 0,
                 l ? (int)l->sampler_heap_binding : -2, l ? l->nstatic : 0);
         }
         xlog("   push constants %u bytes in %u ranges; layer types %u", pl->push_size, pl->npc, ls ? ls->nmut : 0);
      }
      mark_failed(d, ph);
      free(code);
      free(kr);
      free(img);
      return VK_NULL_HANDLE;
   }
   kr->layout = el;
   kr->set_index = pl->nsets;
   kr->ls = ls;
   kr->lset = ls->layout;
   xmap_put(&pipelines, HKEY(p), kr);
   atomic_fetch_add(&replaced_count, 1);
   atomic_fetch_add(&pipe_epoch, 1);
   if (vkpush)
      xlog("kernel %s: replaced (%u surfaces from XeSS' Vulkan push, sampler %s, layer set 0)", ph->hash, nsurf,
           kr->sampler ? "pushed" : "none");
   else
      xlog("kernel %s: replaced (%u surfaces, heap set %u binding %u, sampler %s, layer set %u)", ph->hash, nsurf,
           kr->heap_set, kr->heap_binding,
           !kr->sampler ? "none" : kr->sampler_mode == 0 ? "static" : kr->sampler_mode == 1 ? "heap" : "default",
           pl->nsets);
   free(code);
   free(img);
   return p;
}

static VKAPI_ATTR VkResult VKAPI_CALL
x_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *cis,
                         const VkAllocationCallbacks *alloc, VkPipeline *out)
{
   struct xdevice *d = dev_of(device);
   bool any = false;
   struct xmx_placeholder ph_buf[16];
   struct xmx_placeholder *ph = n <= 16 ? ph_buf : malloc(n * sizeof(*ph));
   if (!ph)
      return d->CreateComputePipelines(device, cache, n, cis, alloc, out);
   for (uint32_t i = 0; i < n; i++) {
      const VkPipelineShaderStageCreateInfo *st = &cis[i].stage;
      memset(&ph[i], 0, sizeof(ph[i]));
      if (st->module != VK_NULL_HANDLE) {
         const struct xmodule *m = xmap_get(&modules, HKEY(st->module));
         if (m)
            ph[i] = m->ph;
      } else {
         for (const VkBaseInStructure *s = st->pNext; s; s = s->pNext)
            if (s->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
               const VkShaderModuleCreateInfo *m = (const void *)s;
               classify_code(d, m->pCode, m->codeSize, &ph[i]);
            }
      }
      any |= ph[i].kind != XMX_PH_NONE;
   }
   if (!any) {
      if (ph != ph_buf)
         free(ph);
      return d->CreateComputePipelines(device, cache, n, cis, alloc, out);
   }
   VkResult res = VK_SUCCESS;
   for (uint32_t i = 0; i < n; i++) {
      out[i] = VK_NULL_HANDLE;
      if (ph[i].kind == XMX_PH_CANARY || ph[i].kind == XMX_PH_LEGACY_CANARY)
         answer_canary(d, &cis[i], &ph[i]);
      if (ph[i].kind == XMX_PH_KERNEL || ph[i].kind == XMX_PH_LEGACY_KERNEL)
         out[i] = replace(d, &cis[i], &ph[i]);
      if (!out[i]) {
         VkResult r = d->CreateComputePipelines(device, cache, 1, &cis[i], alloc, &out[i]);
         if (r && res == VK_SUCCESS)
            res = r;
      }
   }
   if (ph != ph_buf)
      free(ph);
   return res;
}

static VKAPI_ATTR void VKAPI_CALL
x_DestroyPipeline(VkDevice device, VkPipeline p, const VkAllocationCallbacks *alloc)
{
   struct xdevice *d = dev_of(device);
   struct xkernel *kr = p ? xmap_take(&pipelines, HKEY(p)) : NULL;
   if (kr) {
      atomic_fetch_sub(&replaced_count, 1);
      atomic_fetch_add(&pipe_epoch, 1);
      free(kr);
   }
   d->DestroyPipeline(device, p, alloc);
}

/* ---- command buffers ---- */

/* XMX_LAYER_PROF: the time stamps of the last recording. The command buffer is begun, reset or freed again only when
 * its last submission is complete, so they are available. */
static void
prof_harvest(struct xcmd *c)
{
   if (c->qlost)
      xprof_lost(c->qlost);
   c->qlost = 0;
   if (!c->qp || !c->nq) {
      c->nq = 0;
      return;
   }
   struct xdevice *d = c->dev;
   uint64_t ts[PROF_QUERIES];
   VkResult r = d->GetQueryPoolResults(d->handle, c->qp, 0, c->nq, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT);
   if (r == VK_SUCCESS) {
      for (uint32_t i = 0; i + 1 < c->nq; i += 2) {
         const uint64_t dt = (ts[i + 1] - ts[i]) & d->ts_mask;
         xprof_add(c->qk[i / 2], (uint64_t)(dt * d->ts_period));
      }
   } else {
      xprof_lost(c->nq / 2);
   }
   c->nq = 0;
}

/* a time stamp pair around a dispatch of a replaced kernel; both at the bottom of the pipe, so the first waits for
 * the work before the dispatch and the second for the dispatch */
static bool
prof_pool(struct xcmd *c)
{
   struct xdevice *d = c->dev;
   if (!c->qp) {
      VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, 0, 0, VK_QUERY_TYPE_TIMESTAMP,
                                   PROF_QUERIES, 0 };
      if (d->CreateQueryPool(d->handle, &qi, NULL, &c->qp)) {
         c->qp = VK_NULL_HANDLE;
         return false;
      }
      d->CmdResetQueryPool(c->handle, c->qp, 0, PROF_QUERIES);
   }
   if (c->nq + 2 > PROF_QUERIES) {
      c->qlost++;
      return false;
   }
   return true;
}

/* mode 2: the first replaced dispatch after other work starts a run (its time stamp waits for that work); the next
 * bind of another pipeline or the end of the command buffer ends it */
static void
prof_run_start(struct xcmd *c)
{
   struct xdevice *d = c->dev;
   if (c->in_run || !c->primary || !prof_pool(c))
      return;
   c->qk[c->nq / 2] = d->prof_run;
   d->CmdWriteTimestamp(c->handle, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->qp, c->nq++);
   c->in_run = true;
}

static void
prof_run_end(struct xcmd *c)
{
   if (!c->in_run)
      return;
   c->dev->CmdWriteTimestamp(c->handle, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->qp, c->nq++);
   c->in_run = false;
}

static bool
prof_before(struct xcmd *c)
{
   struct xdevice *d = c->dev;
   if (!d->prof || !c->primary || c->bound->prof < 0)
      return false;
   if (d->prof_mode == 2) {
      prof_run_start(c);
      return false;
   }
   if (!c->qp) {
      VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, 0, 0, VK_QUERY_TYPE_TIMESTAMP,
                                   PROF_QUERIES, 0 };
      if (d->CreateQueryPool(d->handle, &qi, NULL, &c->qp)) {
         c->qp = VK_NULL_HANDLE;
         return false;
      }
      d->CmdResetQueryPool(c->handle, c->qp, 0, PROF_QUERIES);
   }
   if (c->nq + 2 > PROF_QUERIES) {
      c->qlost++;
      return false;
   }
   c->qk[c->nq / 2] = c->bound->prof;
   d->CmdWriteTimestamp(c->handle, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->qp, c->nq++);
   return true;
}

static void
prof_after(struct xcmd *c)
{
   c->dev->CmdWriteTimestamp(c->handle, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->qp, c->nq++);
}

static void
cmd_release_pools(struct xcmd *c)
{
   prof_harvest(c);
   if (c->qp)
      c->dev->DestroyQueryPool(c->dev->handle, c->qp, NULL);
   c->qp = VK_NULL_HANDLE;
   for (unsigned i = 0; i < c->npools; i++)
      c->dev->DestroyDescriptorPool(c->dev->handle, c->pools[i], NULL);
   c->npools = 0;
   c->pools_full = 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL
x_AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out)
{
   struct xdevice *d = dev_of(device);
   VkResult r = d->AllocateCommandBuffers(device, ai, out);
   if (r)
      return r;
   struct xcmdpool *cp = xmap_get(&cmdpools, HKEY(ai->commandPool));
   if (!cp) {
      cp = calloc(1, sizeof(*cp));
      cp->dev = d;
      cp->handle = ai->commandPool;
      xmap_put(&cmdpools, HKEY(ai->commandPool), cp);
   }
   for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
      struct xcmd *c = calloc(1, sizeof(*c));
      c->handle = out[i];
      c->pool = cp;
      c->next = cp->cmds;
      if (cp->cmds)
         cp->cmds->prev = c;
      cp->cmds = c;
      c->dev = d;
      c->primary = ai->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      xmap_put(&cmdbufs, (uint64_t)(uintptr_t)out[i], c);
   }
   return r;
}

static VKAPI_ATTR void VKAPI_CALL
x_FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t n, const VkCommandBuffer *cbs)
{
   struct xdevice *d = dev_of(device);
   atomic_fetch_add(&cmd_epoch, 1);
   for (uint32_t i = 0; i < n; i++) {
      struct xcmd *c = cbs[i] ? xmap_take(&cmdbufs, (uint64_t)(uintptr_t)cbs[i]) : NULL;
      if (c) {
         if (c->prev)
            c->prev->next = c->next;
         else
            c->pool->cmds = c->next;
         if (c->next)
            c->next->prev = c->prev;
         cmd_release_pools(c);
         free(c);
      }
   }
   d->FreeCommandBuffers(device, pool, n, cbs);
}

static void
cmd_reset(struct xcmd *c)
{
   prof_harvest(c);
   c->in_run = false;
   c->bound = NULL;
   c->vk_pushed = false;
   memset(c->sets, 0, sizeof(c->sets));
   for (unsigned i = 0; i < c->npools; i++)
      c->dev->ResetDescriptorPool(c->dev->handle, c->pools[i], 0);
   c->pools_full = 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL
x_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
   struct xcmd *c = cmd_of(cb);
   struct xdevice *d = c ? c->dev : dev_of(cb);
   if (c)
      cmd_reset(c);
   VkResult r = d->BeginCommandBuffer(cb, bi);
   if (c && c->qp && r == VK_SUCCESS)
      d->CmdResetQueryPool(cb, c->qp, 0, PROF_QUERIES);
   return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
x_EndCommandBuffer(VkCommandBuffer cb)
{
   struct xcmd *c = cmd_of(cb);
   struct xdevice *d = c ? c->dev : dev_of(cb);
   if (c)
      prof_run_end(c);
   return d->EndCommandBuffer(cb);
}

static VKAPI_ATTR VkResult VKAPI_CALL
x_ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f)
{
   struct xcmd *c = cmd_of(cb);
   struct xdevice *d = c ? c->dev : dev_of(cb);
   if (c)
      cmd_reset(c);
   return d->ResetCommandBuffer(cb, f);
}

/* the pool's command buffers and the pool itself (already out of cmdpools) */
static void
cmdpool_free(struct xcmdpool *cp)
{
   for (struct xcmd *c = cp->cmds, *n; c; c = n) {
      n = c->next;
      xmap_take(&cmdbufs, (uint64_t)(uintptr_t)c->handle);
      cmd_release_pools(c);
      free(c);
   }
   atomic_fetch_add(&cmd_epoch, 1);
   free(cp);
}

static VKAPI_ATTR VkResult VKAPI_CALL
x_ResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags f)
{
   struct xdevice *d = dev_of(device);
   struct xcmdpool *cp = xmap_get(&cmdpools, HKEY(pool));
   for (struct xcmd *c = cp ? cp->cmds : NULL; c; c = c->next)
      cmd_reset(c);
   return d->ResetCommandPool(device, pool, f);
}

static VKAPI_ATTR void VKAPI_CALL
x_DestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *alloc)
{
   struct xdevice *d = dev_of(device);
   struct xcmdpool *cp = pool ? xmap_take(&cmdpools, HKEY(pool)) : NULL;
   if (cp)
      cmdpool_free(cp);
   d->DestroyCommandPool(device, pool, alloc);
}

static VKAPI_ATTR void VKAPI_CALL
x_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
   if (bp != VK_PIPELINE_BIND_POINT_COMPUTE) {
      struct xdevice *d = dev_of(cb);
      if (d->prof_mode == 2) {
         struct xcmd *c = cmd_of(cb);
         if (c)
            prof_run_end(c);
      }
      d->CmdBindPipeline(cb, bp, p);
      return;
   }
   struct xcmd *c = cmd_of(cb);
   struct xdevice *d = c ? c->dev : dev_of(cb);
   if (c) {
      c->bound = atomic_load_explicit(&replaced_count, memory_order_relaxed) ? kernel_of(p) : NULL;
      if (!c->bound)
         prof_run_end(c);
   }
   d->CmdBindPipeline(cb, bp, p);
}

static VKAPI_ATTR void VKAPI_CALL
x_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first,
                        uint32_t n, const VkDescriptorSet *sets, uint32_t ndyn, const uint32_t *dyn)
{
   if (bp != VK_PIPELINE_BIND_POINT_COMPUTE) {
      dev_of(cb)->CmdBindDescriptorSets(cb, bp, layout, first, n, sets, ndyn, dyn);
      return;
   }
   struct xcmd *c = cmd_of(cb);
   struct xdevice *d = c ? c->dev : dev_of(cb);
   if (c && bp == VK_PIPELINE_BIND_POINT_COMPUTE)
      for (uint32_t i = 0; i < n && first + i < 8; i++)
         c->sets[first + i] = sets[i];
   d->CmdBindDescriptorSets(cb, bp, layout, first, n, sets, ndyn, dyn);
}

static VKAPI_ATTR void VKAPI_CALL
x_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t off,
                   uint32_t size, const void *data)
{
   if (!(stages & VK_SHADER_STAGE_COMPUTE_BIT)) {
      dev_of(cb)->CmdPushConstants(cb, layout, stages, off, size, data);
      return;
   }
   struct xcmd *c = cmd_of(cb);
   struct xdevice *d = c ? c->dev : dev_of(cb);
   if (c && (stages & VK_SHADER_STAGE_COMPUTE_BIT) && off < sizeof(c->pc))
      memcpy(c->pc + off, data, size <= sizeof(c->pc) - off ? size : sizeof(c->pc) - off);
   d->CmdPushConstants(cb, layout, stages, off, size, data);
}

/* XeSS' descriptor push in a Vulkan game (vkwrap, XMX_VK_PUSH_SET): kept for the next dispatches, never passed on.
 * Samplers first, then the resources in the order of the kernel's surfaces, one descriptor per write. */
static VKAPI_ATTR void VKAPI_CALL
x_CmdPushDescriptorSetKHR(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set,
                          uint32_t n, const VkWriteDescriptorSet *w)
{
   if (set != XMX_VK_PUSH_SET || layout != VK_NULL_HANDLE) {
      struct xdevice *d = dev_of(cb);
      if (d->CmdPushDescriptorSetKHR)
         d->CmdPushDescriptorSetKHR(cb, bp, layout, set, n, w);
      return;
   }
   struct xcmd *c = cmd_of(cb);
   if (!c)
      return;
   c->vk_n = 0;
   c->vk_sampler = VK_NULL_HANDLE;
   for (uint32_t i = 0; i < n; i++) {
      const VkDescriptorType t = w[i].descriptorType;
      if (t == VK_DESCRIPTOR_TYPE_SAMPLER) {
         if (!c->vk_sampler && w[i].pImageInfo)
            c->vk_sampler = w[i].pImageInfo->sampler;
         continue;
      }
      if (c->vk_n == NSURF_BINDINGS)
         break;
      const uint32_t k = c->vk_n++;
      c->vk_type[k] = t;
      if (t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
         if (w[i].pBufferInfo)
            c->vk_buf[k] = w[i].pBufferInfo[0];
      } else if (w[i].pImageInfo) {
         c->vk_img[k] = w[i].pImageInfo[0];
      }
   }
   c->vk_pushed = true;
}

/* a descriptor set of the layer's layout for this command buffer; a pool that ran out is not tried again until the
 * command buffer is reset */
static VkDescriptorSet
cmd_alloc_set(struct xcmd *c, struct xlayer_set *ls)
{
   struct xdevice *d = c->dev;
   for (unsigned i = 0; i < c->npools; i++) {
      if (c->pool_ls[i] != ls || (c->pools_full >> i & 1))
         continue;
      VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, c->pools[i], 1, &ls->layout };
      VkDescriptorSet s;
      if (d->AllocateDescriptorSets(d->handle, &ai, &s) == VK_SUCCESS)
         return s;
      c->pools_full |= 1ull << i;
   }
   if (c->npools == 64)
      return VK_NULL_HANDLE;
   VkMutableDescriptorTypeListEXT lists[2] = { { ls->nmut, ls->mut_types }, { 0, 0 } };
   VkMutableDescriptorTypeCreateInfoEXT mci = { VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT, 0, 2, lists };
   VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_MUTABLE_EXT, SETS_PER_POOL * NSURF_BINDINGS },
                                     { VK_DESCRIPTOR_TYPE_SAMPLER, SETS_PER_POOL } };
   VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, &mci,
                                      (ls->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) ?
                                         VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0,
                                      SETS_PER_POOL, 2, sizes };
   if (d->CreateDescriptorPool(d->handle, &pci, NULL, &c->pools[c->npools]))
      return VK_NULL_HANDLE;
   c->pool_ls[c->npools] = ls;
   VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, c->pools[c->npools++], 1,
                                      &ls->layout };
   VkDescriptorSet s;
   return d->AllocateDescriptorSets(d->handle, &ai, &s) == VK_SUCCESS ? s : VK_NULL_HANDLE;
}

/* before a dispatch of a replaced pipeline: the layer's set from vkd3d's heap, bound at its index. false if the set
 * cannot be made: the dispatch is then left out, since the kernel would run with no or a stale set of the layer's
 * (a GPU fault). A null set is no way out: it needs a descriptor set just the same, and without the heap there is
 * nothing valid to copy into it. */
static bool
prepare_dispatch(struct xcmd *c)
{
   struct xkernel *k = c->bound;
   struct xdevice *d = c->dev;
   if (k->vkpush) {
      /* a Vulkan game: surface i is the i-th resource of XeSS' last push (vkCmdPushXeSSDescriptorsINTEL) */
      if (!c->vk_pushed || c->vk_n < k->nsurf) {
         static atomic_bool said;
         if (!atomic_exchange(&said, true))
            xlog("kernel %s: dispatched with %u pushed resources for %u surfaces: dispatches left out", k->hash,
                 c->vk_pushed ? c->vk_n : 0, k->nsurf);
         return false;
      }
      VkDescriptorSet s = cmd_alloc_set(c, k->ls);
      if (!s)
         return false;
      VkWriteDescriptorSet w[NSURF_BINDINGS + 1];
      for (uint32_t i = 0; i < k->nsurf; i++) {
         const bool buf = c->vk_type[i] == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                          c->vk_type[i] == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
         w[i] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, s, i, 0, 1, c->vk_type[i],
                                        buf ? NULL : &c->vk_img[i], buf ? &c->vk_buf[i] : NULL, NULL };
      }
      VkDescriptorImageInfo sii = { c->vk_sampler && k->sampler ? c->vk_sampler : d->default_sampler };
      w[k->nsurf] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, s, SAMPLER_BINDING, 0, 1,
                                            VK_DESCRIPTOR_TYPE_SAMPLER, &sii, NULL, NULL };
      d->UpdateDescriptorSets(d->handle, k->nsurf + 1, w, 0, NULL);
      d->CmdBindDescriptorSets(c->handle, VK_PIPELINE_BIND_POINT_COMPUTE, k->layout, k->set_index, 1, &s, 0, NULL);
      return true;
   }
   const VkDescriptorSet heap = k->heap_set < 8 ? c->sets[k->heap_set] : VK_NULL_HANDLE;
   if (k->nsurf && !heap) {
      static atomic_bool said;
      if (!atomic_exchange(&said, true))
         xlog("kernel %s dispatched without its descriptor heap (set %u) bound: dispatches left out", k->hash,
              k->heap_set);
      return false;
   }
   VkDescriptorSet s = cmd_alloc_set(c, k->ls);
   if (!s) {
      static atomic_bool said;
      if (!atomic_exchange(&said, true))
         xlog("kernel %s: no descriptor set for the dispatch: dispatches left out", k->hash);
      return false;
   }
   VkCopyDescriptorSet cp[NSURF_BINDINGS + 1];
   uint32_t ncp = 0;
   uint32_t pc[XMX_PC_BYTES / 4];
   memcpy(pc, c->pc, sizeof(pc));
   for (uint32_t i = 0; i < k->nsurf; i++) {
      uint32_t idx = pc[k->table_off[i] / 4] + k->elem[i];
      if (idx >= k->heap_count)
         idx = 0;   /* as a null surface would be: a valid descriptor instead of a wild one */
      cp[ncp++] = (VkCopyDescriptorSet){ VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, 0, heap, k->heap_binding, idx, s, i, 0, 1 };
   }
   VkDescriptorImageInfo sii = { 0 };
   VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, s, SAMPLER_BINDING, 0, 1,
                              VK_DESCRIPTOR_TYPE_SAMPLER, &sii };
   uint32_t nw = 0;
   if (k->sampler) {
      const VkDescriptorSet sh = k->sampler_heap_set < 8 ? c->sets[k->sampler_heap_set] : VK_NULL_HANDLE;
      if (k->sampler_mode == 1 && sh) {
         cp[ncp++] = (VkCopyDescriptorSet){ VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, 0, sh, k->sampler_heap_binding,
                                            pc[k->sampler_pc_off / 4], s, SAMPLER_BINDING, 0, 1 };
      } else {
         sii.sampler = k->sampler_mode == 0 ? k->static_sampler : d->default_sampler;
         nw = 1;
      }
   } else {
      sii.sampler = d->default_sampler;   /* every binding of the set gets a valid descriptor */
      nw = 1;
   }
   d->UpdateDescriptorSets(d->handle, nw, &w, ncp, cp);
   d->CmdBindDescriptorSets(c->handle, VK_PIPELINE_BIND_POINT_COMPUTE, k->layout, k->set_index, 1, &s, 0, NULL);
   return true;
}

static VKAPI_ATTR void VKAPI_CALL
x_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z)
{
   struct xcmd *c = cmd_of(cb);
   struct xdevice *d = c ? c->dev : dev_of(cb);
   if (c && c->bound && !prepare_dispatch(c))
      return;
   const bool t = c && c->bound && prof_before(c);
   d->CmdDispatch(cb, x, y, z);
   if (t)
      prof_after(c);
}

static VKAPI_ATTR void VKAPI_CALL
x_CmdDispatchBase(VkCommandBuffer cb, uint32_t bx, uint32_t by, uint32_t bz, uint32_t x, uint32_t y, uint32_t z)
{
   struct xcmd *c = cmd_of(cb);
   struct xdevice *d = c ? c->dev : dev_of(cb);
   if (c && c->bound && !prepare_dispatch(c))
      return;
   const bool t = c && c->bound && prof_before(c);
   d->CmdDispatchBase(cb, bx, by, bz, x, y, z);
   if (t)
      prof_after(c);
}

static VKAPI_ATTR void VKAPI_CALL
x_CmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off)
{
   struct xcmd *c = cmd_of(cb);
   struct xdevice *d = c ? c->dev : dev_of(cb);
   if (c && c->bound && !prepare_dispatch(c))
      return;
   const bool t = c && c->bound && prof_before(c);
   d->CmdDispatchIndirect(cb, b, off);
   if (t)
      prof_after(c);
}

static VKAPI_ATTR VkResult VKAPI_CALL
x_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
   struct xdevice *d = dev_of(queue);
   if (d->prof)
      xprof_present(xlog);
   return d->QueuePresentKHR(queue, pi);
}

/* ---- device ---- */
/* the per-handle entries of a device: their handles are reused once it is gone (each struct starts with its device) */
static int of_dev(void *v, void *u) { return *(struct xdevice **)v == u; }
static void free_entry(void *v, void *u) { (void)u; free(v); }
static void free_kernel(void *v, void *u) { (void)u; atomic_fetch_sub(&replaced_count, 1); free(v); }
struct pool_purge { struct xdevice *dev; struct xcmdpool *list; };
static int pool_of_dev(void *v, void *u) { return ((struct xcmdpool *)v)->dev == ((struct pool_purge *)u)->dev; }
static void
pool_collect(void *v, void *u)
{
   struct xcmdpool *cp = v;
   struct pool_purge *pp = u;
   cp->next = pp->list;
   pp->list = cp;
}

static VKAPI_ATTR void VKAPI_CALL
x_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
   struct xdevice *d = xmap_take(&devices, DKEY(device));
   if (!d)
      return;
   if (d->prof)
      xprof_final(xlog);
   struct xdevice *cached = d;
   atomic_compare_exchange_strong(&last_dev, &cached, NULL);
   PFN_vkDestroyDevice destroy = d->DestroyDevice;
   xmap_take_if(&modules, of_dev, free_entry, d);
   xmap_take_if(&set_layouts, of_dev, free_entry, d);
   xmap_take_if(&pipe_layouts, of_dev, free_entry, d);
   xmap_take_if(&pipelines, of_dev, free_kernel, d);
   atomic_fetch_add(&pipe_epoch, 1);
   /* command pools: collected under the map's lock, their descriptor pools destroyed outside it */
   struct pool_purge pp = { d, NULL };
   xmap_take_if(&cmdpools, pool_of_dev, pool_collect, &pp);
   for (struct xcmdpool *cp = pp.list, *n; cp; cp = n) {
      n = cp->next;
      cmdpool_free(cp);
   }
   if (d->default_sampler)
      d->DestroySampler(device, d->default_sampler, NULL);
   for (struct xtemplate *t = d->templates, *n; t; t = n) {
      n = t->next;
      free(t->data);
      free(t);
   }
   for (struct xlayer_set *l = d->lsets, *n; l; l = n) {
      n = l->next;
      d->DestroyDescriptorSetLayout(device, l->layout, NULL);
      free(l);
   }
   free(d);
   destroy(device, alloc);
}

static bool
has_ext(const VkDeviceCreateInfo *ci, const char *name)
{
   for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
      if (!strcmp(ci->ppEnabledExtensionNames[i], name))
         return true;
   return false;
}

static bool
phys_has_ext(struct xinstance *in, VkPhysicalDevice phys, const char *name)
{
   uint32_t n = 0;
   in->EnumerateDeviceExtensionProperties(phys, NULL, &n, NULL);
   VkExtensionProperties *p = calloc(n ? n : 1, sizeof(*p));
   in->EnumerateDeviceExtensionProperties(phys, NULL, &n, p);
   bool r = false;
   for (uint32_t i = 0; i < n && !r; i++)
      r = !strcmp(p[i].extensionName, name);
   free(p);
   return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
x_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci_in, const VkAllocationCallbacks *alloc,
               VkDevice *out)
{
   call_once(&maps_once, maps_init);
   struct xinstance *in = xmap_get(&physdevs, HKEY((uintptr_t)phys));
   VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)ci_in->pNext;
   while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
      link = (VkLayerDeviceCreateInfo *)link->pNext;
   if (!link || !in)
      return VK_ERROR_INITIALIZATION_FAILED;
   PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
   link->u.pLayerInfo = link->u.pLayerInfo->pNext;
   PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(in->handle, "vkCreateDevice");

   /* pipeline binaries (and maintenance5, which they need) on, if the device has them */
   VkDeviceCreateInfo ci = *ci_in;
   const char *exts[256];
   bool want = phys_has_ext(in, phys, "VK_KHR_pipeline_binary") && phys_has_ext(in, phys, "VK_KHR_maintenance5") &&
               ci.enabledExtensionCount < 250;
   VkPhysicalDevicePipelineBinaryFeaturesKHR pbf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, 0, 1 };
   VkPhysicalDeviceMaintenance5FeaturesKHR m5f = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, 0, 1 };
   if (want) {
      memcpy(exts, ci.ppEnabledExtensionNames, ci.enabledExtensionCount * sizeof(char *));
      uint32_t ne = ci.enabledExtensionCount;
      if (!has_ext(&ci, "VK_KHR_pipeline_binary"))
         exts[ne++] = "VK_KHR_pipeline_binary";
      if (!has_ext(&ci, "VK_KHR_maintenance5"))
         exts[ne++] = "VK_KHR_maintenance5";
      ci.enabledExtensionCount = ne;
      ci.ppEnabledExtensionNames = exts;
      /* features: set them where the application's chain already has the structure, else append ours */
      bool have_pb = false, have_m5 = false;
      for (VkBaseOutStructure *s = (VkBaseOutStructure *)ci.pNext; s; s = s->pNext) {
         if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR) {
            ((VkPhysicalDevicePipelineBinaryFeaturesKHR *)s)->pipelineBinaries = VK_TRUE;
            have_pb = true;
         } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR) {
            ((VkPhysicalDeviceMaintenance5FeaturesKHR *)s)->maintenance5 = VK_TRUE;
            have_m5 = true;
         } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES) {
            ((VkPhysicalDeviceVulkan14Features *)s)->maintenance5 = VK_TRUE;
            have_m5 = true;
         }
      }
      void *chain = (void *)ci.pNext;
      if (!have_m5) {
         m5f.pNext = chain;
         chain = &m5f;
      }
      if (!have_pb) {
         pbf.pNext = chain;
         chain = &pbf;
      }
      ci.pNext = chain;
   }
   VkResult r = create(phys, &ci, alloc, out);
   if (r && want) {
      xlog("device with pipeline binaries failed (%d), created without the layer's features", r);
      r = create(phys, ci_in, alloc, out);
      want = false;
   }
   if (r)
      return r;
   struct xdevice *d = calloc(1, sizeof(*d));
   d->handle = *out;
   d->key = DKEY(*out);
   d->phys = phys;
   d->inst = in;
   d->gdpa = gdpa;
   d->binaries = want;
   mtx_init(&d->mtx, mtx_plain);
#define X(n) d->n = (PFN_vk##n)gdpa(*out, "vk" #n);
   DEVICE_FNS(X)
#undef X
   VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
   in->GetPhysicalDeviceProperties2(phys, &p2);
   d->info.pci_id = p2.properties.deviceID;
   d->info.grf_bytes = ((d->info.pci_id & 0xff00) == 0x5600 || (d->info.pci_id & 0xff00) == 0x7d00) ? 32 : 64;
   d->prof = xprof_enabled() && d->CreateQueryPool && d->CmdWriteTimestamp && d->GetQueryPoolResults &&
             p2.properties.limits.timestampComputeAndGraphics;
   if (d->prof) {
      d->ts_period = p2.properties.limits.timestampPeriod;
      uint32_t nf = 0;
      in->GetPhysicalDeviceQueueFamilyProperties(phys, &nf, NULL);
      VkQueueFamilyProperties qf[16];
      nf = nf > 16 ? 16 : nf;
      in->GetPhysicalDeviceQueueFamilyProperties(phys, &nf, qf);
      uint32_t bits = 64;
      for (uint32_t i = 0; i < nf; i++)
         if (qf[i].timestampValidBits && qf[i].timestampValidBits < bits)
            bits = qf[i].timestampValidBits;
      d->ts_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
      d->prof_mode = xprof_mode();
      d->prof_run = d->prof_mode == 2 ? xprof_kernel("runs") : -1;
      xlog("profiling: time stamp period %.3f ns, %u valid bits", d->ts_period, bits);
   }
   const char *sd = getenv("ANV_CM_SPV_DIR"), *pfx = getenv("WINEPREFIX");
   if (sd && *sd)
      snprintf(d->dir, sizeof(d->dir), "%s", sd);
   else if (pfx && *pfx)
      snprintf(d->dir, sizeof(d->dir), "%s/drive_c/igdext_kernels", pfx);
   VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, 0, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                               VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               0, 0, 1, 0, VK_COMPARE_OP_NEVER, 0, 14 };
   d->CreateSampler(*out, &sci, NULL, &d->default_sampler);
   xmap_put(&devices, DKEY(*out), d);
   xlog("device %s (0x%04x), pipeline binaries %s, kernel folder %s", p2.properties.deviceName, d->info.pci_id,
        want ? "on" : "unavailable", d->dir[0] ? d->dir : "unset");
   return VK_SUCCESS;
}

/* vkd3d-proton must give the layer SPIR-V: a module identifier would let it skip the code */
static VKAPI_ATTR VkResult VKAPI_CALL
x_EnumerateDeviceExtensionProperties(VkPhysicalDevice phys, const char *layer, uint32_t *n, VkExtensionProperties *p)
{
   if (layer && !strcmp(layer, "VK_LAYER_XESS_xmx")) {
      *n = 0;
      return VK_SUCCESS;
   }
   struct xinstance *in = xmap_get(&physdevs, HKEY((uintptr_t)phys));
   if (!in)
      return VK_ERROR_INITIALIZATION_FAILED;
   uint32_t m = 0;
   VkResult r = in->EnumerateDeviceExtensionProperties(phys, layer, &m, NULL);
   if (r)
      return r;
   VkExtensionProperties *all = calloc(m ? m : 1, sizeof(*all));
   r = in->EnumerateDeviceExtensionProperties(phys, layer, &m, all);
   uint32_t k = 0;
   for (uint32_t i = 0; i < m; i++)
      if (strcmp(all[i].extensionName, "VK_EXT_shader_module_identifier"))
         all[k++] = all[i];
   if (!p) {
      *n = k;
   } else {
      const uint32_t c = *n < k ? *n : k;
      memcpy(p, all, c * sizeof(*p));
      r = c < k ? VK_INCOMPLETE : r;
      *n = c;
   }
   free(all);
   return r;
}

static VKAPI_ATTR void VKAPI_CALL
x_GetPhysicalDeviceFeatures2(VkPhysicalDevice phys, VkPhysicalDeviceFeatures2 *f)
{
   struct xinstance *in = xmap_get(&physdevs, HKEY((uintptr_t)phys));
   if (!in)
      return;
   in->GetPhysicalDeviceFeatures2(phys, f);
   for (VkBaseOutStructure *s = (VkBaseOutStructure *)f->pNext; s; s = s->pNext)
      if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT)
         ((VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT *)s)->shaderModuleIdentifier = VK_FALSE;
}

/* ---- instance ---- */
static VKAPI_ATTR VkResult VKAPI_CALL
x_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *out)
{
   call_once(&maps_once, maps_init);
   VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)ci->pNext;
   while (link &&
          !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
      link = (VkLayerInstanceCreateInfo *)link->pNext;
   if (!link)
      return VK_ERROR_INITIALIZATION_FAILED;
   PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   link->u.pLayerInfo = link->u.pLayerInfo->pNext;
   PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
   VkResult r = create(ci, alloc, out);
   if (r)
      return r;
   struct xinstance *in = calloc(1, sizeof(*in));
   in->handle = *out;
   in->gipa = gipa;
   in->DestroyInstance = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
   in->EnumerateDeviceExtensionProperties =
      (PFN_vkEnumerateDeviceExtensionProperties)gipa(*out, "vkEnumerateDeviceExtensionProperties");
   in->GetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(*out, "vkGetPhysicalDeviceProperties2");
   in->GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(*out, "vkGetPhysicalDeviceFeatures2");
   in->GetPhysicalDeviceQueueFamilyProperties =
      (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(*out, "vkGetPhysicalDeviceQueueFamilyProperties");
   xmap_put(&instances, DKEY(*out), in);
   /* physical devices -> instance */
   PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(*out, "vkEnumeratePhysicalDevices");
   uint32_t n = 0;
   enumerate(*out, &n, NULL);
   VkPhysicalDevice pds[16];
   if (n > 16)
      n = 16;
   enumerate(*out, &n, pds);
   for (uint32_t i = 0; i < n; i++)
      xmap_put(&physdevs, HKEY((uintptr_t)pds[i]), in);
   return r;
}

static VKAPI_ATTR void VKAPI_CALL
x_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
   struct xinstance *in = xmap_take(&instances, DKEY(instance));
   if (!in)
      return;
   PFN_vkDestroyInstance destroy = in->DestroyInstance;
   free(in);
   destroy(instance, alloc);
}

/* ---- proc addr ---- */
#define HOOK(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)x_##n
#define DEVICE_HOOKS \
   HOOK(DestroyDevice); HOOK(CreateShaderModule); HOOK(DestroyShaderModule); HOOK(CreateComputePipelines); \
   HOOK(DestroyPipeline); HOOK(CreateDescriptorSetLayout); HOOK(DestroyDescriptorSetLayout); \
   HOOK(CreatePipelineLayout); HOOK(DestroyPipelineLayout); HOOK(AllocateCommandBuffers); \
   HOOK(FreeCommandBuffers); HOOK(BeginCommandBuffer); HOOK(ResetCommandBuffer); HOOK(ResetCommandPool); \
   HOOK(DestroyCommandPool); HOOK(CmdBindPipeline); HOOK(CmdBindDescriptorSets); HOOK(CmdPushConstants); \
   HOOK(CmdDispatch); HOOK(CmdDispatchBase); HOOK(CmdDispatchIndirect); HOOK(QueuePresentKHR); HOOK(EndCommandBuffer); \
   HOOK(CmdPushDescriptorSetKHR)

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
xmx_GetDeviceProcAddr(VkDevice device, const char *name)
{
   DEVICE_HOOKS;
   if (!strcmp(name, "vkCmdDispatchBaseKHR"))
      return (PFN_vkVoidFunction)x_CmdDispatchBase;
   if (!strcmp(name, "vkGetDeviceProcAddr"))
      return (PFN_vkVoidFunction)xmx_GetDeviceProcAddr;
   struct xdevice *d = dev_of(device);
   return d ? d->gdpa(device, name) : NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
xmx_GetInstanceProcAddr(VkInstance instance, const char *name)
{
   HOOK(CreateInstance);
   HOOK(DestroyInstance);
   HOOK(CreateDevice);
   HOOK(EnumerateDeviceExtensionProperties);
   HOOK(GetPhysicalDeviceFeatures2);
   if (!strcmp(name, "vkGetPhysicalDeviceFeatures2KHR"))
      return (PFN_vkVoidFunction)x_GetPhysicalDeviceFeatures2;
   DEVICE_HOOKS;
   if (!strcmp(name, "vkGetInstanceProcAddr"))
      return (PFN_vkVoidFunction)xmx_GetInstanceProcAddr;
   if (!strcmp(name, "vkGetDeviceProcAddr"))
      return (PFN_vkVoidFunction)xmx_GetDeviceProcAddr;
   if (!instance)
      return NULL;
   struct xinstance *in = xmap_get(&instances, DKEY(instance));
   return in ? in->gipa(instance, name) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL
xmx_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
   if (v->loaderLayerInterfaceVersion > 2)
      v->loaderLayerInterfaceVersion = 2;
   v->pfnGetInstanceProcAddr = xmx_GetInstanceProcAddr;
   v->pfnGetDeviceProcAddr = xmx_GetDeviceProcAddr;
   v->pfnGetPhysicalDeviceProcAddr = NULL;
   return VK_SUCCESS;
}
