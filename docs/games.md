# Games

| Game | XeSS | XeSS SR on XMX | XeSS FG on XMX | How | Notes |
|---|---|---|---|---|---|
| Wuthering Waves | 2.0.2.68, XeFG 1.3.1.78 | yes | yes, 2x / 3x / 4x | `install.sh` | the XeLL frame-cap fix applies here |
| Forza Horizon 6 | 2.0.2.68 | yes | yes, from FSR upscaling: `FSR=xess FSR_FG=inject %command%` ([fsr-frame-generation.md](fsr-frame-generation.md)); or through OptiScaler (below) | `install.sh` | the game's own FG option is DLSS-G |
| Cyberpunk 2077 (2.31) | ships 2.0.1.41, XeFG 1.1.0.19 | yes, with newer XeSS libraries | yes, with newer XeSS libraries | `install.sh` + `XESS=latest %command%` (below); `XESS_FG=4` for 4x | with the shipped libraries: DP4a, no XeSS FG option. The game offers 2x; `XESS_FG=4` gives 4x, `XESS_FG=5`..`8` up to 8x (experimental) |
| Resident Evil 4 (RE Engine) | none | yes, technically | no | REFramework (below) | worse than the game's FSR2; not recommended |
| Satisfactory | XeSS + XeFG (Unreal plugin 2.0.10) | yes | not reported | `install.sh` | tested by a user on an Arc A310 (Xe-HPG, Mesa 26.3.0-rc1, #28) |

The others were tested on an Arc B390 (Xe3), all on Mesa 26.1.2; Wuthering Waves and Cyberpunk 2077 also on
Mesa 26.3.0-rc1, which was the default driver then (it is now 26.3.0). Forza Horizon 6 with `FSR_FG=inject` was
tested on 26.3.0-rc2.

Anything else with XeSS 1.3 or later on its D3D12 renderer should work the same way: the kernels it needs are compiled
on first use. Please report games you tried (working or not) in the issue tracker, with the `IGDEXT_TRACE=1` log.

## A game that ships XeSS older than 2.0.2.68 (Cyberpunk 2077)

DXVK's DXGI reports an Intel GPU as an AMD one when the `libxess.dll` the game has loaded is older than 2.0.2.68 (a
workaround for early XeSS 2.0 builds, `isXessVendorWaNeeded()` in DXVK's `dxgi_options.cpp`; no option turns it off).
The Proton log (`PROTON_LOG=1`) then says `XeSS: hiding Intel GPU Vendor ID`. XeSS sees no Intel GPU, never loads the
extension library (the shim's trace stays empty) and runs its DP4a path; a game whose XeSS frame generation is older
than 1.2 (Intel only) does not offer it at all.

`XESS=latest %command%` in the game's launch options lifts that: while the game runs, it uses the libraries of Intel's
XeSS SDK (2.0.2.68, XeSS-FG 1.3.1.78, XeLL 1.3.2.10), and its own come back when it exits
([launch-options.md](launch-options.md)). Tested on Cyberpunk 2077 2.31: super resolution and frame generation (the
game offers 2x, `XESS_FG=4` to `8` give more) on XMX. Showing the Intel GPU to the game's own libraries instead (DXVK's
`dxgi.customVendorId = 8086`) is no way out: its XeSS-FG 1.1.0.19 crashes at start.

By hand, replacing the game's XeSS libraries with newer ones does the same. In Cyberpunk 2077 these are `libxess.dll`,
`libxess_dx11.dll`, `libxess_fg.dll` and `libxell.dll` in `bin/x64/`. Sources for a 2.0.2.68 set:

* a game that ships it, or Intel's XeSS SDK release;
* Proton: one launch with `PROTON_XESS_UPGRADE=1 %command%` downloads a newer set into the prefix
  (`drive_c/windows/system32/umu/`). Copy the four files into the game folder; the redirect alone is not enough,
  because the game loads its own `libxess.dll`.

Keep the originals; a game update or "verify integrity" puts the old ones back. Result on a B390: super resolution
and frame generation on XMX, and the XeSS frame generation option appears in the game.

Mesa has a second mechanism with the same effect for some games (`force_vk_vendor=-1` in its game profiles);
`install.sh` overrides it, see [debugging.md](debugging.md).

## XeSS in a D3D11 game

Intel's XeSS for D3D11 (`libxess_dx11.dll`) contains no upscaler: it forwards every call to the XeSS of the Intel
Windows driver, which does not exist under Proton, so a D3D11 game never gets XeSS (and Intel supports it on Arc
GPUs only). With `XESS=latest %command%` the game's `libxess_dx11.dll` is replaced by this package's
(`dx11bridge/`), which implements the same API on the D3D12 XeSS of `libxess.dll`:

* each XeSS context gets a D3D12 device (vkd3d-proton) on the game's GPU, next to the game's D3D11 device (DXVK);
* per frame, the game's inputs are copied into textures both devices share (depth through a small compute shader into
  `R32_FLOAT`), a shared fence orders D3D11 -> D3D12 -> D3D11, and the result is copied into the game's output texture;
