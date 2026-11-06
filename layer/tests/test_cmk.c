/* test_cmk.c - the layer's packer against tools/cmk_pack.py.
 * test_cmk <root> : every <root>/<N>/ holding k_*.bin (ocloc's zebin) and k.cmk (packed by cmk_pack.py from it) is
 * packed again with the work-group size of that k.cmk and a 64-byte GRF; the images must be byte-identical, and the
 * CML1 image must parse back to the same header, group_count offset and code. Prints counts only. */
#include "../src/cmk.h"

#include <dirent.h>
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

int
main(int argc, char **argv)
{
   if (argc < 2) {
      fprintf(stderr, "usage: test_cmk <root>\n");
      return 2;
   }
   DIR *root = opendir(argv[1]);
   if (!root)
      return 2;
   unsigned tested = 0, same = 0, differ = 0, failed = 0, with_gc = 0, cml_ok = 0;
   struct dirent *e;
   while ((e = readdir(root))) {
      if (e->d_name[0] == '.')
         continue;
      char d[600], p[700], bin[1000] = "";
      snprintf(d, sizeof(d), "%s/%s", argv[1], e->d_name);
      DIR *k = opendir(d);
      if (!k)
         continue;
      struct dirent *f;
      while ((f = readdir(k)))
         if (!strncmp(f->d_name, "k_", 2) && strstr(f->d_name, ".bin"))
            snprintf(bin, sizeof(bin), "%s/%s", d, f->d_name);
      closedir(k);
      snprintf(p, sizeof(p), "%s/k.cmk", d);
      size_t zn, rn;
      uint8_t *z = bin[0] ? rd(bin, &zn) : NULL, *ref = rd(p, &rn);
      if (!z || !ref) {
         free(z);
         free(ref);
         continue;
      }
      tested++;
      const uint64_t lws[3] = { xmx_rd32(ref + XMX_H_LWS), xmx_rd32(ref + XMX_H_LWS + 4), xmx_rd32(ref + XMX_H_LWS + 8) };
      size_t on;
      uint32_t gc;
      char err[256];
      uint8_t *o = xmx_pack(z, zn, lws, 64, &on, &gc, err, sizeof(err));
      if (!o) {
         failed++;
         fprintf(stderr, "%s: pack failed: %s\n", e->d_name, err);
      } else if (on == rn && !memcmp(o, ref, rn)) {
         same++;
      } else {
         differ++;
         size_t i = 0;
         while (i < on && i < rn && o[i] == ref[i])
            i++;
         fprintf(stderr, "%s: differs (sizes %zu/%zu, first difference at byte %zu)\n", e->d_name, on, rn, i);
      }
      if (o) {
         with_gc += gc != XMX_NO_GC;
         size_t cn;
         uint8_t *c = xmx_cml_from_cmk(o, on, gc, XMX_CML_NO_LOAD_PAYLOAD, &cn);
         struct xmx_cml m;
         if (c && xmx_cml_parse(c, cn, &m) && m.gc_off == gc && m.flags == XMX_CML_NO_LOAD_PAYLOAD &&
             m.code_size == on - XMX_CMK_HEADER && !memcmp(m.code, o + XMX_CMK_HEADER, m.code_size) &&
             !memcmp(m.hdr + 4, o + 4, XMX_CMK_HEADER - 4))
            cml_ok++;
         else
            fprintf(stderr, "%s: CML1 round trip failed\n", e->d_name);
         free(c);
      }
      free(o);
      free(z);
      free(ref);
   }
   closedir(root);
   printf("test_cmk: %u kernels, %u identical, %u different, %u pack failures, %u with group_count, %u CML1 ok\n",
          tested, same, differ, failed, with_gc, cml_ok);
   return !(tested > 0 && same == tested && cml_ok == tested);
}
