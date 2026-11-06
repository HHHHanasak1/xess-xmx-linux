/* anvbin.c - see anvbin.h. Field order and the kernel's prog_data / bind_map as E3/E3b ran them on Mesa 26.1.2. */
#include "anvbin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../third_party/blake3/blake3.h"



/* ---- reader ---- */
struct rd {
   const uint8_t *d;
   size_t n, p, base;
   bool bad;
};
static void
ralign(struct rd *r, size_t a)
{
   const size_t rel = r->p - r->base;
   r->p = r->base + ((rel + a - 1) & ~(a - 1));
}
static const uint8_t *
rbytes(struct rd *r, size_t k)
{
   static const uint8_t zero[256 * 16];
   if (r->bad || r->p > r->n || r->n - r->p < k) {
      r->bad = true;
      return zero;
   }
   const uint8_t *x = r->d + r->p;
   r->p += k;
   return x;
}
static uint32_t
r32(struct rd *r)
{
   ralign(r, 4);
   uint32_t v;
   memcpy(&v, rbytes(r, 4), 4);
   return v;
}
static uint16_t
r16(struct rd *r)
{
   ralign(r, 2);
   uint16_t v;
   memcpy(&v, rbytes(r, 2), 2);
   return v;
}
static uint8_t
r8(struct rd *r)
{
   return *rbytes(r, 1);
}

bool
anvbin_parse(const struct anv_layout *L, const uint8_t *d, size_t n, size_t hdr_len, size_t pre_len,
             size_t align_base, struct anvbin *b, char *err, size_t errlen)
{
   memset(b, 0, sizeof(*b));
   b->L = L;
   if (L->pd_size > sizeof(b->prog_data) || L->input_att_size > sizeof(b->input_att) ||
       L->push_ranges_size > sizeof(b->push_ranges) || L->dyn_desc_size > sizeof(b->dyn_desc) ||
       L->max_inline > sizeof(b->inline_dwords)) {
      snprintf(err, errlen, "layout table too large");
      return false;
   }
   if (pre_len > sizeof(b->pre) || (hdr_len != 0 && hdr_len != 80)) {
      snprintf(err, errlen, "bad wrapping");
      return false;
   }
   b->hdr_len = hdr_len;
   b->pre_len = pre_len;
   b->align_base = align_base;
   struct rd r = { d, n, 0, align_base, false };
   memcpy(b->pre, rbytes(&r, pre_len), pre_len);
   memcpy(b->header, rbytes(&r, hdr_len), hdr_len);
   b->stage = r32(&r);
   b->code_len = r32(&r);
   b->code = rbytes(&r, b->code_len);
   memcpy(b->prog_data, rbytes(&r, L->pd_size), L->pd_size);
   memcpy(&b->num_relocs, b->prog_data + L->num_relocs, 4);
   if (b->num_relocs > 1024)
      r.bad = true;
   b->relocs = rbytes(&r, (size_t)b->num_relocs * L->reloc_size);
   b->num_stats = r32(&r);
   if (b->num_stats > 64)
      r.bad = true;
   b->stats = rbytes(&r, (size_t)b->num_stats * L->stats_size);
   b->xfb_size = r32(&r);
   b->xfb = b->xfb_size ? rbytes(&r, b->xfb_size) : NULL;
   b->instance_multiplier = r32(&r);
   b->used_descriptors = r32(&r);
   b->fully_promoted = r32(&r);
   b->push_set_buffer = r8(&r);
   memcpy(b->blake3s, rbytes(&r, 96), 96);
   b->layout_type = r16(&r);
   b->binding_mask = r16(&r);
   b->surface_count = r8(&r);
   b->sampler_count = r8(&r);
   b->embedded_count = r16(&r);
   memcpy(b->surf, rbytes(&r, (size_t)b->surface_count * SIZEOF_BINDING), (size_t)b->surface_count * SIZEOF_BINDING);
   memcpy(b->samp, rbytes(&r, (size_t)b->sampler_count * SIZEOF_BINDING), (size_t)b->sampler_count * SIZEOF_BINDING);
   if (b->embedded_count > 256)
      r.bad = true;
   b->emb = rbytes(&r, (size_t)b->embedded_count * L->emb_size);
   memcpy(b->input_att, rbytes(&r, L->input_att_size), L->input_att_size);
   memcpy(b->push_ranges, rbytes(&r, L->push_ranges_size), L->push_ranges_size);
   b->inline_count = r8(&r);
   if (b->inline_count > L->max_inline)
      r.bad = true;
   memcpy(b->inline_dwords, rbytes(&r, b->inline_count), b->inline_count);
   b->used_surface_sets = r8(&r);
   b->used_sampler_sets = r8(&r);
   b->pushed_sets = r8(&r);
   memcpy(b->dyn_desc, rbytes(&r, L->dyn_desc_size), L->dyn_desc_size);
   b->inferred_behavior = r8(&r);
   if (r.bad || r.p != n) {
      snprintf(err, errlen, r.bad ? "payload overrun" : "%zu of %zu bytes consumed", r.p, n);
      return false;
   }
   if (b->stage != STAGE_COMPUTE) {
      snprintf(err, errlen, "stage %u is not compute", b->stage);
      return false;
   }
   return true;
}