* XeSS on the D3D12 side runs on XMX like in a D3D12 game (same shim, same driver).

The copies cost some memory bandwidth per frame, nothing else changes. The same applies to OptiScaler's XeSS in a D3D11
game (`Dx11Upscaler=xess`), which loads `libxess_dx11.dll` from the game folder. Tested with the XeSS SDK's D3D11
sample on an Arc B390: XeSS super resolution on XMX through the bridge (`XESS_DX11_TRACE=1` logs each call;
`XESS_DX11_DUMP=<n>` writes the output of frame `n` next to the DLL).

## XeSS in a Vulkan game

XeSS' Vulkan path runs its XMX kernels only when the Intel Windows driver gives it a few interfaces outside the Vulkan
registry: a device function for the kernels' descriptors, a properties structure that says the path is available, and
a shader module structure with the kernels' build options. Wine passes on only what the registry defines, so under
Proton XeSS logs `Unable to initialize Intel Extensions layer. Using suboptimal implementation.` and runs DP4a.

With `XESS=latest %command%`, for a game whose executable imports `vulkan-1.dll` and that has a `libxess.dll`, the
package puts its own `vulkan-1.dll` (`vkwrap/`) next to the executable while the game runs. It forwards every Vulkan
call to Wine's and adds those interfaces: each XeSS kernel goes to `C:\igdext_kernels` like a D3D12 game's (same files,
same compiled cache), the module XeSS gets is a placeholder, and the descriptors go to the package's Vulkan layer,
which replaces the placeholder pipelines by the compiled kernels and binds them. The game runs on the package's
unpatched Mesa with that layer (the layer cannot read the patched driver's shaders); nothing else changes for it.
`vulkan-1.dll` tells XeSS the path is available only after the layer has answered its self-test on the game's device,
so without the layer XeSS stays on DP4a. `XESS_VK=1` sets this up for a game whose executable loads Vulkan at run time
(no import), `XESS_VK=0` leaves it out; `VKWRAP_LOG=1` writes `vkwrap.log` next to the DLL. XeSS skips the Intel path
whenever the Khronos validation layer is loaded.

Tested with the XeSS SDK's Vulkan sample and a test program (`vkwrap/test/`, low-resolution motion vectors and depth,
the output checked) on an Arc B390: all 63 kernels on XMX, about 7 ms per frame for 1080p output at Performance. No
Vulkan game with XeSS has been tested yet.

## Frame generation in a game without XeSS-FG (OptiScaler)

Forza Horizon 6 ships XeSS super resolution but no XeSS frame generation. This package's `FSR_FG=inject`
([fsr-frame-generation.md](fsr-frame-generation.md)) gives it XeSS frame generation from its FSR upscaling, without
OptiScaler. OptiScaler can instead add XeFG on top of the game's own XeSS: put `OptiScaler.dll` as `dxgi.dll`, `libxess_fg.dll`, `libxell.dll`, `fakenvapi.dll` and
`fakenvapi.ini` from the OptiScaler release into the game folder (not its `libxess.dll`: the game's own copy stays),
use this `OptiScaler.ini`:

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

and start the game with `WINEDLLOVERRIDES="dxgi=n,b"` (for example through `xmx-launch.sh` and its config file).
`Dxgi=false` matters: with the default NVIDIA spoof `libxess.dll` sees an NVIDIA adapter and takes its DP4a path.
XeFG driven this way runs on XMX too. Measured on FH6: ~28 real fps -> ~56 presented at 2x, HUD stable;
`[XeFG] InterpolationCount=2|3` gives 3x / 4x.

Caveat: in FH6 with HDR on and a high car level of detail, OptiScaler's swapchain hooking led to crashes (a double
`vkDestroyImage`) after a few minutes in the online world. Without OptiScaler the game runs fine.

