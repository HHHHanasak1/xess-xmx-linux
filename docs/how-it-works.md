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
  driver would: graphics IP version, XMX support, and whether the GPU is SIMD16-native (see below);
* for every CM kernel XeSS submits (SPIR-V plus compile options), assigns a persistent id in that prefix
  (`C:\igdext_kernels\map.txt`: id and FNV-1a hash of SPIR-V + options), writes the SPIR-V and the options next to it
  (`dyn_<id>.spv/.opt`) and returns a **placeholder** D3D12 compute pipeline instead of the kernel.

A placeholder is a trivial DXBC compute shader that stores the magic value `0x584D5843`. Its `numthreads` encode the
id: planes z = 7, 11 and 13 (primes no game is known to use), and inside a plane every (x, y) with x * y * z <= 1024,
ordered by y then x - 1549 ids. When they run out the map starts over; compiled kernels are keyed by hash, so only the
id assignment is lost. `tools/check_dyn_ids.py` verifies that the shim table, its generator and the driver agree.

## 2. The driver

The patched Mesa ANV (`patches/0001-anv-cm-kernel-injection.patch`) looks at every compute shader it compiles. A
shader whose workgroup size lies on a reserved plane *and* which stores the magic value is a placeholder; any other
shader, even with the same workgroup size, is compiled normally. For a placeholder the driver

1. reads the kernel's hash from the prefix's `map.txt` and looks for `~/.cache/xess-xmx/kernels/<hash>_<pci id>.cmk`;
2. on a miss, runs `tools/cm-compile.sh` (ocloc from Intel's compute-runtime with **IGC 2.10.10**, fetched into `igc/`
   by `tools/get-igc.sh`; `-vc-codegen` plus the options XeSS passes, `-doubleGRF`, `-ze-exp-register-file-size`),
   which packs the result with `tools/cmk_pack.py`. About a second per kernel, once per machine;
3. replaces the shader by that native binary: the `.cmk` holds the ISA, GRF count, SLM size, barrier and sampler use,
   the walker payload (cross-thread data such as `local_size` and `group_count`, per-thread packed local ids with a
   stride of one GRF) and how the kernel's surfaces map to D3D12 descriptor tables;
4. never stores the placeholder in a pipeline cache, so a changed kernel or a compile that failed once is picked up
   on the next launch.

At each dispatch ANV builds the kernel's binding table from vkd3d-proton's descriptor heap (XeSS root signatures are
one descriptor table per resource; vkd3d passes the heap offsets as push constants), takes the sampler state from the
root signature's static sampler (an immutable sampler in the pipeline layout), and programs SLM and barriers. Nothing
leaves the Vulkan queue.

The binding-table code relies on vkd3d-proton's classic descriptor path, hence
`VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer` (for every D3D12 game of the session with the system-wide install;
measured without a frame-rate difference, see the README). If the heap cannot be found the driver says so in the log
and leaves the kernel as a no-op.

If a compile fails, the driver appends the kernel to `C:\igdext_kernels\compile_failed.txt`. At the next launch the
driver compiles the listed kernels again when the Vulkan device is created (before XeSS asks its questions) and
removes the marker if they all succeed; if the marker is still there, the shim answers `LSCSupported=0` and XeSS uses
its DP4a path for that game instead of producing noise. A transient failure heals by itself.

Compile cost: about 0.2 s per kernel on an idle system (ocloc 170 ms, packing 35 ms), 0.3 s while the game loads;
Wuthering Waves' 90 kernels take about 65 s in total, most of it XeSS creating its pipelines one after another while
the game loads. The helper is started with `posix_spawn`, so the game process is not forked for each kernel.

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
them use `dpas` (the XMX instruction), and at 4x twelve of them run once per generated frame. Multi-frame generation
runs on XMX. OptiScaler-driven XeFG in Forza Horizon 6 requests a further 24 kernels, also on XMX.
`IGDEXT_TRACE=1` logs the calling module of every context, query and pipeline, which tells SR and XeFG kernels apart.

## 4. XeLL (latency reduction)

Games with XeSS frame generation also use Intel XeLL (`libxell.dll`). XeLL reports which context it uses:

* **built-in context** (what runs under Wine, log line `[XeLL] Using built-in context`): the game's own `libxell.dll`
  does the pacing itself, with the timing it can measure from the application side (cross-vendor mode);
* **driver DLL context**: `libxell.dll` hands over to `igxell64.dll`, a component of Intel's Windows driver (the
  library checks whether its own module name is `igxell`). That copy uses four driver entry points of the extension
  library (`GetLatencyReductionStatus`, `LatencyReductionExt`, `LatencyReductionGetRenderSubmitTimingsBuffers`,
  `RenderSubmitStart`) through which the driver reports CPU and GPU timestamps of each frame's render submission, in a
  buffer layout that is not documented.

A Linux implementation would need Intel's proprietary `igxell64.dll` and the driver-side timing: the time each frame's
first command list reaches the GPU and completes, which only the Vulkan driver (or vkd3d-proton) can see. The shim
has an experimental probe for that interface (`IGDEXT_XELL_PROBE=<version>`: claims support, logs every call and,
through guard pages, every access XeLL makes to the timing buffers); with `INTC_ALT_DRIVER_EXTENSIONS_PATH` pointing at
the shim, the game's `libxell.dll` still chose its built-in context, so no call reached it. Without `igxell64.dll` there
is nothing to drive, and XeLL stays in its cross-vendor mode, which works.

The shim hooks XeLL for one game-side problem: Wuthering Waves hands XeLL a 33.3 ms frame cap right after enabling
frame generation (30 fps until the pause menu is opened and closed). A cap arriving within 1.5 s of the enable call
is replaced by the previous value. This applies to Wuthering Waves only (`IGDEXT_XELL_FIXCAP=1` for other games).
