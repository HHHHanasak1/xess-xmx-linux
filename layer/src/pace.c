/* pace.c - XMX_LAYER_PACE: even spacing of frame generation's images at vkQueuePresentKHR.
 *
 * With XeSS-FG under vkd3d-proton, the n images of a rendered frame are presented on vkd3d's queue thread soon after
 * the game's Present, long before the GPU has made them; they all become ready when the GPU has finished the frame
 * and its interpolations, and gamescope then shows them one refresh apart, followed by a long gap. A wait inside
 * vkQueuePresentKHR cannot fix that: the call comes before the image is ready, and vkd3d's queue thread (which also
 * submits the game's next frame, under its lock of the VkQueue) would be held up.
 *
 * So with XMX_LAYER_PACE=1, vkQueuePresentKHR returns at once and the present happens later on the layer's thread:
 *  - on the caller's thread: one submission waits for the present's semaphores and signals the layer's semaphore and
 *    fence of that swap chain image; the present (image, present id, present fence, present mode) is queued;
 *  - the watcher thread waits for the fences in order: the time each image became ready (GPU done);
 *  - the presenter thread presents them in order. From the ready times it finds out whether the images come in
 *    bursts of n (frame generation), the rendered frame interval T and n; while they do, and T is steady, an image
 *    goes out at max(ready, previous present + K * T / n), the first one of a burst when it is ready. Holds are
 *    bounded. Otherwise each image is presented as soon as it is dequeued (no wait for the GPU).
 * The VkQueue and the swap chain are then used by two threads: the layer serialises its own present with every
 * queue command (vkQueueSubmit*, vkQueueBindSparse, vkQueueWaitIdle, vkDeviceWaitIdle) and swap chain command
 * (acquire, HDR metadata, release, destroy) passing through it. An acquire waits in steps of 0.5 ms so that it never
 * holds the swap chain for longer while the presenter needs it.
 * XMX_LAYER_PACE=log: the same submission and fences, the present stays on the caller's thread (measuring only).
 * Pacing holds images: the swap chain gets XMX_LAYER_PACE_IMAGES (default 2) more than asked for.
 * XMX_LAYER_PACE_K (default 0.97), XMX_LAYER_PACE_LOG=<file>, XMX_LAYER_PACE_TRACE=1: one line per present. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pace.h"

#define MAXQ 16
#define MAXSW 8
#define MAXIMG 16
#define NJOB 64
#define NHIST 48
#define MS 1000000ull

#define PACE_FNS(X) \
   X(QueueSubmit) X(QueueSubmit2) X(QueueSubmit2KHR) X(QueueBindSparse) X(QueueWaitIdle) X(DeviceWaitIdle) \
   X(QueuePresentKHR) X(AcquireNextImageKHR) X(AcquireNextImage2KHR) X(CreateSwapchainKHR) X(DestroySwapchainKHR) \
   X(GetSwapchainImagesKHR) X(SetHdrMetadataEXT) X(ReleaseSwapchainImagesKHR) X(ReleaseSwapchainImagesEXT) \
   X(CreateSemaphore) X(DestroySemaphore) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences)

struct pq {
   VkQueue h;
   pthread_mutex_t m;
};

struct psw {
   VkSwapchainKHR h;            /* 0: free */
   pthread_mutex_t m;
   uint32_t n;
   VkSemaphore sem[MAXIMG];     /* signalled by the layer's submission, waited by the present */
   VkFence fence[MAXIMG];
   bool busy[MAXIMG];           /* fence submitted, the watcher has not seen it signalled yet */
   VkResult sticky;             /* a deferred present failed: presents pass through until the swap chain is gone */
   int pending;                 /* queued presents not presented yet */
   int want;                    /* the presenter waits for m: a polling acquire steps aside (mutexes are not fair) */
};

struct pjob {
   struct psw *s;
   struct pq *q;                /* its lock (NULL beyond MAXQ queues) */
   VkQueue qh;
   uint32_t idx;
   bool has_id, has_pfence, has_mode;
   uint64_t id;
   VkFence pfence;
   VkPresentModeKHR mode;
   uint64_t call, ready;        /* ns: vkQueuePresentKHR called, image ready (GPU done) */
   bool watched, nowait;        /* nowait: no fence (the watcher skips it) */
};

struct xpace {
   uint64_t key;
   VkDevice dev;
   int mode;                    /* 1 pace, 2 log */
#define X(n) PFN_vk##n n;
   PACE_FNS(X)
#undef X
   pthread_mutex_t m;           /* everything below */
   pthread_cond_t cv;
   struct pq qs[MAXQ];
   int nq;
   struct psw sws[MAXSW];
   struct pjob jobs[NJOB];
   uint64_t tail, watch, head;  /* enqueued, watched, presented */
   pthread_t watcher, presenter;
   bool threads, quit;
   /* the ready times and what they show */
   uint64_t hist[NHIST];
   unsigned nhist, hpos;
   double T, Tspread;           /* ms */
   int n, good, ambiguous, bursts;
   bool active;
   uint64_t prev_ready, last_out;
   int k;                       /* position in the burst of the last present */
   double K;
   int extra_images;            /* XMX_LAYER_PACE_IMAGES: swap chain images added (default 2) */
   /* log */
   FILE *log;
   bool trace;
   uint64_t t0, logt;
   unsigned l_pres, l_active, l_held, l_cut;
   double l_hold, l_hold_max, l_late_max, l_call_max, l_lockw_max, l_acq, l_acq_max, l_lat, l_sub_max;
};

