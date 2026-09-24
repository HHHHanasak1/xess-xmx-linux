# XeSS XMX on Linux (Proton) for Intel Xe2 / Xe3

Real Intel **XeSS** super resolution and **XeSS frame generation** inside Proton games, running on the GPU's XMX
(matrix) units instead of the DP4a fallback that stock Wine/Proton gets. No Windows driver, no change to Proton or
vkd3d-proton:

* `igdext64.dll`, a shim built on Intel's open source [dxvk-igdext](https://github.com/GameTechDev/dxvk-igdext), answers
  XeSS' driver queries like an Intel driver and hands every C-for-Metal (CM) kernel XeSS submits over as a placeholder;
* a patched Mesa **ANV** Vulkan driver compiles those kernels with Intel's compiler (IGC) on first use and runs them in
  place of the placeholders, inside the normal vkd3d-proton queue.

Developed and tested on an ONEXPLAYER 3 (Intel Core Ultra "Panther Lake", Arc B390, Xe3) with SteamOS, Mesa 26.1.2 and
GE-Proton 11-6 / Proton Experimental. XeSS super resolution renders correctly and is temporally stable; frame
generation runs at 2x/3x/4x with its network on XMX as well. Without the Intel extension (stock Proton, or this
project's fallback) XeSS runs its DP4a super resolution and a generic frame generation that is limited to 2x; with it,
the game's 3x/4x multi-frame generation works (see "Performance").

## Install

Download the release archive, unpack it somewhere it can stay, and run:

    ./install.sh     # once; fetches the compiler bundle (about 230 MB), then reboot or log out and in
    ./uninstall.sh   # reverts everything

That is all: every game started from Steam afterwards uses the patched driver, no launch options needed. In the game,
select XeSS (and XeSS frame generation) on its **DX12** renderer. The first launch of a game (more exactly: of an XeSS
version) compiles its kernels, a minute or two once; after that they come from `~/.cache/xess-xmx`.

What `install.sh` does:

* writes `~/.config/environment.d/50-xess-xmx.conf`: the patched driver as the 64-bit Intel Vulkan driver (every other
  driver, including other GPUs and the 32-bit Intel driver, stays in the list), the kernel compiler, and
  `VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer`, which the kernel binding code needs (it applies to every D3D12
  game of the session; see "Performance" below);
* puts the shim into every Proton that bundles an `igdext64.dll` (Proton Experimental copies it into each prefix at
  launch) and into every existing prefix, keeping the original as `.stock`;
* enables a user path unit that puts the shim back after a Proton update and into the prefix of a game started for
  the first time;
* enables `xess-xmx-check.service`, which loads the patched driver and lets `vulkaninfo` find the GPU through it
  before every graphical session. If a system update broke it (library dependencies, kernel driver), the session is
  pointed back to the stock drivers: everything keeps working, XeSS runs as DP4a (instead of the whole session losing
  the Intel GPU). Result in `~/.cache/xess-xmx/driver-status`, which also notes when the system's Mesa version differs
  from the patched driver's; `tools/rebuild-driver.sh` then rebuilds it for the new version.

(XeSS would also accept the shim's folder through
`INTC_ALT_DRIVER_EXTENSIONS_PATH`, which would make the per-prefix copies unnecessary, but GE-Proton sets that variable
to `C:\Windows\System32` for every game, so the copies stay.) It supports the native Steam client (SteamOS, distribution packages);
Flatpak Steam is not supported (its sandbox sees neither the environment file nor the package folder).

**Per game instead of system-wide:** `XMX_APPID=<steam app id> ./enable.sh` (or `XMX_PREFIX=<prefix>` for other
launchers) installs the shim into that prefix and writes the variables to `~/.config/xess-xmx.conf`; put
`/path/to/package/xmx-launch.sh %command%` into the game's launch options. `disable.sh` undoes it.

### Requirements

* Intel Xe3 (Panther Lake, tested) or Xe2 (Lunar Lake, Battlemage: untested, enabled by default) on the `xe` or `i915`
  kernel driver. Xe-HPG (Alchemist) and Xe-LPG are detected and get the answers a Windows driver would give, but
  nothing about them has been tested.
* The driver in the archive is Mesa 26.1.2 with the patches (ray tracing enabled, like the stock driver); it replaces
  the system's 64-bit ANV only for the user session. For another Mesa version, `tools/rebuild-driver.sh` rebuilds it
  (see [docs/building.md](docs/building.md)).
* vkd3d-proton as in Proton 11 / GE-Proton 11.

## Games

See [docs/games.md](docs/games.md): Wuthering Waves (SR + FG), Forza Horizon 6 (SR; FG through OptiScaler), notes on
OptiScaler and on a game without XeSS.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| looks like DP4a, nothing changed | the shim is not in the prefix: re-run `install.sh` (new prefix, Proton update without the watcher) |
| noise or black patches in the image | a kernel failed to compile; the next launch retries once and uses the generic paths if it fails again. See `~/.cache/xess-xmx/compile.log`; a missing compiler: `tools/get-igc.sh` |
| XMX gone after a SteamOS update, game mode otherwise fine | the patched driver no longer works on the updated system: `~/.cache/xess-xmx/driver-status`; run `tools/rebuild-driver.sh` in a distrobox (see docs/building.md) |
| looks like DP4a, trace says `fallback: self-test failed` | the patched driver is not active in the game (session check fell back, variables missing) or no longer recognises the placeholders: `IGDEXT_TRACE=1`, docs/debugging.md |
| no image / GPU hang with ray tracing on (Wuthering Waves) | drivers up to v1.3.1 were built with the wrong install prefix and ran every game without Mesa's per-game workarounds (`/usr/share/drirc.d`); fixed in v1.3.2. A self-built driver: rebuild with `tools/rebuild-driver.sh`. `~/.cache/xess-xmx/driver-status` notes a driver that does not read them |
| first launch hangs for a minute or two | kernels are being compiled (once) |
| 30 fps with FG until the pause menu (Wuthering Waves) | the game's XeLL cap; fixed by the shim |
| another GPU or 32-bit games lost Vulkan | older `install.sh` versions listed only the patched 64-bit driver (found by reading the configuration, not seen on a machine): re-run the current one, re-login |

More in [docs/debugging.md](docs/debugging.md) (logs, trace switches, how to see which kernels run).

## Performance

* Wuthering Waves, Arc B390 at 22 W, frame generation set to 4x in the game, same driver and scene, alternating runs:
  - XMX path: the game gets its 4x (3 generated frames per real frame): 22.4 / 23.2 real fps, about 90 displayed.
  - generic path (`IGDEXT_FORCE_FALLBACK=1`, what stock Proton gets): XeSS frame generation falls back to 2x
    (1 generated frame): 26.5 / 26.8 real fps, about 53 displayed.
  The real frame rate and latency are not comparable across these two runs (different numbers of generated frames);
  a same-multiplier comparison (both at 2x) has not been made.
* XMX vs DP4a super resolution alone: not measured with an uncapped frame rate. The only numbers (September 2026, a
  30 fps cap, GPU 94-95 % busy either way) show nothing beyond both being close to GPU-bound.
* Image quality: not measured objectively. The XMX network is Intel's higher-quality model; the one metric taken here
  (stray pixels in a fast camera pan) came out about equal for XMX and DP4a.
* `VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer` (needed by the kernel binding code, set for the whole session by
  `install.sh`): no reliable measurement yet. The Wuthering Waves numbers were taken on the title screen with a 30 fps
  cap; the Forza Horizon 6 runs (43.7 / 41.2 fps disabled, 43.8 / 40.2 fps enabled) switched the variable through
  Proton's `user_settings.py` without checking that the game process received it.
* Kernel compile: about 0.2 s per kernel on an idle system, 0.3 s while a game loads; Wuthering Waves' 90 kernels
  took 65-70 s in total, once per machine and XeSS version (31 s of that is the compiler itself; the rest was not
  broken down).

## Known limitations

* **XeLL** (Intel's latency reduction, used with frame generation) runs in its cross-vendor mode. Its driver mode
  appears to be a separate component of Intel's Windows driver (`igxell64.dll`, inferred from the disassembly) plus an
  undocumented timing interface; neither exists on Linux. Details in [docs/how-it-works.md](docs/how-it-works.md).
* Only tested on Xe3. The driver patches are written against Mesa 26.1.2.
* The fallback (self-test failed, kernels that did not compile) gives XeSS' generic paths, whose frame generation is
  limited to 2x: a game set to 3x/4x runs at 2x until the XMX path works again.
* The placeholder shaders use workgroup sizes on three reserved planes (z = 7, 11, 13) and a magic value; the driver
  checks both, so a game shader is never replaced, but the check depends on vkd3d-proton compiling the placeholder
  to a store of that constant.
* The binding-table code relies on vkd3d-proton's descriptor heap layout (a mutable-type set with a large array, table
  offsets at the end of the push constants). If a vkd3d-proton update changes that, the driver logs
  `no vkd3d-style descriptor heap` and the kernels do nothing.

## How it works

[docs/how-it-works.md](docs/how-it-works.md): the shim, the placeholders, run-time compilation, which kernel variant
XeSS picks (and why `SIMD16Required` matters), frame generation, XeLL.

## Repository

    install.sh, uninstall.sh      system-wide install / removal
    enable.sh, disable.sh         per-prefix install / removal; xmx-launch.sh: Steam launch wrapper
    patches/0001-*.patch          Mesa 26.1.2 ANV: CM kernel injection and run-time compilation
    patches/0002-*.patch          Mesa 26.1.2 ANV: debug tools (captures, kernel selection, see docs/debugging.md)
    shim/                         igdext64.dll source: dxvk-igdext plus src/dll/ (D3D12Api.cpp feature answers and
                                  placeholders, GpuInfo.cpp device detection, XellHook.cpp XeLL frame-cap fix,
                                  XellDriver.cpp XeLL driver-mode probe, dyn_dummies.inc / xess_dummies.inc tables)
    tools/cm-compile.sh           run-time compiler helper called by the driver
    tools/get-igc.sh              fetches IGC 2.10.10 + ocloc 25.18 into igc/
    tools/cmk_pack.py             zebin -> .cmk packer;  tools/check-kernels.py validates a kernel folder
    tools/gen_dyn_dummies.py      placeholder table for run-time kernels;  tools/check_dyn_ids.py consistency check
    tools/gen_all_dummies.py, s16_map.py, build-kernels.sh, kernel_map_*.txt   optional static kernel set
    docs/                         how it works, building, debugging, games
    .github/workflows/build.yml   CI: builds the shim and the driver, checks the scripts

The release archive holds the two binaries (`igdext64.dll`, `lib/libvulkan_intel.so`) and the scripts. It contains no
Intel kernels; they are compiled on your machine from the SPIR-V inside the game's XeSS libraries.

## License

MIT (see LICENSE); the shim is based on dxvk-igdext (MIT), the patches apply to Mesa (MIT). See THIRD_PARTY.md.
