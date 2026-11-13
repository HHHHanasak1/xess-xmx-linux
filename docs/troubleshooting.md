# Troubleshooting

This guide covers common symptoms and their fixes, why a game might not see your Intel GPU, and how to collect logs for a bug report.

For launch options, see [launch-options.md](launch-options.md). For driver selection and rebuilding a driver after a system update, see [drivers.md](drivers.md). For developer-level debug switches and the XeLL (Xe Low Latency) driver-mode probe, see [dev/debugging.md](dev/debugging.md).

---

## Symptoms and Fixes

| What you see | Cause | What to do |
|---|---|---|
| Looks like the DP4a (8-bit integer dot-product) path, nothing changed; `IGDEXT_TRACE=1` writes no `C:\igdext_trace.log` | The `igdext64.dll` shim is not loaded in the prefix (new prefix, or a Proton update replaced it). | Re-run `./install.sh`, or `./enable.sh` for a per-game setup ([install.md](install.md)). |
| Same as above, but only with a Proton installed by a distribution package (such as CachyOS's `proton-cachyos`) | That Proton is in `/usr/share/steam/compatibilitytools.d`, which only root can write to; Proton copies its stock `igdext64.dll` into the prefix at launch. | Run the `sudo` commands `./install.sh` prints (and repeat after each update of that package), or use a Proton in `~/.local/share/Steam/compatibilitytools.d`. |
| DP4a in one game only, or no XeSS frame generation option; no trace file; the Proton log says `XeSS: hiding Intel GPU Vendor ID` (Cyberpunk 2077) | The game sees no Intel GPU because it ships XeSS older than 2.0.2.68 (where DXVK reports the GPU as an AMD one). | Set `XESS=latest %command%` in the game's launch options ([launch-options.md](launch-options.md)). See [The Game Sees No Intel GPU](#the-game-sees-no-intel-gpu) below. |
| Super resolution runs on DP4a while frame generation uses the shim (Spider-Man Remastered, Hogwarts Legacy, The Witcher 3, Hitman 3, Diablo IV, ...) | Mesa's built-in profile (`drirc`) for that game sets `force_vk_vendor=-1`, so the game sees no Intel GPU. | Re-run `./install.sh` or `./enable.sh` (both set `force_vk_vendor=0`), or set `force_vk_vendor=0 %command%` by hand. To restore Mesa's behavior for one game, set `force_vk_vendor=-1 %command%`. |
| DP4a, and the trace says `fallback: unknown Intel device id` | The GPU's PCI device ID is not in the shim's table (`shim/src/dll/GpuInfo.cpp`). | Report the PCI ID so it can be added, or test with `IGDEXT_GMD=<arch>,<release>` ([dev/debugging.md](dev/debugging.md)). |
| DP4a, and the trace says `fallback: self-test failed` | The patched driver (or Vulkan layer) is not active in the game process (the session check fell back, or environment variables are missing), or vkd3d-proton changed how placeholders reach the driver. | Check `~/.cache/xess-xmx/driver-status` and `VK_DRIVER_FILES` in the game's environment. When working, the patched driver or layer writes `drive_c/igdext_kernels/anv_canary` when it sees the self-test. |
| Noise or black patches in the XeSS output | A C-for-Metal (CM) kernel failed to compile during this launch and ran as a no-op placeholder. | Check `~/.cache/xess-xmx/compile.log`. If the compiler is missing, run `tools/get-igc.sh`. With the patched driver, the next launch compiles the failed kernels once more; if that fails, XeSS uses its generic paths. The Vulkan layer does not retry (see the next row). |
| DP4a after a failed compile | Intended: `drive_c/igdext_kernels/compile_failed.txt` exists in the prefix, so the shim falls back to prevent corrupted output. | The patched driver retries once at the next launch. After a second failure, only a graphics component update (`igc/`, the helper, the patched driver, or the system's Mesa), a game update (`libxess.dll` / `libxess_fg.dll`), deleting `compile_failed.txt`, or running `./install.sh` starts another attempt. (In stock mode on Xe3, the Vulkan layer does not auto-retry `compile_failed.txt` at device creation; re-run `./install.sh` or delete the marker after fixing the compiler.) |
| First launch hangs for a minute or two at XeSS initialization | Every CM kernel is being compiled once into `~/.cache/xess-xmx/kernels`. | Wait for compilation to finish. Subsequent launches load the cached kernels immediately. While kernels are being compiled (on first launch or a cache miss), the patched driver prints `ANV CM: compiling kernel` lines to the game's stderr, and the Vulkan layer logs compiles to `~/.cache/xess-xmx/compile.log`. |
| Lower frame rate at the first launch after installing or updating the package or Mesa | The kernels and the game's shaders were compiled for the new driver during this launch, which can affect performance for the rest of the session. | After compiling has finished, restart the game once. |
| XMX is gone after a SteamOS or system update, and `driver-status` says `FAILED` | In a patched session, the session check switches to the other patched driver of the package (`lib/mesa-26.1.2` or `lib/mesa-26.3.0`) if the active one breaks; if neither loads, the session falls back to the system's stock drivers. In a stock session, if the unpatched driver fails, the session falls back to the system's stock drivers. | Run `tools/rebuild-driver.sh` ([drivers.md](drivers.md) and [dev/building.md](dev/building.md)); on Xe3, switch to it with `XMX_DRIVER=patched ./install.sh` and log in again. Check `journalctl --user -u xess-xmx-check`. |
| Games find no Intel GPU (or `driver-status` says `FAILED`) with `cannot open shared object file`. This error names `libSPIRV-Tools.so` or `libdisplay-info.so.3`. The Proton log has `xess-xmx: cannot load the Mesa ... driver` | The driver needs both libraries, and the Steam Linux Runtime (`pressure-vessel`) container that runs Proton games has neither: they reach it only as dependencies of the host's other Vulkan drivers. Some distributions also name SPIRV-Tools differently (`libSPIRV-Tools-shared.so` on Void Linux, `libSPIRV-Tools-<version>.so` on openSUSE). | The session check copies both into `lib/deps/` (the driver's `RUNPATH`); a system without them gets the copies bundled in `lib/bundled/`. Re-run `./install.sh` (`./enable.sh` for one game) or log out and in again. Verify that `ls lib/deps/` lists both files. |
| `undefined symbol: wl_fixes_interface` in `driver-status` or the Proton log | The system's `libwayland-client` is older than 1.24 (Debian 13, Ubuntu 24.04), while the drivers in the archive are built against 1.24. | Build the driver on your system with `tools/rebuild-driver.sh` ([drivers.md](drivers.md) and [dev/building.md](dev/building.md)); on Xe3, switch to it with `XMX_DRIVER=patched ./install.sh` and log in again. |
| A game misbehaves on Mesa 26.3 only | A regression in Mesa 26.3 or its Jay shader compiler. | Set `MESA=26.1 %command%` in its launch options (or test `INTEL_DEBUG=no-jay %command%` to see if Jay is the cause), and report the issue. |
| Generated frames are black or flicker between black and image | XeSS frame generation (`libxess_fg.dll`) was given the Xe-HPG kernel variant on Xe2/Xe3 because `IGDEXT_OPTIONS2_FG=0,...` was set. | Remove the `IGDEXT_OPTIONS2_FG` override. |
| The game offers only 2x frame generation | The game's menu hardcodes a 2x toggle even though XeSS-FG supports higher multipliers. | Set the multiplier with `XESS_FG=`, for example `XESS_FG=3 %command%` in the game's launch options ([frame-generation.md](frame-generation.md) and [launch-options.md](launch-options.md)). |
| `XESS_FG` changes nothing | Frame generation is switched off in the game, or the game's XeSS frame generation is older than 1.2. | Switch frame generation on in the game's menu, and add `XESS=latest` if the game ships XeSS-FG older than 1.2. |
| `FSR=xess FSR_FG=inject`, but no frame generation | FSR is not the upscaler chosen in the game, no game `.exe` sits next to the upscaler DLL, or the game folder has a `dxgi.dll` of its own. | Choose FSR upscaling in the game. Check the game's stderr (`PROTON_LOG=1`), `dxgi_xefg.log` next to `dxgi.dll`, and `ffx_xefg.log` ([frame-generation.md](frame-generation.md)). |
| A Vulkan driver installed after `./install.sh` (an eGPU's driver, `lavapipe`) is not found | `VK_DRIVER_FILES` lists the drivers that existed when `./install.sh` last ran. | Re-run `./install.sh`, then log out and back in. |
| 30 fps with frame generation until the pause menu is opened and closed (Wuthering Waves) | The game passes a 33.3 ms frame cap to XeLL right after enabling frame generation; the shim fixes this automatically for Wuthering Waves. | For another game with the same symptom, set `IGDEXT_XELL_FIXCAP=1 %command%`. With `IGDEXT_XELL_LOG=1`, `C:\igdext_xell.log` shows `frame cap ... keeping` lines. |
| Lower frame rate with frame generation than before release 20261104, or uneven pacing | The low-latency mode (on by default while frame generation runs) waits too long before starting each frame in this game. | Set `IGDEXT_XELL_CAP=off %command%` to compare with low latency off, or `IGDEXT_XELL_CAP=auto:2 %command%` to start each frame 2 ms earlier ([frame-generation.md](frame-generation.md) and [launch-options.md](launch-options.md)). With `IGDEXT_XELL_LOG=1`, check the `low latency: waited ...` lines in `C:\igdext_xell.log`. |
| After updating from an older release: no image or a GPU hang with ray tracing; another GPU or 32-bit games lack Vulkan; XMX gone in the games of one Proton after it updated | Bugs in older releases that have since been fixed. | Unpack the current release, run `./install.sh`, and log out and back in. |

---

## The Game Sees No Intel GPU

Two separate mechanisms in the Linux graphics stack hide an Intel GPU's vendor ID (`0x8086`) from certain games, causing XeSS to fall back to DP4a (and, for old XeSS, hiding the frame generation option):

1. **Games shipping XeSS older than 2.0.2.68 (such as Cyberpunk 2077):**
   DXVK's DXGI implementation reports an Intel GPU as an AMD GPU (`0x1002`) whenever the loaded `libxess.dll` (or `libxess_dx11.dll`) is older than 2.0.2.68 (`isXessVendorWaNeeded()` in DXVK's `dxgi_options.cpp`). When this happens, the Proton log (`PROTON_LOG=1`) prints `XeSS: hiding Intel GPU Vendor ID`, the shim is never loaded, and `C:\igdext_trace.log` is not created.
   * **Fix:** Put `XESS=latest %command%` in the game's Steam launch options ([launch-options.md](launch-options.md) and [games.md](games.md)). This links Intel's newer XeSS SDK 3.0.2 libraries (`2.0.2.68` / XeSS-FG `1.3.1.78` / XeLL `1.3.2.10`) into the game folder while it runs.
   * Do not force DXVK to report the Intel GPU to the old libraries (`DXVK_CONFIG="dxgi.customVendorId = 8086"`): Cyberpunk 2077's shipped XeSS-FG 1.1.0.19 crashes at startup when it sees an Intel GPU.

2. **Mesa `drirc` game profiles (`force_vk_vendor=-1`):**
   Mesa's built-in per-game profiles for several titles (including Spider-Man Remastered, Hogwarts Legacy, The Witcher 3, Hitman 3, and Diablo IV) set `force_vk_vendor=-1`, which hides the Intel Vulkan vendor ID from the game.
   * **Fix:** `./install.sh` and `./enable.sh` set `force_vk_vendor=0` in the environment to override these profiles. If you need to restore Mesa's profile for a specific game that requires the workaround, set it again in that game's launch options (XeSS will then run on DP4a in that game):

     ```text
     force_vk_vendor=-1 %command%
     ```

---

## Logs for a Bug Report

To collect logs for a single game run, add these options before `%command%` in the game's Steam launch options:

```text
XESS_LOG=1 PROTON_LOG=1 ANV_CM_DEBUG=1 XMX_LAYER_LOG=1 %command%
```

| Launch option | What it writes |
|---|---|
| `XESS_LOG=1 %command%` | Sets `IGDEXT_TRACE=1 IGDEXT_XELL_LOG=1 IGDEXT_XEFG_LOG=1 XESS_FG_LOG=1` (via the Proton hook installed by `install.sh`). Inside the game's prefix (`drive_c/`), this writes:<br>• `C:\igdext_trace.log`: the detected GPU, every extension call, self-test and fallback decisions, each kernel hash, and the calling module chain (if this file does not exist at all, the shim was not loaded). Also dumps raw kernels to `C:\igdext_dump\`.<br>• `C:\igdext_xell.log`: what the game passes to XeLL, the real frame rate from `xellSleep` timing, low-latency pacing status, and the average simulation-start to present-end latency every 100 frames.<br>• `C:\igdext_xefg.log`: XeSS frame generation's own messages, the maximum and active number of generated frames after context creation and initialization, and `XESS_FG` changes.<br>• `C:\igcl_stub.log`: every call `ControlLib.dll` answers and the `XESS_FG` override value it returned. |
| `PROTON_LOG=1 %command%` | Writes `~/steam-<appid>.log`, containing Proton's log and the game's stderr (`xess-xmx:` lines from the Proton hook and forwarding ICD, plus `ANV CM:` lines from the patched driver). Slows the game down; enable it only when collecting logs for a report. |
| `ANV_CM_DEBUG=1 %command%` | Logs each placeholder and kernel injection in the patched driver to the game's stderr (combine with `PROTON_LOG=1`). When using the Vulkan layer (`stock` mode on Xe3 or `xmx-stock.sh`), use `XMX_LAYER_LOG=1 %command%` as well so the layer logs to stderr. |

### Where to Find the Files

* **Game prefix (`C:\` drive):** `~/.local/share/Steam/steamapps/compatdata/<appid>/pfx/drive_c/` (if the game is installed on an SD card or secondary drive, check the `steamapps/compatdata/<appid>/pfx/drive_c/` folder of that Steam library).
* **Proton log:** `~/steam-<appid>.log` in your home directory.
* **Kernel compiler log:** `~/.cache/xess-xmx/compile.log` (override with `CM_LOG`).
* **Session driver check status:** `~/.cache/xess-xmx/driver-status` and `journalctl --user -u xess-xmx-check`.
* **FSR and optical-flow logs (when using `FSR=xess`, `FSR_FG=xess`, `FSR_FG=inject`, or `OFLOW_FG=1`):** These logs are written directly in the game's folder next to the DLLs and hold the last run. They are `ffx_xess.log`, `ffx_xefg.log`, and `dxgi_xefg.log` ([frame-generation.md](frame-generation.md)).
* **D3D11 and Vulkan bridge logs (when using `XESS=latest`):** Set `XESS_DX11_TRACE=1` to log `[xess-dx11]` lines to stderr / `PROTON_LOG=1`. Or set `VKWRAP_LOG=1` to write `vkwrap.log` next to `vulkan-1.dll` in the game folder.
