# Games

| Game | XeSS | XeSS SR on XMX | XeSS FG on XMX | How | Notes |
|---|---|---|---|---|---|
| Wuthering Waves | 2.0.2.68, XeFG 1.3.1.78 | yes | yes, 2x / 3x / 4x | `install.sh` (the game's own launch wrapper is kept for its mod runtime) | the XeLL frame-cap fix applies here |
| Forza Horizon 6 | 2.0.2.68 | yes | only through OptiScaler (below) | `install.sh` | the game's own FG option is DLSS-G |
| Resident Evil 4 (RE Engine) | none (2.0.2.68 added) | yes, technically | no | REFramework pd-upscaler (below) | worse than the game's FSR2; not recommended |
| Cyberpunk 2077 (2.31) | ships 2.0.1.41, XeFG 1.1.0.19; replaced by 2.0.2.68, XeFG 1.3.1.78 | yes, after the replacement | yes, after the replacement | `install.sh` + newer XeSS libraries (below) | with the shipped libraries DXVK hides the Intel GPU: DP4a, no XeSS FG option |

Anything else with XeSS 1.3 or later on its D3D12 renderer should work the same way: the kernels it needs are compiled
on first use. Please report games you tried (working or not) in the issue tracker, with the `IGDEXT_TRACE=1` log.

## A game that ships XeSS older than 2.0.2.68 (Cyberpunk 2077)

DXVK's DXGI reports an Intel GPU as an AMD one when the `libxess.dll` the game has loaded is older than 2.0.2.68 (a
workaround for early XeSS 2.0 builds, `isXessVendorWaNeeded()` in DXVK's `dxgi_options.cpp`; no option turns it off).
The Proton log (`PROTON_LOG=1`) then says `XeSS: hiding Intel GPU Vendor ID` and `vendor ID: 0x1002`. XeSS sees no
Intel GPU, never loads the extension library (the shim's trace stays empty) and runs its DP4a path; a game whose XeSS
frame generation is older than 1.2 (Intel only) does not offer it at all.

Replacing the game's XeSS libraries with newer ones lifts that. In Cyberpunk 2077 (Steam, 2.31, `bin/x64/`):
`libxess.dll`, `libxess_dx11.dll`, `libxess_fg.dll`, `libxell.dll` taken from a game that ships XeSS 2.0.2.68, from
Intel's XeSS SDK release, or from Proton: one launch with `PROTON_XESS_UPGRADE=1 %command%` downloads a newer set
into the prefix (`drive_c/windows/system32/umu/`), from where the four files can be copied into the game folder (the
redirect alone is not enough, the game loads its own `libxess.dll`). Keep the originals; a game update or "verify integrity" puts the old ones back. Result on a
B390: super resolution and frame generation on XMX (126 + 86 kernel pipelines, 24 kernels new to this game compiled on
first use), the XeSS frame generation option appears in the game's menu.

Mesa has a second mechanism with the same effect for some games (`force_vk_vendor=-1` in its game profiles);
`install.sh` overrides it, see the README's troubleshooting table.

## Frame generation in a game without XeSS-FG (OptiScaler)

Forza Horizon 6 ships XeSS super resolution but no XeSS frame generation. OptiScaler can add XeFG on top of the
game's own XeSS: put `OptiScaler.dll` as `dxgi.dll`, `libxess_fg.dll`, `libxell.dll`, `fakenvapi.dll` and
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

## A game without XeSS at all (Resident Evil 4, RE Engine)

RE Engine games ship a custom FSR2 that OptiScaler cannot hook. The only route is REFramework's `pd-upscaler` build +
PureDark's UpscalerBasePlugin 1.1.2 + a `libxess.dll` 2.0.2.68 in the game folder (`dinput8=n,b` override): its
TemporalUpscaler replaces the game's TAA by an XeSS call, and XeSS runs on XMX. Technically it works, but the result in
RE4 1.5.9 was blurry text and flicker, worse than the game's own FSR2, and frame generation is impossible there
(OptiScaler: "FG inputs: none"). Not worth setting up.
