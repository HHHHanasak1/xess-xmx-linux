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

This project implements its own low-latency mode on top of the XeLL API (`shim/src/dll/XellHook.cpp`): the shim intercepts the game's XeLL calls, reads the per-frame timestamps the game reports to XeLL (simulation, render submit, present) to detect when frames are queuing, and waits before starting the next frame so the queue drains. It is on by default (since release `20261104`) while frame generation is active; `IGDEXT_XELL_CAP=off` turns it off. The first test below is for a game's own XeSS frame generation. On the FSR routes (`FSR=xess`, `FSR_FG`), the game sends XeLL no time stamps. There the package's frame generation DLL does the same work with times it measures itself, behind the same switch. That is the second test. The third test compares all three frame generation paths with the release build.

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

Three settings took turns of 20 s within one run (`FFX_XEFG_AB=20 FFX_XEFG_AB_OLD=1`, switches of a debug build since then). Each setting got 4 turns and 1600 to 2400 frames:

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
- **"Before" in this test** is the old code path inside the measuring build, with the game's HDR output on. The next test compares with the release build itself.
- **Left open:** At 3x, CONTROL Resonant's first work still waits 8.7 ms in the GPU queue. A longer wait would use that, but then the DLL can no longer tell whether the GPU or the game is the limit. A fixed wait of 21 ms (`FFX_XEFG_HOLD=21`, a debug-build switch since then) gave 33.9 ms to GPU done but cost 7% of the frame rate.

**Test (all three paths, against the release build)**: 2026-11-11, Arc B390, SteamOS, 1920x1200, 144 Hz VRR, XeSS-FG 1.3.1. Separate runs, interleaved, the same spot or the same benchmark each time, no input. The release build is the package as installed: its shim and its frame generation DLL. "This build" has the low latency with the wait, the answers and the check on all three paths ([frame-generation.md](frame-generation.md), section 2).

Frame time and latency come from the shim's log (`IGDEXT_XELL_LOG=1`): one line per frame, and XeLL's time from simulation start to present end. The GPU figure is the game's share of the render engine over 10 s in the middle of a run. For the runs at a spot, the figures are for the last 60 s of a run.

*FSR routes* (the game stands still at one spot):

| Game | Build | Runs | Frame time (ms) | Latency (ms) | GPU render engine | Wait |
|---|---|---|---|---|---|---|
| Resident Evil Requiem, 3x, HDR | release | 5 | 42.9–47.9 | 41.6–47.5 | 59–64% | |
| | release DLL with this build's shim (the shim's answers) | 1 | 31.8 | 31.4 | 89% | none |
| | this build | 3 | 31.3–32.0 | **28.4–29.6** | 88–90% | 1.8–3.1 ms |
| Resident Evil Requiem, 3x, SDR (`DXVK_HDR=0`) | release | 2 | 29.3 | 28.7–28.9 | 95–96% | |
| | release DLL with this build's shim | 1 | 29.2 | 28.7 | 96% | none |
| | this build | 1 | 29.2 | 28.5 | 96% | 0.5 ms in half of the frames |
| Resident Evil Requiem, 5x, HDR | release | 1 | 40.6 | 40.1 | 78% | |
| | an earlier build of the wait and the answers | 1 | 38.9 | 38.6 | 85% | none |
| CONTROL Resonant, 3x | release | 2 | 35.9–36.2 | 35.4–35.7 | 77–80% | |
| | release DLL with this build's shim (the shim's answers) | 1 | 32.8 | 32.4 | 89% | none |
| | this build | 3 | 32.9–33.1 | **22.6–23.3** | 89% | 9.9–10.1 ms |
| CONTROL Resonant, 6x | release | 1 | 42.6 | 42.2 | 84% | |
| | an earlier build of the wait and the answers | 1 | 42.5 | **34.9** | 84% | 7.3 ms |
| Forza Horizon 6, 3x, car page | release | 2 | 49.2–49.7 | 48.2–48.8 | | |
| | this build | 1 | 25.3 | **24.9** | 72% | none |

*A game's own XeSS-FG* (Cyberpunk 2077's built-in benchmark, 3x, 27 W; started with the game's `-benchmark` option, so no input at all; frame time and latency over the benchmark's scene):

