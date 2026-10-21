# Debugging and troubleshooting

## Symptoms

| What you see | Cause | What to do |
|---|---|---|
| Looks like the DP4a path; `IGDEXT_TRACE=1` writes no `C:\igdext_trace.log` | The shim is not loaded (new prefix, Proton replaced it) | re-run `install.sh` |
| DP4a and often no XeSS frame generation option; no trace file; the Proton log says `XeSS: hiding Intel GPU Vendor ID` (Cyberpunk 2077) | The game ships XeSS older than 2.0.2.68, and DXVK then reports the GPU as an AMD one | replace the game's XeSS libraries: [games.md](games.md) |
| Super resolution on DP4a while frame generation uses the shim (Spider-Man Remastered, Hogwarts Legacy, The Witcher 3, Hitman 3, Diablo IV, ...) | Mesa's profile for that game sets `force_vk_vendor=-1`: the game sees no Intel GPU | `install.sh` / `enable.sh` set `force_vk_vendor=0` (re-run them after an update). By hand: `force_vk_vendor=0 %command%`. Mesa's behaviour back for one game: `force_vk_vendor=-1 %command%` |
| DP4a, the trace says `fallback: self-test failed` | The patched driver is not active in the game process (session check fell back, variables missing), or vkd3d-proton changed how placeholders reach the driver | `driver-status`, `VK_DRIVER_FILES` in the game's environment; the patched driver writes `drive_c/igdext_kernels/anv_canary` when it sees the test |
| Patches of white noise or black areas in the XeSS output | A kernel failed to compile in this launch and ran as a no-op | `~/.cache/xess-xmx/compile.log`; a missing compiler: `tools/get-igc.sh`. The next launch retries |
| DP4a after a failed compile | Intended: `drive_c/igdext_kernels/compile_failed.txt` exists | the driver retries once at the next launch; after a second failure only a graphics component update, a game update, deleting the marker or running `install.sh` start another attempt |
| XMX gone after a system update, `driver-status` says FAILED | Neither of the package's drivers can be loaded any more (if only the active one broke, the session check switched to the other); the session uses the stock drivers | `tools/rebuild-driver.sh` ([building.md](building.md)); `journalctl --user -u xess-xmx-check` |
| Games find no Intel GPU (or `driver-status` says FAILED) with `cannot open shared object file` for `libSPIRV-Tools.so` or `libdisplay-info.so.3`; the Proton log has `xess-xmx: cannot load the Mesa ... driver` | The driver needs both libraries, and the Steam Linux Runtime container that runs Proton games has neither: they reach it only as dependencies of the host's other Vulkan drivers. Some distributions name SPIRV-Tools differently (`libSPIRV-Tools-shared.so` on Void Linux, `libSPIRV-Tools-<version>.so` on openSUSE) | The session check copies both into `lib/deps/` (the driver's run path); re-run `install.sh` (`enable.sh` for one game) or log in again. `ls lib/deps/` must list both; a system without them gets the copies the archive bundles in `lib/bundled/` |
| `undefined symbol: wl_fixes_interface` in `driver-status` or the Proton log | The system's libwayland-client is older than 1.24 (Debian 13, Ubuntu 24.04); the drivers in the archive are built against 1.24 | build the driver on the system: `tools/rebuild-driver.sh` ([building.md](building.md)) |
| Generated frames are black or flicker between black and image | XeFG got the Xe-HPG kernel variant (only with `IGDEXT_OPTIONS2_FG=0,...`) | remove the override |
| 30 fps with frame generation until the pause menu is opened and closed (Wuthering Waves) | The game's XeLL frame cap; the shim fixes it | `IGDEXT_XELL_LOG=1`: `frame cap ... keeping` lines |
| The first launch hangs for a minute or two at XeSS initialisation | Every kernel is compiled once | `ANV CM: compiling kernel` lines in the game's stderr |
| After updating from an older release: no image or a GPU hang with ray tracing; another GPU or 32-bit games without Vulkan; XMX gone in the games of one Proton after it updated | Bugs of older releases, fixed since | install the current release, run `install.sh`, re-login |

