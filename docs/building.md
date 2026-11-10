# Building from source

The release archive contains binaries built from this repository: the shim `igdext64.dll`, `ControlLib.dll`, the FSR
DLLs in `ffxfg/`, the patched Mesa ANV drivers `lib/mesa-<version>/libvulkan_intel.so` and the small ICD
`lib/libvulkan_intel.so` that loads one of them, and the Vulkan layer `layer/libVkLayer_xess_xmx.so` for the system's
own driver (`xmx-stock.sh`).
The kernels are compiled on the user's machine (see [how-it-works.md](how-it-works.md)). CI
(`.github/workflows/build.yml`) builds all of them on every push and keeps them as workflow artifacts; a release
archive is made with `tools/make-release.py` from the artifacts of the release commit.

## The shim (`igdext64.dll`)

Windows with the Visual Studio 2022 Build Tools (C++ workload):

    shim\build.bat            -> shim\build\igdext64.dll

or any shell with the MSVC environment loaded:

    cmake -S shim -B shim/build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build shim/build

The DLL is linked against the static MSVC runtime and imports only `kernel32`/`advapi32`: a dynamically linked build
crashed in `DllMain` in a prefix with an older `msvcp140.dll` (Resident Evil 4), and XeSS silently fell back to DP4a.
`shim/toolchain-mingw64.cmake` is kept from dxvk-igdext for a mingw build; it is not used for releases.

The two placeholder templates are generated, not written by hand (needs `fxc.exe` from the Windows SDK; under WSL pass
the Windows `fxc.exe` as the second argument):

    python tools/gen_placeholders.py shim/src/dll/placeholders.inc                     # kernel and self-test templates
    python tools/check_placeholders.py patches/0001-anv-cm-kernel-injection.patch      # they match the driver
    python tools/check_placeholders.py patches/mesa-26.3/0001-anv-cm-kernel-injection.patch

