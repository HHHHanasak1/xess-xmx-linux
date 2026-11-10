# VK_LAYER_XESS_xmx

`VK_LAYER_XESS_xmx` is a Vulkan layer (an intercept library loaded between a game and the Vulkan driver) that runs XeSS XMX (matrix) kernels on an **unpatched** Mesa ANV (Intel's Linux Vulkan driver). It does from outside the driver what `patches/0001` does inside it, so you can use your distribution's own `vulkan-intel` package. It works with the same `igdext64.dll` shim, supporting both the hash placeholders and the ID placeholders of older shims.

## Status

- **Xe3 only:** The layer serves Xe3 GPUs only.
- **Mesa versions:** The serialized shader form differs between Mesa versions. `src/layouts.h` has one table per version for Mesa 26.1, 26.3, and 27.0-devel. With a driver version the layer does not know, the shim declines the extension and XeSS uses DP4a (dot-product fallback).
- **On Xe3:** `install.sh` runs the whole session on the package's unpatched Mesa 26.3 (`lib/mesa-stock-<version>/`) with this layer. `xmx-stock.sh` (experimental) runs one game on the system's own driver with it.
- **On the patched driver (Xe-HPG and Xe2, or `XMX_DRIVER=patched`):** The layer only serves XeSS's Vulkan path (`vkwrap/`). The patched driver's serialized shaders have a longer `prog_data` (`src/layouts.h`, `pd_size_patched`), and the patched driver injects a D3D12 game's kernels itself.

For how the layer works internally, see [../docs/dev/how-it-works.md](../docs/dev/how-it-works.md). For building the layer from source and running its unit tests, see [../docs/dev/building.md](../docs/dev/building.md).

## Use

To run one game on your system's unpatched Mesa driver with the layer, set its Steam launch options (with the release archive unpacked in `<package>`):

```sh
<package>/xmx-stock.sh %command%
```

The wrapper sets the variables below before Steam's runtime container starts and installs nothing. See [../docs/install.md](../docs/install.md) for what `xmx-stock.sh` sets and keeps. The game's prefix needs the shim (`install.sh`, or `enable.sh` for that game) and `<package>/igc` needs the compiler bundle (`tools/get-igc.sh`).

To run a game by hand outside Steam's container:

```sh
VK_DRIVER_FILES=<the distribution's intel_icd.*.json and other drivers, not the package's run/intel_icd.json> \
VK_ADD_LAYER_PATH=<package>/layer VK_LOADER_LAYERS_ENABLE=VK_LAYER_XESS_xmx \
XMX_LAYER_HELPER=<package>/tools/cm-compile.sh XMX_IGC_DIR=<package>/igc \
VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer,VK_KHR_present_wait2 <game>
```

The Vulkan driver must be an unpatched ANV (or this project's patched one, for the Vulkan path only), and only one Intel ICD (Installable Client Driver, the JSON file that points Vulkan at the driver library) may be in `VK_DRIVER_FILES`. Changing `VK_DRIVER_FILES` inside the container (in Proton's `user_settings.py`) loaded the container's copy of the patched ICD next to the stock one, causing games to recreate their swap chain thousands of times a minute.

## Variables

| Variable | Effect |
|---|---|
| `XMX_LAYER_LOG` | `1`: log to stderr; a path: append to that file |
| `XMX_LAYER_HELPER` | the compile script (else `ANV_CM_HELPER`, else `tools/cm-compile.sh` next to `ANV_CM_KERNEL_DIR`) |
| `XMX_LAYER_PROF=1` | GPU time of every replaced dispatch, a per-kernel table in the log every 5 s |
| `XMX_LAYER_PROF=2` | one timestamp pair per run of back-to-back replaced dispatches, reported as one row `runs` (XeSS's GPU time including the gaps between its dispatches) |
| `XMX_LAYER_PROF_SEC` | report period in seconds |
| `XMX_LAYER_DISABLE=1` | turn the layer off |
