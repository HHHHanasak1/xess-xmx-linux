/* cmk.c - the .cmk packer of the patched driver (patches/0001, anv_cm.c), for the layer: zebin -> CMK2 image.
 * Unchanged apart from the names and one addition: the offset of the group_count argument in the cross-thread data is
 * returned, because on an unpatched driver nothing rewrites it per dispatch (the layer's prologue loads it from the
 * inline data instead). tests/test_cmk.c checks that the image is byte-identical to tools/cmk_pack.py's. */
#include "cmk.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XMX_MIN(a, b) ((a) < (b) ? (a) : (b))
#define XMX_MAX(a, b) ((a) > (b) ? (a) : (b))
#define XMX_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* ---- .cmk packer ------------------------------------------------------------------------------------------------
 * A C port of tools/cmk_pack.py as tools/cm-compile.sh calls it (kernel "auto", --heap auto --surf auto, group count
 * 1,1,1): one kernel of the zebin ocloc wrote is packed into the CMK2 file xmx_kernel_replace() reads. The
 * .ze_info YAML is not parsed but matched the way the script's regular expressions match it (Python's \s, \d and \w
 * on the section decoded as Latin-1), and every input the script rejects is rejected here, so that both write the
 * same bytes. Packing in the driver saves a python3 start per kernel. */
struct xmx_sv {
   const char *p;
   size_t n;
};

#define XMX_NPOS SIZE_MAX

static bool
xmx_py_space(uint8_t c)
{
   return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20) || c == 0x85 || c == 0xa0;
}

static bool
xmx_py_word(uint8_t c)   /* str.isalnum() or '_' */
{
   return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
          c == 0xaa || c == 0xb2 || c == 0xb3 || c == 0xb5 || c == 0xb9 || c == 0xba || (c >= 0xbc && c <= 0xbe) ||
          (c >= 0xc0 && c != 0xd7 && c != 0xf7);
}

static size_t
xmx_sv_find(struct xmx_sv s, size_t from, const char *sub)
{
   const size_t k = strlen(sub);
   for (size_t i = from; i <= s.n && s.n - i >= k; i++)
      if (memcmp(s.p + i, sub, k) == 0)
         return i;
   return XMX_NPOS;
}

/* str.split(sep)[1], if sep occurs */
static bool
xmx_sv_split1(struct xmx_sv s, const char *sep, struct xmx_sv *out)
{
   const size_t a = xmx_sv_find(s, 0, sep);
   if (a == XMX_NPOS)
      return false;
   const size_t from = a + strlen(sep), b = xmx_sv_find(s, from, sep);
   *out = (struct xmx_sv) { s.p + from, (b == XMX_NPOS ? s.n : b) - from };
   return true;
}

/* str.split(sep)[0] */
static struct xmx_sv
xmx_sv_split0(struct xmx_sv s, const char *sep)
{
   const size_t a = xmx_sv_find(s, 0, sep);
   return (struct xmx_sv) { s.p, a == XMX_NPOS ? s.n : a };
}

/* the pattern pieces \s+, a literal, (\d+) and (\w+) at *i; *i moves past them on a match */
static bool
xmx_re_ws(struct xmx_sv s, size_t *i)
{
   size_t j = *i;
   while (j < s.n && xmx_py_space(s.p[j]))
      j++;
   if (j == *i)
      return false;
   *i = j;
   return true;
}

static bool
xmx_re_lit(struct xmx_sv s, size_t *i, const char *lit)
{
   const size_t k = strlen(lit);
   if (*i > s.n || s.n - *i < k || memcmp(s.p + *i, lit, k) != 0)
      return false;
   *i += k;
   return true;
}

