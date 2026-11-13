/* test_compile_timeout.c - a compile helper that hangs is killed after ANV_CM_COMPILE_TIMEOUT seconds, together with
 * the process it started, and the kernel counts as failed (compile_failed.txt).
 * test_compile_timeout <scratch folder> */
#define _GNU_SOURCE
#include "../src/compile.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void
put(const char *path, const char *s)
{
   FILE *f = fopen(path, "w");
   if (f) {
      fputs(s, f);
      fclose(f);
   }
}

int
main(int argc, char **argv)
{
   if (argc < 2)
      return 2;
   char sdir[512], p[700], helper[700], pidf[700];
   snprintf(sdir, sizeof(sdir), "%s/timeout", argv[1]);
   mkdir(sdir, 0755);
   const char *hash = "0123456789abcdef";
   snprintf(p, sizeof(p), "%s/%s.spv", sdir, hash);
   put(p, "x");
   snprintf(p, sizeof(p), "%s/%s.opt", sdir, hash);
   put(p, "x");
   /* the helper starts a child of its own and waits for it: both must be gone afterwards */
   snprintf(pidf, sizeof(pidf), "%s/child.pid", sdir);
   snprintf(helper, sizeof(helper), "%s/hang.sh", sdir);
   char script[900];
   snprintf(script, sizeof(script), "sleep 60 &\necho $! > '%s'\nwait\n", pidf);
   put(helper, script);
   snprintf(p, sizeof(p), "%s/cache", sdir);
   setenv("XDG_CACHE_HOME", p, 1);
   snprintf(p, sizeof(p), "%s/compile.log", sdir);
   setenv("CM_LOG", p, 1);
   setenv("XMX_LAYER_HELPER", helper, 1);
   setenv("ANV_CM_COMPILE_TIMEOUT", "1", 1);
   char fp[600];
   snprintf(fp, sizeof(fp), "%s/compile_failed.txt", sdir);
   unlink(fp);
   unlink(pidf);

   const struct xmx_dev_info dev = { 0xb080, 64 };
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   uint8_t *img = NULL;
   size_t n = 0;
   CHECK(!xmx_kernel_get(&dev, sdir, -1, hash, hash, &img, &n));
   clock_gettime(CLOCK_MONOTONIC, &t1);
   const double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
   CHECK(s >= 1.0 && s < 10.0);
   /* the helper's own child went with it (killed by process group) */
   FILE *f = fopen(pidf, "r");
   int child = 0;
   CHECK(f && fscanf(f, "%d", &child) == 1 && child > 0);
   if (f)
      fclose(f);
   if (child > 0) {   /* reaped by init a moment later (a zombie still answers kill 0) */
      bool gone = false;
      for (int i = 0; i < 200 && !gone; i++) {
         gone = kill(child, 0) != 0 && errno == ESRCH;
         if (!gone)
            usleep(10000);
      }
      CHECK(gone);
   }
   /* marked failed */
   char want[40], got[64] = "";
   snprintf(want, sizeof(want), "-1 %s\n", hash);
   f = fopen(fp, "r");
   CHECK(f && fgets(got, sizeof(got), f) && !strcmp(got, want));
   if (f)
      fclose(f);
   /* the log names the timeout */
   size_t ln = 0;
   char logb[4096] = "";
   snprintf(p, sizeof(p), "%s/compile.log", sdir);
   f = fopen(p, "r");
   if (f) {
      ln = fread(logb, 1, sizeof(logb) - 1, f);
      fclose(f);
   }
   logb[ln] = 0;
   CHECK(strstr(logb, "killed after 1 s"));
   free(img);
   printf(fails ? "test_compile_timeout: %d failure(s)\n" : "test_compile_timeout: all passed\n", fails);
   return fails != 0;
}