| Build | Runs | Benchmark average (fps) | Minimum (fps) | Frame time (ms) | Latency (ms) | Held in `xellSleep` (ms) |
|---|---|---|---|---|---|---|
| release | 7 | 118.9–120.0 | 104.3–106.2 | 25.2–25.5 | 39.0–39.5 | 9.3–9.7 |
| release, `IGDEXT_XELL_CAP=off` | 2 | 116.9–117.0 | 101.0–101.4 | 25.8–25.9 | 50.8 | 0.3 |
| this build | 5 | 118.8–120.1 | 104.1–106.5 | 25.3–25.5 | 38.9–39.6 | 9.5–9.8 |

*`OFLOW_FG=1`* (Resident Evil 4, in the game, standing still):

| Multiplier | Build and setting | Runs | Frame time (ms) | Latency (ms) | GPU render engine |
|---|---|---|---|---|---|
| 2x | release (the frame cap) | 2 | 24.0–24.2 | 21.6–23.1 | 87–89% |
| | this build with `IGDEXT_XELL_POLLS=0` (the frame cap alone) | 1 | 24.0 | 22.9 | 89% |
| | this build (the frame cap and the answers) | 1 | 24.3 | 22.5 | 88% |
| | this build with `FFX_XEFG_CAP=0` (the wait of the FSR routes and the answers) | 2 | 23.9–24.1 | 23.4–23.5 | 88% |
| | this build with `FFX_XEFG_CAP=0 IGDEXT_XELL_CAP=off` (nothing) | 1 | 23.8 | 23.4 | |
| 3x | release (the frame cap) | 2 | 30.5–30.9 | 29.9 | 76–79% |
| | this build (the frame cap and the answers) | 3 | 27.6–27.8 | **27.0–27.1** | 88% |
| | this build with `FFX_XEFG_CAP=0` (the wait of the FSR routes and the answers) | 1 | 27.6 | 26.4 | 88% |

