# Building from Source

The release archive contains prebuilt Windows and Linux binaries compiled from this repository. Intel C-for-Metal (CM, Intel's GPU kernel language) kernels are not in the repository or the release archive; the driver or layer compiles them on the user's machine at runtime from the SPIR-V inside each game's XeSS libraries (see [how-it-works.md](how-it-works.md)).

GitHub Actions (`.github/workflows/build.yml`) builds every binary on every push and pull request and keeps them as workflow artifacts. To build a release archive, download the artifacts for the release commit and run `tools/make-release.py`.

## Repository Layout

```text
install.sh, uninstall.sh      system-wide install / removal
enable.sh, disable.sh         per-prefix install / removal; xmx-launch.sh: Steam launch wrapper
patches/                      Mesa 26.1.2 ANV: 0001 kernel injection and run-time compilation, 0002 debug tools,
                              0003 Large GRF (Xe-HPG, Xe2), 0005 sampler descriptor tables (FSR 4 on XMX);
                              patches/mesa-26.3/: the same for Mesa 26.3, plus 0006, a Jay crash fix for
                              cooperative matrix shaders
layer/, xmx-stock.sh          VK_LAYER_XESS_xmx: the XMX kernels on an unpatched ANV (Xe3's session default; xmx-stock.sh:
                              the system's own, per game) and XeSS' Vulkan path on every driver of the package
shim/                         igdext64.dll source: dxvk-igdext plus this project's code in src/dll/
controllib/                   ControlLib.dll source: the frame generation override (XESS_FG)
ffxfg/                        source of amd_fidelityfx_upscaler_dx12.dll (FSR upscaling on XeSS SR),
                              amd_fidelityfx_framegeneration_dx12.dll (FSR frame generation on XeSS-FG) and
                              dxgi.dll (XeSS-FG for a game without FSR frame generation, FSR_FG=inject, and
                              for any DX12 game from optical flow, OFLOW_FG=1); get-headers.sh fetches the AMD /
                              Intel headers they are built against, build-ffxsdk.bat the FidelityFX SDK 1.1.4
                              optical flow libraries
dx11bridge/                   libxess_dx11.dll source: XeSS for D3D11 games on the D3D12 XeSS
vkwrap/                       vulkan-1.dll source: XeSS' Intel Vulkan interfaces for Vulkan games
icd/xmx-icd.c                 lib/libvulkan_intel.so: loads the driver MESA or the default names
tools/cm-compile.sh           run-time compiler helper called by the driver
tools/get-igc.sh              fetches IGC 2.10.10 + ocloc 25.18 into igc/
tools/get-xess.sh             fetches Intel's XeSS SDK libraries into xess/ (XESS=latest)
tools/get-ffx.sh              fetches AMD's FidelityFX loader and providers into ffx/ (FSR / FSR_FG with FSR 3.1.0-3.1.3)
tools/proton-hook.py          XESS / XESS_LOG / FSR / FSR_FG / OFLOW_FG launch options, run by Proton through user_settings.py
tools/fsr-fg.sh               FSR on XeSS for one game, for Protons without the hook
tools/steam-libraries.sh      every Steam library (libraryfolders.vdf), for the scripts
tools/rebuild-driver.sh       rebuilds the patched driver for the system's Mesa version
tools/switch-driver.sh        sets the default among the package's drivers (per game: MESA)
tools/session-check.sh        the driver check run before each session
tools/cmk_pack.py             zebin -> .cmk packer (the driver has a C port; cm-compile.sh uses this one for older drivers)
tools/xell-probe.sh           XeLL driver-mode probe (debugging.md)
tools/gen_placeholders.py     placeholder templates for run-time kernels; tools/check_placeholders.py consistency check
tools/split_patch.py          writes patches 0001 / 0002 from the marked source tree
tools/build-deps.sh           builds the bundled SPIRV-Tools and libdisplay-info (lib/bundled/)
tools/make-release.py         builds the release archive (the developer tools stay out of it)
docs/                         user guides (install, drivers, games, frame generation, launch options, performance, troubleshooting)
docs/dev/                     developer documents (how it works, building, debugging)
.github/workflows/build.yml   CI: builds the shim, ControlLib.dll, the FSR DLLs, the D3D11/Vulkan DLLs, the ICD,
                              both patched drivers, the unpatched driver and the layer, checks the scripts
```

The release archive holds the binaries (`igdext64.dll`, `ControlLib.dll`, the three DLLs in `ffxfg/`, `dx11bridge/libxess_dx11.dll`, `vkwrap/vulkan-1.dll`, the patched drivers in `lib/mesa-<version>/`, the unpatched driver in `lib/mesa-stock-26.3.0/`, the ICD `lib/libvulkan_intel.so`, `lib/bundled/`, and the layer `layer/libVkLayer_xess_xmx.so` with `layer/VkLayer_xess_xmx.json`) and the scripts. It contains no Intel kernels; they are compiled on the user's machine from the SPIR-V inside the game's XeSS libraries.

| Artifact / Binary | Source Directory | Toolchain | Purpose |
|---|---|---|---|
| `igdext64.dll` | `shim/` | Windows, MSVC (Visual Studio 2022 Build Tools), CMake + Ninja | D3D12 Intel extension shim (`_INTC_D3D12_*`) and XeLL / XeFG hooks |
| `ControlLib.dll` | `controllib/` | Windows, MSVC (`cl`) | Intel Graphics Control Library stand-in for `XESS_FG` multiplier overrides |
| `amd_fidelityfx_upscaler_dx12.dll` | `ffxfg/` | Windows, MSVC (`cl`) | FSR 3.1+ upscaling translated to XeSS super resolution |
| `amd_fidelityfx_framegeneration_dx12.dll` | `ffxfg/` | Windows, MSVC (`cl`, `dxc`), CMake | FSR 3.1+ frame generation and `OFLOW_FG=1` optical flow on XeSS-FG |
| `dxgi.dll` | `ffxfg/` | Windows, MSVC (`cl`) | DXGI factory wrapper for `FSR_FG=inject` and `OFLOW_FG=1` |
| `libxess_dx11.dll` | `dx11bridge/` | Windows, MSVC (`cl`, `fxc`, `rc`) | XeSS D3D11 API bridge running on D3D12 XeSS |
| `vulkan-1.dll` | `vkwrap/` | Windows, MSVC (`cl`, `ml64`), Python 3 | Vulkan loader wrapper providing Intel private interfaces for Vulkan XeSS |
| `lib/libvulkan_intel.so` | `icd/xmx-icd.c` | Linux, `gcc` | Forwarding Vulkan ICD (installable client driver) that loads the selected Mesa driver |
| `lib/mesa-<ver>/libvulkan_intel.so` | `patches/` + Mesa tarball | Linux (Arch container), `meson` + `ninja` | Patched (`26.1.2`, `26.3.0`) and unpatched (`stock-26.3.0`) Mesa ANV drivers |
| `lib/bundled/*` | `tools/build-deps.sh` | Linux (Debian 12 container), CMake + Meson | Bundled `libSPIRV-Tools.so` and `libdisplay-info.so.3` (glibc >= 2.36) for systems that lack them |
| `layer/libVkLayer_xess_xmx.so` | `layer/` | Linux, `gcc`, `glslangValidator`, Python 3 | `VK_LAYER_XESS_xmx` Vulkan layer for unpatched ANV (`xmx-stock.sh`) and Vulkan XeSS |

---

## Building Windows Components

All Windows DLLs build with the Visual Studio 2022 Build Tools (Desktop development with C++ workload, including the Windows 10/11 SDK for `fxc.exe`, `dxc.exe`, and `rc.exe`). Every DLL links statically against the MSVC runtime (`/MT` or `MultiThreaded`) so it does not depend on the Visual C++ redistributable inside a game's Proton prefix.

### 1. The D3D12 Shim (`igdext64.dll`) and `ControlLib.dll`

Build `igdext64.dll` using the batch script on Windows:

```bat
shim\build.bat
```

Or run CMake and Ninja from any shell with the MSVC x64 environment loaded:

```bat
cmake -S shim -B shim/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build shim/build
```

This writes `shim/build/igdext64.dll`. The DLL links against the static MSVC runtime and imports only `kernel32`, `advapi32`, and `dxgi`. A dynamically linked build crashed in `DllMain` in a prefix with an older `msvcp140.dll` (Resident Evil 4), causing XeSS to fall back to DP4a (dot-product instructions). `shim/toolchain-mingw64.cmake` is kept from upstream `dxvk-igdext` for MinGW cross-compilation, but is not used for releases.

#### Regenerating and Checking the DXBC Placeholder Templates

The two DXBC (DirectX Bytecode) compute shader templates in `shim/src/dll/placeholders.inc` (`kPlaceholderKernel` with `numthreads(1, 1, 23)` and `kPlaceholderCanary` with `numthreads(1, 1, 19)`) are generated using `fxc.exe` from the Windows SDK rather than written by hand. Under WSL, pass the path to the Windows `fxc.exe` as the second argument (paths are converted with `wslpath`):

```bash
python tools/gen_placeholders.py shim/src/dll/placeholders.inc                     # kernel and self-test templates
python tools/check_placeholders.py patches/0001-anv-cm-kernel-injection.patch      # verify they match the driver
python tools/check_placeholders.py patches/mesa-26.3/0001-anv-cm-kernel-injection.patch
```

`tools/check_placeholders.py` checks that the template shapes, magic value (`0x584d5843`), self-test canary constants (`0x00414e43` and `0x7fa15952`), byte offsets (`kPlaceholderKernelHashAt` and `kPlaceholderCanaryHashAt`), and custom DXBC MD5 checksums (`shim/src/dll/DxbcChecksum.h`) match the driver patches.

#### Building `ControlLib.dll`

Build `ControlLib.dll` (`controllib/ControlLib.c`, the stand-in for Intel's Control Library that `XESS_FG` needs):

```bat
controllib\build.bat
```

This runs `cl /nologo /O2 /W3 /MT /LD ControlLib.c /Fobuild\ /Fe:build\ControlLib.dll /link dxgi.lib dxguid.lib` and outputs `controllib/build/ControlLib.dll`.

### 2. The FSR DLLs (`ffxfg/amd_fidelityfx_upscaler_dx12.dll`, `ffxfg/amd_fidelityfx_framegeneration_dx12.dll`, `ffxfg/dxgi.dll`)

Building the DLLs in `ffxfg/` ([../frame-generation.md](../frame-generation.md)) requires two prerequisites that are not tracked in git:

1. **Public headers (`ffxfg/inc/`):** Run `ffxfg/get-headers.sh` in a Bash shell (requires `curl` and `sha256sum`). It downloads headers from AMD FSR SDK `v2.4.0` and Intel XeSS SDK (commit `de0fb9c1c510661c571164e1418ceca8101dab69`, the commit after `v3.0.2` that put the XeLL headers under the MIT license) into `ffxfg/inc/` and checks each file by SHA-256:

   ```bash
   bash ffxfg/get-headers.sh
   ```

2. **FidelityFX SDK 1.1.4 optical flow static libraries (`ffxfg/ffxsdk/`):** `ffxfg/build-ffxsdk.bat` clones `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK` at tag `v1.1.4`, checks that `HEAD` matches commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`, and builds `ffx_opticalflow_x64.lib` and `ffx_backend_dx12_x64.lib` with static MSVC runtime (`MultiThreaded`). `ffxfg/build.bat` runs `build-ffxsdk.bat` automatically if the `.lib` files are missing.

Then, on Windows with the Visual Studio 2022 Build Tools:

```bat
ffxfg\build.bat
```

`ffxfg\build.bat` compiles `oflow_expand.hlsl` and `oflow_down.hlsl` to headers (`oflow_expand_cs.h`, `oflow_down_cs.h`) using `dxc -T cs_6_0`, compiles `ffx_oflow.cpp` and `ffx_xefg.cpp` into `ffxfg\build\amd_fidelityfx_framegeneration_dx12.dll`, compiles `ffx_xess.cpp` into `ffxfg\build\amd_fidelityfx_upscaler_dx12.dll`, and compiles `dxgi_xefg.cpp` into `ffxfg\build\dxgi.dll`. The package keeps them in `ffxfg/`.

To include the debug switches (`FFX_XEFG_CAMERA`, `FFX_XEFG_JITTER`, `FFX_XEFG_RENDERTIME`, `FFX_XEFG_RENDERSCALE`, `FFX_XEFG_DUMP`, `FFX_XEFG_ORDER`, and `FFX_XESS_JITTER`; see [debugging.md](debugging.md)), set `FFXFG_DEBUG=1` in the environment before running `build.bat`:

```bat
set FFXFG_DEBUG=1
ffxfg\build.bat
```

### 3. XeSS for D3D11 (`dx11bridge/libxess_dx11.dll`) and Vulkan (`vkwrap/vulkan-1.dll`)

Both components need the XeSS headers fetched by `bash ffxfg/get-headers.sh` ([../games.md](../games.md#xess-in-a-d3d11-game)). On Windows with the same Build Tools (and Python for `gen.py`, which writes 259 forwarding stubs in `stubs.asm`, `names.h`, and `vulkan-1.def` from the 262 exports in `vkwrap/exports.txt`, the export list of Proton's `vulkan-1.dll`, while `vkwrap.c` implements `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr`, and `vkCreateDevice` itself):

```bat
bash ffxfg/get-headers.sh
dx11bridge\build.bat                          -> dx11bridge\build\libxess_dx11.dll
python vkwrap\gen.py vkwrap\exports.txt vkwrap\build
vkwrap\build.bat                              -> vkwrap\build\vulkan-1.dll
```

* `dx11bridge\build.bat` compiles `depth_copy.hlsl` with `fxc /T cs_5_0` into `build\depth_copy_cs.h`, compiles `version.rc` (FileVersion `2.0.2.68`, so DXVK does not hide the Intel GPU), and links `xess_dx11.cpp` into `dx11bridge\build\libxess_dx11.dll`.
* `vkwrap\build.bat` assembles `build\stubs.asm` with `ml64` and compiles `vkwrap.c` into `vkwrap\build\vulkan-1.dll`.

The package keeps these DLLs in `dx11bridge/` and `vkwrap/`.

#### Building the D3D11 and Vulkan Tests

Build the test programs on Windows with the same Build Tools:

```bat
dx11bridge\test\build.bat
set VULKAN_HEADERS=C:\path\to\Vulkan-Headers
vkwrap\test\build.bat
```

* `dx11bridge\test\build.bat` builds `sharetest.exe` (a texture and a fence shared by a D3D11 and a D3D12 device) and `bridgetest.exe` (the D3D11 bridge with `D24_UNORM_S8_UINT` depth, checking output color and preserved D3D11 compute state).
* `vkwrap\test\build.bat` (with `VULKAN_HEADERS` set to a Khronos `Vulkan-Headers` checkout) builds `vktest.exe` (XeSS's Vulkan path through `vulkan-1.dll` and `VK_LAYER_XESS_xmx`, checking output color).

Run each `.exe` under Proton with the corresponding DLL (`libxess_dx11.dll` or `vulkan-1.dll`, plus `libxess.dll`) next to it. Each test writes `<name>.txt` (`sharetest.txt`, `bridgetest.txt`, `vktest.txt`) next to itself.

---

## Building the Vulkan Layer (`layer/libVkLayer_xess_xmx.so`)

Build the layer on Linux with `gcc`, `glslangValidator` (`glslang-tools`), and `python3` against Khronos `Vulkan-Headers` 1.4.350 (CI pins tag `vulkan-sdk-1.4.350.1`, commit `8864cdc896bbc2a9b6eb36b3218fc9ef57908d77`, because a distribution's system headers may lack `VK_KHR_pipeline_binary`):

```bash
VULKAN_HEADERS=<Vulkan-Headers checkout> layer/build.sh out   # -> out/libVkLayer_xess_xmx.so, out/VkLayer_xess_xmx.json
layer/tests/build.sh                                          # unit tests
```

If you omit the output directory argument (`layer/build.sh`), it defaults to `layer/out`. `layer/build.sh` compiles `layer/src/template.comp` into a SPIR-V C array (`template_spv.h`), compiles the layer sources with the portable BLAKE3 implementation in `layer/third_party/blake3/`, and copies `layer/VkLayer_xess_xmx.json`. For a release archive, copy the CI artifact `xmx-layer` (`libVkLayer_xess_xmx.so`) into `layer/` alongside the repository's `layer/VkLayer_xess_xmx.json`.

### Layer Unit Tests and Probing a New Mesa Version

`layer/tests/build.sh` compiles and runs unit tests for SPIR-V placeholder detection (`test_placeholder`), the C zebin-to-`CMK2`/`CML1` packer against `tools/cmk_pack.py` (`test_cmk`), `-noLoadPayload` compilation via `tools/cm-compile.sh` (`test_compile`), the C prologue table against the IGA assembler (`test_prologue`), and ANV binary serialization round-tripping (`test_anvbin`). Tests that need `IGC`, offline kernel sets (`CMK_SET`, `KSET`), or exported template blobs (`TEMPLATES`) are skipped automatically when those are absent.

ANV's serialized compute shader format (`layer/src/anvbin.c`) differs between Mesa minor versions. `layer/src/layouts.h` holds one measured offset table (`struct anv_layout`) per supported version (`Mesa 26.1.`, `Mesa 26.3.`, and `Mesa 27.0.`), including `pd_size_patched` (`656` on 26.1.2, `664` on 26.3.0) for this project's patched drivers. The offsets were measured on exported binaries from the variant shaders in `layer/tests/variants` rather than taken from Mesa's headers:

```bash
VULKAN_HEADERS=<Vulkan-Headers checkout> bash layer/tests/build-probe.sh /tmp/probe-out
bash layer/tests/variants/build.sh /tmp/variants
mkdir -p /tmp/exported && /tmp/probe-out/probe export /tmp/exported /tmp/variants/*.spv
python3 layer/tests/layout_probe.py /tmp/exported
python3 layer/tests/layout_bytes.py /tmp/exported <pd_size> 0 <pd_size>
```

If you change the prologue instruction sequence in `layer/tools/prologue_asm.py`, regenerate `layer/src/prologue_table.inc` using `libiga64.so.2` from the IGC bundle:

```bash
IGC=<package>/igc/igc python3 layer/tools/gen_prologue_table.py layer/src/prologue_table.inc
```

---

## Building the Driver (`libvulkan_intel.so`) and Bundled Dependencies

### 1. The Forwarding ICD (`lib/libvulkan_intel.so`)

The package carries one driver per Mesa version in `lib/mesa-<version>/libvulkan_intel.so`. `lib/libvulkan_intel.so`—the file that the session's `VK_DRIVER_FILES` (`run/intel_icd.json`) points to—is a small forwarding ICD built from `icd/xmx-icd.c`. It loads the driver selected by `MESA`, or else the version named in `lib/active`, or else any driver in `lib/mesa-*/` that loads, and forwards the three Vulkan ICD entry points:

```bash
gcc -O2 -shared -fPIC -fvisibility=hidden -o lib/libvulkan_intel.so icd/xmx-icd.c -ldl -lpthread
```

### 2. Bundled Dependencies (`lib/bundled/`)

`tools/build-deps.sh` builds the two libraries the release archive bundles in `lib/bundled/` for distributions that lack them or ship older sonames: `libSPIRV-Tools.so` (tag `vulkan-sdk-1.4.350.1`, commit `0539c81f69a3daeb706fd3477dca61435b475156`, with `SPIRV-Headers` commit `ad9184e76a66b1001c29db9b0a3e87f646c64de0`) and `libdisplay-info.so.3` (tag `0.3.0`, commit `47a5590e9c4eb35d67651b8c05a55f1a48259329`). Build them in a Debian 12 (`bookworm`, glibc 2.36) container so they run on any distribution with glibc >= 2.36 (the drivers themselves require glibc >= 2.38):

```bash
docker run --rm -v "$PWD":/src -w /src debian:bookworm tools/build-deps.sh lib/bundled
```

### 3. Rebuilding a Patched Driver (`tools/rebuild-driver.sh`)

`tools/rebuild-driver.sh` automates the entire Mesa ANV build for your system's Mesa version (for example after a SteamOS update, or on Debian 13 / Ubuntu 24.04 where `libwayland-client` is 1.23; see [../drivers.md](../drivers.md)):
1. Downloads the release tarball from `archive.mesa3d.org` and checks its SHA-256 (`26.1.2`: `bac2bca9121897a2b8162e79636b50ac998fca799c8e6cf914edd85962babdf0`, `26.3.0`: `91d729aa68bb89e4ec857841300bbe922bcb3e52b1f97566ca83930d3b28343a`; for another version, pass `XMX_MESA_SHA256=<sha256>`).
2. Applies the patches (`patches/mesa-26.3/` for 26.3, `patches/` otherwise). If the patches do not apply cleanly to the new version, nothing is changed and the script exits.
3. Builds `libvulkan_intel.so` (caching the source and build tree under `${XMX_MESA_DIR:-~/.cache/xess-xmx/mesa-src}/<version>`), checks it with `tools/session-check.sh --dry-run`, installs it to `lib/mesa-<version>/libvulkan_intel.so`, and makes it the default driver via `tools/switch-driver.sh`.

SteamOS has no compiler toolchain, so run it inside an Arch Linux distrobox container with the Mesa build dependencies installed:

```bash
distrobox enter <box> -- /path/to/package/tools/rebuild-driver.sh             # the system's Mesa version
distrobox enter <box> -- /path/to/package/tools/rebuild-driver.sh 26.3.0      # or a given one
```

### 4. Building Mesa ANV by Hand

There are two patch series: `patches/` for Mesa `26.1.2` and `patches/mesa-26.3/` for Mesa `26.3.0` (first contributed in #19 for rc1); other versions require rebasing. Both build from the release tarballs on `archive.mesa3d.org` rather than git tags, because upstream deleted the `mesa-26.3.0-rc1` tag and its commit when `staging/26.3` was re-branched.

To build patched Mesa `26.1.2` by hand (for `26.3.0`, use `mesa-26.3.0.tar.xz` and `../patches/mesa-26.3/0*.patch` with the same Meson options; the unpatched `lib/mesa-stock-26.3.0/` driver is the same `26.3.0` tarball built with the same Meson options and no patches):

```bash
mkdir mesa && curl -sfL https://archive.mesa3d.org/mesa-26.1.2.tar.xz | tar -xJ -C mesa --strip-components=1
cd mesa && git init -q && git add -A && git commit -qm "mesa 26.1.2"
git am ../patches/0001-anv-cm-kernel-injection.patch          # the feature
git am ../patches/0002-anv-cm-debug-tools.patch               # optional: ANV_CM_VIEW/GCAP/CAPTURE/... (see debugging.md)
git am ../patches/0003-anv-cm-xe2-large-grf.patch             # Xe-HPG, Xe2: needed; Xe3: no effect (applies with or without 0002)
git am ../patches/0005-anv-cm-sampler-descriptor-table.patch  # kernels whose sampler is a descriptor table (FSR 4 on XMX)
meson setup build -Dprefix=/usr -Dsysconfdir=/etc -Dbuildtype=release -Dvulkan-drivers=intel -Dgallium-drivers= \
  -Dglx=disabled -Dgbm=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=enabled \
  -Dintel-rt=enabled -Dvideo-codecs= -Dvulkan-layers= -Dtools= '-Dcpp_link_args=-Wl,-rpath,$ORIGIN/../deps'
ninja -C build src/intel/vulkan/libvulkan_intel.so
```

Three of the Meson options are easy to get wrong:

* `'-Dcpp_link_args=-Wl,-rpath,$ORIGIN/../deps'` (use single quotes so the shell keeps `$ORIGIN`): lets the driver find `libSPIRV-Tools.so` and `libdisplay-info.so.3` in the package's `lib/deps/` inside the Steam Linux Runtime container, where `tools/session-check.sh` places them on systems that lack them. Without this, the driver loads on the host but can fail inside the game's container. Check that `readelf -d libvulkan_intel.so | grep RUNPATH` shows `$ORIGIN/../deps`.
* `-Dprefix=/usr -Dsysconfdir=/etc`, even though nothing is installed system-wide: the driver reads Mesa's per-game workarounds from `<prefix>/share/drirc.d`, and with Meson's default `/usr/local` it runs without them (Wuthering Waves with ray tracing then hangs the GPU within a minute).
* `-Dintel-rt=enabled`: without it, the driver exposes no Vulkan ray tracing extensions and games hide their ray tracing options.

---

## What the Mesa Patches Change

### `0001-anv-cm-kernel-injection.patch` (26.1.2 and 26.3.0)

Most of the code lives in three new files under `src/intel/vulkan` that are byte-for-byte identical between both patch series; upstream files only get small hooks:

* `anv_cm.c` / `anv_cm.h` (all generations): the environment block (`struct anv_cm_env`), recognition of the shim's placeholder shaders (workgroup size, stored magic value `0x584D5843`, and the 64-bit kernel hash next to it, plus the legacy ID placeholders of older shims), the kernel cache `~/.cache/xess-xmx/kernels`, runtime compilation (with background prefetch workers and timeouts) and its retry logic (`anv_cm_retry_failed`), the C zebin-to-`CMK2` packer (`anv_cm_pack`), and filling `brw_cs_prog_data` from the `.cmk` header instead of compiling the NIR. Hooked in `anv_shader_compile.c` and `anv_device.c`.
* `genX_cm.c` (per hardware generation, in `anv_per_hw_ver_files`): the per-dispatch binding table built from the vkd3d-proton descriptor heap (using the static sampler of the pipeline layout, or null surfaces when the heap is not bound) and the NEO-style walker indirect payload (cross-thread data, per-thread packed local IDs). Hooked in `genX_cmd_buffer.c`, `genX_shader.c`, and `genX_cmd_compute.c`.
* `anv_private.h`: defines `anv_cm_bind_state` and the `ANV_CM_*` macros for the few internal names that differ between Mesa 26.1 and 26.3, so `anv_cm.c`, `anv_cm.h`, and `genX_cm.c` need no `#if` on the Mesa version.
* `vk_pipeline.c` / `vk_shader.h`: adds a `no_cache` flag so placeholders are never stored in a Vulkan pipeline cache.
* `brw_compiler.h`, `anv_private.h`, `anv_shader.c`: adds the CM fields to `brw_cs_prog_data` and `anv_shader`.

In the `patches/mesa-26.3/` series, `0001` also initializes `device->general_state_pool` on the direct-descriptor path (where Mesa 26.3 stopped creating it, and pins it in `i915/anv_batch_chain.c`), and calls `anv_cm_kernel_replace` in `anv_shader_compile` before the Jay/brw compiler branch.

### `0002-anv-cm-debug-tools.patch` (26.1.2 and 26.3.0)

Adds the developer debug switches (`ANV_CM_PC`, `ANV_CM_VIEW`, `ANV_CM_GCAP`, `ANV_CM_CAPTURE`, `ANV_CM_ONLY`, `ANV_CM_SKIP`, `ANV_CM_SYNC`, `ANV_CM_MAXGROUPS`, `ANV_CM_PREEMPT`, `ANV_CM_CAP_DIR`, and `<blake3>.cmk` shader replacement) documented in [debugging.md](debugging.md).

`0001` and `0002` are generated from a single Mesa source tree where the debug-only code sits between `/* XMX-DEBUG-BEGIN */` and `/* XMX-DEBUG-END */` lines:

```bash
python3 tools/split_patch.py <mesa tree> mesa-26.1.2 <out dir>
```

`tools/split_patch.py` writes `0001` with those blocks stripped and `0002` with them included (untracked files under `src/` such as `anv_cm.c` are picked up automatically), then restores the marked working tree. CI checks that `anv_cm.c`, `anv_cm.h`, and `genX_cm.c` are identical across both patch series and that no debug symbols end up in `0001`.

### `0003-anv-cm-xe2-large-grf.patch` (26.1.2 and 26.3.0)

Contributed in #16 for Xe2, and extended to Xe-HPG in #31, #32, and #39. Switches `STATE_COMPUTE_MODE` Large GRF Mode on around 256-GRF CM kernels on Xe-HPG and Xe2, and switches it off before non-CM compute walkers, Mesa's own BLORP compute work (`blorp_exec_on_compute`), ray tracing (`cmd_buffer_trace_rays`), internal simple shaders (`genX_simple_shader.c`), device-generated commands (`genX_cmd_dgc.c` on 26.3), at the end of command buffers, and before executing secondary command buffers (see [how-it-works.md](how-it-works.md)). It also drops direct CM dispatches with a zero workgroup count in any dimension, which the Vulkan specification makes a no-op, on every generation. Applies cleanly with or without `0002`.

*(There is no `0004` patch any more. It was a Mesa bug fix for Resident Evil Requiem and its demo, Pragmata, and Monster Hunter Stories 3: Mesa's `drirc` enabled `anv_emulate_divergent_barriers` for them, a lowering that only works with `brw`, causing a crash at startup in `jay_opt_propagate_forwards` when Jay was active. Mesa 26.3's `drirc` now pairs `anv_emulate_divergent_barriers` with `anv_disable_jay` for those games, and SteamOS's 26.1.2 `drirc` has no ANV entries for them, so the patch was dropped with 26.3.0.)*

### `0005-anv-cm-sampler-descriptor-table.patch` (26.1.2 and 26.3.0)

Handles CM kernels whose sampler is passed in a D3D12 descriptor table rather than as a static sampler, as used by the FSR 4 on XMX plugin ([../games.md](../games.md)). vkd3d-proton puts one push-constant dword per descriptor table at the end of the push constants, and that sampler table is the last root parameter, so the surface tables end one dword earlier; the sampler state is copied from vkd3d-proton's sampler heap at dispatch time. XeSS uses static samplers and does not take this path.

### `0006-anv-cmat-vars-before-jay.patch` (26.3.0 only)

Fixes an upstream Mesa 26.3 bug (not part of the XMX feature). Cooperative matrix lowering (`brw_nir_lower_cmat`) leaves `function_temp` variables behind (in a compute shader of Cyberpunk 2077, dead stores into `ivec8` vectors) and runs no variable-to-SSA or dead-variable pass afterward. When those derefs reach Jay, `jay_emit_instr` hits `UNREACHABLE("All derefs should've been lowered")`. In a release build (`__builtin_unreachable`), execution jumps into whatever function the compiler placed next (`jay_emit_fb_write` when built with GCC 15; GCC 16 happened not to crash), crashing the game while compiling the shader. Patch `0006` runs `nir_lower_vars_to_ssa`, `nir_opt_dead_write_vars`, `nir_remove_dead_variables`, and `nir_opt_dce` right after `brw_nir_lower_cmat`.

---

## Packaging a Release Archive (`tools/make-release.py`)

With all binaries built and placed in their release locations in the working tree (`igdext64.dll`, `ControlLib.dll`, `ffxfg/*.dll`, `dx11bridge/libxess_dx11.dll`, `vkwrap/vulkan-1.dll`, `lib/libvulkan_intel.so`, `lib/active`, `lib/mesa-*/libvulkan_intel.so`, `lib/bundled/*`, and `layer/libVkLayer_xess_xmx.so`), build the release zip archive:

```bash
python3 tools/make-release.py <version> [out dir]
```

`tools/make-release.py` checks that every required file exists and verifies signature strings inside each binary (preventing stock or third-party DLLs from being packed by mistake), excludes developer-only scripts (`gen_placeholders.py`, `check_placeholders.py`, `build-deps.sh`, `split_patch.py`), sets executable permissions (`0755`) on `.sh` and `.py` files, and writes `release/xess-xmx-linux-<version>.zip`.
