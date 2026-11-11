# Games

After you run `install.sh` ([install.md](install.md)), any Direct3D 12 (D3D12) game that ships a recent Intel XeSS (Xe Super Sampling, Intel's upscaler and frame generator) runs on your GPU's XMX (Xe Matrix Extensions, hardware matrix engines) units automatically. You do not need launch options for those games.

For every other case, find what your game ships in the list below. All launch options go into the game's Steam **Properties → General → Launch options**; [launch-options.md](launch-options.md) lists them all.

## What to Do for Your Game

* **D3D12 with XeSS 2.0.2.68 or newer:** Nothing. Turn on XeSS (and XeSS Frame Generation, if offered) in the game's menu. *Example: Wuthering Waves.*
* **D3D12 with XeSS older than 2.0.2.68** (XeSS runs on DP4a, or the XeSS Frame Generation option is missing): `XESS=latest %command%` ([below](#games-with-xess-older-than-20268-cyberpunk-2077)). *Example: Cyberpunk 2077.*
* **The menu offers only 2x frame generation:** Set the multiplier with `XESS_FG=`, for example `XESS_FG=3` ([frame-generation.md](frame-generation.md#setting-the-multiplier-xess_fg)). *Example: Cyberpunk 2077.*
* **No XeSS Frame Generation, but FSR 3.1+ frame generation:** `FSR=xess %command%` runs the game's FSR upscaling and frame generation on XeSS ([frame-generation.md](frame-generation.md#3-fsr-31-on-xess-fsrxess-and-fsr_fgxess)). *Example: CONTROL Resonant.*
* **No XeSS Frame Generation and no FSR frame generation, but FSR 3.1+ upscaling:** `FSR=xess FSR_FG=inject %command%` adds XeSS frame generation fed by the FSR upscaler ([frame-generation.md](frame-generation.md#4-a-game-with-fsr-upscaling-and-no-fsr-frame-generation-fsr_fginject)). *Example: Forza Horizon 6, whose own frame generation is NVIDIA DLSS-G only.*
* **Neither XeSS nor FSR 3.1+** (FSR 2, FSR 3.0, or FSR compiled into the game): `OFLOW_FG=1 %command%` adds experimental frame generation from optical flow on the final image; there is no XeSS upscaling in this case ([frame-generation.md](frame-generation.md#5-frame-generation-for-any-dx12-game-oflow_fg1-experimental)). *Examples: Resident Evil 4 (2023), Viewfinder.*
* **D3D11 with XeSS:** `XESS=latest %command%` ([below](#xess-in-a-d3d11-game)).
* **Vulkan with XeSS (Xe3 only):** `XESS=latest %command%` ([below](#xess-in-a-vulkan-game)). Xe3 here means `0xB080`–`0xB08F` / `0xFD80`–`0xFD8F`.

If the game still runs XeSS on DP4a or sees no Intel GPU, see [troubleshooting.md](troubleshooting.md).

## Tested Games

| Game | What Runs on XMX | Launch Options | Tested On |
|---|---|---|---|
| Wuthering Waves | XeSS SR, XeSS-FG | None | Arc B390 |
| Cyberpunk 2077 (2.31) | XeSS SR, XeSS-FG | `XESS=latest`, `XESS_FG=<n>` | Arc B390 |
| Forza Horizon 6 | XeSS SR, XeSS-FG from FSR upscaling | `FSR=xess FSR_FG=inject` | Arc B390 |
| CONTROL Resonant | XeSS SR, XeSS-FG from FSR | `FSR=xess` | Arc B390 |
| Resident Evil Requiem | XeSS-FG from FSR (XeSS SR too with `FSR=xess`) | `FSR=xess` or `FSR_FG=xess` | Arc B390 |
| Resident Evil 4 (2023) | XeSS-FG from optical flow (experimental) | `OFLOW_FG=1` | Arc B390 |
| Viewfinder | XeSS-FG from optical flow (experimental) | `OFLOW_FG=1`, `-force-d3d12` after `%command%` | Arc B390 |
| Satisfactory | XeSS SR | None | Arc A310 (Xe-HPG), by a user ([#28](https://github.com/HHHHanasak1/xess-xmx-linux/issues/28)) |

Wuthering Waves and Cyberpunk 2077 were tested on Mesa 26.1.2 and 26.3.0-rc1, Forza Horizon 6 with `FSR_FG=inject` on Mesa 26.3.0-rc2, Satisfactory on Mesa 26.3.0-rc1; the benchmarks in [performance.md](performance.md) used 26.1.2 and 26.3.0-rc3. Details for the FSR and optical-flow games are in [frame-generation.md](frame-generation.md); [#19](https://github.com/HHHHanasak1/xess-xmx-linux/issues/19) has a Final Fantasy VII Rebirth comparison (~15% faster on Mesa 26.3 + XMX than on 26.1.2 + DP4a).

Any other D3D12 game with XeSS 2.0.2.68 or newer works the same way (older XeSS: add `XESS=latest`): its kernels are compiled on first use and cached in `~/.cache/xess-xmx/kernels`. To share how a game runs on your GPU, working or not, post it in [Game reports](https://github.com/HHHHanasak1/xess-xmx-linux/discussions/categories/game-reports). If a game does not work, please also open an issue with the `XESS_LOG=1` log (`C:\igdext_trace.log`) ([troubleshooting.md](troubleshooting.md)).

---

## Games With XeSS Older Than 2.0.2.68 (Cyberpunk 2077)

### Quick Setup

1. In Steam, open the game's **Properties → General → Launch options** and set:
   ```text
   XESS=latest %command%
   ```
   To change the frame generation multiplier, add `XESS_FG=`, for example 3x:
   ```text
   XESS=latest XESS_FG=3 %command%
   ```
2. Start the game and enable **XeSS** and **XeSS Frame Generation** in the game's graphics settings.

While the game runs, `tools/proton-hook.py` replaces the game's older `libxess.dll`, `libxess_fg.dll`, and `libxell.dll` with links to Intel's XeSS SDK 3.0.2 libraries (`xess/`: XeSS 2.0.2.68, XeSS-FG 1.3.1.78, XeLL 1.3.2.10) and restores the game's original files when it exits ([launch-options.md](launch-options.md)). Tested in Cyberpunk 2077 2.31 on an Arc B390: super resolution and frame generation, including multi-frame generation, run on XMX.

### Why Old XeSS Needs `XESS=latest`

When a game loads a `libxess.dll` older than 2.0.2.68, DXVK's DXGI (DirectX Graphics Infrastructure) layer activates a built-in workaround (`isXessVendorWaNeeded()` in DXVK's `dxgi_options.cpp`, which has no option to turn it off) and reports your Intel GPU as an AMD GPU. In the Proton log (`PROTON_LOG=1`), this appears as:

```text
XeSS: hiding Intel GPU Vendor ID
```

Because the game sees no Intel GPU, XeSS never loads `igdext64.dll` (the shim's trace log stays empty) and runs its generic DP4a fallback, and because XeSS-FG before 1.2 runs only on Intel GPUs, games with it hide the XeSS Frame Generation option completely. Forcing DXVK to report the Intel vendor ID to the old libraries (`DXVK_CONFIG="dxgi.customVendorId = 8086"`) does not work: Cyberpunk 2077's `libxess_fg.dll` 1.1.0.19 crashes at startup.

Mesa has a second mechanism that hides the Intel vendor ID in certain games: `force_vk_vendor=-1` in its `drirc` profiles. `install.sh` and `enable.sh` override it with `force_vk_vendor=0`. See [troubleshooting.md](troubleshooting.md).

### Replacing the Files Manually (Without the Hook)

If you use a per-game setup (`enable.sh`) or a Proton without the hook, you can replace the game's XeSS DLLs by hand. In Cyberpunk 2077, these are `libxess.dll`, `libxess_dx11.dll`, `libxess_fg.dll`, and `libxell.dll` in `bin/x64/`. Back up the originals first (a game update or Steam's "Verify integrity of game files" restores the old files). You can get a 2.0.2.68 set from:

* `<package>/xess/` (downloaded by `install.sh` or `tools/get-xess.sh`), Intel's XeSS SDK release, or another game that ships 2.0.2.68;
* Proton: launch the game once with `PROTON_XESS_UPGRADE=1 %command%` so Proton downloads a newer set into the prefix (`drive_c/windows/system32/umu/`), then copy those four files into the game folder. Proton's redirect in `umu/` is not enough by itself because the game loads `libxess.dll` from its own folder.

---

## XeSS in a D3D11 Game

Intel's stock `libxess_dx11.dll` contains no upscaler: it forwards every call to the Intel Windows D3D11 driver, which does not exist under Proton, so D3D11 games get no XeSS at all (on Windows, Intel supports D3D11 XeSS only on Arc GPUs).

To run XeSS on XMX in a D3D11 game:

1. Set the game's Steam launch options to:
   ```text
   XESS=latest %command%
   ```
2. Enable **XeSS** in the game's graphics settings.

While the game runs, `XESS=latest` replaces every `libxess_dx11.dll` in the game folder with this package's D3D11-to-D3D12 bridge (`dx11bridge/libxess_dx11.dll`), regardless of the game file's version, and links the XeSS SDK's `libxess.dll` next to it if the folder does not already have one. The bridge creates a companion D3D12 device on the same GPU, copies the frame's inputs across shared textures (converting depth via a small compute shader), runs D3D12 XeSS on XMX through the shim and driver, and copies the upscaled output back to D3D11.

The cross-device texture copies cost a small amount of memory bandwidth per frame; nothing else changes. This also works with **OptiScaler's** XeSS in a D3D11 game (`Dx11Upscaler=xess`), which loads `libxess_dx11.dll` from the game folder. Tested with the XeSS SDK's D3D11 sample on an Arc B390 (`XESS_DX11_TRACE=1` logs each call; `XESS_DX11_DUMP=<n>` writes the output of execute `n` next to the DLL).

For the internal design of the D3D11 bridge, see [dev/how-it-works.md](dev/how-it-works.md).

---

## XeSS in a Vulkan Game

XeSS's Vulkan backend only runs its XMX kernels when the Vulkan driver provides private Intel interfaces outside the Khronos Vulkan registry. Wine (`winevulkan`) only passes standard registry functions and structures through, so under stock Proton XeSS logs `Unable to initialize Intel Extensions layer. Using suboptimal implementation.` and falls back to DP4a.

To run XeSS on XMX in a Vulkan game (on **Xe3** GPUs `0xB080`–`0xB08F` and `0xFD80`–`0xFD8F`):

1. Set the game's Steam launch options to:
   ```text
   XESS=latest %command%
   ```
   Some games load `vulkan-1.dll` dynamically at runtime. They call `LoadLibrary` instead of listing `vulkan-1.dll` in their `.exe` import table. If such a game stays on DP4a with `XESS=latest` alone, add `XESS_VK=1`:
   ```text
   XESS=latest XESS_VK=1 %command%
   ```
   (To prevent `XESS=latest` from placing `vulkan-1.dll` in a non-Vulkan game where it gets in the way, set `XESS_VK=0`.)
2. Enable **XeSS** in the game's graphics settings.

When `XESS=latest` detects a game that has `libxess.dll` and imports `vulkan-1.dll`, or when `XESS_VK=1` is set, the hook does two things while the game runs. It places the package's `vkwrap/vulkan-1.dll` next to the game executable, and it enables `VK_LAYER_XESS_xmx` (`layer/`). This `vulkan-1.dll` forwards standard Vulkan calls to Wine's `vulkan-1.dll`. It intercepts XeSS's private Intel queries, shader module options, and descriptor pushes, and hands the kernels to the package's Vulkan layer.

Notes and requirements for Vulkan games:
* **Xe3 GPUs `0xB080`–`0xB08F` / `0xFD80`–`0xFD8F` only:** `VK_LAYER_XESS_xmx` serves only these. On Xe-HPG, Xe2 and other Xe3 IDs, the layer does not answer `vulkan-1.dll`'s self-test (`layer_canary`), so `vulkan-1.dll` reports the Intel path as unavailable and XeSS stays on DP4a.
* **Unpatched and patched drivers:** The game keeps the driver your session or `MESA=` selected; `VK_LAYER_XESS_xmx` serves Vulkan XeSS on both the unpatched `stock-26.3.0` driver and the patched drivers (on a patched driver it leaves D3D12 games to the driver).
* **No Khronos validation layer:** XeSS skips its Intel path whenever `VkLayer_khronos_validation.dll` is loaded.
* **Logging:** `VKWRAP_LOG=1` writes `vkwrap.log` next to `vulkan-1.dll`.

Tested with the XeSS SDK's Vulkan sample and `vkwrap/test/vktest.exe` (low-resolution motion vectors and depth, output verified) on an Arc B390: all 63 kernels run on XMX (~7 ms per frame for 1080p output at Performance). No retail Vulkan game with XeSS has been tested yet.

For the internal design of `vkwrap/vulkan-1.dll` and `VK_LAYER_XESS_xmx`, see [dev/how-it-works.md](dev/how-it-works.md).

---

## FSR 4 on XMX (Upscaler Builds from the OptiScaler Discord)

**No longer developed.** This project no longer actively develops FSR 4 on XMX. Visual quality differences were hard to judge on a handheld's small screen, it brought no significant performance gain over XeSS on XMX, it adds setup complexity, and combining it with XeSS multi-frame generation required per-game image fixes—which goes against this project's goal of working across games without per-game workarounds. FSR 3.1+ games are supported through `FSR=xess` instead ([frame-generation.md](frame-generation.md)). The notes below are kept for manual setup (tested with one build on one GPU in two games).

Experimental FSR 4 upscaler builds shared on the OptiScaler Discord server run FSR 4 as C-for-Metal (CM) kernels through Intel's D3D12 extension, so they work through this package's shim and driver (using `patches/0005-anv-cm-sampler-descriptor-table.patch`, which supports CM kernels whose sampler is in a descriptor table; older drivers without patch 0005 render corrupted images). The build tested here is "B Boost" (FileVersion `4.0.2.1`); other builds may behave differently.

### Manual Setup

In the game folder that contains `amd_fidelityfx_upscaler_dx12.dll` (back up the original file first):

1. Rename the downloaded plugin DLL to `amd_fidelityfx_upscaler_dx12.dll` (OptiScaler itself is not required; if you use OptiScaler, replace the `amd_fidelityfx_upscaler_dx12.dll` that OptiScaler loads).
2. Copy `Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll` from AMD's FSR SDK 2.4.0 (FSR 4.1.2) into the same folder and rename it to `fsr-original.dll`. (`install.sh` and `tools/get-ffx.sh` already download this file to `<package>/ffx/amd_fidelityfx_upscaler_dx12.dll`, so you can copy it from there.) The plugin reads the FSR 4 weights from `fsr-original.dll`.
3. Copy `C:\Windows\System32\cabinet.dll` from a Windows installation into the same folder, and add `WINEDLLOVERRIDES=cabinet=n,b` to the game's launch options. The plugin decompresses its kernels using the Windows Cabinet compression API (`Cabinet.dll` ordinals 40, 43, and 45), which Wine does not implement; without native `cabinet.dll`, the game aborts with `unimplemented function Cabinet.dll.40` (Proton Experimental) or hangs (GE-Proton).
4. If the Proton log says `igdext64.dll with required DX12 exports was not found` (for example, in a prefix where `install.sh` has not created the driver store folder), copy `<package>/igdext64.dll` next to the plugin.

### Plugin Environment Variables and Benchmarks

* `FSR4_XMX_NO_DIALOG=1`: prevents the plugin's error dialog from blocking the game.
* `FSR4_XMX_LITE=1`: selects the Lite model (Cyberpunk 2077 picks Lite by default).
* `FSR4_XMX_PROVIDER_INDEX=0`: selects the full model (`4.0.2X`; CONTROL Resonant picks the full model by default).

Cyberpunk 2077 only shows the "FSR 4" option in its menu when this plugin (or `FSR=xess`) is in place, because AMD's own DLL reports FSR 4 as unsupported on Intel GPUs. Measured on an Arc B390 (25 W in CONTROL Resonant, 27 W in Cyberpunk 2077; 1280x800 render resolution, no frame generation):

| Upscaler | Cyberpunk 2077 benchmark (average fps) | CONTROL Resonant, fixed spot (fps) |
|---|---|---|
| XeSS, Ultra Quality (XMX) | 57.5 | — |
| FSR 3 (the game's DLL) | Not offered | 36 |
| FSR 4 on XMX, full model | 56.4 | 35 |
| FSR 4 on XMX, Lite | 58.2 / 58.1 | 36 |

On the Arc B390, the FSR 4 kernels take 2.5 ms (full model) or 1.9 ms (Lite model) per frame. Native FSR frame generation stays at 2x because AMD has not released multi-frame generation.
