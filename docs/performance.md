# Performance

**Device**: ONEXPLAYER 3, Intel Arc B390 (Panther Lake, Arc G3 Extreme), SteamOS, 1920x1200, SoC (system-on-chip) power limit 27 W.

**Method**: the built-in benchmarks of Forza Horizon 6 and Cyberpunk 2077, one run each.

**Terms**
- **DP4a** (8-bit integer dot-product instructions): XeSS's generic fallback path, which is what XeSS (Xe Super Sampling, Intel's upscaler and frame generator) uses on Linux without this project. For these runs, `IGDEXT_FORCE_FALLBACK=1` (`XESS=dp4a`, see [launch-options.md](launch-options.md)) makes the shim decline Intel's extension while keeping the same XeSS libraries.
- **XMX** (Xe Matrix Extensions, the GPU's hardware matrix engines): the native path this project enables.
- **FG 3x**: XeSS frame generation at 3x (`XESS_FG=3`). XeSS's generic path offers at most 2x, so DP4a is compared without frame generation only.
- **26.1.2 / 26.3**: this project's patched Mesa ANV (Intel's open-source Vulkan driver) builds from release `20261104` (Mesa `26.1.2` / `26.3.0-rc3`).
- **Unpatched + layer**: Mesa `26.3.0-rc3` as Mesa ships it, with this project's Vulkan layer (`VK_LAYER_XESS_xmx`) running the same XMX kernels. It was first available as `xmx-stock.sh` from release `20261107`. The package's unpatched driver + layer is now the session default on the Xe3 and Xe2 GPUs of `layer/gpu-ids.txt` ([drivers.md](drivers.md)); these runs are on Xe3.
- All runs use the libraries from Intel's XeSS SDK 3.0.2 (`XESS=latest`).

---

## 1. Cyberpunk 2077

**Settings**: Steam Deck preset with motion blur and vignette off; the highest XeSS preset (1.3x); ray tracing off.

**Average fps (low / high)**

| | Mesa 26.1.2 | Mesa 26.3.0-rc3 |
|---|---|---|
| DP4a | 47.7 (40.5 / 55.5) | 58.6 (49.4 / 69.7) |
| XMX | 50.1 (42.1 / 59.1) | 61.3 (51.2 / 74.1) |
| XMX + FG 3x | 86.7 (75.9 / 102.0) | 117.0 (98.6 / 135.3) |
| XMX + FG 3x, unpatched + layer | — | 118.9 (104.4 / 136.3) |

