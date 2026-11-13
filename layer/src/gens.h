/* gens.h - the GPU generations the layer serves on an unpatched ANV, by PCI device id.
 *
 * Xe3 (Panther Lake, Wildcat Lake): the hardware takes the register count per kernel (INTERFACE_DESCRIPTOR_DATA
 * RegistersPerThread, which ANV derives from prog_data grf_used), so a kernel runs with the 128 or 256 GRF it was
 * compiled for.
 * Xe2 (Lunar Lake, Battlemage): the register count is a mode of the engine (STATE_COMPUTE_MODE Large GRF Mode), which
 * an unpatched ANV never sets (patches/0003 does in the patched driver): every thread gets 128 GRF. The layer compiles
 * every kernel for 128 GRF there (cm-compile.sh CM_MAX_GRF=128). XeSS' SIMD16 kernels, the set XeSS uses on Xe2 and
 * Xe3, compile at 128 GRF without spilling (offline check of all 337 kernel bodies for bmg and lnl).
 * Xe-HPG (DG2 / Arc A, Meteor Lake and Arrow Lake graphics) is not served: the same mode switch is missing, and its
 * SIMD8 kernel set spills at 128 GRF. It stays with the patched driver.
 * The ids: layer/gpu-ids.txt (Mesa 26.3's Panther Lake, Wildcat Lake, Lunar Lake and Battlemage ids), which
 * build.sh turns into gpu_ids.inc and install.sh reads for its stock-mode choice. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "prologue.h"

struct xmx_gen {
   const char *name;    /* in the log and in the kernel cache's file names */
   enum xmx_isa isa;    /* the prologue's encodings */
   uint32_t max_grf;    /* 0: the kernel's own GRF request (up to 256); else the GRF count every kernel is compiled for */
};

/* the generation of a device id, NULL if the layer does not serve it */
static inline const struct xmx_gen *
xmx_gen_of(uint32_t id)
{
   static const struct xmx_gen xe3 = { "xe3", XMX_ISA_XE3, 0 }, xe2 = { "xe2", XMX_ISA_XE2, 128 };
   static const struct { const char *gen; uint16_t id; } ids[] = {
#include "gpu_ids.inc"   /* { "xe3", 0xb080 }, ... */
   };
   for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
      if (id == ids[i].id)
         return !strcmp(ids[i].gen, "xe2") ? &xe2 : &xe3;
   return NULL;
}
