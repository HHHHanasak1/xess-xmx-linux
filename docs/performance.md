# Performance (2026-11-05)

**Device**: ONEXPLAYER 3, Intel Arc B390 (Panther Lake, Arc G3 Extreme), SteamOS, 1920x1200, SoC (system-on-chip) power limit 27 W.

**Method**: the built-in benchmarks of Forza Horizon 6 and Cyberpunk 2077, one run each.

**Terms**
- **DP4a** (8-bit integer dot-product instructions): XeSS's generic fallback path, which is what XeSS (Xe Super Sampling, Intel's upscaler and frame generator) uses on Linux without this project. For these runs, `IGDEXT_FORCE_FALLBACK=1` (`XESS=dp4a`, see [launch-options.md](launch-options.md)) makes the shim decline Intel's extension while keeping the same XeSS libraries.
- **XMX** (Xe Matrix Extensions, the GPU's hardware matrix engines): the native path this project enables.
- **FG 3x**: XeSS frame generation at 3x (`XESS_FG=3`). XeSS's generic path offers at most 2x, so DP4a is compared without frame generation only.
- **26.1.2 / 26.3**: this project's patched Mesa ANV (Intel's open-source Vulkan driver) builds from release `20261104` (Mesa `26.1.2` / `26.3.0-rc3`).
- **Unpatched + layer**: Mesa `26.3.0-rc3` as Mesa ships it, with this project's Vulkan layer (`VK_LAYER_XESS_xmx`) running the same XMX kernels. It was first available as `xmx-stock.sh` (experimental) from release `20261107`. The package's unpatched driver + layer is now the session default on Xe3 ([drivers.md](drivers.md)). Xe3 here means `0xB080`–`0xB08F` / `0xFD80`–`0xFD8F`.
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
- Unpatched + layer gives 103 fps against 98 fps on the patched driver; GPU frame rate is about the same, with 74 stutters against 162.
- Unpatched + layer against the patched driver: matches in Cyberpunk 2077 and is faster in Forza Horizon 6. Other games may run slower on it; it needs more testing.

---

## 3. Latency (Low-Latency Mode with Frame Generation)

Frame generation lets rendered frames queue before they are shown, which adds input latency. On Windows, Intel's XeLL (Xe Low Latency) relies on the graphics driver to pace the game; under Proton, that driver mode does not start and XeLL runs in its generic cross-vendor mode.

This project implements its own low-latency mode on top of the XeLL API (`shim/src/dll/XellHook.cpp`): the shim intercepts the game's XeLL calls, reads the per-frame timestamps the game reports to XeLL (simulation, render submit, present) to detect when frames are queuing, and waits before starting the next frame so the queue drains. It is on by default (since release `20261104`) while frame generation is active; `IGDEXT_XELL_CAP=off` turns it off. The first test below is for a game's own XeSS frame generation. On the FSR routes (`FSR=xess`, `FSR_FG`), the game sends XeLL no time stamps. There the package's frame generation DLL does the same work with times it measures itself, behind the same switch. That is the second test.

**Test**: Wuthering Waves, frame generation 3x, SoC 22 W. Latency is the time from the start of a frame's simulation to the end of its present, taken from those XeLL timestamps (`IGDEXT_XELL_LOG=1`). The builds measured were development versions; their algorithm is the default mode in the release.

| Scene | Low latency | Frame time (ms) | Average latency (ms) | Latency change |
|---|---|---|---|---|
| GPU-limited (fixed spot, 10 minutes) | off | 25.07 | 69.0 | |
| | on | 25.12 | 34.8 | **−50%** |
| CPU-limited (CPU held to about 2 W, 4–6 minutes) | off | 28.7 | 76.1 | |
| | on | 28.7 | 47.8 | **−37%** |
| Render-thread-limited (heavy scene while moving, about 6 minutes) | off | 42.0 | 105.5 | |
| | on | 42.2 | 67.2 | **−36%** |

- In all three cases, the frame rate does not change and latency drops by 36–50%.
- In the CPU-limited and moving scenes, the runs with and without low-latency mode did not cover the exact same stretch of gameplay, so only the rough magnitude of the reduction counts.

**Test (FSR routes)**: CONTROL Resonant (`FSR=xess`), Resident Evil Requiem (`FSR_FG=xess`) and Forza Horizon 6 (`FSR=xess FSR_FG=inject`). Arc B390, XeSS-FG 1.3.1, 2026-11-11. The game stood at one spot without input. Forza Horizon 6 stood on its 3D car page, a light load for the GPU.

Three settings took turns of 20 s within one run (`FFX_XEFG_AB=20 FFX_XEFG_AB_OLD=1`). Each setting got 4 turns and 1600 to 2400 frames:

* **before:** the swap chain answers the game's questions, and there is no wait. This is the old behaviour, and what `IGDEXT_XELL_CAP=off` gives.
* **answers:** the DLL answers the questions, with no wait.
* **low latency:** the answers and the wait. This is the default.

The times come from the DLL's time line (`FFX_XEFG_TRACE=1` in a debug build, read by `tools/fsr-trace.py`). Latency is given two ways:

* from the frame's start (its Present returned to the game) to the end of its present, as XeLL counts it;
* from the game's controller read (`XInputGetState`) to the moment the GPU has finished the frame's interpolation. That is when its first image can go to the display.

The controller read was checked against XeLL in Wuthering Waves, with its own XeSS-FG. It comes 0.10 ms after the game's simulation start marker. It gives XeLL's latency in both states: 72.7 against 72.8 ms with pacing off, 34.3 against 34.4 ms with pacing on.

"Held up" is the time from the game's Present call to the moment XeSS-FG takes the frame.

| Game | Setting | Frame rate (fps) | Frame time mean / p95 / p99 (ms) | Held up (ms) | Frame start -> present end (ms) | Controller read -> GPU done (ms) |
|---|---|---|---|---|---|---|
| CONTROL Resonant, 3x | before | 28.5 | 35.1 / 39.3 / 41.6 | 7.3 | 34.7 | 47.7 |
| | answers | 29.4 | 34.0 / 35.1 / 35.6 | 18.0 | 33.7 | 61.3 |
| | low latency | 29.3 | 34.1 / 35.6 / 36.6 | 3.0 | **19.0** | **46.3** |
| CONTROL Resonant, 6x | before | 22.4 | 44.6 / 47.3 / 48.6 | 21.5 | 44.3 | 55.1 |
| | answers | 22.5 | 44.4 / 47.2 / 48.1 | 27.7 | 44.1 | 61.3 |
| | low latency | 22.4 | 44.7 / 47.7 / 48.6 | 16.6 | **33.2** | **50.6** |
| Resident Evil Requiem, 3x | before | 26.1 | 38.3 / 49.9 / 52.7 | 2.4 | 37.8 | 49.3 |
| | answers | 31.1 | 32.1 / 34.0 / 36.7 | 8.7 | 31.7 | 57.7 |
| | low latency | 31.5 | 31.8 / 33.7 / 35.4 | 5.4 | **27.0** | **52.7** |
| Resident Evil Requiem, 6x | before | 22.1 | 45.2 / 48.8 / 52.7 | 16.5 | 44.7 | 56.6 |
| | answers | 23.0 | 43.5 / 46.0 / 50.5 | 20.6 | 43.1 | 59.5 |
| | low latency | 23.1 | 43.3 / 46.0 / 49.1 | 20.5 | 42.9 | 59.4 |
| Forza Horizon 6, 3x | before | 23.5 | 42.6 / 60.2 / 64.6 | 1.0 | 42.0 | 41.4 |
| | answers | 39.4 | 25.4 / 26.4 / 31.0 | 2.0 | 25.0 | 39.4 |
| | low latency | 39.4 | 25.4 / 27.0 / 29.8 | 0.9 | 24.9 | 40.9 |

- **Before**, each game switched every few seconds between two states. In one, frames queue in front of Present. In the other, the next frame starts only when the GPU has finished this one, and the GPU idles meanwhile.
- **The answers** end that. The frame rate is the higher one and steady: p95 frame time 39.3 -> 35.1 ms in CONTROL Resonant, 49.9 -> 34.0 ms in Resident Evil Requiem, 60.2 -> 26.4 ms in Forza Horizon 6.
- **The wait** then removes the queue as far as the GPU allows. Against the answers alone, frame start to present end drops by 14.7 ms (−44%) in CONTROL Resonant at 3x, by 10.9 ms (−25%) at 6x, and by 4.7 ms (−15%) in Resident Evil Requiem at 3x.
- **Where nothing queues, the game does not wait:** Resident Evil Requiem at 6x (the GPU is still rendering when XeSS-FG takes the frame) and Forza Horizon 6. There the last two rows differ by chance only.
- **Cost of the wait** in frame rate, against the answers alone: 0.4% in CONTROL Resonant at 3x, 0.7% at 6x, nothing measurable in Resident Evil Requiem at 3x.
- **Forza Horizon 6 benchmark:** the built-in benchmark, with the settings of section 2 and FG 3x (`FSR=xess FSR_FG=inject XESS_FG=3`, unpatched + layer), the second of two runs each. Average latency from the result screen: 86.7 -> 81.8 ms. Stutters: 137 -> 77. The frame rate rises only slightly (101 -> 103 fps). On the race track the GPU is busy for about 26 ms of each 28 ms frame, XeSS-FG's work included, so there is no idle GPU time to win back. The game's "GPU limited" figure (2.4%) counts only the game's own work.
- **Left open:** At 3x, CONTROL Resonant's first work still waits 8.7 ms in the GPU queue. A longer wait would use that, but then the DLL can no longer tell whether the GPU or the game is the limit. A fixed wait of 21 ms (`FFX_XEFG_HOLD=21`) gave 33.9 ms to GPU done but cost 7% of the frame rate.

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
- With frame generation 3x on top, both games run at 2.3–2.5 times the starting frame rate, and the low-latency mode built on the XeLL API removes 36–50% of the latency that the frame queue would otherwise add.
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