/* ---- writer ---- */
struct wr {
   uint8_t *d;
   size_t n, cap, base;
   bool bad;
};
static void
wbytes(struct wr *w, const void *x, size_t k)
{
   if (w->bad)
      return;
   if (w->n + k > w->cap) {
      size_t c = w->cap ? w->cap * 2 : 4096;
      while (c < w->n + k)
         c *= 2;
      uint8_t *nd = realloc(w->d, c);
      if (!nd) {
         w->bad = true;
         return;
      }
      w->d = nd;
      w->cap = c;
   }
   if (k)
      memcpy(w->d + w->n, x, k);
   w->n += k;
}
static void
walign(struct wr *w, size_t a)
{
   static const uint8_t z[8];
   const size_t rel = w->n - w->base;
   wbytes(w, z, ((rel + a - 1) & ~(a - 1)) - rel);
}
static void w32(struct wr *w, uint32_t v) { walign(w, 4); wbytes(w, &v, 4); }
static void w16(struct wr *w, uint16_t v) { walign(w, 2); wbytes(w, &v, 2); }
static void w8(struct wr *w, uint8_t v) { wbytes(w, &v, 1); }

uint8_t *
anvbin_write(const struct anvbin *b, size_t *n)
{
   const struct anv_layout *L = b->L;
   struct wr w = { NULL, 0, 0, b->align_base, false };
   wbytes(&w, b->pre, b->pre_len);
   wbytes(&w, b->header, b->hdr_len);
   w32(&w, b->stage);
   w32(&w, b->code_len);
   wbytes(&w, b->code, b->code_len);
   wbytes(&w, b->prog_data, L->pd_size);
   wbytes(&w, b->relocs, (size_t)b->num_relocs * L->reloc_size);
   w32(&w, b->num_stats);
   wbytes(&w, b->stats, (size_t)b->num_stats * L->stats_size);
   w32(&w, b->xfb_size);
   if (b->xfb_size)
      wbytes(&w, b->xfb, b->xfb_size);
   w32(&w, b->instance_multiplier);
   w32(&w, b->used_descriptors);
   w32(&w, b->fully_promoted);
   w8(&w, b->push_set_buffer);
   wbytes(&w, b->blake3s, 96);
   w16(&w, b->layout_type);
   w16(&w, b->binding_mask);
   w8(&w, b->surface_count);
   w8(&w, b->sampler_count);
   w16(&w, b->embedded_count);
   wbytes(&w, b->surf, (size_t)b->surface_count * SIZEOF_BINDING);
   wbytes(&w, b->samp, (size_t)b->sampler_count * SIZEOF_BINDING);
   wbytes(&w, b->emb, (size_t)b->embedded_count * L->emb_size);
   wbytes(&w, b->input_att, L->input_att_size);
   wbytes(&w, b->push_ranges, L->push_ranges_size);
   w8(&w, b->inline_count);
   wbytes(&w, b->inline_dwords, b->inline_count);
   w8(&w, b->used_surface_sets);
   w8(&w, b->used_sampler_sets);
   w8(&w, b->pushed_sets);
   wbytes(&w, b->dyn_desc, L->dyn_desc_size);
   w8(&w, b->inferred_behavior);
   if (w.bad) {
      free(w.d);
      return NULL;
   }
   if (b->hdr_len == 80) {
      /* the shader object header: size (u64 at 40) and BLAKE3 (32 bytes at 48) of the whole with a zero hash field;
       * a corruption check of the cache format */
      uint8_t *h = w.d + b->pre_len;
      const uint64_t size = w.n - b->pre_len;
      memcpy(h + 40, &size, 8);
      memset(h + 48, 0, 32);
      blake3_hasher bh;
      blake3_hasher_init(&bh);
      blake3_hasher_update(&bh, h, size);
      uint8_t out[32];
      blake3_hasher_finalize(&bh, out, 32);
      memcpy(h + 48, out, 32);
   }
   *n = w.n;
   return w.d;
}

