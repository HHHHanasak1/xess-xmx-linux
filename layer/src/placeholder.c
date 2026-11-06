/* placeholder.c - see placeholder.h */
#include "placeholder.h"

#include <stdio.h>
#include <string.h>

#define SPV_MAGIC 0x07230203u
enum {
   OP_EXECUTION_MODE = 16,
   OP_TYPE_INT = 21,
   OP_TYPE_FLOAT = 22,
   OP_CONSTANT = 43,
   OP_SPEC_CONSTANT = 50,
   OP_EXECUTION_MODE_ID = 331,
   MODE_LOCAL_SIZE = 17,
   MODE_LOCAL_SIZE_ID = 38,
   OP_DECORATE = 71,
   DEC_BUILTIN = 11,
   BUILTIN_WORKGROUP_SIZE = 25,
   OP_CONSTANT_COMPOSITE = 44,
   OP_SPEC_CONSTANT_COMPOSITE = 51,
};

static const uint32_t dyn_planes[] = { 7, 11, 13 };

int
xmx_dyn_id(uint32_t wx, uint32_t wy, uint32_t wz)
{
   int base = 0;
   for (unsigned p = 0; p < sizeof(dyn_planes) / sizeof(dyn_planes[0]); p++) {
      const uint32_t z = dyn_planes[p], lim = 1024 / z;
      if (wz == z) {
         if (wx < 1 || wy < 1 || wx * wy > lim)
            return -1;
         int id = base;
         for (uint32_t y = 1; y < wy; y++)
            id += lim / y;
         return id + (int)(wx - 1);
      }
      for (uint32_t y = 1; y <= lim; y++)
         base += lim / y;
   }
   return -1;
}

/* Scalar constant table: id -> (width, value) for ids below this bound, enough for the small placeholders. Larger
 * modules are real shaders; their constants are still collected, but LocalSizeId lookups beyond the bound fail. */
#define ID_BOUND 4096

bool
xmx_spv_scan(const uint32_t *w, size_t n, struct xmx_spv_info *out)
{
   memset(out, 0, sizeof(*out));
   if (n < 5 || w[0] != SPV_MAGIC)
      return false;
   static _Thread_local uint8_t width_of_type[ID_BOUND];   /* 32/64 for int/float type ids, else 0 */
   static _Thread_local uint32_t scalar[ID_BOUND];         /* low word of 32-bit scalar constants */
   static _Thread_local uint8_t is_scalar[ID_BOUND];
   memset(width_of_type, 0, sizeof(width_of_type));
   memset(is_scalar, 0, sizeof(is_scalar));
   uint32_t size_ids[3] = { 0 }, wg_builtin = 0;
   bool have_size_ids = false;
   for (size_t i = 5; i < n;) {
      const uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
      if (wc == 0 || i + wc > n)
         return false;
      const uint32_t *a = &w[i + 1];
      switch (op) {
      case OP_EXECUTION_MODE:
         if (wc >= 6 && a[1] == MODE_LOCAL_SIZE) {
            out->wg[0] = a[2]; out->wg[1] = a[3]; out->wg[2] = a[4];
         }
         break;
      case OP_EXECUTION_MODE_ID:
         if (wc >= 6 && a[1] == MODE_LOCAL_SIZE_ID) {
            size_ids[0] = a[2]; size_ids[1] = a[3]; size_ids[2] = a[4];
            have_size_ids = true;
         }
         break;
      case OP_DECORATE:
         if (wc >= 4 && a[1] == DEC_BUILTIN && a[2] == BUILTIN_WORKGROUP_SIZE)
            wg_builtin = a[0];
         break;
      case OP_TYPE_INT:
      case OP_TYPE_FLOAT:
         if (a[0] < ID_BOUND && (a[1] == 32 || a[1] == 64))
            width_of_type[a[0]] = (uint8_t)a[1];
         break;
      case OP_CONSTANT:
      case OP_SPEC_CONSTANT: {
         const uint32_t type = a[0], id = a[1];
         const unsigned width = type < ID_BOUND ? width_of_type[type] : 0;
         if (width == 32 && wc >= 4) {
            if (id < ID_BOUND) { scalar[id] = a[2]; is_scalar[id] = 1; }
            if (out->nconsts < XMX_MAX_CONSTS)
               out->consts[out->nconsts++] = a[2];
            out->has_magic |= a[2] == XMX_MAGIC;
         } else if (width == 64 && wc >= 5) {
            if (out->nconsts + 1 < XMX_MAX_CONSTS) {
               out->consts[out->nconsts++] = a[2];
               out->consts[out->nconsts++] = a[3];
            }
            out->has_magic |= a[2] == XMX_MAGIC || a[3] == XMX_MAGIC;
         }
         break;
      }
      case OP_CONSTANT_COMPOSITE:
      case OP_SPEC_CONSTANT_COMPOSITE:
         /* the WorkgroupSize builtin (older modules): components are scalar constant ids */
         if (wg_builtin && a[1] == wg_builtin && wc >= 6) {
            for (int c = 0; c < 3; c++)
               if (a[2 + c] < ID_BOUND && is_scalar[a[2 + c]])
                  out->wg[c] = scalar[a[2 + c]];
         }
         break;
      default:
         break;
      }
      i += wc;
   }
   if (have_size_ids)
      for (int c = 0; c < 3; c++)
         if (size_ids[c] < ID_BOUND && is_scalar[size_ids[c]])
            out->wg[c] = scalar[size_ids[c]];
   return true;
}

