/* test_anvbin.c - parse and rewrite of the driver's serialized shaders.
 * test_anvbin <blob>... : each blob (a shader object binary exported by ANV 26.1.2) must parse completely and be
 * written back byte-identical, BLAKE3 included. A one-byte change of the payload must change the recomputed hash.
 * Prints counts and sizes only. */
#include "../src/anvbin.h"

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
   int ok = 0, bad = 0;
   for (int i = 1; i < argc; i++) {
      size_t n, wn;
      uint8_t *d = rd(argv[i], &n);
      struct anvbin b;
      char err[200];
      const char *name = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
      if (!d || !anvbin_parse(&anv_layouts[0], d, n, 80, 0, 0, &b, err, sizeof(err))) {   /* 26.1.2 blobs */
         printf("%s: parse failed: %s\n", name, d ? err : "unreadable");
         bad++;
         continue;
      }
      uint8_t *w = anvbin_write(&b, &wn);
      const bool same = w && wn == n && !memcmp(w, d, n);
      /* a changed payload byte must give another hash */
      bool hash_moves = false;
      if (same && b.code_len) {
         uint8_t *c = malloc(b.code_len);
         memcpy(c, b.code, b.code_len);
         c[0] ^= 1;
         struct anvbin b2 = b;
         b2.code = c;
         size_t n2;
         uint8_t *w2 = anvbin_write(&b2, &n2);
         hash_moves = w2 && n2 == n && memcmp(w2 + 48, d + 48, 32) != 0;
         free(w2);
         free(c);
      }
      printf("%s: %zu bytes, code %u, surfaces %u, samplers %u, layout %u, round trip %s, hash %s\n", name, n,
             b.code_len, b.surface_count, b.sampler_count, b.layout_type, same ? "identical" : "DIFFERS",
             hash_moves ? "follows the payload" : "DOES NOT CHANGE");
      if (same && hash_moves)
         ok++;
      else
         bad++;
      free(w);
      free(d);
   }
   printf("test_anvbin: %d ok, %d failed\n", ok, bad);
   return bad != 0 || ok == 0;
}