static struct xpace *paces[4];
static pthread_mutex_t paces_m = PTHREAD_MUTEX_INITIALIZER;

static uint64_t
now_ns(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void
sleep_until(uint64_t t)
{
   struct timespec ts = { .tv_sec = (time_t)(t / 1000000000ull), .tv_nsec = (long)(t % 1000000000ull) };
   while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
      ;
}

static void
abs_ts(struct timespec *ts, uint64_t t)
{
   /* pthread_cond_timedwait: CLOCK_MONOTONIC (the condition variable is made so) */
   ts->tv_sec = (time_t)(t / 1000000000ull);
   ts->tv_nsec = (long)(t % 1000000000ull);
}

static void __attribute__((format(printf, 2, 3)))
plog(struct xpace *p, const char *fmt, ...)
{
   if (!p->log)
      return;
   va_list ap;
   va_start(ap, fmt);
   fprintf(p->log, "%9.3f ", (now_ns() - p->t0) / 1e6);
   vfprintf(p->log, fmt, ap);
   fputc('\n', p->log);
   va_end(ap);
}

static struct xpace *
pace_of(void *dispatchable)
{
   const uint64_t k = (uint64_t)(uintptr_t)(*(void **)dispatchable);
   for (int i = 0; i < 4; i++)
      if (paces[i] && paces[i]->key == k)
         return paces[i];
   return NULL;
}

static struct pq *
pq_of(struct xpace *p, VkQueue q)
{
   for (int i = 0; i < p->nq; i++)
      if (p->qs[i].h == q)
         return &p->qs[i];
   pthread_mutex_lock(&p->m);
   struct pq *r = NULL;
   for (int i = 0; i < p->nq; i++)
      if (p->qs[i].h == q)
         r = &p->qs[i];
   if (!r && p->nq < MAXQ) {
      r = &p->qs[p->nq];
      pthread_mutex_init(&r->m, NULL);
      r->h = q;
      __atomic_store_n(&p->nq, p->nq + 1, __ATOMIC_RELEASE);
   }
   pthread_mutex_unlock(&p->m);
   return r;   /* NULL only beyond MAXQ queues: then that queue is not serialised (never seen) */
}

static struct psw *
psw_of(struct xpace *p, VkSwapchainKHR h)
{
   if (!h)
      return NULL;
   for (int i = 0; i < MAXSW; i++)
      if (p->sws[i].h == h)
         return &p->sws[i];
   return NULL;
}

#define QLOCK(q) do { if (q) pthread_mutex_lock(&(q)->m); } while (0)
#define QUNLOCK(q) do { if (q) pthread_mutex_unlock(&(q)->m); } while (0)

/* waits until no present of s (all swap chains: NULL) is queued */
static void
flush(struct xpace *p, struct psw *s)
{
   pthread_mutex_lock(&p->m);
   while (!p->quit && (s ? s->pending > 0 : p->head < p->tail))
      pthread_cond_wait(&p->cv, &p->m);
   pthread_mutex_unlock(&p->m);
}

/* ---- what the ready times show: bursts of n images, T apart ---- */
static int
cmp_d(const void *a, const void *b)
{
   double x = *(const double *)a, y = *(const double *)b;
   return x < y ? -1 : x > y;
}

static void
classify(struct xpace *p)
{
   if (p->nhist < NHIST) {
      p->active = false;
      return;
   }
   uint64_t r[NHIST];
   for (unsigned i = 0; i < NHIST; i++)
      r[i] = p->hist[(p->hpos + i) % NHIST];
   const double m = (double)(r[NHIST - 1] - r[0]) / (NHIST - 1) / 1e6;
   int ambiguous = 0, sizes[NHIST], nb = 0, cur = 1;
   double starts[NHIST];
   int ns = 0;
   for (unsigned i = 1; i < NHIST; i++) {
      const double g = (double)(r[i] - r[i - 1]) / 1e6;
      if (g > 1.5 * m) {
         if (ns)   /* a complete burst: the one since the last big gap */
            sizes[nb++] = cur;
         starts[ns++] = (double)r[i] / 1e6;
         cur = 1;
      } else {
         if (g > 0.5 * m)
            ambiguous++;
         cur++;
      }
   }
   int best = 0, bestc = 0;
   for (int a = 0; a < nb; a++) {
      int c = 0;
      for (int b = 0; b < nb; b++)
         c += sizes[b] == sizes[a];
      if (c > bestc)
         bestc = c, best = sizes[a];
   }
   double iv[NHIST];
   int ni = 0;
   for (int i = 1; i < ns; i++)
      iv[ni++] = starts[i] - starts[i - 1];
   qsort(iv, ni, sizeof(double), cmp_d);
   double T = ni ? iv[ni / 2] : 0, spread = ni >= 4 ? iv[ni * 3 / 4] / iv[ni / 4] : 9;
   p->ambiguous = ambiguous;
   p->bursts = nb;
   const bool ok = m > 0.5 && nb >= 4 && best >= 2 && best <= 6 && bestc * 4 >= nb * 3 && ambiguous <= 3 &&
                   spread <= 1.25 && T >= 8.0 && T <= 100.0;
   if (ok) {
      /* the interval of the bursts: smoothed, the burst size and the steadiness as of now */
      p->T = p->n == best && p->T > 0 ? 0.9 * p->T + 0.1 * T : T;
      p->n = best;
      p->Tspread = spread;
      p->good++;
   } else {
      p->good = 0;
   }
   const bool was = p->active;
   p->active = p->mode == 1 && (p->active ? ok : p->good >= 8);   /* on after 8 good checks in a row, off at once */
   if (was != p->active)
      plog(p, "pacing %s: n %d, T %.2f ms (spread %.2f), %d bursts, %d ambiguous gaps", p->active ? "ON" : "off",
           best, T, spread, nb, ambiguous);
}

/* ---- the watcher: the ready time of every image ---- */
static void *
watcher_main(void *arg)
{
   struct xpace *p = arg;
   pthread_mutex_lock(&p->m);
   for (;;) {
      while (!p->quit && p->watch == p->tail)
         pthread_cond_wait(&p->cv, &p->m);
      if (p->quit)
         break;
      struct pjob *j = &p->jobs[p->watch % NJOB];
      if (j->nowait) {
         j->watched = true;
         p->watch++;
         pthread_cond_broadcast(&p->cv);
         continue;
      }
      VkFence f = j->s->fence[j->idx];
      pthread_mutex_unlock(&p->m);
      VkResult r = p->WaitForFences(p->dev, 1, &f, VK_TRUE, 2000 * MS);
      const uint64_t t = now_ns();
      if (r == VK_SUCCESS)
         p->ResetFences(p->dev, 1, &f);
      pthread_mutex_lock(&p->m);
      if (r != VK_SUCCESS)
         plog(p, "fence wait failed: %d", r);
      j->ready = t;
      j->watched = true;
      j->s->busy[j->idx] = r != VK_SUCCESS;   /* a fence that never signalled is not used again */
      if (p->trace && p->mode == 2)
         plog(p, "R %u ready %.3f", j->idx, (t - p->t0) / 1e6);
      p->hist[p->hpos] = t;
      p->hpos = (p->hpos + 1) % NHIST;
      if (p->nhist < NHIST)
         p->nhist++;
      classify(p);
      p->watch++;
      pthread_cond_broadcast(&p->cv);
   }
   pthread_mutex_unlock(&p->m);
   return NULL;
}

static void
log_period(struct xpace *p, uint64_t t)
{
   if (!p->log || t - p->logt < 2000 * MS)
      return;
   const double s = (t - p->logt) / 1e9;
   plog(p, "%.1f presents/s (%u paced, %u held, %u holds cut): n %d T %.2f ms spread %.2f | hold mean %.2f max %.2f "
        "ms | late max %.2f | present call max %.2f, lock wait max %.2f, submit max %.2f | acquire blocked %.1f ms/s "
        "(max %.2f) | ready-present mean %.2f ms", p->l_pres / s, p->l_active, p->l_held, p->l_cut, p->n, p->T,
        p->Tspread, p->l_held ? p->l_hold / p->l_held : 0, p->l_hold_max, p->l_late_max, p->l_call_max, p->l_lockw_max,
        p->l_sub_max, p->l_acq / s, p->l_acq_max, p->l_pres ? p->l_lat / p->l_pres : 0);
   fflush(p->log);
   p->logt = t;
   p->l_pres = p->l_active = p->l_held = p->l_cut = 0;
   p->l_hold = p->l_hold_max = p->l_late_max = p->l_call_max = p->l_lockw_max = p->l_acq = p->l_acq_max = 0;
   p->l_lat = p->l_sub_max = 0;
}

/* ---- the presenter ---- */
static VkResult
present_job(struct xpace *p, struct pjob *j, double *call_ms, double *lockw_ms)
{
   VkPresentIdKHR pid = { .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &j->id };
   VkSwapchainPresentFenceInfoKHR pf = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
                                         .swapchainCount = 1, .pFences = &j->pfence };
   VkSwapchainPresentModeInfoKHR pm = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR,
                                        .swapchainCount = 1, .pPresentModes = &j->mode };
   const void *next = NULL;
   if (j->has_id)
      pid.pNext = next, next = &pid;
   if (j->has_pfence)
      pf.pNext = next, next = &pf;
   if (j->has_mode)
      pm.pNext = next, next = &pm;
   VkResult res = VK_SUCCESS;
   VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = next, .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &j->s->sem[j->idx], .swapchainCount = 1, .pSwapchains = &j->s->h,
                           .pImageIndices = &j->idx, .pResults = &res };
   const uint64_t t0 = now_ns();
   __atomic_add_fetch(&j->s->want, 1, __ATOMIC_SEQ_CST);
   pthread_mutex_lock(&j->s->m);
   __atomic_sub_fetch(&j->s->want, 1, __ATOMIC_SEQ_CST);
   QLOCK(j->q);
   const uint64_t t1 = now_ns();
   VkResult r = p->QueuePresentKHR(j->qh, &pi);
   QUNLOCK(j->q);
   pthread_mutex_unlock(&j->s->m);
   *lockw_ms = (t1 - t0) / 1e6;
   *call_ms = (now_ns() - t1) / 1e6;
   return r == VK_SUCCESS ? res : r;
}

