# VK_LAYER_XESS_xmx

`VK_LAYER_XESS_xmx` is a Vulkan layer that runs Intel XeSS (Xe Super Sampling) kernels on XMX (Xe Matrix Extensions, the GPU's hardware matrix units) on an **unpatched** Mesa ANV (Intel's open-source Vulkan driver). It does from outside the driver what `patches/0001` does inside it, so you can use your Linux distribution's own `vulkan-intel` package. It works with the same `igdext64.dll` shim, supporting both the hash placeholders and the ID placeholders of older shims.

## Status

* **Xe3 and Xe2:** The layer serves the GPUs of `gpu-ids.txt`, which `build.sh` compiles into it (`src/gens.h`) and `install.sh` reads. Xe3 means Panther Lake `0xB080`–`0xB087`, `0xB08F`, `0xB090`, `0xB0A0`, `0xB0B0` and Wildcat Lake `0xFD80`, `0xFD81`. Xe2 means the Lunar Lake IDs `0x6420`, `0x64A0`, `0x64B0` and the Battlemage IDs `0xE202`, `0xE209`, `0xE20B`–`0xE20D`, `0xE210`–`0xE212`, `0xE215`, `0xE216`, `0xE220`–`0xE223` (the lists in Mesa 26.3). On those GPUs, `install.sh` runs the whole user session on the package's unpatched Mesa 26.3 (`lib/mesa-stock-<version>/`) with this layer by default.
* **128 GRF on Xe2:** An unpatched ANV never turns on `STATE_COMPUTE_MODE.LargeGRFMode` on Xe2, so every thread gets 128 GRF (general register file registers). The layer therefore compiles every kernel for 128 GRF there (`CM_MAX_GRF=128` to `tools/cm-compile.sh`, which drops the kernels' `-doubleGRF` / `-ze-exp-register-file-size=256` options). An offline compile of all 337 XeSS SIMD16 kernel bodies for Battlemage and Lunar Lake at 128 GRF needed no spill or scratch memory. They have about 0.6% more instructions on average, and up to 5–9% more in a few convolution kernels. Xe3 picks the register count per kernel, so kernels there keep their 256 GRF. **Xe2 stock mode is untested on real Xe2 hardware.**
* **Not Xe-HPG / Xe-LPG+:** Alchemist and Arrow Lake-H have the same engine-wide switch, and their SIMD8 kernel set spills at 128 GRF. They stay on the patched driver. (Meteor Lake, which Mesa counts to the same generation, has no XMX units.)
* **System driver per game:** `xmx-stock.sh` runs a single game on your distribution's own unpatched ANV with this layer.
* **Supported Mesa versions:** The layer has serialized-shader layout tables (`src/layouts.h`) for Mesa 26.1, 26.3, and 27.0. With an unknown driver version, the layer leaves the shim's self-test (`anv_canary`) unanswered and XeSS falls back to DP4a (4-element dot-product instructions).
* **vkd3d-proton descriptor model:** The layer answers the self-test only when the test pipeline's layout has vkd3d-proton's classic descriptor heap (a `MUTABLE` binding of at least 1000 descriptors plus push constants for the descriptor table). With `VK_EXT_descriptor_buffer` or the `VK_EXT_descriptor_heap` path of vkd3d-proton 3.1 and newer, it logs `self-test placeholder NOT answered` and XeSS falls back to DP4a. A kernel it cannot bind is listed in `compile_failed.txt`, so the shim falls back from the next launch on.
* **Vulkan XeSS games:** On this project's patched drivers (on Xe3 with `XMX_DRIVER=patched` or `MESA=`; on Xe2 the layer leaves the patched driver to run the kernels itself at their own 256 GRF, and on other GPUs it declines), the layer detects the patched driver's longer `prog_data` (`src/layouts.h`, `pd_size_patched`) and only serves XeSS's Vulkan path (`vkwrap/`); the patched driver injects D3D12 (Direct3D 12) games' kernels itself.

For how the layer works internally (SPIR-V placeholder scanning, `-noLoadPayload` compilation into `~/.cache/xess-xmx/kernels/*.cml`, the machine-code prologue from `src/prologue_table.inc` and `tools/gen_prologue_table.py`, template shader binary rewriting via `VK_KHR_pipeline_binary` in `src/anvbin.c` and `src/template.comp`, and descriptor set copying), see [../docs/dev/how-it-works.md](../docs/dev/how-it-works.md).

For building the layer (`VULKAN_HEADERS=<Vulkan-Headers checkout> ./build.sh [out dir]`) and running its unit tests (`./tests/build.sh`), see [../docs/dev/building.md](../docs/dev/building.md).

## Use

To run one game on your distribution's unpatched ANV driver with the release archive unpacked in `<package>`, set the game's Steam launch options to:

```sh
<package>/xmx-stock.sh %command%
```

`xmx-stock.sh` sets the environment variables below before Steam's runtime container (`pressure-vessel`) starts and installs nothing on your system (see [../docs/install.md](../docs/install.md) and [../docs/launch-options.md](../docs/launch-options.md)). The game's prefix needs the shim (`install.sh`, or `enable.sh` for that game) and `<package>/igc` needs the compiler bundle (`tools/get-igc.sh`).

To run a game with the layer by hand outside Steam's container:

```sh
VK_DRIVER_FILES=<the distribution's intel_icd.*.json and other drivers, not the package's run/intel_icd.json> \
VK_ADD_LAYER_PATH=<package>/layer VK_LOADER_LAYERS_ENABLE=VK_LAYER_XESS_xmx \
XMX_LAYER_HELPER=<package>/tools/cm-compile.sh XMX_IGC_DIR=<package>/igc \
VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer,VK_EXT_descriptor_heap,VK_KHR_present_wait2 <game>
```

The Vulkan driver must be an unpatched ANV (or this project's patched one, for the Vulkan path only), and only one Intel ICD (Installable Client Driver) may be in `VK_DRIVER_FILES`. Changing `VK_DRIVER_FILES` inside the container (such as in Proton's `user_settings.py`) loads the container's copy of the patched ICD alongside the stock one, which caused games to recreate their swap chain thousands of times a minute.

### Environment Variables

| Variable | Effect |
|---|---|
| `XMX_LAYER_LOG` | `1`: log to `stderr`; a file path (containing `/`): append to that file |
| `XMX_LAYER_HELPER` | Path to the compile script (defaults to `ANV_CM_HELPER`, else `tools/cm-compile.sh` next to `ANV_CM_KERNEL_DIR`) |
| `XMX_LAYER_COMPILE_JOBS=<n>` | Kernel compiles at a time (`1`–`32`; default the online CPUs, at most `8`). On a cache miss the thread that needs the kernel compiles it, and `n - 1` background threads compile the other kernels of the shim's folder that have no cache file yet, as the patched driver does (`ANV_CM_COMPILE_JOBS`, which counts only the background threads). `1` compiles one kernel after another when XeSS asks for it, as before |
| `ANV_CM_COMPILE_TIMEOUT=<s>` | As for the driver: seconds one run of the compile script may take before its process group is killed and the kernel is marked failed (`compile_failed.txt`; default `120`, `0` = no limit) |
| `XMX_LAYER_PROF=1` | Measures the GPU time of every replaced dispatch and writes a per-kernel table to the log every 5 s |
| `XMX_LAYER_PROF=2` | Records one timestamp pair per run of back-to-back replaced dispatches, reported as one row `runs` (XeSS's GPU time including the gaps between its dispatches) |
| `XMX_LAYER_PROF_SEC` | Report period in seconds for `XMX_LAYER_PROF` (default `5.0`, must be `> 0.5`) |
| `XMX_LAYER_PACE=1` | Experimental, off by default: spaces frame generation's images evenly (`src/pace.c`). `vkQueuePresentKHR` returns at once, and the layer's own thread presents each image when it is ready and at least `K * T / n` after the previous one (`T` = the rendered frame interval and `n` = the images per frame, both read from the images' ready times). It paces only while the images come in steady bursts. `XMX_LAYER_PACE=log` adds the layer's fence and present-side timing without moving the present. Worth it only under a fixed frame cap at 3x or more (`FFX_XEFG_CAP=<us>`): at 2x under the automatic cap it costs about 6% frame time, and at 3x under the automatic cap it stays off (see `docs/dev/how-it-works.md`, "The shown frames") |
| `XMX_LAYER_PACE_IMAGES` | Swap chain images the layer adds while `XMX_LAYER_PACE` is on (default `2`), so that held images do not stall the game's acquire |
| `XMX_LAYER_PACE_K` | The spacing factor `K` for `XMX_LAYER_PACE` (default `0.97`) |
| `XMX_LAYER_PACE_LOG=<file>` | The pacing log (a summary every 2 s); `XMX_LAYER_PACE_TRACE=1` adds one line per present |
| `XMX_LAYER_FORCE_GRF=128` | Debug: compiles every kernel for 128 GRF on Xe3 as well, as the layer always does on Xe2, to compare image and speed of the 128- and 256-GRF builds on one device. Their cache files are separate (`*_xe3-g128_*.cml`). Other values are ignored |