static bool
xmx_re_num(struct xmx_sv s, size_t *i, uint64_t *v)   /* saturates; such values fail later, as in Python */
{
   size_t j = *i;
   uint64_t x = 0;
   while (j < s.n && s.p[j] >= '0' && s.p[j] <= '9') {
      const unsigned d = s.p[j] - '0';
      x = x > (UINT64_MAX - d) / 10 ? UINT64_MAX : x * 10 + d;
      j++;
   }
   if (j == *i)
      return false;
   *i = j;
   *v = x;
   return true;
}

static bool
xmx_re_word(struct xmx_sv s, size_t *i, struct xmx_sv *w)
{
   size_t j = *i;
   while (j < s.n && xmx_py_word(s.p[j]))
      j++;
   if (j == *i)
      return false;
   *w = (struct xmx_sv) { s.p + *i, j - *i };
   *i = j;
   return true;
}

static bool
xmx_sv_eq(struct xmx_sv s, const char *lit)
{
   return s.n == strlen(lit) && memcmp(s.p, lit, s.n) == 0;
}

/* re.search(r"\n\s+" + key + r":\s+(\d+)", s) */
static bool
xmx_re_geti(struct xmx_sv s, const char *key, uint64_t *v)
{
   for (size_t i = 0; i < s.n; i++) {
      size_t j = i + 1;
      if (s.p[i] == '\n' && xmx_re_ws(s, &j) && xmx_re_lit(s, &j, key) && xmx_re_lit(s, &j, ":") &&
          xmx_re_ws(s, &j) && xmx_re_num(s, &j, v))
         return true;
   }
   return false;
}

/* re.finditer(r"- arg_type:\s+(\w+)\s+offset:\s+(\d+)\s+size:\s+(\d+)", s), one match per call from *pos */
static bool
xmx_re_next_arg(struct xmx_sv s, size_t *pos, struct xmx_sv *type, uint64_t *off, uint64_t *size)
{
   for (size_t i = xmx_sv_find(s, *pos, "- arg_type:"); i != XMX_NPOS; i = xmx_sv_find(s, i + 1, "- arg_type:")) {
      size_t j = i + strlen("- arg_type:");
      if (xmx_re_ws(s, &j) && xmx_re_word(s, &j, type) && xmx_re_ws(s, &j) &&
          xmx_re_lit(s, &j, "offset:") && xmx_re_ws(s, &j) && xmx_re_num(s, &j, off) && xmx_re_ws(s, &j) &&
          xmx_re_lit(s, &j, "size:") && xmx_re_ws(s, &j) && xmx_re_num(s, &j, size)) {
         *pos = j;
         return true;
      }
   }
   return false;
}

/* re.finditer(r"bti_value:\s+(\d+)\s+arg_index:\s+(\d+)", s), one match per call from *pos */
static bool
xmx_re_next_bti(struct xmx_sv s, size_t *pos, uint64_t *bti, uint64_t *arg)
{
   for (size_t i = xmx_sv_find(s, *pos, "bti_value:"); i != XMX_NPOS; i = xmx_sv_find(s, i + 1, "bti_value:")) {
      size_t j = i + strlen("bti_value:");
      if (xmx_re_ws(s, &j) && xmx_re_num(s, &j, bti) && xmx_re_ws(s, &j) &&
          xmx_re_lit(s, &j, "arg_index:") && xmx_re_ws(s, &j) && xmx_re_num(s, &j, arg)) {
         *pos = j;
         return true;
      }
   }
   return false;
}

/* bytes.decode() (strict UTF-8) succeeds */
static bool
xmx_utf8_ok(const uint8_t *s, size_t n)
{
   for (size_t i = 0; i < n;) {
      const uint8_t c = s[i];
      unsigned len, cp;
      if (c < 0x80) {
         i++;
         continue;
      } else if (c >= 0xc2 && c <= 0xdf) {
         len = 2, cp = c & 0x1f;
      } else if (c >= 0xe0 && c <= 0xef) {
         len = 3, cp = c & 0x0f;
      } else if (c >= 0xf0 && c <= 0xf4) {
         len = 4, cp = c & 0x07;
      } else {
         return false;
      }
      if (n - i < len)
         return false;
      for (unsigned k = 1; k < len; k++) {
         if ((s[i + k] & 0xc0) != 0x80)
            return false;
         cp = cp << 6 | (s[i + k] & 0x3f);
      }
      if ((len == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff)))
         return false;
      i += len;
   }
   return true;
}

