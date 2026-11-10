/* anvbin.h - ANV's serialized compute shader, as the driver exports it for a template shader the layer creates
 * itself: parse it, write it back, and build a kernel's binary from it. The sizes and offsets come from the table of
 * the running Mesa version (layouts.h).
 *
 * Two wrappings of the same payload:
 *   shader object (vkGetShaderBinaryDataEXT): an 80-byte header (size at 40, BLAKE3 of the whole at 48) + payload
 *   pipeline binary (vkGetPipelineBinaryDataKHR): the payload alone (26.1.2 and 26.3.0-rc3).
 * Alignment inside the payload is relative to `align_base` (offset of the payload's alignment origin in the data). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cmk.h"
#include "layouts.h"

struct anvbin {
   const struct anv_layout *L;
   size_t hdr_len;            /* 80 (shader object) or 0 */
   size_t pre_len;            /* bytes before the payload that are not a header */
   size_t align_base;
   uint8_t pre[16];
   uint8_t header[80];
   uint32_t stage, code_len;
   const uint8_t *code;
   uint8_t prog_data[768];   /* 664 on the patched 26.3 driver (layouts.h) */
   uint32_t num_relocs;
   const uint8_t *relocs;
   uint32_t num_stats;
   const uint8_t *stats;
   uint32_t xfb_size;
   const uint8_t *xfb;
   uint32_t instance_multiplier, used_descriptors, fully_promoted;
   uint8_t push_set_buffer;
   uint8_t blake3s[96];
   uint16_t layout_type, binding_mask;
   uint8_t surface_count, sampler_count;
   uint16_t embedded_count;
   uint8_t surf[256 * SIZEOF_BINDING], samp[256 * SIZEOF_BINDING];
   const uint8_t *emb;
   uint8_t input_att[64], push_ranges[64];
   uint8_t inline_count, inline_dwords[16];
   uint8_t used_surface_sets, used_sampler_sets, pushed_sets;
   uint8_t dyn_desc[64];
   uint8_t inferred_behavior;
};

/* false (and err) if d[n] is not a complete payload of layout L */
bool anvbin_parse(const struct anv_layout *L, const uint8_t *d, size_t n, size_t hdr_len, size_t pre_len,
                  size_t align_base, struct anvbin *b, char *err, size_t errlen);

/* malloc'd serialization; with hdr_len 80 the header's size and BLAKE3 are recomputed */
uint8_t *anvbin_write(const struct anvbin *b, size_t *n);

/* What the kernel's binary needs from the layer. */
struct anvbin_kernel {
   const struct xmx_cml *k;   /* the kernel (header, group_count offset) */
   const uint8_t *code;       /* prologue + code (prologue.h) */
   uint32_t code_len;
   uint32_t set;              /* the layer's descriptor set: surface k is its binding k, the sampler binding `sampler` */
   uint32_t sampler_binding;
   bool uses_sampler;
   uint32_t surface_stride;   /* !0: write the entries the template lacks (descriptor k at k * stride), see .c */
};

/* Template t (a shader that uses the layer's set, and the sampler binding) -> the kernel's binary in *out (pointing
 * into t and kc). false and err if the template lacks an entry the kernel needs. */
bool anvbin_kernel(const struct anvbin *t, const struct anvbin_kernel *kc, struct anvbin *out, char *err,
                   size_t errlen);
