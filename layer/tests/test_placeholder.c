/* test_placeholder.c - placeholder recognition on SPIR-V built by tests/build.sh from ph.comp.
 * test_placeholder <dir with the .spv files> */
#include "../src/placeholder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t *
load(const char *dir, const char *name, size_t *n)
{
   char p[600];
   snprintf(p, sizeof(p), "%s/%s.spv", dir, name);
   FILE *f = fopen(p, "rb");
   if (!f) { fprintf(stderr, "missing %s\n", p); exit(2); }
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *w = malloc(sz);
   if (fread(w, 1, sz, f) != (size_t)sz) exit(2);
   fclose(f);
   *n = sz / 4;
   return w;
}

static const char *known_hash;
static bool known(const char *h, void *u) { (void)u; return known_hash && strcmp(h, known_hash) == 0; }

static void
classify(const char *dir, const char *name, struct xmx_spv_info *s, struct xmx_placeholder *p)
{
   size_t n;
   uint32_t *w = load(dir, name, &n);
   CHECK(xmx_spv_scan(w, n, s));
   xmx_classify(s, dir, known, NULL, p);
   free(w);
}

int
main(int argc, char **argv)
{
   const char *dir = argc > 1 ? argv[1] : ".";
   struct xmx_spv_info s;
   struct xmx_placeholder p;

   /* old id table: plane z=7 has 1024/7 = 146 entries in row y=1 */
   CHECK(xmx_dyn_id(1, 1, 7) == 0);
   CHECK(xmx_dyn_id(146, 1, 7) == 145);
   CHECK(xmx_dyn_id(1, 2, 7) == 146);
   CHECK(xmx_dyn_id(147, 1, 7) == -1);
   CHECK(xmx_dyn_id(1, 1, 23) == -1);
   CHECK(xmx_dyn_id(1, 1, 11) > 0 && xmx_dyn_id(1, 1, 13) > xmx_dyn_id(1, 1, 11));

   /* kernel placeholder, vector store; hash 1122334455667788 = (lo 0x55667788, hi 0x11223344) */
   known_hash = "1122334455667788";
   classify(dir, "kernel_vec", &s, &p);
   CHECK(s.wg[0] == 1 && s.wg[1] == 1 && s.wg[2] == 23);
   CHECK(s.has_magic);
   CHECK(p.kind == XMX_PH_KERNEL && strcmp(p.hash, known_hash) == 0);

   /* the same, scalar stores (offsets 1 and 2 are constants between the halves) */
   classify(dir, "kernel_scalar", &s, &p);
   CHECK(p.kind == XMX_PH_KERNEL && strcmp(p.hash, known_hash) == 0);

   /* no kernel for the hash: not ours to replace */
   known_hash = "ffffffffffffffff";
   classify(dir, "kernel_vec", &s, &p);
   CHECK(p.kind == XMX_PH_NONE);

   /* self-tests */
   classify(dir, "canary", &s, &p);
   CHECK(p.kind == XMX_PH_CANARY && p.canary_ok);
   classify(dir, "canary_bad", &s, &p);
   CHECK(p.kind == XMX_PH_CANARY && !p.canary_ok);
   classify(dir, "canary_legacy", &s, &p);
   CHECK(p.kind == XMX_PH_LEGACY_CANARY);

   /* old id format with map.txt: (3,2,11); two lines for the id, the last wins */
   const int id = xmx_dyn_id(3, 2, 11);
   char mp[600];
   snprintf(mp, sizeof(mp), "%s/map.txt", dir);
   FILE *m = fopen(mp, "w");
   fprintf(m, "%d aaaaaaaaaaaaaaaa\n%d 0123456789abcdef\n%d bbbbbbbbbbbbbbbb\n", id, id, id + 1);
   fclose(m);
   classify(dir, "legacy", &s, &p);
   CHECK(p.kind == XMX_PH_LEGACY_KERNEL && p.dyn_id == id && strcmp(p.hash, "0123456789abcdef") == 0);
   unlink(mp);
   classify(dir, "legacy", &s, &p);
   CHECK(p.kind == XMX_PH_NONE);

   /* ordinary shaders: no magic, or the magic on a workgroup size of neither format */
   classify(dir, "plain", &s, &p);
   CHECK(!s.has_magic && p.kind == XMX_PH_NONE);
   classify(dir, "other", &s, &p);
   CHECK(s.has_magic && p.kind == XMX_PH_NONE);

   /* not SPIR-V */
   uint32_t junk[8] = { 1, 2, 3 };
   CHECK(!xmx_spv_scan(junk, 8, &s));

   printf(fails ? "test_placeholder: %d failure(s)\n" : "test_placeholder: all passed\n", fails);
   return fails != 0;
}