static void *
presenter_main(void *arg)
{
   struct xpace *p = arg;
   pthread_mutex_lock(&p->m);
   for (;;) {
      while (!p->quit && p->head == p->tail)
         pthread_cond_wait(&p->cv, &p->m);
      if (p->quit)
         break;
      struct pjob *j = &p->jobs[p->head % NJOB];
      uint64_t target = 0, ready = 0;
      bool paced = false, start = false, cut = false;
      if (p->active) {
         /* wait for the image to be ready (bounded), then space it from the last present */
         struct timespec ts;
         abs_ts(&ts, j->call + 100 * MS);
         while (!p->quit && !j->watched && pthread_cond_timedwait(&p->cv, &p->m, &ts) != ETIMEDOUT)
            ;
         if (j->watched && p->active) {
            paced = true;
            ready = j->ready;
            const double per = p->T / p->n;
            const uint64_t D = (uint64_t)(p->K * per * 1e6);
            start = !p->prev_ready || (double)(ready - p->prev_ready) / 1e6 > 0.5 * per;
            p->k = start ? 0 : p->k + 1;
            target = p->last_out + D > ready ? p->last_out + D : ready;
            const double hold = (double)(target - ready) / 1e6;
            /* the first image of a burst waits little; the k-th about k spacings; never beyond 40 ms */
            const double bound = start ? 0.3 * per : p->k * per + 2.0;
            if (hold > bound || hold > 40.0)
               target = ready, cut = true;
            p->prev_ready = ready;
         }
      }
      if (!paced)
         p->prev_ready = j->watched ? j->ready : 0;
      pthread_mutex_unlock(&p->m);
      if (target)
         sleep_until(target);
      const uint64_t out = now_ns();
      double call_ms, lockw_ms;
      VkResult r = present_job(p, j, &call_ms, &lockw_ms);
      pthread_mutex_lock(&p->m);
      p->last_out = out;
      if (r != VK_SUCCESS && !j->s->sticky) {
         j->s->sticky = r;
         plog(p, "deferred present: %d (presents of this swap chain pass through from now on)", r);
      }
      p->l_pres++;
      if (paced) {
         p->l_active++;
         const double hold = (double)(out - ready) / 1e6, late = target ? (double)(out - target) / 1e6 : 0;
         if (target > ready) {
            p->l_held++;
            p->l_hold += hold;
         }
         if (hold > p->l_hold_max)
            p->l_hold_max = hold;
         if (late > p->l_late_max)
            p->l_late_max = late;
         p->l_cut += cut;
      }
      if (call_ms > p->l_call_max)
         p->l_call_max = call_ms;
      if (lockw_ms > p->l_lockw_max)
         p->l_lockw_max = lockw_ms;
      if (j->watched)
         p->l_lat += (double)(out > j->ready ? out - j->ready : 0) / 1e6;
      if (p->trace)
         plog(p, "P %u call %.3f ready %.3f out %.3f%s%s k %d", j->idx, (j->call - p->t0) / 1e6,
              j->watched ? (j->ready - p->t0) / 1e6 : -1.0, (out - p->t0) / 1e6, paced ? " paced" : "",
              start ? " start" : "", p->k);
      log_period(p, out);
      j->s->pending--;
      p->head++;
      pthread_cond_broadcast(&p->cv);
   }
   pthread_mutex_unlock(&p->m);
   return NULL;
}

