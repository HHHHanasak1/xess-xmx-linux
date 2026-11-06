# Frame generation for any DX12 game (optical flow)

**Experimental.** XeSS frame generation (XeSS-FG, its network on XMX) needs motion vectors and depth from the game.
A game without FSR 3.1+ or XeSS gives neither. With `OFLOW_FG=1` the package works them out from the final image
itself, the way a driver-level frame generation has to (AMD Fluid Motion Frames, NVIDIA Smooth Motion):

* FidelityFX optical flow (FidelityFX SDK 1.1.4, MIT, linked into `ffxfg/amd_fidelityfx_framegeneration_dx12.dll`)
  estimates, for every 8x8 block of the final image, how it moved since the last frame;
* a small compute shader (`ffxfg/oflow_expand.hlsl`) turns that into per-pixel motion vectors at display size and
  writes a constant depth;
* XeSS-FG gets those with an identity camera, no jitter and no UI texture (`ffxfg/ffx_oflow.cpp`).

The game is not changed and does not need to know anything about frame generation: `dxgi.dll` next to its executable
(the same one as `FSR_FG=inject`, [fsr-frame-generation.md](fsr-frame-generation.md)) gives it an XeSS-FG swap chain.

**A game that has XeSS or FSR 3.1+ should use that** (`XESS=latest`, `FSR=xess`): its own motion vectors, depth and
HUD-less image are better than what the final image shows. With `FSR` or `FSR_FG` set, `OFLOW_FG` is left out.

## Use

In the game's launch options:

    OFLOW_FG=1 %command%

`XESS_FG=<n>` sets the frames shown per rendered frame (default 2, one generated frame), as for the game's own
XeSS-FG. Only DX12: a Unity game picks DX12 with `-force-d3d12` after `%command%` when its build has it (Unity 2022:
`strings UnityPlayer.dll | grep force-d3d12`). Play windowed or borderless; exclusive fullscreen is turned into a
windowed swap chain, which looks the same under gamescope.

Tested (Arc B390, SteamOS, 1920x1200, 144 Hz VRR):

| Game | Engine | Without frame generation | Rendered | Shown | Frame time of the rendered frames |
|---|---|---|---|---|---|
| Resident Evil 4 (2023) | RE Engine, FSR 2 only | about 50 fps | about 40 fps | about 80 fps | 95th percentile within 0.01 to 1.8 ms of the median |
| Viewfinder | Unity 2022.3 (`-force-d3d12`) | not measured | about 70 fps | about 140 fps (the display's limit) | within 0.01 to 0.03 ms |
The image was correct in both, also where Viewfinder swaps large parts of the scene at once.

## Cost

| | GPU time per rendered frame |
|---|---|
| optical flow, full resolution | 1.36 ms (RE4), 1.65 ms (Viewfinder, at a lower clock) |
| optical flow at half resolution (`FFX_XEFG_OFLOW_DIV=2`) | about 0.55 ms |
| motion vector expansion | about 0.1 ms |
| XeSS-FG itself | about 2.6 ms (the same as with the game's own motion vectors) |

RE4 at 1920x1200: about 50 fps without frame generation, about 41 rendered (82 shown) with it. Half-resolution
optical flow gave a few frames more without a visible difference there; full resolution is the default.

## Even frames

Generated frames sit between two rendered ones, so an uneven rendered frame rate shows directly. RE4 without any limit
swung between 20 and 28 ms per rendered frame, which looked and felt uneven. The DLL therefore limits the rendered
frame rate through XeLL's frame cap by default (`FFX_XEFG_CAP=auto`):

* the GPU time of every frame is measured on the game's queue, from the game's first submission after a present to the
  optical flow right before the next one;
* scaled by the GPU clock (read from the xe driver's `act_freq` in sysfs): a capped GPU idles, its clock drops and
  the same work takes longer, and without this the cap would drag itself down;
* the interval is that time (90th percentile) plus what a frame needs beyond it (the interpolation, the present;
  learned from the frames in which the GPU could not keep up), at least one display refresh per frame shown;
* when the game reaches an outside limit (gamescope's frame limit or the display's refresh rate) and waits in Present,
  the cap goes off and that limit paces, until the game no longer reaches it.

`FFX_XEFG_CAP=<us>` sets a fixed interval per rendered frame instead, `FFX_XEFG_CAP=0` none.

Latency: generated frames add the wait for the next rendered frame, and XeLL cannot see where the game reads its
input (no markers from the game), so it cannot reduce the latency as it does for a game with XeSS. Below about 50 fps
rendered, the game feels like its rendered frame rate however many frames are shown.

## Limits

* The HUD is interpolated with the rest of the image (there is no HUD-less image): text and fast HUD elements can
  smear a little.
* Where something appears that was hidden in the last frame, optical flow has nothing to follow; with no depth either,
  edges of moving objects can wobble.
* DX12 only (XeSS-FG has no D3D11 or Vulkan swap chain).

## Variables (`ffx_xefg.env` next to the DLL, or the launch options)

| Variable | What |
|---|---|
| `FFX_XEFG_OFLOW_DIV=n` | optical flow at 1/n of the display size (1 to 4, default 1) |
| `FFX_XEFG_OFLOW_DEPTH=d` | the constant depth (default 1.0) |
| `FFX_XEFG_CAP=auto\|<us>\|0` | the rendered frame interval (above) |
| `FFX_XEFG_CAP_PAD=ms` | auto's first guess of the time beyond the measured GPU time (default 1.5) |
| `FFX_XEFG_REFRESH=Hz` | the display's refresh rate, when Windows reports a wrong one |
| `FFX_XEFG_TIMES=1` | frame time, time in Present and XeLL's sleep (percentiles) in the log every 300 frames |

The log is `ffx_xefg.log` next to the game's executable; `cap auto:` lines show every change of the interval,
`oflow: GPU time` the optical flow's cost.
