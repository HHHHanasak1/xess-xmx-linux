/* test_compile.c - the layer's run-time compile end to end, with the real compile script and ocloc.
 * test_compile <kernel folder with dyn_N.spv/.opt and map.txt> <reference root with N/k.cmk compiled -noLoadPayload>
 *              <old compile script without CM_NO_LOAD_PAYLOAD>
 * Environment: ANV_CM_HELPER (the script under test), XMX_IGC_DIR, CM_DEVICE, XDG_CACHE_HOME (a scratch folder).
 * TEST_GEN=xe2: as the layer compiles for a Battlemage B580 (0xe20b, every kernel at 128 GRF): the code is not
 * compared with the reference root (an Xe3 set), but every kernel must come out at 128 GRF, in an "xe2-g128" cache
 * file; and an Xe3 device compiling the same kernel must get its own cache file at its 256 GRF. */
#define _GNU_SOURCE
#include "../src/cmk.h"
#include "../src/compile.h"
#include "../src/placeholder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

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
   if (argc < 4)
      return 2;
   const char *sdir = argv[1], *refroot = argv[2], *oldhelper = argv[3];
   const bool xe2 = getenv("TEST_GEN") && !strcmp(getenv("TEST_GEN"), "xe2");
   const struct xmx_dev_info dev = xe2 ? (struct xmx_dev_info){ 0xe20b, 64, 128, "xe2" }
                                       : (struct xmx_dev_info){ 0xb080, 64, 0, "xe3" };
   const int ids[] = { 3, 1, 0 };   /* dyn_1 is the kernel with group_count */
   for (unsigned t = 0; t < 3; t++) {
      const int id = ids[t];
      char hash[17], name[32], ref[700], cdir[512], path[700];
      CHECK(xmx_map_lookup(sdir, id, hash));
      snprintf(name, sizeof(name), "dyn_%d", id);
      uint8_t *img;
      size_t n;
      CHECK(xmx_kernel_get(&dev, sdir, id, name, hash, &img, &n));
      struct xmx_cml m;
      CHECK(img && xmx_cml_parse(img, n, &m));
      CHECK(m.flags == XMX_CML_NO_LOAD_PAYLOAD);
      CHECK((m.gc_off != XMX_NO_GC) == (id == 1));
      uint8_t *r = NULL;
      if (xe2) {
         /* every kernel of this set asks for 256 GRF (-doubleGRF): compiled at 128 here */
         CHECK(xmx_rd32(m.hdr + XMX_H_GRF) == 128);
      } else {
         /* the code must be what the reference compile with -noLoadPayload produced */
         snprintf(ref, sizeof(ref), "%s/%d/k.cmk", refroot, id);
         size_t rn;
         r = rd(ref, &rn);
         CHECK(r && rn - XMX_CMK_HEADER == m.code_size && !memcmp(r + XMX_CMK_HEADER, m.code, m.code_size));
      }
      /* cached now: the second call reads the file */
      xmx_cache_path(&dev, hash, cdir, sizeof(cdir), path, sizeof(path));
      CHECK(access(path, R_OK) == 0 && strstr(path, ".cml"));
      CHECK(strstr(path, xe2 ? "_xe2-g128_" : "_xe3_"));
      if (xe2 && t == 0) {
         /* the same kernel for an Xe3 device on the same toolchain: its own file, at the kernel's 256 GRF */
         const struct xmx_dev_info x3 = { 0xe20b, 64, 0, "xe3" };   /* same id on purpose: only the cap differs */
         char p3[700], d3[512];
         xmx_cache_path(&x3, hash, d3, sizeof(d3), p3, sizeof(p3));
         CHECK(strcmp(p3, path) != 0);
         uint8_t *i3;
         size_t n3;
         struct xmx_cml m3;
         CHECK(xmx_kernel_get(&x3, sdir, id, name, hash, &i3, &n3) && xmx_cml_parse(i3, n3, &m3) &&
               xmx_rd32(m3.hdr + XMX_H_GRF) == 256);
         free(i3);
      }
      free(img);
      CHECK(xmx_kernel_get(&dev, sdir, id, name, hash, &img, &n));
      free(img);
      free(r);
   }
   /* a compile script without CM_NO_LOAD_PAYLOAD (older package): refused, marked in compile_failed.txt */
   setenv("XMX_LAYER_HELPER", oldhelper, 1);
   char hash[17], fp[600];
   CHECK(xmx_map_lookup(sdir, 5, hash));
   snprintf(fp, sizeof(fp), "%s/compile_failed.txt", sdir);
   unlink(fp);
   uint8_t *img = NULL;
   size_t n;
   CHECK(!xmx_kernel_get(&dev, sdir, 5, "dyn_5", hash, &img, &n));
   size_t fn;
   uint8_t *f = rd(fp, &fn);
   char want[40];
   snprintf(want, sizeof(want), "5 %s\n", hash);
   CHECK(f && fn == strlen(want) && !memcmp(f, want, fn));
   free(f);
   unlink(fp);
   printf(fails ? "test_compile: %d failure(s)\n" : "test_compile: all passed\n", fails);
   return fails != 0;
}
