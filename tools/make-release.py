#!/usr/bin/env python3
"""make-release.py <version> [out dir]
Builds release/xess-xmx-linux-<version>.zip from the working tree: the binaries (igdext64.dll, the drivers
lib/mesa-<version>/libvulkan_intel.so with the active one as lib/libvulkan_intel.so and its name in lib/active),
the scripts, tools, patches and docs. Shell and Python scripts get the executable bit.
Kernels are not included (they are compiled on the user's machine). Refuses to run if a binary is missing."""
import os, sys, zipfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
version = sys.argv[1]
outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "release")
name = "xess-xmx-linux-%s" % version
files = ["igdext64.dll", "lib/libvulkan_intel.so", "install.sh", "uninstall.sh", "enable.sh", "disable.sh",
         "xmx-launch.sh", "README.md", "LICENSE", "THIRD_PARTY.md"]
tools = ["cm-compile.sh", "get-igc.sh", "session-check.sh", "rebuild-driver.sh", "switch-driver.sh", "cmk_pack.py", "check-kernels.py", "build-kernels.sh", "s16_map.py",
         "gen_all_dummies.py", "gen_dyn_dummies.py", "check_dyn_ids.py"]
files += ["tools/" + t for t in tools]
for dp, dn, fn in os.walk(os.path.join(root, "patches")):
    dn.sort()
    files += [os.path.relpath(os.path.join(dp, p), root).replace(os.sep, "/") for p in sorted(fn) if p.endswith(".patch")]
# one driver per Mesa version in lib/mesa-<version>/, the active one copied to lib/libvulkan_intel.so (tools/switch-driver.sh)
libdir = os.path.join(root, "lib")
drivers = ["lib/%s/libvulkan_intel.so" % d for d in sorted(os.listdir(libdir))
           if d.startswith("mesa-") and os.path.isfile(os.path.join(libdir, d, "libvulkan_intel.so"))]
if not drivers or not os.path.isfile(os.path.join(libdir, "active")):
    sys.exit("lib/mesa-<version>/libvulkan_intel.so and lib/active are needed")
active = "lib/mesa-%s/libvulkan_intel.so" % open(os.path.join(libdir, "active")).read().strip()
if active not in drivers or open(os.path.join(root, active), "rb").read() != open(os.path.join(libdir, "libvulkan_intel.so"), "rb").read():
    sys.exit("lib/libvulkan_intel.so is not the driver lib/active names")
files += drivers + ["lib/active"]
files += ["docs/" + d for d in sorted(os.listdir(os.path.join(root, "docs"))) if d.endswith(".md")]
missing = [f for f in files if not os.path.isfile(os.path.join(root, f))]
if missing:
    sys.exit("missing: " + ", ".join(missing))
os.makedirs(outdir, exist_ok=True)
out = os.path.join(outdir, name + ".zip")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    # an empty kernels/ folder (static kernel set, developer option)
    zi = zipfile.ZipInfo(name + "/kernels/")
    zi.create_system = 3              # Unix: unzip applies the permission bits only then
    zi.external_attr = (0o40755 << 16) | 0x10
    z.writestr(zi, b"")
    for f in files:
        zi = zipfile.ZipInfo("%s/%s" % (name, f))
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.create_system = 3
        zi.external_attr = (0o100755 if f.endswith((".sh", ".py")) else 0o100644) << 16
        with open(os.path.join(root, f), "rb") as fh:
            z.writestr(zi, fh.read())
print(out, os.path.getsize(out), "bytes,", len(files), "files")
