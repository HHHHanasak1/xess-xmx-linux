# XeSS XMX on Linux (Proton) for Intel Arc: Xe-HPG, Xe2, Xe3

Real Intel **XeSS** super resolution and **XeSS frame generation** inside Proton games, running on the GPU's XMX
(matrix) units instead of the DP4a fallback that stock Wine/Proton gets. No Windows driver, no change to Proton or
vkd3d-proton:

* `igdext64.dll`, a shim built on Intel's open source [dxvk-igdext](https://github.com/GameTechDev/dxvk-igdext), answers
  XeSS' driver queries like an Intel driver and hands every C-for-Metal (CM) kernel XeSS submits over as a placeholder;
* a patched Mesa **ANV** Vulkan driver compiles those kernels with Intel's compiler (IGC) on first use and runs them in
  place of the placeholders, inside the normal vkd3d-proton queue;
* `ControlLib.dll`, a stand-in for Intel's Control Library, gives XeSS frame generation the override that Intel
  Graphics Software sets on Windows: `XESS_FG=<n>` in a game's launch options.

Developed on an ONEXPLAYER 3 (Arc B390, Xe3) with SteamOS, Mesa 26.1.2 / 26.3 and GE-Proton 11-6 / Proton
Experimental; tested by users on Xe2 (Arc B580) and Xe-HPG (Arc A310). Super resolution renders correctly and is
temporally stable; frame generation runs at 2x/3x/4x on XMX as well (without the Intel extension, XeSS frame
generation is limited to 2x). With `XESS_FG` a game runs frame generation at a fixed multiplier, also above what its
menu offers: up to 4x as Intel's override does, and experimentally up to **8x**
([docs/launch-options.md](docs/launch-options.md)).

## Install

Download the release archive, unpack it somewhere it can stay, and run:

    ./install.sh     # once; fetches the compiler bundle (about 230 MB) and Intel's XeSS SDK libraries (77 MB),
                     # then reboot or log out and in
    ./uninstall.sh   # reverts everything

Every game started from Steam afterwards uses the patched driver, no launch options needed. In the game, select XeSS
(and XeSS frame generation) on its **DX12** renderer. The first launch compiles the game's kernels, a minute or two
once; after that they come from `~/.cache/xess-xmx`.

What `install.sh` does:

* writes `~/.config/environment.d/50-xess-xmx.conf`:
  - the patched driver as the 64-bit Intel Vulkan driver (other GPUs' drivers and the 32-bit Intel driver stay in the
    list) and the kernel compiler;
  - `VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer`, which the kernel binding code needs (it applies to every D3D12
    game of the session);
  - `force_vk_vendor=0`, because Mesa hides the Intel GPU from some games (see [docs/debugging.md](docs/debugging.md));
* puts the shim into every Proton that bundles an `igdext64.dll` and into every prefix, keeping the original as
  `.stock`, and enables a watcher that repeats this after a Proton update and for a new prefix. Every Steam library
  counts (SD card, second disk: `steamapps/libraryfolders.vdf`). A Proton installed by a distribution package (CachyOS'
  `proton-cachyos` in `/usr/share/steam/compatibilitytools.d`) belongs to root: `install.sh` lists it with the `sudo`
  commands that put the shim in, and games on it run on DP4a until then;
* puts `ControlLib.dll` into every prefix, for `XESS_FG`;
* puts a small `user_settings.py` into every Proton (yours, if any, is kept as `user_settings.user.py`), which gives
  each game the `XESS=...` launch options, e.g. `XESS=latest %command%`: Intel's newer XeSS libraries while the game
  runs, for a game whose XeSS is too old to see the Intel GPU ([docs/launch-options.md](docs/launch-options.md));
* enables `xess-xmx-check.service`: before every graphical session it checks that the patched driver loads and finds
  the GPU. If a system update broke it, the other driver of the package takes over; if both are broken, the session
  uses the stock drivers and XeSS runs as DP4a. `~/.cache/xess-xmx/driver-status` says so, and
  `tools/rebuild-driver.sh` rebuilds a driver for the new Mesa version. The check also copies the two host libraries
  the Steam Linux Runtime does not have (SPIRV-Tools, libdisplay-info) into `lib/deps/`, where the driver finds them
  inside the game's container too.

Only the native Steam client is supported (SteamOS, distribution packages), not Flatpak Steam.

