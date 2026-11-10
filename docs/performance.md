# Performance (2026-11-05)

**Device**: OneXPlayer 3, Intel Arc B390 (Panther Lake, Arc G3 Extreme), SteamOS, 1920x1200, SoC (system-on-chip power limit) 27 W.

**Method**: the built-in benchmarks of Forza Horizon 6 and Cyberpunk 2077, one run each.

**Terms**
- **DP4a** (8-bit integer dot-product instructions): XeSS's generic fallback path, which is what XeSS (Xe Super Sampling, Intel's upscaler and frame generator) uses on Linux without this project. For these runs, `IGDEXT_FORCE_FALLBACK=1` (`XESS=dp4a`, see [launch-options.md](launch-options.md)) makes the shim decline Intel's extension while keeping the same XeSS libraries.
- **XMX** (Xe Matrix Extensions, the GPU's hardware matrix engines): the native path this project enables.
- **FG 3x**: XeSS frame generation at 3x (`XESS_FG=3`). XeSS's generic path offers at most 2x, so DP4a is compared without frame generation only.
- **26.1.2 / 26.3**: this project's patched Mesa ANV (Intel's open-source Vulkan driver) builds from release `20261104` (Mesa `26.1.2` / `26.3.0-rc3`).
- **Unpatched + layer**: Mesa `26.3.0-rc3` as Mesa ships it, with this project's Vulkan layer (`VK_LAYER_XESS_xmx`) running the same XMX kernels (`xmx-stock.sh`, experimental, from release `20261107`; now the session default on Xe3, see [drivers.md](drivers.md)).
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
- 26.3 is about 22% faster than 26.1.2, mostly because Jay (Mesa's new shader compiler backend) compiles the game's own shaders better.
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

## 3. Latency (the low-latency mode with frame generation)

Frame generation lets rendered frames queue before they are shown, which adds input latency. On Windows, Intel's XeLL (Xe Low Latency) relies on the graphics driver to pace the game; under Proton, that driver mode does not start and XeLL runs in its generic cross-vendor mode.

This project implements its own low-latency mode on top of the XeLL API (`shim/src/dll/XellHook.cpp`): the shim intercepts the game's XeLL calls, reads the per-frame timestamps the game reports to XeLL (simulation, render submit, present) to detect when frames are queuing, and waits before starting the next frame so the queue drains. It is enabled by default while frame generation is active and can be turned off with `IGDEXT_XELL_CAP=off` (since release `20261104`). It applies to a game's own XeSS frame generation; on the FSR routes (`FSR=xess`, `FSR_FG`, `OFLOW_FG`), the package's frame generation DLL reports each present right before the next frame's sleep, so the shim sees no queue and does not wait (see [frame-generation.md](frame-generation.md)).

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
- Issue #19 also has a Final Fantasy VII Rebirth comparison showing about +15% total gain (roughly half from Jay, half from XMX). Not measured yet: image quality differences and the cost of disabling `VK_EXT_descriptor_buffer`.

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