static uint64_t
xmx_rd_le(const uint8_t *p, unsigned bytes)
{
   uint64_t v = 0;
   for (unsigned i = 0; i < bytes; i++)
      v |= (uint64_t)p[i] << (8 * i);
   return v;
}

static void
xmx_wr32(uint8_t *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; i++)
      p[i] = v >> (8 * i);
}

/* d[off:off + size] with Python's clamping */
static struct xmx_sv
xmx_slice(const uint8_t *d, size_t n, uint64_t off, uint64_t size)
{
   const uint64_t a = XMX_MIN(off, (uint64_t)n);
   const uint64_t b = size > n - a ? n : a + size;
   return (struct xmx_sv) { (const char *)d + a, b - a };
}

#define XMX_HEADER_SIZE 472

/* Packs zebin d (n bytes) for the work-group size lws into a malloc'd CMK2 image; NULL and a reason on failure. */
uint8_t *
xmx_pack(const uint8_t *d, size_t n, const uint64_t lws[3], uint32_t grf_bytes, size_t *out_size,
         uint32_t *gc_off, char *err, size_t errlen)
{
   *gc_off = XMX_NO_GC;
#define PACK_FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); goto fail; } while (0)
   char *s = NULL;
   uint8_t *out = NULL;
   /* the ELF section headers */
   if (n < 4 || memcmp(d, "\x7f" "ELF", 4) != 0)
      PACK_FAIL("not an ELF file");
   if (n < 0x40)
      PACK_FAIL("truncated ELF header");
   const uint64_t shoff = xmx_rd_le(d + 0x28, 8);
   const unsigned shentsize = xmx_rd_le(d + 0x3a, 2), shnum = xmx_rd_le(d + 0x3c, 2),
                  shstrndx = xmx_rd_le(d + 0x3e, 2);
   if (shstrndx >= shnum)
      PACK_FAIL("no section name table");
   for (unsigned k = 0; k < shnum; k++) {
      const uint64_t o = shoff + (uint64_t)k * shentsize;
      if (o < shoff || o > n || n - o < 40)
         PACK_FAIL("section header %u out of the file", k);
   }
#define SEC(k, field) xmx_rd_le(d + shoff + (uint64_t)(k) * shentsize + (field), (field) < 8 ? 4 : 8)
   const uint64_t stroff = SEC(shstrndx, 24);
   /* every section name must be readable (the script decodes them all); the kernel is the first .text.* that is
    * not .text.Intel_*, its code and .ze_info the last section of that name */
   int text = -1, zeinfo = -1;
   struct xmx_sv kname = { NULL, 0 };
   for (unsigned k = 0; k < shnum; k++) {
      const uint64_t no = stroff + SEC(k, 0);
      if (no < stroff || no >= n)
         PACK_FAIL("section name %u out of the file", k);
      const uint8_t *nm = d + no, *end = memchr(nm, 0, n - no);
      if (!end)
         PACK_FAIL("section name %u not terminated", k);
      const size_t len = end - nm;
      if (!xmx_utf8_ok(nm, len))
         PACK_FAIL("section name %u is not UTF-8", k);
      if (!kname.p && len >= 6 && memcmp(nm, ".text.", 6) == 0 && !(len >= 12 && memcmp(nm, ".text.Intel_", 12) == 0))
         kname = (struct xmx_sv) { (const char *)nm + 6, len - 6 };
      if (len == 8 && memcmp(nm, ".ze_info", 8) == 0)
         zeinfo = k;
   }
   if (!kname.p)
      PACK_FAIL("no kernel in the zebin");
   for (unsigned k = 0; k < shnum; k++) {
      const uint8_t *nm = d + stroff + SEC(k, 0);
      if (strlen((const char *)nm) == kname.n + 6 && memcmp(nm, ".text.", 6) == 0 && memcmp(nm + 6, kname.p, kname.n) == 0)
         text = k;
   }
   if (zeinfo < 0)
      PACK_FAIL("no .ze_info");
   for (size_t i = 0; i < kname.n; i++)
      if ((uint8_t)kname.p[i] >= 0x80)
         PACK_FAIL("kernel name is not ASCII");   /* the script's Latin-1/UTF-8 comparison could never match it */
   const struct xmx_sv code = xmx_slice(d, n, SEC(text, 24), SEC(text, 32));
   const struct xmx_sv info = xmx_slice(d, n, SEC(zeinfo, 24), SEC(zeinfo, 32));
#undef SEC

   /* the kernel's block: re.split(r"\n  - name:\s+", "\n" + info[info.index("kernels:"):]), the first piece that
    * starts with the kernel's name */
   const size_t ki = xmx_sv_find(info, 0, "kernels:");
   if (ki == XMX_NPOS)
      PACK_FAIL("no kernels in .ze_info");
   s = malloc(info.n - ki + 1);
   if (!s)
      PACK_FAIL("out of memory");
   s[0] = '\n';
   memcpy(s + 1, info.p + ki, info.n - ki);
   const struct xmx_sv all = { s, info.n - ki + 1 };
   struct xmx_sv blk = { NULL, 0 };
   for (size_t from = 0;;) {
      size_t ms = XMX_NPOS, me = 0;
      for (size_t i = xmx_sv_find(all, from, "\n  - name:"); i != XMX_NPOS; i = xmx_sv_find(all, i + 1, "\n  - name:")) {
         size_t j = i + strlen("\n  - name:");
         if (xmx_re_ws(all, &j)) {
            ms = i, me = j;
            break;
         }
      }
      const struct xmx_sv piece = { all.p + from, (ms == XMX_NPOS ? all.n : ms) - from };
      if (piece.n >= kname.n && memcmp(piece.p, kname.p, kname.n) == 0) {
         blk = piece;
         break;
      }
      if (ms == XMX_NPOS)
         break;
      from = me;
   }
   if (!blk.p)
      PACK_FAIL("kernel %.*s not in .ze_info", (int)kname.n, kname.p);
   /* the driver gives kernels no scratch or private memory (as the script) */
   if (xmx_sv_find(blk, 0, "per_thread_memory_buffers") != XMX_NPOS)
      PACK_FAIL("kernel %.*s needs scratch or private memory, which the driver does not provide", (int)kname.n, kname.p);
   uint64_t grf = 128, slm = 0, barriers = 0;
   xmx_re_geti(blk, "grf_count", &grf);
   xmx_re_geti(blk, "slm_size", &slm);
   xmx_re_geti(blk, "barrier_count", &barriers);
   for (size_t i = xmx_sv_find(blk, 0, "addrspace:"); i != XMX_NPOS; i = xmx_sv_find(blk, i + 1, "addrspace:")) {
      size_t j = i + strlen("addrspace:");
      if (xmx_re_ws(blk, &j) && xmx_re_lit(blk, &j, "sampler")) {
         barriers |= 0x80000000u;
         break;
      }
   }
   for (unsigned k = 0; k < 3; k++)
      if (lws[k] > UINT32_MAX)
         PACK_FAIL("work-group size out of range");

   /* cross-thread data */
   uint8_t cross[128] = { 0 };
   uint64_t cross_size = 0;
   struct xmx_sv pay = { NULL, 0 }, typ;
   if (xmx_sv_split1(blk, "payload_arguments:", &pay))
      pay = xmx_sv_split0(pay, "per_thread_payload_arguments:");
   uint64_t off, size;
   for (size_t pos = 0; pay.p && xmx_re_next_arg(pay, &pos, &typ, &off, &size);) {
      if (size == 0)
         continue;
      cross_size = XMX_MAX(cross_size, off > UINT64_MAX - size ? UINT64_MAX : off + size);
      if (xmx_sv_eq(typ, "local_size") || xmx_sv_eq(typ, "enqueued_local_size") || xmx_sv_eq(typ, "group_count")) {
         if (off > sizeof(cross) - 12)
            PACK_FAIL("cross-thread argument at %" PRIu64 " out of range", off);
         const bool gc = xmx_sv_eq(typ, "group_count");   /* 1,1,1 in the image; the layer writes the real count */
         if (gc)
            *gc_off = (uint32_t)off;
         for (unsigned k = 0; k < 3; k++)
            xmx_wr32(cross + off + 4 * k, gc ? 1 : (uint32_t)lws[k]);
      } else if (xmx_sv_eq(typ, "work_dimensions")) {
         if (off > sizeof(cross) - 4)
            PACK_FAIL("cross-thread argument at %" PRIu64 " out of range", off);
         xmx_wr32(cross + off, lws[2] > 1 ? 3 : (lws[1] > 1 ? 2 : 1));
      }
   }
   cross_size = cross_size > sizeof(cross) ? UINT64_MAX : (cross_size + 31) / 32 * 32;
   if (cross_size > sizeof(cross))
      PACK_FAIL("cross-thread data larger than 128 bytes");

   /* per-thread data: one GRF per thread */
   uint64_t per_size = 0;
   struct xmx_sv per;
   if (xmx_sv_split1(blk, "per_thread_payload_arguments:", &per)) {
      for (size_t pos = 0; xmx_re_next_arg(per, &pos, &typ, &off, &size);)
         per_size = XMX_MAX(per_size, off > UINT64_MAX - size ? UINT64_MAX : off + size);
   }
   if (per_size <= UINT32_MAX)
      per_size = (per_size + grf_bytes - 1) / grf_bytes * grf_bytes;
   if (per_size > UINT32_MAX)
      PACK_FAIL("per-thread data out of range");

   /* surfaces in argument order; surface k is the k-th descriptor table */
   uint64_t bti_arg[24], bti_val[24], b, a;
   unsigned ns = 0;
   struct xmx_sv bt;
   if (xmx_sv_split1(blk, "binding_table_indices:", &bt)) {
      for (size_t pos = 0; xmx_re_next_bti(bt, &pos, &b, &a);) {
         unsigned k = 0;
         while (k < ns && bti_arg[k] != a)
            k++;
         if (k == ns) {
            if (ns == XMX_ARRAY_SIZE(bti_arg))
               PACK_FAIL("more than 24 surfaces");
            /* sorted by argument index */
            while (k > 0 && bti_arg[k - 1] > a) {
               bti_arg[k] = bti_arg[k - 1];
               bti_val[k] = bti_val[k - 1];
               k--;
            }
            bti_arg[k] = a;
            ns++;
         }
         bti_val[k] = b;
      }
   }
   if (grf > UINT32_MAX || slm > UINT32_MAX || barriers > UINT32_MAX || code.n > UINT32_MAX)
      PACK_FAIL("header value out of range");
   for (unsigned k = 0; k < ns; k++)
      if (bti_val[k] > UINT32_MAX)
         PACK_FAIL("binding table index out of range");

   *out_size = XMX_HEADER_SIZE + code.n;
   out = calloc(1, *out_size);
   if (!out)
      PACK_FAIL("out of memory");
   memcpy(out, "CMK2", 4);
   const uint64_t hdr[9] = { code.n, grf, slm, barriers, lws[0], lws[1], lws[2], cross_size, per_size };
   for (unsigned k = 0; k < 9; k++)
      xmx_wr32(out + 4 + 4 * k, hdr[k]);
   memcpy(out + 40, cross, sizeof(cross));
   xmx_wr32(out + 168, 0xffffffffu);   /* heap set: found at pipeline creation; binding and table base 0 */
   xmx_wr32(out + 180, ns);
   for (unsigned k = 0; k < ns; k++) {
      xmx_wr32(out + 184 + 4 * k, bti_val[k]);
      xmx_wr32(out + 280 + 4 * k, k);    /* table k, element 0 */
   }
   memcpy(out + XMX_HEADER_SIZE, code.p, code.n);
   free(s);
   return out;
fail:
   free(s);
   free(out);
   return NULL;
#undef PACK_FAIL
}

