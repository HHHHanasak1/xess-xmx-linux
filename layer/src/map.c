/* map.c - see map.h. Key 0 = empty, key ~0 = removed (Vulkan handles are never either). */
#define _GNU_SOURCE
#include "map.h"

#include <stdlib.h>

#define EMPTY 0
#define GONE (~(uint64_t)0)

void
xmap_init(struct xmap *m)
{
   pthread_rwlock_init(&m->lock, NULL);
   m->cap = 64;
   m->keys = calloc(m->cap, sizeof(uint64_t));
   m->vals = calloc(m->cap, sizeof(void *));
   m->used = m->live = 0;
   m->init = 1;
}

static unsigned
slot(uint64_t k, unsigned cap)
{
   k ^= k >> 33;
   k *= 0xff51afd7ed558ccdull;
   k ^= k >> 33;
   return (unsigned)k & (cap - 1);
}

static void
grow(struct xmap *m)
{
   const unsigned oc = m->cap;
   uint64_t *ok = m->keys;
   void **ov = m->vals;
   m->cap = m->live * 4 > oc ? oc * 2 : oc;
   m->keys = calloc(m->cap, sizeof(uint64_t));
   m->vals = calloc(m->cap, sizeof(void *));
   m->used = m->live = 0;
   for (unsigned i = 0; i < oc; i++)
      if (ok[i] != EMPTY && ok[i] != GONE) {
         unsigned s = slot(ok[i], m->cap);
         while (m->keys[s] != EMPTY)
            s = (s + 1) & (m->cap - 1);
         m->keys[s] = ok[i];
         m->vals[s] = ov[i];
         m->used++;
         m->live++;
      }
   free(ok);
   free(ov);
}

void
xmap_put(struct xmap *m, uint64_t k, void *v)
{
   pthread_rwlock_wrlock(&m->lock);
   if ((m->used + 1) * 2 > m->cap)
      grow(m);
   unsigned s = slot(k, m->cap), first_gone = ~0u;
   while (m->keys[s] != EMPTY && m->keys[s] != k) {
      if (m->keys[s] == GONE && first_gone == ~0u)
         first_gone = s;
      s = (s + 1) & (m->cap - 1);
   }
   if (m->keys[s] == k) {
      m->vals[s] = v;
   } else {
      if (first_gone != ~0u)
         s = first_gone;
      else
         m->used++;
      m->keys[s] = k;
      m->vals[s] = v;
      m->live++;
   }
   pthread_rwlock_unlock(&m->lock);
}

static unsigned
find(struct xmap *m, uint64_t k)
{
   unsigned s = slot(k, m->cap);
   while (m->keys[s] != EMPTY) {
      if (m->keys[s] == k)
         return s;
      s = (s + 1) & (m->cap - 1);
   }
   return ~0u;
}

void *
xmap_get(struct xmap *m, uint64_t k)
{
   pthread_rwlock_rdlock(&m->lock);
   const unsigned s = find(m, k);
   void *v = s == ~0u ? NULL : m->vals[s];
   pthread_rwlock_unlock(&m->lock);
   return v;
}

void *
xmap_take(struct xmap *m, uint64_t k)
{
   pthread_rwlock_wrlock(&m->lock);
   const unsigned s = find(m, k);
   void *v = NULL;
   if (s != ~0u) {
      v = m->vals[s];
      m->keys[s] = GONE;
      m->vals[s] = NULL;
      m->live--;
   }
   pthread_rwlock_unlock(&m->lock);
   return v;
}

void
xmap_take_if(struct xmap *m, int (*pred)(void *v, void *user), void (*f)(void *v, void *user), void *user)
{
   pthread_rwlock_wrlock(&m->lock);
   for (unsigned i = 0; i < m->cap; i++)
      if (m->keys[i] != EMPTY && m->keys[i] != GONE && pred(m->vals[i], user)) {
         void *v = m->vals[i];
         m->keys[i] = GONE;
         m->vals[i] = NULL;
         m->live--;
         f(v, user);
      }
   pthread_rwlock_unlock(&m->lock);
}
