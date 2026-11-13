/* compile.c - see compile.h. Ported from the patched driver (anv_cm.c); the differences: the helper always runs with
 * CM_OUT_DIR and CM_NO_LOAD_PAYLOAD=1, its output is always packed here (a helper that ignores CM_OUT_DIR, i.e. one
 * of an older package, cannot compile for the layer and counts as a failure), and the cache holds CML1 images. */
#define _GNU_SOURCE
#include "compile.h"
#include "cmk.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#define XMX_PACKER_VERSION "cml1-l1"

static uint8_t *
read_file(const char *path, size_t *size)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   uint8_t *buf = NULL;
   size_t n = 0, cap = 0;
   for (;;) {
      if (cap - n < 65536) {
         uint8_t *nb = realloc(buf, cap + 65536 + 1);
         if (!nb) {
            free(buf);
            fclose(f);
            return NULL;
         }
         buf = nb;
         cap += 65536;
      }
      const size_t got = fread(buf + n, 1, cap - n, f);
      n += got;
      if (got == 0)
         break;
   }
   const bool err = ferror(f);
   fclose(f);
   if (err) {
      free(buf);
      return NULL;
   }
   buf[n] = 0;
   *size = n;
   return buf;
}

void
xmx_mkdirs(const char *path)
{
   char tmp[512];
   snprintf(tmp, sizeof(tmp), "%s", path);
   for (char *p = tmp + 1; *p; p++)
      if (*p == '/') {
         *p = 0;
         mkdir(tmp, 0755);
         *p = '/';
      }
   mkdir(tmp, 0755);
}

void
xmx_compile_log(const char *fmt, ...)
{
   char path[600];
   const char *log = getenv("CM_LOG"), *xdg = getenv("XDG_CACHE_HOME");
   if (log && log[0])
      snprintf(path, sizeof(path), "%s", log);
   else if (xdg && xdg[0])
      snprintf(path, sizeof(path), "%s/xess-xmx/compile.log", xdg);
   else
      snprintf(path, sizeof(path), "%s/.cache/xess-xmx/compile.log", getenv("HOME") ? getenv("HOME") : "/tmp");
   /* compiles run in parallel (see prefetch): one line at a time */
   static pthread_mutex_t lm = PTHREAD_MUTEX_INITIALIZER;
   pthread_mutex_lock(&lm);
   FILE *f = fopen(path, "a");
   if (f) {
      va_list ap;
      va_start(ap, fmt);
      vfprintf(f, fmt, ap);
      va_end(ap);
      fclose(f);
   }
   pthread_mutex_unlock(&lm);
}

/* the compile script: XMX_LAYER_HELPER, else ANV_CM_HELPER, else <package>/tools/cm-compile.sh next to
 * ANV_CM_KERNEL_DIR (the package's environment file sets both of the latter) */
static bool
helper_path(char *buf, size_t len)
{
   const char *h = getenv("XMX_LAYER_HELPER");
   if (!h || !h[0])
      h = getenv("ANV_CM_HELPER");
   if (h && h[0]) {
      snprintf(buf, len, "%s", h);
      return true;
   }
   const char *kd = getenv("ANV_CM_KERNEL_DIR");
   if (!kd || !kd[0])
      return false;
   snprintf(buf, len, "%s/../tools/cm-compile.sh", kd);
   return true;
}

