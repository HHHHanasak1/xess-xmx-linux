# Building from source

The release archive contains binaries built from this repository:

* the shim `igdext64.dll` (a translator library that answers Intel driver queries)
* `ControlLib.dll`
* the FSR DLLs in `ffxfg/`
* the D3D11 bridge `dx11bridge/libxess_dx11.dll` and Vulkan wrapper `vkwrap/vulkan-1.dll`
* the patched Mesa ANV drivers `lib/mesa-<version>/libvulkan_intel.so` (ANV is Mesa's Intel Vulkan driver) and the unpatched driver `lib/mesa-stock-26.3.0/libvulkan_intel.so`
* the small ICD (Installable Client Driver, the entry point the Vulkan loader reads) `lib/libvulkan_intel.so` that loads one of the drivers
* the Vulkan layer `layer/libVkLayer_xess_xmx.so` for an unpatched ANV (Xe3's default session driver and `xmx-stock.sh`) and XeSS' Vulkan path

The kernels are compiled on the user's machine (see [how-it-works.md](how-it-works.md)). CI (`.github/workflows/build.yml`) builds all of these binaries on every push and keeps them as workflow artifacts. Build a release archive from the artifacts of the release commit with `tools/make-release.py`.

## Repository layout

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
dx11bridge/                   libxess_dx11.dll source: runs D3D12 XeSS next to a D3D11 game's device (XESS=latest)
vkwrap/                       vulkan-1.dll source: adds the Intel interfaces XeSS' Vulkan path looks for (XESS=latest)
tools/cm-compile.sh           run-time compiler helper called by the driver
tools/get-igc.sh              fetches IGC 2.10.10 + ocloc 25.18 into igc/
tools/get-xess.sh             fetches Intel's XeSS SDK libraries into xess/ (XESS=latest)
tools/get-ffx.sh              fetches AMD's FidelityFX loader and providers into ffx/ (FSR / FSR_FG with FSR 3.1.0-3.1.3)
tools/proton-hook.py          XESS / XESS_LOG / FSR / FSR_FG launch options, run by Proton through user_settings.py
tools/fsr-fg.sh               FSR on XeSS for one game, for Protons without the hook
tools/steam-libraries.sh      every Steam library (libraryfolders.vdf), for the scripts
tools/rebuild-driver.sh       rebuilds the patched driver for the system's Mesa version
tools/switch-driver.sh        sets the default among the package's drivers (per game: MESA)
icd/xmx-icd.c                 lib/libvulkan_intel.so: loads the driver MESA or the default names
tools/session-check.sh        the driver check run before each session
tools/cmk_pack.py             zebin -> .cmk packer (the driver has a C port; cm-compile.sh uses this one for older drivers)
tools/xell-probe.sh           XeLL driver-mode probe (docs/dev/debugging.md)
tools/gen_placeholders.py     placeholder templates for run-time kernels;  tools/check_placeholders.py consistency check
tools/split_patch.py          writes patches 0001 / 0002 from the marked source tree
tools/build-deps.sh           builds the bundled SPIRV-Tools and libdisplay-info (lib/bundled/)
tools/make-release.py         builds the release archive (the developer tools stay out of it)
docs/                         user guides (install, drivers, games, frame generation, Steam launch options, performance,
                              troubleshooting); docs/dev/: how it works, building, debugging
.github/workflows/build.yml   CI: builds the shim, ControlLib.dll, the FSR DLLs, the D3D11/Vulkan DLLs, the ICD,
                              the drivers and the layer, checks the scripts
```

The release archive holds the binaries (`igdext64.dll`, `ControlLib.dll`, the three DLLs in `ffxfg/`, `dx11bridge/libxess_dx11.dll`, `vkwrap/vulkan-1.dll`, the patched drivers in `lib/mesa-<version>/`, the unpatched driver in `lib/mesa-stock-26.3.0/`, the ICD `lib/libvulkan_intel.so`, the layer `layer/libVkLayer_xess_xmx.so`) and the scripts. It contains no Intel kernels; they are compiled on your machine from the SPIR-V inside the game's XeSS libraries.

## The shim (`igdext64.dll`) and `ControlLib.dll`

Build the shim on Windows with the Visual Studio 2022 Build Tools (C++ workload):

```bat
shim\build.bat            -> shim\build\igdext64.dll
```

Or run CMake from any shell with the MSVC environment loaded:

```bash
cmake -S shim -B shim/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build shim/build
```

The DLL links against the static MSVC runtime and imports only `kernel32` and `advapi32`. A dynamically linked build crashed in `DllMain` in a prefix with an older `msvcp140.dll` (Resident Evil 4), and XeSS silently fell back to DP4a (the generic non-XMX instruction path). `shim/toolchain-mingw64.cmake` is kept from dxvk-igdext for a mingw build; it is not used for releases.

Generate the two placeholder templates rather than writing them by hand. This requires `fxc.exe` (the DirectX shader compiler) from the Windows SDK. Under WSL, pass the Windows `fxc.exe` as the second argument:

```bash
python tools/gen_placeholders.py shim/src/dll/placeholders.inc                     # kernel and self-test templates
python tools/check_placeholders.py patches/0001-anv-cm-kernel-injection.patch      # they match the driver
python tools/check_placeholders.py patches/mesa-26.3/0001-anv-cm-kernel-injection.patch
```

Build `ControlLib.dll` (the stand-in for Intel's Control Library that `XESS_FG` needs) the same way:

```bat
controllib\build.bat      -> controllib\build\ControlLib.dll
```

## The FSR DLLs (`ffxfg/amd_fidelityfx_upscaler_dx12.dll`, `ffxfg/amd_fidelityfx_framegeneration_dx12.dll`, `ffxfg/dxgi.dll`)

The headers these DLLs build against (AMD FSR SDK 2.4.0 and Intel XeSS SDK) are not in the repository. Run `get-headers.sh` to fetch them into `ffxfg/inc/` and check each one by SHA-256. Then build on Windows with the Visual Studio 2022 Build Tools:

```bash
bash ffxfg/get-headers.sh
ffxfg\build.bat           -> ffxfg\build\amd_fidelityfx_upscaler_dx12.dll, amd_fidelityfx_framegeneration_dx12.dll, dxgi.dll
```

Set `FFXFG_DEBUG=1` in the environment of `build.bat` to compile the debug switches (`FFX_XEFG_CAMERA`, `FFX_XEFG_JITTER`, `FFX_XEFG_RENDERTIME`, `FFX_XEFG_RENDERSCALE`, `FFX_XEFG_DUMP`, `FFX_XEFG_ORDER`, `FFX_XESS_JITTER`) into the DLLs.

The release archive keeps the built DLLs in `ffxfg/` (see [../frame-generation.md](../frame-generation.md) and [how-it-works.md](how-it-works.md)). CI builds them next to the shim.

## XeSS for D3D11 (`dx11bridge/libxess_dx11.dll`) and Vulkan (`vkwrap/vulkan-1.dll`)

Both DLLs need the XeSS headers from `ffxfg/get-headers.sh`. Build them on Windows with the Visual Studio 2022 Build Tools and Python (`gen.py` writes the forwarding stubs of `vulkan-1.dll` from `vkwrap/exports.txt`, the export list of Proton's `vulkan-1.dll`):

```bash
bash ffxfg/get-headers.sh
dx11bridge\build.bat                          -> dx11bridge\build\libxess_dx11.dll
python vkwrap\gen.py vkwrap\exports.txt vkwrap\build
vkwrap\build.bat                              -> vkwrap\build\vulkan-1.dll
```

The package keeps them in `dx11bridge/` and `vkwrap/` (see [../games.md](../games.md#xess-in-a-d3d11-game)).

To build the tests:

* Run `dx11bridge\test\build.bat` to build `sharetest.exe` (a texture and a fence shared by a D3D11 and a D3D12 device) and `bridgetest.exe` (the bridge with depth, output checked).
* Set `VULKAN_HEADERS` to a Vulkan-Headers checkout and run `vkwrap\test\build.bat` to build `vktest.exe` (XeSS' Vulkan path through `vulkan-1.dll` and the layer, output checked).

Run each test under Proton with the corresponding DLL next to it. Each test writes `<name>.txt` next to itself.

## The layer (`layer/libVkLayer_xess_xmx.so`)

Build the layer on Linux with `gcc`, `glslangValidator` (a GLSL-to-SPIR-V shader compiler), and `python3` against Vulkan-Headers 1.4.350. The distribution's own headers may lack `VK_KHR_pipeline_binary`, which the headers must define:

```bash
VULKAN_HEADERS=<Vulkan-Headers checkout> layer/build.sh out   # default out/ -> out/libVkLayer_xess_xmx.so, out/VkLayer_xess_xmx.json
layer/tests/build.sh                                          # unit tests
```

From inside `layer/`, run `VULKAN_HEADERS=<Vulkan-Headers checkout> ./build.sh [out dir]` and `./tests/build.sh`.

For a release archive, place the CI artifact `xmx-layer` into `layer/` (the manifest there is the repository's own).

## The driver (`libvulkan_intel.so`)

The package carries one patched driver per Mesa version in `lib/mesa-<version>/`. The file the session's Vulkan driver list points at, `lib/libvulkan_intel.so`, is a small ICD built from `icd/xmx-icd.c`. It loads one of the drivers (`MESA`, else the version in `lib/active`, else any that loads) and passes the loader its three entry points.

Build the ICD with:

```bash
gcc -O2 -shared -fPIC -fvisibility=hidden -o lib/libvulkan_intel.so icd/xmx-icd.c -ldl -lpthread
```

### Rebuilding with `tools/rebuild-driver.sh`

Run `tools/rebuild-driver.sh` to automate the driver build for the system's Mesa version (for example, after a SteamOS update). The script:

1. Downloads the release tarball from `archive.mesa3d.org`.
2. Applies the patches (`patches/mesa-26.3/` for 26.3, `patches/` otherwise).
3. Builds the driver and checks the result the same way the session check does.
4. Installs it as `lib/mesa-<version>/libvulkan_intel.so` and makes it the default driver (`tools/switch-driver.sh`, see [../drivers.md](../drivers.md)).

If the patches do not apply to the new version, the script changes nothing and tells you. SteamOS has no compiler, so run the script in an Arch Linux distrobox (a container that shares your home directory) with the Mesa build dependencies installed:

```bash
distrobox enter <box> -- /path/to/package/tools/rebuild-driver.sh             # the system's Mesa version
distrobox enter <box> -- /path/to/package/tools/rebuild-driver.sh 26.3.0      # or a given one
```

The unpatched 26.3.0 driver in the archive (`lib/mesa-stock-26.3.0/`, Xe3's default with the layer) is the same tarball built with the same options and no patches (CI job `mesa stock-26.3.0`).

### Building by hand

There are two patch series: `patches/` for Mesa 26.1.2 and `patches/mesa-26.3/` for Mesa 26.3.0 (first contributed in #19 for rc1). Other versions need a rebase. Build both from the release tarballs rather than git tags: upstream deleted the `mesa-26.3.0-rc1` tag and its commit when `staging/26.3` was branched again.

To build Mesa 26.1.2 by hand (for 26.3, use the 26.3.0 tarball and `patches/mesa-26.3/` with the same meson options):

```bash
mkdir mesa && curl -sfL https://archive.mesa3d.org/mesa-26.1.2.tar.xz | tar -xJ -C mesa --strip-components=1
cd mesa && git init -q && git add -A && git commit -qm "mesa 26.1.2"
git am ../patches/0001-anv-cm-kernel-injection.patch      # the feature
git am ../patches/0002-anv-cm-debug-tools.patch           # optional: ANV_CM_VIEW/GCAP/CAPTURE/... (see debugging.md)
git am ../patches/0003-anv-cm-xe2-large-grf.patch         # Xe-HPG, Xe2: needed; Xe3: no effect (applies with or without 0002)
git am ../patches/0005-anv-cm-sampler-descriptor-table.patch  # kernels whose sampler is a descriptor table (FSR 4 on XMX)
meson setup build -Dprefix=/usr -Dsysconfdir=/etc -Dbuildtype=release -Dvulkan-drivers=intel -Dgallium-drivers= \
  -Dglx=disabled -Dgbm=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=enabled \
  -Dintel-rt=enabled -Dvideo-codecs= -Dvulkan-layers= -Dtools= '-Dcpp_link_args=-Wl,-rpath,$ORIGIN/../deps'
ninja -C build src/intel/vulkan/libvulkan_intel.so
```

Pay close attention to three of these meson options:

* `'-Dcpp_link_args=-Wl,-rpath,$ORIGIN/../deps'` (use single quotes so the shell keeps `$ORIGIN`): this lets the driver find SPIRV-Tools and libdisplay-info in the package's `lib/deps/` inside the Steam Linux Runtime, where the session check puts them on systems that lack them. Without this option, the driver loads on the host but can fail in the game's container. Run `readelf -d libvulkan_intel.so | grep RUNPATH` to verify that it shows `$ORIGIN/../deps`.
* `-Dprefix=/usr -Dsysconfdir=/etc`: pass these even though nothing is installed to the system. The driver reads Mesa's per-game workarounds from `<prefix>/share/drirc.d`. With meson's default `/usr/local`, the driver runs without those workarounds (Wuthering Waves with ray tracing then hangs the GPU within a minute).
* `-Dintel-rt=enabled`: without this option, the driver exposes no ray tracing extensions and games hide their ray tracing options.

### What the patches change

#### `0001` (kernel injection and run-time compilation)

This patch touches mostly `src/intel/vulkan`, plus a small hook in the common Vulkan runtime. The code lives in three new files that are identical in both patch series, while the upstream files only get small hooks:

* `anv_cm.c` / `anv_cm.h` (all generations): holds the environment block (`struct anv_cm_env`), recognition of the shim's placeholder shaders (their workgroup size, the stored magic value `0x584D5843`, and the kernel hash next to it, as well as the id placeholders of older shims), the kernel cache `~/.cache/xess-xmx/kernels`, the run-time compile and its retry, and filling `brw_cs_prog_data` from the `.cmk` header instead of compiling the NIR (Mesa's intermediate shader representation). Hooks into `anv_shader_compile.c` (and `anv_device.c` for the retry).
* `genX_cm.c` (per generation, in `anv_per_hw_ver_files`): builds the per-dispatch binding table from the vkd3d-proton descriptor heap (using the static sampler of the pipeline layout, or null surfaces when the heap is not bound) and the NEO-style walker payload (cross-thread data, per-thread local ids). Hooks into `genX_cmd_buffer.c`, `genX_shader.c`, and `genX_cmd_compute.c`.
* `anv_private.h`: defines the few names that differ between the Mesa versions (`anv_cm_bind_state`, `ANV_CM_*` macros), so that the three files need no `#if` on the Mesa version.
* `vk_pipeline.c` / `vk_shader.h`: adds a `no_cache` flag so placeholders are never stored in a pipeline cache.
* `brw_compiler.h`, `anv_private.h`, `anv_shader.c`: adds the new fields.

In the 26.3 series, `0001` also creates the general state pool (which holds the walker payload) on the direct-descriptor path where 26.3 no longer creates it, and replaces the placeholder before the Jay/brw compiler branch.

#### `0002` (debug tools)

Adds the debug switches listed in [debugging.md](debugging.md).

`0001` and `0002` are generated from a single source tree where the debug-only code sits between `/* XMX-DEBUG-BEGIN */` and `/* XMX-DEBUG-END */` lines. Run:

```bash
tools/split_patch.py <mesa tree> mesa-26.1.2 <out dir>
```

This writes `0001` without those blocks and `0002` with them (new files such as `anv_cm.c` may stay untracked in that tree; the script picks them up). `0003`, `0005`, and `0006` are single commits on top, rebased when `0001` changes. CI checks that no debug code ends up in `0001`.

#### `0003` (Large GRF Mode)

Contributed in #16 for Xe2, extended to Xe-HPG in #31 and #32, and updated in #39. It switches Large GRF Mode (256 registers per thread instead of 128) around CM kernels on Xe-HPG and Xe2, including switching it off before Mesa's own BLORP (blit and copy) compute work, trace rays, internal shaders, and generated commands (see [how-it-works.md](how-it-works.md)). It also drops CM dispatches with a zero group count, which the Vulkan specification makes a no-op, on every generation. The switch itself lives in `genX_cm.c`; the other files get one call each.

#### `0004` (removed)

There is no `0004` any more. It was a Mesa bug fix for Resident Evil Requiem and its demo, Pragmata, and Monster Hunter Stories 3. Mesa's `drirc` turns on `anv_emulate_divergent_barriers` for them, a lowering pass that only works with the old `brw` compiler, so with Jay (the default compiler on Xe2 and Xe3) they crashed at start in `jay_opt_propagate_forwards`. Mesa 26.3's `drirc` now pairs that option with `anv_disable_jay` for the same games, which compiles them with `brw`. A `drirc` without either option (SteamOS ships 26.1.2's, which has no ANV entries for them) never turns the lowering on. The patch was dropped with 26.3.0.

#### `0005` (sampler descriptor tables)

In both series, `0005` handles CM kernels whose sampler is a descriptor table rather than a static sampler, as in the FSR 4 on XMX plugin (see [../games.md](../games.md)). vkd3d puts one push-constant dword per descriptor table at the end of the push constants, and that sampler table is the last root parameter, so the surface tables end one dword earlier. The driver copies the sampler state from vkd3d's sampler heap. XeSS uses static samplers and does not take this path.

#### `0006` (26.3 only: Jay cooperative matrix fix)

`0006` is a Mesa bug fix, not part of the XMX feature. The cooperative matrix lowering leaves `function_temp` variables behind (in a compute shader of Cyberpunk 2077, dead stores into its vectors) and runs nothing that removes them. Those variable dereferences reach Jay, which has no case for them: an `UNREACHABLE` macro that a release build turns into a jump into unrelated code. Whether that crashes depends on the build (26.3.0-rc3 with this series built with GCC 15 crashes in `jay_emit_fb_write` while Cyberpunk 2077 compiles the shader; GCC 16 happened not to). An assert build stops at `All derefs should've been lowered`. The patch lowers them to SSA (Static Single Assignment values) and removes the dead ones right after the lowering.
