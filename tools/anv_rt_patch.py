#!/usr/bin/env python3
"""anv_rt_patch.py <mesa dir>: extend the CM kernel injection in anv_shader_compile.c with run-time compilation.
Dummy pipelines whose workgroup z is 7, 11 or 13 (primes no game uses) are dynamically assigned kernels
(tools/gen_dyn_dummies.py): z=7 -> id = (x-1)+(y-1)*16 (x<=16, y<=9), z=11 -> 144 + ..., (y<=5), z=13 -> 224 + ... (y<=4).
Their SPIR-V / options were written by the shim to $WINEPREFIX/drive_c/igdext_kernels/dyn_<id>.{spv,opt} (override with
ANV_CM_SPV_DIR). If dyn_<id>.cmk is missing there, ANV_CM_HELPER (default $ANV_CM_KERNEL_DIR/../tools/cm-compile.sh)
is run to build it. Re-running the script on an already patched tree updates the id mapping."""
import sys, os, re
p = os.path.join(sys.argv[1], "src/intel/vulkan/anv_shader_compile.c")
s = open(p).read()

mapping = r'''   int id = -1;
   if (wx >= 1 && wx <= 16) {
      if (wz == 7 && wy >= 1 && wy <= 9)
         id = (wx - 1) + (wy - 1) * 16;
      else if (wz == 11 && wy >= 1 && wy <= 5)
         id = 144 + (wx - 1) + (wy - 1) * 16;
      else if (wz == 13 && wy >= 1 && wy <= 4)
         id = 224 + (wx - 1) + (wy - 1) * 16;
   }
   if (id < 0)
      return false;'''
if "anv_cm_dyn_kernel" in s:
    old = re.search(r"   if \(wx < 1 \|\| wx > 8.*?\n   const unsigned id = [^\n]*\n", s, re.S)
    if not old:
        print("already patched with the current mapping"); sys.exit(0)
    s = s.replace(old.group(0), mapping + "\n", 1)
    open(p, "w", newline="\n").write(s)
    print("mapping updated"); sys.exit(0)

helper = r'''
/* Dynamically registered kernels (shim dyn_dummies): workgroup z in {7, 11, 13} -> id, compiled on first use */
static bool
anv_cm_dyn_kernel(uint32_t wx, uint32_t wy, uint32_t wz, char *path, size_t pathlen)
{
''' + mapping + r'''
   char dirbuf[512];
   const char *sdir = getenv("ANV_CM_SPV_DIR");
   if (!sdir) {
      const char *pfx = getenv("WINEPREFIX");
      if (!pfx)
         return false;
      snprintf(dirbuf, sizeof(dirbuf), "%s/drive_c/igdext_kernels", pfx);
      sdir = dirbuf;
   }
   snprintf(path, pathlen, "%s/dyn_%u.cmk", sdir, id);
   if (access(path, R_OK) == 0)
      return true;
   char spv[600], opt[600];
   snprintf(spv, sizeof(spv), "%s/dyn_%u.spv", sdir, id);
   snprintf(opt, sizeof(opt), "%s/dyn_%u.opt", sdir, id);
   if (access(spv, R_OK) != 0 || access(opt, R_OK) != 0) {
      fprintf(stderr, "ANV CM: dynamic kernel %u: no %s\n", id, spv);
      return false;
   }
   char helperbuf[600];
   const char *helper = getenv("ANV_CM_HELPER");
   if (!helper) {
      snprintf(helperbuf, sizeof(helperbuf), "%s/../tools/cm-compile.sh", getenv("ANV_CM_KERNEL_DIR"));
      helper = helperbuf;
   }
   char cmd[2600];
   snprintf(cmd, sizeof(cmd), "/bin/sh '%s' '%s' '%s' '%s'", helper, spv, opt, path);
   fprintf(stderr, "ANV CM: compiling dynamic kernel %u (%s)\n", id, spv);
   const int rc = system(cmd);
   if (rc != 0 || access(path, R_OK) != 0) {
      fprintf(stderr, "ANV CM: dynamic kernel %u: helper failed (rc %d)\n", id, rc);
      return false;
   }
   return true;
}

static bool
anv_cm_kernel_replace('''
anchor = "\nstatic bool\nanv_cm_kernel_replace("
assert s.count(anchor) == 1
s = s.replace(anchor, helper, 1)

old = '''   if (access(path, R_OK) != 0) {
      /* dummy pipelines made by the igdext shim encode the XeSS kernel index in the workgroup size (x, y, 5) */
      if (nir->info.workgroup_size[2] != 5)
         return false;
      snprintf(path, sizeof(path), "%s/wg%u_%u_5.cmk", dir,
               nir->info.workgroup_size[0], nir->info.workgroup_size[1]);
      if (access(path, R_OK) != 0)
         return false;
   }'''
new = '''   if (access(path, R_OK) != 0) {
      /* dummy pipelines made by the igdext shim encode the XeSS kernel index in the workgroup size: static set (x, y, 5),
       * dynamically registered kernels (x<=8, y<=8, 6..15) compiled on first use */
      const uint32_t wx = nir->info.workgroup_size[0], wy = nir->info.workgroup_size[1], wz = nir->info.workgroup_size[2];
      if (wz == 5) {
         snprintf(path, sizeof(path), "%s/wg%u_%u_5.cmk", dir, wx, wy);
         if (access(path, R_OK) != 0)
            return false;
      } else if (!anv_cm_dyn_kernel(wx, wy, wz, path, sizeof(path))) {
         return false;
      }
   }'''
assert s.count(old) == 1
s = s.replace(old, new, 1)
open(p, "w", newline="\n").write(s)
print("patched")