## FSR 4 on XMX (upscaler builds from the OptiScaler Discord)

**No longer developed.** This project no longer develops FSR 4 on XMX actively. Whether the image improved was not
assessed: on a handheld's small screen the difference is hard to see. It brought no significant performance gain, it
adds complexity, and it conflicts with XeSS multi-frame generation: that combination did run, but needs image fixes for
each game separately, which goes against this project's aim of not adapting to each game the way OptiScaler does. What
is below stays as it is (a manual setup, nothing automated, one build tested on one GPU in two games). FSR 3.1+ games
run on XMX through `FSR=xess` instead.

Builds of an FSR 4 upscaler that runs as C-for-Metal kernels through Intel's extension are shared on the OptiScaler
project's Discord server, several of them and under different file names. Such a build goes through this project's
shim and driver like XeSS does. The one tested here is "B Boost" (FileVersion 4.0.2.1); other builds may behave
differently. Whatever the download is called, it takes the place of the game's `amd_fidelityfx_upscaler_dx12.dll`,
which the game loads through AMD's FFX loader; OptiScaler itself is not needed (with OptiScaler, it replaces the
`amd_fidelityfx_upscaler_dx12.dll` OptiScaler loads instead). It needs the driver fix for kernels whose sampler is a
descriptor table (patch 0005): with an older driver the image is wrong.

Setup, in the folder of the game's `amd_fidelityfx_upscaler_dx12.dll` (keep the original):

* the plugin, renamed to `amd_fidelityfx_upscaler_dx12.dll`;
* `fsr-original.dll`: `Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll` from AMD's FSR SDK 2.4.0 (FSR
  4.1.2), renamed. `install.sh` already fetched that file to the package's `ffx/amd_fidelityfx_upscaler_dx12.dll`
  (`tools/get-ffx.sh`); a copy of it will do. The plugin reads the FSR 4 weights from it;
* `cabinet.dll` from a Windows installation (`C:\Windows\System32\cabinet.dll`), and `WINEDLLOVERRIDES=cabinet=n,b`
  in the launch options. The plugin unpacks its kernels with the Windows compression API (`Cabinet.dll` ordinals 40,
  43, 45), which Wine does not implement; without it the game aborts with `unimplemented function Cabinet.dll.40`
  (Proton Experimental) or hangs (GE-Proton);
* if the Proton log says `igdext64.dll with required DX12 exports was not found` (a prefix without the driver store
  folder, for example a game `install.sh` has not covered), `<package>/igdext64.dll` next to the plugin.

`FSR4_XMX_NO_DIALOG=1` keeps its error dialog from blocking the game. The plugin offers several FSR 4 versions and the
game picks one: Cyberpunk 2077 picks the Lite model, CONTROL Resonant the full one. `FSR4_XMX_LITE=1` asks for Lite,
`FSR4_XMX_PROVIDER_INDEX=0` for the full model (4.0.2X).

Cyberpunk 2077 shows the FSR 4 option only with the plugin in place (AMD's own DLL reports FSR 4 as unsupported on
Intel). Measured on an Arc B390 (25 W in CONTROL Resonant, 27 W in Cyberpunk 2077; render 1280x800, no frame
generation):

| | Cyberpunk 2077 benchmark, average fps | CONTROL Resonant, fixed spot |
|---|---|---|
| XeSS, Ultra Quality (XMX) | 57.5 | - |
| FSR 3 (the game's DLL) | not offered | 36 |
| FSR 4 on XMX, full model | 56.4 | 35 |
| FSR 4 on XMX, Lite | 58.2 / 58.1 | 36 |

The FSR 4 kernels take 2.5 ms (full) / 1.9 ms (Lite) per frame on the B390. FSR frame generation stays at 2x: AMD has
not released multi-frame generation.

## A game without XeSS at all (Resident Evil 4, RE Engine)

RE Engine games ship a custom FSR2 that OptiScaler cannot hook. REFramework's `pd-upscaler` build with PureDark's
UpscalerBasePlugin 1.1.2 and a `libxess.dll` 2.0.2.68 in the game folder (`dinput8=n,b` override) replaces the game's
TAA by an XeSS call, and that runs on XMX. In RE4 1.5.9 the result was blurry text and flicker, worse than the game's
own FSR2, and frame generation is not possible there. Not worth setting up.
