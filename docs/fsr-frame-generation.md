# FSR frame generation on XeSS-FG

A game with FSR 3.1+ frame generation but no XeSS frame generation can run XeSS-FG instead, with multi-frame generation
on XMX. The package's `amd_fidelityfx_framegeneration_dx12.dll` (source in `ffxfg/`) implements AMD's FidelityFX
frame generation API on top of XeSS frame generation and XeLL and takes the place of the game's own. The game keeps its
FSR integration and its menu; nothing hooks into the game. Upscaling stays whatever the game uses (FSR, or FSR 4 on XMX,
[games.md](games.md)), so upscaling and frame generation can be chosen separately.

| Game ships | Upscaling | Frame generation |
|---|---|---|
| FSR 2 | the game's own FSR 2 | none (the game has no frame generation) |
| FSR 3.0 | the game's own FSR 3 | the game's own FSR 3 frame generation |
| FSR 3.1.0 to 3.1.3: one `amd_fidelityfx_dx12.dll` | FSR 4 on XMX, if the upscaler build takes this layout (not checked) | not supported yet |
| FSR 3.1.4+ / FSR 4: `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_framegeneration_dx12.dll` | FSR 4 on XMX | **XeSS-FG**, 2x to 8x |

Tested: CONTROL Resonant (2x, 4x, 6x), Resident Evil Requiem (4x, 5x; the game ships XeSS frame generation as well, but
does not offer it under Proton for an unknown reason, so its FSR frame generation is the way to XeSS-FG there).

## Use

1. In the game's launch options:

       FSR_FG=xess XESS_FG=4 %command%

   `FSR_FG=xess` puts the DLL into the game folder while the game runs (`tools/proton-hook.py`, the same way as
   `XESS=latest`: originals renamed to `<name>.xmx-orig`, back when the game exits). Next to it go the XeSS SDK's
   `libxess_fg.dll` and `libxell.dll` (`xess/`, `tools/get-xess.sh`); a newer copy the game has of its own stays.
   `XESS_FG=<n>` is the number of frames, as for XeSS frame generation ([launch-options.md](launch-options.md)):
   without it XeSS-FG runs 2x. The game's own frame generation menu (often 2x only) no longer decides the number.
2. In the game: turn on FSR frame generation. The menu still says FSR.

The hook runs in every Proton `install.sh` could write to. For another one (GE-Proton, a Proton of a distribution
package) `tools/fsr-fg.sh` changes the game folder until it is switched back:

    tools/fsr-fg.sh on <app id | game folder>       the DLL and the XeSS SDK libraries in, originals as <name>.ffxfg-orig
    tools/fsr-fg.sh off <app id | game folder>      the game's own files back
    tools/fsr-fg.sh status <app id | game folder>

A game update or Steam's file check brings the game's own DLL back; run `on` again. `uninstall.sh` switches every game
back.

XeSS-FG needs the shim for more than 2x and for XMX: `XESS_LOG=1` shows in `C:\igdext_xefg.log` whether it was loaded
and which maximum XeSS-FG reports.

## How it works

The game creates two kinds of FidelityFX contexts, and the DLL maps them onto XeSS-FG:

* the frame generation **swap chain** (FFX's WRAP, NEW or FOR_HWND variant) becomes an XeSS-FG proxy swap chain, with
  an XeLL context (XeSS-FG requires one). The game gets a thin wrapper around the proxy that adds the XeLL present
  markers and the present id;
* the **frame generation** context: each frame's prepare call (depth, motion vectors, jitter, camera) is tagged for
  XeSS-FG, with view and projection matrices built from the camera vectors; the HUD-less color of the configure call
  becomes XeSS-FG's UI composition input. The game's own interpolation dispatch does nothing: XeSS-FG interpolates
  inside the proxy swap chain.

Two things the game would otherwise trip over are handled in the wrapper:

* **frame latency waitable object.** The game waits on it before each frame. The proxy's object deadlocked CONTROL
  Resonant within seconds, so the game gets an object of the DLL's own that is signalled once per present;
* **exclusive fullscreen.** XeSS-FG does not interpolate in exclusive fullscreen. A request for it is answered as if
  it had worked while the swap chain stays windowed (under gamescope it looks the same); after a mode change or a
  resize interpolation is switched on again.

## Log and switches

`ffx_xefg.log` next to the DLL: the swap chain, the maximum XeSS-FG reports and the number set, the first frames and
every 600th present (`presented 4` = 4x), XeSS-FG's warnings. The same `KEY=VALUE` lines can go into `ffx_xefg.env`
next to the DLL (the environment wins; `XESS_FG` belongs in the launch options, the shim reads it before the DLL loads).

| Variable | Effect |
|---|---|
| `FFX_XEFG_MULT=<n>` | frames per rendered frame (default `XESS_FG`, else 2), capped at XeSS-FG's maximum |
| `FFX_XEFG_LOG=<path>` | log file; `0` turns it off |
| `FFX_XEFG_XEFGLOG=0..3` | XeSS-FG's own messages: 0 debug, 1 info, 2 warnings (default), 3 errors |
| `FFX_XEFG_SLEEP=0` | no `xellSleep` (XeLL stays on, XeSS-FG requires it) |
| `FFX_XEFG_OWNWAIT=0` | the game gets the proxy's frame latency waitable object |
| `FFX_XEFG_EXCLUSIVE=1` | exclusive fullscreen is let through (then no interpolation) |

## Building

    bash ffxfg/get-headers.sh      AMD FSR SDK 2.4.0 and Intel XeSS SDK headers into ffxfg/inc (checked by SHA-256)
    ffxfg\build.bat                Visual Studio 2022 Build Tools -> ffxfg\build\amd_fidelityfx_framegeneration_dx12.dll

The release archive has the DLL in `ffxfg/`. CI builds it next to the shim.
