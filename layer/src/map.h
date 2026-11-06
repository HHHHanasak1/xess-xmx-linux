/* map.h - a small thread-safe map from a 64-bit handle to a pointer (open addressing, grows, tombstones). */
#pragma once
#include <stdint.h>
#include <pthread.h>

struct xmap {
   pthread_rwlock_t lock;   /* lookups share it, changes take it alone */
   uint64_t *keys;
   void **vals;
   unsigned cap, used, live;
   int init;
};

void xmap_init(struct xmap *m);
void xmap_put(struct xmap *m, uint64_t k, void *v);
void *xmap_get(struct xmap *m, uint64_t k);
void *xmap_take(struct xmap *m, uint64_t k);   /* removes and returns */

/* calls f(value, user) for every entry whose value satisfies pred, removing those entries (under the lock) */
void xmap_take_if(struct xmap *m, int (*pred)(void *v, void *user), void (*f)(void *v, void *user), void *user);