/* ---- the kernel's binary ---- */
static void
s32(uint8_t *p, unsigned off, uint32_t v)
{
   memcpy(p + off, &v, 4);
}


bool
anvbin_kernel(const struct anvbin *t, const struct anvbin_kernel *kc, struct anvbin *b, char *err, size_t errlen)
{
   const struct anv_layout *L = t->L;
   if (t->layout_type != L->layout_type_direct) {
      snprintf(err, errlen, "template layout_type %u is not the direct one (%u)", t->layout_type, L->layout_type_direct);
      return false;
   }
   const uint8_t *h = kc->k->hdr;
   const uint32_t grf = xmx_rd32(h + XMX_H_GRF), slm = xmx_rd32(h + XMX_H_SLM), bar = xmx_rd32(h + XMX_H_BARRIERS);
   const uint32_t lx = xmx_rd32(h + XMX_H_LWS), ly = xmx_rd32(h + XMX_H_LWS + 4), lz = xmx_rd32(h + XMX_H_LWS + 8);
   const uint32_t nsurf = xmx_rd32(h + XMX_H_NSURF);
   const bool gc = kc->k->gc_off != XMX_NO_GC;
   *b = *t;
   b->code = kc->code;
   b->code_len = kc->code_len;
   uint8_t *p = b->prog_data;
   if (L->zero_prog_data) {
      /* 26.1, as E3/E3b ran it: all zero but for the stage and the source hash */
      uint8_t keep_stage[4], keep_hash[8];
      memcpy(keep_stage, p + L->stage, 4);
      memcpy(keep_hash, p + L->source_hash, L->source_hash_size);
      memset(p, 0, L->pd_size);
      memcpy(p + L->stage, keep_stage, 4);
      memcpy(p + L->source_hash, keep_hash, L->source_hash_size);
   }
   /* otherwise the template's prog_data stays, with the fields below written over it: the ones whose meaning is not
    * measured for that version keep the values of a real shader of this driver */
   s32(p, L->total_scratch, 0);
   s32(p, L->program_size, kc->code_len);
   s32(p, L->grf_used, grf);
   s32(p, L->total_shared, slm);
   /* one SIMD32 hardware thread per CM work item: the driver's "local size" is lx * 32 lanes */
   s32(p, L->local_size, lx * 32);
   s32(p, L->local_size + 4, ly);
   s32(p, L->local_size + 8, lz);
   for (unsigned i = 0; i < 3; i++)
      s32(p, L->prog_offset + 4 * i, 0);
   s32(p, L->prog_mask, 1u << 2);
   s32(p, L->prog_spilled, 0);
   p[L->uses_barrier] = (bar & 0x7fffffffu) != 0;
   p[L->generate_local_id] = 0;   /* the prologue makes the local ids */
   if (L->uses_inline_push_addr >= 0)
      p[L->uses_inline_push_addr] = gc;   /* 26.1: inline data on = num_workgroups in r1 */
   if (L->uses_systolic >= 0)
      p[L->uses_systolic] = 0;   /* no effect on Xe3 + 26.1.2 (E3b) */
   p[L->uses_sampler] = kc->uses_sampler;
   s32(p, L->walk_order, 0);   /* XYZ */
   b->num_relocs = 0;
   b->relocs = NULL;
   s32(p, L->num_relocs, 0);
   b->num_stats = t->num_stats ? 1 : 0;   /* the template's statistics, informational only */
   b->used_descriptors = 0;
   b->fully_promoted = 0;
   b->push_set_buffer = 0;

   /* binding table: BTI bti[k] = the entry of (set, binding k); BTI 0 and the gaps = the null surface, or where
    * its id is not measured, the kernel's first surface (those slots are never accessed) */
   unsigned top = 1;
   for (unsigned k = 0; k < nsurf; k++)
      if (xmx_rd32(h + XMX_H_BTI + 4 * k) + 1 > top)
         top = xmx_rd32(h + XMX_H_BTI + 4 * k) + 1;
   if (nsurf > 24 || top > 240) {
      snprintf(err, errlen, "binding table out of range (%u surfaces, top %u)", nsurf, top);
      return false;
   }
   memset(b->surf, 0, sizeof(b->surf));
   uint8_t first[SIZEOF_BINDING] = { 0 };
   for (unsigned k = 0; k < nsurf; k++) {
      const uint32_t bti = xmx_rd32(h + XMX_H_BTI + 4 * k);
      uint8_t *e = b->surf + bti * SIZEOF_BINDING;
      bool found = false;
      for (unsigned i = 0; i < t->surface_count && !found; i++) {
         const uint8_t *te = t->surf + i * SIZEOF_BINDING;
         uint32_t bnd;
         memcpy(&bnd, te + BINDING_OFF_binding, 4);
         if (te[BINDING_OFF_set] == kc->set && bnd == k) {
            memcpy(e, te, SIZEOF_BINDING);
            found = true;
         }
      }
      if (!found && kc->surface_stride) {
         /* The driver gave the template no binding-table entries: with vkd3d-proton as the engine, Mesa lets
          * shaders reach surfaces bindlessly. The kernel addresses them by binding-table index, so the entry is
          * written as the driver writes it for this layout otherwise (measured with another engine name): binding k
          * of the layer's set = descriptor k, at k * stride in the set's memory. */
         memset(e, 0, SIZEOF_BINDING);
         const uint32_t off = k * kc->surface_stride;
         memcpy(e + BINDING_OFF_index, &k, 4);
         memcpy(e + BINDING_OFF_binding, &k, 4);
         memcpy(e + BINDING_OFF_set_offset, &off, 4);
         e[BINDING_OFF_set] = (uint8_t)kc->set;
         e[BINDING_OFF_plane] = 0;
         found = true;
      }
      if (!found) {
         snprintf(err, errlen, "template has no surface for set %u binding %u", kc->set, k);
         return false;
      }
      if (k == 0)
         memcpy(first, e, SIZEOF_BINDING);
   }
   /* the unused slots */
   for (unsigned s = 0; s < top; s++) {
      bool used = false;
      for (unsigned k = 0; k < nsurf && !used; k++)
         used = xmx_rd32(h + XMX_H_BTI + 4 * k) == s;
      if (used)
         continue;
      uint8_t *e = b->surf + s * SIZEOF_BINDING;
      if (L->null_set >= 0)
         e[BINDING_OFF_set] = (uint8_t)L->null_set;
      else
         memcpy(e, first, SIZEOF_BINDING);
   }
   b->surface_count = top;
   b->embedded_count = 0;
   b->emb = NULL;
   if (kc->uses_sampler) {
      bool found = false;
      for (unsigned i = 0; i < t->sampler_count && !found; i++) {
         const uint8_t *te = t->samp + i * SIZEOF_BINDING;
         uint32_t bnd;
         memcpy(&bnd, te + BINDING_OFF_binding, 4);
         if (te[BINDING_OFF_set] == kc->set && bnd == kc->sampler_binding) {
            memset(b->samp, 0, sizeof(b->samp));
            memcpy(b->samp, te, SIZEOF_BINDING);   /* sampler index 0, copied as is (its set_offset is its own) */
            found = true;
         }
      }
      if (!found) {
         snprintf(err, errlen, "template has no sampler for set %u binding %u", kc->set, kc->sampler_binding);
         return false;
      }
      b->sampler_count = 1;
      b->used_sampler_sets = t->used_sampler_sets;
   } else {
      b->sampler_count = 0;
      b->used_sampler_sets = 0;
   }
   memset(b->push_ranges, 0, L->push_ranges_size);
   b->inline_count = 0;
   if (gc) {
      /* num_workgroups in the inline data (r1.0-2), where the prologue copies group_count from */
      b->inline_count = 3;
      memcpy(b->inline_dwords, L->inl_num_wg, 3);
   }
   /* the template uses exactly the layer's set, as the kernel does */
   b->used_surface_sets = t->used_surface_sets;
   b->pushed_sets = 0;
   b->binding_mask = t->binding_mask;
   b->inferred_behavior = 0;
   return true;
}