/* ---- vkQueuePresentKHR ---- */
static VkResult
present_through(struct xpace *p, VkQueue queue, const VkPresentInfoKHR *pi)
{
   struct psw *ss[MAXSW];
   uint32_t n = 0;
   flush(p, NULL);
   for (uint32_t i = 0; i < pi->swapchainCount && n < MAXSW; i++) {
      struct psw *s = psw_of(p, pi->pSwapchains[i]);
      if (s)
         ss[n++] = s;
   }
   struct pq *q = pq_of(p, queue);
   for (uint32_t i = 0; i < n; i++)
      pthread_mutex_lock(&ss[i]->m);
   QLOCK(q);
   VkResult r = p->QueuePresentKHR(queue, pi);
   QUNLOCK(q);
   for (uint32_t i = n; i-- > 0;)
      pthread_mutex_unlock(&ss[i]->m);
   return r;
}

VkResult
xpace_present(struct xpace *p, VkQueue queue, const VkPresentInfoKHR *pi)
{
   const uint64_t call = now_ns();
   struct psw *s = pi->swapchainCount == 1 ? psw_of(p, pi->pSwapchains[0]) : NULL;
   struct pjob j = { .s = s, .call = call };
   bool unknown = !s || s->sticky || pi->waitSemaphoreCount > 16 || pi->pImageIndices[0] >= s->n;
   for (const VkBaseInStructure *e = pi->pNext; e && !unknown; e = e->pNext) {
      switch (e->sType) {
      case VK_STRUCTURE_TYPE_PRESENT_ID_KHR: {
         const VkPresentIdKHR *x = (const void *)e;
         if (x->pPresentIds)
            j.has_id = true, j.id = x->pPresentIds[0];
         break;
      }
      case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR: {
         const VkSwapchainPresentFenceInfoKHR *x = (const void *)e;
         if (x->pFences[0])
            j.has_pfence = true, j.pfence = x->pFences[0];
         break;
      }
      case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR: {
         const VkSwapchainPresentModeInfoKHR *x = (const void *)e;
         j.has_mode = true, j.mode = x->pPresentModes[0];
         break;
      }
      default:
         unknown = true;
      }
   }
   if (unknown) {
      static bool said;
      if (!said && s && !s->sticky) {
         said = true;
         plog(p, "a present the layer does not defer (%u swap chains, pNext %d): passed through",
              pi->swapchainCount, pi->pNext ? ((const VkBaseInStructure *)pi->pNext)->sType : 0);
      }
      return present_through(p, queue, pi);
   }
   const uint32_t idx = pi->pImageIndices[0];
   j.idx = idx;
   j.q = pq_of(p, queue);
   j.qh = queue;
   pthread_mutex_lock(&p->m);
   /* the image's fence is free once the watcher saw its last use signalled (the image was presented and acquired
    * again since, so this is a formality); a full ring waits for the presenter */
   while (!p->quit && (s->busy[idx] || p->tail - (p->head < p->watch ? p->head : p->watch) >= NJOB))
      pthread_cond_wait(&p->cv, &p->m);
   pthread_mutex_unlock(&p->m);
   VkPipelineStageFlags stages[16];
   for (uint32_t i = 0; i < pi->waitSemaphoreCount; i++)
      stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = pi->waitSemaphoreCount,
                       .pWaitSemaphores = pi->pWaitSemaphores, .pWaitDstStageMask = stages, .signalSemaphoreCount = 1,
                       .pSignalSemaphores = &s->sem[idx] };
   const uint64_t ts = now_ns();
   VkResult r, rp = VK_SUCCESS;
   if (p->mode == 2)
      pthread_mutex_lock(&s->m);   /* the presenter's order: swap chain, then queue */
   QLOCK(j.q);
   r = p->QueueSubmit(queue, 1, &si, s->fence[idx]);
   const double sub_ms = (now_ns() - ts) / 1e6;
   if (r == VK_SUCCESS && p->mode == 2) {
      /* measuring only: the present stays here, after the layer's semaphore */
      VkPresentInfoKHR pi2 = *pi;
      pi2.waitSemaphoreCount = 1;
      pi2.pWaitSemaphores = &s->sem[idx];
      rp = p->QueuePresentKHR(queue, &pi2);
   }
   QUNLOCK(j.q);
   if (p->mode == 2)
      pthread_mutex_unlock(&s->m);
   if (r != VK_SUCCESS) {
      plog(p, "the layer's submission failed: %d", r);
      return r;
   }
   pthread_mutex_lock(&p->m);
   if (sub_ms > p->l_sub_max)
      p->l_sub_max = sub_ms;
   s->busy[idx] = true;
   p->jobs[p->tail % NJOB] = j;
   if (p->mode == 2) {
      /* the watcher still times it; there is nothing to present later */
      p->l_pres++;
      if (p->trace)
         plog(p, "C %u call %.3f", idx, (call - p->t0) / 1e6);
      log_period(p, now_ns());
   } else {
      s->pending++;
   }
   p->tail++;
   if (p->mode == 2)
      p->head = p->tail;
   pthread_cond_broadcast(&p->cv);
   pthread_mutex_unlock(&p->m);
   if (p->mode == 1 && pi->pResults)
      pi->pResults[0] = VK_SUCCESS;
   return rp;
}

