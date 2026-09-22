# XeSS XMX on Linux (Proton) for Intel Xe2 / Xe3

Real Intel **XeSS XMX** super resolution inside a Proton game, running on the GPU's XMX (DPAS) matrix units, without a
Windows Intel driver. Stock Wine/Proton only ever gets the DP4a fallback of XeSS, because the XMX path needs two things
that do not exist on Linux: Intel's D3D12 extension layer (`igdext64.dll`, the `_INTC_D3D12_*` entry points) and a
C-for-Metal (CM) kernel compiler/runtime in the driver. This project provides both:

* `igdext64.dll` - a shim (built on Intel's open source [dxvk-igdext](https://github.com/GameTechDev/dxvk-igdext)) that
  answers XeSS' capability queries like an Intel Xe2/Xe3 driver and turns every CM kernel XeSS submits into a tiny
  placeholder D3D12 compute pipeline.
* a patched Mesa **ANV** Vulkan driver that recognises those placeholders and executes the real kernels, compiled
  ahead of time with Intel's own compiler (IGC) from the SPIR-V that XeSS ships, inside the normal vkd3d-proton queue.

Developed and tested on an ONEXPLAYER 3 (Intel Core Ultra "Panther Lake", Arc B390, Xe3) running SteamOS with
Mesa 26.1.2 and GE-Proton 11-6, with Wuthering Waves (XeSS 2.0.2.68, XeFG 1.3.1.78). Xe2 (Lunar Lake, Battlemage)
should work with the same code but a kernel set compiled for that device (`-device` in `tools/build-kernels.sh`).

Result: XeSS XMX super resolution renders correctly and is temporally stable, including under fast camera motion;
frame generation (XeFG 2x/3x/4x) works. Throughput on the B390 is at parity with DP4a (the GPU, not the upscaler,
is the limit at 22 W), so the point is quality, not speed: the XMX network is the better one.

## Quick start (release archive)

Download the release archive for your GPU generation (it contains `lib/libvulkan_intel.so`, `kernels/`, `igdext64.dll`
and these scripts) and unpack it anywhere, then:

    ./enable.sh      # game closed; copies the shim into the game's Proton prefix, appends a config block, clears the cache
    ./disable.sh     # restores everything

`enable.sh` writes four environment variables into a shell file that must be sourced before the game starts
(default `~/.config/wuwa-opti.conf`, the file our Wuthering Waves launch wrapper sources; set `XMX_CONF` to your own).
If you have no launch wrapper, put the printed `export` lines into the game's environment yourself, for example through
a small wrapper script in the Steam launch options (`/path/to/wrapper.sh %command%`):

    export VK_DRIVER_FILES=<package>/run/intel_icd.json
    export VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer
    export ANV_CM_KERNEL_DIR=<package>/kernels
    export MESA_SHADER_CACHE_DIR=~/.cache/xess-xmx-mesa

Other settings: `XMX_APPID` (Steam app id or non-Steam shortcut id, default 3513350), `XMX_PREFIX` (Proton prefix of
another launcher: the folder that contains `drive_c`), `XMX_GAME` (process name pattern used to refuse running while the
game is up). The game must use its DX12 renderer; XeSS' XMX path only exists on D3D12. In the game, select XeSS as usual.

Requirements: an Intel Xe3 (or, with a rebuilt kernel set, Xe2) GPU on the `xe` kernel driver, Mesa 26.1.2 (the patched
ANV is ABI-bound to the exact Mesa version - rebuild it from the patch for other versions), vkd3d-proton 2.14 / GE-Proton 11.

## How it works

1. **Shim.** XeSS loads `igdext64.dll` from the prefix's `driverstore/filerepository/igd_faux.inf_1` folder and calls
   `_INTC_D3D12_GetSupportedVersions`, `CreateDeviceExtensionContext`, `CheckFeatureSupport` and
   `CreateComputePipelineState`. The shim reports an Xe3 device with XMX, and for every CM kernel (SPIR-V + compile
   options) it returns a dummy DXBC compute pipeline whose `numthreads` is `(1 + id % 16, 1 + id / 16, 5)`, `id` being the
   kernel's index in a compiled-in table keyed by FNV-1a of SPIR-V + options (`shim/src/dll/xess_dummies.inc`).
2. **Driver.** The patched ANV (`patches/anv_cm_injection.patch`) sees a compute shader with workgroup z = 5, loads
   `$ANV_CM_KERNEL_DIR/wg<x>_<y>_5.cmk` and replaces the shader by that native binary. A `.cmk` (`tools/cmk_pack.py`)
   holds the ISA, GRF count, SLM size, barrier use, the walker payload layout (cross-thread data such as `local_size`
   and `group_count`, per-thread packed local ids with the 64-byte stride Xe3 uses) and the surface mapping. At each
   dispatch ANV builds the kernel's binding table out of vkd3d-proton's descriptor heap (XeSS' root signatures are
   one descriptor table per resource, whose heap offsets vkd3d passes as push constants), provides the single static
   sampler XeSS declares (linear, clamp) and programs SLM and barriers. Nothing leaves the Vulkan queue, so vkd3d-proton
   is unmodified.
3. **Kernels.** Compiled offline from the dumped SPIR-V with `ocloc` from Intel's compute-runtime and **IGC 2.10.10**
   (`-vc-codegen` plus the options XeSS passes: `-doubleGRF`, `-ze-exp-register-file-size`). Newer IGC versions crash on
   a few kernels and are used only as fallbacks.

### Which kernel variant XeSS hands out

XeSS asks the driver `CheckFeatureSupport(OPTIONS2)` for `SIMD16Required / LSCSupported / LegacyTranslationRequired`
and picks a variant set from the answer. With `SIMD16Required=0` it hands out the Xe-HPG set (SIMD8 kernels reading
images with the legacy `gather4.typed` messages, which Xe2/Xe3 hardware and IGC cannot run); that set produced a stippled
ghost trail behind moving objects. With `SIMD16Required=1` - Xe2/Xe3 are SIMD16-native - XeSS ships a different set:
57 of the 90 super-resolution kernels differ, they use `lsc.load/store.quad.typed` and compile for Xe3 unchanged.
The shim answers `SIMD16Required=1` to every XeSS context, and the shipped kernel set is this one. `LSCSupported=0` makes
XeSS use no CM kernels at all (plain DP4a HLSL path). XeSS-FG, when it gets `SIMD16Required=1`, creates no CM kernels
and generates frames through its non-CM path; measured throughput is identical, so frame generation is simply left there.

## Contents of this repository

    enable.sh, disable.sh        install / remove for one Proton prefix
    patches/anv_cm_injection.patch   Mesa ANV patch (tag mesa-26.1.2); also adds the debug switches listed below
    shim/                        igdext64.dll source: dxvk-igdext plus src/dll/D3D12Api.cpp (feature answers, dummy
                                 pipelines, kernel dump), XellHook.cpp (XeLL 30 fps cap fix), FgHook.cpp, StackSample.cpp
                                 (diagnostics), xess_dummies.inc (kernel table for XeSS 2.0.2.68 + XeFG 1.3.1.78);
                                 build with shim/build.bat (VS 2022 Build Tools) or the mingw toolchain file
    tools/s16_map.py             unique kernels of a dump -> id map (id dumpindex hash name lws)
    tools/build-kernels.sh       compile + pack a whole map (ocloc + IGC bundles)
    tools/cmk_pack.py            zebin -> .cmk packer;  tools/check-kernels.py validates a kernel dir
    tools/gen_all_dummies.py     regenerate the shim table from a map (needs fxc)
    tools/kernel_map_xess_2.0.2.68.txt   the map of the shipped set

Release archive only: `lib/libvulkan_intel.so` (patched ANV), `kernels/*.cmk` (114 kernels: 90 SR + 24 old XeFG),
`igdext64.dll`. See THIRD_PARTY.md about the kernels.

## Another game or another XeSS version

The kernel table is keyed by the kernel contents, not by the game. A game that ships the same `libxess.dll` (same
version) works out of the box: run `enable.sh` with `XMX_APPID`/`XMX_PREFIX` pointing at its prefix and get the four
variables into its environment. A different XeSS version means different kernels; unknown kernels currently run as
no-ops and XeSS' output goes black, so rebuild the set:

1. Run the game once with `IGDEXT_TRACE=1` in its environment. The shim writes `C:\igdext_trace.log` and dumps every
   kernel as `C:\igdext_dump\cs_NNNN_type2.bin` + `cs_NNNN_options.txt` (inside the prefix's `drive_c`).
2. `tools/s16_map.py <dump dir> map.txt` - unique kernels with ids (order of first appearance).
3. `tools/build-kernels.sh <dump dir> map.txt kernels/` - needs unpacked Intel compute-runtime (ocloc) and IGC debs,
   see the variables at the top of the script. IGC 2.10.10 first, 2.12.5 / 2.16.0 as fallbacks; `-device ptl` for Xe3,
   `lnl`/`bmg` for Xe2.
4. `tools/gen_all_dummies.py map.txt shim/src/dll/xess_dummies.inc` (fxc from the Windows SDK), rebuild the shim.
5. `tools/check-kernels.py kernels/` must report 0 bad files; clear `~/.cache/xess-xmx-mesa`.

Rebuilding ANV: clone Mesa at tag `mesa-26.1.2`, `git apply patches/anv_cm_injection.patch`, build with
`-Dvulkan-drivers=intel`, take `libvulkan_intel.so`.

## Debug switches

Patched ANV (game environment): `ANV_CM_DEBUG=1` (log injections), `ANV_CM_TRACE=1|2` (dispatches / surface states),
`ANV_CM_PC=1` (push constants), `ANV_CM_CAPTURE="x,y:N;..."` / `ANV_CM_GCAP="x,y:N;..."` (dump the buffers a kernel
uses, CPU / GPU side, into `/tmp/anv_cm_cap/`), `ANV_CM_SYNC=1` (full flush before each walker), `ANV_CM_VIEW`,
`ANV_CM_SKIP`, `ANV_CM_ONLY` (rebind / skip kernels), `ANV_CM_MAXGROUPS`, `ANV_CM_PREEMPT`.
Use `MESA_SHADER_CACHE_DISABLE=true` for kernel experiments: the shader cache is keyed by the dummy shader, not by the
kernel file, so a changed `.cmk` is otherwise served from the cache.

Shim (game environment): `IGDEXT_TRACE=1` (trace log + kernel dump), `IGDEXT_OPTIONS1=<xmx>,<dlboost>,<emul64>`,
`IGDEXT_OPTIONS2=<simd16>,<lsc>,<legacy>` (feature answers; default 1,1,0), `IGDEXT_GMD=<arch>,<release>`,
`IGDEXT_GTGEN`, `IGDEXT_GTNAME`, `IGDEXT_EUS=<eus>,<cores>` (reported device), `IGDEXT_XELL_LOG=1` and the other
`IGDEXT_XELL_*` / `IGDEXT_FG*` switches documented in `shim/src/dll/XellHook.cpp` and `FgHook.cpp`.

The shim also fixes one game-side problem: right after enabling frame generation the game hands XeLL a 33333 us frame
cap, which XeLL applies per displayed frame (30 fps until you open and close the pause menu). A cap arriving within
1.5 s of the FG enable call is replaced by the previous value; `IGDEXT_XELL_KEEPCAP=1` disables that.

## Caveats

* Only Xe3 (Panther Lake B390) has been tested. The ANV build is tied to Mesa 26.1.2; a SteamOS update that moves
  Mesa needs a rebuilt driver from the patch.
* Kernel sets are per XeSS version and per GPU generation (`ocloc -device`).
* XeSS quality presets 1 and 3 verified; frame generation verified at 2x/3x/4x; a 10 minute continuous run without
  GPU hangs; fast camera motion clean with the SIMD16 set.
* XeLL runs in software mode (the driver-side latency extension entry points are not implemented).
* Unknown kernels become no-ops instead of a DP4a fallback.
