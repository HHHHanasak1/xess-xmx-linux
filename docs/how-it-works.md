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
* for every CM kernel XeSS submits (SPIR-V plus compile options), writes the SPIR-V and the options to
  `C:\igdext_kernels\<hash>.spv` / `.opt` (hash: FNV-1a of SPIR-V + options, 16 hex digits) and returns a
  **placeholder** D3D12 compute pipeline instead of the kernel.

A placeholder is a trivial DXBC compute shader with `numthreads(1, 1, 23)` (a prime no game is known to use) whose one
`store_raw` writes the magic value `0x584D5843` and the kernel's 64-bit hash as two dwords. The shim has one fxc-built
template of it (`shim/src/dll/placeholders.inc`, written by `tools/gen_placeholders.py`), writes the hash into a copy
and recomputes the DXBC checksum (MD5 with DXBC's own end-of-message layout, `shim/src/dll/DxbcChecksum.h`). The driver
reads the hash back from the constants of the shader. vkd3d-proton does not verify the checksum (neither its own DXBC
parser up to 2.14 nor dxbc-spirv, which 3.x uses), it keys its caches by a hash of the whole blob, and it keeps the
three values as 32-bit constants: one vector store in 3.x, one store per dword in 2.x. `tools/check_placeholders.py`
verifies that the templates (shape, magic, checksum, where the hash goes) and the shim's self-test values match the
driver.

Shims before this format numbered the kernels instead (`map.txt`, `dyn_<id>.spv`, the id encoded in `numthreads`
on the planes z = 7, 11 and 13); the driver still understands those placeholders.

XeSS could also be pointed at the shim's folder through `INTC_ALT_DRIVER_EXTENSIONS_PATH`, without a copy in each
prefix, but GE-Proton sets that variable to `C:\Windows\System32` for every game, so the copies stay. The variable
does point at the prefix's copy for Protons that show XeSS no driver store at all (Proton 11.0): there XeSS would
never find the folder on its own.

## 2. The driver

The patched Mesa ANV (`patches/0001`) looks at every compute shader it compiles. A shader with a placeholder's
workgroup size (1, 1, 23), or one on the id planes of an older shim, *and* which stores the magic value is a
placeholder; any other shader, even with the same workgroup size, is compiled normally (on Mesa 26.3 by Jay or brw; the placeholder never reaches either). For a placeholder the
driver

1. reads the kernel's hash from the placeholder (the two constants next to the magic value; as their order after
   vkd3d-proton and NIR depends on the store's path, it takes the first pair the shim wrote a `.spv` for or that is
   in the cache) and looks for
   `~/.cache/xess-xmx/kernels/<hash>_<pci id>_<toolchain>.cmk` (the toolchain part comes from the compiler file names
   in `igc/`, the contents of the compile scripts and the packer version, so a compiler update, a changed script or a
   new `.cmk` format leads to recompiles);
2. on a miss, runs `tools/cm-compile.sh` (ocloc from Intel's compute-runtime with **IGC 2.10.10**, fetched into `igc/`
   by `tools/get-igc.sh`; `-vc-codegen` plus the options XeSS passes, `-doubleGRF`, `-ze-exp-register-file-size`)
   into a temporary folder, and packs the zebin into the `.cmk` itself (a C port of `tools/cmk_pack.py`, byte for
   byte the same output). Besides ocloc the script starts no program; the driver passes it the PCI id and the GRF
   size. With a driver of an older package, which does not ask for that, the script still packs with
   `tools/cmk_pack.py`. Before the driver packed itself a kernel took 0.2-0.3 s; Wuthering Waves' 90 kernels took
   65-70 s in total at the first launch, about 31 s of it in the compiler itself. The kernels are still compiled one
   after the other, inside `vkCreateComputePipelines`;
3. replaces the shader by that native binary: the `.cmk` holds the ISA, GRF count, SLM size, barrier and sampler use,
   the walker payload (cross-thread data such as `local_size` and `group_count`, per-thread packed local ids with a
   stride of one GRF) and how the kernel's surfaces map to D3D12 descriptor tables;
4. never stores the placeholder in a pipeline cache, so a changed kernel or a compile that failed once is picked up
   on the next launch.

