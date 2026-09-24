# XeSS XMX on Linux (Proton) for Intel Xe2 / Xe3

Real Intel **XeSS XMX** super resolution inside a Proton game, running on the GPU's XMX (DPAS) matrix units, without a
Windows Intel driver. Stock Wine/Proton only ever gets the DP4a fallback of XeSS, because the XMX path needs two things
that do not exist on Linux: Intel's D3D12 extension layer (`igdext64.dll`, the `_INTC_D3D12_*` entry points) and a
C-for-Metal (CM) kernel compiler/runtime in the driver. This project provides both:

* `igdext64.dll` - a shim (built on Intel's open source [dxvk-igdext](https://github.com/GameTechDev/dxvk-igdext)) that
  answers XeSS' capability queries like an Intel Xe2/Xe3 driver and turns every CM kernel XeSS submits into a tiny
  placeholder D3D12 compute pipeline.
* a patched Mesa **ANV** Vulkan driver that recognises those placeholders and executes the real kernels, compiled
  with Intel's own compiler (IGC) from the SPIR-V that XeSS ships - a precompiled set for the known XeSS versions, any
  other kernel on first use at run time - inside the normal vkd3d-proton queue.

Developed and tested on an ONEXPLAYER 3 (Intel Core Ultra "Panther Lake", Arc B390, Xe3) running SteamOS with
Mesa 26.1.2 and GE-Proton 11-6, with Wuthering Waves (XeSS 2.0.2.68, XeFG 1.3.1.78). Xe2 (Lunar Lake, Battlemage)
should work with the same code but a kernel set compiled for that device (`-device` in `tools/build-kernels.sh`).

Result: XeSS XMX super resolution renders correctly and is temporally stable, including under fast camera motion;
frame generation (XeFG 2x/3x/4x) works. Throughput on the B390 is at parity with DP4a (the GPU, not the upscaler,
is the limit at 22 W), so the point is quality, not speed: the XMX network is the better one.

## Quick start (release archive)

Download the release archive for your GPU generation (it contains `lib/libvulkan_intel.so`, `kernels/`, `igdext64.dll`
and these scripts) and unpack it anywhere (the folder stays in place), then either

**System-wide (recommended):**

    ./install.sh     # once; fetches the compiler bundle (~230 MB), then reboot / re-login
    ./uninstall.sh   # reverts everything

`install.sh` puts the three variables below into `~/.config/environment.d/50-xess-xmx.conf`, so the whole user session
(Steam and every game it starts, gamescope included) uses the patched ANV - no launch options, no per-game setup; installs
the shim into every Proton that bundles an `igdext64.dll` (Proton Experimental copies it into the prefix at each launch)
and into every existing prefix; and enables a user path unit that re-installs the shim after a Proton update. Kernels not
in the shipped set are compiled on first use (see "How it works"). Re-run it after a game created a new prefix.
Steam's per-game `MESA_SHADER_CACHE_DIR` is kept; `VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer` applies to every
D3D12 game (it makes vkd3d-proton use its classic descriptor path, which the kernel binding-table code relies on).

**Per game (the original way):**

    ./enable.sh      # game closed; copies the shim into the game's Proton prefix, appends a config block, clears the cache
    ./disable.sh     # restores everything

`enable.sh` writes four environment variables into a shell file that must be sourced before the game starts
(default `~/.config/wuwa-opti.conf`, the file our Wuthering Waves launch wrapper sources; set `XMX_CONF` to your own).
If you have no launch wrapper, use the one in this package: set `XMX_CONF=~/.config/xess-xmx.conf` when running
`enable.sh` and put `/path/to/xess-xmx-linux/xmx-launch.sh %command%` into the game's Steam launch options
(`xmx-launch.sh` sources that file and starts the game). The variables are:

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
3. **Kernels.** Compiled from the kernel's SPIR-V with `ocloc` from Intel's compute-runtime and **IGC 2.10.10**
   (`-vc-codegen` plus the options XeSS passes: `-doubleGRF`, `-ze-exp-register-file-size`). Newer IGC versions crash on
   a few kernels. The release ships the set for XeSS 2.0.2.68 / XeFG 1.3.1.78 precompiled, and everything else is
   compiled **at run time**: a kernel whose hash is not in the shim's table gets a persistent id per prefix
   (`C:\igdext_kernels\map.txt`), its SPIR-V and options are written next to it (`dyn_<id>.spv/.opt`) and a dummy pipeline
   with `numthreads (1 + m % 16, 1 + m / 16, z)`, z = 7, 11 or 13 (primes no game uses, so a real shader is never
   mistaken for one), is returned. When ANV meets such a workgroup and finds no `dyn_<id>.cmk`, it runs
   `tools/cm-compile.sh` (ocloc + IGC from `igc/`, fetched by `tools/get-igc.sh`) and loads the result - a one-time
   stall of about a second per kernel on the first launch. Run-time and precompiled kernels are byte-identical
   (verified for all 114 shipped ones), so a new XeSS version needs no rebuild of anything.

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

    enable.sh, disable.sh        install / remove for one Proton prefix;  xmx-launch.sh: Steam launch wrapper
    patches/anv_cm_injection.patch   Mesa ANV patch (tag mesa-26.1.2); also adds the debug switches listed below
    shim/                        igdext64.dll source: dxvk-igdext plus src/dll/D3D12Api.cpp (feature answers, dummy
                                 pipelines, kernel dump), XellHook.cpp (XeLL 30 fps cap fix), FgHook.cpp, StackSample.cpp
                                 (diagnostics), xess_dummies.inc (kernel table for XeSS 2.0.2.68 + XeFG 1.3.1.78);
                                 build with shim/build.bat (VS 2022 Build Tools) or the mingw toolchain file
    tools/s16_map.py             unique kernels of a dump -> id map (id dumpindex hash name lws)
    tools/build-kernels.sh       compile + pack a whole map (ocloc + IGC bundles)
    tools/cmk_pack.py            zebin -> .cmk packer;  tools/check-kernels.py validates a kernel dir
    tools/gen_all_dummies.py     regenerate the shim table from a map (needs fxc)
    tools/gen_dyn_dummies.py     the 288 dummies for dynamically assigned kernel ids (src/dll/dyn_dummies.inc)
    tools/cm-compile.sh          run-time compiler helper called by ANV (ocloc + IGC from igc/, then cmk_pack.py)
    tools/get-igc.sh             fetch IGC 2.10.10 + ocloc 25.18 into igc/ (needed for the run-time path)
    tools/anv_rt_patch.py        the run-time part of the ANV patch as a script (already contained in the .patch)
    tools/kernel_map_xess_2.0.2.68.txt   the map of the shipped set

Release archive only: `lib/libvulkan_intel.so` (patched ANV), `kernels/*.cmk` (146 kernels: 90 SR + 4 SR variants
first seen in Forza Horizon 6 + 24 XeFG SIMD16 variants + 4 SR variants first seen in Resident Evil 4 + 24 old XeFG),
`igdext64.dll`. See THIRD_PARTY.md about the kernels.

## Another game or another XeSS version

Nothing to do, provided the compiler bundle is present: run `tools/get-igc.sh` once (about 230 MB into `igc/`), then
run `enable.sh` for the game's prefix. Kernels the shipped table does not know are compiled on first use (see "How it
works", step 3). Watch it happen with `IGDEXT_TRACE=1` (`C:\igdext_trace.log`: `kernel hash ... -> dynamic id N`) and in
the game's stderr (`ANV CM: compiling dynamic kernel N` / `injected .../dyn_N.cmk`); failures are logged to
`$XDG_RUNTIME_DIR/xess-xmx-compile.log`. `IGDEXT_DYN_ONLY=1` ignores the static table so every kernel takes the
run-time path (test switch). The compiled kernels live in the prefix (`drive_c/igdext_kernels/dyn_<id>.cmk`); delete the
folder to force a recompile, delete `~/.cache/xess-xmx-mesa` when `lib/` or `kernels/` change.

The offline route still exists for shipping a precompiled set (no first-launch stall) or for another GPU generation
(`-device lnl`/`bmg` for Xe2):

1. Run the game once with `IGDEXT_TRACE=1` in its environment. The shim writes `C:\igdext_trace.log` and dumps every
   kernel as `C:\igdext_dump\cs_NNNN_type2.bin` + `cs_NNNN_options.txt` (inside the prefix's `drive_c`).
2. `tools/s16_map.py <dump dir> map.txt` - unique kernels with ids (order of first appearance).
3. `tools/build-kernels.sh <dump dir> map.txt kernels/` - needs unpacked Intel compute-runtime (ocloc) and IGC debs,
   see the variables at the top of the script. IGC 2.10.10 first, 2.12.5 / 2.16.0 as fallbacks.
4. `tools/gen_all_dummies.py map.txt shim/src/dll/xess_dummies.inc` (fxc from the Windows SDK), rebuild the shim.
5. `tools/check-kernels.py kernels/` must report 0 bad files; clear `~/.cache/xess-xmx-mesa`.

Rebuilding ANV: clone Mesa at tag `mesa-26.1.2`, `git apply patches/anv_cm_injection.patch`, build with
`-Dvulkan-drivers=intel`, take `libvulkan_intel.so`.

### Frame generation in a game without XeSS-FG (OptiScaler)

Forza Horizon 6 ships XeSS super resolution but no XeSS frame generation (its FG option is DLSS-G). OptiScaler can add
XeFG on top of the game's own XeSS: put `OptiScaler.dll` as `dxgi.dll`, `libxess_fg.dll`, `libxell.dll`, `fakenvapi.dll`
and `fakenvapi.ini` from the OptiScaler release into the game folder (not its `libxess.dll` - the game's own copy must
stay so the kernel table matches), use this `OptiScaler.ini`:

    [Upscalers]
    Dx12Upscaler=xess
    [FrameGen]
    Enabled=true
    FGInput=upscaler
    FGOutput=xefg
    [OptiFG]
    HUDFix=true
    [Inputs]
    EnableXeSSInputs=true
    EnableDlssInputs=false
    [Spoofing]
    Dxgi=false

and add `export WINEDLLOVERRIDES="dxgi=n,b"` to the sourced config file. `Dxgi=false` matters: with the default NVIDIA
spoof `libxess.dll` would see an NVIDIA adapter and take its DP4a path. XeFG driven this way asks the shim for CM
kernels (a SIMD16 variant set of 24, ids 94-117 in the table, which the game-integrated XeFG of Wuthering Waves never
requests). Verified on FH6: ~28 real fps -> ~56 presented at 2x, HUD stable; `[XeFG] InterpolationCount=2|3` gives
3x / 4x.

### A game without XeSS at all (Resident Evil 4, RE Engine) - tried, not recommended

RE Engine games ship a custom FSR2 that OptiScaler cannot hook. The only route is REFramework's `pd-upscaler` build +
PureDark's UpscalerBasePlugin 1.1.2 + a `libxess.dll` 2.0.2.68 in the game folder (`dinput8=n,b` override): its
TemporalUpscaler replaces the game's TAA by an XeSS call, XeSS loads the shim and runs on XMX (four more SR variants,
ids 118-121, are in the shipped set for that). Technically it works, but the result in RE4 1.5.9 was blurry text and
flicker, worse than the game's own FSR2, and frame generation is impossible there (OptiScaler: "FG inputs: none").
Left in the kernel set only; not worth setting up.

Note for anyone rebuilding the shim: it is linked against the static MSVC runtime on purpose. RE4's prefix carries an
older `msvcp140.dll`, in which a dynamically linked build crashed inside `DllMain` and XeSS silently fell back to DP4a
(the only symptom: `igdext64.dll` loaded and unloaded twice in a `PROTON_LOG` and no trace file).

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
  GPU hangs; fast camera motion clean with the SIMD16 set. Games verified: Wuthering Waves, Forza Horizon 6 (SR only:
  that game has no XeSS frame generation, its FG option is DLSS-G).
* XeLL runs in software mode (the driver-side latency extension entry points are not implemented).
* Unknown kernels become no-ops instead of a DP4a fallback.
