# Developer Debugging Reference

This document covers the developer logs, the XeLL (Xe Low Latency) driver-mode probe, and the full environment variable reference across every component of the project.

For user-facing symptoms, fixes, and quick bug-report instructions, see [../troubleshooting.md](../troubleshooting.md). For Steam launch options, see [../launch-options.md](../launch-options.md). For internal architecture details, see [how-it-works.md](how-it-works.md), and for building from source, see [building.md](building.md).

---

## 1. Logs

In the paths below, `<pfx>` is the game's Wine prefix (`<Steam library>/steamapps/compatdata/<appid>/pfx`, or `~/.local/share/Steam/steamapps/compatdata/<appid>/pfx` on the main drive).

* **Shim (`igdext64.dll`):** `IGDEXT_TRACE=1` writes `C:\igdext_trace.log` inside the prefix (`<pfx>/drive_c/igdext_trace.log`). It records every D3D12 (Direct3D 12) extension call, the detected GPU and PCI ID, the `CheckFeatureSupport` (`OPTIONS1` / `OPTIONS2`) feature answers, each C-for-Metal (CM) kernel with its ID and hash, and `callers(...)` module chains so super resolution (`libxess.dll`) and frame generation (`libxess_fg.dll`) kernels can be told apart. It also dumps every raw kernel binary and options string to `C:\igdext_dump` (`<pfx>/drive_c/igdext_dump/`).
* **XeLL (`libxell.dll` hook):** `IGDEXT_XELL_LOG=1` writes `C:\igdext_xell.log` (`<pfx>/drive_c/igdext_xell.log`). It records what the game passes to `xellSetSleepMode` and `xellSetFgEnabled`, the real frame rate and blocking times from `xellSleep` timing, the low latency's state (`low latency: ...`, the same lines as in `ffx_xefg.log` on the FSR routes), XeLL's own messages via `xellSetLoggingCallback`, and the average simulation-start to present-end latency from `xellGetFramesReports` every 100 frames.
* **XeSS Frame Generation (`libxess_fg.dll` hook):** `IGDEXT_XEFG_LOG=1` writes `C:\igdext_xefg.log` (`<pfx>/drive_c/igdext_xefg.log`). It records XeSS-FG's own internal log via `xefgSwapChainSetLoggingCallback`, `maxSupportedInterpolations` after context creation and initialization, `XESS_FG=5..8` probe results, tagged resource types/formats, camera matrices every 300 frames, and changes to the interpolated frame count.
* **Control Library (`ControlLib.dll`):** `XESS_FG_LOG=1` (or `IGDEXT_TRACE=1`) writes `C:\igcl_stub.log` (`<pfx>/drive_c/igcl_stub.log`), recording every IGCL call and the `XESS_FG` override value returned for `CTL_3D_FEATURE_FRAME_GENERATION`.
* **Patched Mesa ANV driver:** `ANV_CM_DEBUG=1` logs each placeholder and kernel injection to the game's `stderr` (and while a kernel is being compiled on first launch or a cache miss, the patched driver prints `ANV CM: compiling kernel` even without `ANV_CM_DEBUG`); `ANV_CM_TRACE=1` logs every dispatch (`ANV CM: walker kernel x,y,z ...` and descriptor heap indices), and `ANV_CM_TRACE=2` also dumps the 16 `RENDER_SURFACE_STATE` dwords of each bound surface. To capture the game's `stderr`, put `exec 2>>/some/file` into the launch wrapper's config, or set `PROTON_LOG=1` (which writes `~/steam-<appid>.log`).
* **Vulkan layer (`VK_LAYER_XESS_xmx`):** `XMX_LAYER_LOG=1` logs to `stderr`, or `XMX_LAYER_LOG=<path>` appends to that file (self-test results, template round-trips, kernel replacements, and `XMX_LAYER_PROF` GPU profiling tables; the layer logs kernel compiles to `~/.cache/xess-xmx/compile.log`).
* **Compiler (`tools/cm-compile.sh`, driver, and layer):** `~/.cache/xess-xmx/compile.log` (`CM_LOG` overrides the path). Logs every kernel compile (`cm-compile ok`, `cm-compile FAILED`, `cm-compile PACK FAILED`, `cm-compile TIMEOUT`) with target device, workgroup size, flags, and elapsed time.
* **Proton hook (`tools/proton-hook.py`):** `~/.cache/xess-xmx/hook-<appid>.log`. `<appid>` is `SteamGameId`, else `SteamAppId`, else `STEAM_COMPAT_APP_ID` (the only one Steam's install-script helper `iscriptevaluator.exe` gets, which Proton runs before a game: it is the run that puts back the files a killed run left), else the name of the executable. Each run that has something to say starts with one line of what it was started with (the time, the Proton, the executable, the launch options the hook reads), followed by the hook's lines: the route `XESS=auto` chose and why, each file it replaced, and that the game's own files are back. The file is emptied when it has passed 200 KB. The same lines go to Proton's own stderr, which is Steam's output (the user journal in SteamOS Game Mode), not the file `PROTON_LOG=1` writes.
* **Session check (`xess-xmx-check.service`):** `~/.cache/xess-xmx/driver-status` and the user systemd journal (`journalctl --user -u xess-xmx-check`).
* **FSR-to-XeSS and optical-flow DLLs (`ffxfg/`):** Write `ffx_xess.log`, `ffx_xefg.log`, and `dxgi_xefg.log` in the game folder next to the DLLs (`FFX_XESS_LOG=0` and `FFX_XEFG_LOG=0` turn the first two off).
* **D3D11 bridge and Vulkan wrapper:** `XESS_DX11_TRACE=1` logs `dx11bridge/libxess_dx11.dll` calls to `stderr` and `OutputDebugString`; `VKWRAP_LOG=1` writes `vkwrap.log` next to `vkwrap/vulkan-1.dll`.

### Counting Dispatches per Kernel

From an `ANV_CM_TRACE=1` stderr log, count dispatches per kernel to see what runs every frame:

```bash
grep -a -o "walker kernel [0-9]*,[0-9]*,[0-9]*" stderr.log | sort | uniq -c
```

---

## 2. XeLL Driver-Mode Probe

Under Proton, Intel XeLL (`libxell.dll`) runs in its cross-vendor mode ([how-it-works.md](how-it-works.md)). The reason is inside `libxell.dll` itself (read from 1.3.2.10, the SDK copy; run `strings libxell.dll | grep wine_get_version` to check whether the 1.3.0.5 build that games ship has the same check):

1. Before anything else at context creation, `libxell.dll` checks that the DXGI adapter has `VendorId == 0x8086` **and** that `GetProcAddress(GetModuleHandle("ntdll.dll"), "wine_get_version")` returns `NULL`. Under Wine that export exists, so `libxell.dll` immediately selects its built-in context (`[XeLL] Using built-in context`) without ever loading `igdext64.dll`.
2. When `wine_get_version` is hidden, `libxell.dll` loads `igdext64.dll`, asks `GetSupportedVersions` for extension version `5/13` or `4/13`, checks that the driver version string is `>= 31.0.101.6252`, and checks that `INTCDeviceInfo::GTGeneration` is one of Intel's `PRODUCT_FAMILY` IDs (`1300` for Panther Lake, `1275` Lunar Lake, `1274` Battlemage, `1273` Arrow Lake, `1272` Meteor Lake, `1270` Alchemist, `1276`, or `>= 1340`; the shim normally reports `30`, `20`, or `12` there for XeSS).
3. Only after passing those checks does `libxell.dll` call the latency-reduction entry points (`_INTC_D3D12_GetLatencyReductionStatus`, `_INTC_D3D12_LatencyReductionExt`, `_INTC_D3D12_LatencyReductionGetRenderSubmitTimingsBuffers`, `_INTC_D3D12_RenderSubmitStart`, `_INTC_D3D12_SetNumGeneratedFrames`, `_INTC_D3D12_SetPresentSequenceNumber`). Details and offsets are in `shim/src/dll/XellDriver.cpp`.

### Running the Probe

`IGDEXT_XELL_PROBE=<version>` turns all of that on for one launch: the shim hides `wine_get_version` from `libxell.dll` (via an import-table hook of `GetProcAddress` in `libxell.dll` only, installed when `libxell.dll` loads), reports `gpu.productFamily` in `GTGeneration`, claims support for that latency-reduction interface version, accepts the `Set*` pacing calls, allocates guard-page buffers (`PAGE_GUARD`) for the render-submit timing buffers, and logs every call and every read/write fault XeLL makes on the timing buffers.

Put the probe wrapper in the game's Steam launch options:

```text
/path/to/package/tools/xell-probe.sh %command%
```

*(If the game uses the per-game `enable.sh` install, keep `xmx-launch.sh` in front: `/path/to/package/xmx-launch.sh /path/to/package/tools/xell-probe.sh %command%`.)*

Play for a minute with frame generation on (the Wuthering Waves title screen with FG enabled is enough), quit, and check the summary the wrapper prints to `stderr`. It also writes `~/xess-xmx-probe-<date>.tar.gz` containing:
* `proton-xell.log`: XeLL's own `OutputDebugString` messages (the wrapper sets `PROTON_LOG=1` and extracts the XeLL lines from `~/steam-<appid>.log`),
* `libxell.log`: written by `libxell.dll` 1.3.2+ when `XELL_FORCE_LOGS_TO_FILE=1` is set (`1.3.0.x` has no file log),
* `igdext_trace.log`: `XELLPROBE` lines showing the IAT hook, every latency-reduction call with its arguments, each `RenderSubmitStart` with the elapsed time since the previous one, and every read or write XeLL makes to the timing buffers with the slot number, in-slot byte offset, and code address (`libxell.dll+0x...`),
* `igdext_xell.log`, `driver-status`, and `environment.txt`.

Attach `~/xess-xmx-probe-<date>.tar.gz` to the issue.

### How to Read the Probe Result

* **No `XELLPROBE libxell.dll ...: GetProcAddress import hooked` line:** The shim never saw `libxell.dll` load. Either the shim is not loaded (`./install.sh`), or the game created its XeLL context before `igdext64.dll` was loaded: toggle frame generation off and back on in the game's menu, which recreates the context, and check again.
* **`[XeLL] Using built-in context` and no `Intel Extensions` lines although the hook is in place:** XeLL still did not take the Intel path. Check the DXGI vendor ID the game sees (`force_vk_vendor=0`, [../games.md](../games.md)) and whether `XELL_FORCE_CROSS_VENDOR` is set.
* **`XeLL context creation failed - unsupported device` or `unsupported driver`:** The family ID or driver version string the shim reported was rejected. `IGDEXT_FAMILY=<id>` overrides the family ID.
* **`Intel Extensions initialized.` and `Latency reduction enabled.` followed by `XELLPROBE LatencyReductionExt(...)` and buffer accesses:** Driver mode started. The access log gives the slot size and field offsets; a read of `gpu +0x0` followed by a crash means XeLL treats the GPU buffer pointer as a COM object (an `ID3D12Resource*` it calls `Map` on) rather than plain memory, which a future shim version must then provide.
* **`GetDisplayTelemetry` called and then cross-vendor fallback:** Retry with `XELL_PROBE_TELEMETRY=1`.

---

## 3. Switches Reference

User-facing Steam launch options for a single game are explained in [../launch-options.md](../launch-options.md) and [../frame-generation.md](../frame-generation.md). The tables below list every environment variable and script switch across the project.

### Session Environment (`install.sh` and `enable.sh`)

Written by `./install.sh` to `~/.config/environment.d/50-xess-xmx.conf` or by `./enable.sh` to `~/.config/xess-xmx.conf` (sourced by `xmx-launch.sh`):

| Variable | Effect |
|---|---|
| `VK_DRIVER_FILES` | Puts the package's driver (`run/intel_icd.json`) first as the 64-bit Intel Vulkan driver; every other driver on the system (other GPUs and the 32-bit Intel ICD) stays in the list. |
| `VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer,VK_EXT_descriptor_heap,VK_KHR_present_wait2` | `VK_EXT_descriptor_buffer` and `VK_EXT_descriptor_heap` (vkd3d-proton 3.1 and newer) keep vkd3d-proton on its classic `MUTABLE` descriptor heap, which the kernel binding code needs. `VK_KHR_present_wait2` keeps vkd3d-proton on present wait 1: with present wait 2, Cyberpunk 2077 with frame generation (several presents per frame) crashed within minutes in `vkWaitForPresent2KHR`. |
| `force_vk_vendor=0` | Overrides Mesa's `force_vk_vendor=-1` game profiles ([../troubleshooting.md](../troubleshooting.md)); `force_vk_vendor=-1` in one game's launch options restores Mesa's behavior. |
| `ANV_CM_KERNEL_DIR`, `ANV_CM_HELPER` | The patched driver's kernel folder (`<package>/kernels`) and compiler helper (`<package>/tools/cm-compile.sh`). |
| `INTC_ALT_DRIVER_EXTENSIONS_PATH` | Points XeSS at the prefix folder containing the shim (`C:\windows\system32\driverstore\filerepository\igd_faux.inf_1`). Proton Experimental exposes a driver store in the registry and needs no help; Proton 11.0 has none, and without this variable XeSS never loads the shim there (no XMX, frame generation capped at 2x). GE-Proton sets its own value (`C:\Windows\System32`) for every game. |

*(On the Xe3 and Xe2 GPUs of `layer/gpu-ids.txt`, where `./install.sh` defaults to `stock` mode, `50-xess-xmx.conf` also sets `MESA=stock-26.3.0`, `VK_ADD_LAYER_PATH=<package>/layer`, `VK_LOADER_LAYERS_ENABLE=VK_LAYER_XESS_xmx`, `XMX_LAYER_HELPER=<package>/tools/cm-compile.sh`, and `XMX_IGC_DIR=<package>/igc`.)*

### Launch-Option Shorthands (`tools/proton-hook.py`)

Run by Proton through the `user_settings.py` that `./install.sh` places into every Proton ([../launch-options.md](../launch-options.md)). Any variable set explicitly in the game's launch options wins over the variable a shorthand expands to:

| Variable | Effect |
|---|---|
| `XESS=auto` | Looks after the game's own XeSS as `XESS=latest` does, then takes the FSR route the game folder calls for (`auto_route()`): with `libxess_fg.dll` in the game, none; without it and with an FSR 3.1+ frame generation provider, that of `FSR=xess`; with only an FSR upscaler, that of `FSR=xess`, and of `FSR=xess FSR_FG=inject` when `XESS_FG=<n>` or `OFLOW_FG=1` asks for frame generation; with neither, none. An `FSR` or `FSR_FG` of the launch options decides instead. `OFLOW_FG=1` next to it is used only where neither XeSS frame generation nor FSR 3.1+ is found. Files the hook added itself (another run of the same game that is still alive) are not taken for the game's. |
| `XESS=auto_off` | No `XESS=auto` for this game, where the session has it for every game. Nothing else changes. |
| `XESS=latest` | Runs the game with Intel's XeSS SDK libraries (`xess/`, fetched by `tools/get-xess.sh`) in place of its older ones, replaces `libxess_dx11.dll` with `dx11bridge/libxess_dx11.dll` (adding `xess/libxess.dll` if missing), and for Vulkan games that import `vulkan-1.dll` links `vkwrap/vulkan-1.dll` (`WINEDLLOVERRIDES=vulkan-1=n,b`) and enables `VK_LAYER_XESS_xmx`. Restores originals when the game exits. |
| `XESS_VK=1` / `XESS_VK=0` | With `XESS=auto` or `XESS=latest`: forces the Vulkan wrapper (`vkwrap/vulkan-1.dll` + layer) on for a Vulkan game that loads `vulkan-1.dll` at runtime (`1`), or disables it (`0`). |
| `XESS=dp4a` | Sets `IGDEXT_FORCE_FALLBACK=1`. |
| `XESS_LL=off` / `on` / `auto:<ms>` | Sets `IGDEXT_XELL_CAP=off` / `auto` / `auto:<ms>` (`0` and `1` are `off` and `on`). Anything else, a plain number included, is refused. An `IGDEXT_XELL_CAP` that is already set decides. |
| `XESS_LOG=1` | Sets `IGDEXT_TRACE=1 IGDEXT_XELL_LOG=1 IGDEXT_XEFG_LOG=1 XESS_FG_LOG=1`. |
| `XESS_LOG=2` | `XESS_LOG=1`, and sets `PROTON_LOG=1`, `ANV_CM_DEBUG=1`, `XMX_LAYER_LOG=~/.cache/xess-xmx/layer.log` (emptied at each start, as the layer appends), `VKWRAP_LOG=1`, and `XESS_DX11_TRACE=1`. |
| `FSR=xess` | Replaces the game's FSR 3.1+ DLLs with the package's (`ffxfg/`), with the SDK's XeSS libraries next to them (and `ffx/amd_fidelityfx_loader_dx12.dll` for FSR 3.1.0–3.1.3 single-DLL games); restores originals when the game exits. |
| `FSR_FG=xess` | Same as `FSR=xess`, for the frame generation DLL only (keeps the game's FSR upscaler, or uses `ffx/amd_fidelityfx_upscaler_dx12.dll` for FSR 3.1.0–3.1.3). |
| `FSR_FG=inject` | Means `FSR=xess FSR_FG=inject`: also links `dxgi.dll` and `ffx_xefg_bridge.dll` next to the game executable, for a game with FSR 3.1+ upscaling and no FSR frame generation. |
| `OFLOW_FG=1` | Links `dxgi.dll`, `ffx_xefg_bridge.dll`, `libxess_fg.dll`, and `libxell.dll` next to the game executable and sets `FFX_XEFG_OFLOW=1` (XeSS-FG from optical flow on the final image for any DX12 game; more than 2x is experimental with it). Ignored if `FSR` or `FSR_FG` is set, and next to `XESS=auto` in a game that has XeSS frame generation or FSR 3.1+. |
| `FSR_VENDOR=amd` | Where FSR runs on XeSS (`FSR`, `FSR_FG`, or `XESS=auto` in a game without XeSS frame generation): sets `dxgi.customVendorId = 1002` in `DXVK_CONFIG` so games that restrict FSR to AMD GPUs show the option, while `ffx_xefg` and `ControlLib.dll` still report `0x8086` to XeSS-FG and XeLL. |

### Driver Choice and Mesa Options (Game Environment)

| Variable | Effect |
|---|---|
| `MESA=<version>` | Makes `lib/libvulkan_intel.so` (`icd/xmx-icd.c`) load that driver from `lib/mesa-<version>/` instead of the default in `lib/active` (`26.1`, `26.3`, `stock-26.3`; a prefix is enough). Prints `xess-xmx: using the Mesa ... driver` to `stderr`, or `no such driver` and loads the default. Caveat: setting `MESA=stock-26.3` in a patched session loads the unpatched driver without enabling `VK_LAYER_XESS_xmx`, so that game runs without XMX (on DP4a). |
| `INTEL_DEBUG=no-jay` | On Mesa 26.3: compiles the game's own shaders with the old `brw` compiler instead of Jay. |

### Shim (`igdext64.dll`) and `ControlLib.dll` (Game Environment)

| Variable | Effect |
|---|---|
| `IGDEXT_TRACE=1` | Enables the trace log (`C:\igdext_trace.log`) and kernel dump (`C:\igdext_dump\`). |
| `IGDEXT_IGNORE_FAILED=1` | Ignores `C:\igdext_kernels\compile_failed.txt` instead of falling back to DP4a after a failed compile. |
| `IGDEXT_SKIP_SELFTEST=1` | Skips the `anv_canary` self-test pipeline and assumes the patched driver or layer is active. |
| `IGDEXT_FORCE_FALLBACK=1` | Declines the D3D12 extension context (`E_NOTIMPL`): XeSS uses DP4a, and XeSS frame generation uses its generic 2x path. The shim also does this automatically for an Intel PCI device ID not in its table (trace: `fallback: unknown Intel device id`) unless `IGDEXT_GMD` is set. |
| `IGDEXT_OPTIONS1=<xmx>,<dlboost>,<emul64>` | Overrides `CheckFeatureSupport(INTC_D3D12_FEATURE_D3D12_OPTIONS1)`. |
| `IGDEXT_OPTIONS2=<simd16>,<lsc>,<legacy>` | Overrides `CheckFeatureSupport(INTC_D3D12_FEATURE_D3D12_OPTIONS2)` for non-FG callers (default from the detected GPU: `1,1,0` on Xe2/Xe3, `0,1,0` on Xe-HPG / Xe-LPG(+)). Does not apply to calls from `libxess_fg.dll`. |
| `IGDEXT_OPTIONS2_FG=<simd16>,<lsc>,<legacy>` | Overrides `CheckFeatureSupport(INTC_D3D12_FEATURE_D3D12_OPTIONS2)` for calls coming from `libxess_fg.dll` (when unset, `libxess_fg.dll` calls use the GPU default). |
| `IGDEXT_GMD=<arch>,<rel>`, `IGDEXT_GTGEN=<n>`, `IGDEXT_GTNAME=<name>`, `IGDEXT_EUS=<eus>,<cores>` | Override the reported GMD architecture/release, `GTGeneration`, `GTGenerationName`, and `EUCount`/`XeCoresCount` in `INTCDeviceInfo1`. Setting `IGDEXT_GMD` also allows an unrecognized Intel PCI device ID to use the extension context. |
| `IGDEXT_XELL_LOG=1` | Enables `C:\igdext_xell.log`, including the average simulation-start to present-end latency from XeLL's frame reports every 100 frames. Next to it, `input latency:` gives the same from where the game reads its controller (`XInputGetState`). |
| `IGDEXT_XELL_FIXCAP=1` / `IGDEXT_XELL_KEEPCAP=1` | Applies the Wuthering Waves 30 fps XeLL frame-cap fix in any game (`FIXCAP=1`) or disables it everywhere (`KEEPCAP=1`; default: Wuthering Waves only). |
| `IGDEXT_XELL_CAP=off` / `auto:<ms>` / `<us>` | Configures low-latency frame pacing while frame generation runs ([../frame-generation.md](../frame-generation.md)): `off` (or `0`) turns it off; `auto` (default) or `auto:<ms>` starts each frame `<ms>` earlier (later if negative); `<us>` (`0..1000000`) disables auto-pacing and raises any smaller `minimumIntervalUs` passed to `xellSetSleepMode` to `<us>` microseconds per displayed frame (takes effect when the game calls `xellSetSleepMode`; any other value turns both off). With `IGDEXT_XELL_LOG=1`, the log has the `low latency:` lines: the state at start, `frames are held back in xellSleep (frame generation on ...)` or `no wait (frame generation off ...)`, `frame time ... ms without a wait` once learned, `a queue of ...` when a heavier scene raises it, `waited in N of 300 frames ...` every 300 frames, and the check's lines. On the FSR routes and with `OFLOW_FG`, the frame generation DLL paces the game instead (`xell context ...: frame start paced by its caller` in this log), and `ffx_xefg.log` has the same `low latency:` lines. |
| `IGDEXT_XELL_CHECK=0` | The low latency's wait is not checked. By default it stays only if the frame time with it is within 1% of the frame time without it and the latency is lower (`low latency: the wait ... is kept` / `costs frame time` / `does not lower the latency` in the log). Read by the shim (a game's own XeSS-FG) and by `amd_fidelityfx_framegeneration_dx12.dll` (the FSR routes, `OFLOW_FG`). |
| `IGDEXT_XELL_POLLS=0\|1` | `0`: the game's `GetFrameStatistics` and `CheckColorSpaceSupport` go to the swap chain each time (they wait there for XeSS-FG's presentation thread). `1`: they are answered as of the game's last `Present`. The default is `1` with the low latency on, on every path (with `OFLOW_FG` the frame cap paces the frames, and the answers are on too). Read by the shim and by `amd_fidelityfx_framegeneration_dx12.dll`. |
| `XESS_FG=<n>` | Frame generation multiplier override (`<n>`x): `2..4` is answered through `ControlLib.dll` (`ControlLib.c` accepts `2..16` and clamps values above `4` to `3`, i.e., 4x); `5..8` (`8` is the maximum) is raised in `libxess_fg.dll` memory by `XefgHook.cpp` (`XefgRaise`): Intel supports up to 4x, and the game stays at 4x if the change cannot be verified. All four work; `7` and `8` are **experimental** (not recommended due to latency). |
| `IGDEXT_XEFG_LOG=1` | Enables `C:\igdext_xefg.log`: XeSS-FG's own log (`xefgSwapChainSetLoggingCallback`), the maximum number of generated frames after context creation and initialization, tagged resources/constants, and every change of the number. |
| `XESS_FG_LOG=1` | Enables `C:\igcl_stub.log`: logs every call `ControlLib.dll` answers. |
| `IGDEXT_XELL_PROBE=<version>` | Hides `wine_get_version` from `libxell.dll`, reports the Intel `PRODUCT_FAMILY` ID as `GTGeneration`, claims XeLL driver-mode support (interface `<version>`), accepts the `Set*` pacing calls, and logs XeLL's use of the timing buffers (`tools/xell-probe.sh`, [Section 2](#2-xell-driver-mode-probe)). |
| `IGDEXT_FAMILY=<id>` | Overrides the `PRODUCT_FAMILY` ID reported in `GTGeneration` when `IGDEXT_XELL_PROBE` is active (default from the detected GPU). |
| `IGDEXT_XELL_TELEMETRY=1` | With `IGDEXT_XELL_PROBE`: answers `_INTC_D3D12_GetDisplayTelemetry` with a zeroed buffer (`S_OK`) instead of `E_NOTIMPL`. |

### Patched Mesa ANV Driver (`patches/0001` and `patches/0002`)

#### Core Driver Variables (`0001-anv-cm-kernel-injection.patch`)

| Variable | Effect |
|---|---|
| `ANV_CM_KERNEL_DIR` | Sets the default helper path (`$ANV_CM_KERNEL_DIR/../tools/cm-compile.sh`); set by `install.sh` and `enable.sh`. With patch `0002`, any `<blake3>.cmk` placed in this folder also replaces the matching compute shader. |
| `ANV_CM_HELPER` | Compiler helper script (default `$ANV_CM_KERNEL_DIR/../tools/cm-compile.sh`). |
| `ANV_CM_SPV_DIR` | Directory where the shim's `.spv` and `.opt` files live (default `$WINEPREFIX/drive_c/igdext_kernels`). |
| `ANV_CM_COMPILE_JOBS=<n>` | Maximum number of detached background worker threads started on a cache miss to prefetch and compile uncached `.spv` kernels in `ANV_CM_SPV_DIR` (default: `min(online_cpus - 1, 8)`, max `32`; `0` turns background prefetching off). |
| `ANV_CM_COMPILE_TIMEOUT=<s>` | Seconds a single `cm-compile.sh` run may take before its process group is killed with `SIGKILL` and marked failed (default `120`, max `86400`; `0` disables the timeout). The layer (`VK_LAYER_XESS_xmx`) applies the same limit. |
| `ANV_CM_DEBUG=1`, `ANV_CM_TRACE=1\|2` | Driver debug and per-dispatch trace logs to `stderr` ([Section 1](#1-logs)). |

#### Driver Debug Tools (`0002-anv-cm-debug-tools.patch`, included in release drivers)

All hash-format placeholders share the workgroup size `(1, 1, 23)`, so each kernel is assigned the next free `x,y` (`1..16` each) in the order the process first compiles it. `ANV_CM_DEBUG=1` prints `ANV CM: kernel <hash> is x,y in this process's log` (for legacy ID placeholders from older shims, `x,y` is the placeholder's workgroup size):

| Variable | Effect |
|---|---|
| `ANV_CM_PC=1` | Prints the non-table push constant dwords of each dispatch (requires `ANV_CM_TRACE`). |
| `ANV_CM_CAPTURE="x,y:N;..."` | CPU-side dump of every raw buffer surface (`format == 0x1ff`) that kernel `x,y` reads at its `N`-th dispatch into `ANV_CM_CAP_DIR` (`cap_x_y_nN_s<surf>.bin`). |
| `ANV_CM_GCAP="x,y:N;..."` | GPU-side copy (`mi_memcpy` into a 4 MiB host-coherent BO right before the dispatch) of every small (`<= 4 KiB`) raw buffer surface at the `N`-th dispatch of kernel `x,y`, written to `ANV_CM_CAP_DIR` (`gcap_x_y_nN_s<surf>.bin`) 40 dispatches of that kernel later. |
| `ANV_CM_CAP_DIR=<dir>` | Output folder for `ANV_CM_CAPTURE` and `ANV_CM_GCAP` (default `~/.cache/xess-xmx/cap`, or `$XDG_CACHE_HOME/xess-xmx/cap`; no capture happens if `ANV_CM_CAP_DIR` is unset and neither `XDG_CACHE_HOME` nor `HOME` is an absolute path). |
| `ANV_CM_VIEW="x,y:s=X,Y:S;..."` | Makes surface `s` of kernel `x,y` use the most recently recorded 64-byte surface state `S` of kernel `X,Y` (for probe kernels inspecting intermediate buffers). |
| `ANV_CM_ONLY="x,y ..."`, `ANV_CM_SKIP="x,y ..."` | Injects only (`ONLY`) or never (`SKIP`) the listed kernels; skipped kernels stay no-op placeholders. |
| `ANV_CM_SYNC=1` | Emits a full end-of-pipe sync and cache flush/invalidate before every CM kernel dispatch. |
| `ANV_CM_MAXGROUPS=<n>`, `ANV_CM_PREEMPT=0\|1` | Clamps `num_wg[0..2]` of every CM dispatch to at most `<n>` / forces `InterfaceDescriptor.ThreadPreemption` to `0` or `1` on Xe2+ (`GFX_VER >= 20`). |

### Unpatched-Driver Vulkan Layer (`VK_LAYER_XESS_xmx`, `layer/`)

The layer is loaded as an explicit layer via `VK_ADD_LAYER_PATH=<package>/layer` and `VK_LOADER_LAYERS_ENABLE=VK_LAYER_XESS_xmx`.

| Variable | Effect |
|---|---|
| `XMX_LAYER_LOG=1\|<path>` | `1`: logs to `stderr`; a path containing `/`: appends to that file. |
| `XMX_LAYER_HELPER=<path>` | Compiler helper script (else `ANV_CM_HELPER`, else `$ANV_CM_KERNEL_DIR/../tools/cm-compile.sh`). |
| `XMX_LAYER_COMPILE_JOBS=<n>` | Kernel compiles at a time, the pipeline thread that needs a kernel included: on a cache miss the other uncached kernels of `C:\igdext_kernels` are compiled by `n - 1` background threads (default: online CPUs, at most `8`; `1..32`; `1` = one after another, no prefetching). Unlike the driver's `ANV_CM_COMPILE_JOBS`, which counts only the background threads. |
| `XMX_LAYER_PROF=1\|2` | GPU timestamp profiling of replaced dispatches: `=1` records a timestamp pair around every replaced dispatch and logs a per-kernel table every `XMX_LAYER_PROF_SEC` seconds; `=2` records one timestamp pair per contiguous run of replaced dispatches and reports them as one `runs` row (measuring XeSS's GPU time including barriers and copies between dispatches). |
| `XMX_LAYER_PROF_SEC=<sec>` | Report period in seconds for `XMX_LAYER_PROF` (default `5.0`, minimum `> 0.5`). |

### D3D11 Bridge (`dx11bridge/`), Vulkan Wrapper (`vkwrap/`), and FSR / Optical-Flow DLLs (`ffxfg/`)

| Variable | Component | Effect |
|---|---|---|
| `XESS_DX11_TRACE=1` | `dx11bridge/libxess_dx11.dll` | Logs D3D12 device setup, shared texture creation, and every `xessD3D11Execute` call to `stderr` and `OutputDebugString`. |
| `XESS_DX11_DUMP=<n>` | `dx11bridge/libxess_dx11.dll` | Dumps the output texture of the `<n>`-th `xessD3D11Execute` call to `xess_dx11_dump.raw` next to `libxess_dx11.dll` (header `"XD11"`, width, height, DXGI format, row pitch, then pixel rows). |
| `VKWRAP_LOG=1` | `vkwrap/vulkan-1.dll` | Writes `vkwrap.log` next to `vulkan-1.dll`. |
| `FFX_XESS_LOG=<path>\|0` | `amd_fidelityfx_upscaler_dx12.dll` | Upscaler log file path (default `ffx_xess.log` next to the DLL); `0` turns it off. Can also be set in `ffx_xess.env`. |
| `FFX_XESS_XESSLOG=0..3` | `amd_fidelityfx_upscaler_dx12.dll` | `libxess.dll`'s own log level: `0` debug, `1` info, `2` warnings (default), `3` errors. |
| `FFX_XESS_QUALITY=100..106` | `amd_fidelityfx_upscaler_dx12.dll` | Forces a specific XeSS quality setting (`100` Ultra Performance .. `105` Ultra Quality Plus, `106` Native AA). |
| `FFX_XESS_AUTOEXPOSURE=0` | `amd_fidelityfx_upscaler_dx12.dll` | Debug: disables `XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE`. |
| `FFX_XESS_PREEXPOSURE=0` | `amd_fidelityfx_upscaler_dx12.dll` | Debug: does not pass the game's `preExposure` on (`xessSetExposureMultiplier` stays `1`); the behaviour of releases up to 20261111. |
| `FFX_XEFG_MULT=<n>` | `amd_fidelityfx_framegeneration_dx12.dll` | Frames shown per rendered frame (default `XESS_FG`, else `2`), capped at XeSS-FG's maximum. Can also be set in `ffx_xefg.env`. |
| `FFX_XEFG_LOG=<path>\|0` | `amd_fidelityfx_framegeneration_dx12.dll` | Frame generation log file path (default `ffx_xefg.log` next to the DLL); `0` turns it off. |
| `FFX_XEFG_XEFGLOG=0..3` | `amd_fidelityfx_framegeneration_dx12.dll` | `libxess_fg.dll`'s own log level: `0` debug, `1` info, `2` warnings (default), `3` errors. |
| `FFX_XEFG_HUDLESS=0` | `amd_fidelityfx_framegeneration_dx12.dll` | Ignores the game's HUD-less color texture and leaves XeSS-FG UI composition off. |
| `FFX_XEFG_HDR10=1` | `amd_fidelityfx_framegeneration_dx12.dll` | Tells XeSS-FG when the game's back buffer is HDR10 (PQ); by default it is not told ([how-it-works.md](how-it-works.md#frame-generation-ffxfgffx_xefgcpp-amd_fidelityfx_framegeneration_dx12dll)). |
| `IGDEXT_XELL_CAP`, `IGDEXT_XELL_CHECK`, `IGDEXT_XELL_POLLS` | `amd_fidelityfx_framegeneration_dx12.dll` | The low latency's switches. This DLL reads them as the shim does (see the shim's table above). |
| `FFX_XEFG_EXCLUSIVE=1` | `amd_fidelityfx_framegeneration_dx12.dll` | Lets exclusive fullscreen requests through instead of keeping the swap chain windowed (XeSS-FG then refuses to interpolate). |
| `FFX_XEFG_VENDOR=0` | `amd_fidelityfx_framegeneration_dx12.dll` | Disables the `IDXGIAdapter::GetDesc*` hook that reports `0x8086` to `libxess_fg.dll` and `libxell.dll` when DXVK spoofs another vendor ID. |
| `FFX_XEFG_OFLOW=1` | `amd_fidelityfx_framegeneration_dx12.dll` | Computes motion vectors from FidelityFX optical flow on the final backbuffer (`OFLOW_FG=1` sets this). |
| `FFX_XEFG_OFLOW_DIV=1..4` | `amd_fidelityfx_framegeneration_dx12.dll` | Runs optical flow at `1/n` of the display size (default `1`). |
| `FFX_XEFG_OFLOW_DEPTH=<d>` | `amd_fidelityfx_framegeneration_dx12.dll` | Constant depth value written by `oflow_expand.hlsl` (default `1.0`). |
| `FFX_XEFG_CAP=auto\|<us>\|0` | `amd_fidelityfx_framegeneration_dx12.dll` | Real-frame interval cap via XeLL (`auto` is default when `FFX_XEFG_OFLOW=1`, `<us>` sets a fixed interval in microseconds, `0` disables). |
| `FFX_XEFG_CAP_PAD=<ms>` | `amd_fidelityfx_framegeneration_dx12.dll` | Initial estimate in milliseconds of per-frame time beyond the measured GPU time for `FFX_XEFG_CAP=auto` (default `1.5`, then learned). |
| `FFX_XEFG_REFRESH=<Hz>` | `amd_fidelityfx_framegeneration_dx12.dll` | Overrides the display refresh rate for `FFX_XEFG_CAP=auto` when Windows reports a wrong one (default from `EnumDisplaySettingsA`, else `144`). |
| `FFX_XEFG_TIMES=1` | `amd_fidelityfx_framegeneration_dx12.dll` | Logs frame time, time in `Present`, and `xellSleep` percentiles every 300 frames, plus optical-flow and expansion GPU times every 300 runs. Every 300 rendered frames it also logs the gaps between the shown frames' `Present` calls on the real swap chain and the longest / shortest gap per rendered frame (`shown frames over ...`). |

#### `FFXFG_DEBUG=1` Build-Only Switches (`ffxfg/build.bat` with `FFXFG_DEBUG=1`)

Available only when `ffxfg/` is compiled with `/DFFXFG_DEBUG` ([building.md](building.md)). A release build does not read them: its code for them is left out by the compiler. `FFX_XEFG_SLEEP`, `FFX_XEFG_OWNWAIT`, `FFX_XEFG_PACE`, `FFX_XEFG_AB` / `FFX_XEFG_AB_OLD`, `FFX_XEFG_HOLD`, `FFX_XEFG_VALIDITY` and `FFX_XEFG_MVNDC` were read by every build in releases up to 20261113:
* `FFX_XESS_JITTER=<f>`: multiplies upscaler jitter offsets by `<f>` (`-1` negates jitter).
* `FFX_XEFG_CAMERA=0..6`: `0` identity view and projection matrices; `1` normal (default); `2` transposed; `3` projection not depth-inverted; `4` both transposed and not inverted; `5` all zero; `6` ignores camera vectors (fixed view, real projection).
* `FFX_XEFG_JITTER=<n>`: multiplies frame generation jitter offsets by the integer `<n>` (`0` disables, `-1` negates).
* `FFX_XEFG_RENDERTIME=<ms>`: overrides `frameRenderTime` with `<ms>`.
* `FFX_XEFG_RENDERSCALE=<f>`: multiplies the game's `frameTimeDelta` by `<f>` (`0.001` = seconds).
* `FFX_XEFG_DUMP=<n>`: dumps depth and motion vectors of two consecutive frames every `<n>` frames to `ffx_xefg_<frame>_depth.bin` and `ffx_xefg_<frame>_mv.bin` (up to 16 dumps total, 8 pairs).
* `FFX_XEFG_ORDER=1`: logs the order of `prepare`, `configure`, and `present` calls with their frame IDs for the first 300 calls after interpolation starts.
* `FFX_XEFG_TRACE=1`: writes the time line of every frame to `ffx_xefg_trace.txt` next to the DLL (`ffxfg/ffx_trace.h`, `ffx_trace.cpp`). `tools/fsr-trace.py` reads it.
* `FFX_XEFG_SLEEP=0`: skips calling `xellSleep` (XeLL stays enabled, as XeSS-FG requires it).
* `FFX_XEFG_OWNWAIT=0`: hands the game the XeSS-FG proxy swap chain's own frame-latency waitable object instead of the DLL's semaphore (a game that waits on it before each frame can deadlock, for example CONTROL Resonant).
* `FFX_XEFG_PACE=1`: releases the DLL's waitable semaphore when a GPU fence signaled after `Present` completes (with a 200 ms watchdog), instead of when `Present` returns. It showed no gain where it was measured (CONTROL Resonant at 6x).
* `FFX_XEFG_AB=<s>`: measuring aid: the low latency's wait is off and on in turns of `<s>` seconds (the log says when). With `FFX_XEFG_AB_OLD=1`, three turns: as before, answers only, answers and wait.
* `FFX_XEFG_HOLD=<ms>`: measuring aid: a fixed wait in `Present` instead of the one the low latency works out. It is not checked (`IGDEXT_XELL_CHECK`).
* `FFX_XEFG_VALIDITY=0`: measuring aid: tags depth and motion vectors `UNTIL_NEXT_PRESENT` instead of `ONLY_NOW` (avoids the copy at prepare to measure its cost; can flicker if the game overwrites them before present).
* `FFX_XEFG_HDR10_AB=<s>`: measuring aid: XeSS-FG is told and not told about an HDR10 back buffer in turns of `<s>` seconds, told first (`ffx_xefg.log` has the time of each change).
* `FFX_XEFG_MVNDC=1`: sets `XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY` and scales motion vectors to NDC (`2 / width`, `-2 / height`).
* `FFX_XEFG_TRACE_SUBS=1` (with `FFX_XEFG_TRACE=1`): also notes when the GPU finished each submission to a D3D12 queue. It signals a fence after every `ExecuteCommandLists`, so it is a little slower. `tools/fsr-trace.py --subs` lists them.

### Scripts (Shell Environment When Running Them)

| Script | Arguments / Variables |
|---|---|
| `install.sh`, `uninstall.sh` | `STEAM_DIR` (default: the first of `~/.local/share/Steam`, `~/.steam/steam`, `~/.steam/root`, `~/.steam/debian-installation` that holds a Steam install); `install.sh` only: `XMX_DRIVER=auto\|patched\|stock` (session driver mode, saved in `run/driver-mode`; `stock` is accepted on any GPU if `lib/mesa-stock-*` and `layer/libVkLayer_xess_xmx.so` exist, though on Xe-HPG and Xe2 the layer declines its self-test and XeSS runs on DP4a), `XMX_NO_ENV=1` (no session environment and no session check; the shim, hook, and watcher stay), `XMX_WAIT_PREFIX=<s>` (used by `xess-xmx-shim.service`). |
| `enable.sh`, `disable.sh` | `XMX_APPID` or `XMX_PREFIX` (one is required), `XMX_CONF` (default `~/.config/xess-xmx.conf`, read by `xmx-launch.sh`), `XMX_GAME` (refuse to run while that process runs), `STEAM_DIR`. |
| `xmx-stock.sh` | `XMX_STOCK_ICD=<json>[:<json>]` (custom Intel ICD(s) to use instead of the system's), `XMX_ICD_DIRS` (colon-separated ICD search directories, for tests). |
| `tools/switch-driver.sh [version]` | No argument: lists the package's drivers (`*` = default in `lib/active`); with a version or prefix: tests it and makes it the default. |
| `tools/rebuild-driver.sh [version]` | Version defaults to the system's Mesa; `XMX_MESA_DIR` (default `~/.cache/xess-xmx/mesa-src`) holds the source and build trees; `XMX_MESA_SHA256` provides the tarball SHA-256 for versions other than `26.1.2` and `26.3.0`. |
| `tools/session-check.sh [--dry-run]` | Run by `xess-xmx-check.service`; `--dry-run` only reports (and fills `lib/deps/`); `XMX_CHECK_LIB` / `XMX_CHECK_ICD` test another driver library or ICD file. |
| `tools/get-xess.sh [package dir]` | Downloads Intel's XeSS SDK libraries for `XESS=auto` and `XESS=latest` into `xess/` (run by `install.sh`); `XESS_TAG` (default `v3.0.2`) and `XESS_SHA256` select another release. |
| `tools/get-ffx.sh [package dir]` | Downloads AMD's FidelityFX loader and providers into `ffx/` (run by `install.sh`), for FSR 3.1.0 to 3.1.3. |
| `tools/fsr-fg.sh all\|on\|off\|status <app id \| game folder>` | Persistent FSR-on-XeSS file swap in folders that contain `amd_fidelityfx_framegeneration_dx12.dll` (or the FSR 3.1.0–3.1.3 single-DLL `amd_fidelityfx_dx12.dll`), for Protons without the `user_settings.py` hook ([../frame-generation.md](../frame-generation.md)). |
| `tools/get-igc.sh [package dir]` | `IGC_TAG`, `NEO_TAG` select other compiler releases (defaults: IGC `v2.10.10`, compute-runtime `25.18.33578.6`; others are untested). |
| `tools/cm-compile.sh` | Called by the driver and layer; `XMX_IGC_DIR` (default `<package>/igc`), `CM_DEVICE` (`ocloc -device`, overrides the detection), `CM_LOG` (default `~/.cache/xess-xmx/compile.log`). The driver and layer also set `CM_PCI_ID`, `CM_GRF_BYTES`, and `CM_OUT_DIR` (`ocloc`'s output folder; the driver/layer packs the `.cmk`/`.cml` itself), and the layer sets `CM_NO_LOAD_PAYLOAD=1`. Run by hand without `CM_OUT_DIR`, it packs with `tools/cmk_pack.py` (which reads `CMK_GC` and `CMK_GRF_BYTES`). |
| `tools/xell-probe.sh` | `XELL_PROBE_VERSION=<n>` (default `1`, exported as `IGDEXT_XELL_PROBE`), `XELL_PROBE_TELEMETRY=1` (exports `IGDEXT_XELL_TELEMETRY=1`), `XELL_PROBE_KEEP=1` (keeps raw logs in the prefix after creating the archive). |
