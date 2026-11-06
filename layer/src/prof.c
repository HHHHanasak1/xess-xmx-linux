/* prof.c - see prof.h. Every slot keeps two sets of counters: the current interval and the whole run. */
#define _GNU_SOURCE
#include "prof.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#define MAX_KERNELS 512
#define TOP 12

struct slot {
   char hash[17];
   atomic_uint_fast64_t ns, n;           /* interval */
   atomic_uint_fast64_t total_ns, total_n;
};

static struct slot slots[MAX_KERNELS];
static atomic_uint nslots;
static mtx_t mtx;
static once_flag once = ONCE_FLAG_INIT;
static bool enabled;
static int mode;
static double period = 5.0;
static atomic_uint_fast64_t presents, total_presents, lost, total_lost;
static double t_start, t_last;
static atomic_flag reporting = ATOMIC_FLAG_INIT;

static double
now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void
init(void)
{
   mtx_init(&mtx, mtx_plain);
   const char *e = getenv("XMX_LAYER_PROF");
   enabled = e && *e && strcmp(e, "0");
   mode = enabled ? (atoi(e) == 2 ? 2 : 1) : 0;
   const char *p = getenv("XMX_LAYER_PROF_SEC");
   if (p && atof(p) > 0.5)
      period = atof(p);
   t_start = t_last = now();
}

bool
xprof_enabled(void)
{
   call_once(&once, init);
   return enabled;
}

int
xprof_kernel(const char *hash)
{
   if (!xprof_enabled())
      return -1;
   mtx_lock(&mtx);
   unsigned n = atomic_load(&nslots);
   int r = -1;
   for (unsigned i = 0; i < n && r < 0; i++)
      if (!strcmp(slots[i].hash, hash))
         r = (int)i;
   if (r < 0 && n < MAX_KERNELS) {
      strncpy(slots[n].hash, hash, sizeof(slots[n].hash) - 1);
      atomic_store(&nslots, n + 1);
      r = (int)n;
   }
   mtx_unlock(&mtx);
   return r;
}

void
xprof_add(int k, uint64_t ns)
{
   if (k < 0 || k >= MAX_KERNELS)
      return;
   atomic_fetch_add_explicit(&slots[k].ns, ns, memory_order_relaxed);
   atomic_fetch_add_explicit(&slots[k].n, 1, memory_order_relaxed);
}

void
xprof_lost(unsigned n)
{
   atomic_fetch_add_explicit(&lost, n, memory_order_relaxed);
}

struct row { unsigned i; uint64_t ns, n; };
static int
by_ns(const void *a, const void *b)
{
   const struct row *x = a, *y = b;
   return x->ns < y->ns ? 1 : x->ns > y->ns ? -1 : 0;
}

/* interval: move the interval counters into the totals; final: report the totals */
static void
report(xprof_log_fn log, bool final, double secs, uint64_t np, uint64_t nl)
{
   unsigned n = atomic_load(&nslots);
   struct row *rows = calloc(n ? n : 1, sizeof(*rows));
   uint64_t all_ns = 0, all_n = 0;
   for (unsigned i = 0; i < n; i++) {
      uint64_t ns, c;
      if (final) {
         ns = atomic_load(&slots[i].total_ns);
         c = atomic_load(&slots[i].total_n);
      } else {
         ns = atomic_exchange(&slots[i].ns, 0);
         c = atomic_exchange(&slots[i].n, 0);
         atomic_fetch_add(&slots[i].total_ns, ns);
         atomic_fetch_add(&slots[i].total_n, c);
      }
      rows[i] = (struct row){ i, ns, c };
      all_ns += ns;
      all_n += c;
   }
   qsort(rows, n, sizeof(*rows), by_ns);
   const double fps = np / secs;
   log("prof%s: %.1f s, %.1f presents/s, kernels %.2f ms/s GPU (%.3f ms per present), %.0f dispatches/s, %llu lost",
       final ? " total" : "", secs, fps, all_ns / 1e6 / secs, np ? all_ns / 1e6 / np : 0.0, all_n / secs,
       (unsigned long long)nl);
   for (unsigned j = 0; j < n && j < TOP && rows[j].n; j++)
      log("prof%s:   %s %6.1f/s avg %7.1f us  %6.2f ms/s  %4.1f%%", final ? " total" : "", slots[rows[j].i].hash,
          rows[j].n / secs, rows[j].ns / 1e3 / rows[j].n, rows[j].ns / 1e6 / secs,
          all_ns ? 100.0 * rows[j].ns / all_ns : 0.0);
   free(rows);
}

void
xprof_present(xprof_log_fn log)
{
   if (!xprof_enabled())
      return;
   atomic_fetch_add_explicit(&presents, 1, memory_order_relaxed);
   const double t = now();
   if (t - t_last < period || atomic_flag_test_and_set(&reporting))
      return;
   const double secs = t - t_last;
   t_last = t;
   const uint64_t np = atomic_exchange(&presents, 0), nl = atomic_exchange(&lost, 0);
   atomic_fetch_add(&total_presents, np);
   atomic_fetch_add(&total_lost, nl);
   report(log, false, secs, np, nl);
   atomic_flag_clear(&reporting);
}

void
xprof_final(xprof_log_fn log)
{
   if (!xprof_enabled() || atomic_flag_test_and_set(&reporting))
      return;
   /* the last partial interval into the totals */
   for (unsigned i = 0; i < atomic_load(&nslots); i++) {
      atomic_fetch_add(&slots[i].total_ns, atomic_exchange(&slots[i].ns, 0));
      atomic_fetch_add(&slots[i].total_n, atomic_exchange(&slots[i].n, 0));
   }
   atomic_fetch_add(&total_presents, atomic_exchange(&presents, 0));
   atomic_fetch_add(&total_lost, atomic_exchange(&lost, 0));
   if (atomic_load(&total_presents))   /* a device that never presented (a launcher, a probe) has nothing to say */
      report(log, true, now() - t_start, atomic_load(&total_presents), atomic_load(&total_lost));
   atomic_flag_clear(&reporting);
}

int
xprof_mode(void)
{
   call_once(&once, init);
   return mode;
}
