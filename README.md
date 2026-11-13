# XeSS XMX on Linux (Proton) for Intel Arc: Xe-HPG, Xe2, Xe3

Run Intel **XeSS** super resolution (upscaling) and **XeSS frame generation** inside Steam Proton games on the GPU's **XMX** (Xe Matrix Extensions, hardware matrix units) engines. Under stock Wine and Proton, XeSS falls back to generic **DP4a** (8-bit integer dot-product instructions) shaders and caps frame generation at 2x. This package runs the native XMX kernels and multi-frame generation without a Windows driver and without modifying Proton or vkd3d-proton (Proton's Direct3D 12 to Vulkan translation layer).

## Supported GPUs

Requires an Intel GPU with XMX engines on the Linux `xe` or `i915` kernel driver:

* **Xe3** (Panther Lake `0xB080`–`0xB0FF`, Wildcat Lake `0xFD80`–`0xFD8F`): tested on an Arc B390 (Panther Lake); Wildcat Lake is detected but untested. (By default, the IDs Mesa 26.3 lists, Panther Lake `0xB080`–`0xB087`, `0xB08F`, `0xB090`, `0xB0A0`, `0xB0B0` and Wildcat Lake `0xFD80`, `0xFD81` (`layer/gpu-ids.txt`), use the unpatched driver + Vulkan layer; other IDs of these ranges use the patched driver.)
* **Xe2** (Battlemage `0xE200`–`0xE2FF`, Lunar Lake `0x6420`–`0x64FF`): tested by a user on an Arc B580 with the patched driver ([#16](https://github.com/HHHHanasak1/xess-xmx-linux/issues/16)); Lunar Lake is detected but untested. (By default, the Battlemage and Lunar Lake IDs that Mesa 26.3 lists now use the unpatched driver + Vulkan layer, with every kernel compiled for 128 GRF. This stock mode is untested on real Xe2 hardware so far; `XMX_DRIVER=patched ./install.sh` goes back to the patched driver.)
* **Xe-HPG / Xe-LPG+** (Alchemist `0x5690`–`0x56FF`, Arrow Lake-H `0x7D51` / `0x7DD1`): tested by a user on an Arc A310 ([#28](https://github.com/HHHHanasak1/xess-xmx-linux/issues/28)); Arrow Lake-H is detected but untested.

Xe-LPG (Meteor Lake) has no XMX units. Only the native Linux Steam client is supported, not Flatpak Steam. Full system requirements are in [docs/install.md](docs/install.md).

The project is developed on an ONEXPLAYER 3 (Arc B390) with SteamOS, Mesa 26.1.2 / 26.3, and GE-Proton 11-6 / Proton Experimental.

## What It Gives You

* **Native XeSS and XeSS-FG on XMX (`igdext64.dll` + Mesa ANV / `VK_LAYER_XESS_xmx`):** `igdext64.dll` answers XeSS's driver queries and hands every C-for-Metal (CM, Intel's GPU kernel language) kernel over as a placeholder. It is built on Intel's open-source [dxvk-igdext](https://github.com/GameTechDev/dxvk-igdext). The package's Mesa ANV driver or Vulkan layer then compiles those kernels with Intel's Graphics Compiler (IGC) on first use and runs them in place of the placeholders ([docs/drivers.md](docs/drivers.md), [docs/dev/how-it-works.md](docs/dev/how-it-works.md)). The Mesa ANV driver is `libvulkan_intel.so`, with 26.3 patches by @kas5852 ([#19](https://github.com/HHHHanasak1/xess-xmx-linux/issues/19)). The Vulkan layer is `libVkLayer_xess_xmx.so` on unpatched Mesa 26.3. It is the default on the Xe3 and Xe2 GPUs of `layer/gpu-ids.txt` (on Xe2 with every kernel compiled for 128 GRF; untested on real Xe2 hardware).
* **Multi-frame generation override (`ControlLib.dll`, `XESS_FG=<n>`):** Stand-in for Intel's Control Library that forces 2x, 3x, or 4x frame generation even in games whose menus only offer 2x. The shim raises it to 5x and 6x, beyond the 4x Intel supports, and experimentally to 7x and **8x** ([docs/frame-generation.md](docs/frame-generation.md)).
* **Low-latency frame pacing (`IGDEXT_XELL_CAP`):** Built on the Intel XeLL (Xe Low Latency) API, the shim prevents frames from queueing before the GPU while native XeSS frame generation is on, cutting frame-queue latency by 36–50% at the same frame rate ([docs/frame-generation.md](docs/frame-generation.md)). It is on by default, and `IGDEXT_XELL_CAP=off` turns it off.
* **Upgrading old XeSS in games (`XESS=latest`):** Temporarily replaces a game's older XeSS DLLs with Intel's XeSS SDK 3.0.2 libraries while the game runs, for games where DXVK hides the Intel GPU from XeSS older than 2.0.2.68, such as Cyberpunk 2077 ([docs/games.md](docs/games.md)).
* **FSR 3.1+ upscaling and frame generation on XeSS (`amd_fidelityfx_upscaler_dx12.dll`, `amd_fidelityfx_framegeneration_dx12.dll`, `FSR=xess`, `FSR_FG=xess`):** Runs a game's FidelityFX Super Resolution (FSR) 3.1+ calls on XeSS SR and XeSS-FG on XMX ([docs/frame-generation.md](docs/frame-generation.md)). A game with native XeSS should use that instead, because this route can flicker more.
* **XeSS-FG for FSR upscaling games without FSR FG (`dxgi.dll`, `FSR=xess FSR_FG=inject`):** Injects an XeSS-FG swap chain fed by the FSR 3.1+ upscaler into games whose own frame generation is NVIDIA DLSS-G only, such as Forza Horizon 6 ([docs/frame-generation.md](docs/frame-generation.md)).
* **XeSS in D3D11 games (`libxess_dx11.dll`, `XESS=latest`):** Runs D3D12 XeSS on XMX alongside the game's D3D11 device (Intel's stock D3D11 DLL forwards to the Windows driver and does nothing under Proton), including with OptiScaler in D3D11 games ([docs/games.md](docs/games.md#xess-in-a-d3d11-game)).
* **XeSS in Vulkan games (`vulkan-1.dll`, `XESS=latest`):** Provides the private Intel Vulkan interfaces XeSS looks for. With them, Vulkan games run XeSS SR on XMX. On the Xe3 GPUs of `layer/gpu-ids.txt`, and on its Xe2 GPUs in stock mode, `VK_LAYER_XESS_xmx` runs the kernels. On Xe-HPG, other Xe3 IDs and Xe2 with the patched driver, the patched driver runs them (not yet tested on Xe-HPG or Xe2) ([docs/games.md](docs/games.md#xess-in-a-vulkan-game)).
* **Optical-flow frame generation for any DX12 game (`OFLOW_FG=1`):** Runs XeSS-FG on XMX in DX12 games that have neither XeSS nor FSR 3.1+ by computing motion vectors from FidelityFX optical flow on the final image, similar to driver-level frame generation such as AMD Fluid Motion Frames and NVIDIA Smooth Motion. It is meant for 2x; 3x and higher are experimental ([docs/frame-generation.md](docs/frame-generation.md)).

**OptiScaler** works normally on top of this package: XeSS and XeSS-FG chosen in OptiScaler run on XMX (in D3D11 games through `XESS=latest`). Set `Dxgi=false` under `[Spoofing]` in `OptiScaler.ini`; with OptiScaler's default NVIDIA spoofing, XeSS sees an NVIDIA GPU and falls back to DP4a.

## Quick Install

1. Download the release archive and unpack it in a permanent folder (to move it later, run `./uninstall.sh` first).
2. Run the installer once (fetches the IGC compiler bundle ~95 MB, Intel's XeSS SDK libraries ~77 MB, and AMD's FidelityFX loader and providers ~87 MB):
   ```bash
   ./install.sh
   ```
3. Reboot or log out and back in so Steam picks up the session environment.
4. Start your game in Steam on its **DX12** renderer and enable XeSS (and XeSS Frame Generation). For a D3D11 or Vulkan game, or a game with old XeSS libraries, set `XESS=latest %command%` in the game's Steam launch options. The first launch compiles the game's kernels into `~/.cache/xess-xmx`, which takes a minute or two once.

To revert everything, run `./uninstall.sh`. For per-game installation (`enable.sh` / `disable.sh` with `xmx-launch.sh`), running a single game on your distribution's stock Mesa driver (`xmx-stock.sh`), or handling root-owned Proton packages, see [docs/install.md](docs/install.md).

## Performance

Measured on an ONEXPLAYER 3 (Arc B390, Xe3, SteamOS, 1920x1200, 27 W SoC) using built-in benchmarks and `XESS=latest`. "Without this project" is Mesa 26.1.2 with XeSS on its generic DP4a path; 26.3 is this project's Mesa 26.3.0-rc3 (release 20261104):

| Configuration | Cyberpunk 2077 | Forza Horizon 6 |
|---|---|---|
| Without this project (26.1.2 + DP4a) | 47.7 fps | 43 fps (GPU 47.3) |
| 26.3 + XMX | 61.3 fps, +28% | 46 fps, +7% (GPU 55.0, +16%) |
| 26.3 + XMX + frame generation 3x | 117.0 fps, about 2.5x | 98 fps, about 2.3x |

* XMX itself is about 5% faster than DP4a in both games; most of the gain from 26.1.2 to 26.3 comes from **Jay**, Mesa's new shader compiler backend (default on Xe2 and Xe3). Forza Horizon 6 is held back by its CPU render thread on this device, so overall frame rate rises less than GPU frame rate. Final Fantasy VII Rebirth gains about +15% ([#19](https://github.com/HHHHanasak1/xess-xmx-linux/issues/19), half from Jay).
* With native XeSS frame generation on, the shim's low-latency mode cuts frame-queue latency by 36–50% at the same frame rate (tested in Wuthering Waves at 3x: 69.0 -> 34.8 ms GPU-limited, 76.1 -> 47.8 ms CPU-limited, 105.5 -> 67.2 ms render-thread-limited).
* The unpatched driver with `VK_LAYER_XESS_xmx`, the default on Xe3 and Xe2 (measured here with Mesa 26.3.0-rc3), performs like the patched driver in Cyberpunk 2077 (118.9 fps) and faster in Forza Horizon 6 (103 fps), the two games in which both were measured.

Full settings and benchmark tables are in [docs/performance.md](docs/performance.md).

## Known Limitations

* **XeLL driver mode:** Intel XeLL runs in its cross-vendor context under Proton because its driver mode checks for Wine and does not start. The shim's low-latency mode paces native XeSS-FG games on top of XeLL's frame reports ([docs/dev/how-it-works.md](docs/dev/how-it-works.md)).
* **Fallback behavior:** If the self-test fails or a kernel fails to compile, the shim declines the Intel extension context so XeSS uses its generic DP4a path. Generic XeSS frame generation is limited to 2x, so games set to 3x or 4x run at 2x until the XMX path works again.
* **Multipliers above 4x:** `XESS_FG=5` to `8` raise the interpolation count by modifying a value inside `libxess_fg.dll` in memory while the game runs. Intel does not support more than 4x; if the change cannot be verified, the game stays at 4x. 7x and 8x are experimental: they add a lot of latency.
* **vkd3d-proton and Mesa internals:** The driver patches (for Mesa 26.1.2 and 26.3.0) and the Vulkan layer (for Mesa 26.1, 26.3, and 27.0 on the Xe3 and Xe2 GPUs of `layer/gpu-ids.txt`) rely on how vkd3d-proton compiles placeholder shaders and lays out its classic descriptor heap (`VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer,VK_KHR_present_wait2`). If a vkd3d-proton update changes the first, the self-test turns XMX off; if it changes the second, the driver logs `no vkd3d-style descriptor heap` and kernels run as no-ops. Not measured yet: image quality, and how much the game itself slows down because vkd3d-proton runs without `VK_EXT_descriptor_buffer`.

## Documentation

### User Guides

* **[docs/install.md](docs/install.md):** System-wide install, updating, uninstalling, what `install.sh` does, requirements, per-game setup (`enable.sh` / `xmx-launch.sh`), and running one game on the system's driver (`xmx-stock.sh`).
* **[docs/drivers.md](docs/drivers.md):** The three drivers in the archive (unpatched 26.3.0 + layer, patched 26.3.0, patched 26.1.2), how to switch (`XMX_DRIVER`, `MESA=`, `tools/switch-driver.sh`), the pre-session check, and rebuilding after an OS update (`tools/rebuild-driver.sh`).
* **[docs/games.md](docs/games.md):** What to do for your game, by what it ships (XeSS version, FSR, D3D11, Vulkan); tested games; `XESS=latest`; FSR 4 on XMX notes.
* **[docs/frame-generation.md](docs/frame-generation.md):** Using `XESS_FG`, FSR on XeSS, optical-flow frame generation (`OFLOW_FG=1`), and low-latency pacing (`IGDEXT_XELL_CAP`). FSR on XeSS covers `FSR=xess`, `FSR_FG=xess`, `FSR_FG=inject`, `FSR_VENDOR=amd`, and `tools/fsr-fg.sh`.
* **[docs/launch-options.md](docs/launch-options.md):** Quick reference of every Steam launch option.
* **[docs/performance.md](docs/performance.md):** Benchmark and latency measurements.
* **[docs/troubleshooting.md](docs/troubleshooting.md):** Common symptoms and fixes, collecting logs for a bug report (`XESS_LOG=1`, `PROTON_LOG=1`), and when a game sees no Intel GPU.
* **[Game reports](https://github.com/HHHHanasak1/xess-xmx-linux/discussions/categories/game-reports):** How games run on other users' GPUs, and where to post your own results.

### Developer Documentation

* **[docs/dev/how-it-works.md](docs/dev/how-it-works.md):** Internals of the shim, DXBC placeholders, runtime compilation, driver patches, the Vulkan layer (`layer/README.md`), kernel variants (`SIMD16Required`), XeLL, the D3D11 bridge, the Vulkan wrapper, FSR translation, and optical flow.
* **[docs/dev/building.md](docs/dev/building.md):** Building every Windows and Linux binary from source, maintaining the Mesa patch series, and packaging releases.
* **[docs/dev/debugging.md](docs/dev/debugging.md):** Complete reference of internal environment switches, driver debug tools (`ANV_CM_*`), and the XeLL driver-mode probe (`tools/xell-probe.sh`).

## License

MIT (see [LICENSE](LICENSE)). The shim is based on dxvk-igdext (MIT), and the patches apply to Mesa 3D (MIT). The release archive contains no Intel kernels; they are compiled on your machine from the SPIR-V inside each game's XeSS libraries. See [THIRD_PARTY.md](THIRD_PARTY.md) for third-party licenses.