bool
xmx_map_lookup(const char *dir, int id, char hash[17])
{
   if (!dir)
      return false;
   char path[600];
   snprintf(path, sizeof(path), "%s/map.txt", dir);
   FILE *f = fopen(path, "r");
   if (!f)
      return false;
   char line[128], h[32];
   int i;
   bool found = false;
   while (fgets(line, sizeof(line), f))
      if (sscanf(line, "%d %31s", &i, h) == 2 && i == id && strlen(h) == 16) {
         memcpy(hash, h, 17);
         found = true;   /* keep reading: the last line for an id wins */
      }
   fclose(f);
   return found;
}

/* The hash halves follow the magic, but which constants hold them depends on how the store came through (one vector,
 * or one scalar store per dword with offsets in between). Same order as the driver: the two values after the magic,
 * then every pair after it, then every pair; the first hash with a known kernel wins. */
static bool
pick_hash(const struct xmx_spv_info *s, xmx_hash_known_fn known, void *user, char hash[17])
{
   unsigned m = 0;
   while (m < s->nconsts && s->consts[m] != XMX_MAGIC)
      m++;
   for (unsigned pass = 0; pass < 3; pass++) {
      const unsigned from = pass < 2 ? m + 1 : 0;
      for (unsigned i = from; i < s->nconsts; i++)
         for (unsigned j = from; j < s->nconsts; j++) {
            if (pass == 0 && (i != m + 1 || j != m + 2))
               continue;
            if (s->consts[i] == XMX_MAGIC || s->consts[j] == XMX_MAGIC)
               continue;
            snprintf(hash, 17, "%08x%08x", s->consts[j], s->consts[i]);
            if (known && known(hash, user))
               return true;
         }
   }
   hash[0] = 0;
   return false;
}

void
xmx_classify(const struct xmx_spv_info *s, const char *dir, xmx_hash_known_fn known, void *user,
             struct xmx_placeholder *out)
{
   memset(out, 0, sizeof(*out));
   out->dyn_id = -1;
   const uint32_t wx = s->wg[0], wy = s->wg[1], wz = s->wg[2];
   if (!s->has_magic)
      return;
   if (wx == 1 && wy == 1 && wz == XMX_CANARY_WG_Z) {
      bool lo = false, hi = false;
      for (unsigned i = 0; i < s->nconsts; i++) {
         lo |= s->consts[i] == XMX_CANARY_LO;
         hi |= s->consts[i] == XMX_CANARY_HI;
      }
      out->kind = XMX_PH_CANARY;
      out->canary_ok = lo && hi;
      return;
   }
   if (wx == 1 && wy == 1 && wz == XMX_LEGACY_CANARY_WG_Z) {
      out->kind = XMX_PH_LEGACY_CANARY;
      out->canary_ok = true;
      return;
   }
   if (wx == 1 && wy == 1 && wz == XMX_HASH_WG_Z) {
      if (pick_hash(s, known, user, out->hash))
         out->kind = XMX_PH_KERNEL;
      return;   /* no known kernel: left to the driver, which compiles the placeholder as it is */
   }
   const int id = xmx_dyn_id(wx, wy, wz);
   if (id >= 0 && xmx_map_lookup(dir, id, out->hash)) {
      out->kind = XMX_PH_LEGACY_KERNEL;
      out->dyn_id = id;
   }
}