/* The work-group size in the compile options, read as tools/cm-compile.sh reads it (NUL bytes dropped, the first
 * "LWS_SIZE_X=<n> -DLWS_SIZE_Y=<n> -DLWS_SIZE_Z=<n>"), else 1,1,1. */
void
xmx_opt_lws(const uint8_t *opt, size_t n, uint64_t lws[3])
{
   lws[0] = lws[1] = lws[2] = 1;
   char *t = malloc(n + 1);
   if (!t)
      return;
   size_t m = 0;
   for (size_t i = 0; i < n; i++)
      if (opt[i])
         t[m++] = opt[i];
   const struct xmx_sv s = { t, m };
   for (size_t i = xmx_sv_find(s, 0, "LWS_SIZE_X="); i != XMX_NPOS; i = xmx_sv_find(s, i + 1, "LWS_SIZE_X=")) {
      size_t j = i + strlen("LWS_SIZE_X=");
      uint64_t v[3];
      if (xmx_re_num(s, &j, &v[0]) && xmx_re_lit(s, &j, " -DLWS_SIZE_Y=") && xmx_re_num(s, &j, &v[1]) &&
          xmx_re_lit(s, &j, " -DLWS_SIZE_Z=") && xmx_re_num(s, &j, &v[2])) {
         memcpy(lws, v, sizeof(v));
         break;
      }
   }
   free(t);
}
/* ---- end of the .cmk packer ---- */

