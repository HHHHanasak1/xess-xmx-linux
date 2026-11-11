# Frame Generation

Without Intel's driver extensions, **XeSS frame generation** (XeSS-FG, `libxess_fg.dll`) is limited to 2x (one generated frame per rendered frame) and runs on generic shaders. With this package, XeSS-FG runs on the GPU's **XMX** (Xe Matrix Extensions, hardware matrix engines) units and supports multi-frame generation.

This guide covers all four ways to run XeSS frame generation on XMX, how to set the frame multiplier, and how low-latency pacing works:

* **The game's own XeSS-FG (`XESS_FG=<n>`, section 1; low-latency pacing in section 2):** Override the frame generation multiplier.
* **FSR 3.1+ games (`FSR=xess`, `FSR_FG=xess`, section 3):** Run a game's AMD FidelityFX Super Resolution (FSR) 3.1+ upscaling and frame generation on XeSS.
* **FSR 3.1+ upscaling games without FSR frame generation (`FSR=xess FSR_FG=inject`, section 4):** Inject an XeSS-FG swap chain fed by the FSR upscaler (for games where frame generation is NVIDIA DLSS-G only).
* **Any Direct3D 12 (DX12) game via optical flow (`OFLOW_FG=1`, experimental, section 5):** Run XeSS-FG using motion vectors computed from the final image.

For internal implementation details of `ControlLib.dll`, the shim hooks, the FidelityFX translation DLLs, and optical flow, see [dev/how-it-works.md](dev/how-it-works.md). For what to use in a given game, see [games.md](games.md). For all launch options in one list, see [launch-options.md](launch-options.md).

---

## 1. Native XeSS Frame Generation and `XESS_FG` Multipliers

In a game with native XeSS frame generation, turn on XeSS frame generation in the game's graphics menu. If the game ships `libxess.dll` older than 2.0.2.68 or `libxess_fg.dll` older than 1.2 (such as Cyberpunk 2077), add `XESS=latest` to its Steam launch options so it uses Intel's XeSS SDK 3.0.2 libraries ([games.md](games.md)).

### Setting the Multiplier (`XESS_FG`)

Many games (such as Cyberpunk 2077) only offer 2x frame generation in their menus. Put `XESS_FG=<n>` in the game's Steam launch options to set a fixed multiplier, where `<n>` is the total number of frames shown per rendered frame (`2` = one interpolated frame, `3` = two, `4` = three):

```text
XESS=latest XESS_FG=4 %command%
```

* **2x to 4x (`XESS_FG=2`, `XESS_FG=3`, `XESS_FG=4`):** Matches the range of the frame generation override in Intel Graphics Software on Windows. To answer the override query of `libxess_fg.dll`, `install.sh` and `enable.sh` place `ControlLib.dll` into each prefix. The file goes to `C:\windows\system32\ControlLib.dll`. The DLL accepts `XESS_FG` up to `16` and reports anything above `4` as 4x.
* **5x to 8x (`XESS_FG=5` .. `XESS_FG=8`, experimental):** `libxess_fg.dll` ignores `ControlLib.dll` overrides above 4x. So the shim (`igdext64.dll`) raises the maximum number of interpolated frames inside `libxess_fg.dll` at runtime, capped at `8`. If verification through the public API fails, the game stays at 4x. Intel does not support more than 4x. All four multipliers (`5` to `8`) work and were tested in Cyberpunk 2077 and Wuthering Waves on an Arc B390, but every step above 4x lowers the base rendered frame rate and adds input latency. **7x and 8x are not recommended** because of the latency.
* **In-game toggles:** You still switch frame generation on and off in the game's menu. In a game with its own multi-frame menu (such as Wuthering Waves, which offers 2x/3x/4x), `XESS_FG` takes precedence over the menu's number while still letting you turn frame generation off in the menu.
* **Fallback limit:** If the XMX path falls back to generic shaders (because the self-test failed or a kernel failed to compile), generic XeSS frame generation is limited to 2x. A game set to 3x or 4x will run at 2x until the XMX path works again.

### Display Mode and Frame Rate Limiters

* **Keep a frame rate limiter on:** Use the game's built-in limiter or gamescope's limiter regardless of the multiplier. Without a limiter, generated frames reach the screen unevenly.
* **Use windowed or borderless fullscreen:** `libxess_fg.dll` refuses to interpolate when the swap chain is in exclusive fullscreen mode. Under SteamOS and gamescope, borderless fullscreen looks the same as exclusive fullscreen.

