# FSR on XeSS

A game with FSR 3.1+ but no XeSS can run XeSS instead, on XMX and with multi-frame generation. The package has two
DLLs (source in `ffxfg/`) that implement AMD's FidelityFX API on top of Intel's libraries and take the place of the
game's own:

* `amd_fidelityfx_upscaler_dx12.dll`: FSR upscaling on XeSS super resolution;
* `amd_fidelityfx_framegeneration_dx12.dll`: FSR frame generation on XeSS frame generation (XeSS-FG) and XeLL.

The game keeps its FSR integration and its menu; nothing hooks into the game. A game with FSR upscaling but no FSR
frame generation can still get XeSS-FG, with a third DLL ([below](#a-game-with-fsr-upscaling-and-no-fsr-frame-generation)).

**A game that has XeSS itself should use its XeSS** (`XESS=latest`, [launch-options.md](launch-options.md)). This
route translates the game's FSR calls, and what a game hands to FSR is not always what XeSS works best with: the image
can flicker more than with the game's own XeSS (in Cyberpunk 2077, dialog boxes and text with frame generation).

| Game ships | `FSR=xess` | `FSR_FG=xess` |
|---|---|---|
| FSR 2, FSR 3.0, or FSR compiled into the game | nothing changes (no FidelityFX DLLs to replace) | nothing changes |
| FSR 3.1.0 to 3.1.3: one `amd_fidelityfx_dx12.dll` | XeSS SR + **XeSS-FG**, 2x to 8x | AMD's FSR 3.1.6 (FSR SDK 2.4.0) + **XeSS-FG** |
| FSR 3.1.4+ / FSR 4: `amd_fidelityfx_loader_dx12.dll` with the upscaler and frame generation DLLs | XeSS SR + **XeSS-FG**, 2x to 8x | the game's own FSR upscaling + **XeSS-FG** |

Tested:

* CONTROL Resonant: XeSS SR and 2x to 6x; with HDR on, bright sunlit clouds flicker at 3x and more (below);
* Resident Evil Requiem: 4x, 5x (the game ships XeSS frame generation as well, but does not offer it under Proton for
  an unknown reason, so its FSR frame generation is the way to XeSS-FG there);
* Cyberpunk 2077: the "FSR 4" upscaling option (XeSS SR) and 5x; the game's own XeSS is better there (above);
* Wuthering Waves (FSR 3.1.1 in one DLL): XeSS SR and 5x; the game's own XeSS is the one to use;
* Forza Horizon 6 with `FSR_FG=inject`: XeSS SR and 2x, 3x (below).

## Use

1. In the game's launch options:

       FSR=xess XESS_FG=4 %command%

   `FSR=xess` puts the DLLs into the game folder while the game runs (`tools/proton-hook.py`, the same way as
   `XESS=latest`: originals renamed to `<name>.xmx-orig`, back when the game exits). Next to them go the XeSS SDK's
   `libxess.dll`, `libxess_fg.dll` and `libxell.dll` (`xess/`, `tools/get-xess.sh`); a newer copy the game has of its
   own stays. `XESS_FG=<n>` is the number of frames, as for XeSS frame generation
   ([launch-options.md](launch-options.md)): without it XeSS-FG runs 2x. The game's own frame generation menu (often
   2x only) no longer decides the number.

   `FSR_FG=xess` replaces only the frame generation and keeps the game's FSR upscaling.

   A game with FSR 3.1.0 to 3.1.3 has upscaling and frame generation in one `amd_fidelityfx_dx12.dll`. AMD's loader of
   FSR SDK 2.4.0 has the same five functions and loads `amd_fidelityfx_upscaler_dx12.dll` and
   `amd_fidelityfx_framegeneration_dx12.dll` from its own folder, so it takes that file's place, with the package's
   DLLs next to it (with `FSR_FG=xess`: AMD's upscaler and the package's frame generation). `install.sh` fetches the
   loader and AMD's providers into `ffx/` (`tools/get-ffx.sh`; AMD's license in `ffx/license.md`).
2. In the game: choose FSR upscaling and turn on FSR frame generation. The menu still says FSR.

Cyberpunk 2077 has two FSR upscalers: FSR 2, compiled into the game, which stays FSR 2, and "FSR 4", which goes
through the FidelityFX DLLs. The game offers FSR 4 only when the upscaler DLL reports it, so with `FSR=xess` the
option appears and runs on XeSS SR. Choose it: with FSR 2 upscaling and XeSS-FG at 5x, near and far objects moved at
different speeds while the camera turned fast.

The hook runs in every Proton `install.sh` could write to, GE-Proton in `compatibilitytools.d` included. For another
one (a Proton of a distribution package) `tools/fsr-fg.sh` changes the game folder until it is switched back:

    tools/fsr-fg.sh all <app id | game folder>      as FSR=xess, originals kept as <name>.ffxfg-orig
    tools/fsr-fg.sh on <app id | game folder>       as FSR_FG=xess: frame generation only
    tools/fsr-fg.sh off <app id | game folder>      the game's own files back
    tools/fsr-fg.sh status <app id | game folder>