## Logs

* Shim: `IGDEXT_TRACE=1` writes `C:\igdext_trace.log` inside the prefix (every extension call, the detected GPU, the
  feature answers, each kernel with its id, and with `callers(...)` the module chain that made the call, so SR
  (`libxess.dll`) and frame generation (`libxess_fg.dll`) kernels can be told apart) and dumps every kernel to
  `C:\igdext_dump`.
* XeLL: `IGDEXT_XELL_LOG=1` writes `C:\igdext_xell.log` (what the game passes to XeLL, the real frame rate from
  `xellSleep` timing, XeLL's own messages).
* Driver: `ANV_CM_DEBUG=1` logs each placeholder and injection to the game's stderr; `ANV_CM_TRACE=1` logs every
  dispatch (`ANV CM: walker kernel x,y,z ...`), `=2` also the surface states. To get the game's stderr, put
  `exec 2>>/some/file` into the launch wrapper's config, or use `PROTON_LOG=1`.
* Compiler: `~/.cache/xess-xmx/compile.log` (`CM_LOG` overrides).
* Session check: `~/.cache/xess-xmx/driver-status`, `journalctl --user -u xess-xmx-check`.

Counting dispatches per kernel from a `ANV_CM_TRACE=1` log shows what runs every frame:

    grep -a -o "walker kernel [0-9]*,[0-9]*,[0-9]*" stderr.log | sort | uniq -c

## XeLL driver-mode probe

XeLL runs in cross-vendor mode under Proton ([how-it-works.md](how-it-works.md) §4). To find out what it would need
for driver mode, run one launch of a game with XeSS frame generation through the probe wrapper. Steam launch options:

    /path/to/package/tools/xell-probe.sh %command%

Play for a minute with frame generation on (the Wuthering Waves title screen with FG enabled is enough), quit, and
look at the summary the wrapper prints; it also writes `~/xess-xmx-probe-<date>.tar.gz` with `libxell.log` (XeLL's own
log: where it loaded the extension library from, the driver version it parsed, `Latency reduction enabled` or
`... - Cross Vendor`), `igdext_trace.log` (`XELLPROBE` lines: every latency-reduction call with its arguments, each
`RenderSubmitStart` with the time since the previous one, and every read or write XeLL makes to the timing buffers
with the slot, the in-slot offset and the code address in `libxell.dll`), `igdext_xell.log` and the session's
driver status. Attach the archive to the issue.

How to read the first result:

* `libxell.log` says `Cannot load Intel Extensions library` or shows no `Loading Intel Extensions` line: XeLL did not
  find the shim. Check that `igdext64.dll` is in the prefix's `windows/system32` or wherever
  `INTC_ALT_DRIVER_EXTENSIONS_PATH` points (GE-Proton sets it to `C:\Windows\System32`).
* `Intel Extensions initialized` but `Latency reduction enabled - Cross Vendor` and no `XELLPROBE
  LatencyReductionExt` line: XeLL rejected the shim before enabling; the usual suspects are the driver version in the
  extension info and `GetLatencyReductionStatus.driverSupportedVersion`. Retry with `XELL_PROBE_VERSION=2` (and 3).
* `XELLPROBE LatencyReductionExt(...)` followed by buffer accesses: driver mode started. The access log gives the
  slot size and the fields; a read of `gpu +0x0` followed by a crash means XeLL treats the GPU buffer as a COM object
  (an `ID3D12Resource` it maps), not as plain memory, which the next shim version must then provide.
* `GetDisplayTelemetry` called and then cross-vendor: retry with `XELL_PROBE_TELEMETRY=1`.

## Switches

Shim (game environment):

| Variable | Effect |
|---|---|
| `IGDEXT_TRACE=1` | trace log and kernel dump (above) |
| `IGDEXT_STATIC=1` | use the prebuilt kernel table (`kernels/wg*_5.cmk`) instead of run-time ids; only in shims built with `-DXMX_STATIC_TABLE=ON` |
| `IGDEXT_IGNORE_FAILED=1` | do not fall back to DP4a after a failed compile |
| `IGDEXT_SKIP_SELFTEST=1` | skip the self-test (the shim then assumes the patched driver is active) |
| `IGDEXT_FORCE_FALLBACK=1` | decline the extension context: XeSS uses DP4a, XeSS frame generation its generic path (as without the shim) |
| `IGDEXT_OPTIONS1=<xmx>,<dlboost>,<emul64>` | override `CheckFeatureSupport(OPTIONS1)` |
| `IGDEXT_OPTIONS2=<simd16>,<lsc>,<legacy>` | override `OPTIONS2` (default from the detected GPU: `1,1,0` on Xe2/Xe3) |
| `IGDEXT_OPTIONS2_FG=...` | the same, only for calls from `libxess_fg.dll` |
| `IGDEXT_GMD=<arch>,<rel>`, `IGDEXT_GTGEN`, `IGDEXT_GTNAME`, `IGDEXT_EUS=<eus>,<cores>` | override the reported device |
| `IGDEXT_XELL_LOG=1` | XeLL log (above), including the average simulation-start -> present-end latency from XeLL's frame reports every 100 frames |
| `IGDEXT_XELL_FIXCAP=1` / `IGDEXT_XELL_KEEPCAP=1` | apply the XeLL frame-cap fix in any game / nowhere (default: Wuthering Waves only) |
| `IGDEXT_XELL_PROBE=<version>` | claim XeLL driver-mode support (interface version), accept the Set* pacing calls, and log XeLL's use of it (`tools/xell-probe.sh`, "XeLL driver-mode probe" above) |
| `IGDEXT_XELL_TELEMETRY=1` | with the probe: answer `GetDisplayTelemetry` with a zeroed buffer instead of E_NOTIMPL |

Driver choice: `MESA=<version>` (a prefix such as `26.1` is enough) makes a game use that driver of the package
instead of the default; the game's stderr then says `xess-xmx: using the Mesa ... driver`.

Driver (patch 0001):

| Variable | Effect |
|---|---|
| `ANV_CM_KERNEL_DIR` | folder of static kernels (set by install.sh; may be empty) |
| `ANV_CM_HELPER` | compiler helper (default `$ANV_CM_KERNEL_DIR/../tools/cm-compile.sh`) |
| `ANV_CM_SPV_DIR` | where the shim's SPIR-V is (default `$WINEPREFIX/drive_c/igdext_kernels`) |
| `ANV_CM_DEBUG=1`, `ANV_CM_TRACE=1\|2` | logs (above) |

Driver debug tools (patch 0002; the release driver includes it):

| Variable | Effect |
|---|---|
| `ANV_CM_PC=1` | print the push constants of each dispatch (with `ANV_CM_TRACE`) |
| `ANV_CM_CAPTURE="x,y:N;..."` | CPU-side dump of every buffer kernel x,y reads at its N-th dispatch into `ANV_CM_CAP_DIR` (default `/tmp/anv_cm_cap`) |
| `ANV_CM_GCAP="x,y:N;..."` | GPU-side copy of every small (<= 4 KiB) buffer right before the dispatch, same folder |
| `ANV_CM_VIEW="x,y:s=X,Y:S;..."` | let surface s of kernel x,y see surface S of kernel X,Y (probe kernels) |
| `ANV_CM_ONLY="x,y ..."`, `ANV_CM_SKIP="x,y ..."` | inject only / never these kernels |
| `ANV_CM_SYNC=1` | full flush and stall around every kernel dispatch |
| `ANV_CM_MAXGROUPS=<n>`, `ANV_CM_PREEMPT=0\|1` | clamp the dispatch size / force thread preemption |
