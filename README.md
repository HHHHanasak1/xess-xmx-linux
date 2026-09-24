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
generation runs at 2x/3x/4x with its network on XMX as well. On the B390 the frame rate is at parity with DP4a (at 22 W
the GPU, not the upscaler, is the limit), so the point is image quality: the XMX network is the better one.

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
* enables a user path unit that puts the shim back after a Proton update;
* enables `xess-xmx-check.service`, which loads the patched driver once before every graphical session. If a system
  update broke one of its library dependencies (`libSPIRV-Tools.so`, glibc), the session is pointed back to the stock
  drivers: XeSS runs as DP4a, everything else keeps working (instead of the whole session losing the Intel GPU).
  Result in `~/.cache/xess-xmx/driver-status`.

Run it again after a game created a new prefix. (XeSS would also accept the shim's folder through
`INTC_ALT_DRIVER_EXTENSIONS_PATH`, which would make the per-prefix copies unnecessary, but GE-Proton sets that variable
to `C:\Windows\System32` for every game, so the copies stay.) It supports the native Steam client (SteamOS, distribution packages);
Flatpak Steam is not supported (its sandbox sees neither the environment file nor the package folder).

**Per game instead of system-wide:** `XMX_APPID=<steam app id> ./enable.sh` (or `XMX_PREFIX=<prefix>` for other
launchers) installs the shim into that prefix and writes the variables to `~/.config/xess-xmx.conf`; put
`/path/to/package/xmx-launch.sh %command%` into the game's launch options. `disable.sh` undoes it.

### Requirements

* Intel Xe3 (Panther Lake, tested) or Xe2 (Lunar Lake, Battlemage: should work, untested) on the `xe` or `i915`
  kernel driver. Xe-HPG (Alchemist) and Xe-LPG are detected and get the answers a Windows driver would give, but
  nothing about them has been tested.
* The driver in the archive is Mesa 26.1.2 with the patches; it replaces the system's 64-bit ANV only for the user
  session. For another Mesa version, rebuild it from `patches/` (see [docs/building.md](docs/building.md)).
* vkd3d-proton as in Proton 11 / GE-Proton 11.

## Games

See [docs/games.md](docs/games.md): Wuthering Waves (SR + FG), Forza Horizon 6 (SR; FG through OptiScaler), notes on
OptiScaler and on a game without XeSS.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| looks like DP4a, nothing changed | the shim is not in the prefix: re-run `install.sh` (new prefix, Proton update without the watcher) |
| noise or black patches in the image | a kernel failed to compile; the next launch retries it and falls back to DP4a if it still fails. See `~/.cache/xess-xmx/compile.log`; a missing compiler: `tools/get-igc.sh` |
| XMX gone after a SteamOS update, game mode otherwise fine | the patched driver no longer loads on the updated system: `~/.cache/xess-xmx/driver-status`; a release built for the new system fixes it |
| first launch hangs for a minute or two | kernels are being compiled (once) |
| 30 fps with FG until the pause menu (Wuthering Waves) | the game's XeLL cap; fixed by the shim |
| another GPU or 32-bit games lost Vulkan | installed with an older `install.sh`: re-run the current one, re-login |

More in [docs/debugging.md](docs/debugging.md) (logs, trace switches, how to see which kernels run).

## Performance

* XMX vs DP4a on the Arc B390 at 22 W: same frame rate (GPU-bound either way); the XMX network gives the better image.
* `VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer` (needed by the kernel binding code, set for the whole session by
  `install.sh`): Wuthering Waves, no difference in frame rate or GPU load. Forza Horizon 6 (open
  world, car standing, alternating runs): 43.7 / 41.2 fps with the extension disabled, 43.8 / 40.2 fps with it enabled,
  no measurable difference.
* Kernel compile: about 1 s per kernel, once per machine and XeSS version (90 kernels for Wuthering Waves: ~80 s).

## Known limitations

* **XeLL** (Intel's latency reduction, used with frame generation) runs in its cross-vendor mode. Its driver mode is a
  separate component of Intel's Windows driver (`igxell64.dll`) plus an undocumented timing interface; neither exists
  on Linux. Details in [docs/how-it-works.md](docs/how-it-works.md).
* Only tested on Xe3. The driver patches are written against Mesa 26.1.2.
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
