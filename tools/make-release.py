#!/usr/bin/env python3
"""make-release.py <version> [out dir]
Builds release/xess-xmx-linux-<version>.zip from the working tree: the binaries (igdext64.dll, the drivers
lib/mesa-<version>/libvulkan_intel.so, the default's name in lib/active, and lib/libvulkan_intel.so built from
icd/xmx-icd.c, which loads one of them),
lib/bundled/ (tools/build-deps.sh), the Vulkan layer for the system's own driver (layer/, xmx-stock.sh), the scripts,
tools, patches and docs. Shell and Python scripts get the executable bit.
Kernels are not included (they are compiled on the user's machine). Refuses to run if a binary is missing."""
import os, sys, zipfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
version = sys.argv[1]
outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "release")
name = "xess-xmx-linux-%s" % version
files = ["igdext64.dll", "ControlLib.dll", "ffxfg/amd_fidelityfx_framegeneration_dx12.dll", "ffxfg/amd_fidelityfx_upscaler_dx12.dll", "ffxfg/dxgi.dll",
         "ffxfg/LICENSE.FidelityFX-SDK.txt", "dx11bridge/libxess_dx11.dll", "vkwrap/vulkan-1.dll",
         "lib/libvulkan_intel.so", "install.sh", "uninstall.sh", "enable.sh", "disable.sh",
         "xmx-launch.sh", "xmx-stock.sh", "README.md", "LICENSE", "THIRD_PARTY.md"]
# what a user's install needs; developer tools (gen_placeholders.py, check_placeholders.py, build-deps.sh, split_patch.py)
# stay in the repository
tools = ["cm-compile.sh", "cmk_pack.py", "get-igc.sh", "get-xess.sh", "get-ffx.sh", "proton-hook.py", "fsr-fg.sh",
         "steam-libraries.sh", "session-check.sh", "rebuild-driver.sh", "switch-driver.sh", "xell-probe.sh"]
files += ["tools/" + t for t in tools]
for dp, dn, fn in os.walk(os.path.join(root, "patches")):
    dn.sort()
    files += [os.path.relpath(os.path.join(dp, p), root).replace(os.sep, "/") for p in sorted(fn) if p.endswith(".patch")]
# one driver per Mesa version in lib/mesa-<version>/; lib/active names the default (tools/switch-driver.sh), and
# lib/libvulkan_intel.so, the forwarding ICD (icd/xmx-icd.c), loads it
libdir = os.path.join(root, "lib")
drivers = ["lib/%s/libvulkan_intel.so" % d for d in sorted(os.listdir(libdir))
           if d.startswith("mesa-") and os.path.isfile(os.path.join(libdir, d, "libvulkan_intel.so"))]
if not drivers or not os.path.isfile(os.path.join(libdir, "active")):
    sys.exit("lib/mesa-<version>/libvulkan_intel.so and lib/active are needed")
active = "lib/mesa-%s/libvulkan_intel.so" % open(os.path.join(libdir, "active")).read().strip()
if active not in drivers:
    sys.exit("lib/active names no driver in lib/")
if b"xess-xmx: using the Mesa" not in open(os.path.join(libdir, "libvulkan_intel.so"), "rb").read():
    sys.exit("lib/libvulkan_intel.so must be the ICD built from icd/xmx-icd.c")
# the DLLs in ffxfg/ carry AMD's file names; only this project's own builds (ffxfg/) may be packed, never AMD's files
for dll, mark in (("ffxfg/amd_fidelityfx_framegeneration_dx12.dll", b"ffx_xefg start"),
                  ("ffxfg/amd_fidelityfx_upscaler_dx12.dll", b"ffx_xess start"), ("ffxfg/dxgi.dll", b"dxgi_xefg.log")):
    p = os.path.join(root, dll)
    if os.path.isfile(p) and mark not in open(p, "rb").read():
        sys.exit("%s is not this project's build (ffxfg/build.bat)" % dll)
# dx11bridge/libxess_dx11.dll carries Intel's file name; only this project's build (dx11bridge/build.bat) may be packed
p = os.path.join(root, "dx11bridge/libxess_dx11.dll")
if os.path.isfile(p) and b"[xess-dx11]" not in open(p, "rb").read():
    sys.exit("dx11bridge/libxess_dx11.dll is not this project's build (dx11bridge/build.bat)")
p = os.path.join(root, "vkwrap/vulkan-1.dll")
if os.path.isfile(p) and b"xess-xmx vkwrap" not in open(p, "rb").read():
    sys.exit("vkwrap/vulkan-1.dll is not this project's build (vkwrap/build.bat)")
files += drivers + ["lib/active"]
# lib/bundled/: SPIRV-Tools and libdisplay-info for systems that lack them, with licenses (tools/build-deps.sh)
bundled = ["lib/bundled/" + f for f in ("libSPIRV-Tools.so", "libdisplay-info.so.3", "LICENSE.SPIRV-Tools",
                                         "LICENSE.libdisplay-info", "SOURCES.txt")]
files += bundled
# layer/: VK_LAYER_XESS_xmx for xmx-stock.sh (the system's unpatched ANV). The release is packed in a fresh clone, so
# libVkLayer_xess_xmx.so is the CI artifact "xmx-layer" (job "layer", layer/build.sh) copied into layer/; the manifest
# is the repository's layer/VkLayer_xess_xmx.json (library_path ./libVkLayer_xess_xmx.so, next to it). At run time it
# needs tools/cm-compile.sh and the compiler bundle (igc/), which the package has anyway. BLAKE3 is compiled into it
# (its licences ship). layer/gpu-ids.txt: the GPUs the layer serves, which install.sh reads for its stock-mode choice
layer = ["layer/libVkLayer_xess_xmx.so", "layer/VkLayer_xess_xmx.json", "layer/gpu-ids.txt",
         "layer/third_party/blake3/LICENSE_A2", "layer/third_party/blake3/LICENSE_CC0"]
so = os.path.join(root, layer[0])
if os.path.isfile(so):
    d = open(so, "rb").read()
    if not d.startswith(b"\x7fELF") or b"xmx_NegotiateLoaderLayerInterfaceVersion" not in d:
        sys.exit("layer/libVkLayer_xess_xmx.so is not the layer (layer/build.sh)")
files += layer
for dp, dn, fn in os.walk(os.path.join(root, "docs")):   # docs/ and docs/dev/
    dn.sort()
    files += [os.path.relpath(os.path.join(dp, d), root).replace(os.sep, "/") for d in sorted(fn) if d.endswith(".md")]
missing = [f for f in files if not os.path.isfile(os.path.join(root, f))]
if missing:
    sys.exit("missing: " + ", ".join(missing))
os.makedirs(outdir, exist_ok=True)
out = os.path.join(outdir, name + ".zip")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    # (install.sh creates kernels/, the folder ANV_CM_KERNEL_DIR names, itself)
    for f in files:
        zi = zipfile.ZipInfo("%s/%s" % (name, f))
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.create_system = 3              # Unix: unzip applies the permission bits only then
        zi.external_attr = (0o100755 if f.endswith((".sh", ".py")) else 0o100644) << 16
        with open(os.path.join(root, f), "rb") as fh:
            z.writestr(zi, fh.read())
print(out, os.path.getsize(out), "bytes,", len(files), "files")
