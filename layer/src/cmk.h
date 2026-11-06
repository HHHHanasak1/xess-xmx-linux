/* cmk.h - kernel images for the layer.
 *
 * CMK2 (the patched driver's .cmk, 472-byte header + code): written by xmx_pack exactly as tools/cmk_pack.py does.
 * CML1 (the layer's .cml): the same 472-byte header with the magic "CML1", then
 *    472  uint32 gc_off   byte offset of group_count in the cross-thread data, XMX_NO_GC if the kernel has none
 *    476  uint32 flags    XMX_CML_NO_LOAD_PAYLOAD: compiled with -noLoadPayload (needs the layer's prologue)
 *    480  code
 * A separate format and file extension, so the driver's cache and the layer's never mix up kernels compiled with and
 * without the payload load. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define XMX_CMK_HEADER 472
#define XMX_CML_HEADER 480
#define XMX_NO_GC 0xffffffffu
#define XMX_CML_NO_LOAD_PAYLOAD 1u

/* header fields (byte offsets, both formats) */
enum {
   XMX_H_CODE_SIZE = 4, XMX_H_GRF = 8, XMX_H_SLM = 12, XMX_H_BARRIERS = 16, XMX_H_LWS = 20, XMX_H_CROSS_SIZE = 32,
   XMX_H_PER_THREAD = 36, XMX_H_CROSS = 40, XMX_H_HEAP_SET = 168, XMX_H_HEAP_BINDING = 172, XMX_H_TABLE_PC_BASE = 176,
   XMX_H_NSURF = 180, XMX_H_BTI = 184, XMX_H_TABLE_IDX = 280, XMX_H_ELEM = 376,
};

/* zebin d[n] -> malloc'd CMK2 image of *out_size bytes; *gc_off as above. NULL and err on failure. */
uint8_t *xmx_pack(const uint8_t *d, size_t n, const uint64_t lws[3], uint32_t grf_bytes, size_t *out_size,
                  uint32_t *gc_off, char *err, size_t errlen);

/* the work-group size in a compile options file, read as cm-compile.sh does; 1,1,1 if absent */
void xmx_opt_lws(const uint8_t *opt, size_t n, uint64_t lws[3]);

/* CMK2 image -> malloc'd CML1 image; NULL if cmk is not a CMK2 image */
uint8_t *xmx_cml_from_cmk(const uint8_t *cmk, size_t n, uint32_t gc_off, uint32_t flags, size_t *out_size);

/* checks a CML1 image; on success fills the pointers (into img) and returns 1 */
struct xmx_cml {
   const uint8_t *hdr;   /* the 472-byte header (read fields with xmx_rd32(hdr + XMX_H_...)) */
   uint32_t gc_off, flags;
   const uint8_t *code;
   uint32_t code_size;
};
int xmx_cml_parse(const uint8_t *img, size_t n, struct xmx_cml *out);

static inline uint32_t
xmx_rd32(const uint8_t *p)
{
   return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