A game update or Steam's file check brings the game's own DLLs back; run `all` or `on` again. `uninstall.sh` switches
every game back.

### A game that offers FSR only on AMD GPUs

Wuthering Waves shows FSR in its menu only on an AMD GPU. `FSR_VENDOR=amd` in the launch options (or a `dxvk.conf`
next to the game's exe with `dxgi.customVendorId = 1002`) makes DXVK report the GPU as AMD's to the game. XeSS-FG and
XeLL read the vendor the same way and need Intel's (XeSS SR does not): the frame generation DLL answers their adapter
queries, and only theirs, with Intel's vendor id when the adapter description is Intel's (`FFX_XEFG_VENDOR=0`
turns this off), and the package's `ControlLib.dll` takes such an adapter as Intel's. Everything else in the game sees
an AMD GPU, so its own XeSS options may go away while it is set.

XeSS needs the shim for XMX and XeSS-FG for more than 2x: `XESS_LOG=1` shows in `C:\igdext_trace.log` and
`C:\igdext_xefg.log` whether it was loaded and which maximum XeSS-FG reports.

### A game with FSR upscaling and no FSR frame generation

Forza Horizon 6 has FSR upscaling and XeSS, but its frame generation is DLSS-G only, which needs an NVIDIA GPU. FSR
upscaling receives most of what frame generation needs (depth, motion vectors, jitter, frame time), so:

    FSR=xess FSR_FG=inject %command%

and choose FSR upscaling in the game. Its frame generation menu plays no part and stays off; `XESS_FG=<n>` sets the
number. While the game runs, the hook also puts `dxgi.dll` and `ffx_xefg_bridge.dll` (the frame generation DLL under
a name the FidelityFX loader ignores) next to the upscaler. That has to be the folder of the game's exe: the hook says
so if it is not, and the game then runs without frame generation. A `dxgi.dll` the game folder already has (a mod, for
example) stays, also without frame generation, and a game with FSR frame generation of its own needs only `FSR=xess`. FSR 2, FSR 3.0 and FSR compiled into the game have no DLL to swap, so
they do not qualify. This needs the hook; `tools/fsr-fg.sh` does not do it.

The game hands over less than for real frame generation:

* no HUD-less image and no UI layer: the HUD is interpolated with the rest and can ghost;
* no camera vectors: XeSS-FG gets a fixed view with the real projection, so fast camera turns may distort more.

Forza Horizon 6 on an Arc B390 at SoC 27 W, 1920x1200 output, 1280x800 rendered, the same road:

| | rendered | shown |
|---|---|---|
| 2x | ~46 fps | ~93 fps |
| 3x | ~38 fps | ~115 fps |

The SoC is at its power limit in both. The third frame costs about 4.8 ms there: about 2.7 ms in XeSS-FG, 0.9 ms for
gamescope to compose it, the rest not attributed. The Steam performance overlay made no measurable difference.

### Known limits

* **CONTROL Resonant with HDR:** with HDR10 (PQ) on, bright sunlit clouds flicker at 3x and more while the camera
  turns; with HDR off, or at 2x, they do not. The other games did not show it with HDR on.
* **HUD:** a game that gives FSR a HUD-less image gets XeSS-FG's UI composition from it, as AMD's frame generation
  separates the UI that way. `FFX_XEFG_HUDLESS=0` leaves it off, so XeSS-FG interpolates the whole image, UI
  included: in Cyberpunk 2077 that leaves less ghosting on dialog text.

## How it works

**Upscaling.** The game's upscale context becomes an XeSS SR context on the same device. XeSS is initialised at the
first dispatch, with the quality setting whose optimal input size reaches the game's maximum render size (FSR Quality,
1.5x, gets XeSS Ultra Quality, 1.5x), and again when the output size changes; it takes any render size inside that
setting's range. The FFX flags map onto XeSS's: inverted depth, display-resolution motion vectors, jittered motion
vectors, auto exposure or an exposure texture, and a game without the HDR flag gets XeSS's LDR input. The motion
vector scale goes to XeSS unchanged (both multiply by it to get pixels). The game asks the upscaler for its render
resolution, jitter sequence and quality ratios; the DLL answers with FSR 3.1's numbers, so the game renders exactly as
it would for FSR. The inputs are moved to the states XeSS reads them in and back after the dispatch. The DLL reports
itself as FSR 4.0.2 and FSR 3.1.5, so games that offer an FSR option only for one of them find it.

**FSR_FG=inject.** `dxgi.dll` passes everything to the prefix's `dxgi.dll` and changes the two slots of the DXGI
factory that create swap chains. A D3D12 swap chain gets the same XeSS-FG proxy and wrapper as a FidelityFX swap chain
context; any other stays plain, and so does a D3D12 one if XeSS-FG fails. Each upscale dispatch tags that frame's depth
and motion vectors, under frame ids of the DLL's own: a game may record a frame's upscale before it presents the frame
before, so presents take the upscaled frames in order, and a present without one (menus, loading) is not interpolated.
This sits below whatever the game loads on top of DXGI, and the game's files stay as they are.

**Frame generation.** The game creates two kinds of FidelityFX contexts, and the DLL maps them onto XeSS-FG:

