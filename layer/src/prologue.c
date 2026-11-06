/* prologue.c - see prologue.h. The instruction sequence is the one of tools/prologue_asm.py (prologue_lines); the
 * encodings come from prologue_table.inc. */
#include "prologue.h"

#include <stdlib.h>
#include <string.h>

#include "prologue_table.inc"

static int
log2_exact(uint32_t v)
{
   if (!v || (v & (v - 1)))
      return -1;
   int s = 0;
   while (v >>= 1)
      s++;
   return s;
}

static void
put(uint8_t **p, const uint8_t enc[16])
{
   memcpy(*p, enc, 16);
   *p += 16;
}

static void
put_imm(uint8_t **p, const uint8_t enc[16], unsigned off, uint32_t v)
{
   memcpy(*p, enc, 16);
   (*p)[off] = v;
   (*p)[off + 1] = v >> 8;
   (*p)[off + 2] = v >> 16;
   (*p)[off + 3] = v >> 24;
   *p += 16;
}

uint8_t *
xmx_prologue(const struct xmx_cml *k, size_t *n)
{
   const uint8_t *h = k->hdr;
   const uint32_t lx = xmx_rd32(h + XMX_H_LWS), ly = xmx_rd32(h + XMX_H_LWS + 4);
   const uint32_t cross = xmx_rd32(h + XMX_H_CROSS_SIZE);
   const int sx = log2_exact(lx), sxy = log2_exact(lx * ly);
   if (sx < 0 || sxy < 0 || log2_exact(ly) < 0 || cross > 128)
      return NULL;
   unsigned nreg = (cross + 63) / 64;
   if (nreg < 1)
      nreg = 1;
   if (k->gc_off != XMX_NO_GC && (k->gc_off % 4 || k->gc_off / 4 + 2 >= nreg * 16))
      return NULL;
   uint8_t *buf = malloc(16 * (2 + 32 + 3 + 9)), *p = buf;
   if (!buf)
      return NULL;
   for (unsigned r = 0; r < nreg; r++)
      put(&p, PRO_CLR[r]);
   for (unsigned i = 0; i < nreg * 16; i++) {
      const uint32_t v = 4 * i < 128 ? xmx_rd32(h + XMX_H_CROSS + 4 * i) : 0;
      if (v && 4 * i < cross)
         put_imm(&p, PRO_IMM[i], PRO_IMM_OFF, v);
   }
   if (k->gc_off != XMX_NO_GC)
      for (unsigned i = 0; i < 3; i++)
         put(&p, PRO_GC[(k->gc_off / 4 + i) * 3 + i]);
   put(&p, PRO_TID);
   put(&p, PRO_R1CLR);
   put_imm(&p, PRO_AND41, PRO_AND41_OFF, lx - 1);
   put_imm(&p, PRO_SHR42, PRO_SHR42_OFF, (uint32_t)sx);
   put_imm(&p, PRO_AND42, PRO_AND42_OFF, ly - 1);
   put_imm(&p, PRO_SHR43, PRO_SHR43_OFF, (uint32_t)sxy);
   for (unsigned i = 0; i < 3; i++)
      put(&p, PRO_LID[i]);
   *n = p - buf;
   return buf;
}

uint8_t *
xmx_full_code(const struct xmx_cml *k, size_t *n)
{
   size_t pn;
   uint8_t *pro = xmx_prologue(k, &pn);
   if (!pro)
      return NULL;
   uint8_t *c = malloc(pn + k->code_size);
   if (c) {
      memcpy(c, pro, pn);
      memcpy(c + pn, k->code, k->code_size);
      *n = pn + k->code_size;
   }
   free(pro);
   return c;
}