/* ---- the queue commands: serialised with the presenter ---- */
#define QWRAP(name, params, args) \
   static VKAPI_ATTR VkResult VKAPI_CALL p_##name params \
   { \
      struct xpace *p = pace_of(queue); \
      struct pq *q = pq_of(p, queue); \
      QLOCK(q); \
      VkResult r = p->name args; \
      QUNLOCK(q); \
      return r; \
   }
QWRAP(QueueSubmit, (VkQueue queue, uint32_t n, const VkSubmitInfo *si, VkFence f), (queue, n, si, f))
QWRAP(QueueSubmit2, (VkQueue queue, uint32_t n, const VkSubmitInfo2 *si, VkFence f), (queue, n, si, f))
QWRAP(QueueSubmit2KHR, (VkQueue queue, uint32_t n, const VkSubmitInfo2 *si, VkFence f), (queue, n, si, f))
QWRAP(QueueBindSparse, (VkQueue queue, uint32_t n, const VkBindSparseInfo *bi, VkFence f), (queue, n, bi, f))
QWRAP(QueueWaitIdle, (VkQueue queue), (queue))

static VKAPI_ATTR VkResult VKAPI_CALL
p_DeviceWaitIdle(VkDevice device)
{
   struct xpace *p = pace_of(device);
   const int n = __atomic_load_n(&p->nq, __ATOMIC_ACQUIRE);
   for (int i = 0; i < n; i++)
      pthread_mutex_lock(&p->qs[i].m);
   VkResult r = p->DeviceWaitIdle(device);
   for (int i = n; i-- > 0;)
      pthread_mutex_unlock(&p->qs[i].m);
   return r;
}

