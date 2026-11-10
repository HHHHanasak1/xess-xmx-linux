# Games

After you run `install.sh` ([install.md](install.md)), any Direct3D 12 (D3D12) game that ships a recent Intel XeSS (Xe Super Sampling, Intel's upscaler and frame generator) runs on your GPU's XMX (Xe Matrix Extensions, hardware matrix engines) units automatically. You do not need launch options for those games.

Use the guides below when a game ships an older XeSS library, uses Direct3D 11 (D3D11) or Vulkan, or lacks XeSS Frame Generation (XeSS-FG). For all frame generation options (`XESS_FG`, `FSR=xess`, `FSR_FG=inject`, and `OFLOW_FG=1`), see [frame-generation.md](frame-generation.md). For a list of every launch option, see [launch-options.md](launch-options.md).

## Tested Games

| Game | XeSS Version | XeSS SR on XMX | XeSS FG on XMX | Setup | Notes |
|---|---|---|---|---|---|
| Wuthering Waves | 2.0.2.68, XeFG 1.3.1.78 | Yes | Yes, 2x / 3x / 4x | `install.sh` | The XeLL (Xe Low Latency) 30 fps frame-cap fix applies automatically here ([frame-generation.md](frame-generation.md)). |
| Forza Horizon 6 | 2.0.2.68 | Yes | Yes, from FSR upscaling: `FSR=xess FSR_FG=inject %command%` ([frame-generation.md](frame-generation.md)); or via OptiScaler ([below](#frame-generation-in-a-game-without-xess-fg-optiscaler)) | `install.sh` | The game's own frame generation menu option is NVIDIA DLSS-G only. |
| Cyberpunk 2077 (2.31) | Ships 2.0.1.41, XeFG 1.1.0.19 | Yes, with newer XeSS libraries | Yes, with newer XeSS libraries | `install.sh` + `XESS=latest %command%` ([below](#a-game-that-ships-xess-older-than-20268-cyberpunk-2077)); `XESS_FG=4` for 4x | With the shipped libraries: runs on DP4a (dot-product fallback) and hides the XeSS FG option. The game menu offers 2x; `XESS_FG=4` gives 4x, and `XESS_FG=5`..`8` give up to 8x (experimental). |
| Resident Evil 4 (RE Engine) | None | Yes, technically | No (or 2x via `OFLOW_FG=1`, [frame-generation.md](frame-generation.md)) | REFramework ([below](#a-game-without-xess-at-all-resident-evil-4-re-engine)) | REFramework XeSS SR has blurry text and flicker, worse than the game's FSR 2; not recommended. |
| Satisfactory | XeSS + XeFG (Unreal plugin 2.0.10) | Yes | Not reported | `install.sh` | Tested by a user on an Arc A310 (Xe-HPG, Mesa 26.3.0-rc1, #28). |

All other games in the table were tested on an Arc B390 (Xe3) on Mesa 26.1.2; Wuthering Waves and Cyberpunk 2077 were also tested on Mesa 26.3.0-rc1 (the default driver at the time; the default is now 26.3.0). Forza Horizon 6 with `FSR_FG=inject` was tested on Mesa 26.3.0-rc2. See [frame-generation.md](frame-generation.md) for games tested with FSR on XeSS (CONTROL Resonant, Resident Evil Requiem) and optical flow frame generation (Resident Evil 4, Viewfinder), and #19 for Final Fantasy VII Rebirth (~15% faster on Mesa 26.3 + XMX vs 26.1.2 + DP4a).

Any other D3D12 game with XeSS 1.3 or newer works the same way: its kernels are compiled on first use and cached in `~/.cache/xess-xmx/kernels`. Please report games you test (working or not) in the issue tracker, with the `IGDEXT_TRACE=1` (`XESS_LOG=1`) log ([troubleshooting.md](troubleshooting.md)).

---

## A Game That Ships XeSS Older Than 2.0.2.68 (Cyberpunk 2077)

### Quick Setup

1. In Steam, open the game's **Properties → General → Launch options** and set:
   ```text
   XESS=latest %command%
   ```
   To also run 4x frame generation (or 3x..8x), add `XESS_FG=4`:
   ```text
   XESS=latest XESS_FG=4 %command%
   ```
2. Start the game and enable **XeSS** and **XeSS Frame Generation** in the game's graphics settings.

While the game runs, `tools/proton-hook.py` replaces the game's older `libxess.dll`, `libxess_fg.dll`, and `libxell.dll` with links to Intel's XeSS SDK 3.0.2 libraries (`xess/`: XeSS 2.0.2.68, XeSS-FG 1.3.1.78, XeLL 1.3.2.10) and restores the game's original files when it exits ([launch-options.md](launch-options.md)). Tested in Cyberpunk 2077 2.31 on an Arc B390: super resolution and frame generation (2x in the menu, 4x and up to 8x with `XESS_FG`) run on XMX.

### Why Old XeSS Needs `XESS=latest`

When a game loads a `libxess.dll` older than 2.0.2.68, DXVK's DXGI (DirectX Graphics Infrastructure) layer activates a built-in workaround (`isXessVendorWaNeeded()` in DXVK's `dxgi_options.cpp`, which has no option to turn it off) and reports your Intel GPU as an AMD GPU. In the Proton log (`PROTON_LOG=1`), this appears as:

```text
XeSS: hiding Intel GPU Vendor ID
```

Because the game sees no Intel GPU, XeSS never loads `igdext64.dll` (the shim's trace log stays empty) and runs its generic DP4a fallback, and games with XeSS-FG older than 1.2 (which was Intel-only) hide the XeSS Frame Generation option completely. Forcing DXVK to report the Intel vendor ID to the old libraries (`DXVK_CONFIG="dxgi.customVendorId = 8086"`) does not work: Cyberpunk 2077's `libxess_fg.dll` 1.1.0.19 crashes at startup.

*(Mesa has a second mechanism that hides the Intel vendor ID in certain games via `force_vk_vendor=-1` in its `drirc` profiles; `install.sh` and `enable.sh` override it with `force_vk_vendor=0`. See [troubleshooting.md](troubleshooting.md).)*

### Replacing the Files Manually (Without the Hook)

If you use a per-game setup (`enable.sh`) or a Proton without the hook, you can replace the game's XeSS DLLs by hand. In Cyberpunk 2077, these are `libxess.dll`, `libxess_dx11.dll`, `libxess_fg.dll`, and `libxell.dll` in `bin/x64/`. Back up the originals first (a game update or Steam's "Verify integrity of game files" restores the old files). You can get a 2.0.2.68 set from:

* `<package>/xess/` (downloaded by `install.sh` or `tools/get-xess.sh`), Intel's XeSS SDK release, or another game that ships 2.0.2.68;
* Proton: launch the game once with `PROTON_XESS_UPGRADE=1 %command%` so Proton downloads a newer set into the prefix (`drive_c/windows/system32/umu/`), then copy those four files into the game folder. Proton's redirect in `umu/` is not enough by itself because the game loads `libxess.dll` from its own folder.

---

## XeSS in a D3D11 Game

Intel's stock `libxess_dx11.dll` contains no upscaler: it forwards every call to the Intel Windows D3D11 driver, which does not exist under Proton, so D3D11 games get no XeSS at all (and Intel only supports it on Arc GPUs on Windows).

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

To run XeSS on XMX in a Vulkan game (on **Xe3** GPUs):

1. Set the game's Steam launch options to:
   ```text
   XESS=latest %command%
   ```
   If the game loads `vulkan-1.dll` dynamically at runtime (via `LoadLibrary` instead of listing `vulkan-1.dll` in its `.exe` import table) and stays on DP4a with `XESS=latest` alone, add `XESS_VK=1`:
   ```text
   XESS=latest XESS_VK=1 %command%
   ```
   (To prevent `XESS=latest` from placing `vulkan-1.dll` in a non-Vulkan game where it gets in the way, set `XESS_VK=0`.)
2. Enable **XeSS** in the game's graphics settings.

When `XESS=latest` detects a game that has `libxess.dll` and imports `vulkan-1.dll` (or when `XESS_VK=1` is set), the hook places the package's `vkwrap/vulkan-1.dll` next to the game executable while it runs and enables `VK_LAYER_XESS_xmx` (`layer/`). `vulkan-1.dll` forwards standard Vulkan calls to Wine's `vulkan-1.dll`, intercepts XeSS's private Intel queries, shader module options, and descriptor pushes, and hands the kernels to the package's Vulkan layer.

Notes and requirements for Vulkan games:
* **Xe3 GPUs only:** `VK_LAYER_XESS_xmx` supports Xe3 only. On Xe-HPG and Xe2, the layer does not answer `vulkan-1.dll`'s self-test (`layer_canary`), so `vulkan-1.dll` reports the Intel path as unavailable and XeSS stays on DP4a.
* **Works on both unpatched and patched drivers:** The game keeps whichever package driver your session or `MESA=` option selected (`VK_LAYER_XESS_xmx` serves Vulkan games on both the unpatched `stock-26.3.0` driver and the patched drivers, while leaving D3D12 games to the patched driver when one is active).
* **No Khronos validation layer:** XeSS skips its Intel path whenever `VkLayer_khronos_validation.dll` is loaded.
* **Logging:** `VKWRAP_LOG=1` writes `vkwrap.log` next to `vulkan-1.dll`.

Tested with the XeSS SDK's Vulkan sample and `vkwrap/test/vktest.exe` (low-resolution motion vectors and depth, output verified) on an Arc B390: all 63 kernels run on XMX (~7 ms per frame for 1080p output at Performance). No retail Vulkan game with XeSS has been tested yet.

For the internal design of `vkwrap/vulkan-1.dll` and `VK_LAYER_XESS_xmx`, see [dev/how-it-works.md](dev/how-it-works.md).

---

## Frame Generation in a Game Without XeSS-FG (OptiScaler)

Forza Horizon 6 ships XeSS Super Resolution and FSR upscaling, but its built-in frame generation option is NVIDIA DLSS-G only. The simplest way to get XeSS Frame Generation in Forza Horizon 6 is this package's `FSR=xess FSR_FG=inject %command%` ([frame-generation.md](frame-generation.md)), which requires no third-party files.

Alternatively, you can use **OptiScaler** to add XeSS-FG on top of the game's own XeSS SR:

1. From an OptiScaler release, copy `OptiScaler.dll` (renamed to `dxgi.dll`), `libxess_fg.dll`, `libxell.dll`, `fakenvapi.dll`, and `fakenvapi.ini` into the game folder. **Do not** copy OptiScaler's `libxess.dll`; keep the game's own copy.
2. Create `OptiScaler.ini` in the game folder with these settings:
   ```ini
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
   ```
   `Dxgi=false` is required: with OptiScaler's default NVIDIA DXGI spoofing enabled, `libxess.dll` sees an NVIDIA adapter and falls back to DP4a. To run 3x or 4x frame generation, set `InterpolationCount=2` or `3` under `[XeFG]`.
3. Set the game's Steam launch options to load the local `dxgi.dll`:
   ```text
   WINEDLLOVERRIDES="dxgi=n,b" %command%
   ```
   (Or place `export WINEDLLOVERRIDES="dxgi=n,b"` in `~/.config/xess-xmx.conf` when using `xmx-launch.sh`.)

Measured in Forza Horizon 6 on an Arc B390: XeFG driven by OptiScaler runs on XMX (~28 real fps -> ~56 presented fps at 2x, with a stable HUD).

**Caveat:** In Forza Horizon 6 with HDR enabled and a high car level of detail, OptiScaler's swap chain hooking caused crashes (a double `vkDestroyImage`) after a few minutes in the online world. Without OptiScaler, the game runs without crashing.

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

---

## A Game Without XeSS at All (Resident Evil 4, RE Engine)

RE Engine games ship a custom built-in FSR 2 that OptiScaler cannot hook.

For frame generation in Resident Evil 4 (2023), use this package's experimental optical flow frame generation (`OFLOW_FG=1 %command%`, see [frame-generation.md](frame-generation.md)).

For super resolution, REFramework's `pd-upscaler` build combined with PureDark's UpscalerBasePlugin 1.1.2 and a 2.0.2.68 `libxess.dll` in the game folder (`WINEDLLOVERRIDES="dinput8=n,b"`) replaces the game's TAA with an XeSS call that runs on XMX. However, in Resident Evil 4 1.5.9 this produced blurry text and flicker—worse than the game's built-in FSR 2—and does not support frame generation. It is not worth setting up.
