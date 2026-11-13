# VK_LAYER_XESS_xmx

`VK_LAYER_XESS_xmx` is a Vulkan layer that runs Intel XeSS (Xe Super Sampling) kernels on XMX (Xe Matrix Extensions, the GPU's hardware matrix units) on an **unpatched** Mesa ANV (Intel's open-source Vulkan driver). It does from outside the driver what `patches/0001` does inside it, so you can use your Linux distribution's own `vulkan-intel` package. It works with the same `igdext64.dll` shim, supporting both the hash placeholders and the ID placeholders of older shims.

## Status

* **Xe3 only:** The layer serves Xe3 GPUs whose PCI device ID has `(deviceID & 0xfff0) == 0xb080` or `0xfd80` (`0xB080`–`0xB08F` and `0xFD80`–`0xFD8F`, Panther Lake and Wildcat Lake). On those GPUs, `install.sh` runs the whole user session on the package's unpatched Mesa 26.3 (`lib/mesa-stock-<version>/`) with this layer by default.
* **System driver per game:** `xmx-stock.sh` runs a single game on your distribution's own unpatched ANV with this layer.
* **Supported Mesa versions:** The layer has serialized-shader layout tables (`src/layouts.h`) for Mesa 26.1, 26.3, and 27.0. With an unknown driver version, the layer leaves the shim's self-test (`anv_canary`) unanswered and XeSS falls back to DP4a (4-element dot-product instructions).
* **vkd3d-proton descriptor model:** The layer answers the self-test only when the test pipeline's layout has vkd3d-proton's classic descriptor heap (a `MUTABLE` binding of at least 1000 descriptors plus push constants for the descriptor table). With `VK_EXT_descriptor_buffer` or the `VK_EXT_descriptor_heap` path of vkd3d-proton 3.1 and newer, it logs `self-test placeholder NOT answered` and XeSS falls back to DP4a. A kernel it cannot bind is listed in `compile_failed.txt`, so the shim falls back from the next launch on.
* **Vulkan XeSS games:** On this project's patched drivers (on Xe3 `0xB080`–`0xB08F` / `0xFD80`–`0xFD8F` with `XMX_DRIVER=patched` or `MESA=`; on other GPUs the layer declines), the layer detects the patched driver's longer `prog_data` (`src/layouts.h`, `pd_size_patched`) and only serves XeSS's Vulkan path (`vkwrap/`); the patched driver injects D3D12 (Direct3D 12) games' kernels itself.

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
| `ANV_CM_COMPILE_TIMEOUT=<s>` | As for the driver: seconds one run of the compile script may take before its process group is killed and the kernel is marked failed (`compile_failed.txt`; default `120`, `0` = no limit) |
| `XMX_LAYER_PROF=1` | Measures the GPU time of every replaced dispatch and writes a per-kernel table to the log every 5 s |
| `XMX_LAYER_PROF=2` | Records one timestamp pair per run of back-to-back replaced dispatches, reported as one row `runs` (XeSS's GPU time including the gaps between its dispatches) |
| `XMX_LAYER_PROF_SEC` | Report period in seconds for `XMX_LAYER_PROF` (default `5.0`, must be `> 0.5`) |