/* ---- the swap chain commands ---- */
static VkResult
acquire(struct xpace *p, struct psw *s, uint64_t timeout, VkResult (*call)(void *, uint64_t), void *ctx)
{
   if (!s)
      return call(ctx, timeout);
   const uint64_t t0 = now_ns(), end = timeout > UINT64_MAX - t0 ? UINT64_MAX : t0 + timeout;
   VkResult r;
   for (;;) {
      const uint64_t t = now_ns();
      const uint64_t left = end > t ? end - t : 0;
      while (__atomic_load_n(&s->want, __ATOMIC_SEQ_CST))
         sleep_until(now_ns() + 20000);
      pthread_mutex_lock(&s->m);
      r = call(ctx, left < MS / 2 ? left : MS / 2);
      pthread_mutex_unlock(&s->m);
      if ((r != VK_TIMEOUT && r != VK_NOT_READY) || now_ns() >= end)
         break;
   }
   if (timeout && r == VK_NOT_READY)
      r = VK_TIMEOUT;
   const double ms = (now_ns() - t0) / 1e6;
   if (ms > 0.1) {
      pthread_mutex_lock(&p->m);
      p->l_acq += ms;
      if (ms > p->l_acq_max)
         p->l_acq_max = ms;
      pthread_mutex_unlock(&p->m);
   }
   return r;
}

struct acq1 { struct xpace *p; VkDevice d; VkSwapchainKHR sw; VkSemaphore sem; VkFence f; uint32_t *idx; };
static VkResult
acq1_call(void *c, uint64_t timeout)
{
   struct acq1 *a = c;
   return a->p->AcquireNextImageKHR(a->d, a->sw, timeout, a->sem, a->f, a->idx);
}
static VKAPI_ATTR VkResult VKAPI_CALL
p_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR sw, uint64_t timeout, VkSemaphore sem, VkFence f, uint32_t *idx)
{
   struct xpace *p = pace_of(device);
   struct acq1 a = { p, device, sw, sem, f, idx };
   return acquire(p, psw_of(p, sw), timeout, acq1_call, &a);
}

struct acq2 { struct xpace *p; VkDevice d; VkAcquireNextImageInfoKHR info; uint32_t *idx; };
static VkResult
acq2_call(void *c, uint64_t timeout)
{
   struct acq2 *a = c;
   a->info.timeout = timeout;
   return a->p->AcquireNextImage2KHR(a->d, &a->info, a->idx);
}
static VKAPI_ATTR VkResult VKAPI_CALL
p_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *info, uint32_t *idx)
{
   struct xpace *p = pace_of(device);
   struct acq2 a = { p, device, *info, idx };
   return acquire(p, psw_of(p, info->swapchain), info->timeout, acq2_call, &a);
}

static VKAPI_ATTR void VKAPI_CALL
p_SetHdrMetadataEXT(VkDevice device, uint32_t n, const VkSwapchainKHR *sws, const VkHdrMetadataEXT *md)
{
   struct xpace *p = pace_of(device);
   struct psw *s = n == 1 ? psw_of(p, sws[0]) : NULL;
   flush(p, s);   /* the metadata belongs to the presents after it */
   if (s)
      pthread_mutex_lock(&s->m);
   p->SetHdrMetadataEXT(device, n, sws, md);
   if (s)
      pthread_mutex_unlock(&s->m);
}

static VKAPI_ATTR VkResult VKAPI_CALL
p_ReleaseSwapchainImagesKHR(VkDevice device, const VkReleaseSwapchainImagesInfoKHR *ri)
{
   struct xpace *p = pace_of(device);
   struct psw *s = psw_of(p, ri->swapchain);
   PFN_vkReleaseSwapchainImagesKHR f = p->ReleaseSwapchainImagesKHR ? p->ReleaseSwapchainImagesKHR
                                                                    : p->ReleaseSwapchainImagesEXT;
   if (s)
      pthread_mutex_lock(&s->m);
   VkResult r = f(device, ri);
   if (s)
      pthread_mutex_unlock(&s->m);
   return r;
}

