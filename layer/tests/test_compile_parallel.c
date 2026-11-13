/* test_compile_parallel.c - the layer's compile jobs (XMX_LAYER_COMPILE_JOBS, compile.c).
 * test_compile_parallel run <folder of <hash>.spv/.opt>
 *    asks for every kernel of the folder one after another, as XeSS does from its pipeline thread, and prints the
 *    wall time; with XMX_LAYER_COMPILE_JOBS > 1 the first miss queues the others for the workers. Every kernel must
 *    come out, and no compile_failed.txt. Environment as for test_compile (ANV_CM_HELPER, XMX_IGC_DIR, XDG_CACHE_HOME).
 * test_compile_parallel fail <scratch folder>
 *    a helper that fails after 0.3 s, six kernels, XMX_LAYER_COMPILE_JOBS=3: the needed kernel is marked in
 *    compile_failed.txt and not compiled again in this process; the background failures are not marked, run at the
 *    same time, and are compiled again (and then marked) when a kernel is needed. */
#define _GNU_SOURCE
#include "../src/compile.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const struct xmx_dev_info dev = { 0xb080, 64, 0, "xe3" };

static double
now(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec + t.tv_nsec / 1e9;
}

static int
cmp(const void *a, const void *b)
{
   return strcmp((const char *)a, (const char *)b);
}

static unsigned
lines(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return 0;
   unsigned n = 0;
   int c;
   while ((c = fgetc(f)) != EOF)
      n += c == '\n';
   fclose(f);
   return n;
}

static void
put(const char *path, const char *s)
{
   FILE *f = fopen(path, "w");
   if (f) {
      fputs(s, f);
      fclose(f);
   }
}

static int
run(const char *sdir)
{
   static char hashes[1024][17];
   unsigned n = 0;
   DIR *d = opendir(sdir);
   if (!d)
      return 2;
   struct dirent *e;
   while ((e = readdir(d)) && n < 1024)
      if (strlen(e->d_name) == 20 && !strcmp(e->d_name + 16, ".spv")) {
         memcpy(hashes[n], e->d_name, 16);
         hashes[n++][16] = 0;
      }
   closedir(d);
   qsort(hashes, n, sizeof(hashes[0]), cmp);
   char fp[600];
   snprintf(fp, sizeof(fp), "%s/compile_failed.txt", sdir);
   unlink(fp);
   const double t0 = now();
   unsigned ok = 0;
   for (unsigned i = 0; i < n; i++) {
      uint8_t *img = NULL;
      size_t sz = 0;
      ok += xmx_kernel_get(&dev, sdir, -1, hashes[i], hashes[i], &img, &sz);
      free(img);
   }
   const char *jobs = getenv("XMX_LAYER_COMPILE_JOBS");
   printf("test_compile_parallel: %u of %u kernels in %.1f s (XMX_LAYER_COMPILE_JOBS=%s)\n", ok, n, now() - t0,
          jobs ? jobs : "default");
   CHECK(n > 0 && ok == n);
   CHECK(access(fp, F_OK) != 0);
   return fails != 0;
}

static int
fail_test(const char *scratch)
{
   char sdir[512], p[700], helper[700], hlog[700], fp[700];
   snprintf(sdir, sizeof(sdir), "%s/parallel-fail", scratch);
   mkdir(sdir, 0755);
   static const char *hashes[] = { "0000000000000001", "0000000000000002", "0000000000000003",
                                   "0000000000000004", "0000000000000005", "0000000000000006" };
   for (unsigned i = 0; i < 6; i++) {
      snprintf(p, sizeof(p), "%s/%s.spv", sdir, hashes[i]);
      put(p, "x");
      snprintf(p, sizeof(p), "%s/%s.opt", sdir, hashes[i]);
      put(p, "x");
   }
   snprintf(hlog, sizeof(hlog), "%s/helper.log", sdir);
   snprintf(helper, sizeof(helper), "%s/fail.sh", sdir);
   char script[900];
   /* one line per run: its start and end (wall clock, s) */
   snprintf(script, sizeof(script), "s=$(date +%%s.%%N)\nsleep 0.3\necho \"$s $(date +%%s.%%N)\" >> '%s'\nexit 1\n", hlog);
   put(helper, script);
   snprintf(p, sizeof(p), "%s/cache", sdir);
   setenv("XDG_CACHE_HOME", p, 1);
   snprintf(p, sizeof(p), "%s/compile.log", sdir);
   setenv("CM_LOG", p, 1);
   setenv("XMX_LAYER_HELPER", helper, 1);
   setenv("XMX_LAYER_COMPILE_JOBS", "3", 1);
   snprintf(fp, sizeof(fp), "%s/compile_failed.txt", sdir);
   unlink(fp);
   unlink(hlog);

   uint8_t *img = NULL;
   size_t n = 0;
   const double t0 = now();
   CHECK(!xmx_kernel_get(&dev, sdir, -1, hashes[0], hashes[0], &img, &n));
   /* the five others went to two workers: about 3 x 0.3 s more */
   for (int i = 0; i < 100 && lines(hlog) < 6; i++)
      usleep(50000);
   const double t1 = now();
   CHECK(lines(hlog) == 6);
   CHECK(t1 - t0 < 2.0);
   CHECK(lines(fp) == 1);   /* only the needed kernel */
   /* at most three at a time, and more than one */
   double s[16], e[16];
   unsigned k = 0;
   FILE *f = fopen(hlog, "r");
   while (f && k < 16 && fscanf(f, "%lf %lf", &s[k], &e[k]) == 2)
      k++;
   if (f)
      fclose(f);
   unsigned maxc = 0;
   for (unsigned i = 0; i < k; i++) {
      unsigned c = 0;
      for (unsigned j = 0; j < k; j++)
         c += s[j] <= s[i] && e[j] > s[i];
      maxc = c > maxc ? c : maxc;
   }
   printf("test_compile_parallel: 6 failing compiles, at most %u at a time, %.1f s\n", maxc, t1 - t0);
   CHECK(maxc >= 2 && maxc <= 3);
   /* the failed needed kernel: not compiled again, not marked again */
   CHECK(!xmx_kernel_get(&dev, sdir, -1, hashes[0], hashes[0], &img, &n));
   CHECK(lines(hlog) == 6 && lines(fp) == 1);
   /* a kernel that failed in the background: compiled again when needed, then marked */
   CHECK(!xmx_kernel_get(&dev, sdir, -1, hashes[3], hashes[3], &img, &n));
   CHECK(lines(hlog) == 7 && lines(fp) == 2);
   return fails != 0;
}

int
main(int argc, char **argv)
{
   if (argc < 3)
      return 2;
   const int r = !strcmp(argv[1], "run") ? run(argv[2]) : !strcmp(argv[1], "fail") ? fail_test(argv[2]) : 2;
   printf(r ? "test_compile_parallel: FAILED\n" : "test_compile_parallel: ok\n");
   return r;
}
