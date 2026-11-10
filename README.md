# XeSS XMX on Linux (Proton) for Intel Arc: Xe-HPG, Xe2, Xe3

This package runs Intel **XeSS** super resolution (upscaling) and **XeSS frame generation** on the XMX (matrix) hardware units of Intel Arc GPUs inside Steam Proton games. Stock Wine and Proton lack the Windows driver interfaces that XeSS looks for, so games fall back to the slower generic DP4a instruction path and limit frame generation to 2x.

This package requires no Windows driver and no changes to Proton or vkd3d-proton (the Direct3D 12 to Vulkan translation layer). It contains no Intel kernels; your machine compiles them on first use from the SPIR-V shaders inside the game's own XeSS libraries.

## Supported GPUs

You need an Intel GPU with XMX units running on the `xe` or `i915` Linux kernel driver:

* **Xe3** (Panther Lake): developed and tested on an ONEXPLAYER 3 (Arc B390) with SteamOS, Mesa 26.1.2 / 26.3, and GE-Proton 11-6 / Proton Experimental.
* **Xe2** (Battlemage): tested by users on an Arc B580 (#16).
* **Xe-HPG** (Alchemist): tested by users on an Arc A310 (#28).
* Lunar Lake and Arrow Lake-H are detected but untested.
* Xe-LPG (Meteor Lake) has no XMX units.

Only the native Steam client is supported (SteamOS, distribution packages), not Flatpak Steam. Full system requirements are in [docs/install.md](docs/install.md).

## Quick install

1. Download the release archive and unpack it in a permanent folder. (If you want to move it later, run `./uninstall.sh` first.)
2. Run the installer:
   ```sh
   ./install.sh
   ```
   This fetches the Intel shader compiler bundle (about 95 MB), Intel's XeSS SDK libraries (77 MB), and AMD's FidelityFX loader and providers (87 MB), configures your session environment, and puts the shim DLLs into your Proton prefixes (the simulated Windows folders for each game).
3. Reboot, or log out and back in.
4. Start your game from Steam and enable XeSS (and XeSS frame generation) on its **DX12** renderer. No launch options are needed for standard DX12 games; DX11, Vulkan, or older XeSS games use `XESS=latest %command%`. The first launch compiles the game's kernels into `~/.cache/xess-xmx`, which takes a minute or two once.

To revert everything, run:
```sh
./uninstall.sh
```

For per-game installation (`enable.sh` / `xmx-launch.sh`), using your system's stock driver for a single game (`xmx-stock.sh`), and details on what `install.sh` sets up, see [docs/install.md](docs/install.md).

## Features

* **XeSS Super Resolution on XMX (DX12):** `igdext64.dll` (a shim built on Intel's open-source [dxvk-igdext](https://github.com/GameTechDev/dxvk-igdext)) answers XeSS driver queries and hands C-for-Metal (CM) kernels over as placeholders. A patched Mesa **ANV** Vulkan driver (the default on Xe-HPG and Xe2; the Mesa 26.3 patches by @kas5852, #19) or the `VK_LAYER_XESS_xmx` Vulkan layer on unpatched Mesa 26.3.0 (the default on Xe3) compiles them with Intel's compiler (IGC) and runs them inside the normal vkd3d-proton queue. Super resolution renders temporally stable images. Details in [docs/dev/how-it-works.md](docs/dev/how-it-works.md) and [docs/drivers.md](docs/drivers.md).
* **XeSS Frame Generation and multipliers (`XESS_FG`):** Runs XeSS frame generation on XMX at 2x, 3x, and 4x. `ControlLib.dll` stands in for Intel's Control Library so you can set `XESS_FG=<n>` in a game's launch options to force a fixed multiplier up to 4x (and experimentally up to **8x**), even above what the game's menu offers. Details in [docs/frame-generation.md](docs/frame-generation.md) and [docs/launch-options.md](docs/launch-options.md).
* **Low latency mode:** Enabled by default during XeSS frame generation (`IGDEXT_XELL_CAP=off` turns it off). The shim uses the XeLL (Xe Low Latency) API to keep frames from queueing before the GPU, which roughly halves latency in a GPU-limited game at the same frame rate. Details in [docs/frame-generation.md](docs/frame-generation.md).
* **FSR 3.1+ replaced by XeSS (`FSR=xess`):** `amd_fidelityfx_upscaler_dx12.dll` and `amd_fidelityfx_framegeneration_dx12.dll` run a game's FSR 3.1+ upscaling and frame generation on XeSS and XMX. A game that has native XeSS should use native XeSS instead, as the FSR route can flicker more. With `dxgi.dll` (`FSR=xess FSR_FG=inject`), a game with FSR 3.1+ upscaling but no FSR frame generation (such as Forza Horizon 6) gets XeSS frame generation too. Details in [docs/frame-generation.md](docs/frame-generation.md).
* **Optical flow frame generation for any DX12 game (`OFLOW_FG=1`, experimental):** Adds XeSS frame generation to DX12 games that have neither XeSS nor FSR 3.1+, calculating motion vectors from optical flow on the final image like a driver-level frame generator (AMD Fluid Motion Frames, NVIDIA Smooth Motion). Details in [docs/frame-generation.md](docs/frame-generation.md).
* **XeSS in DX11 games (`XESS=latest`):** Intel's `libxess_dx11.dll` forwards to the Windows driver and does nothing under Proton. This package's `libxess_dx11.dll` runs D3D12 XeSS on XMX next to the game's D3D11 device (also works with OptiScaler in DX11 games). Details in [docs/games.md](docs/games.md#xess-in-a-d3d11-game).
* **XeSS in Vulkan games (`XESS=latest`):** This package's `vulkan-1.dll` and Vulkan layer give Vulkan games the Intel interfaces XeSS looks for so they run on XMX (Xe3 only). Details in [docs/games.md](docs/games.md#xess-in-a-vulkan-game).
* **Upgrading old game XeSS libraries (`XESS=latest`):** Temporarily replaces a game's older XeSS libraries while it runs, for games like Cyberpunk 2077 whose built-in XeSS is too old to detect an Intel GPU under Proton. Details in [docs/games.md](docs/games.md) and [docs/launch-options.md](docs/launch-options.md).

## Performance

Measured on an Arc B390 using built-in benchmarks (one run each). "Without this project" is Mesa 26.1.2 with XeSS on its generic DP4a path; 26.3 is this project's patched Mesa 26.3.0-rc3 (release 20261104):

| | Cyberpunk 2077 | Forza Horizon 6 |
|---|---|---|
| Without this project (26.1.2 + DP4a) | 47.7 fps | 43 fps (GPU 47.3) |
| 26.3 + XMX | 61.3 fps, +28% | 46 fps, +7% (GPU 55.0, +16%) |
| 26.3 + XMX + frame generation 3x | 117.0 fps, about 2.5x | 98 fps, about 2.3x |

* XMX itself is about 5% faster than DP4a in both games. Most of the jump from 26.1.2 to 26.3 comes from Jay, Mesa's new shader compiler backend (default on Xe2 and Xe3). Forza Horizon 6 is limited by its render thread on this device, so its overall frame rate gains less than its GPU frame rate.
* With frame generation on, the shim's low latency mode cuts frame queue latency by 36–50% at the same frame rate (tested in Wuthering Waves using the game's native XeSS frame generation; does not apply to the FSR or optical flow routes).
* Mesa 26.3.0-rc3 unpatched with this project's Vulkan layer (`xmx-stock.sh`, experimental) performs like the patched driver in Cyberpunk 2077 and faster in Forza Horizon 6; other games may run slower and it needs more testing.

For benchmark settings, full tables, latency tests, and the Final Fantasy VII Rebirth comparison (#19, about +15%, half of it from Jay), see [docs/performance.md](docs/performance.md). Image quality and the cost of disabling `VK_EXT_descriptor_buffer` have not been measured yet.

## Known limitations

* **XeLL** (Intel's latency reduction, used with frame generation) runs in its cross-vendor mode; its driver mode does not start under Proton. Details in [docs/dev/how-it-works.md](docs/dev/how-it-works.md).
* **Fallback limits frame generation to 2x:** If the self-test fails or a kernel fails to compile, the package falls back to XeSS' generic paths, where frame generation is capped at 2x. A game set to 3x or 4x will run at 2x until the XMX path works again.
* **`XESS_FG=5` to `8` are experimental:** Multipliers above 4x work by changing a value inside Intel's frame generation library in memory while the game runs. Intel does not support more than 4x, and 7x/8x add a lot of latency.
* **vkd3d-proton and Mesa dependencies:** The driver patches are written against Mesa 26.1.2 and 26.3 and rely on how vkd3d-proton compiles placeholder shaders and lays out its descriptor heap. If a vkd3d-proton update changes shader compilation, the self-test turns the XMX path off; if it changes the descriptor heap layout, the driver logs `no vkd3d-style descriptor heap` and the kernels do nothing.

## Documentation

User guides:
* [docs/install.md](docs/install.md) — Installing, updating, uninstalling, system requirements, per-game setup (`enable.sh` / `xmx-launch.sh`), and running one game on the system's stock driver (`xmx-stock.sh`).
* [docs/drivers.md](docs/drivers.md) — The included Mesa drivers (unpatched 26.3.0 + layer, patched 26.3.0, patched 26.1.2), choosing drivers (`XMX_DRIVER`, `MESA=`, `tools/switch-driver.sh`), the session check, and rebuilding after a system update (`tools/rebuild-driver.sh`).
* [docs/games.md](docs/games.md) — Per-game guides (Wuthering Waves, Cyberpunk 2077, Forza Horizon 6, Satisfactory, Resident Evil 4), DX11 and Vulkan games, OptiScaler, and FSR 4 on XMX notes.
* [docs/frame-generation.md](docs/frame-generation.md) — Native XeSS-FG and `XESS_FG`, FSR on XeSS (`FSR=xess`, `FSR_FG=xess`, `FSR_FG=inject`, `FSR_VENDOR`), optical flow frame generation (`OFLOW_FG=1`), and low latency mode.
* [docs/launch-options.md](docs/launch-options.md) — Reference for all Steam launch options.
* [docs/performance.md](docs/performance.md) — Frame rate and latency benchmarks.
* [docs/troubleshooting.md](docs/troubleshooting.md) — Common symptoms, fixes, and how to collect logs for a bug report.

Developer guides:
* [docs/dev/how-it-works.md](docs/dev/how-it-works.md) — Architecture of the shim, driver patches, Vulkan layer, kernel variants, XeLL, D3D11 bridge, Vulkan wrapper, FSR translation, and optical flow.
* [docs/dev/building.md](docs/dev/building.md) — Building the shim, DLLs, layer, and patched Mesa drivers from source, plus the repository layout.
* [docs/dev/debugging.md](docs/dev/debugging.md) — Developer debug switches, trace logs, and the XeLL driver-mode probe (`tools/xell-probe.sh`).
* [layer/README.md](layer/README.md) — Overview of the `VK_LAYER_XESS_xmx` Vulkan layer.

## License

MIT (see `LICENSE`). The shim is based on dxvk-igdext (MIT), and the driver patches apply to Mesa (MIT). See [THIRD_PARTY.md](THIRD_PARTY.md) for all third-party components and licenses.