**Per game instead of system-wide:** `XMX_APPID=<steam app id> ./enable.sh` (or `XMX_PREFIX=<prefix>` for other
launchers) installs the shim into that prefix and writes the variables to `~/.config/xess-xmx.conf`; put
`/path/to/package/xmx-launch.sh %command%` into the game's launch options. `disable.sh` undoes it.

### Two drivers: Mesa 26.1.2 and Mesa 26.3

The archive carries the patched driver for two Mesa versions; both run the same XeSS kernels:

* **Mesa 26.3.0-rc2** (the default): its new shader compiler Jay, the default on Xe2 and Xe3, makes the games' own
  shaders much faster. Patches by @kas5852 (#19). A release candidate: tested in fewer games so far.
* **Mesa 26.1.2**: the driver of the earlier releases, the same version as SteamOS' own.

Choose per game in its Steam launch options:

    MESA=26.1 %command%         # this game uses Mesa 26.1.2
    MESA=26.3 %command%         # this game uses Mesa 26.3

Games without `MESA` use the default, which `tools/switch-driver.sh` lists (`*`) and changes
(`tools/switch-driver.sh 26.1`); both take effect at the next game start, no re-login. If the default driver fails the
check before a session (for example after a system update), the other one becomes the default automatically.
`INTEL_DEBUG=no-jay %command%` runs a game's own shaders on Mesa 26.3 with the old compiler.

### Requirements

* An Intel GPU with XMX on the `xe` or `i915` kernel driver: Xe3 (Panther Lake; tested on an Arc B390), Xe2
  (Battlemage; tested by a user on an Arc B580, #16) or Xe-HPG (Alchemist; tested by a user on an Arc A310, #28).
  Lunar Lake and Arrow Lake-H are detected but untested. Xe-LPG (Meteor Lake) has no XMX units.
* The drivers in the archive (Mesa 26.1.2 and 26.3.0-rc2 with the patches) replace the system's 64-bit ANV for the user
  session only. For another Mesa version see [docs/building.md](docs/building.md).
* The drivers are built on Arch Linux and need glibc 2.38 and libwayland-client 1.24 or newer. SPIRV-Tools and
  libdisplay-info 0.3 come with the archive (`lib/bundled/`) for systems that lack them. Checked to load on SteamOS,
  Arch, openSUSE Tumbleweed, Void Linux, Fedora 43 and Ubuntu 25.10; Debian 13 and Ubuntu 24.04 have libwayland-client
  1.23 and need a driver built on the system (`tools/rebuild-driver.sh`).
* vkd3d-proton as in Proton 11 / GE-Proton 11.

## Games

See [docs/games.md](docs/games.md): Wuthering Waves (SR + FG), Forza Horizon 6 (SR; FG through OptiScaler), Cyberpunk
2077 (SR + FG with `XESS=latest`, which replaces its older XeSS libraries while it runs; 4x and more with `XESS_FG`),
notes on OptiScaler and on a game without XeSS.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| looks like DP4a, nothing changed | the shim is not in the prefix: re-run `install.sh` |
| DP4a in one game only, or no XeSS frame generation option | the game sees no Intel GPU: XeSS older than 2.0.2.68 (`XESS=latest %command%`, docs/launch-options.md) or a Mesa game profile (docs/debugging.md) |
| noise or black patches in the image | a kernel failed to compile; the next launch retries once and then uses XeSS' generic paths. See `~/.cache/xess-xmx/compile.log`; a missing compiler: `tools/get-igc.sh` |
| XMX gone after a SteamOS update | neither patched driver loads any more: `~/.cache/xess-xmx/driver-status`; run `tools/rebuild-driver.sh` (docs/building.md) |
| a game misbehaves on Mesa 26.3 only | `MESA=26.1 %command%` in its launch options, and please report it |
| first launch hangs for a minute or two | kernels are being compiled (once) |
| the game offers only 2x frame generation | `XESS_FG=4 %command%` (docs/launch-options.md) |
| `XESS_FG` changes nothing | frame generation must be switched on in the game, and the game's XeSS frame generation must be 1.2 or newer (`XESS=latest`) |

Steam launch options for one game (driver choice, Jay off, XMX off, logs): [docs/launch-options.md](docs/launch-options.md).
The full table, the logs and every switch: [docs/debugging.md](docs/debugging.md).

## Performance

* Mesa 26.3 (Jay) against 26.1.2, Arc B390, same spot and settings, XeSS super resolution and frame generation on XMX:

  | Game | Mesa 26.1.2 | Mesa 26.3.0-rc1 |
  |---|---|---|
  | Wuthering Waves | 112-119 fps, low 94 | 120-126 fps, low 113 |
  | Cyberpunk 2077 | 61-62 fps | 91-96 fps |

  #19 has a Final Fantasy VII Rebirth comparison (about +15%, half of it from Jay).
* Not measured yet: XMX against DP4a super resolution alone, image quality, and the cost of disabling
  `VK_EXT_descriptor_buffer`.

## Known limitations

* **XeLL** (Intel's latency reduction, used with frame generation) runs in its cross-vendor mode; its driver mode does
  not start under Proton. Details in [docs/how-it-works.md](docs/how-it-works.md).
* The fallback (self-test failed, kernels that did not compile) gives XeSS' generic paths, whose frame generation is
  limited to 2x: a game set to 3x/4x runs at 2x until the XMX path works again.
* `XESS_FG=5` to `8` raise the number of generated frames by changing a value inside Intel's frame generation library
  while the game runs; Intel does not support more than 4x. Experimental, and 7x/8x add a lot of latency.
* The driver patches are written against Mesa 26.1.2 and 26.3 and rely on vkd3d-proton internals: how it compiles the
  placeholder shaders and how it lays out its descriptor heap. If a vkd3d-proton update changes the first, the
  self-test turns the XMX path off; if it changes the second, the driver logs `no vkd3d-style descriptor heap` and the
  kernels do nothing.

## How it works

[docs/how-it-works.md](docs/how-it-works.md): the shim, the placeholders, run-time compilation, which kernel variant
XeSS picks (and why `SIMD16Required` matters), frame generation, XeLL.

## Repository

    install.sh, uninstall.sh      system-wide install / removal
    enable.sh, disable.sh         per-prefix install / removal; xmx-launch.sh: Steam launch wrapper
    patches/                      Mesa 26.1.2 ANV: 0001 kernel injection and run-time compilation, 0002 debug tools,
                                  0003 Large GRF (Xe-HPG, Xe2); patches/mesa-26.3/: the same for Mesa 26.3,
                                  plus 0004, the Jay crash fix for RE Engine games
    shim/                         igdext64.dll source: dxvk-igdext plus this project's code in src/dll/
    controllib/                   ControlLib.dll source: the frame generation override (XESS_FG)
    tools/cm-compile.sh           run-time compiler helper called by the driver
    tools/get-igc.sh              fetches IGC 2.10.10 + ocloc 25.18 into igc/
    tools/get-xess.sh             fetches Intel's XeSS SDK libraries into xess/ (XESS=latest)
    tools/proton-hook.py          XESS / XESS_LOG launch options, run by Proton through user_settings.py
    tools/steam-libraries.sh      every Steam library (libraryfolders.vdf), for the scripts
    tools/rebuild-driver.sh       rebuilds the patched driver for the system's Mesa version
    tools/switch-driver.sh        sets the default among the package's drivers (per game: MESA)
    icd/xmx-icd.c                 lib/libvulkan_intel.so: loads the driver MESA or the default names
    tools/session-check.sh        the driver check run before each session
    tools/cmk_pack.py             zebin -> .cmk packer;  tools/check-kernels.py validates a kernel folder
    tools/gen_dyn_dummies.py      placeholder table for run-time kernels;  tools/check_dyn_ids.py consistency check
    tools/split_patch.py          writes patches 0001 / 0002 from the marked source tree
    tools/build-deps.sh           builds the bundled SPIRV-Tools and libdisplay-info (lib/bundled/)
    tools/make-release.py         builds the release archive
    tools/xell-probe.sh           XeLL driver-mode probe (docs/debugging.md)
    tools/gen_all_dummies.py, s16_map.py, build-kernels.sh, kernel_map_*.txt   optional static kernel set
    docs/                         how it works, building, debugging, games, Steam launch options
    .github/workflows/build.yml   CI: builds the shim, ControlLib.dll, the ICD and both drivers, checks the scripts

The release archive holds the binaries (`igdext64.dll`, `ControlLib.dll`, the drivers in `lib/mesa-<version>/`, the
ICD `lib/libvulkan_intel.so`) and the scripts. It contains no Intel kernels; they are compiled on your machine from
the SPIR-V inside the game's XeSS libraries.

## License

MIT (see LICENSE); the shim is based on dxvk-igdext (MIT), the patches apply to Mesa (MIT). See THIRD_PARTY.md.
