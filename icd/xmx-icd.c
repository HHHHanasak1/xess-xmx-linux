/* xmx-icd.c - lib/libvulkan_intel.so of the package: a Vulkan ICD that loads one of the package's patched drivers
 * (lib/mesa-<version>/libvulkan_intel.so) and hands the loader its three entry points.
 * Which one: MESA=<version or prefix> in the process' environment (a game's launch options: MESA=26.1 %command%),
 * else the version named in lib/active (tools/switch-driver.sh), else any. A driver that does not load is skipped.
 * A prefix picks the exact version if there is one, else a final release before a release candidate, else the newest.
 * Build: gcc -O2 -shared -fPIC -fvisibility=hidden -o libvulkan_intel.so xmx-icd.c -ldl -lpthread */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
#define VK_ERROR_INCOMPATIBLE_DRIVER (-9)

typedef void (*vk_void_fn)(void);
typedef vk_void_fn (*gipa_fn)(void *instance, const char *name);
typedef int (*negotiate_fn)(unsigned *version);

static gipa_fn real_gipa, real_gpdpa;
static negotiate_fn real_negotiate;
static pthread_once_t once = PTHREAD_ONCE_INIT;

/* rank of a version string for a prefix match: final releases (no '-') above release candidates, then by name */
static int better(const char *a, const char *b)
{
   int fa = strchr(a, '-') == NULL, fb = strchr(b, '-') == NULL;
   if (fa != fb)
      return fa > fb;
   return strverscmp(a, b) > 0;
}

/* version directory in libdir for "want" (exact or prefix; NULL: any) -> out, 0 if none */
static int pick(const char *libdir, const char *want, const char *skip, char *out, size_t n)
{
   DIR *d = opendir(libdir);
   struct dirent *e;
   char best[256] = "", path[PATH_MAX];
   if (!d)
      return 0;
   while ((e = readdir(d))) {
      const char *v = e->d_name + 5;
      if (strncmp(e->d_name, "mesa-", 5) || (skip && !strcmp(v, skip)))
         continue;
      if (want && strncmp(v, want, strlen(want)))
         continue;
      snprintf(path, sizeof(path), "%s/%s/libvulkan_intel.so", libdir, e->d_name);
      if (access(path, R_OK))
         continue;
      if (want && !strcmp(v, want)) {
         snprintf(best, sizeof(best), "%s", v);
         break;
      }
      if (!best[0] || better(v, best))
         snprintf(best, sizeof(best), "%s", v);
   }
   closedir(d);
   snprintf(out, n, "%s", best);
   return best[0] != 0;
}

static void *open_version(const char *libdir, const char *v)
{
   char path[PATH_MAX];
   void *h;
   snprintf(path, sizeof(path), "%s/mesa-%s/libvulkan_intel.so", libdir, v);
   h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (!h)
      fprintf(stderr, "xess-xmx: cannot load the Mesa %s driver: %s\n", v, dlerror());
   return h;
}

static void load(void)
{
   Dl_info info;
   char self[PATH_MAX], libdir[PATH_MAX], active[256] = "", v[256], tried[256] = "";
   const char *want = getenv("MESA");
   void *h = NULL;
   FILE *f;

   if (!dladdr((void *)load, &info) || !realpath(info.dli_fname, self))
      return;
   snprintf(libdir, sizeof(libdir), "%s", self);
   *strrchr(libdir, '/') = 0;

   if (want && *want) {
      if (pick(libdir, want, NULL, v, sizeof(v))) {
         h = open_version(libdir, v);
         snprintf(tried, sizeof(tried), "%s", v);
      } else {
         fprintf(stderr, "xess-xmx: MESA=%s: no such driver in %s, using the default\n", want, libdir);
      }
   }
   if (!h) {
      char path[PATH_MAX + 8];
      snprintf(path, sizeof(path), "%s/active", libdir);
      if ((f = fopen(path, "r"))) {
         if (fgets(active, sizeof(active), f))
            active[strcspn(active, " \r\n")] = 0;
         fclose(f);
      }
      if (active[0] && strcmp(active, tried) && pick(libdir, active, NULL, v, sizeof(v)) && !strcmp(v, active)) {
         h = open_version(libdir, v);
         snprintf(tried, sizeof(tried), "%s", v);
      }
   }
   if (!h && pick(libdir, NULL, tried[0] ? tried : NULL, v, sizeof(v)))
      h = open_version(libdir, v);
   if (!h)
      return;
   if (want && *want)
      fprintf(stderr, "xess-xmx: using the Mesa %s driver\n", v);
   real_negotiate = (negotiate_fn)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
   real_gipa = (gipa_fn)dlsym(h, "vk_icdGetInstanceProcAddr");
   real_gpdpa = (gipa_fn)dlsym(h, "vk_icdGetPhysicalDeviceProcAddr");
}

EXPORT int vk_icdNegotiateLoaderICDInterfaceVersion(unsigned *version)
{
   pthread_once(&once, load);
   return real_negotiate ? real_negotiate(version) : VK_ERROR_INCOMPATIBLE_DRIVER;
}

EXPORT vk_void_fn vk_icdGetInstanceProcAddr(void *instance, const char *name)
{
   pthread_once(&once, load);
   return real_gipa ? real_gipa(instance, name) : NULL;
}

EXPORT vk_void_fn vk_icdGetPhysicalDeviceProcAddr(void *instance, const char *name)
{
   pthread_once(&once, load);
   return real_gpdpa ? real_gpdpa(instance, name) : NULL;
}