- **Resident Evil Requiem asks the swap chain about its colour space every frame only while its HDR output is on.** With HDR (its normal state on this device), the release build ran with an idle GPU in every run: 42.9–47.9 ms a frame in 5 runs, with the render engine at 59–64%. This build is at 31.3–32.0 ms there, and the wait takes the latency from 31.1–31.7 ms (answers only) to 28.4–29.6 ms.
- **One release run of that day was fast** (29.4 ms, render engine 95%). That run was in SDR: of the game's 67 starts before this test that day, it is the only one whose swap chain had the sRGB colour space instead of HDR10 (gamescope's Vulkan layer logs every swap chain, and gamescope did not switch its HDR output on). With `DXVK_HDR=0` the release build is at 29.3 ms every time. This build is equal there (29.2 ms): in SDR nothing queues at this spot, so there is almost no wait.
- **HDR itself costs.** In both builds a frame takes about 2.5 ms longer in HDR than in SDR at 3x. The game's share of the render engine is 88–90% in HDR and 95–96% in SDR, and the GPU is never idle in either. Most of the difference is gamescope: with HDR output it composites every shown frame on Intel GPUs instead of scanning the game's image out directly (direct HDR scanout needs plane colour properties that only AMD's display driver has). Checked on SteamOS with kernel 7.2 and gamescope 3.17: Intel's `xe` driver has the kernel's generic plane colour pipeline (`COLOR_PIPELINE`: a 128-entry 1D LUT, a 3x4 matrix and a 32-entry 1D LUT per plane), but gamescope only uses AMD's own plane properties (`AMD_PLANE_*`), so it composites whenever a colour conversion is needed. In Resident Evil 4 with `OFLOW_FG` at 2x, the GPU time outside the game was about 8% with HDR and 1–2.5% without, and a frame took 24.4 ms against 23.3 ms, on the patched and the unpatched driver alike. This is outside the project; turn HDR off in the game (or `DXVK_HDR=0 %command%`) if the frame rate matters more.
- **The wait is not what costs.** With the answers alone the frame time is the same (31.7–32.1 ms). In the three games the wait costs 0 to 0.2 ms a frame. A wait that is forced too long (`IGDEXT_XELL_CAP=auto:-20`, Resident Evil Requiem) costs 1.1–1.8 ms.
- **Other multipliers:** at 5x, Resident Evil Requiem does not queue, so there is no wait; the answers give 4% more frames. At 6x, CONTROL Resonant's frame time stays the same and its latency drops by 7.3 ms (17%).
- **Forza Horizon 6's car page:** the release build's GPU idled 35–41% of the time in both runs. This build shows 118 frames per second there. Nothing queues, so it does not wait.
- **A game's own XeSS-FG:** the release build already had the wait. Without it (`IGDEXT_XELL_CAP=off`) the benchmark is 2% slower and the latency 12 ms higher. This build is equal to the release build in frame rate. Cyberpunk 2077 does not ask the swap chain, so the answers do nothing there. The benchmark's scene changes all the time, so the check found no steady stretch: it never took the wait away (5 runs, 119.3 fps on average against 119.6 for the release build's 7 runs; in the last two pairs, run in turns, 119.9 and 120.1 against 119.7 and 119.6). Earlier builds of the check, which asked for less steadiness, took the wait away once or twice per benchmark for about 3 s and learned nothing; the average latency was 0.4–1.4 ms higher in those four runs (118.6–119.7 fps).
- **The shim's answers** were checked where a game asks: the release frame generation DLL (which has no answers of its own) with this build's shim. With HDR, Resident Evil Requiem went from 42.9–47.9 to 31.8 ms a frame and CONTROL Resonant from 35.9–36.2 to 32.8 ms, the same as with the frame generation DLL's own answers. The shim's log names what a game asks. Resident Evil Requiem asks about a colour space (12, HDR10) on its main thread and for the frame statistics on another thread. In SDR it asks for the frame statistics only: the question about the colour space is the one that held its frames up. CONTROL Resonant asks for the frame statistics.
- **`OFLOW_FG`:** at 2x nothing queues at this spot, and every setting gives the same frame time (23.8–24.3 ms) and about the same latency. At 3x the release build ran with an idle GPU part of the time: 30.5–30.9 ms a frame, with a 95th percentile of 43–44 ms. With the answers it is 27.6–27.8 ms (10% shorter) with a 95th percentile of 32 ms, and the latency is 2.9 ms lower, under the same frame cap. Without the cap, the wait of the FSR routes gave the same frame time and 0.6 ms less latency, and its check found the wait too costly there. So the frame cap stays the default with `OFLOW_FG`, and the answers are added to it.
- **The check at a spot:** where the game stands still the frame time is steady, and the check decides. Resident Evil Requiem (HDR): in six checks the frame time with the wait (2.5–4.1 ms long, 1.3 ms once) was between 0.73 ms shorter and 0.69 ms longer than without it, 0.13 ms longer on average, and the latency 1.2–4.1 ms lower. Single checks vary by more than the limit of 0.3 ms there, which is why one check alone does not shorten the wait: in a build where it did, one check took the wait from 3.7 to 1.7 ms (latency 29.6 instead of 28.4 ms); in the final build the second check contradicted the first, and the wait stayed. CONTROL Resonant: the wait (10 ms) was kept in all six checks, frame 33.1–33.5 ms with it against 32.9–33.5 ms without (0.14 ms longer on average), latency 22.5–23.1 ms against 32.5–33.2 ms. Resident Evil 4 with `OFLOW_FG` at 3x and the frame cap off: the wait (3 ms) cost 0.7 ms a frame, was shortened, and was dropped when 0.6 ms of it still cost 0.35 ms.
- **How the frames reach the screen:** the display's real refreshes were counted (DRM vblank counter). The count matches the frames shown in every state measured: 94 refreshes per second for 94 shown frames in Resident Evil Requiem with HDR, 104 for 103 in SDR, 108 for 108 in Resident Evil 4 at 3x, 120 for 118 on Forza Horizon 6's car page (where a rendered frame's gap is longer than about 20 ms, as in the release build's runs with an idle GPU, the panel repeats a frame and the count is higher). The images of a rendered frame reach the display one refresh apart (7 ms), and then nothing comes until the next frame: at 3x and 31.9 ms a frame the gaps are 7, 7 and 18 ms.

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