* the frame generation **swap chain** (FFX's WRAP, NEW or FOR_HWND variant) becomes an XeSS-FG proxy swap chain, with
  an XeLL context (XeSS-FG requires one). The game gets a thin wrapper around the proxy that adds the XeLL present
  markers and the present id;
* the **frame generation** context: each frame's prepare call (depth, motion vectors, jitter, camera) is tagged for
  XeSS-FG, with view and projection matrices built from the camera vectors; the HUD-less color of the configure call
  becomes XeSS-FG's UI composition input. The game's own interpolation dispatch does nothing: XeSS-FG interpolates
  inside the proxy swap chain.

XeSS-FG is set up when the swap chain is created, before the game's frame generation context says how its depth is
stored. The DLL uses the flags the game gave the last time (kept per exe in `%LOCALAPPDATA%\ffx_xefg\`), else it
assumes inverted depth; the log says when the game's flags differ (`wrong until the game starts again`).

Three things the game would otherwise trip over are handled in the DLL:

* **frame latency waitable object.** The game waits on it before each frame. The proxy's object deadlocked CONTROL
  Resonant within seconds, so the game gets an object of the DLL's own that is signalled once per present;
* **swap chain lifetime.** FFX's swap chain context holds a reference to the swap chain until it is destroyed, and a
  game may rely on it: Wuthering Waves releases its own reference and goes on presenting. The DLL's context does the
  same;
* **exclusive fullscreen.** XeSS-FG does not interpolate in exclusive fullscreen. A request for it is answered as if
  it had worked while the swap chain stays windowed (under gamescope it looks the same); after a mode change or a
  resize interpolation is switched on again.

## Log and switches

The logs hold the last run and stay in the game's folder (`FFX_XESS_LOG=0`, `FFX_XEFG_LOG=0` turn them off); Steam's
file check ignores them. `ffx_xess.log` next to the upscaler DLL: the XeSS version, the context, the quality setting and flags XeSS was set up
with, the first dispatches and every 600th. `ffx_xefg.log` next to the frame generation DLL: the upscalers the game's
FidelityFX loader offers on this GPU (the first is the default), the swap chain, the maximum XeSS-FG reports and the
number set, the first frames and every 600th present (`presented 4` = 4x), XeSS-FG's warnings. With `FSR_FG=inject`,
`dxgi_xefg.log` next to `dxgi.dll` says what became of each swap chain. The same `KEY=VALUE`
lines can go into `ffx_xess.env` / `ffx_xefg.env` next to the DLL (the environment wins; `XESS_FG` belongs in the
launch options, the shim reads it before the DLLs load).

| Variable | Effect |
|---|---|
| `FFX_XESS_LOG=<path>` | upscaler log file; `0` turns it off |
| `FFX_XESS_XESSLOG=0..3` | XeSS's own messages: 0 debug, 1 info, 2 warnings (default), 3 errors |
| `FFX_XESS_QUALITY=100..106` | the XeSS quality setting (100 Ultra Performance ... 105 Ultra Quality Plus, 106 Native AA) |
| `FFX_XESS_JITTER=<f>`, `FFX_XESS_AUTOEXPOSURE=0` | debug: jitter times f; no XeSS auto exposure |
| `FFX_XEFG_MULT=<n>` | frames per rendered frame (default `XESS_FG`, else 2), capped at XeSS-FG's maximum |
| `FFX_XEFG_LOG=<path>` | frame generation log file; `0` turns it off |
| `FFX_XEFG_XEFGLOG=0..3` | XeSS-FG's own messages: 0 debug, 1 info, 2 warnings (default), 3 errors |
| `FFX_XEFG_HUDLESS=0` | no HUD-less image and no UI composition (Known limits) |
| `FFX_XEFG_SLEEP=0` | no `xellSleep` (XeLL stays on, XeSS-FG requires it) |
| `FFX_XEFG_OWNWAIT=0` | the game gets the proxy's frame latency waitable object |
| `FFX_XEFG_EXCLUSIVE=1` | exclusive fullscreen is let through (then no interpolation) |
| `FFX_XEFG_VENDOR=0` | XeSS-FG and XeLL see the vendor DXVK reports, also when it is not Intel's (see above) |
| `FFX_XEFG_CAMERA`, `FFX_XEFG_JITTER`, `FFX_XEFG_MVNDC`, `FFX_XEFG_RENDERTIME`, `FFX_XEFG_RENDERSCALE`, `FFX_XEFG_DUMP`, `FFX_XEFG_ORDER` | debug: change or log one input at a time; the comments at their variables in `ffxfg/ffx_xefg.cpp` say how |

## Building

    bash ffxfg/get-headers.sh      AMD FSR SDK 2.4.0 and Intel XeSS SDK headers into ffxfg/inc (checked by SHA-256)
    ffxfg\build.bat                Visual Studio 2022 Build Tools -> ffxfg\build\amd_fidelityfx_upscaler_dx12.dll,
                                   amd_fidelityfx_framegeneration_dx12.dll and dxgi.dll

The release archive has the DLLs in `ffxfg/`. CI builds them next to the shim.
