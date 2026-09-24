#!/usr/bin/env python3
"""make-release.py <version> [out dir]
Builds release/xess-xmx-linux-<version>.zip from the working tree: the two binaries (igdext64.dll,
lib/libvulkan_intel.so), the scripts, tools, patches and docs. Shell and Python scripts get the executable bit.
Kernels are not included (they are compiled on the user's machine). Refuses to run if a binary is missing."""
import os, sys, zipfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
version = sys.argv[1]
outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "release")
name = "xess-xmx-linux-%s" % version
files = ["igdext64.dll", "lib/libvulkan_intel.so", "install.sh", "uninstall.sh", "enable.sh", "disable.sh",
         "xmx-launch.sh", "README.md", "LICENSE", "THIRD_PARTY.md"]
tools = ["cm-compile.sh", "get-igc.sh", "cmk_pack.py", "check-kernels.py", "build-kernels.sh", "s16_map.py",
         "gen_all_dummies.py", "gen_dyn_dummies.py", "check_dyn_ids.py"]
files += ["tools/" + t for t in tools]
files += ["patches/" + p for p in sorted(os.listdir(os.path.join(root, "patches"))) if p.endswith(".patch")]
files += ["docs/" + d for d in sorted(os.listdir(os.path.join(root, "docs"))) if d.endswith(".md")]
missing = [f for f in files if not os.path.isfile(os.path.join(root, f))]
if missing:
    sys.exit("missing: " + ", ".join(missing))
os.makedirs(outdir, exist_ok=True)
out = os.path.join(outdir, name + ".zip")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    # an empty kernels/ folder (static kernel set, developer option)
    zi = zipfile.ZipInfo(name + "/kernels/")
    zi.external_attr = (0o40755 << 16) | 0x10
    z.writestr(zi, b"")
    for f in files:
        zi = zipfile.ZipInfo("%s/%s" % (name, f))
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.external_attr = (0o100755 if f.endswith((".sh", ".py")) else 0o100644) << 16
        with open(os.path.join(root, f), "rb") as fh:
            z.writestr(zi, fh.read())
print(out, os.path.getsize(out), "bytes,", len(files), "files")