static uint32_t gen_value;
static once_flag gen_once = ONCE_FLAG_INIT;
static void
gen_init(void)
{
   uint32_t h = 2166136261u;
#define MIX(byte) (h = (h ^ (uint8_t)(byte)) * 16777619u)
   char helper[600], dir[700];
   if (helper_path(helper, sizeof(helper))) {
      char *slash = strrchr(helper, '/');
      if (slash)
         *slash = 0;
      const char *sub[] = { "/../igc/neo/bin", "/../igc/igc/local/lib" };
      for (unsigned i = 0; i < 2; i++) {
         snprintf(dir, sizeof(dir), "%s%s", helper, sub[i]);
         DIR *d = opendir(dir);
         if (!d)
            continue;
         struct dirent *e;
         while ((e = readdir(d))) {
            if (strncmp(e->d_name, "ocloc", 5) != 0 && strncmp(e->d_name, "libigc.so.", 10) != 0)
               continue;
            for (const char *c = e->d_name; *c; c++)
               MIX(*c);
         }
         closedir(d);
      }
      snprintf(dir, sizeof(dir), "%s/cm-compile.sh", helper);
      size_t size;
      uint8_t *data = read_file(dir, &size);
      if (data) {
         for (size_t k = 0; k < size; k++)
            MIX(data[k]);
         free(data);
         h = (h ^ (uint32_t)size) * 16777619u;
      }
   }
   for (const char *c = XMX_PACKER_VERSION; *c; c++)
      MIX(*c);
   const char *cmdev = getenv("CM_DEVICE");
   for (const char *c = cmdev ? cmdev : ""; *c; c++)
      MIX(*c);
#undef MIX
   gen_value = h;
}

uint32_t
xmx_toolchain_gen(void)
{
   call_once(&gen_once, gen_init);
   return gen_value;
}

void
xmx_cache_path(const struct xmx_dev_info *dev, const char *hash, char *dir, size_t dirlen, char *path, size_t pathlen)
{
   const char *xdg = getenv("XDG_CACHE_HOME");
   if (xdg && xdg[0])
      snprintf(dir, dirlen, "%s/xess-xmx/kernels", xdg);
   else
      snprintf(dir, dirlen, "%s/.cache/xess-xmx/kernels", getenv("HOME") ? getenv("HOME") : "/tmp");
   /* the GRF cap goes into the toolchain value as well as into the name */
   uint32_t tc = xmx_toolchain_gen();
   for (unsigned i = 0; i < 4; i++)
      tc = (tc ^ (uint8_t)(dev->max_grf >> (8 * i))) * 16777619u;
   char grf[16] = "";
   if (dev->max_grf)
      snprintf(grf, sizeof(grf), "-g%u", dev->max_grf);
   snprintf(path, pathlen, "%s/%s_%04x_%s%s_%08x.cml", dir, hash, dev->pci_id, dev->gen ? dev->gen : "any", grf, tc);
}

static void
rm_dir(const char *d)
{
   DIR *dir = opendir(d);
   if (dir) {
      struct dirent *e;
      char f[900];
      while ((e = readdir(dir)))
         if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            snprintf(f, sizeof(f), "%s/%s", d, e->d_name);
            unlink(f);
         }
      closedir(dir);
   }
   rmdir(d);
}

/* the first k_*.bin ocloc wrote into outdir */
static bool
find_zebin(const char *outdir, char *bin, size_t len)
{
   DIR *dir = opendir(outdir);
   if (!dir)
      return false;
   struct dirent *e;
   char best[256] = "";
   while ((e = readdir(dir))) {
      const size_t l = strlen(e->d_name);
      if (l > 6 && !strncmp(e->d_name, "k_", 2) && !strcmp(e->d_name + l - 4, ".bin") &&
          (!best[0] || strcmp(e->d_name, best) < 0))
         snprintf(best, sizeof(best), "%s", e->d_name);
   }
   closedir(dir);
   if (!best[0])
      return false;
   snprintf(bin, len, "%s/%s", outdir, best);
   return true;
}

/* Seconds one run of the helper may take, as in the driver: ANV_CM_COMPILE_TIMEOUT, default 120, 0 = no limit. A
 * helper past it is killed with its process group (the ocloc it started) and counts as a failed compile. */
static unsigned
compile_timeout(void)
{
   const char *t = getenv("ANV_CM_COMPILE_TIMEOUT");
   if (!t || !t[0])
      return 120;
   char *end;
   const unsigned long v = strtoul(t, &end, 10);
   return *end ? 120 : (unsigned)(v < 86400ul ? v : 86400ul);
}