static void
psw_free(struct xpace *p, struct psw *s)
{
   for (uint32_t i = 0; i < s->n; i++) {
      if (s->sem[i])
         p->DestroySemaphore(p->dev, s->sem[i], NULL);
      if (s->fence[i])
         p->DestroyFence(p->dev, s->fence[i], NULL);
   }
   pthread_mutex_destroy(&s->m);
   memset(s, 0, sizeof(*s));
}

static VKAPI_ATTR VkResult VKAPI_CALL
p_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *alloc,
                     VkSwapchainKHR *out)
{
   struct xpace *p = pace_of(device);
   struct psw *old = psw_of(p, ci->oldSwapchain);
   if (old) {
      flush(p, old);
      pthread_mutex_lock(&old->m);
   }
   /* the presenter holds images until their time: more images, so that the game's acquire does not wait for them */
   VkSwapchainCreateInfoKHR ci2 = *ci;
   if (p->mode == 1)
      ci2.minImageCount += (uint32_t)p->extra_images;
   VkResult r = p->CreateSwapchainKHR(device, &ci2, alloc, out);
   if (r != VK_SUCCESS && ci2.minImageCount != ci->minImageCount)
      r = p->CreateSwapchainKHR(device, ci, alloc, out);
   if (old)
      pthread_mutex_unlock(&old->m);
   if (r != VK_SUCCESS)
      return r;
   uint32_t n = 0;
   if (p->GetSwapchainImagesKHR(device, *out, &n, NULL) < 0 || !n || n > MAXIMG) {
      plog(p, "swap chain %#llx: %u images, not paced", (unsigned long long)*out, n);
      return r;
   }
   pthread_mutex_lock(&p->m);
   struct psw *s = NULL;
   for (int i = 0; i < MAXSW && !s; i++)
      if (!p->sws[i].h)
         s = &p->sws[i];
   pthread_mutex_unlock(&p->m);
   if (!s) {
      plog(p, "swap chain %#llx: too many swap chains, not paced", (unsigned long long)*out);
      return r;
   }
   bool ok = true;
   for (uint32_t i = 0; i < n && ok; i++) {
      VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
      VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
      ok = p->CreateSemaphore(device, &sci, NULL, &s->sem[i]) == VK_SUCCESS &&
           p->CreateFence(device, &fci, NULL, &s->fence[i]) == VK_SUCCESS;
   }
   s->n = n;
   pthread_mutex_init(&s->m, NULL);
   if (!ok) {
      psw_free(p, s);
      plog(p, "swap chain %#llx: no semaphores, not paced", (unsigned long long)*out);
      return r;
   }
   pthread_mutex_lock(&p->m);
   s->h = *out;   /* from now on psw_of finds it */
   pthread_mutex_unlock(&p->m);
   plog(p, "swap chain %#llx: %u images (min %u), %ux%u, present mode %d, old %#llx", (unsigned long long)*out, n,
        ci->minImageCount, ci->imageExtent.width, ci->imageExtent.height, ci->presentMode,
        (unsigned long long)ci->oldSwapchain);
   return r;
}

static VKAPI_ATTR void VKAPI_CALL
p_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR sw, const VkAllocationCallbacks *alloc)
{
   struct xpace *p = pace_of(device);
   struct psw *s = psw_of(p, sw);
   if (!s) {
      p->DestroySwapchainKHR(device, sw, alloc);
      return;
   }
   flush(p, s);
   pthread_mutex_lock(&p->m);
   for (;;) {   /* the watcher is done with its fences */
      bool busy = false;
      for (uint32_t i = 0; i < s->n; i++)
         busy |= s->busy[i];
      if (!busy || p->quit || p->watch == p->tail)
         break;
      pthread_cond_wait(&p->cv, &p->m);
   }
   s->h = 0;
   pthread_mutex_unlock(&p->m);
   pthread_mutex_lock(&s->m);
   p->DestroySwapchainKHR(device, sw, alloc);
   pthread_mutex_unlock(&s->m);
   /* the presents may still wait for the semaphores on the GPU */
   p_DeviceWaitIdle(device);
   psw_free(p, s);
   plog(p, "swap chain %#llx destroyed", (unsigned long long)sw);
}

