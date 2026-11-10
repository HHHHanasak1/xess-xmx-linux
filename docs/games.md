# Games

This page covers specific games and how to configure them. In the table below, SR is super resolution (upscaling), FG is frame generation (XeFG), XMX is the hardware matrix engine on Intel Arc GPUs, and DP4a is the slower generic fallback path.

| Game | XeSS | XeSS SR on XMX | XeSS FG on XMX | How | Notes |
|---|---|---|---|---|---|
| Wuthering Waves | 2.0.2.68, XeFG 1.3.1.78 | yes | yes, 2x / 3x / 4x | `install.sh` | the XeLL (Intel Low Latency) frame-cap fix applies here |
| Forza Horizon 6 | 2.0.2.68 | yes | yes, from FSR upscaling: `FSR=xess FSR_FG=inject %command%` ([frame-generation.md](frame-generation.md)); or through OptiScaler (below) | `install.sh` | the game's own FG option is DLSS-G (NVIDIA's frame generation) |
| Cyberpunk 2077 (2.31) | ships 2.0.1.41, XeFG 1.1.0.19 | yes, with newer XeSS libraries | yes, with newer XeSS libraries | `install.sh` + `XESS=latest %command%` (below); `XESS_FG=4` for 4x | with the shipped libraries: DP4a, no XeSS FG option. The game offers 2x; `XESS_FG=4` gives 4x, `XESS_FG=5`..`8` up to 8x (experimental) |
| Resident Evil 4 (RE Engine) | none | yes, technically | no | REFramework (below) | worse than the game's FSR2; not recommended |
| Satisfactory | XeSS + XeFG (Unreal plugin 2.0.10) | yes | not reported | `install.sh` | tested by a user on an Arc A310 (Xe-HPG, Mesa 26.3.0-rc1, #28) |

The other games were tested on an Arc B390 (Xe3), all on Mesa 26.1.2. Wuthering Waves and Cyberpunk 2077 were also tested on Mesa 26.3.0-rc1, which was the default driver then (it is now 26.3.0). Forza Horizon 6 with `FSR_FG=inject` was tested on 26.3.0-rc2.

Any other game with XeSS 1.3 or later on its D3D12 (DirectX 12) renderer should work the same way. The driver compiles the GPU kernels (small programs that run on the GPU) on first use. Report games you tried (working or not) in the issue tracker, and attach the `IGDEXT_TRACE=1` log.

For games that use AMD's FSR 3.1+ instead of XeSS (such as CONTROL Resonant or Resident Evil Requiem), or DX12 games without any upscaler, see [frame-generation.md](frame-generation.md).

## A game that ships XeSS older than 2.0.2.68 (Cyberpunk 2077)

Put `XESS=latest %command%` in the game's Steam launch options:

```text
XESS=latest %command%
```

While the game runs, it uses the libraries from Intel's XeSS SDK (2.0.2.68, XeSS-FG 1.3.1.78, XeLL 1.3.2.10), and its own files come back when it exits ([launch-options.md](launch-options.md)).

Why this is needed: DXVK (Proton's D3D11 and DXGI layer) reports an Intel GPU as an AMD one when the `libxess.dll` loaded by the game is older than 2.0.2.68. This is a workaround for early XeSS 2.0 builds (`isXessVendorWaNeeded()` in DXVK's `dxgi_options.cpp`), and no option turns it off. The Proton log (`PROTON_LOG=1`) then says `XeSS: hiding Intel GPU Vendor ID`. Because XeSS sees no Intel GPU, it never loads the Intel extension library (the shim's trace stays empty) and runs its DP4a path. In addition, a game whose XeSS frame generation is older than 1.2 (which was Intel-only) does not offer frame generation at all.

Tested on Cyberpunk 2077 2.31: super resolution and frame generation run on XMX (the game menu offers 2x, while `XESS_FG=4` to `8` give higher multipliers). Forcing DXVK to show the Intel GPU to the game's original libraries (`dxgi.customVendorId = 8086`) does not work, because the game's bundled XeSS-FG 1.1.0.19 crashes at start.

### Replacing the libraries by hand

You can also replace the game's XeSS libraries with newer ones manually. In Cyberpunk 2077, these files are `libxess.dll`, `libxess_dx11.dll`, `libxess_fg.dll`, and `libxell.dll` in `bin/x64/`:

1. Back up the original files in the game folder. A game update or Steam's "Verify integrity of game files" puts the old ones back.
2. Get a 2.0.2.68 set of libraries from one of these sources:
   - Another game that ships them, or Intel's XeSS SDK release.
   - Proton: launch the game once with `PROTON_XESS_UPGRADE=1 %command%` to download a newer set into the Wine prefix (the virtual Windows drive, at `drive_c/windows/system32/umu/`).
3. Copy the four files into `bin/x64/`. Proton's redirect alone is not enough because the game loads `libxess.dll` from its own folder.

On an Arc B390, this enables super resolution and frame generation on XMX, and makes the XeSS frame generation option appear in the game menu.

Mesa (the Linux graphics driver collection) has a second mechanism that hides the Intel GPU in some games (`force_vk_vendor=-1` in its game profiles). `install.sh` overrides it; see [troubleshooting.md](troubleshooting.md).

## XeSS in a D3D11 game

Put `XESS=latest %command%` in the game's launch options:

```text
XESS=latest %command%
```

Intel's `libxess_dx11.dll` for D3D11 (DirectX 11) contains no upscaler of its own: it forwards every call to the Intel Windows driver, which does not exist under Proton, so a D3D11 game never gets XeSS (and Intel supports it on Arc GPUs only).

With `XESS=latest`, the hook replaces the game's `libxess_dx11.dll` with this package's bridge (`dx11bridge/`), which runs D3D12 XeSS from `libxess.dll` on XMX alongside the game's D3D11 device. Copying textures between D3D11 and D3D12 costs some memory bandwidth per frame; nothing else changes. The same applies to OptiScaler's XeSS in a D3D11 game (`Dx11Upscaler=xess`), which loads `libxess_dx11.dll` from the game folder.

Tested with the XeSS SDK's D3D11 sample on an Arc B390: XeSS super resolution runs on XMX through the bridge. For debugging, `XESS_DX11_TRACE=1` logs each call, and `XESS_DX11_DUMP=<n>` writes the output of frame `n` next to the DLL. For how the bridge works internally, see [dev/how-it-works.md](dev/how-it-works.md).

## XeSS in a Vulkan game

Put `XESS=latest %command%` in the game's launch options:

```text
XESS=latest %command%
```

Under Proton, Wine does not pass through the custom Intel Vulkan interfaces that XeSS looks for, so XeSS logs `Unable to initialize Intel Extensions layer. Using suboptimal implementation.` and falls back to DP4a.

When you set `XESS=latest` for a game whose executable imports `vulkan-1.dll` and has a `libxess.dll`, the package puts its own `vulkan-1.dll` (`vkwrap/`) next to the executable while the game runs. That wrapper and the package's Vulkan layer (a plugin that intercepts Vulkan calls) run the game's XeSS kernels on XMX.

Requirements and options:

- **Xe3 GPUs only:** The Vulkan layer serves Xe3 only, so on Xe-HPG and Xe2 a Vulkan game's XeSS stays on DP4a.
- **Drivers:** The game keeps its driver. The layer handles Vulkan games on both the unpatched and patched drivers of the package. Without the layer active, `vulkan-1.dll` leaves XeSS on DP4a.
- **Validation layer:** XeSS skips the Intel path whenever the Khronos validation layer is loaded.
- **Launch options:**
  - `XESS_VK=1`: enable the Vulkan wrapper for a game whose executable loads Vulkan at run time instead of importing `vulkan-1.dll`.
  - `XESS_VK=0`: disable the Vulkan wrapper when using `XESS=latest`.
  - `VKWRAP_LOG=1`: write `vkwrap.log` next to the DLL.

Tested with the XeSS SDK's Vulkan sample and a test program (`vkwrap/test/`, low-resolution motion vectors and depth, output checked) on an Arc B390: all 63 kernels run on XMX, taking about 7 ms per frame for 1080p output at Performance quality. No Vulkan game with XeSS has been tested yet. For internal details, see [dev/how-it-works.md](dev/how-it-works.md).

## Frame generation in a game without XeSS-FG (OptiScaler)

Forza Horizon 6 ships XeSS super resolution, but its frame generation is DLSS-G only. You can get XeSS frame generation without OptiScaler by using `FSR=xess FSR_FG=inject %command%` from the game's FSR upscaling ([frame-generation.md](frame-generation.md)).

Alternatively, OptiScaler (a tool that translates upscaler calls between vendors) can add XeFG on top of the game's own XeSS:

1. From the OptiScaler release, copy `OptiScaler.dll` (renamed to `dxgi.dll`), `libxess_fg.dll`, `libxell.dll`, `fakenvapi.dll`, and `fakenvapi.ini` into the game folder. Do not copy OptiScaler's `libxess.dll`; keep the game's own copy.
2. Put the following settings in `OptiScaler.ini`:

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

   Keep `Dxgi=false`: with the default NVIDIA spoof enabled, `libxess.dll` sees an NVIDIA adapter and takes its DP4a path. To get 3x or 4x frame generation, add `[XeFG] InterpolationCount=2` or `3`.
3. Start the game with `WINEDLLOVERRIDES="dxgi=n,b"` (for example in the launch options or through `xmx-launch.sh` and its config file).

XeFG driven this way runs on XMX too. On Forza Horizon 6, this gave ~28 real fps -> ~56 presented fps at 2x with a stable HUD.

Caveat: in Forza Horizon 6 with HDR on and a high car level of detail, OptiScaler's swapchain hooking led to crashes (a double `vkDestroyImage`) after a few minutes in the online world. Without OptiScaler, the game runs fine.

## FSR 4 on XMX (upscaler builds from the OptiScaler Discord)

**No longer developed.** This project no longer actively develops FSR 4 on XMX. Whether the image improved was not assessed: on a handheld's small screen, the difference is hard to see. It brought no significant performance gain, adds complexity, and conflicts with XeSS multi-frame generation (that combination ran, but requires per-game image fixes, which goes against this project's goal of avoiding game-specific adaptations like OptiScaler). The notes below remain for reference (a manual setup, nothing automated, one build tested on one GPU in two games). Games with FSR 3.1+ run on XMX through `FSR=xess` instead ([frame-generation.md](frame-generation.md)).

The OptiScaler Discord server shares builds of an FSR 4 upscaler that runs as C-for-Metal (CM, Intel's GPU kernel language) kernels through Intel's D3D12 extension, under various file names. Such a build goes through this project's shim and driver like XeSS does. The build tested here is "B Boost" (FileVersion 4.0.2.1); other builds may behave differently.

Whatever the downloaded file is named, it replaces the game's `amd_fidelityfx_upscaler_dx12.dll` (which the game loads through AMD's FFX loader). OptiScaler itself is not needed; if you use OptiScaler, replace the `amd_fidelityfx_upscaler_dx12.dll` that OptiScaler loads instead. This requires the driver fix for kernels whose sampler is a descriptor table (patch 0005); with an older driver, the image is wrong.

### Setup

In the folder containing the game's `amd_fidelityfx_upscaler_dx12.dll` (back up the original file first):

1. Place the downloaded plugin in the folder and rename it to `amd_fidelityfx_upscaler_dx12.dll`.
2. Copy `<package>/ffx/amd_fidelityfx_upscaler_dx12.dll` into the game folder and rename it to `fsr-original.dll`. The plugin reads the FSR 4 weights from this file, which is `Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll` from AMD's FSR SDK 2.4.0 (FSR 4.1.2) that `install.sh` fetched via `tools/get-ffx.sh`.
3. Copy `cabinet.dll` from a Windows installation (`C:\Windows\System32\cabinet.dll`) into the game folder, and add `WINEDLLOVERRIDES=cabinet=n,b` to the game's launch options. The plugin unpacks its kernels using the Windows compression API (`Cabinet.dll` ordinals 40, 43, 45), which Wine does not implement. Without it, the game aborts with `unimplemented function Cabinet.dll.40` (Proton Experimental) or hangs (GE-Proton).
4. If the Proton log says `igdext64.dll with required DX12 exports was not found` (which happens in a prefix without the driver store folder, such as a game `install.sh` has not covered), copy `<package>/igdext64.dll` next to the plugin.

### Options and performance

The plugin offers several FSR 4 versions and the game picks one: Cyberpunk 2077 picks the Lite model, while CONTROL Resonant picks the full one. You can control the plugin with environment variables in the launch options:

- `FSR4_XMX_NO_DIALOG=1`: prevent its error dialog from blocking the game.
- `FSR4_XMX_LITE=1`: request the Lite model.
- `FSR4_XMX_PROVIDER_INDEX=0`: request the full model (4.0.2X).

Cyberpunk 2077 shows the FSR 4 menu option only when the plugin is in place (AMD's own DLL reports FSR 4 as unsupported on Intel). Measured on an Arc B390 (25 W in CONTROL Resonant, 27 W in Cyberpunk 2077; render resolution 1280x800, no frame generation):

| | Cyberpunk 2077 benchmark, average fps | CONTROL Resonant, fixed spot |
|---|---|---|
| XeSS, Ultra Quality (XMX) | 57.5 | - |
| FSR 3 (the game's DLL) | not offered | 36 |
| FSR 4 on XMX, full model | 56.4 | 35 |
| FSR 4 on XMX, Lite | 58.2 / 58.1 | 36 |

The FSR 4 kernels take 2.5 ms (full) or 1.9 ms (Lite) per frame on the B390. FSR frame generation stays at 2x because AMD has not released multi-frame generation.

## A game without XeSS at all (Resident Evil 4, RE Engine)

RE Engine games ship a custom FSR2 implementation that OptiScaler cannot hook. Using REFramework's `pd-upscaler` build with PureDark's UpscalerBasePlugin 1.1.2 and a `libxess.dll` 2.0.2.68 in the game folder (with the `dinput8=n,b` DLL override) replaces the game's TAA (temporal anti-aliasing) with an XeSS call, which runs on XMX.

In Resident Evil 4 (version 1.5.9), this produced blurry text and flickering, looking worse than the game's own FSR2, and frame generation is not possible through this method. It is not worth setting up. For Resident Evil 4, you can instead use optical flow frame generation (`OFLOW_FG=1`, [frame-generation.md](frame-generation.md)).
