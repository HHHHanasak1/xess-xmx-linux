/* compile.h - the layer's kernels: cache, run-time compile, packing (the patched driver's anv_cm_kernel_path and
 * anv_cm_compile, for kernels compiled with -noLoadPayload into the layer's CML1 format). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct xmx_dev_info {
   uint32_t pci_id;      /* VkPhysicalDeviceProperties::deviceID */
   uint32_t grf_bytes;   /* 64 on Xe2 and later, 32 on Xe-HPG / Xe-LPG */
   uint32_t max_grf;     /* 0: as the kernel's options ask (up to 256 GRF); 128: every kernel compiled for 128 GRF
                          * (Xe2, see gens.h; XMX_LAYER_FORCE_GRF=128 on Xe3) */
   const char *gen;      /* generation name (gens.h), part of the cache file name; NULL = "any" */
};

/* Cache file of a kernel: <XDG_CACHE_HOME or ~/.cache>/xess-xmx/kernels/<hash>_<pci>_<gen>[-g128]_<toolchain>.cml;
 * the toolchain value (xmx_toolchain_gen) is mixed with max_grf as well, so that 128- and 256-GRF images never share
 * a file. */
void xmx_cache_path(const struct xmx_dev_info *dev, const char *hash, char *dir, size_t dirlen, char *path,
                    size_t pathlen);

/* The CML1 image of a kernel, compiled from <sdir>/<name>.spv + .opt on a cache miss (name: the hash, or dyn_<id> of
 * an older shim with id >= 0). malloc'd image in *img; false if it cannot be had - then <sdir>/compile_failed.txt
 * gets the line "<id> <hash>" as with the driver, and the kernel is not tried again in this process. A miss of a kernel
 * named by its hash (id < 0) also queues every other kernel of sdir without a cache file for background compiles
 * (XMX_LAYER_COMPILE_JOBS at a time, the calling thread included; see compile.c). Thread-safe. */
bool xmx_kernel_get(const struct xmx_dev_info *dev, const char *sdir, int id, const char *name, const char *hash,
                    uint8_t **img, size_t *n);

/* Generation of the toolchain (IGC/ocloc file names, the helper scripts, the layer's packer version, CM_DEVICE). */
uint32_t xmx_toolchain_gen(void);

/* Appends a line to the compile log (CM_LOG, else ~/.cache/xess-xmx/compile.log), as the driver does. */
void xmx_compile_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* mkdir -p */
void xmx_mkdirs(const char *path);