`ControlLib.dll` (the stand-in for Intel's Control Library that `XESS_FG` above 4x needs) is built the same way:

    controllib\build.bat      -> controllib\build\ControlLib.dll

## The FSR DLLs (`ffxfg/amd_fidelityfx_upscaler_dx12.dll`, `ffxfg/amd_fidelityfx_framegeneration_dx12.dll`, `ffxfg/dxgi.dll`)

The headers they are built against (AMD FSR SDK 2.4.0, Intel XeSS SDK) are not in the repository; `get-headers.sh`
fetches them into `ffxfg/inc/` and checks each one by SHA-256. Then, on Windows with the same Build Tools:

    bash ffxfg/get-headers.sh
    ffxfg\build.bat           -> ffxfg\build\amd_fidelityfx_upscaler_dx12.dll, amd_fidelityfx_framegeneration_dx12.dll, dxgi.dll

The package keeps them in `ffxfg/` ([fsr-frame-generation.md](fsr-frame-generation.md)).

## XeSS for D3D11 (`dx11bridge/libxess_dx11.dll`) and Vulkan (`vkwrap/vulkan-1.dll`)

Both need the XeSS headers of `ffxfg/get-headers.sh`; on Windows with the same Build Tools (and a Python for `gen.py`,
which writes the forwarding stubs of `vulkan-1.dll` from `vkwrap/exports.txt`, the export list of Proton's
`vulkan-1.dll`):

    bash ffxfg/get-headers.sh
    dx11bridge\build.bat                          -> dx11bridge\build\libxess_dx11.dll
    python vkwrap\gen.py vkwrap\exports.txt vkwrap\build
    vkwrap\build.bat                              -> vkwrap\build\vulkan-1.dll

The package keeps them in `dx11bridge/` and `vkwrap/` ([games.md](games.md#xess-in-a-d3d11-game)). Tests, run under
Proton with the DLL next to them: `dx11bridge\test\build.bat` (`sharetest.exe`: a texture and a fence shared by a D3D11
and a D3D12 device; `bridgetest.exe`: the bridge with depth, output checked) and `vkwrap\test\build.bat` with
`VULKAN_HEADERS` set to a Vulkan-Headers checkout (`vktest.exe`: XeSS' Vulkan path through `vulkan-1.dll` and the layer,
output checked). Each writes `<name>.txt` next to itself.

## The layer (`layer/libVkLayer_xess_xmx.so`)

Linux with gcc, `glslangValidator` and python3, against Vulkan-Headers 1.4.350 (the distribution's headers may lack
`VK_KHR_pipeline_binary`):

    VULKAN_HEADERS=<Vulkan-Headers checkout> layer/build.sh out   # -> out/libVkLayer_xess_xmx.so, out/VkLayer_xess_xmx.json
    layer/tests/build.sh                                          # unit tests

For a release archive, the CI artifact `xmx-layer` goes into `layer/` (the manifest there is the repository's own).

## The driver (`libvulkan_intel.so`)

The package carries one patched driver per Mesa version in `lib/mesa-<version>/`. `lib/libvulkan_intel.so`, the
file the session's Vulkan driver list points at, is a small ICD built from `icd/xmx-icd.c` that loads one of them
(`MESA`, else the version in `lib/active`, else any that loads) and passes the loader its three entry points:

    gcc -O2 -shared -fPIC -fvisibility=hidden -o lib/libvulkan_intel.so icd/xmx-icd.c -ldl -lpthread

`tools/rebuild-driver.sh` does everything below for the system's Mesa version, for example after a SteamOS update:
download the release tarball from archive.mesa3d.org, apply the patches (`patches/mesa-26.3/` for 26.3, `patches/`
otherwise), build, check the result like the session check does, install it as `lib/mesa-<version>/libvulkan_intel.so`
and make it the default driver (`tools/switch-driver.sh`, see the README). If the patches do not apply to the new version, nothing is changed and the
script says so. SteamOS has no compiler, so run it in an Arch Linux distrobox with the Mesa build dependencies:

    distrobox enter <box> -- /path/to/package/tools/rebuild-driver.sh             # the system's Mesa version
    distrobox enter <box> -- /path/to/package/tools/rebuild-driver.sh 26.3.0      # or a given one

There are two patch series: `patches/` for Mesa 26.1.2 and `patches/mesa-26.3/` for Mesa 26.3.0 (first
contributed in #19, for rc1); other versions need a rebase. Both are built from the release tarballs, not the git tags: upstream
deleted the `mesa-26.3.0-rc1` tag and its commit when `staging/26.3` was branched again. By hand, for 26.1.2 (for
26.3: that tarball and that folder, the same meson options):

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

Three of the options are easy to get wrong:

* `-Dcpp_link_args=-Wl,-rpath,$ORIGIN/../deps` (single quotes, so the shell keeps `$ORIGIN`): the driver then finds
  SPIRV-Tools and libdisplay-info in the package's `lib/deps/` inside the Steam Linux Runtime, where the session check
  puts them on systems that lack them. Without it the driver loads on the host but can fail in the game's container.
  `readelf -d libvulkan_intel.so | grep RUNPATH` shows `$ORIGIN/../deps`.

* `-Dprefix=/usr -Dsysconfdir=/etc`, even though nothing is installed: the driver reads Mesa's per-game workarounds
  from `<prefix>/share/drirc.d`, and with meson's default `/usr/local` it runs without them (Wuthering Waves with ray
  tracing then hangs the GPU within a minute).
* `-Dintel-rt=enabled`: without it the driver exposes no ray tracing extensions and games hide their ray tracing
  options.

### What the patches change

`0001` (mostly under `src/intel/vulkan`, plus a small hook in the common Vulkan runtime). The code lives in three
new files, which are the same in both series; the upstream files only get small hooks:

* `anv_cm.c` / `anv_cm.h` (all generations): the environment block (`struct anv_cm_env`), recognition of the shim's
  placeholder shaders (their workgroup size, the stored magic value `0x584D5843` and the kernel hash next to it; the id
  placeholders of older shims too), the kernel cache
  `~/.cache/xess-xmx/kernels`, the run-time compile and its retry, and filling `brw_cs_prog_data` from the `.cmk`
  header instead of compiling the NIR. Hook: `anv_shader_compile.c` (and `anv_device.c` for the retry).
* `genX_cm.c` (per generation, in `anv_per_hw_ver_files`): the per-dispatch binding table built from the
  vkd3d-proton descriptor heap (the static sampler of the pipeline layout, null surfaces when the heap is not bound)
  and the NEO-style walker payload (cross-thread data, per-thread local ids). Hooks: `genX_cmd_buffer.c`,
  `genX_shader.c`, `genX_cmd_compute.c`.
* `anv_private.h`: the few names that differ between the Mesa versions (`anv_cm_bind_state`, `ANV_CM_*` macros), so
  that the three files need no `#if` on the Mesa version.
* `vk_pipeline.c` / `vk_shader.h`: a `no_cache` flag, so placeholders are never stored in a pipeline cache.
* `brw_compiler.h`, `anv_private.h`, `anv_shader.c`: the new fields.

`0002` adds the debug switches listed in [debugging.md](debugging.md).

`0003` (contributed in #16 for Xe2, extended to Xe-HPG in #31 and #32, and in #39) switches Large GRF Mode around CM kernels on
Xe-HPG and Xe2, including off before Mesa's own BLORP compute work, trace rays, internal shaders and generated commands
(see how-it-works.md), and drops CM dispatches with
a zero group count, which the Vulkan specification makes a no-op, on every generation. The switch itself is in
`genX_cm.c`; the other files get one call each.

The 26.3 series does the same; its `0001` also creates the general state pool (which holds the walker payload) on the
direct-descriptor path, where 26.3 no longer creates it, and replaces the placeholder before the Jay/brw branch.

There is no `0004` any more. It was a Mesa bug fix for Resident Evil Requiem and its demo, Pragmata and Monster
Hunter Stories 3: Mesa's drirc turns on `anv_emulate_divergent_barriers` for them, a lowering that only works with
brw, so with Jay (the default on Xe2 and Xe3) they crashed at start in `jay_opt_propagate_forwards`. Mesa 26.3's
drirc now pairs that option with `anv_disable_jay` for the same games, which compiles them with brw; a drirc without
either option (SteamOS ships 26.1.2's, which has no ANV entries for them) never turns the lowering on. The patch was
dropped with 26.3.0.

`0006` (26.3 only) is a Mesa bug fix, not part of the XMX feature. The cooperative matrix lowering leaves function_temp variables behind (in a
compute shader of Cyberpunk 2077, dead stores into its vectors) and runs nothing that removes them, so the derefs reach
Jay, which has no case for them: an `UNREACHABLE` that a release build turns into a jump into unrelated code. Whether
that crashes depends on the build (26.3.0-rc3 with this series built with GCC 15 crashes in `jay_emit_fb_write` while
Cyberpunk 2077 compiles the shader; GCC 16 happened not to); an assert build stops at "All derefs should've been
lowered". The patch lowers them to SSA and removes the dead ones right after the lowering.

`0005` (both series) handles CM kernels whose sampler is a descriptor table
rather than a static sampler, as in the FSR 4 on XMX plugin ([games.md](games.md)). vkd3d puts one push-constant
dword per descriptor table at the end of the push constants, and that sampler table is the last root parameter, so the
surface tables end one dword earlier; the sampler state is copied from vkd3d's sampler heap. XeSS uses static
samplers and does not take this path.

`0001` and `0002` are generated from one source tree in which the debug-only code sits between
`/* XMX-DEBUG-BEGIN */` and `/* XMX-DEBUG-END */` lines: `tools/split_patch.py <mesa tree> mesa-26.1.2 <out dir>`
writes `0001` without those blocks and `0002` with them (new files such as `anv_cm.c` may stay untracked in that
tree; the script picks them up). `0003`, `0005` and `0006` are single commits on top, rebased when `0001` changes. CI checks
that no debug code ends up in `0001`.
