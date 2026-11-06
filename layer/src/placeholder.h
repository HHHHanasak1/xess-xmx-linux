/* placeholder.h - recognise the shim's placeholder compute shaders in the SPIR-V that vkd3d-proton hands to
 * vkCreateShaderModule, and find the kernel each one stands for. Same rules as the patched driver (anv_cm.c), but on
 * SPIR-V instead of NIR. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XMX_MAGIC 0x584d5843u          /* stored by every placeholder shader of the shim */
#define XMX_HASH_WG_Z 23               /* kernel placeholder: (1,1,23), stores (magic, hash lo, hash hi) */
#define XMX_CANARY_WG_Z 19             /* self-test placeholder: (1,1,19), stores (magic, CANARY_LO, CANARY_HI) */
#define XMX_CANARY_LO 0x00414e43u
#define XMX_CANARY_HI 0x7fa15952u
#define XMX_LEGACY_CANARY_WG_Z 17      /* self-test of an older shim: (1,1,17), magic alone */
#define XMX_MAX_CONSTS 64

enum xmx_ph_kind {
   XMX_PH_NONE = 0,       /* an ordinary shader */
   XMX_PH_KERNEL,         /* hash format: kernel <hash>.spv */
   XMX_PH_CANARY,         /* hash-format self-test; canary_ok says whether both canary dwords arrived intact */
   XMX_PH_LEGACY_CANARY,  /* self-test of an older shim */
   XMX_PH_LEGACY_KERNEL,  /* id format: dyn_<id>, hash via map.txt */
};

struct xmx_spv_info {
   uint32_t wg[3];                /* workgroup size, 0 if not found */
   uint32_t consts[XMX_MAX_CONSTS]; /* 32-bit integer/float constants in declaration order, 64-bit ones as lo, hi */
   unsigned nconsts;
   bool has_magic;
};

struct xmx_placeholder {
   enum xmx_ph_kind kind;
   bool canary_ok;
   int dyn_id;                    /* XMX_PH_LEGACY_KERNEL */
   char hash[17];                 /* XMX_PH_KERNEL / XMX_PH_LEGACY_KERNEL: 16 hex digits */
};

/* Parse a SPIR-V module (words, little-endian host). false if it is not a valid SPIR-V header. */
bool xmx_spv_scan(const uint32_t *words, size_t nwords, struct xmx_spv_info *out);

/* Workgroup size -> id of the old format (planes z = 7, 11, 13), -1 if none. */
int xmx_dyn_id(uint32_t wx, uint32_t wy, uint32_t wz);

/* Callback: does a kernel exist for this hash (<dir>/<hash>.spv, or a compiled one in the cache)? */
typedef bool (*xmx_hash_known_fn)(const char *hash, void *user);

/* Classify a scanned shader. dir: the prefix's igdext_kernels folder (for map.txt), may be NULL.
 * known: used to pick the hash among the candidates as the driver does. */
void xmx_classify(const struct xmx_spv_info *info, const char *dir, xmx_hash_known_fn known, void *user,
                  struct xmx_placeholder *out);

/* map.txt lookup: the hash of dyn id `id` (the last line for that id wins). false if absent. */
bool xmx_map_lookup(const char *dir, int id, char hash[17]);
