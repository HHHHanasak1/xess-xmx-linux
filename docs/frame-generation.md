# Frame generation

This package runs Intel XeSS frame generation (XeSS-FG, which creates extra frames between rendered ones) on the GPU's XMX matrix units. You can use it in four ways, depending on what the game ships:

1. **The game's own XeSS-FG**: works out of the box after `install.sh`. Use `XESS_FG=<n>` to set the multiplier up to 4x, or experimentally up to 8x.
2. **Games with FSR 3.1+ frame generation (`FSR=xess` or `FSR_FG=xess`)**: replaces the game's FidelityFX DLLs so its FSR upscaling and frame generation run on XeSS and XeSS-FG.
3. **Games with FSR 3.1+ upscaling but no FSR frame generation (`FSR=xess FSR_FG=inject`)**: injects an XeSS-FG swap chain (the queue of finished frames waiting for the display) fed from the FSR upscaler.
4. **Any other DX12 game (`OFLOW_FG=1`, experimental)**: injects an XeSS-FG swap chain and calculates motion vectors from the final image using optical flow (estimating motion between frames by tracking pixel blocks).

If a game has its own XeSS, use that (`XESS=latest` if its XeSS is older than 2.0.2.68; see [games.md](games.md)). The FSR and optical flow routes can flicker or ghost more than native XeSS.