/* Waits for the helper, which leads its own process group. Its exit code, -1 if a signal ended it, -2 if the deadline
 * passed (the group is killed then). A helper reaped by someone else (SIGCHLD ignored by the game) counts as exit
 * code 0: packing then shows whether it worked. */
static int
wait_helper(pid_t pid, unsigned timeout_s)
{
   struct timespec t0, now;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   for (;;) {
      int status = 0;
      const pid_t r = waitpid(pid, &status, timeout_s ? WNOHANG : 0);
      if (r == pid)
         return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      if (r < 0) {
         if (errno == EINTR)
            continue;
         return 0;
      }
      if (r != 0)
         continue;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if ((now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9 >= timeout_s) {
         kill(-pid, SIGKILL);   /* the group of this helper only (posix_spawn made it its leader) */
         while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
         return -2;
      }
      const struct timespec nap = { 0, 5 * 1000 * 1000 };
      nanosleep(&nap, NULL);
   }
}

/* runs the helper for <sdir>/<name>.spv and writes the CML1 image to path */
static bool
compile_one(const struct xmx_dev_info *dev, const char *sdir, const char *name, const char *hash, const char *cdir,
            const char *path)
{
   char spv[600], opt[600];
   snprintf(spv, sizeof(spv), "%s/%s.spv", sdir, name);
   snprintf(opt, sizeof(opt), "%s/%s.opt", sdir, name);
   if (access(spv, R_OK) || access(opt, R_OK))
      return false;
   char helper[600];
   if (!helper_path(helper, sizeof(helper))) {
      xmx_compile_log("cm-compile FAILED (layer): %s: no compile script (set ANV_CM_HELPER or ANV_CM_KERNEL_DIR)\n", spv);
      return false;
   }
   xmx_mkdirs(cdir);
   char outdir[600];
   const char *tmpdir = getenv("TMPDIR");
   snprintf(outdir, sizeof(outdir), "%s/cml.XXXXXX", tmpdir && tmpdir[0] ? tmpdir : "/tmp");
   if (!mkdtemp(outdir))
      return false;
   char pci[32], grf[32], maxgrf[32], out[640], ignored[700];
   snprintf(pci, sizeof(pci), "CM_PCI_ID=0x%04x", dev->pci_id);
   snprintf(grf, sizeof(grf), "CM_GRF_BYTES=%u", dev->grf_bytes);
   snprintf(maxgrf, sizeof(maxgrf), "CM_MAX_GRF=%u", dev->max_grf);
   snprintf(out, sizeof(out), "CM_OUT_DIR=%s", outdir);
   snprintf(ignored, sizeof(ignored), "%s/helper-out.cmk", outdir);   /* never read: see the file comment */
   extern char **environ;
   unsigned nenv = 0;
   while (environ[nenv])
      nenv++;
   char **envp = calloc(nenv + 6, sizeof(char *));
   if (!envp) {
      rm_dir(outdir);
      return false;
   }
   unsigned ne = 0;
   for (unsigned e = 0; e < nenv; e++)
      if (strncmp(environ[e], "CM_PCI_ID=", 10) && strncmp(environ[e], "CM_GRF_BYTES=", 13) &&
          strncmp(environ[e], "CM_OUT_DIR=", 11) && strncmp(environ[e], "CM_NO_LOAD_PAYLOAD=", 19) &&
          strncmp(environ[e], "CM_MAX_GRF=", 11))
         envp[ne++] = environ[e];
   envp[ne++] = pci;
   envp[ne++] = grf;
   envp[ne++] = out;
   envp[ne++] = (char *)"CM_NO_LOAD_PAYLOAD=1";
   if (dev->max_grf)
      envp[ne++] = maxgrf;   /* Xe2: the script drops the kernel's 256-GRF options */
   char *argv[] = { (char *)"/bin/sh", helper, spv, opt, ignored, NULL };
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   /* as in the driver: the helper leads its own process group (so one past its deadline is killed together with its
    * ocloc), no signal blocked, SIGCHLD/SIGPIPE at their defaults, stdin /dev/null */
   posix_spawnattr_t attr;
   posix_spawn_file_actions_t fa;
   sigset_t none, dfl;
   sigemptyset(&none);
   sigemptyset(&dfl);
   sigaddset(&dfl, SIGCHLD);
   sigaddset(&dfl, SIGPIPE);
   posix_spawnattr_init(&attr);
   posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
   posix_spawnattr_setpgroup(&attr, 0);
   posix_spawnattr_setsigmask(&attr, &none);
   posix_spawnattr_setsigdefault(&attr, &dfl);
   posix_spawn_file_actions_init(&fa);
   posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
   pid_t pid;
   int rc = posix_spawn(&pid, "/bin/sh", &fa, &attr, argv, envp);
   posix_spawn_file_actions_destroy(&fa);
   posix_spawnattr_destroy(&attr);
   free(envp);
   const unsigned timeout_s = compile_timeout();
   if (rc == 0)
      rc = wait_helper(pid, timeout_s);
   bool ok = false;
   char err[300] = "", bin[900], infop[700];
   snprintf(infop, sizeof(infop), "%s/info", outdir);
   size_t infon = 0;
   uint8_t *info = read_file(infop, &infon);
   if (rc == -2) {
      snprintf(err, sizeof(err), "helper killed after %u s (ANV_CM_COMPILE_TIMEOUT)", timeout_s);
   } else if (rc != 0) {
      snprintf(err, sizeof(err), "helper exit %d", rc);
   } else if (!info || !strstr((char *)info, "-noLoadPayload")) {
      snprintf(err, sizeof(err), "the compile script does not support CM_NO_LOAD_PAYLOAD (older package?)");
   } else if (!find_zebin(outdir, bin, sizeof(bin))) {
      snprintf(err, sizeof(err), "no zebin");
   } else {
      size_t zn, on;
      uint8_t *z = read_file(bin, &zn), *o = read_file(opt, &on);
      if (z && o) {
         uint64_t lws[3];
         xmx_opt_lws(o, on, lws);
         size_t cn, mn;
         uint32_t gc;
         uint8_t *cmk = xmx_pack(z, zn, lws, dev->grf_bytes, &cn, &gc, err, sizeof(err));
         if (cmk && dev->max_grf && xmx_rd32(cmk + XMX_H_GRF) > dev->max_grf) {
            /* a script that ignores CM_MAX_GRF (older package): the driver would run it with too few registers */
            snprintf(err, sizeof(err), "kernel compiled for %u GRF, more than the %u this GPU runs (compile script "
                     "without CM_MAX_GRF?)", xmx_rd32(cmk + XMX_H_GRF), dev->max_grf);
            free(cmk);
            cmk = NULL;
         }
         uint8_t *cml = cmk ? xmx_cml_from_cmk(cmk, cn, gc, XMX_CML_NO_LOAD_PAYLOAD, &mn) : NULL;
         if (cml) {
            static atomic_int seq;
            char tmp[800];
            snprintf(tmp, sizeof(tmp), "%s.%d.%d.tmp", path, (int)getpid(), atomic_fetch_add(&seq, 1));
            FILE *f = fopen(tmp, "wb");
            ok = f && fwrite(cml, 1, mn, f) == mn;
            if (f && fclose(f) != 0)
               ok = false;
            if (ok && rename(tmp, path) != 0)
               ok = false;
            if (!ok) {
               snprintf(err, sizeof(err), "cannot write %s: %s", tmp, strerror(errno));
               unlink(tmp);
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (ok) {
               const char *ob = strrchr(path, '/');
               xmx_compile_log("cm-compile ok (layer): %s from %s.spv lws %" PRIu64 "x%" PRIu64 "x%" PRIu64
                               " group_count %s in %.1f s\n", ob ? ob + 1 : path, name, lws[0], lws[1], lws[2],
                               gc == XMX_NO_GC ? "no" : "yes",
                               (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
            }
         } else if (cmk && !err[0]) {
            snprintf(err, sizeof(err), "CML1 conversion");
         }
         free(cml);
         free(cmk);
      }
      free(z);
      free(o);
   }
   free(info);
   rm_dir(outdir);
   if (!ok)
      xmx_compile_log("cm-compile FAILED (layer): %s: %s\n", spv, err);
   (void)hash;
   return ok;
}

/* ---- compile jobs ----------------------------------------------------------------------------------------------
 * As the patched driver (anv_cm_prefetch): every kernel this process compiles has a job, keyed by its cache file. The
 * kernel a pipeline needs now is compiled by the thread creating that pipeline (XeSS asks for its kernels one at a
 * time, inside vkCreateComputePipelines). On such a miss, the other kernels in the shim's folder that have no cache file
 * (all of them on a first launch with a filled folder, after a driver, IGC or helper update, or with a new cache) are
 * queued for worker threads, so that they are ready when XeSS asks for them. XMX_LAYER_COMPILE_JOBS is the number of
 * compiles at a time, the needing thread included (default the online CPUs, at most 8; 1 = one after another, no
 * workers). A thread that needs a kernel a worker is compiling waits for it; a queued one it takes over. A failed
 * background compile is not final (the kernel is compiled again when it is needed); a failed compile of a needed
 * kernel is not tried again in this process (the next launch retries it), and only that one is marked in
 * compile_failed.txt. Cache files are written under a private name and renamed into place (compile_one). */
enum job_state { JOB_QUEUED, JOB_RUNNING, JOB_OK, JOB_PREFETCH_FAILED, JOB_FAILED };

struct job {
   enum job_state state;
   struct xmx_dev_info dev;   /* this and the fields below never change after the job is created */
   char hash[20], name[32];
   char sdir[512], cdir[512], path[700];
};

#define MAX_JOBS 1024
static struct job *jobs[MAX_JOBS];   /* never freed: a few hundred per process at most */
static unsigned njobs, nworkers;
static pthread_mutex_t jobs_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cnd = PTHREAD_COND_INITIALIZER;
/* the current batch (from the first queued or running job to the moment none is left), for one log line */
static unsigned busy, batch_n, batch_failed;
static struct timespec batch_t0;

/* XMX_LAYER_COMPILE_JOBS (1..32), else the online CPUs, at most 8 */
static unsigned
max_jobs(void)
{
   const char *s = getenv("XMX_LAYER_COMPILE_JOBS");
   if (s && s[0]) {
      char *end;
      const unsigned long v = strtoul(s, &end, 10);
      if (!*end)
         return v < 1 ? 1 : v > 32 ? 32 : (unsigned)v;
   }
   const long n = sysconf(_SC_NPROCESSORS_ONLN);
   return n < 1 ? 1 : n > 8 ? 8 : (unsigned)n;
}

/* one more job queued or running (the first starts a batch); job_done: one finished (the last one ends the batch and
 * logs it). Both called with jobs_mtx held. */
static void
job_busy(void)
{
   if (busy++ == 0) {
      clock_gettime(CLOCK_MONOTONIC, &batch_t0);
      batch_n = batch_failed = 0;
   }
   batch_n++;
}

static void
job_done(struct job *j, enum job_state st)
{
   j->state = st;
   batch_failed += st != JOB_OK;
   pthread_cond_broadcast(&jobs_cnd);
   if (--busy == 0 && batch_n > 1) {
      struct timespec t1;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      xmx_compile_log("compile jobs (layer): %u kernel(s) (%u failed) in %.1f s, %u at a time\n", batch_n,
                      batch_failed, (t1.tv_sec - batch_t0.tv_sec) + (t1.tv_nsec - batch_t0.tv_nsec) / 1e9,
                      max_jobs());
   }
}

/* The job of cache file path, created in state st if there is none (*created); NULL if the table is full. Called with
 * jobs_mtx held. */
static struct job *
job_get(const char *path, enum job_state st, const struct xmx_dev_info *dev, const char *sdir, const char *name,
        const char *hash, const char *cdir, bool *created)
{
   *created = false;
   for (unsigned i = 0; i < njobs; i++)
      if (!strcmp(jobs[i]->path, path))
         return jobs[i];
   if (njobs == MAX_JOBS)
      return NULL;
   struct job *j = calloc(1, sizeof(*j));
   if (!j)
      return NULL;
   j->state = st;
   j->dev = *dev;
   snprintf(j->hash, sizeof(j->hash), "%s", hash);
   snprintf(j->name, sizeof(j->name), "%s", name);
   snprintf(j->sdir, sizeof(j->sdir), "%s", sdir);
   snprintf(j->cdir, sizeof(j->cdir), "%s", cdir);
   snprintf(j->path, sizeof(j->path), "%s", path);
   jobs[njobs++] = j;
   *created = true;
   job_busy();
   return j;
}

static void *
worker(void *arg)
{
   (void)arg;
   pthread_mutex_lock(&jobs_mtx);
   for (;;) {
      struct job *j = NULL;
      for (unsigned i = 0; i < njobs && !j; i++)
         if (jobs[i]->state == JOB_QUEUED)
            j = jobs[i];
      if (!j)
         break;
      j->state = JOB_RUNNING;
      pthread_mutex_unlock(&jobs_mtx);
      /* another process may have compiled it meanwhile */
      const bool ok = access(j->path, R_OK) == 0 || compile_one(&j->dev, j->sdir, j->name, j->hash, j->cdir, j->path);
      pthread_mutex_lock(&jobs_mtx);
      job_done(j, ok ? JOB_OK : JOB_PREFETCH_FAILED);
   }
   nworkers--;   /* workers end when the queue is empty; the next miss starts new ones */
   pthread_mutex_unlock(&jobs_mtx);
   return NULL;
}

/* The workers run code of this library: it stays mapped even if the loader unloads the layer (the last
 * vkDestroyInstance) while they compile. */
static void
pin_library(void)
{
   Dl_info dl;
   if (dladdr((void *)pin_library, &dl) && dl.dli_fname)
      (void)dlopen(dl.dli_fname, RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE);
}

static bool
hash_ok(const char *h)
{
   for (unsigned i = 0; i < 16; i++)
      if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f')))
         return false;
   return true;
}

/* Queues every kernel of the shim's folder sdir that has no cache file and no job in this process (<16 hex
 * digits>.spv with its .opt: the shim writes both under a temporary name and renames them, the .opt last), and starts
 * workers for them. */
static void
prefetch(const struct xmx_dev_info *dev, const char *sdir)
{
   const unsigned max = max_jobs() - 1;
   if (max == 0)
      return;
   DIR *d = opendir(sdir);
   if (!d)
      return;
   unsigned queued = 0;
   struct dirent *e;
   while ((e = readdir(d))) {
      char hash[17], opt[600], cdir[512], path[700];
      if (strlen(e->d_name) != 20 || strcmp(e->d_name + 16, ".spv") != 0)
         continue;
      memcpy(hash, e->d_name, 16);
      hash[16] = 0;
      if (!hash_ok(hash))
         continue;
      xmx_cache_path(dev, hash, cdir, sizeof(cdir), path, sizeof(path));
      if (access(path, R_OK) == 0)
         continue;
      snprintf(opt, sizeof(opt), "%s/%s.opt", sdir, hash);
      if (access(opt, R_OK) != 0)
         continue;
      bool created;
      pthread_mutex_lock(&jobs_mtx);
      job_get(path, JOB_QUEUED, dev, sdir, hash, hash, cdir, &created);
      pthread_mutex_unlock(&jobs_mtx);
      queued += created;
   }
   closedir(d);
   if (queued == 0)
      return;
   static pthread_once_t pin_once = PTHREAD_ONCE_INIT;
   pthread_once(&pin_once, pin_library);
   pthread_mutex_lock(&jobs_mtx);
   unsigned waiting = 0;
   for (unsigned i = 0; i < njobs; i++)
      waiting += jobs[i]->state == JOB_QUEUED;
   unsigned started = 0;
   while (nworkers < max && nworkers < waiting) {
      /* signals blocked in the workers, so that none of the game's handlers runs on them */
      sigset_t all, old;
      sigfillset(&all);
      pthread_sigmask(SIG_SETMASK, &all, &old);
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      pthread_t t;
      const int rc = pthread_create(&t, &attr, worker, NULL);
      pthread_attr_destroy(&attr);
      pthread_sigmask(SIG_SETMASK, &old, NULL);
      if (rc != 0)
         break;
      nworkers++;
      started++;
   }
   pthread_mutex_unlock(&jobs_mtx);
   xmx_compile_log("compile jobs (layer): %u more kernel(s) without a cache file queued, %u background compile(s) "
                   "started\n", queued, started);
}

/* A kernel needed now: compiled on this thread, or waited for if a worker is compiling it. with_prefetch: queue the
 * other kernels of sdir too (placeholders of the hash format). *fresh is false when the kernel failed earlier in this
 * process and was not tried again. */
static bool
compile_needed(const struct xmx_dev_info *dev, const char *sdir, const char *name, const char *hash, const char *cdir,
               const char *path, bool with_prefetch, bool *fresh)
{
   *fresh = false;
   bool created;
   pthread_mutex_lock(&jobs_mtx);
   struct job *j = job_get(path, JOB_RUNNING, dev, sdir, name, hash, cdir, &created);
   while (j && !created) {
      if (j->state == JOB_RUNNING) {
         pthread_cond_wait(&jobs_cnd, &jobs_mtx);
         continue;
      }
      if (j->state == JOB_FAILED || (j->state == JOB_OK && access(path, R_OK) == 0)) {
         const bool ok = j->state == JOB_OK;
         pthread_mutex_unlock(&jobs_mtx);
         return ok;
      }
      /* still queued (taken over), failed in the background, or its file is gone: compile it here */
      if (j->state != JOB_QUEUED)
         job_busy();
      j->state = JOB_RUNNING;
      break;
   }
   pthread_mutex_unlock(&jobs_mtx);
   if (with_prefetch)
      prefetch(dev, sdir);
   *fresh = true;
   const bool ok = compile_one(dev, sdir, name, hash, cdir, path);
   if (j) {
      pthread_mutex_lock(&jobs_mtx);
      job_done(j, ok ? JOB_OK : JOB_FAILED);
      pthread_mutex_unlock(&jobs_mtx);
   }
   return ok;
}

bool
xmx_kernel_get(const struct xmx_dev_info *dev, const char *sdir, int id, const char *name, const char *hash,
               uint8_t **img, size_t *n)
{
   char cdir[512], path[700];
   xmx_cache_path(dev, hash, cdir, sizeof(cdir), path, sizeof(path));
   struct xmx_cml m;
   if ((*img = read_file(path, n)) && xmx_cml_parse(*img, *n, &m))
      return true;
   free(*img);
   *img = NULL;
   bool fresh;
   if (compile_needed(dev, sdir, name, hash, cdir, path, id < 0, &fresh) && (*img = read_file(path, n)) &&
       xmx_cml_parse(*img, *n, &m))
      return true;
   free(*img);
   *img = NULL;
   if (!fresh)
      return false;   /* failed before in this process: marked then */
   char mark[600];
   snprintf(mark, sizeof(mark), "%s/compile_failed.txt", sdir);
   pthread_mutex_lock(&jobs_mtx);   /* one appending thread at a time */
   FILE *ff = fopen(mark, "a");
   if (ff) {
      fprintf(ff, "%d %s\n", id, hash);
      fclose(ff);
   }
   pthread_mutex_unlock(&jobs_mtx);
   return false;
}