/* ---- device ---- */
struct xpace *
xpace_create(VkDevice device, PFN_vkGetDeviceProcAddr gdpa)
{
   const char *e = getenv("XMX_LAYER_PACE");
   if (!e || !*e || !strcmp(e, "0"))
      return NULL;
   struct xpace *p = calloc(1, sizeof(*p));
   if (!p)
      return NULL;
   p->mode = !strcmp(e, "log") ? 2 : 1;
   p->dev = device;
   p->key = (uint64_t)(uintptr_t)(*(void **)device);
#define X(n) p->n = (PFN_vk##n)gdpa(device, "vk" #n);
   PACE_FNS(X)
#undef X
   const char *k = getenv("XMX_LAYER_PACE_K");
   p->K = k ? atof(k) : 0.97;
   const char *xi = getenv("XMX_LAYER_PACE_IMAGES");
   p->extra_images = xi ? atoi(xi) : 2;
   if (p->extra_images < 0 || p->extra_images > 4)
      p->extra_images = 2;
   if (!(p->K > 0.5 && p->K <= 1.2))
      p->K = 0.97;
   p->t0 = p->logt = now_ns();
   const char *lf = getenv("XMX_LAYER_PACE_LOG");
   if (lf && *lf)
      p->log = fopen(lf, "a");
   p->trace = getenv("XMX_LAYER_PACE_TRACE") && !strcmp(getenv("XMX_LAYER_PACE_TRACE"), "1");
   if (!p->QueueSubmit || !p->QueuePresentKHR || !p->AcquireNextImageKHR || !p->CreateSwapchainKHR ||
       !p->DestroySwapchainKHR || !p->GetSwapchainImagesKHR || !p->WaitForFences || !p->ResetFences) {
      plog(p, "no swap chain on this device: no pacing");
      if (p->log)
         fclose(p->log);
      free(p);
      return NULL;
   }
   pthread_mutex_init(&p->m, NULL);
   pthread_condattr_t ca;
   pthread_condattr_init(&ca);
   pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
   pthread_cond_init(&p->cv, &ca);
   pthread_condattr_destroy(&ca);
   bool ok = pthread_create(&p->watcher, NULL, watcher_main, p) == 0;
   if (ok && p->mode == 1 && pthread_create(&p->presenter, NULL, presenter_main, p) != 0) {
      pthread_mutex_lock(&p->m);
      p->quit = true;
      pthread_cond_broadcast(&p->cv);
      pthread_mutex_unlock(&p->m);
      pthread_join(p->watcher, NULL);
      ok = false;
   }
   if (!ok) {
      plog(p, "no threads: no pacing");
      if (p->log)
         fclose(p->log);
      free(p);
      return NULL;
   }
   p->threads = true;
   pthread_setname_np(p->watcher, "xmx-pace-watch");
   if (p->mode == 1)
      pthread_setname_np(p->presenter, "xmx-pace");
   pthread_mutex_lock(&paces_m);
   int slot = -1;
   for (int i = 0; i < 4 && slot < 0; i++)
      if (!paces[i])
         slot = i;
   if (slot >= 0)
      __atomic_store_n(&paces[slot], p, __ATOMIC_RELEASE);
   pthread_mutex_unlock(&paces_m);
   if (slot < 0) {
      xpace_destroy(p);
      return NULL;
   }
   plog(p, "pacing %s (pid %d), K %.3f", p->mode == 1 ? "on" : "measuring only", (int)getpid(), p->K);
   return p;
}

void
xpace_destroy(struct xpace *p)
{
   if (!p)
      return;
   flush(p, NULL);
   pthread_mutex_lock(&p->m);
   p->quit = true;
   pthread_cond_broadcast(&p->cv);
   pthread_mutex_unlock(&p->m);
   if (p->threads) {
      pthread_join(p->watcher, NULL);
      if (p->mode == 1)
         pthread_join(p->presenter, NULL);
   }
   pthread_mutex_lock(&paces_m);
   for (int i = 0; i < 4; i++)
      if (paces[i] == p)
         paces[i] = NULL;
   pthread_mutex_unlock(&paces_m);
   if (p->DeviceWaitIdle)
      p->DeviceWaitIdle(p->dev);
   for (int i = 0; i < MAXSW; i++)
      if (p->sws[i].n)
         psw_free(p, &p->sws[i]);
   for (int i = 0; i < p->nq; i++)
      pthread_mutex_destroy(&p->qs[i].m);
   plog(p, "device gone");
   if (p->log)
      fclose(p->log);
   pthread_cond_destroy(&p->cv);
   pthread_mutex_destroy(&p->m);
   free(p);
}

PFN_vkVoidFunction
xpace_proc(VkDevice device, const char *name)
{
   if (!device || strncmp(name, "vk", 2))
      return NULL;
   struct xpace *p = pace_of(device);
   if (!p)
      return NULL;
#define P(n) if (!strcmp(name + 2, #n)) return p->n ? (PFN_vkVoidFunction)p_##n : NULL
   P(QueueSubmit); P(QueueSubmit2); P(QueueSubmit2KHR); P(QueueBindSparse); P(QueueWaitIdle); P(DeviceWaitIdle);
   P(AcquireNextImageKHR); P(AcquireNextImage2KHR); P(SetHdrMetadataEXT); P(CreateSwapchainKHR);
   P(DestroySwapchainKHR);
#undef P
   if (!strcmp(name, "vkReleaseSwapchainImagesKHR") || !strcmp(name, "vkReleaseSwapchainImagesEXT"))
      return p->ReleaseSwapchainImagesKHR || p->ReleaseSwapchainImagesEXT ? (PFN_vkVoidFunction)p_ReleaseSwapchainImagesKHR
                                                                         : NULL;
   return NULL;
}
