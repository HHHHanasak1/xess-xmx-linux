# Debugging and troubleshooting

## Symptoms

| What you see | Likely cause | Check |
|---|---|---|
| Image looks like the DP4a path (softer, no change after enabling) | The shim is not loaded (Proton replaced it, new prefix) | `IGDEXT_TRACE=1` produces no `C:\igdext_trace.log` in the prefix; re-run `install.sh` |
| Patches of white noise or black areas in the XeSS output | A kernel ran as a no-op (compile failed or missing) | `~/.cache/xess-xmx/compile.log`, `drive_c/igdext_kernels/compile_failed.txt`; the next launch falls back to DP4a by itself |
| XeSS is back to DP4a after a failed compile | Intended fallback after `compile_failed.txt` was written | the driver retries the listed kernels at every launch and lifts the fallback once they compile; a missing compiler: `tools/get-igc.sh` |
| XMX gone after a system update, `driver-status` says FAILED | the patched driver cannot be loaded any more (library dependencies changed); the session check switched to the stock drivers | update the package / rebuild the driver; `journalctl --user -u xess-xmx-check` |
| Generated frames are black, flicker between black and image | XeFG got the Xe-HPG kernel variant (only with `IGDEXT_OPTIONS2_FG=0,...`) | remove the override |
| 30 fps with frame generation until you open and close the pause menu (Wuthering Waves) | The game's XeLL frame cap; the shim fixes it | `IGDEXT_XELL_LOG=1`: `frame cap ... keeping` lines |
| The first launch hangs for a minute or two at XeSS initialisation | Every kernel is compiled once (about 1 s each) | `ANV CM: compiling kernel` lines in the game's stderr; only once per machine and XeSS version |
| Another GPU (NVIDIA / AMD) disappeared from Vulkan | an old `install.sh` wrote only the Intel driver into `VK_DRIVER_FILES` | re-run the current `install.sh`, re-login |
| 32-bit Vulkan games fail to start | same (the old `install.sh` left the 32-bit Intel driver out) | same |

## Logs

* Shim: `IGDEXT_TRACE=1` writes `C:\igdext_trace.log` inside the prefix (every extension call, the detected GPU, the
  feature answers, each kernel with its id, and with `callers(...)` the module chain that made the call, so SR
  (`libxess.dll`) and frame generation (`libxess_fg.dll`) kernels can be told apart) and dumps every kernel to
  `C:\igdext_dump`.
* XeLL: `IGDEXT_XELL_LOG=1` writes `C:\igdext_xell.log` (what the game passes to XeLL, the real frame rate from
  `xellSleep` timing, XeLL's own messages).
* Driver: `ANV_CM_DEBUG=1` logs each placeholder and injection to the game's stderr; `ANV_CM_TRACE=1` logs every
  dispatch (`ANV CM: walker kernel x,y,z ...`), `=2` also the surface states. To get the game's stderr, put
  `exec 2>>/some/file` into the launch wrapper's config.
* Compiler: `~/.cache/xess-xmx/compile.log` (`CM_LOG` overrides; inside the Steam runtime container `$XDG_RUNTIME_DIR`
  is private, so the log lives in the cache folder).
* Session check: `~/.cache/xess-xmx/driver-status`, `journalctl --user -u xess-xmx-check`.

Counting dispatches per kernel from a `ANV_CM_TRACE=1` log shows what runs every frame:

    grep -a -o "walker kernel [0-9]*,[0-9]*,[0-9]*" stderr.log | sort | uniq -c

## Switches

Shim (game environment):

| Variable | Effect |
|---|---|
| `IGDEXT_TRACE=1` | trace log and kernel dump (above) |
| `IGDEXT_STATIC=1` | use the prebuilt kernel table (`kernels/wg*_5.cmk`) instead of run-time ids; only in shims built with `-DXMX_STATIC_TABLE=ON` |
| `IGDEXT_IGNORE_FAILED=1` | do not fall back to DP4a after a failed compile |
| `IGDEXT_OPTIONS1=<xmx>,<dlboost>,<emul64>` | override `CheckFeatureSupport(OPTIONS1)` |
| `IGDEXT_OPTIONS2=<simd16>,<lsc>,<legacy>` | override `OPTIONS2` (default from the detected GPU: `1,1,0` on Xe2/Xe3) |
| `IGDEXT_OPTIONS2_FG=...` | the same, only for calls from `libxess_fg.dll` |
| `IGDEXT_GMD=<arch>,<rel>`, `IGDEXT_GTGEN`, `IGDEXT_GTNAME`, `IGDEXT_EUS=<eus>,<cores>` | override the reported device |
| `IGDEXT_XELL_LOG=1` | XeLL log (above) |
| `IGDEXT_XELL_FIXCAP=1` / `IGDEXT_XELL_KEEPCAP=1` | apply the XeLL frame-cap fix in any game / nowhere (default: Wuthering Waves only) |
| `IGDEXT_XELL_PROBE=<version>` | experimental: claim XeLL driver-mode support and log XeLL's use of it (see how-it-works.md) |

Driver (patch 0001):

| Variable | Effect |
|---|---|
| `ANV_CM_KERNEL_DIR` | folder of static kernels (set by install.sh; may be empty) |
| `ANV_CM_HELPER` | compiler helper (default `$ANV_CM_KERNEL_DIR/../tools/cm-compile.sh`) |
| `ANV_CM_SPV_DIR` | where the shim's SPIR-V is (default `$WINEPREFIX/drive_c/igdext_kernels`) |
| `ANV_CM_DEBUG=1`, `ANV_CM_TRACE=1\|2` | logs (above) |

Driver debug tools (patch 0002, not in release builds unless noted):

| Variable | Effect |
|---|---|
| `ANV_CM_PC=1` | print the push constants of each dispatch (with `ANV_CM_TRACE`) |
| `ANV_CM_CAPTURE="x,y:N;..."` | CPU-side dump of every buffer kernel x,y reads at its N-th dispatch into `ANV_CM_CAP_DIR` (default `/tmp/anv_cm_cap`) |
| `ANV_CM_GCAP="x,y:N;..."` | GPU-side copy of every small (<= 4 KiB) buffer right before the dispatch, same folder |
| `ANV_CM_VIEW="x,y:s=X,Y:S;..."` | let surface s of kernel x,y see surface S of kernel X,Y (probe kernels) |
| `ANV_CM_ONLY="x,y ..."`, `ANV_CM_SKIP="x,y ..."` | inject only / never these kernels |
| `ANV_CM_SYNC=1` | full flush and stall around every kernel dispatch |
| `ANV_CM_MAXGROUPS=<n>`, `ANV_CM_PREEMPT=0\|1` | clamp the dispatch size / force thread preemption |

The release driver is built with both patches, so every switch above is available.