---

## 2. Low-Latency Pacing with Frame Generation

When a game is GPU-limited or CPU-limited with frame generation on, frames queue up before the GPU finishes earlier work. Waiting in that queue accounts for most of the game's latency.

Games with XeSS frame generation use **Intel XeLL** (`libxell.dll`, Xe Low Latency). Under Wine and Proton, XeLL's hardware driver mode does not start, so XeLL runs in its cross-vendor built-in context ([dev/how-it-works.md](dev/how-it-works.md)). In that mode, XeLL sleeps only to honour a manual frame cap; its own estimate of the CPU time already includes the time spent waiting in the queue, so it never shortens that wait.

To remove the queue, the shim (`igdext64.dll`) hooks XeLL and paces each frame so that it reaches the GPU right when the GPU gets free.

* **On by default:** Low-latency pacing is on by default whenever a game's native XeSS frame generation is active, and off while frame generation is off. If a game has no queue, the pacer does not wait and changes nothing.
* **FSR routes:** With `FSR=xess` and `FSR_FG`, the game sends XeLL no time stamps. There the package's frame generation DLL does the pacing itself, with times it measures. See [Low Latency on the FSR Routes](#low-latency-on-the-fsr-routes) below. With `OFLOW_FG`, the frame cap paces the frames instead.
* **Measured latency reduction:** Tested in Wuthering Waves on an Arc B390 at 22 W SoC with 3x frame generation (`IGDEXT_XELL_LOG=1`, measuring simulation start to present end):
  - **GPU-limited** (fixed spot, 10 minutes): **69.0 ms -> 34.8 ms (−50%)** at ~25.1 ms frame time.
  - **CPU-limited** (CPU held to ~2 W, 4–6 minutes): **76.1 ms -> 47.8 ms (−37%)** at 28.7 ms frame time.
  - **Render-thread-limited** (moving in a heavy scene, ~6 minutes): **105.5 ms -> 67.2 ms (−36%)** at ~42.1 ms frame time.
  Full benchmark data is in [performance.md](performance.md).

### Low-Latency Launch Options (`IGDEXT_XELL_*`)

| Launch Option | What It Does | When to Use It |
|---|---|---|
| `IGDEXT_XELL_CAP=off %command%` (or `0`) | Turns low-latency pacing off | Comparing latency/frame rate, or if low-latency pacing costs frame rate or causes uneven pacing in a game |
| `IGDEXT_XELL_CAP=auto:<ms> %command%` | Keeps low-latency pacing on, starting each frame `<ms>` milliseconds earlier (or later if negative) | Giving the game more frame-rate headroom at the cost of a little more latency (for example, `IGDEXT_XELL_CAP=auto:2 %command%`) |
| `IGDEXT_XELL_CAP=<us> %command%` | Turns low-latency pacing off. When the game calls `xellSetSleepMode`, raises any smaller interval to `<us>` microseconds per displayed frame through XeLL's frame cap. Valid values: `1` to `1000000`; `0` = off. Any other value turns both off. A game that never calls `xellSetSleepMode` gets no cap from it | Locking a steady fixed frame rate through XeLL |
| `IGDEXT_XELL_FIXCAP=1 %command%` | Applies the Wuthering Waves 30 fps frame-cap fix in any game | Another game drops to 30 fps when enabling frame generation until a menu is opened and closed |
| `IGDEXT_XELL_KEEPCAP=1 %command%` | Turns the Wuthering Waves 30 fps frame-cap fix off | Ruling out the cap fix when debugging |
| `IGDEXT_XELL_LOG=1 %command%` | Writes `C:\igdext_xell.log` inside the game's prefix | Checking low-latency pacing state. At start, it logs `low latency: on`, `fixed cap` or `off`. It logs `auto: frame generation on/off (...)` and, once learned, `auto: frame time ...`. It logs `auto: queue ...` when a heavier scene raises it, and `auto: waited in N of 100 frames` every 100 frames |

### Low Latency on the FSR Routes

On the FSR routes (`FSR=xess`, `FSR_FG=xess`, `FSR_FG=inject`), the frame generation DLL handles the low latency. It is on by default. The same `IGDEXT_XELL_CAP` launch option controls it: `off` turns it off, and `auto:<ms>` starts each frame `<ms>` milliseconds earlier.

Two things made these games slow or laggy before:

* **XeSS-FG's Present blocks.** It waits until XeSS-FG has finished presenting the previous frame. A game that reaches Present early waits there with its frame already made, so its input is old by the time the frame is shown.
* **The game's questions to the swap chain block.** Right after Present, many games ask the swap chain for its frame statistics (`GetFrameStatistics`). DXVK locks the swap chain for a whole Present, so the question can wait for XeSS-FG. When it does, the GPU idles while the game works, and the frame rate drops. The games switched between the two states every few seconds.

The DLL fixes both:

1. **It answers those questions itself**, with values taken right before XeSS-FG's Present. The frame rate becomes higher and steady.
2. **It holds the game in Present for a short time** when frames arrive early. It measures each frame and adjusts the wait. If nothing queues (the game is CPU-limited, at a frame limit, or at the display's limit), it does not wait.

Measured on an Arc B390 (frame start to the end of its present, the game standing still):

| Game | Before | Low latency | Frame rate |
|---|---|---|---|
| CONTROL Resonant, 3x | 34.7 ms | **19.0 ms** | 28.5 -> 29.3 fps |
| CONTROL Resonant, 6x | 44.3 ms | **33.2 ms** | 22.4 fps (no change) |
| Resident Evil Requiem, 3x | 37.8 ms | **27.0 ms** | 26.1 -> 31.5 fps |
| Forza Horizon 6, 3x, car page (`FSR_FG=inject`) | 42.0 ms | **24.9 ms** | 23.5 -> 39.4 fps |

The frame rate gains come from the GPU no longer idling. They show where the GPU has room to spare, such as Forza Horizon 6's car page. Where the GPU is already fully busy, the frame rate rises only slightly: in Forza Horizon 6's built-in benchmark, 101 -> 103 fps, with the game's average latency 86.7 -> 81.8 ms. The wait costs less than 1% of the frame rate. Full data and method are in [performance.md](performance.md). `ffx_xefg.log` shows the state in its `low latency:` lines.

**Wuthering Waves 30 fps cap fix:** Wuthering Waves passes a 33.3 ms (30 fps) frame cap to XeLL right after enabling frame generation and only clears it when the pause menu is opened and closed. By default, for Wuthering Waves only, the shim replaces any non-zero cap arriving within 1.5 s of enabling frame generation with the previous interval (`IGDEXT_XELL_LOG=1` logs `frame cap ... keeping` lines when this triggers).

---

## 3. FSR 3.1+ on XeSS (`FSR=xess` and `FSR_FG=xess`)

A DX12 game with FSR 3.1+ (or FSR 4) but no native XeSS can run XeSS instead, on XMX and with multi-frame generation. The package includes two DLLs (source in `ffxfg/`) that implement AMD's FidelityFX API on top of Intel's libraries and take the place of the game's own DLLs:

* `amd_fidelityfx_upscaler_dx12.dll`: runs FSR upscaling on XeSS super resolution (`libxess.dll`).
* `amd_fidelityfx_framegeneration_dx12.dll`: runs FSR frame generation on XeSS frame generation (`libxess_fg.dll`) and XeLL (`libxell.dll`).

The game keeps its FSR integration and its menu; nothing hooks into the game.

**If a game has native XeSS, use its own XeSS instead (`XESS=latest`).** This route translates the game's FSR calls, and what a game hands to FSR is not always what XeSS works best with: the image can flicker more than with the game's own XeSS (for example, dialog boxes and text with frame generation in Cyberpunk 2077).

### How Different FSR Versions Behave

| Game Ships | `FSR=xess` | `FSR_FG=xess` |
|---|---|---|
| FSR 2, FSR 3.0, or FSR compiled into the game | Nothing changes (no FidelityFX DLLs to replace) | Nothing changes |
| **FSR 3.1.0 to 3.1.3:** one `amd_fidelityfx_dx12.dll` | XeSS SR + **XeSS-FG** | AMD's FSR 3.1.6 (FSR SDK 2.4.0) + **XeSS-FG** |
| **FSR 3.1.4+ / FSR 4:** `amd_fidelityfx_loader_dx12.dll` with the upscaler and frame generation DLLs | XeSS SR + **XeSS-FG** | The game's own FSR upscaling + **XeSS-FG** |

### Tested Games

* **CONTROL Resonant:** XeSS SR and 2x to 6x (`FSR=xess`). With HDR10 (PQ) on, bright sunlit clouds flicker at 3x and higher while the camera turns; with HDR off, or at 2x, they do not.
* **Resident Evil Requiem:** 4x and 5x (`FSR=xess` / `FSR_FG=xess`). The game ships XeSS frame generation as well, but does not offer it under Proton, so its FSR frame generation is the way to run XeSS-FG there.
* **Cyberpunk 2077:** The "FSR 4" upscaling option (XeSS SR) and 5x (`FSR=xess`). The game's own XeSS (`XESS=latest`) is better there.
* **Wuthering Waves** (FSR 3.1.1 in one DLL): XeSS SR and 5x (`FSR=xess FSR_VENDOR=amd`). The game's own XeSS is the one to use.
* **Forza Horizon 6** with `FSR=xess FSR_FG=inject`: XeSS SR and 2x, 3x (see [Section 4](#4-a-game-with-fsr-upscaling-and-no-fsr-frame-generation-fsr_fginject)).

### How to Use `FSR=xess` and `FSR_FG=xess`

1. In the game's Steam launch options, set:

   ```text
   FSR=xess XESS_FG=4 %command%
   ```

   * `FSR=xess` links the package's upscaler and frame generation DLLs into the game folder while the game runs (`tools/proton-hook.py`, the same way as `XESS=latest`: originals are kept as `<name>.xmx-orig` and restored when the game exits). Next to them go the XeSS SDK's `libxess.dll`, `libxess_fg.dll`, and `libxell.dll`. They come from `xess/`, fetched by `install.sh` / `tools/get-xess.sh`. If the game has a newer copy of its own, that copy stays.
   * `FSR_FG=xess` replaces only the frame generation DLL and keeps the game's FSR upscaling.
   * `XESS_FG=<n>` sets the number of frames ([Setting the Multiplier](#setting-the-multiplier-xess_fg)); without it, XeSS-FG runs at 2x. The game's own frame generation menu (often 2x only) no longer decides the number.
   * A game with FSR 3.1.0 to 3.1.3 has upscaling and frame generation in one `amd_fidelityfx_dx12.dll`. For such a game, AMD's loader from FSR SDK 2.4.0 takes that file's place, with the package's DLLs next to it. When using `FSR_FG=xess`, AMD's upscaler + the package's frame generation go next to it instead. The loader is `ffx/amd_fidelityfx_loader_dx12.dll`, fetched by `install.sh` / `tools/get-ffx.sh`; its license is in `ffx/license.md`.

2. In the game, choose **FSR** upscaling and turn on **FSR frame generation**. The menu still says FSR.

**Cyberpunk 2077 note:** Cyberpunk 2077 has two FSR upscalers: FSR 2, compiled into the game (which stays FSR 2), and "FSR 4", which goes through the FidelityFX DLLs. Because our upscaler DLL reports itself as FSR 4.0.2 and FSR 3.1.5, the "FSR 4" option appears in the menu and runs on XeSS SR. Choose "FSR 4": with FSR 2 upscaling and XeSS-FG at 5x, near and far objects moved at different speeds while turning the camera fast.

**First-launch depth flags:** XeSS-FG is set up when the swap chain is created, before the game's frame generation context reports whether its depth buffer is inverted. The DLL saves the game's flags per executable in `%LOCALAPPDATA%\ffx_xefg\<exe>.flags`; on the very first launch of a game it assumes inverted depth. If the game uses different flags, `ffx_xefg.log` notes `wrong until the game starts again`, and the next launch uses the saved flags.

### Protons Without the Hook (`tools/fsr-fg.sh`)

The Proton hook runs in every Proton `install.sh` could write to, including GE-Proton in `compatibilitytools.d`. For a Proton that `install.sh` could not write to (such as a root-owned distribution package in `/usr/share/steam/compatibilitytools.d`), `tools/fsr-fg.sh` copies the DLLs into the game folder until you switch it back:

```bash
tools/fsr-fg.sh all <app id | game folder>      # frame generation + upscaler in folders that have FSR FG; originals kept as <name>.ffxfg-orig
tools/fsr-fg.sh on <app id | game folder>       # as FSR_FG=xess: frame generation only
tools/fsr-fg.sh off <app id | game folder>      # restore the game's own files
tools/fsr-fg.sh status <app id | game folder>   # show what each DLL in the game folder is
```

Unlike the `FSR=xess` hook, `tools/fsr-fg.sh all` looks for a frame generation DLL: `amd_fidelityfx_framegeneration_dx12.dll` or the single `amd_fidelityfx_dx12.dll`. It swaps `amd_fidelityfx_upscaler_dx12.dll` next to that DLL. A game folder that has only `amd_fidelityfx_upscaler_dx12.dll` and no frame generation DLL is refused (`no FSR 3.1+ frame generation`). A game update or Steam's file verification restores the game's own DLLs; run `all` or `on` again afterwards. `uninstall.sh` switches every game back automatically.

### A Game That Offers FSR Only on AMD GPUs (`FSR_VENDOR=amd`)

Wuthering Waves shows FSR in its menu only on an AMD GPU. Add `FSR_VENDOR=amd` to the launch options (or put `dxgi.customVendorId = 1002` in `dxvk.conf` next to the game's `.exe`) to make DXVK report the GPU as AMD's to the game:

```text
FSR=xess FSR_VENDOR=amd XESS_FG=4 %command%
```

`libxess_fg.dll` and `libxell.dll` also read the adapter vendor ID and require Intel's (`0x8086`). Our frame generation DLL answers their adapter queries—and only theirs—with Intel's vendor ID when the adapter description starts with `"Intel"` (`FFX_XEFG_VENDOR=0` turns this off), and the package's `ControlLib.dll` accepts such an adapter as Intel's. Everything else in the game sees an AMD GPU, so the game's own XeSS options may disappear while `FSR_VENDOR=amd` is set.

### Known Limits of `FSR=xess` / `FSR_FG=xess`

* **CONTROL Resonant with HDR:** Sunlit clouds flicker at 3x and higher with HDR10 on (see [Tested Games](#tested-games)). Other tested games did not show this with HDR on.
* **HUD ghosting (`FFX_XEFG_HUDLESS=0`):** When a game passes FSR a HUD-less image, the DLL enables XeSS-FG's UI composition from it. Setting `FFX_XEFG_HUDLESS=0` leaves UI composition off so XeSS-FG interpolates the whole image, UI included; in Cyberpunk 2077 that leaves less ghosting on dialog text.

---

## 4. A Game with FSR Upscaling and No FSR Frame Generation (`FSR_FG=inject`)

Forza Horizon 6 has FSR upscaling and XeSS super resolution, but its frame generation option is NVIDIA DLSS-G only. Because FSR 3.1+ upscaling receives depth, motion vectors, jitter, and frame time every frame, the package can feed those inputs into an XeSS-FG swap chain.

### How to Use `FSR_FG=inject`

1. In the game's Steam launch options, set:

   ```text
   FSR=xess FSR_FG=inject XESS_FG=3 %command%
   ```

2. In the game, choose **FSR** upscaling. Leave the game's own frame generation menu off; `XESS_FG=<n>` sets the multiplier (default 2x).

While the game runs, `tools/proton-hook.py` places `dxgi.dll` and `ffx_xefg_bridge.dll` (the frame generation DLL under a name the FidelityFX loader ignores) next to the upscaler DLL. Using `FSR_FG=inject` requires the Proton hook (`tools/fsr-fg.sh` does not support it) and requires `FSR=xess`.

Requirements for `FSR_FG=inject`:
* The folder containing the upscaler DLL must also contain the game's `.exe`. If it does not, the hook logs a message to stderr and the game runs without frame generation.
* If the game folder already has its own `dxgi.dll` (for example, from a mod), the hook leaves it alone and starts the game without frame generation.
* If the game already has FSR frame generation of its own (`amd_fidelityfx_framegeneration_dx12.dll` or `amd_fidelityfx_dx12.dll`), `FSR=xess` uses that directly and ignores `FSR_FG=inject`.
* Games with FSR 2, FSR 3.0, or FSR compiled into the `.exe` have no FidelityFX DLL to swap and cannot use `FSR_FG=inject`.

### Limits and Cost of `FSR_FG=inject`

Because the upscaler receives less information than a full frame generation integration:
* **No HUD-less image or UI layer:** The HUD is interpolated along with the rest of the image and can ghost.
* **No camera orientation vectors:** XeSS-FG gets a fixed identity view matrix with the real projection matrix, so fast camera turns may distort more.

Measured in Forza Horizon 6 on an Arc B390 at 27 W SoC, 1920x1200 output (1280x800 rendered), on the same road:

| Multiplier | Rendered | Shown |
|---|---|---|
| 2x (`XESS_FG=2`) | ~46 fps | ~93 fps |
| 3x (`XESS_FG=3`) | ~38 fps | ~115 fps |

The SoC is at its power limit in both runs. The second generated frame (3x instead of 2x) costs about 4.8 ms: about 2.7 ms in XeSS-FG, 0.9 ms for gamescope to compose it, and the rest unattributed. The Steam performance overlay made no measurable difference.

---

## 5. Frame Generation for Any DX12 Game (`OFLOW_FG=1`, Experimental)

**Experimental.** XeSS-FG needs motion vectors and depth, which a game without XeSS or FSR 3.1+ does not provide. With `OFLOW_FG=1`, the package computes motion vectors from the final presented image using FidelityFX optical flow (FidelityFX SDK 1.1.4, linked into `ffxfg/amd_fidelityfx_framegeneration_dx12.dll`) and writes a constant depth texture, similar to driver-level frame generation (AMD Fluid Motion Frames, NVIDIA Smooth Motion). `dxgi.dll` next to the game's executable gives the game an XeSS-FG swap chain.

**If a game has XeSS or FSR 3.1+, use `XESS=latest` or `FSR=xess` instead:** the game's own motion vectors, depth, and HUD-less image are better than optical flow on the final image. If `FSR` or `FSR_FG` is set alongside `OFLOW_FG=1`, `OFLOW_FG` is ignored.

### How to Use `OFLOW_FG=1`

In the game's Steam launch options, set:

```text
OFLOW_FG=1 %command%
```

* Use `XESS_FG=<n>` to set the number of frames shown per rendered frame (default `2`, one generated frame).
* **DX12 only:** For a Unity game that defaults to D3D11, pass `-force-d3d12` after `%command%` if its `UnityPlayer.dll` supports DX12 (`strings UnityPlayer.dll | grep force-d3d12`):
  ```text
  OFLOW_FG=1 %command% -force-d3d12
  ```
* **Windowed or borderless:** Play in windowed or borderless fullscreen mode (exclusive fullscreen requests are kept windowed automatically, which looks the same under gamescope).
* If the game's folder already has a `dxgi.dll` or `ffx_xefg_bridge.dll`, `OFLOW_FG=1` leaves it alone and starts the game without frame generation.

### Tested Games (`OFLOW_FG=1`)

Tested on an Arc B390, SteamOS, 1920x1200, 144 Hz VRR display:

| Game | Engine | Without Frame Generation | Rendered | Shown | Frame Time of the Rendered Frames |
|---|---|---|---|---|---|
| **Resident Evil 4 (2023)** | RE Engine, FSR 2 only | about 50 fps | about 41 fps | about 82 fps | 95th percentile within 0.01 to 1.8 ms of the median |
| **Viewfinder** | Unity 2022.3 (`-force-d3d12`) | not measured | about 70 fps | about 140 fps (the display's limit) | 95th percentile within 0.01 to 0.03 ms of the median |

The image was accurate in both games, including where Viewfinder swaps large parts of the scene at once.

### GPU Cost and Even Frame Pacing (`FFX_XEFG_CAP=auto`)

| Stage | GPU Time per Rendered Frame |
|---|---|
| Optical flow, full resolution (`FFX_XEFG_OFLOW_DIV=1`, default) | 1.36 ms (Resident Evil 4), 1.65 ms (Viewfinder, at a lower clock) |
| Optical flow at half resolution (`FFX_XEFG_OFLOW_DIV=2`) | about 0.55 ms |
| Motion vector expansion (`oflow_expand.hlsl`) | about 0.1 ms |
| XeSS-FG interpolation | about 2.6 ms (same as with a game's own motion vectors) |

In Resident Evil 4 at 1920x1200, frame rate is about 50 fps without frame generation and about 41 fps rendered (82 fps shown) with `OFLOW_FG=1`. Half-resolution optical flow (`FFX_XEFG_OFLOW_DIV=2`) gave only 3 to 4 fps more with frame generation with a worse image, so full resolution (`1`) is the default.

**Automatic rendered frame-rate cap (`FFX_XEFG_CAP=auto`):**
Because generated frames sit between two rendered ones, an uneven rendered frame rate shows directly (uncapped Resident Evil 4 swung between 20 and 28 ms per rendered frame). By default with `OFLOW_FG=1`, the DLL limits the rendered frame rate through XeLL's frame cap (`FFX_XEFG_CAP=auto`):
* It measures the GPU time of every frame on the game's command queue, from the game's first submission after a present to the end of optical flow right before the next present, and scales that time by the GPU clock read from the `xe` kernel driver's `act_freq` sysfs file (so an idle clock drop under the cap does not drag the cap down).
* Every 30 frames, it sets the interval to the 90th-percentile GPU time plus the learned overhead for interpolation and present (`FFX_XEFG_CAP_PAD`, first guess `1.5` ms), at least one display refresh per frame shown.
* When the game reaches an outside limit (gamescope's frame limit or the display's refresh rate) and waits inside `Present`, the auto cap turns itself off and lets that limit pace the game until the game no longer reaches it.
* Set `FFX_XEFG_CAP=<us>` for a fixed interval per rendered frame in microseconds, or `FFX_XEFG_CAP=0` to disable the cap.

### Limits of `OFLOW_FG=1`

* **Latency:** Generated frames add the wait for the next rendered frame, and XeLL cannot see where the game reads input (the game sends no XeLL simulation markers), so it cannot reduce latency the way it does for a native XeSS game. Below about 50 fps rendered, the game feels as sluggish as its rendered frame rate, however smooth the shown frames look.
* **No FSR-route low latency by default:** The frame cap above paces the frames instead. With `FFX_XEFG_CAP=0`, the wait of the FSR routes is on, and `FFX_XEFG_POLLS=1` adds the answers. This has not been measured with optical flow.
* **HUD smear:** Because there is no HUD-less image, the HUD is interpolated with the rest of the image; text and fast HUD elements can smear slightly.
* **Disocclusion wobble:** Where something appears that was hidden in the previous frame, optical flow has nothing to follow, and with no depth buffer either, edges of moving objects can wobble.
* **DX12 only:** XeSS-FG only supports D3D12 swap chains (no D3D11 or Vulkan swap chain).

---

## 6. Logs and Configuration Variables

### Logs

The FSR and optical-flow DLLs write logs for the last run into the game's folder (Steam's file check ignores them):

* `ffx_xess.log` (next to `amd_fidelityfx_upscaler_dx12.dll`, disabled with `FFX_XESS_LOG=0`): logs the `libxess.dll` version, context creation, the quality setting and flags XeSS was initialized with, the first 3 upscale dispatches, and every 600th dispatch.
* `ffx_xefg.log` (next to `amd_fidelityfx_framegeneration_dx12.dll` / `ffx_xefg_bridge.dll`, disabled with `FFX_XEFG_LOG=0`): logs the upscalers the game's FidelityFX loader offers on this GPU, swap chain parameters, the maximum interpolations XeSS-FG reports and the number set, the first 10 presents (`presented 4` = 4x, with sync interval and flags), the first 5 prepares and every 600th prepare, XeSS-FG warnings, `oflow: GPU time` lines, and `cap auto:` interval changes. The `low latency:` lines say what is on. Every 300 presents, they also give how often and how long the game waited, and the time from frame start to present end.
* `dxgi_xefg.log` (next to `dxgi.dll`, with `FSR_FG=inject` or `OFLOW_FG=1`): logs the system `dxgi.dll` path and what became of each swap chain.

For native XeSS-FG, XeLL, and `ControlLib.dll` logs, see [troubleshooting.md](troubleshooting.md) and [dev/debugging.md](dev/debugging.md). These logs are `C:\igdext_xefg.log`, `C:\igdext_xell.log`, and `C:\igcl_stub.log` in the prefix. They are enabled with `XESS_LOG=1`, or individually via `IGDEXT_XEFG_LOG=1`, `IGDEXT_XELL_LOG=1`, and `XESS_FG_LOG=1`.

### Variables for `ffxfg/` (`FSR=xess`, `FSR_FG`, `OFLOW_FG`)

You can pass these variables in the game's Steam launch options, or put `KEY=VALUE` lines into `ffx_xess.env` / `ffx_xefg.env` next to the DLLs. Environment variables win over `.env` files, and `XESS_FG` itself belongs in the launch options because `igdext64.dll` reads it before the DLLs load:

| Variable | Effect |
|---|---|
| `FFX_XESS_LOG=<path>\|0` | Upscaler log file path (default `ffx_xess.log` next to the DLL); `0` turns it off |
| `FFX_XESS_XESSLOG=0..3` | XeSS's own messages forwarded to `ffx_xess.log`: `0` debug, `1` info, `2` warnings (default), `3` errors |
| `FFX_XESS_QUALITY=100..106` | Force the XeSS quality setting. The values are `100` Ultra Performance, `101` Performance, and `102` Balanced. They continue with `103` Quality, `104` Ultra Quality, `105` Ultra Quality Plus, and `106` Native AA |
| `FFX_XESS_AUTOEXPOSURE=0` | Debug: disable XeSS auto exposure |
| `FFX_XEFG_MULT=<n>` | Frames shown per rendered frame (default `XESS_FG`, else `2`), capped at XeSS-FG's maximum |
| `FFX_XEFG_LOG=<path>\|0` | Frame generation log file path (default `ffx_xefg.log` next to the DLL); `0` turns it off |
| `FFX_XEFG_XEFGLOG=0..3` | XeSS-FG's own messages forwarded to `ffx_xefg.log`: `0` debug, `1` info, `2` warnings (default), `3` errors |
| `FFX_XEFG_HUDLESS=0` | Ignore the game's HUD-less image and leave XeSS-FG UI composition off |
| `FFX_XEFG_SLEEP=0` | Skip calling `xellSleep` after each present (XeLL stays on, as XeSS-FG requires it) |
| `FFX_XEFG_OWNWAIT=0` | Hand the game the XeSS-FG proxy's own frame-latency waitable object instead of the DLL's semaphore (deadlocks CONTROL Resonant) |
| `FFX_XEFG_PACE=1` | Release the DLL's waitable object when a GPU fence after the present finishes (with a 200 ms watchdog), instead of when `Present` returns (off by default; CONTROL Resonant at 6x showed no gain) |
| `FFX_XEFG_POLLS=0\|1` | `0`: the game's `GetFrameStatistics` and `CheckColorSpaceSupport` go to the swap chain each time, as before. `1`: the DLL answers them where that is off by default (`OFLOW_FG`, `IGDEXT_XELL_CAP=off`) |
| `FFX_XEFG_AB=<s>` | Measuring aid: the low latency's wait is off and on in turns of `<s>` seconds. With `FFX_XEFG_AB_OLD=1`, three turns: as before, answers only, answers and wait |
| `FFX_XEFG_HOLD=<ms>` | Measuring aid: a fixed wait in Present instead of the measured one |
| `FFX_XEFG_VALIDITY=0` | Measuring aid: tag depth and motion vectors as `UNTIL_NEXT_PRESENT` instead of `ONLY_NOW` (avoids copying them at prepare time to measure what the copy costs; can flicker if the game overwrites them before present) |
| `FFX_XEFG_EXCLUSIVE=1` | Let exclusive fullscreen requests through to the swap chain (XeSS-FG then refuses to interpolate) |
| `FFX_XEFG_VENDOR=0` | Disable the adapter `GetDesc*` hook that reports Intel's `0x8086` vendor ID to XeSS-FG and XeLL when DXVK spoofs another vendor (such as `FSR_VENDOR=amd`) |
| `FFX_XEFG_MVNDC=1` | Pass `XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY` and scale motion vectors to Normalized Device Coordinates (NDC) |
| `FFX_XEFG_OFLOW=1` | Compute motion vectors from the final backbuffer using FidelityFX optical flow and use constant depth (`OFLOW_FG=1` sets this) |
| `FFX_XEFG_OFLOW_DIV=1..4` | Run optical flow at `1/n` of the display resolution (`1` to `4`, default `1`) |
| `FFX_XEFG_OFLOW_DEPTH=<d>` | Constant depth value written for optical flow (default `1.0`) |
| `FFX_XEFG_CAP=auto\|<us>\|0` | Rendered frame interval in microseconds via XeLL: `auto` (default with `OFLOW_FG=1`), `<us>` fixed interval per rendered frame, or `0` (off) |
| `FFX_XEFG_CAP_PAD=<ms>` | Initial guess in milliseconds of the per-frame time beyond the measured GPU time (interpolation and present) for `FFX_XEFG_CAP=auto` (default `1.5`, then learned) |
| `FFX_XEFG_REFRESH=<Hz>` | Override the display refresh rate for `FFX_XEFG_CAP=auto` when Windows/Wine reports a wrong one (default from `EnumDisplaySettingsA`, or `144` Hz) |
| `FFX_XEFG_TIMES=1` | Log frame time, time in `Present`, and `xellSleep` percentiles every 300 frames, plus optical-flow GPU time every 300 runs |

*(Debug switches that alter or dump individual inputs are compiled in only when building `ffxfg/` with `FFXFG_DEBUG=1`; see [dev/building.md](dev/building.md) and [dev/debugging.md](dev/debugging.md). This covers `FFX_XEFG_CAMERA`, `FFX_XEFG_JITTER`, `FFX_XEFG_RENDERTIME`, and `FFX_XEFG_RENDERSCALE`. It also covers `FFX_XEFG_DUMP`, `FFX_XEFG_ORDER`, and `FFX_XESS_JITTER`, and the frame time line `FFX_XEFG_TRACE=1`.)*