The register count (128 or 256, from the kernel's compile options) goes into the interface descriptor on Xe3. Xe-HPG
(Alchemist) and Xe2 have one engine-wide switch instead, Large GRF Mode, which nothing else turns on: without it the
first 256-register kernel never finishes and the engine is reset. `patches/0003` switches it on before such a kernel
and off before other compute work (Mesa's own BLORP copies and fills, trace rays, the driver's internal shaders and
generated commands included) and at the end of each command buffer, only when it changes.

At each dispatch ANV builds the kernel's binding table from vkd3d-proton's descriptor heap (XeSS root signatures are
one descriptor table per resource; vkd3d passes the heap offsets as push constants), takes the sampler state from the
root signature's static sampler (an immutable sampler in the pipeline layout), and programs SLM and barriers. A kernel
whose sampler is a descriptor table instead (the FSR 4 on XMX plugin, [games.md](games.md)) has that table as the last
root parameter, after the surfaces; the driver then takes the sampler state from vkd3d's sampler heap. Nothing
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
workgroup (1, 1, 19), from the second template, with the magic value and two test values where a kernel's
placeholder has its hash (as floats a denormal and a signaling NaN, so a translation step that flushed or canonicalized
them shows up here too). The patched driver recognises it, checks that both test values arrived, and writes
`C:\igdext_kernels\anv_canary`. A driver of an older package does not know this shape, so with a newer shim it fails
the test (DP4a instead of noise); the old test shape (1, 1, 17) is still answered for older shims. Only if that file is newer than the game process does the shim accept the context;
otherwise it declines it and XeSS behaves as on a system without the Intel extension: DP4a super resolution and
generic frame generation, which is limited to 2x. (Answering `LSCSupported=0` instead would switch XeSS frame
generation off altogether.) This catches every way placeholders could silently stay placeholders: the stock driver in
use (the session check fell back, the variables did not reach the game) or a vkd3d-proton change in how the
placeholder reaches the driver.

## 3. Which kernel variant XeSS hands out

XeSS asks `CheckFeatureSupport(OPTIONS2)` for `SIMD16Required / LSCSupported / LegacyTranslationRequired` and picks a
kernel set from the answer:

* `SIMD16Required=0`: the Xe-HPG set (SIMD8, images read with the legacy `gather4.typed` / `scatter4.typed` messages).
  This is what the shim answers on Xe-HPG, the GPU the set is made for. Xe2/Xe3 hardware and IGC cannot run those
  messages; on them a hand-rewritten version of this set produced a stippled ghost behind moving objects, and XeSS
  frame generation with this answer requests kernels that do not compile at all.
* `SIMD16Required=1`: a different set (57 of Wuthering Waves' 90 kernels differ) using `lsc.load/store.quad.typed`,
  which compiles for Xe2/Xe3 unchanged. This is what the shim answers on Xe2 and Xe3.
* `LSCSupported=0`: no CM kernels at all, the DP4a HLSL path.

Frame generation uses the same mechanism. `libxess_fg.dll` opens four API-10 extension contexts (and one API-11 one
that creates nothing) and builds its own CM network: in Wuthering Waves 60 of the 90 kernels belong to XeFG, half of
them use `dpas` (the XMX instruction), and at 4x twelve of them run once per generated frame. OptiScaler-driven XeFG
in Forza Horizon 6 requests a further 24 kernels, also on XMX. `IGDEXT_TRACE=1` logs the calling module of every
context, query and pipeline, which tells SR and XeFG kernels apart.

### The number of generated frames (`XESS_FG`)

On Windows, Intel Graphics Software can override how many frames XeSS frame generation generates. When a game creates
its frame generation context, `libxess_fg.dll` loads Intel's Control Library (`ControlLib.dll` from `system32`), asks
it for the 3D feature "frame generation" globally and for the game's executable, and uses the answer as its maximum
number of interpolated frames instead of the game's choice. The package's `controllib/ControlLib.c` answers exactly
these calls (device enumeration through DXGI, the feature query) from `XESS_FG`; `install.sh` puts it into every
prefix. XeSS frame generation accepts the values of Intel's public enum, 2x to 4x.

Above 4x it ignores the override, so the shim (`shim/src/dll/XellHook.cpp`, hooks on `libxess_fg`'s exports) raises
the number itself, by observation and through the public API only: after the game's context creation (override 4x)
it collects the 32-bit values 3 in `libxess_fg`'s writable sections, creates a probe context with the override at 3x
and keeps the one value that followed it, sets the wanted number there, and checks that
`xefgSwapChainGetProperties` of the game's context now reports it; otherwise the value goes back and the game keeps
4x. This happens before the game sizes or initializes frame generation (at creation, or at the first call after it
when `libxess_fg` loaded the shim itself during that creation). A game that sets the number while it runs (Wuthering
Waves, from its 2x/3x/4x menu) gets `XESS_FG`'s number through `xefgSwapChainSetNumInterpolatedFrames` instead.

A game with FSR 3.1+ reaches the same XeSS and XeSS frame generation through the package's FSR DLLs:
[fsr-frame-generation.md](fsr-frame-generation.md).

## 4. XeLL (latency reduction)

Games with XeSS frame generation also use Intel XeLL (`libxell.dll`). Under Wine it runs in its built-in context
(cross-vendor mode, log line `[XeLL] Using built-in context`): the library paces the frames itself, with the timing it
can measure from the application side. That works.

Its driver mode, which takes CPU and GPU timestamps of each frame's render submission from the driver through four
entry points of the extension library (in a buffer layout that is not documented), does not start under Proton:
`libxell.dll` checks for Wine before anything else and then keeps its built-in context. What it requires behind that
check, and the shim's experimental probe for the interface (`IGDEXT_XELL_PROBE`, `tools/xell-probe.sh`), are in
[debugging.md](debugging.md). Even with the probe, Wuthering Waves stays in the built-in context.

The shim hooks XeLL for two things. One is a game-side problem: Wuthering Waves hands XeLL a 33.3 ms frame cap right
after enabling frame generation (30 fps until the pause menu is opened and closed). A cap arriving within 1.5 s of the
enable call is replaced by the previous value. This applies to Wuthering Waves only (`IGDEXT_XELL_FIXCAP=1` for other
games).

The other is latency. XeLL's built-in context sleeps only for a frame cap; its own estimate of the CPU time holds the
wait in the queue, so a queued game looks CPU-bound to it and never waits. While frame generation runs (the shim sees
`xefgSwapChainSetEnabled` or `xellSetFgEnabled`), the shim waits after `xellSleep` until the frame's CPU time before the
GPU gets free: the newest finished frame in XeLL's frame reports + the frames in flight x a frame's time - the lowest
simulation -> render submit of the last 64 frames. The frame time is learned before any wait and only raised when that
CPU time shows a queue again (a heavier scene), so the wait cannot drift down the frame rate; it follows a lighter
scene down. The wait is bounded to two frame times. `IGDEXT_XELL_CAP=off` turns it off ([launch-options.md](launch-options.md)).