- XMX is about 5% faster than DP4a (26.3: +4.6%, 26.1.2: +4.9%).
- 26.3 is about 22% faster than 26.1.2, mostly because Jay (Mesa's new shader compiler backend) makes the game's own shaders run faster.
- Frame generation 3x: on 26.3, frame rate rises from 61.3 to 117.0 fps (about 1.9x).

---

## 2. Forza Horizon 6

**Settings**: render resolution 1280x800 (1.5x); night shadows Ultra, motion blur Short, everything else High; ray tracing off.

Super resolution (SR) comes from one of two sources: the game's own XeSS ("XeSS-SR Ultra Quality"), or "AMD FSR 4.0.2 Quality" in the game translated to XeSS (`FSR=xess`). Frame generation is available through the FSR route (`FSR=xess FSR_FG=inject`, see [frame-generation.md](frame-generation.md)).

**Frame rate (GPU frame rate in brackets)**

| | Super resolution | Mesa 26.1.2 | Mesa 26.3.0-rc3 |
|---|---|---|---|
| DP4a | game's XeSS | 43 (47.3) | 46 (52.5) |
| XMX | game's XeSS | 44 (49.9) | 46 (55.0) |
| XMX | FSR to XeSS | — | 47 (55.3) |
| XMX + FG 3x | FSR to XeSS | 94 (47.9) | 98 (52.5) |
| XMX + FG 3x, unpatched + layer | FSR to XeSS | — | 103 (51.6) |

- On this device, Forza Horizon 6 is held back by its CPU render thread (CPU render is about 48 fps), so overall frame rate moves little; compare the GPU frame rate instead.
- GPU frame rate: XMX is about 5% faster than DP4a (26.3: +4.8%, 26.1.2: +5.5%); 26.3 is about 10% faster than 26.1.2.
- FSR translated to XeSS performs like the game's own XeSS (GPU 55.3 against 55.0): the translation costs nothing measurable.
- Unpatched + layer gives 103 fps against 98 fps on the patched driver; GPU frame rate is about the same, with 74 stutters against 162. Each figure is from a single run, and single runs vary: another run with the same settings on the unpatched driver gave 101 fps and 137 stutters.
- Unpatched + layer against the patched driver: about the same to slightly faster in Cyberpunk 2077 and Forza Horizon 6, the two games in which both were measured, one run each.

---

## 3. Latency (Low Latency with Frame Generation)

Frame generation lets rendered frames queue before they are shown, and the input a queued frame was made from gets old. The package's low latency removes that queue on all three frame generation paths ([frame-generation.md](frame-generation.md), section 2). It is on by default while frame generation runs; `XESS_LL=off` (`IGDEXT_XELL_CAP=off`) turns it off.

**Test**: 2026-11-15, Arc B390, SteamOS, 1920x1200, 144 Hz VRR, unpatched Mesa 26.3 + layer, XeSS-FG 1.3.1. Separate runs, interleaved, at least 45 s apart. The game stood at one spot without input; Cyberpunk 2077 and Forza Horizon 6 (settings of section 2) ran their built-in benchmarks.

Three settings:
- **Off:** `XESS_LL=off`. There is no wait. On the FSR routes the swap chain also answers the game's questions itself, as it did before the low latency had answers. With `OFLOW_FG`, its own pacing stays on: the rendered frame-rate cap that follows the GPU's measured time (`FFX_XEFG_CAP=auto`, [frame-generation.md](frame-generation.md)) and is its default instead of the wait.
- **20261113:** the release `20261113`.
- **20261115:** the release `20261115`. For latency, its change is on the FSR routes: where a frame's first work lies more than 6 ms in the GPU's queue, that queue alone decides how long the wait is.

**Figures**
- **Latency:** the time from a frame's start to the moment the GPU has finished the frame's first shown image. The image can go to the display from then on.
  - For a game's own XeSS-FG, a frame starts at its XeLL simulation marker.
  - On the FSR routes and with `OFLOW_FG`, it starts when the game gets control back from Present, or from a swap-chain call that made it wait. Where a game actually reads its input differs from game to game (below). The frame generation DLL logs this time every 300 frames (`low latency: waited ...`, [frame-generation.md](frame-generation.md)).
  - Where it could be checked against the GPU's fences and the presents (a Vulkan layer build for measuring, not in the package), the two agree within 1 ms.
- **Frame time:** the time between rendered frames.

Each cell gives **latency / frame time** in milliseconds: means over the runs, the last 60 s or so of each run, or the whole benchmark.

**A game's own XeSS-FG**

| Game | Off | 20261113 | 20261115 |
|---|---|---|---|
| Cyberpunk 2077, benchmark, 3x | 45.1 / 25.3 (116.6 fps) | as 20261115 (118.1 fps) | **31.7** / 24.8 (119.2 fps) |

- The wait lowers the latency by 13.4 ms (30%), and the benchmark is 2% faster with it. The wait is 10.7 ms a frame; the 90th percentile of the latency goes from 50.5 to 36.8 ms.
- `20261113`'s shim does not record single frames, so its latency to GPU done was not measured. Its own averages (simulation start to the end of the present: 37.2 ms) are the same as `20261115`'s, and the shim's low latency did not change between the two.
- An earlier test of the wait alone, in Wuthering Waves at 3x, gave 69.0 -> 34.8 ms where the GPU was the limit, 76.1 -> 47.8 ms where the CPU was, and 105.5 -> 67.2 ms in a heavy scene while moving, each at the same frame rate ([frame-generation.md](frame-generation.md)). That test counted from the simulation start to the end of the present, not to GPU done.

**FSR routes** (`FSR=xess`, `FSR_FG`)

| Game | Off | 20261113 | 20261115 |
|---|---|---|---|
| CONTROL Resonant, 3x | 37.2 / 35.2 | 52.6 / 33.8 | **48.4** / 33.0 |
| CONTROL Resonant, 6x | 45.6 / 43.7 | 46.1 / 42.0 | 46.1 / 41.9 |
| Resident Evil Requiem, 3x | 42.0 / 41.3 | 54.1 / 31.9 | 54.0 / 31.5 |
| Resident Evil Requiem, 5x | 44.0 / 40.3 | 57.5 / 38.5 | 57.9 / 38.8 |

Forza Horizon 6's built-in benchmark, 3x (`FSR_FG=inject`), from the game's result screen:

| | Off | 20261113 | 20261115 |
|---|---|---|---|
| Frame rate (fps) | 71 | 102 | 103 |
| GPU (fps) | 54.3 | 52.0 | 52.1 |
| Stutters | 7 | 92 | 103 |
| The game's average latency (ms) | 126.5 | 103.3 | 101.8 |
| Start -> GPU done (DLL, ms) | – | 44.0 | 43.2 |

- **Without the answers the GPU idles.** A game that asks the swap chain about each frame then waits for the answer until the GPU is done. Nothing queues, but the frame rate drops: Resident Evil Requiem at 3x takes 41.3 ms a frame instead of about 32 ms, and Forza Horizon 6's benchmark runs at 71 instead of 103 fps.
- **Whether Off then has the lower latency depends on where the game reads its input.** The table counts from the moment the game gets back from the waiting call. Counted from the controller read instead (`XInputGetState`, in a debug build with `FFX_XEFG_TRACE=1`, 2 runs each):
  - CONTROL Resonant at 3x reads its input after that call. Off stays lower: 38.9 against 46.0 ms, at 42.3 against 38.1 ms a frame.
  - Resident Evil Requiem at 3x reads its input on two threads, and on the first one before that call. Counted from that read, the default is lower: 52.7 against 62.4 ms. Counted from the later read, it is 45.3 against 46.9 ms.
  - Forza Horizon 6's own figure in its benchmark is 101.8 against 126.5 ms with Off, at 45% more frames.
- **The answers** bring the frame rate up. With the GPU kept busy, frames queue again. The wait then takes that queue away as far as the frame rate allows: the check keeps a wait only where it costs at most 1% of the frame time ([frame-generation.md](frame-generation.md), section 2).
- **CONTROL Resonant at 3x is the only case with a real queue on the GPU.** In `20261113`, a frame's first work still lay 11–12.5 ms in the GPU's queue there: the wait stopped at 9.7 ms, because a queued frame is held up in front of Present for only about 3.5 ms, and that held-up time decided the wait. In `20261115` the queue decides, and the wait is 11.2 ms. The latency drops from 52.6 to 48.4 ms at a slightly shorter frame time, and the check kept the wait.
- **Where nothing queues, the two releases are equal:** CONTROL Resonant at 6x, Resident Evil Requiem at 3x and 5x (at 5x, XeSS-FG's presentation holds the frames up for 16–31 ms in front of Present, so there is little or nothing to wait for), and Forza Horizon 6's benchmark (a wait of 0.4–0.5 ms). The differences there are within run-to-run variation.
- **The measuring tools raise Forza Horizon 6's own latency figure** by about 20 ms at the same frame rate and stutter count: a run without them gave 104 fps and 80.6 ms. The cause is not known, so the game's figure is compared only between runs of the same kind; all the runs above had the tools loaded.

**`OFLOW_FG=1`** (Resident Evil 4)

| Multiplier | Nothing (`XESS_LL=off FFX_XEFG_CAP=0`) | Off (the frame cap alone) | 20261113 | 20261115 |
|---|---|---|---|---|
| 2x | 39.6 / 23.7 | 38.4 / 23.8 | 39.7 / 24.3 | 38.2 / 24.2 |
| 3x | 27.9 / 25.0 | 27.85 / 25.05 | 28.3 / 25.1 | 27.8 / 25.1 |

- `XESS_LL=off` leaves the frame cap of `OFLOW_FG` on; `FFX_XEFG_CAP=0` turns it off as well.
- At this spot every setting gives about the same result: Resident Evil 4 does not queue here. At 3x the game is held at 40 rendered frames per second, and the images then wait about 13 ms for the display after the GPU has finished them.

---

## 4. Summary

The baseline is the behavior **without this project**: Mesa 26.1.2 + DP4a (SteamOS ships Mesa 26.1.2, and without this project XeSS can only use DP4a). In these tests, that row uses the 26.1.2 driver from this package with the shim falling back to DP4a, which executes the same path.

| | Cyberpunk 2077 | Forza Horizon 6 |
|---|---|---|
| Without this project (26.1.2 + DP4a) | 47.7 fps | 43 fps (GPU 47.3) |
| 26.3 + XMX | 61.3 fps, **+28%** | 46 fps, **+7%** (GPU 55.0, +16%) |
| 26.3 + XMX + FG 3x | 117.0 fps, **about 2.5x** | 98 fps, **about 2.3x** |
| 26.3 + XMX + FG 3x, unpatched + layer | 118.9 fps, about 2.5x | 103 fps, about 2.4x |

- Super resolution alone, moving from "without this project" to "26.3 + XMX": Cyberpunk 2077 is 28% faster, with about 23 percentage points coming from the newer driver (Jay) and about 5 from XMX. Forza Horizon 6, held back by its render thread, gains 7% in overall frame rate and 16% on the GPU.
- With frame generation 3x on top, both games run at 2.3–2.5 times the starting frame rate, and the low latency lowers the latency of a game's own XeSS-FG by 30% (Cyberpunk 2077: 45.1 -> 31.7 ms) at a slightly higher frame rate (section 3).
- Issue [#19](https://github.com/HHHHanasak1/xess-xmx-linux/issues/19) also has a Final Fantasy VII Rebirth comparison showing about +15% total gain (roughly half from Jay, half from XMX). Not measured yet: image quality differences, and how much the game itself slows down because vkd3d-proton runs without `VK_EXT_descriptor_buffer`.

---

## Appendix: All Data

### Cyberpunk 2077

| Driver | Path | FG | Average | Low | High | Frames |
|---|---|---|---|---|---|---|
| 26.1.2 | DP4a | off | 47.72 | 40.52 | 55.50 | 3065 |
| 26.1.2 | XMX | off | 50.07 | 42.14 | 59.05 | 3216 |
| 26.1.2 | XMX | 3x | 86.71 | 75.87 | 101.99 | 5571 |
| 26.3 | DP4a | off | 58.59 | 49.44 | 69.67 | 3763 |
| 26.3 | XMX | off | 61.31 | 51.21 | 74.11 | 3938 |
| 26.3 | XMX | 3x | 117.02 | 98.57 | 135.33 | 7515 |
| Unpatched + layer | XMX | 3x | 118.88 | 104.37 | 136.29 | 7635 |

### Forza Horizon 6

| Driver | Super resolution | Path | FG | Frame rate | CPU simulation | CPU render | GPU | Stutters | GPU limited (%) | Average latency |
|---|---|---|---|---|---|---|---|---|---|---|
| 26.1.2 | game's XeSS | DP4a | off | 43 | 87.7 | 48.5 | 47.3 | 108 | 70.4 | 68.3 |
| 26.1.2 | game's XeSS | XMX | off | 44 | 89.1 | 48.6 | 49.9 | 102 | 52.7 | 65.1 |
| 26.1.2 | FSR to XeSS | XMX | 3x | 94 | 84.3 | 47.8 | 47.9 | 134 | 7.6 | 93.8 |
| 26.3 | game's XeSS | DP4a | off | 46 | 90.1 | 49.1 | 52.5 | 113 | 42.2 | 62.5 |
| 26.3 | game's XeSS | XMX | off | 46 | 90.6 | 48.8 | 55.0 | 111 | 30.7 | 61.1 |
| 26.3 | FSR to XeSS | XMX | off | 47 | 91.8 | 50.2 | 55.3 | 118 | 34.4 | 60.2 |
| 26.3 | FSR to XeSS | XMX | 3x | 98 | 85.6 | 47.9 | 52.5 | 162 | 2.4 | 89.0 |
| Unpatched + layer | FSR to XeSS | XMX | 3x | 103 | 87.0 | 48.0 | 51.6 | 74 | 1.5 | 86.1 |

All columns come from the game's benchmark result screen: frame rate, CPU simulation, CPU render, and GPU are in frames per second, stutters is a count, "GPU limited" is the percentage of time the GPU was the bottleneck, and average latency is in milliseconds.
