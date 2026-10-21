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

XeLL runs in cross-vendor mode under Proton ([how-it-works.md](how-it-works.md) §4). The reason is in `libxell.dll`
itself (read from 1.3.2.10, the SDK copy; `strings libxell.dll | grep wine_get_version` shows whether the 1.3.0.5 that
games ship has the same check): before anything else it
checks `GetProcAddress(GetModuleHandle("ntdll.dll"), "wine_get_version")`, and if that export exists it uses its
built-in context without ever loading the extension library. With the export hidden it loads `igdext64.dll`, asks for
extension version 5/13 or 4/13, checks the driver version string (>= 31.0.101.6252) and `GTGeneration`, which must be
one of Intel's PRODUCT_FAMILY ids (1300 for Panther Lake, 1275 Lunar Lake, 1274 Battlemage, 1270 Alchemist, ...; the
shim normally reports 30 there), and only then calls the latency-reduction functions. Details and offsets:
`shim/src/dll/XellDriver.cpp`.

`IGDEXT_XELL_PROBE=<version>` turns driver mode on for one launch: the shim hides `wine_get_version` from
`libxell.dll` (an import-table hook in that DLL only), reports the family id, claims that interface version, and then
provides what XeLL reads (the layout, worked out on a B390 and from the DLL's code, is in `XellDriver.cpp`):

* per `RenderSubmitStart`: the CPU time of the submit (QPC in nanoseconds, XeLL's own clock) and the GPU time at
  which the GPU began the frame: a timestamp query the shim executes in front of the frame's first
  `ExecuteCommandLists` from the game (it hooks that call in the command-queue vtable), with the GPU clock at submit
  from `GetClockCalibration` as the provisional value until the query lands (`XELLPROBE gpu start frame N: measured
  ... (+N ticks after submit)` lines; the offset is the queue depth XeLL now sees);
* display telemetry: a vsync model from the current display mode (`IGDEXT_XELL_HZ=<hz>`, or `IGDEXT_XELL_VRR=<min>,<max>`
  for a VRR range, `IGDEXT_XELL_TEARING=1` for vsync off), and one record per `SetPresentSequenceNumber` call (flip
  queued = the call, shown = the next modelled vsync).

Steam launch options:

    /path/to/package/tools/xell-probe.sh %command%

Play for a minute with frame generation on (the Wuthering Waves title screen with FG enabled is enough), quit, and
look at the summary the wrapper prints; it also writes `~/xess-xmx-probe-<date>.tar.gz` with `proton-xell.log` (XeLL's
own messages, which it sends through `OutputDebugString`; the wrapper sets `PROTON_LOG=1` and keeps the XeLL lines of
`~/steam-<appid>.log`), `libxell.log` if that libxell version writes one (1.3.2+), `igdext_trace.log` (`XELLPROBE`
lines: the hook, the display model, every latency-reduction call with its arguments, each `RenderSubmitStart` with the
times it stored), `igdext_xell.log` (with `xell latency:` lines, XeLL's own simulation-start-to-present-end
measurement every 100 frames: the number to compare against a run without the probe) and the session's driver status.
Attach the archive to the issue.

How to read the result:

* No `XELLPROBE libxell.dll ...: GetProcAddress import hooked` line: the shim never saw `libxell.dll` load. Either the
  shim is not loaded (`install.sh`), or the game created its XeLL context before the shim was loaded: toggle frame
  generation off and on in the game's menu, which recreates the context, and check again.
* `[XeLL] Using built-in context` and no `Intel Extensions` lines although the hook is in place: XeLL still did not
  take the Intel path. Check the DXGI vendor id the game sees (`force_vk_vendor=0`, [games.md](games.md)) and
  whether `XELL_FORCE_CROSS_VENDOR` is set.
* `XeLL context creation failed - unsupported device` / `unsupported driver`: the family id or the driver version
  string the shim reported was rejected. `IGDEXT_FAMILY=<id>` overrides the family id.
* `Display telemetry corrupted.` / `Failed to get display telemetry.`: the telemetry block was rejected or not
  provided (`IGDEXT_XELL_TELEMETRY=0`); `Display/FramStats match not found.`: the present sequence numbers XeLL got
  from the shim's records do not match its frame ids.
* `Driver timings not available for last visible frame.` every frame: XeLL found zero slots; the `RenderSubmitStart`
  lines show what was stored and in which slot.
* `xell latency:` lower than the same scene without the probe: driver mode is paying off. Higher: compare with
  `IGDEXT_XELL_GPUSTART=0` (submit-time GPU clock) and `IGDEXT_XELL_TELEMETRY=0` (no telemetry: XeLL paces nothing,
  which is the cross-vendor behaviour) to see which input misleads it.
* No `timestamp query queued ahead of frame` lines: the game's first submit after `RenderSubmitStart` was not seen
  (not a direct queue, or the call came through a module the hook treats as XeSS's own); the provisional value stays.
* `IGDEXT_XELL_GUARD=1` logs every read and write XeLL makes to the timing buffers with the slot, the offset and the
  code address in `libxell.dll`, for a new XeLL version.

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
| `IGDEXT_XELL_PROBE=<version>` | hide `wine_get_version` from `libxell.dll`, report the Intel product family as `GTGeneration`, claim XeLL driver-mode support (interface version), accept the Set* pacing calls, and log XeLL's use of it (`tools/xell-probe.sh`, "XeLL driver-mode probe" above) |
| `IGDEXT_FAMILY=<id>` | override the product family id reported with the probe (default from the detected GPU) |
| `IGDEXT_XELL_TELEMETRY=0` | with the probe: answer `GetDisplayTelemetry` with E_NOTIMPL instead of the display model |
| `IGDEXT_XELL_HZ=<hz>`, `IGDEXT_XELL_VRR=<min>,<max>`, `IGDEXT_XELL_TEARING=1` | the display model (default: the current mode's refresh rate, vsync on) |
| `IGDEXT_XELL_GUARD=1` | with the probe: timing buffers on guard pages, every access logged |
| `IGDEXT_XELL_GPUSTART=0` | with the probe: keep the GPU clock at submit as the frame's GPU start instead of measuring it with a timestamp query ahead of the frame's first `ExecuteCommandLists` |

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
