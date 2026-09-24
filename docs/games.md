# Games

| Game | XeSS | XeSS SR on XMX | XeSS FG on XMX | How | Notes |
|---|---|---|---|---|---|
| Wuthering Waves | 2.0.2.68, XeFG 1.3.1.78 | yes | yes, 2x / 3x / 4x | `install.sh` (the game's own launch wrapper is kept for its mod runtime) | the XeLL frame-cap fix applies here |
| Forza Horizon 6 | 2.0.2.68 | yes | only through OptiScaler (below) | `install.sh` | the game's own FG option is DLSS-G |
| Resident Evil 4 (RE Engine) | none (2.0.2.68 added) | yes, technically | no | REFramework pd-upscaler (below) | worse than the game's FSR2; not recommended |

Anything else with XeSS 1.3 or later on its D3D12 renderer should work the same way: the kernels it needs are compiled
on first use. Please report games you tried (working or not) in the issue tracker, with the `IGDEXT_TRACE=1` log.

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
