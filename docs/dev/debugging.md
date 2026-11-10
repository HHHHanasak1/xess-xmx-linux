# Developer debugging

This document lists the logs, probes, and environment variables used to debug the shim, the drivers, the layer, the FSR/XeSS wrappers, and the scripts.

For common symptoms and user fixes, see [../troubleshooting.md](../troubleshooting.md). For user-facing Steam launch options, see [../launch-options.md](../launch-options.md).

## Logs

* **Shim (`igdext64.dll`):** Set `IGDEXT_TRACE=1`. It writes `C:\igdext_trace.log` inside the game's Wine prefix (the simulated Windows directory tree, usually `~/.local/share/Steam/steamapps/compatdata/<appid>/pfx/drive_c/`, or in the `steamapps` folder on another drive). The log records every extension call, the detected GPU, the feature answers, each C-for-Metal (CM) kernel with its id, and a `callers(...)` module chain so you can tell super resolution (`libxess.dll`) and frame generation (`libxess_fg.dll`) kernels apart. It also dumps every kernel to `C:\igdext_dump`.
* **XeLL (Intel Xe Low Latency):** Set `IGDEXT_XELL_LOG=1`. It writes `C:\igdext_xell.log` with the arguments the game passes to XeLL, the real frame rate from `xellSleep` timing, the average simulation-start to present-end latency every 100 frames, and XeLL's own messages.
* **XeSS frame generation:** Set `IGDEXT_XEFG_LOG=1` to write `C:\igdext_xefg.log` (XeSS-FG's own log via `xefgSwapChainSetLoggingCallback`, the maximum number of generated frames after context creation and initialization, and every change to that number). Set `XESS_FG_LOG=1` to write `C:\igcl_stub.log` with every call `ControlLib.dll` answers.
* **Patched driver (`libvulkan_intel.so`):** Set `ANV_CM_DEBUG=1` to log each placeholder and kernel injection to the game's standard error (`stderr`). Set `ANV_CM_TRACE=1` to log every dispatch (`ANV CM: walker kernel x,y,z ...`), or `ANV_CM_TRACE=2` to also log the surface states. To capture the game's `stderr`, put `exec 2>>/some/file` into the launch wrapper's config, or use `PROTON_LOG=1 %command%` (which writes `~/steam-<appid>.log`).
* **Vulkan layer (`VK_LAYER_XESS_xmx`):** Set `XMX_LAYER_LOG=1` to log to `stderr`, or `XMX_LAYER_LOG=<path>` to append to a file.
* **D3D11 bridge and Vulkan wrapper:** `XESS_DX11_TRACE=1` logs each D3D11 bridge call. `VKWRAP_LOG=1` writes `vkwrap.log` next to `vulkan-1.dll`.
* **FSR and optical flow DLLs (`ffxfg/`):** Placed in the game's folder next to the DLLs; they keep the last run. `ffx_xess.log` logs the XeSS version, context, quality setting, flags, the first dispatches, and every 600th dispatch. `ffx_xefg.log` logs the upscalers the game's FidelityFX loader offers on the GPU, the swap chain, the maximum multiplier XeSS-FG reports and the number set, the first frames, every 600th present (`presented 4` = 4x, with sync interval and flags),XeSS-FG's warnings, `cap auto:` interval changes, and `oflow: GPU time`. With `FSR_FG=inject`, `dxgi_xefg.log` next to `dxgi.dll` logs what became of each swap chain.
* **Compiler (`tools/cm-compile.sh`):** Writes to `~/.cache/xess-xmx/compile.log` (`CM_LOG` overrides the path).
* **Session check:** Writes to `~/.cache/xess-xmx/driver-status` and the user systemd journal (`journalctl --user -u xess-xmx-check`).

To count dispatches per kernel from an `ANV_CM_TRACE=1` log and see what runs every frame:

```bash
grep -a -o "walker kernel [0-9]*,[0-9]*,[0-9]*" stderr.log | sort | uniq -c
```

## XeLL driver-mode probe

XeLL runs in cross-vendor mode under Proton (see [how-it-works.md](how-it-works.md)). The reason is in `libxell.dll` itself (read from 1.3.2.10, the SDK copy; run `strings libxell.dll | grep wine_get_version` to check if the 1.3.0.5 copy that games ship has the same check). Before doing anything else, `libxell.dll` checks `GetProcAddress(GetModuleHandle("ntdll.dll"), "wine_get_version")`. If that export exists, it uses its built-in context without ever loading the extension library.

If that export is hidden, `libxell.dll` loads `igdext64.dll`, asks for extension version 5/13 or 4/13, checks the driver version string (`>= 31.0.101.6252`) and `GTGeneration`. `GTGeneration` must be one of Intel's `PRODUCT_FAMILY` IDs (1300 for Panther Lake, 1275 for Lunar Lake, 1274 for Battlemage, 1270 for Alchemist, and so on; the shim normally reports 30 there). Only then does it call the latency-reduction functions. Details and offsets are in `shim/src/dll/XellDriver.cpp`.

`IGDEXT_XELL_PROBE=<version>` turns all of this on for one launch: the shim hooks the import table in `libxell.dll` to hide `wine_get_version`, reports the product family ID, claims the requested interface version, accepts the pacing calls, and logs what XeLL does with the timing buffers.

To run the probe, put this in the game's Steam launch options:

```text
/path/to/package/tools/xell-probe.sh %command%
```

Play for a minute with frame generation enabled (the Wuthering Waves title screen with FG on is enough), then quit. The wrapper prints a summary and writes `~/xess-xmx-probe-<date>.tar.gz` containing:

* `proton-xell.log`: XeLL's own messages sent through `OutputDebugString` (the wrapper sets `PROTON_LOG=1` and keeps the XeLL lines from `~/steam-<appid>.log`).
* `libxell.log`: written by `libxell` 1.3.2+.
* `igdext_trace.log`: `XELLPROBE` lines showing the hook, every latency-reduction call with its arguments, each `RenderSubmitStart` with the time since the previous one, and every read or write XeLL makes to the timing buffers (slot, in-slot offset, and code address in `libxell.dll`).
* `igdext_xell.log` and the session's `driver-status`.

Attach the archive to the issue.

### Reading the probe result

* **No `XELLPROBE libxell.dll ...: GetProcAddress import hooked` line:** The shim never saw `libxell.dll` load. Either the shim is not loaded (run `install.sh`), or the game created its XeLL context before the shim loaded. Toggle frame generation off and on in the game's menu to recreate the context, then check again.
* **`[XeLL] Using built-in context` and no `Intel Extensions` lines when the hook is in place:** XeLL still did not take the Intel path. Check the DXGI vendor ID the game sees (`force_vk_vendor=0`, see [../games.md](../games.md)) and check whether `XELL_FORCE_CROSS_VENDOR` is set.
* **`XeLL context creation failed - unsupported device` or `unsupported driver`:** The family ID or driver version string reported by the shim was rejected. Override the family ID with `IGDEXT_FAMILY=<id>`.
* **`Intel Extensions initialized.` and `Latency reduction enabled.` followed by `XELLPROBE LatencyReductionExt(...)` and buffer accesses:** Driver mode started. The access log gives the slot size and fields. A read of `gpu +0x0` followed by a crash means XeLL treats the GPU buffer as a COM object (an `ID3D12Resource` it maps) rather than plain memory, which the next shim version must then provide.
* **`GetDisplayTelemetry` called and then cross-vendor:** Retry with `XELL_PROBE_TELEMETRY=1` (or `IGDEXT_XELL_TELEMETRY=1`).

## Environment variables and switches

Variables set in a game's launch options override the shorthands (`XESS=...`, `FSR=...`) and the session defaults.

### Session environment

Set by `install.sh` in `~/.config/environment.d/50-xess-xmx.conf` or by `enable.sh` in `~/.config/xess-xmx.conf` (read by `xmx-launch.sh`):

| Variable | Effect |
|---|---|
| `VK_DRIVER_FILES` | Points to `run/intel_icd.json` as the 64-bit Intel Vulkan driver while keeping every other driver on the system in the list. |
| `VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer,VK_KHR_present_wait2` | Keeps vkd3d-proton on its classic descriptor heap (needed by the kernel binding code) and uses present wait 1 (with present wait 2, Cyberpunk 2077 with frame generation crashed within minutes in `vkWaitForPresent2KHR`). |
| `force_vk_vendor=0` | Overrides Mesa's `force_vk_vendor=-1` game profiles so games see the Intel GPU. Put `force_vk_vendor=-1 %command%` in a game's launch options to restore Mesa's profile for that game. |
| `ANV_CM_KERNEL_DIR`, `ANV_CM_HELPER` | The driver's kernel folder and compiler helper (see Driver table below). |
| `INTC_ALT_DRIVER_EXTENSIONS_PATH` | Points to `C:\windows\system32\driverstore\filerepository\igd_faux.inf_1`. Proton 11.0 has no driver store folder of its own and needs this variable to load the shim. Proton Experimental has a driver store; GE-Proton sets its own value for every game. |

### Launch-option shorthands (`tools/proton-hook.py`)

Run by Proton via the `user_settings.py` installed by `install.sh`:

| Variable | Effect |
|---|---|
| `XESS=latest` | Replaces older `libxess.dll`, `libxess_fg.dll`, and `libxell.dll` in the game folder with Intel's XeSS SDK libraries (`xess/`), replaces `libxess_dx11.dll` with `dx11bridge/libxess_dx11.dll`, and adds `vkwrap/vulkan-1.dll` for Vulkan games with XeSS. Restores originals on exit. |
| `XESS_VK=1` / `XESS_VK=0` | Used with `XESS=latest`: forces the `vulkan-1.dll` wrapper on (for games that load Vulkan at run time without importing it) or off. |
| `XESS=dp4a` | Sets `IGDEXT_FORCE_FALLBACK=1`. |
| `XESS_LOG=1` | Sets `IGDEXT_TRACE=1 IGDEXT_XELL_LOG=1 IGDEXT_XEFG_LOG=1 XESS_FG_LOG=1`. |
| `FSR=xess` | Replaces the game's FSR 3.1+ DLLs with the package's (`ffxfg/`) and places the SDK's XeSS libraries next to them while the game runs. |
| `FSR_FG=xess` | Same as `FSR=xess`, for the frame generation DLL only. |
| `FSR_FG=inject` | Used with `FSR=xess`: also places `dxgi.dll` and `ffx_xefg_bridge.dll` next to the upscaler for games without FSR frame generation. |
| `OFLOW_FG=1` | Experimental: places `dxgi.dll` and the optical flow frame generation DLL next to a DX12 game's executable. Ignored when `FSR` or `FSR_FG` is set. |
| `FSR_VENDOR=amd` | Used with `FSR` or `FSR_FG`: sets `dxgi.customVendorId = 1002` in `DXVK_CONFIG`. |

### Driver selection and Mesa options

| Variable | Effect |
|---|---|
| `MESA=<version>` | Selects the package driver for this game (`26.1`, `26.3`, `stock-26.3`; a prefix is enough). Logs `xess-xmx: using the Mesa ... driver` to `stderr`, or `no such driver` and falls back to the default. |
| `INTEL_DEBUG=no-jay` | On Mesa 26.3, compiles the game's own shaders with the old `brw` compiler instead of Jay. |
| `XMX_STOCK_ICD=<json>` | Used with `xmx-stock.sh`: uses the given Intel ICD JSON file instead of searching the system paths. |

### Shim (`igdext64.dll` and `ControlLib.dll`)

| Variable | Effect |
|---|---|
| `IGDEXT_TRACE=1` | Writes `C:\igdext_trace.log` and dumps kernels to `C:\igdext_dump`. |
| `IGDEXT_IGNORE_FAILED=1` | Does not fall back to DP4a when `drive_c/igdext_kernels/compile_failed.txt` exists. |
| `IGDEXT_SKIP_SELFTEST=1` | Skips the `anv_canary` self-test and assumes the patched driver or layer is active. |
| `IGDEXT_FORCE_FALLBACK=1` | Declines the extension context so XeSS uses DP4a and XeSS-FG uses its generic 2x path. The shim also does this automatically for unknown Intel device IDs (`fallback: unknown Intel device id`). |
| `IGDEXT_OPTIONS1=<xmx>,<dlboost>,<emul64>` | Overrides `CheckFeatureSupport(OPTIONS1)`. |
| `IGDEXT_OPTIONS2=<simd16>,<lsc>,<legacy>` | Overrides `CheckFeatureSupport(OPTIONS2)`. Default from the detected GPU: `1,1,0` on Xe2/Xe3, `0,1,0` on Xe-HPG. |
| `IGDEXT_OPTIONS2_FG=...` | Same as `IGDEXT_OPTIONS2`, but only for calls coming from `libxess_fg.dll`. Do not set `0,...` on Xe2/Xe3: giving XeFG the Xe-HPG kernel variant makes generated frames black or flicker. |
| `IGDEXT_GMD=<arch>,<rel>` | Overrides the reported GMD architecture and release (also enables unknown Intel device IDs for testing). |
| `IGDEXT_GTGEN`, `IGDEXT_GTNAME`, `IGDEXT_EUS=<eus>,<cores>` | Overrides the reported GT generation, name, EU count, and core count. |
| `IGDEXT_XELL_LOG=1` | Writes `C:\igdext_xell.log`. |
| `IGDEXT_XELL_FIXCAP=1` / `IGDEXT_XELL_KEEPCAP=1` | Applies the Wuthering Waves 33.3 ms frame-cap fix in any game, or disables it everywhere (default: Wuthering Waves only). |
| `IGDEXT_XELL_CAP=off` / `auto:<ms>` / `<us>` | Configures low latency with frame generation: `off` disables it; `auto:<ms>` starts each frame `<ms>` earlier (or later if negative); `<us>` sets a fixed XeLL frame cap in microseconds (up to `1000000`). With `IGDEXT_XELL_LOG=1`, logs `low latency: ...`, `auto: frame generation on/off (source)`, `auto: frame time ...`, `auto: queue ...`, and `auto: waited in N of 100 frames`. |
| `XESS_FG=<n>` | Overrides the frame generation multiplier (`2` to `4` via `ControlLib.dll`; `5` to `8` raised by the shim, experimental). |
| `IGDEXT_XEFG_LOG=1` | Writes `C:\igdext_xefg.log`. |
| `XESS_FG_LOG=1` | Writes `C:\igcl_stub.log`. |
| `IGDEXT_XELL_PROBE=<version>` | Enables the XeLL driver-mode probe for the given extension interface version (`tools/xell-probe.sh`). |
| `IGDEXT_FAMILY=<id>` | Overrides the Intel `PRODUCT_FAMILY` ID reported during the XeLL probe. |
| `IGDEXT_XELL_TELEMETRY=1` / `XELL_PROBE_TELEMETRY=1` | During the XeLL probe, answers `GetDisplayTelemetry` with a zeroed buffer instead of `E_NOTIMPL`. |

### Patched driver (`patches/0001` and `patches/0002`)

`0001` variables:

| Variable | Effect |
|---|---|
| `ANV_CM_KERNEL_DIR` | Sets the base path for the default helper below (set by `install.sh`; the folder itself may be empty). |
| `ANV_CM_HELPER` | Path to the compiler helper script (default: `$ANV_CM_KERNEL_DIR/../tools/cm-compile.sh`). |
| `ANV_CM_SPV_DIR` | Folder where the shim writes `.spv` and `.opt` files (default: `$WINEPREFIX/drive_c/igdext_kernels`). |
| `ANV_CM_DEBUG=1` | Logs placeholder detection and kernel injection to `stderr`. |
| `ANV_CM_TRACE=1` / `2` | Logs every CM kernel dispatch (`1`), plus surface states (`2`), to `stderr`. |

`0002` debug tools (included in the release drivers). These variables refer to a kernel as `x,y` matching the logs. Because all hash placeholders share the workgroup size `(1, 1, 23)`, each kernel is assigned the next free `x,y` pair (`1..16` each) in the order the process first compiles it. `ANV_CM_DEBUG=1` prints `ANV CM: kernel <hash> is x,y in this process's log` (for older id placeholders, `x,y` is the workgroup size):

| Variable | Effect |
|---|---|
| `ANV_CM_PC=1` | Prints the push constants of each dispatch (use with `ANV_CM_TRACE`). |
| `ANV_CM_CAPTURE="x,y:N;..."` | CPU-side dump of every buffer that kernel `x,y` reads at its `N`-th dispatch into `ANV_CM_CAP_DIR` (default: `~/.cache/xess-xmx/cap`, or under `XDG_CACHE_HOME` when set). |
| `ANV_CM_GCAP="x,y:N;..."` | GPU-side copy of every small (`<= 4 KiB`) buffer right before the `N`-th dispatch into `ANV_CM_CAP_DIR`. |
| `ANV_CM_VIEW="x,y:s=X,Y:S;..."` | Lets surface `s` of kernel `x,y` see surface `S` of kernel `X,Y` (used for probe kernels). |
| `ANV_CM_ONLY="x,y ..."` | Injects only the listed kernels; leaves others as placeholders. |
| `ANV_CM_SKIP="x,y ..."` | Never injects the listed kernels. |
| `ANV_CM_SYNC=1` | Inserts a full flush and stall around every CM kernel dispatch. |
| `ANV_CM_MAXGROUPS=<n>` | Clamps the dispatch workgroup count to `<n>`. |
| `ANV_CM_PREEMPT=0` / `1` | Forces thread preemption off or on. |

### Vulkan layer (`VK_LAYER_XESS_xmx`)

| Variable | Effect |
|---|---|
| `XMX_LAYER_LOG` | `1`: logs to `stderr`; `<path>`: appends to that file. |
| `XMX_LAYER_HELPER` | Path to the compile script (falls back to `ANV_CM_HELPER`, then `tools/cm-compile.sh` next to `ANV_CM_KERNEL_DIR`). |
| `XMX_LAYER_PROF=1` | Measures GPU time of every replaced dispatch and writes a per-kernel table to the log every 5 seconds. |
| `XMX_LAYER_PROF=2` | Uses one timestamp pair per run of back-to-back replaced dispatches, reported as one `runs` row (measures XeSS's GPU time including gaps between dispatches). |
| `XMX_LAYER_PROF_SEC=<n>` | Sets the profiling report period in seconds. |
| `XMX_LAYER_DISABLE=1` | Turns the layer off. |

### D3D11 bridge, Vulkan wrapper, and FSR 4 plugin

| Variable | Effect |
|---|---|
| `XESS_DX11_TRACE=1` | Logs each call in `dx11bridge/libxess_dx11.dll`. |
| `XESS_DX11_DUMP=<n>` | Writes the D3D11 bridge output of frame `<n>` next to `libxess_dx11.dll`. |
| `VKWRAP_LOG=1` | Writes `vkwrap.log` next to `vkwrap/vulkan-1.dll`. |
| `FSR4_XMX_NO_DIALOG=1` | Suppresses the error dialog in the third-party FSR 4 on XMX plugin. |
| `FSR4_XMX_LITE=1` / `FSR4_XMX_PROVIDER_INDEX=0` | Selects the Lite model or full model (4.0.2X) in the FSR 4 on XMX plugin. |

### FSR on XeSS and optical flow (`ffxfg/`)

These can be set in the environment or as `KEY=VALUE` lines in `ffx_xess.env` / `ffx_xefg.env` next to the DLL (environment variables take precedence; `XESS_FG` must be in the launch options because the shim reads it before the DLLs load):

| Variable | Effect |
|---|---|
| `FFX_XESS_LOG=<path>` | Upscaler log file (`0` disables logging; default `ffx_xess.log`). |
| `FFX_XESS_XESSLOG=0..3` | XeSS internal log level: `0` debug, `1` info, `2` warnings (default), `3` errors. |
| `FFX_XESS_QUALITY=100..106` | Forces the XeSS quality preset (`100` Ultra Performance to `105` Ultra Quality Plus, `106` Native AA). |
| `FFX_XESS_AUTOEXPOSURE=0` | Disables XeSS auto exposure. |
| `FFX_XEFG_MULT=<n>` | Frames shown per rendered frame (defaults to `XESS_FG`, else `2`), capped at XeSS-FG's maximum. |
| `FFX_XEFG_LOG=<path>` | Frame generation log file (`0` disables logging; default `ffx_xefg.log`). |
| `FFX_XEFG_XEFGLOG=0..3` | XeSS-FG internal log level: `0` debug, `1` info, `2` warnings (default), `3` errors. |
| `FFX_XEFG_HUDLESS=0` | Disables HUD-less image and UI composition; XeSS-FG interpolates the whole image including UI. |
| `FFX_XEFG_SLEEP=0` | Skips calling `xellSleep` (XeLL itself stays on because XeSS-FG requires it). |
| `FFX_XEFG_OWNWAIT=0` | Gives the game the XeSS-FG proxy's frame latency waitable object instead of the DLL's own object. |
| `FFX_XEFG_PACE=1` | Releases the DLL's waitable object when the GPU finishes the presented frame (via a fence and helper thread, 200 ms timeout) instead of when Present returns. |
| `FFX_XEFG_VALIDITY=0` | Measuring aid: tags depth and motion vectors as valid until the next present instead of copying them at prepare time, to measure copy cost (can flicker if the game overwrites them before present). |
| `FFX_XEFG_EXCLUSIVE=1` | Lets exclusive fullscreen requests through instead of keeping the swap chain windowed (disables interpolation in exclusive fullscreen). |
| `FFX_XEFG_VENDOR=0` | Lets XeSS-FG and XeLL see the vendor ID reported by DXVK instead of spoofing Intel (`0x8086`) on Intel adapters when `FSR_VENDOR=amd` is used. |
| `FFX_XEFG_MVNDC=1` | Treats motion vectors as NDC (as Cyberpunk 2077 passes them). |
| `FFX_XEFG_OFLOW_DIV=n` | With `OFLOW_FG=1`: runs optical flow at `1/n` of the display size (`1` to `4`, default `1`). |
| `FFX_XEFG_OFLOW_DEPTH=d` | With `OFLOW_FG=1`: sets the constant depth value written by the expansion shader (default `1.0`). |
| `FFX_XEFG_CAP=auto` / `<us>` / `0` | With `OFLOW_FG=1`: controls the rendered frame interval via XeLL's frame cap (`auto` is default, `<us>` sets a fixed microsecond interval, `0` disables). |
| `FFX_XEFG_CAP_PAD=ms` | With `OFLOW_FG=1`: sets `auto`'s initial guess in milliseconds for the time needed beyond the measured GPU time (default `1.5`). |
| `FFX_XEFG_REFRESH=Hz` | With `OFLOW_FG=1`: overrides the display refresh rate when Windows reports a wrong one. |
| `FFX_XEFG_TIMES=1` | With `OFLOW_FG=1`: logs frame time, time in Present, and XeLL sleep percentiles every 300 frames. |

Input-isolation debug switches (`FFX_XEFG_CAMERA`, `FFX_XEFG_JITTER`, `FFX_XEFG_RENDERTIME`, `FFX_XEFG_RENDERSCALE`, `FFX_XEFG_DUMP`, `FFX_XEFG_ORDER`, `FFX_XESS_JITTER`) are only compiled in when building `ffxfg` with `FFXFG_DEBUG=1` in the environment of `build.bat`. See the comments at their definitions in `ffxfg/ffx_xefg.cpp`.

### Scripts

Environment variables and arguments accepted when running the package's scripts from a shell:

| Script | Arguments and variables |
|---|---|
| `install.sh`, `uninstall.sh` | `STEAM_DIR`: overrides the Steam installation path (default: first found of `~/.local/share/Steam`, `~/.steam/steam`, `~/.steam/root`). On `install.sh`: `XMX_DRIVER=patched`, `stock`, or `auto` selects the session driver; `XMX_NO_ENV=1` skips writing the session environment and session check while still installing the shim, hook, and watcher. |
| `enable.sh`, `disable.sh` | Requires `XMX_APPID` or `XMX_PREFIX`. `XMX_CONF` overrides the config file path (default `~/.config/xess-xmx.conf`). `XMX_GAME` refuses to run while that process is running. |
| `tools/switch-driver.sh [version]` | With no argument, lists the package's drivers (`*` marks the default). With a version, checks it and makes it the default. |
| `tools/rebuild-driver.sh [version]` | Rebuilds the patched driver for `[version]` (defaults to the system's Mesa version). `XMX_MESA_DIR` (default `~/.cache/xess-xmx/mesa-src`) holds the source and build trees. |
| `tools/session-check.sh [--dry-run]` | Run by `xess-xmx-check.service`. `--dry-run` only reports and populates `lib/deps/`. `XMX_CHECK_LIB` and `XMX_CHECK_ICD` test a specific driver library or ICD. |
| `tools/get-xess.sh [package dir]` | Downloads Intel's XeSS SDK libraries into `xess/`. `XESS_TAG` and `XESS_SHA256` select another SDK release. |
| `tools/get-ffx.sh [package dir]` | Downloads AMD's FidelityFX loader and providers into `ffx/` for FSR 3.1.0 to 3.1.3 games. |
| `tools/fsr-fg.sh all\|on\|off\|status <app id \| game folder>` | Installs or removes the FSR-on-XeSS DLLs directly in a game folder for Protons without the hook. |
| `tools/get-igc.sh [package dir]` | Downloads the compiler bundle into `igc/`. `IGC_TAG` and `NEO_TAG` select other releases (defaults: IGC `2.10.10`, compute-runtime `25.18`; other versions are untested). |
| `tools/cm-compile.sh` | Called by the driver and layer. Accepts `XMX_IGC_DIR` (default `<package>/igc`), `CM_DEVICE` (overrides `ocloc -device` detection), `CM_LOG` (default `~/.cache/xess-xmx/compile.log`), and `CM_NO_LOAD_PAYLOAD=1` (used by the layer). The driver also passes `CM_PCI_ID`, `CM_GRF_BYTES`, and `CM_OUT_DIR` (when `CM_OUT_DIR` is unset, `cm-compile.sh` packs the output with `tools/cmk_pack.py`). |
