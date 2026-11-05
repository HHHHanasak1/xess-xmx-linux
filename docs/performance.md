# Performance (2026-11-05)

**Device**: OneXPlayer 3, Intel Arc B390 (Panther Lake, Arc G3 Extreme), SteamOS, 1920x1200, SoC 27 W.

**Method**: the built-in benchmarks of Forza Horizon 6 and Cyberpunk 2077.

**Terms**
- **DP4a**: XeSS' generic path, which is what XeSS uses on Linux without this project. For these runs
  `IGDEXT_FORCE_FALLBACK=1` makes the shim decline Intel's extension; the XeSS libraries stay the same.
- **XMX**: the path this project gives XeSS.
- **FG 3x**: XeSS frame generation 3x (`XESS_FG=3`). XeSS' generic path offers 2x at most, so DP4a is compared
  without frame generation only.
- **26.1.2 / 26.3**: this project's patched drivers of release 20261104 (Mesa 26.1.2 / 26.3.0-rc3).
- **Unpatched + layer**: Mesa 26.3.0-rc3 as Mesa ships it, with a Vulkan layer of this project that runs the same
  kernels on it. The layer is not in a release yet.
- All runs use the libraries of Intel's XeSS SDK 3.0.2 (`XESS=latest`).

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
- 26.3 is about 22% faster than 26.1.2, mostly because Jay compiles the game's own shaders better.
- Frame generation 3x: on 26.3 from 61.3 to 117.0 fps (about 1.9x).

## 2. Forza Horizon 6

**Settings**: render resolution 1280x800 (1.5x); night shadows Ultra, motion blur Short, everything else High; ray
tracing off.
Super resolution comes from one of two sources: the game's own XeSS ("XeSS-SR Ultra Quality"), or "AMD FSR 4.0.2
Quality" in the game, which this project turns into XeSS (`FSR=xess`). Frame generation is only available through the
latter (`FSR_FG=inject`).

**Frame rate (GPU frame rate in brackets)**

| | Super resolution | Mesa 26.1.2 | Mesa 26.3.0-rc3 |
|---|---|---|---|
| DP4a | game's XeSS | 43 (47.3) | 46 (52.5) |
| XMX | game's XeSS | 44 (49.9) | 46 (55.0) |
| XMX | FSR to XeSS | — | 47 (55.3) |
| XMX + FG 3x | FSR to XeSS | 94 (47.9) | 98 (52.5) |
| XMX + FG 3x, unpatched + layer | FSR to XeSS | — | 103 (51.6) |

- On this device Forza Horizon 6 is held back by its render thread (CPU render about 48 fps), so the frame rate moves
  little; compare the GPU frame rate.
- GPU frame rate: XMX is about 5% faster than DP4a (26.3: +4.8%, 26.1.2: +5.5%); 26.3 is about 10% faster than
  26.1.2.
- FSR turned into XeSS performs like the game's own XeSS (GPU 55.3 against 55.0): the conversion costs nothing
  measurable.
- Unpatched + layer gives 103 fps, the patched driver 98; the GPU frame rate is about the same, 74 stutters against
  162.

## 3. Latency (the low latency mode with frame generation)

Frame generation lets frames queue before they are shown, which adds latency. On Windows, Intel's XeLL relies on the
driver to pace the game; under Proton that driver mode does not start, and XeLL runs only in its generic mode. This
project implements its own low latency mode on the XeLL API: the shim takes over the game's XeLL calls, uses the
time stamps the game reports to XeLL (simulation, render, present) to tell whether frames queue, and waits before the
game's next frame to remove the queue. It is on by default when frame generation is on and can be switched off with
`IGDEXT_XELL_CAP=off` (since release 20261104).

**Test**: Wuthering Waves, frame generation 3x, SoC 22 W. Latency is the time from the start of a frame's simulation
to the end of its present, taken from those XeLL time stamps (`IGDEXT_XELL_LOG=1`). The builds measured were
development versions; their algorithm is the one of the default mode in the release.

| Scene | Low latency | Frame time (ms) | Average latency (ms) | Latency change |
|---|---|---|---|---|
| GPU-limited (fixed spot, 10 minutes) | off | 25.07 | 69.0 | |
| | on | 25.12 | 34.8 | **−50%** |
| CPU-limited (CPU held to about 2 W, 4–6 minutes) | off | 28.7 | 76.1 | |
| | on | 28.7 | 47.8 | **−37%** |
| Render-thread-limited (heavy scene while moving, about 6 minutes) | off | 42.0 | 105.5 | |
| | on | 42.2 | 67.2 | **−36%** |

- In all three cases the frame rate does not change and latency drops by 36–50%.
- In the CPU-limited and the moving scene, the runs with and without the mode did not show the same stretch of the
  game, so only the rough size of the change counts.

## 4. Summary

The starting point is the behaviour **without this project**: Mesa 26.1.2 + DP4a (SteamOS ships Mesa 26.1.2, and
without this project XeSS can only use DP4a). In these tests that row uses the 26.1.2 of this project's driver
package with the shim falling back to DP4a, which is the same path.

| | Cyberpunk 2077 | Forza Horizon 6 |
|---|---|---|
| Without this project (26.1.2 + DP4a) | 47.7 fps | 43 fps (GPU 47.3) |
| 26.3 + XMX | 61.3 fps, **+28%** | 46 fps, **+7%** (GPU 55.0, +16%) |
| 26.3 + XMX + FG 3x | 117.0 fps, **about 2.5x** | 98 fps, **about 2.3x** |
| 26.3 + XMX + FG 3x, unpatched + layer | 118.9 fps, about 2.5x | 103 fps, about 2.4x |

- Super resolution alone, from "without this project" to "26.3 + XMX": Cyberpunk 2077 gets 28% faster, about 23 points
  of it from the newer driver (Jay) and about 5 from XMX. Forza Horizon 6, held back by its render thread, gains only
  7% in frame rate but 16% on the GPU.
- With frame generation 3x on top, both games run at 2.3–2.5 times the starting frame rate, and the low latency mode
  built on the XeLL API removes 36–50% of the latency the frame queue would add.

---

## Appendix: all data

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

All columns are from the game's result screen: frame rate, CPU simulation, CPU render and GPU are frames per second,
stutters a count, "GPU limited" the share of the time the GPU was the bottleneck, average latency in milliseconds.