/* ---- CML1, the layer's cache format (see cmk.h) ---- */
uint8_t *
xmx_cml_from_cmk(const uint8_t *cmk, size_t n, uint32_t gc_off, uint32_t flags, size_t *out_size)
{
   if (n < XMX_CMK_HEADER || memcmp(cmk, "CMK2", 4) != 0 || xmx_rd32(cmk + XMX_H_CODE_SIZE) != n - XMX_CMK_HEADER)
      return NULL;
   *out_size = n + (XMX_CML_HEADER - XMX_CMK_HEADER);
   uint8_t *o = malloc(*out_size);
   if (!o)
      return NULL;
   memcpy(o, cmk, XMX_CMK_HEADER);
   memcpy(o, "CML1", 4);
   xmx_wr32(o + XMX_CMK_HEADER, gc_off);
   xmx_wr32(o + XMX_CMK_HEADER + 4, flags);
   memcpy(o + XMX_CML_HEADER, cmk + XMX_CMK_HEADER, n - XMX_CMK_HEADER);
   return o;
}

int
xmx_cml_parse(const uint8_t *img, size_t n, struct xmx_cml *out)
{
   if (n < XMX_CML_HEADER || memcmp(img, "CML1", 4) != 0)
      return 0;
   const uint32_t cs = xmx_rd32(img + XMX_H_CODE_SIZE);
   if (cs != n - XMX_CML_HEADER || xmx_rd32(img + XMX_H_NSURF) > 24 || xmx_rd32(img + XMX_H_CROSS_SIZE) > 128)
      return 0;
   out->hdr = img;
   out->gc_off = xmx_rd32(img + XMX_CMK_HEADER);
   out->flags = xmx_rd32(img + XMX_CMK_HEADER + 4);
   if (out->gc_off != XMX_NO_GC && out->gc_off > 128 - 12)
      return 0;
   out->code = img + XMX_CML_HEADER;
   out->code_size = cs;
   return 1;
}
