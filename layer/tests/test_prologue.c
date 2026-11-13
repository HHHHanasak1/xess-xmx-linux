/* test_prologue.c - helper of tests/build.sh for the prologue check:
 *   test_prologue cml <zebin> <options> <out.cml>   pack a zebin with the work-group size of its options into CML1
 *   test_prologue pro <in.cml> <out.bin> [xe2|xe3]  write the C-built prologue of a CML1 kernel (default xe3)
 * build.sh compares the latter with tools/prologue_asm.py's assembled prologue of the same file and platform. */
#include "../src/cmk.h"
#include "../src/prologue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int
wr(const char *p, const void *d, size_t n)
{
   FILE *f = fopen(p, "wb");
   if (!f || fwrite(d, 1, n, f) != n)
      return 1;
   return fclose(f) != 0;
}

int
main(int argc, char **argv)
{
   if (argc == 5 && !strcmp(argv[1], "cml")) {
      size_t zn, on, cn, mn;
      uint8_t *z = rd(argv[2], &zn), *o = rd(argv[3], &on);
      if (!z || !o)
         return 2;
      uint64_t lws[3];
      xmx_opt_lws(o, on, lws);
      uint32_t gc;
      char err[256];
      uint8_t *cmk = xmx_pack(z, zn, lws, 64, &cn, &gc, err, sizeof(err));
      uint8_t *cml = cmk ? xmx_cml_from_cmk(cmk, cn, gc, XMX_CML_NO_LOAD_PAYLOAD, &mn) : NULL;
      if (!cml) {
         fprintf(stderr, "%s: %s\n", argv[2], err);
         return 1;
      }
      return wr(argv[4], cml, mn);
   }
   if ((argc == 4 || argc == 5) && !strcmp(argv[1], "pro")) {
      enum xmx_isa isa = XMX_ISA_XE3;
      if (argc == 5 && !strcmp(argv[4], "xe2"))
         isa = XMX_ISA_XE2;
      else if (argc == 5 && strcmp(argv[4], "xe3"))
         return 2;
      size_t n, pn;
      uint8_t *d = rd(argv[2], &n);
      struct xmx_cml k;
      if (!d || !xmx_cml_parse(d, n, &k))
         return 2;
      uint8_t *p = xmx_prologue(&k, isa, &pn);
      if (!p)
         return 3;
      size_t fn;
      uint8_t *full = xmx_full_code(&k, isa, &fn);
      if (!full || fn != pn + k.code_size || memcmp(full, p, pn) || memcmp(full + pn, k.code, k.code_size))
         return 4;
      return wr(argv[3], p, pn);
   }
   fprintf(stderr, "usage: test_prologue cml <zebin> <options> <out.cml> | pro <in.cml> <out.bin> [xe2|xe3]\n");
   return 2;
}