Whatever multiplier you choose, keep a frame rate limiter on (the game's own, or gamescope's). Without a limiter, generated frames reach the screen unevenly. With XeSS frame generation on, the game cannot use exclusive fullscreen because XeSS-FG refuses it; the game runs borderless instead, which looks the same under gamescope (SteamOS's display compositor).

For how these features work under the hood, see [dev/how-it-works.md](dev/how-it-works.md). For all launch options in one place, see [launch-options.md](launch-options.md).

---

## 1. The game's own XeSS frame generation and `XESS_FG`

In a game with native XeSS frame generation, turn on XeSS frame generation in the game's graphics menu. Without this package's Intel extension shim (`igdext64.dll`), XeSS frame generation runs on its generic path, which is limited to 2x. On XMX it runs at 2x, 3x, or 4x, and you can override the multiplier from the game's Steam launch options:

```sh
XESS_FG=4 %command%
```

`XESS_FG` works like the frame generation override in Intel Graphics Software on Windows. When the game creates its frame generation context, `libxess_fg.dll` asks `ControlLib.dll` (installed into every prefix by `install.sh` and `enable.sh` as `C:\windows\system32\ControlLib.dll`) for a multiplier override.

* **`XESS_FG=2`, `XESS_FG=3`, `XESS_FG=4`**: sets the multiplier to 2x, 3x, or 4x (the range Intel's override supports), regardless of what the game's menu offers or selects. Use this in a game that offers only 2x in its menu, such as Cyberpunk 2077. In a game with its own multi-frame menu (such as Wuthering Waves with 2x/3x/4x), `XESS_FG` takes precedence over the menu's number, while turning frame generation off in the menu still works.
* **`XESS_FG=5` to `XESS_FG=8` (experimental)**: 5x to 8x (8 is the maximum). Intel does not support more than 4x; above 4x `libxess_fg.dll` ignores `ControlLib.dll`, so the shim raises the number inside `libxess_fg.dll` in memory while the game runs (if anything does not match, the game keeps 4x). All four multipliers were tested in Cyberpunk 2077 and Wuthering Waves on an Arc B390. Every step above 4x lowers the real rendered frame rate and raises input latency, so 7x and 8x are not recommended.

Requirements for `XESS_FG`:

* Frame generation must be switched on in the game's menu.
* The game's `libxess_fg.dll` must be version 1.2 or newer. If the game ships an older version (like Cyberpunk 2077, which ships 1.1.0.19), add `XESS=latest`:

```sh
XESS=latest XESS_FG=4 %command%
```

If the self-test fails or a kernel fails to compile, XeSS falls back to its generic paths. Because generic frame generation is limited to 2x, a game set to 3x or 4x runs at 2x until the XMX path works again.

---

## 2. Low latency and XeLL

Games with XeSS frame generation use Intel XeLL (`libxell.dll`), Intel's low-latency library, to pace frame submission. Under Wine, XeLL runs in its built-in cross-vendor mode (`[XeLL] Using built-in context`); its Windows driver mode does not start under Proton.

### Automatic low latency

A GPU-limited or CPU-limited game queues frames before the GPU, and the wait in that queue causes most of the input latency. While a game's own XeSS frame generation is running, the shim automatically waits after `xellSleep` so each frame starts just in time to reach the GPU when the GPU becomes free.

In Wuthering Waves on an Arc B390 at 3x frame generation, this cuts simulation-start to present-end latency at the same frame rate (see [performance.md](performance.md)):

* GPU-limited: 69 ms down to 35 ms (−50%)
* CPU-limited: 76 ms down to 48 ms (−37%)
* Render-thread-limited: 105 ms down to 67 ms (−36%)

A game without a frame queue (such as CONTROL) does not wait, so nothing changes there.

This automatic low latency works only for a game's own XeSS frame generation. With `FSR=xess`, `FSR_FG`, and `OFLOW_FG`, the package's frame generation DLL reports each present to XeLL right before the next frame's sleep, so the shim sees no queue and does not wait. Low latency is off when frame generation is off.

### Wuthering Waves 30 fps cap fix

Wuthering Waves hands XeLL a 33.3 ms frame cap right after you enable frame generation, locking the game to 30 fps until you open and close the pause menu. By default, the shim ignores a frame cap that arrives within 1.5 seconds of enabling frame generation in Wuthering Waves and keeps the previous value.

### Low-latency launch options

| Launch option | Effect |
|---|---|
| `IGDEXT_XELL_CAP=off %command%` | Turns automatic low latency off. Use this to compare latency or if you suspect it lowers the frame rate in a game. |
| `IGDEXT_XELL_CAP=auto:<ms> %command%` | Keeps low latency on, but starts each frame `<ms>` milliseconds earlier (or later if negative). For example, `IGDEXT_XELL_CAP=auto:2` gives more frame rate headroom at the cost of a little more latency. |
| `IGDEXT_XELL_CAP=<us> %command%` | Replaces automatic low latency with a fixed minimum interval per displayed frame in microseconds through XeLL's frame cap (up to `1000000`; any other value turns both off). |
| `IGDEXT_XELL_FIXCAP=1 %command%` | Applies the Wuthering Waves 30 fps frame-cap fix in any game. |
| `IGDEXT_XELL_KEEPCAP=1 %command%` | Turns the Wuthering Waves frame-cap fix off everywhere. |
| `IGDEXT_XELL_LOG=1 %command%` | Writes `C:\igdext_xell.log` inside the prefix with XeLL calls, real frame rate, average latency every 100 frames, and low-latency state (`low latency: on`, `auto: frame generation on/off`, `auto: waited in N of 100 frames`). |

---

## 3. FSR on XeSS (`FSR=xess`, `FSR_FG=xess`, `FSR_FG=inject`)

A game with AMD FSR 3.1 or newer, but no XeSS, can run XeSS super resolution and XeSS frame generation on XMX instead. The package includes two DLLs (built from `ffxfg/`) that implement AMD's FidelityFX API on top of Intel's libraries:

* `amd_fidelityfx_upscaler_dx12.dll`: runs FSR upscaling on XeSS super resolution.
* `amd_fidelityfx_framegeneration_dx12.dll`: runs FSR frame generation on XeSS-FG and XeLL.

The game keeps its own FSR integration and menus, and nothing hooks into the game code. A third DLL (`dxgi.dll`) lets a game with FSR 3.1+ upscaling but no FSR frame generation run XeSS-FG (`FSR_FG=inject`).

If a game has native XeSS, use that instead (`XESS=latest`). Translating FSR calls to XeSS can flicker more than native XeSS because games do not always pass FSR what XeSS works best with (for example, dialog boxes and text flicker with frame generation in Cyberpunk 2077 on this route).

### What gets replaced

| What the game ships | `FSR=xess` | `FSR_FG=xess` |
|---|---|---|
| FSR 2, FSR 3.0, or FSR compiled into the game executable | Nothing changes (no FidelityFX DLLs to replace) | Nothing changes |
| FSR 3.1.0 to 3.1.3 (single `amd_fidelityfx_dx12.dll`) | XeSS SR + **XeSS-FG**, 2x to 8x | AMD's FSR 3.1.6 (from FSR SDK 2.4.0) + **XeSS-FG** |
| FSR 3.1.4+ / FSR 4 (`amd_fidelityfx_loader_dx12.dll` plus upscaler and frame generation DLLs) | XeSS SR + **XeSS-FG**, 2x to 8x | The game's own FSR upscaling + **XeSS-FG** |

A game with FSR 3.1.0 to 3.1.3 has upscaling and frame generation in one `amd_fidelityfx_dx12.dll`. AMD's loader of FSR SDK 2.4.0 has the same five functions and loads `amd_fidelityfx_upscaler_dx12.dll` and `amd_fidelityfx_framegeneration_dx12.dll` from its own folder, so it takes that file's place, with the package's DLLs next to it (with `FSR_FG=xess`: AMD's upscaler and the package's frame generation). `install.sh` fetches the loader and AMD's providers into `ffx/` (`tools/get-ffx.sh`; AMD's license in `ffx/license.md`).

### Tested games

* **CONTROL Resonant**: XeSS SR and 2x to 6x. With HDR on, bright sunlit clouds flicker at 3x and above (see Known limits below).
* **Resident Evil Requiem**: 4x and 5x. The game ships XeSS frame generation, but does not offer it under Proton for an unknown reason, so `FSR=xess` / `FSR_FG=xess` is the way to run XeSS-FG there.
* **Cyberpunk 2077**: the "FSR 4" upscaling option (running XeSS SR) and 5x. Native XeSS (`XESS=latest`) is better here.
* **Wuthering Waves** (FSR 3.1.1 in one DLL): XeSS SR and 5x. Native XeSS is the one to use here.
* **Forza Horizon 6** (with `FSR_FG=inject`): XeSS SR and 2x, 3x.

### How to use `FSR=xess` and `FSR_FG=xess`

1. Put `FSR=xess` (or `FSR_FG=xess` to keep the game's FSR upscaling) and your desired `XESS_FG` multiplier in the game's Steam launch options:

   ```sh
   FSR=xess XESS_FG=4 %command%
   ```

   Without `XESS_FG=<n>`, XeSS-FG runs at 2x. The game's own frame generation menu (often 2x only) no longer controls the multiplier.
   While the game runs, `tools/proton-hook.py` renames the game's original DLLs to `<name>.xmx-orig`, puts the package's DLLs and the XeSS SDK's `libxess.dll`, `libxess_fg.dll`, and `libxell.dll` (`xess/`, fetched by `tools/get-xess.sh`) next to them (leaving any newer copy the game already has), and restores the originals when the game exits.
2. In the game's graphics menu, select FSR upscaling and turn on FSR frame generation. The menu still says FSR.

In **Cyberpunk 2077**, there are two FSR upscalers: FSR 2 (compiled into the game, which stays FSR 2) and "FSR 4" (which uses the FidelityFX DLLs). With `FSR=xess`, the "FSR 4" option appears in the menu and runs on XeSS SR. Select "FSR 4": if you combine FSR 2 upscaling with XeSS-FG at 5x, near and far objects move at different speeds during fast camera turns.

### Protons without the hook (`tools/fsr-fg.sh`)

`FSR=xess` and `FSR_FG=xess` rely on the `user_settings.py` hook that `install.sh` puts into writable Proton installations (including GE-Proton in `compatibilitytools.d`). If you use the per-game `enable.sh` install or a read-only Proton from a distribution package, use `tools/fsr-fg.sh` to swap the files in the game folder until you switch them back:

```sh
tools/fsr-fg.sh all <app id | game folder>      # same as FSR=xess; originals kept as <name>.ffxfg-orig
tools/fsr-fg.sh on <app id | game folder>       # same as FSR_FG=xess (frame generation only)
tools/fsr-fg.sh off <app id | game folder>      # restore the game's own files
tools/fsr-fg.sh status <app id | game folder>
```

A game update or Steam's file verification restores the game's own DLLs; run `all` or `on` again afterwards. `uninstall.sh` restores every game automatically.

### Games that offer FSR only on AMD GPUs (`FSR_VENDOR=amd`)

Some games, such as Wuthering Waves, show FSR in their menu only when they detect an AMD GPU. Add `FSR_VENDOR=amd` to the launch options (or put `dxgi.customVendorId = 1002` into `dxvk.conf` next to the game's executable):

```sh
FSR=xess FSR_VENDOR=amd XESS_FG=4 %command%
```

This makes DXVK report an AMD vendor ID to the game. XeSS-FG and XeLL also query the vendor ID and require Intel's (XeSS SR does not); the package's frame generation DLL intercepts their adapter queries—and only theirs—and returns Intel's vendor ID when the adapter description is Intel's (`FFX_XEFG_VENDOR=0` disables this), and `ControlLib.dll` accepts the adapter as Intel's. Everything else in the game sees an AMD GPU, so the game's own XeSS menu options may disappear while `FSR_VENDOR=amd` is set.

To verify that the shim loaded and check the maximum multiplier XeSS-FG reports, run with `XESS_LOG=1` and check `C:\igdext_trace.log` and `C:\igdext_xefg.log`.

### Games with FSR upscaling and no FSR frame generation (`FSR_FG=inject`)

Some games (such as Forza Horizon 6) ship FSR 3.1+ upscaling and XeSS SR, but only offer DLSS-G for frame generation, which requires an NVIDIA GPU. Because the game already passes depth, motion vectors, jitter, and frame time to the FSR upscaler, this package can feed those inputs into an XeSS-FG swap chain:

1. Set the launch options:

   ```sh
   FSR=xess FSR_FG=inject XESS_FG=3 %command%
   ```

2. In the game, select FSR upscaling. Leave the game's frame generation menu off; `XESS_FG=<n>` sets the multiplier.

While the game runs, the Proton hook places `dxgi.dll` and `ffx_xefg_bridge.dll` (the frame generation DLL under a name the FidelityFX loader ignores) next to the upscaler DLL. Requirements and limits for `FSR_FG=inject`:

* The upscaler DLL must be in the same folder as the game's executable. If it is not, the hook logs a message to stderr and the game runs without frame generation.
* If the game folder already has a `dxgi.dll` (for example, from a mod), the hook leaves it alone and skips frame generation.
* Games with FSR 2, FSR 3.0, or built-in FSR have no upscaler DLL to replace and cannot use this.
* Requires the system-wide Proton hook (`install.sh`); `tools/fsr-fg.sh` and `enable.sh` do not support `FSR_FG=inject`.
* Because the game is only calling an upscaler, it provides no HUD-less image, no UI layer, and no camera vectors: the HUD is interpolated along with the scene and can ghost, and XeSS-FG gets a fixed view with the real projection, so fast camera turns may distort more.

Measured in **Forza Horizon 6** on an Arc B390 at 27 W SoC power limit, 1920x1200 output (1280x800 rendered), on the same road:

| Multiplier | Rendered frame rate | Shown frame rate |
|---|---|---|
| 2x | ~46 fps | ~93 fps |
| 3x | ~38 fps | ~115 fps |

The SoC is at its power limit in both runs. Generating the third frame costs about 4.8 ms: about 2.7 ms in XeSS-FG, 0.9 ms for gamescope to compose it, and the rest unattributed. The Steam performance overlay made no measurable difference.

### Known limits of FSR on XeSS

* **CONTROL Resonant with HDR**: with HDR10 (PQ) on, bright sunlit clouds flicker at 3x and above while the camera turns. With HDR off, or at 2x, they do not flicker. Other tested games did not show this with HDR on.
* **HUD ghosting**: when a game passes FSR a HUD-less image, the DLL uses it for XeSS-FG's UI composition. Setting `FFX_XEFG_HUDLESS=0` turns this off so XeSS-FG interpolates the whole image including the UI; in Cyberpunk 2077, that leaves less ghosting on dialog text.

### Logs and variables for FSR on XeSS

The DLLs write logs of the last run into the game's folder (Steam's file verification ignores them; set `FFX_XESS_LOG=0` or `FFX_XEFG_LOG=0` to disable them):

* `ffx_xess.log` (next to the upscaler DLL): logs the XeSS version, context, quality setting, setup flags, the first dispatches, and every 600th dispatch.
* `ffx_xefg.log` (next to the frame generation DLL): logs the upscalers the game's FidelityFX loader offers on this GPU (the first is the default), the swap chain, the maximum multiplier XeSS-FG reports and the number set, the first frames, every 600th present (`presented 4` = 4x, with sync interval and flags), and XeSS-FG warnings.
* `dxgi_xefg.log` (next to `dxgi.dll`, with `FSR_FG=inject`): logs what happened to each swap chain.

You can pass the variables below in the Steam launch options or put `KEY=VALUE` lines into `ffx_xess.env` / `ffx_xefg.env` next to the DLL (environment variables take precedence; always put `XESS_FG` in the launch options because the shim reads it before the DLLs load):

| Variable | Effect |
|---|---|
| `FFX_XESS_LOG=<path>` | Upscaler log file path; `0` turns logging off. |
| `FFX_XESS_XESSLOG=0..3` | XeSS's own log level: `0` debug, `1` info, `2` warnings (default), `3` errors. |
| `FFX_XESS_QUALITY=100..106` | Override the XeSS quality preset (`100` Ultra Performance to `105` Ultra Quality Plus, `106` Native AA). |
| `FFX_XESS_AUTOEXPOSURE=0` | Debug: disable XeSS auto exposure. |
| `FFX_XEFG_MULT=<n>` | Frames shown per rendered frame (defaults to `XESS_FG`, else `2`), capped at XeSS-FG's maximum. |
| `FFX_XEFG_LOG=<path>` | Frame generation log file path; `0` turns logging off. |
| `FFX_XEFG_XEFGLOG=0..3` | XeSS-FG's own log level: `0` debug, `1` info, `2` warnings (default), `3` errors. |
| `FFX_XEFG_HUDLESS=0` | Do not pass the HUD-less image or use UI composition. |
| `FFX_XEFG_SLEEP=0` | Skip `xellSleep` calls (XeLL itself stays on because XeSS-FG requires it). |
| `FFX_XEFG_OWNWAIT=0` | Hand the game the XeSS-FG proxy's own frame latency waitable object instead of the DLL's. |
| `FFX_XEFG_PACE=1` | Release the DLL's frame latency waitable object when the GPU finishes the presented frame (or after 200 ms) instead of immediately when present returns. Off by default. |
| `FFX_XEFG_VALIDITY=0` | Measuring aid: tag depth and motion vectors as valid until the next present instead of copying them at prepare time, to see what the copy costs (may cause flicker if the game overwrites them before present). |
| `FFX_XEFG_EXCLUSIVE=1` | Allow exclusive fullscreen through to DXGI (disables frame interpolation). |
| `FFX_XEFG_VENDOR=0` | Let XeSS-FG and XeLL see the vendor ID DXVK reports, even when it is not Intel's. |
| `FFX_XEFG_MVNDC=1` | Treat motion vectors as normalized device coordinates (NDC, as Cyberpunk 2077 passes them). |

---

## 4. Optical flow frame generation for any DX12 game (`OFLOW_FG=1`)

**Experimental.** XeSS-FG requires motion vectors and depth, which a game without XeSS or FSR 3.1+ does not provide. With `OFLOW_FG=1`, the package computes motion vectors from the final rendered image itself—similar to driver-level frame generation like AMD Fluid Motion Frames or NVIDIA Smooth Motion—and feeds them to XeSS-FG on XMX.

If a game has native XeSS or FSR 3.1+, use `XESS=latest` or `FSR=xess` instead: the game's own motion vectors, depth buffer, and HUD-less image produce a better result than estimating motion from the finished frame. If `FSR` or `FSR_FG` is set in the launch options, `OFLOW_FG` is ignored.

### How to use `OFLOW_FG=1`

Put `OFLOW_FG=1` in the game's Steam launch options:

```sh
OFLOW_FG=1 %command%
```

Use `XESS_FG=<n>` to set the number of frames shown per rendered frame (default `2`, which means one generated frame per rendered frame).

Requirements:

* Requires the system-wide install (`install.sh`); `enable.sh` does not support `OFLOW_FG`.
* **DX12 only**: XeSS-FG has no D3D11 or Vulkan swap chain. A Unity game that supports DX12 can be switched to DX12 by adding `-force-d3d12` after `%command%` (for Unity 2022, check with `strings UnityPlayer.dll | grep force-d3d12`).
* Run the game windowed or borderless. Exclusive fullscreen requests are automatically turned into a windowed swap chain, which looks identical under gamescope.

### Tested games and GPU cost

Tested on an Arc B390, SteamOS, 1920x1200, 144 Hz VRR display:

| Game | Engine | Without FG | Rendered with `OFLOW_FG=1` | Shown with `OFLOW_FG=1` | Rendered frame time consistency (95th percentile vs. median) |
|---|---|---|---|---|---|
| Resident Evil 4 (2023) | RE Engine (FSR 2 only) | ~50 fps | ~40 fps | ~80 fps | within 0.01 to 1.8 ms |
| Viewfinder | Unity 2022.3 (`-force-d3d12`) | not measured | ~70 fps | ~140 fps (display limit) | within 0.01 to 0.03 ms |

The image rendered properly in both games, including scenes where Viewfinder swaps large parts of the geometry at once.

GPU time per rendered frame at 1920x1200 on the Arc B390:

| Step | GPU time per rendered frame |
|---|---|
| Optical flow, full resolution (default) | 1.36 ms (Resident Evil 4), 1.65 ms (Viewfinder, at a lower clock) |
| Optical flow, half resolution (`FFX_XEFG_OFLOW_DIV=2`) | ~0.55 ms |
| Motion vector expansion compute shader | ~0.1 ms |
| XeSS-FG interpolation | ~2.6 ms (same as with game-provided motion vectors) |

In Resident Evil 4 at 1920x1200, the game runs at about 50 fps without frame generation and about 41 fps rendered (82 fps shown) with `OFLOW_FG=1`. Running optical flow at half resolution (`FFX_XEFG_OFLOW_DIV=2`) gave only 3 to 4 extra frames after frame generation with worse image quality, so full resolution is the default.

### Frame pacing and latency

Because generated frames sit between two rendered frames, an uneven rendered frame rate is immediately noticeable. Without a cap, Resident Evil 4 swung between 20 and 28 ms per rendered frame, which looked and felt uneven.

To keep frame delivery smooth, the DLL automatically caps the rendered frame rate through XeLL's frame cap (`FFX_XEFG_CAP=auto` by default) based on the measured GPU time and GPU clock frequency. When the game hits an external limit (such as gamescope's frame limiter or the display's refresh rate) and waits in `Present`, the automatic cap turns itself off until the game drops below that external limit again. Set `FFX_XEFG_CAP=<us>` for a fixed interval per rendered frame in microseconds, or `FFX_XEFG_CAP=0` for no cap.

For input latency: generating frames adds the wait for the next rendered frame, and because the game does not call XeLL directly, XeLL has no simulation markers from the game and cannot reduce latency as it does for native XeSS. Below about 50 fps rendered, the game's input feels like its rendered frame rate regardless of how many frames are shown.

### Known limits of `OFLOW_FG=1`

* **HUD smearing**: because there is no HUD-less image, the HUD is interpolated along with the rest of the frame, so text and fast-moving HUD elements can smear slightly.
* **Disocclusion wobble**: where an object uncovers background that was hidden in the previous frame, optical flow has nothing to track, and without a depth buffer, the edges of moving objects can wobble.
* **DX12 only**.

### Logs and variables for `OFLOW_FG=1`

The log is written to `ffx_xefg.log` next to the game's executable (`cap auto:` lines show every frame interval change; `oflow: GPU time` shows the optical flow cost). Set these variables in `ffx_xefg.env` next to the DLL or in the Steam launch options:

| Variable | Effect |
|---|---|
| `FFX_XEFG_OFLOW_DIV=n` | Run optical flow at `1/n` of the display resolution (`1` to `4`, default `1`). |
| `FFX_XEFG_OFLOW_DEPTH=d` | Constant depth value written for XeSS-FG (default `1.0`). |
| `FFX_XEFG_CAP=auto\|<us>\|0` | Rendered frame interval cap: `auto` (default), fixed microseconds `<us>`, or `0` (off). |
| `FFX_XEFG_CAP_PAD=ms` | Initial estimate in milliseconds for frame work beyond the measured GPU time in `auto` mode (default `1.5`). |
| `FFX_XEFG_REFRESH=Hz` | Override the display refresh rate in Hz if Windows reports the wrong rate. |
| `FFX_XEFG_TIMES=1` | Log frame time, time in `Present`, and XeLL sleep percentiles every 300 frames. |
