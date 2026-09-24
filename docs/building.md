# Building from source

The release archive contains two binaries built from this repository: the shim `igdext64.dll` and the patched Mesa
ANV driver `lib/libvulkan_intel.so`. Kernels are not built ahead of time any more; the driver compiles them on the
user's machine (see [how-it-works.md](how-it-works.md)). CI (`.github/workflows/build.yml`) builds both binaries on every
push and keeps them as workflow artifacts. Release binaries are built the same way and then tested on the device.

## The shim (`igdext64.dll`)

Windows with the Visual Studio 2022 Build Tools (C++ workload):

    shim\build.bat            -> shim\build\igdext64.dll

or any shell with the MSVC environment loaded:

    cmake -S shim -B shim/build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build shim/build

The static kernel table (`xess_dummies.inc`, only used with `IGDEXT_STATIC=1`) is compiled in only with
`-DXMX_STATIC_TABLE=ON`.

The DLL is linked against the static MSVC runtime on purpose (see "Why a static runtime" below) and imports only
`kernel32`/`advapi32`. `shim/toolchain-mingw64.cmake` is kept from dxvk-igdext for a mingw build; it is not used for
releases.

The two placeholder tables are generated, not written by hand (both need `fxc.exe` from the Windows SDK):

    python tools/gen_dyn_dummies.py shim/src/dll/dyn_dummies.inc                       # run-time kernel ids
    python tools/gen_all_dummies.py tools/kernel_map_xess_2.0.2.68.txt shim/src/dll/xess_dummies.inc   # static ids
    python tools/check_dyn_ids.py patches/0001-anv-cm-kernel-injection.patch          # layout matches the driver

### Why a static runtime

Resident Evil 4's Proton prefix carries an older `msvcp140.dll`. A dynamically linked shim crashed inside `DllMain`
there, and XeSS silently fell back to DP4a. The only symptom was `igdext64.dll` being loaded and unloaded twice in a
`PROTON_LOG`, with no trace file.

## The driver (`libvulkan_intel.so`)

After a SteamOS update to a new Mesa version, `tools/rebuild-driver.sh` does everything below for the system's
version: fetch the tag, apply both patches, build (ray tracing enabled), check the result like the session check
does, and install it into the package (the previous driver is kept as `lib/libvulkan_intel.so.prev`). SteamOS has
no compiler, so run it in an Arch Linux distrobox with the Mesa build dependencies:

    distrobox enter <box> -- /path/to/package/tools/rebuild-driver.sh

If the patches do not apply to the new version, nothing is changed and the script says so.

The patches apply to Mesa **26.1.2** (tag `mesa-26.1.2`, the version SteamOS ships at the time of writing). The
driver must match the Mesa version of the system it runs on only loosely (it is a complete Vulkan driver), but the
patches themselves are written against 26.1.2 and need a rebase for other versions.

    git clone --branch mesa-26.1.2 https://gitlab.freedesktop.org/mesa/mesa.git && cd mesa
    git am ../patches/0001-anv-cm-kernel-injection.patch      # the feature
    git am ../patches/0002-anv-cm-debug-tools.patch           # optional: ANV_CM_VIEW/GCAP/CAPTURE/... (see debugging.md)
    meson setup build -Dprefix=/usr -Dsysconfdir=/etc -Dbuildtype=release -Dvulkan-drivers=intel -Dgallium-drivers= \
      -Dglx=disabled -Dgbm=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=enabled \
      -Dintel-rt=enabled -Dvideo-codecs= -Dvulkan-layers= -Dtools=
    ninja -C build src/intel/vulkan/libvulkan_intel.so

`-Dprefix=/usr -Dsysconfdir=/etc` matter even though nothing is installed: the driver reads Mesa's per-game workarounds
from `<prefix>/share/drirc.d`, and with meson's default `/usr/local` it silently runs without them. Wuthering Waves
with ray tracing on hangs the GPU within a minute without the vkd3d entries there (same with an unpatched Mesa
26.1.2 or 26.1.8 built that way; fine with `DRIRC_CONFIGDIR=/usr/share/drirc.d` or the right prefix).

On SteamOS the release build is made in an Arch Linux distrobox. Keep `spirv-tools` at the version of the host
(1.4.350.1 on the tested SteamOS build), since a mismatch has been suspected of subtle breakage before (it turned out not to be the
cause, but there is no reason to risk it).

### What the patches change

`0001` (mostly under `src/intel/vulkan`, plus a small hook in the common Vulkan runtime):

* `anv_shader_compile.c`: recognises the shim's placeholder shaders (workgroup size on the reserved planes and the
  stored magic value `0x584D5843`), resolves the kernel (static file or run-time compile into
  `~/.cache/xess-xmx/kernels`), and fills `brw_cs_prog_data` from the `.cmk` header instead of compiling the NIR.
* `genX_cmd_buffer.c`: per-dispatch binding table built from the vkd3d-proton descriptor heap, the static sampler of
  the pipeline layout, null surfaces when the heap is not bound.
* `genX_shader.c`, `genX_cmd_compute.c`: the NEO-style walker payload (cross-thread data, per-thread local ids).
* `vk_pipeline.c` / `vk_shader.h`: a `no_cache` flag, so placeholders are never stored in a pipeline cache.
* `brw_compiler.h`, `anv_private.h`, `anv_shader.c`: the new fields and the environment block (`struct anv_cm_env`).

`0002` adds the debug switches listed in [debugging.md](debugging.md).

The two patches are generated from one source tree in which the debug-only code sits between
`/* XMX-DEBUG-BEGIN */` and `/* XMX-DEBUG-END */` lines: `tools/split_patch.py <mesa tree> mesa-26.1.2 <out dir>`
writes `0001` without those blocks and `0002` with them. CI checks that no debug code ends up in `0001`.

`-Dintel-rt=enabled` matters: without it the driver exposes no ray tracing extensions and games hide their ray
tracing options (v1.3.1 and earlier were built that way).

## Kernels ahead of time (optional)

The run-time path makes this unnecessary. It is still possible to build a static kernel set, for example to measure
the compiler or to avoid the first-launch compile on a machine without network:

1. Run the game once with `IGDEXT_TRACE=1`. The shim writes `C:\igdext_trace.log` and dumps every kernel as
   `C:\igdext_dump\cs_NNNN_type2.bin` + `cs_NNNN_options.txt` (inside the prefix's `drive_c`).
2. `tools/s16_map.py <dump dir> map.txt`: the unique kernels with ids.
3. `tools/build-kernels.sh <dump dir> map.txt kernels/`: compile and pack (variables at the top of the script).
4. `tools/gen_all_dummies.py map.txt shim/src/dll/xess_dummies.inc`, then rebuild the shim.
5. `tools/check-kernels.py kernels/` must report 0 bad files. Run the game with `IGDEXT_STATIC=1`.
