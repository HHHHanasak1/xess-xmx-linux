# Troubleshooting

**Terms**

- **DP4a** is the generic shader path that XeSS falls back to whenever the XMX path is not available (when the shim is not loaded, the self-test fails, or a kernel compile fails), even on GPUs that have XMX hardware.
- A **prefix** (`WINEPREFIX`) is the simulated Windows directory that Proton creates for each game.
- **The shim** (`igdext64.dll`) is the DLL that exposes Intel's driver extensions to the game inside the prefix.
- **XeLL** (`libxell.dll`) is Intel's latency-reduction library, used alongside frame generation.

## Symptoms and fixes

| What you see | Cause | What to do |
|---|---|---|
| Looks like DP4a, nothing changed; `IGDEXT_TRACE=1` writes no `C:\igdext_trace.log` | The shim is not loaded in the prefix (new prefix, or Proton replaced it). | Re-run `./install.sh`. |
| Same as above, only with a Proton from a distribution package (CachyOS's `proton-cachyos`) | That Proton is in `/usr/share/steam/compatibilitytools.d`, which only root can write to; Proton copies its stock `igdext64.dll` into the prefix. | Run the `sudo` commands `install.sh` prints (and again after each update of that package), or use a Proton in `~/.local/share/Steam/compatibilitytools.d`. |
| DP4a in one game only, or no XeSS frame generation option; no trace file; the Proton log says `XeSS: hiding Intel GPU Vendor ID` (Cyberpunk 2077) | The game ships XeSS older than 2.0.2.68, so DXVK reports the GPU as an AMD one ([The game sees no Intel GPU](#the-game-sees-no-intel-gpu)). | Set `XESS=latest %command%` in the launch options to use Intel's newer libraries while the game runs ([launch-options.md](launch-options.md)). Making DXVK report the Intel GPU to the old libraries instead (`DXVK_CONFIG="dxgi.customVendorId = 8086"`) crashes Cyberpunk 2077's XeSS-FG 1.1.0.19. |
| Super resolution runs on DP4a while frame generation uses the shim (Spider-Man Remastered, Hogwarts Legacy, The Witcher 3, Hitman 3, Diablo IV, ...) | Mesa's profile for that game sets `force_vk_vendor=-1`, so the game sees no Intel GPU. | Re-run `./install.sh` or `./enable.sh` (they set `force_vk_vendor=0`), or set `force_vk_vendor=0 %command%` in the launch options. To get Mesa's behaviour back for one game, see [The game sees no Intel GPU](#the-game-sees-no-intel-gpu). |
| DP4a, and the trace says `fallback: unknown Intel device id` | The GPU is not in the shim's table (`GpuInfo.cpp`). | Add it to the table, or set `IGDEXT_GMD=<arch>,<release>` for a test. |
| DP4a, and the trace says `fallback: self-test failed` | The patched driver is not active in the game process (the session check fell back, or variables are missing), or vkd3d-proton changed how placeholders reach the driver. | Check `~/.cache/xess-xmx/driver-status` and `VK_DRIVER_FILES` in the game's environment. The patched driver writes `drive_c/igdext_kernels/anv_canary` when it sees the test. |
| Noise or black patches in the XeSS output | A kernel failed to compile in this launch and ran as a no-op. | Check `~/.cache/xess-xmx/compile.log`. If the compiler is missing, run `tools/get-igc.sh`. The next launch retries once and then uses XeSS's generic paths. |
| DP4a after a failed compile | Intended: `drive_c/igdext_kernels/compile_failed.txt` exists. | The driver retries once at the next launch. After a second failure, only a graphics component update, a game update, deleting the marker, or running `./install.sh` starts another attempt. |
| XMX is gone after a SteamOS or system update, and `~/.cache/xess-xmx/driver-status` says `FAILED` | Neither of the package's drivers loads any more (if only the active one broke, the session check switched to the other); the session uses the stock drivers. | Run `tools/rebuild-driver.sh` ([drivers.md](drivers.md) / [dev/building.md](dev/building.md)). Check `journalctl --user -u xess-xmx-check`. |
| Games find no Intel GPU (or `driver-status` says `FAILED`) with `cannot open shared object file` for `libSPIRV-Tools.so` or `libdisplay-info.so.3`; the Proton log has `xess-xmx: cannot load the Mesa ... driver` | The driver needs both libraries, and the Steam Linux Runtime container that runs Proton games has neither: they reach it only as dependencies of the host's other Vulkan drivers. Some distributions name SPIRV-Tools differently (`libSPIRV-Tools-shared.so` on Void Linux, `libSPIRV-Tools-<version>.so` on openSUSE). | Re-run `./install.sh` (`./enable.sh` for one game) or log out and back in so the session check copies both into `lib/deps/` (the driver's run path). `ls lib/deps/` must list both; a system without them gets the copies the archive bundles in `lib/bundled/`. |
| `undefined symbol: wl_fixes_interface` in `driver-status` or the Proton log | The system's `libwayland-client` is older than 1.24 (Debian 13, Ubuntu 24.04); the drivers in the archive are built against 1.24. | Build the driver on the system with `tools/rebuild-driver.sh` ([drivers.md](drivers.md) / [dev/building.md](dev/building.md)). |
| A game misbehaves on Mesa 26.3 only | A regression in Mesa 26.3 or its Jay shader compiler. | Set `MESA=26.1 %command%` in the game's launch options ([launch-options.md](launch-options.md)), and report it in the issue tracker. |
| First launch hangs for a minute or two at XeSS initialisation | Kernels are being compiled once (`ANV CM: compiling kernel` lines in the game's stderr). | Wait for compilation to finish; later launches load the kernels from `~/.cache/xess-xmx`. |
| The game offers only 2x frame generation | The game's menu does not expose higher multipliers. | Set `XESS_FG=4 %command%` in the launch options ([launch-options.md](launch-options.md)). |
| `XESS_FG` changes nothing | Frame generation is off in the game, or the game's XeSS frame generation library is older than 1.2. | Switch frame generation on in the game's menu, and add `XESS=latest` if the game's XeSS-FG is older than 1.2 ([launch-options.md](launch-options.md)). |
| Generated frames are black or flicker between black and image | XeFG got the Xe-HPG kernel variant (only happens with `IGDEXT_OPTIONS2_FG=0,...`). | Remove the `IGDEXT_OPTIONS2_FG` override. |
| `FSR=xess FSR_FG=inject` is set, but there is no frame generation | FSR is not the upscaler chosen in the game, or the game's stderr says why it was skipped: no game executable next to the upscaler DLL, or the game folder has a `dxgi.dll` of its own. | Choose FSR upscaling in the game. Check `dxgi_xefg.log` next to `dxgi.dll` and `ffx_xefg.log` for the swap chain status ([frame-generation.md](frame-generation.md)). |
| A Vulkan driver installed after `install.sh` (an eGPU's driver, lavapipe) is not found | `VK_DRIVER_FILES` lists the drivers that existed when `install.sh` last ran. | Re-run `./install.sh` and log out and back in. |
| 30 fps with frame generation until the pause menu is opened and closed (Wuthering Waves) | The game's XeLL frame cap; the shim fixes it automatically in Wuthering Waves. | Set `IGDEXT_XELL_LOG=1` to see `frame cap ... keeping` lines; for another game with the same symptom, set `IGDEXT_XELL_FIXCAP=1 %command%` ([launch-options.md](launch-options.md)). |
| Lower frame rate with frame generation than before release 20261104, or uneven pacing | The low latency mode (on by default with frame generation) waits too long in this game. | Set `IGDEXT_XELL_CAP=off %command%` to compare, or `IGDEXT_XELL_CAP=auto:2 %command%` to start each frame 2 ms earlier. `IGDEXT_XELL_LOG=1` logs `auto: waited ...` lines ([launch-options.md](launch-options.md)). |
| After updating from an older release: no image or a GPU hang with ray tracing; another GPU or 32-bit games lack Vulkan; XMX is gone in the games of one Proton after it updated | Bugs of older releases, fixed since. | Install the current release, run `./install.sh`, and log out and back in. |

## The game sees no Intel GPU

Two mechanisms outside this package hide the Intel GPU from certain games, which pushes XeSS onto its DP4a fallback:

1. **XeSS older than 2.0.2.68:** DXVK reports an Intel GPU as an AMD GPU when the `libxess.dll` loaded by the game is older than 2.0.2.68. XeSS then runs on DP4a and the game may hide its XeSS frame generation option. Set `XESS=latest %command%` in the game's launch options to run with the newer Intel XeSS SDK libraries ([games.md](games.md), [launch-options.md](launch-options.md)).
2. **Mesa game profiles:** Mesa sets `force_vk_vendor=-1` in its built-in profiles for several games (Spider-Man Remastered, Hogwarts Legacy, The Witcher 3, Hitman 3, Diablo IV, and others), hiding the Intel vendor ID. `install.sh` and `enable.sh` override this globally by setting `force_vk_vendor=0`.

If a game needs Mesa's original workaround to start or render properly, restore it for that game in its Steam launch options (XeSS will then run on DP4a):

| Launch option | What it does | When |
|---|---|---|
| `force_vk_vendor=-1 %command%` | Restores Mesa's profile for this game so the Intel vendor ID is hidden again | A game needs Mesa's workaround; overrides the `force_vk_vendor=0` set by `install.sh` |

## Logs for a bug report

Put these options before `%command%` in the game's Steam launch options to record logs for an issue report:

```sh
XESS_LOG=1 PROTON_LOG=1 ANV_CM_DEBUG=1 %command%
```

| Launch option | What it writes |
|---|---|
| `XESS_LOG=1 %command%` | Shorthand for `IGDEXT_TRACE=1 IGDEXT_XELL_LOG=1 IGDEXT_XEFG_LOG=1 XESS_FG_LOG=1`. Writes in the game's prefix (`drive_c/`):<br>• `C:\igdext_trace.log`: the detected GPU, the feature answers, every extension call, and each kernel with its ID and calling module chain `callers(...)` (telling `libxess.dll` and `libxess_fg.dll` apart); also dumps every kernel to `C:\igdext_dump`. No file at all means the shim was not loaded.<br>• `C:\igdext_xell.log`: what the game passes to XeLL, the real frame rate from `xellSleep` timing, the latency, and XeLL's own messages.<br>• `C:\igdext_xefg.log`: XeSS frame generation's own messages, the maximum number of generated frames, and the number it starts with.<br>• `C:\igcl_stub.log`: the `XESS_FG` override answered by `ControlLib.dll`. |
| `PROTON_LOG=1 %command%` | Writes `~/steam-<appid>.log`: Proton's log, including the game's stderr (`xess-xmx:` and `ANV CM:` lines). This makes the game slow; use it only for a report. |
| `ANV_CM_DEBUG=1 %command%` | Logs each placeholder and kernel the driver injects to the game's stderr (combine with `PROTON_LOG=1`, or add `exec 2>>/some/file` to the launch wrapper's config). |

The game's prefix is at `~/.local/share/Steam/steamapps/compatdata/<appid>/pfx/drive_c/` (for a game on another drive, look under the `steamapps` folder of that Steam library).

### Logs outside the game prefix

- **Kernel compiler:** `~/.cache/xess-xmx/compile.log` (override the path with `CM_LOG`).
- **Session driver check:** `~/.cache/xess-xmx/driver-status` and `journalctl --user -u xess-xmx-check`.
- **FSR on XeSS / optical flow DLLs:** `ffx_xess.log`, `ffx_xefg.log`, and `dxgi_xefg.log` in the game folder next to the DLLs ([frame-generation.md](frame-generation.md)).
- **D3D11 and Vulkan XeSS bridges:** `XESS_DX11_TRACE=1` and `VKWRAP_LOG=1` ([games.md](games.md)).
- **Vulkan layer:** `XMX_LAYER_LOG=1` (stderr) or `XMX_LAYER_LOG=<file>` ([../layer/README.md](../layer/README.md)).

For developer debug switches (`ANV_CM_TRACE`, buffer captures, kernel overrides, and the XeLL driver-mode probe), see [dev/debugging.md](dev/debugging.md). For all launch options, see [launch-options.md](launch-options.md).
