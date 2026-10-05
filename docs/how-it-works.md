# How it works

Stock Wine/Proton only ever gets the DP4a fallback of XeSS, because the XMX path needs two things that do not exist
on Linux: Intel's D3D12 extension layer (`igdext64.dll`, the `_INTC_D3D12_*` entry points) and a C-for-Metal (CM)
kernel compiler/runtime in the driver. This project provides both, without modifying Proton or vkd3d-proton.

## 1. The shim

XeSS (and XeSS frame generation, `libxess_fg.dll`) loads `igdext64.dll` from the prefix's
`driverstore/filerepository/igd_faux.inf_1` folder and calls `GetSupportedVersions`, `CreateDeviceExtensionContext`,
`CheckFeatureSupport` and `CreateComputePipelineState`. The shim, built on Intel's open source
[dxvk-igdext](https://github.com/GameTechDev/dxvk-igdext):

* detects the GPU (Wine lists it under `HKLM\System\CurrentControlSet\Enum\PCI`) and describes it like the Windows
  driver would: graphics IP version, XMX support, and whether the GPU is SIMD16-native (see section 3);
* for every CM kernel XeSS submits (SPIR-V plus compile options), assigns a persistent id in that prefix
  (`C:\igdext_kernels\map.txt`: id and FNV-1a hash of SPIR-V + options), writes the SPIR-V and the options next to it
  (`dyn_<id>.spv/.opt`) and returns a **placeholder** D3D12 compute pipeline instead of the kernel.

A placeholder is a trivial DXBC compute shader that stores the magic value `0x584D5843`. Its `numthreads` encode the
id: planes z = 7, 11 and 13 (primes no game is known to use), and inside a plane every (x, y) with x * y * z <= 1024,
ordered by y then x - 1549 ids. When they run out the map starts over; compiled kernels are keyed by hash, so only the
id assignment is lost. `tools/check_dyn_ids.py` verifies that the shim table, its generator and the driver agree.

XeSS could also be pointed at the shim's folder through `INTC_ALT_DRIVER_EXTENSIONS_PATH`, without a copy in each
prefix, but GE-Proton sets that variable to `C:\Windows\System32` for every game, so the copies stay.

## 2. The driver

The patched Mesa ANV (`patches/0001`) looks at every compute shader it compiles. A shader whose workgroup size lies on
a reserved plane *and* which stores the magic value is a placeholder; any other shader, even with the same workgroup
size, is compiled normally. For a placeholder the driver

1. reads the kernel's hash from the prefix's `map.txt` and looks for
   `~/.cache/xess-xmx/kernels/<hash>_<pci id>_<toolchain>.cmk` (the toolchain part comes from the compiler in `igc/`
   and the packer, so a compiler update or a new `.cmk` format leads to recompiles);
2. on a miss, runs `tools/cm-compile.sh` (ocloc from Intel's compute-runtime with **IGC 2.10.10**, fetched into `igc/`
   by `tools/get-igc.sh`; `-vc-codegen` plus the options XeSS passes, `-doubleGRF`, `-ze-exp-register-file-size`),
   which packs the result with `tools/cmk_pack.py`. That takes 0.2-0.3 s per kernel; Wuthering Waves' 90 kernels take
   65-70 s in total at the first launch, about 31 s of it in the compiler itself;
3. replaces the shader by that native binary: the `.cmk` holds the ISA, GRF count, SLM size, barrier and sampler use,
   the walker payload (cross-thread data such as `local_size` and `group_count`, per-thread packed local ids with a
   stride of one GRF) and how the kernel's surfaces map to D3D12 descriptor tables;
4. never stores the placeholder in a pipeline cache, so a changed kernel or a compile that failed once is picked up
   on the next launch.

The register count (128 or 256, from the kernel's compile options) goes into the interface descriptor on Xe3. Xe2 has
one engine-wide switch instead, Large GRF Mode, which nothing else turns on: without it the first 256-register kernel
never finishes and the engine is reset. `patches/0003` switches it on before such a kernel and off before other
compute work and at the end of each command buffer, only when it changes.

At each dispatch ANV builds the kernel's binding table from vkd3d-proton's descriptor heap (XeSS root signatures are
one descriptor table per resource; vkd3d passes the heap offsets as push constants), takes the sampler state from the
root signature's static sampler (an immutable sampler in the pipeline layout), and programs SLM and barriers. Nothing
leaves the Vulkan queue. This relies on vkd3d-proton's classic descriptor path, hence
`VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer`. If the heap cannot be found the driver says so in the log and
leaves the kernel as a no-op.

### Failed compiles

If a compile fails, the driver lists the kernel in `C:\igdext_kernels\compile_failed.txt`. While that marker exists,
the shim declines the extension context, so XeSS uses its generic paths for that game instead of producing noise.
At the next launch, when the Vulkan device is created, the driver compiles the listed kernels once more and removes
the marker if they all succeed. After a second failure it tries again only when something has changed:

* a graphics component is newer than the marker (the compiler in `igc/`, the helper, the patched driver, the system's
  Mesa);
* the game's `libxess.dll` / `libxess_fg.dll` are newer than the marker (a game update; the shim removes the marker);
* the marker was deleted by hand, or `install.sh` was run.

### Self-test

Before the shim accepts an extension context it creates one extra placeholder pipeline on the game's device:
workgroup (1, 1, 17), outside the id planes, with the magic value. The patched driver recognises it and writes
`C:\igdext_kernels\anv_canary`. Only if that file is newer than the game process does the shim accept the context;
otherwise it declines it and XeSS behaves as on a system without the Intel extension: DP4a super resolution and
generic frame generation, which is limited to 2x. (Answering `LSCSupported=0` instead would switch XeSS frame
generation off altogether.) This catches every way placeholders could silently stay placeholders: the stock driver in
use (the session check fell back, the variables did not reach the game) or a vkd3d-proton change in how the
placeholder reaches the driver.

## 3. Which kernel variant XeSS hands out

XeSS asks `CheckFeatureSupport(OPTIONS2)` for `SIMD16Required / LSCSupported / LegacyTranslationRequired` and picks a
kernel set from the answer:

* `SIMD16Required=0`: the Xe-HPG set (SIMD8, images read with the legacy `gather4.typed` / `scatter4.typed` messages).
  Xe2/Xe3 hardware and IGC cannot run those messages; a hand-rewritten version of this set produced a stippled ghost
  behind moving objects, and XeSS frame generation with this answer requests kernels that do not compile at all.
* `SIMD16Required=1`: a different set (57 of Wuthering Waves' 90 kernels differ) using `lsc.load/store.quad.typed`,
  which compiles for Xe2/Xe3 unchanged. This is what the shim answers on Xe2 and Xe3.
* `LSCSupported=0`: no CM kernels at all, the DP4a HLSL path.

Frame generation uses the same mechanism. `libxess_fg.dll` opens four API-10 extension contexts (and one API-11 one
that creates nothing) and builds its own CM network: in Wuthering Waves 60 of the 90 kernels belong to XeFG, half of
them use `dpas` (the XMX instruction), and at 4x twelve of them run once per generated frame. OptiScaler-driven XeFG
in Forza Horizon 6 requests a further 24 kernels, also on XMX. `IGDEXT_TRACE=1` logs the calling module of every
context, query and pipeline, which tells SR and XeFG kernels apart.

## 4. XeLL (latency reduction)

Games with XeSS frame generation also use Intel XeLL (`libxell.dll`). Under Wine it runs in its built-in context
(cross-vendor mode, log line `[XeLL] Using built-in context`): the library paces the frames itself, with the timing it
can measure from the application side. That works.

In its driver mode `libxell.dll` hands over to `igxell64.dll`, a component of Intel's Windows driver, which gets CPU
and GPU timestamps of each frame's render submission from the driver through four entry points of the extension
library, in a buffer layout that is not documented. Neither that DLL nor the driver-side timing exists on Linux, so
the driver mode is not available. (The shim has an experimental probe for the interface, `IGDEXT_XELL_PROBE`; the
game's `libxell.dll` never called it.)

The shim hooks XeLL for one game-side problem: Wuthering Waves hands XeLL a 33.3 ms frame cap right after enabling
frame generation (30 fps until the pause menu is opened and closed). A cap arriving within 1.5 s of the enable call
is replaced by the previous value. This applies to Wuthering Waves only (`IGDEXT_XELL_FIXCAP=1` for other games).
