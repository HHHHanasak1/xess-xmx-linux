# XeSS XMX on Linux (Proton) for Intel Arc: Xe-HPG, Xe2, Xe3

Real Intel **XeSS** super resolution and **XeSS frame generation** inside Proton games, running on the GPU's XMX
(matrix) units instead of the DP4a fallback that stock Wine/Proton gets. No Windows driver, no change to Proton or
vkd3d-proton:

* `igdext64.dll`, a shim built on Intel's open source [dxvk-igdext](https://github.com/GameTechDev/dxvk-igdext), answers
  XeSS' driver queries like an Intel driver and hands every C-for-Metal (CM) kernel XeSS submits over as a placeholder;
* a patched Mesa **ANV** Vulkan driver compiles those kernels with Intel's compiler (IGC) on first use and runs them in
  place of the placeholders, inside the normal vkd3d-proton queue;
* `ControlLib.dll`, a stand-in for Intel's Control Library, gives XeSS frame generation the override that Intel
  Graphics Software sets on Windows: `XESS_FG=<n>` in a game's launch options;
* `amd_fidelityfx_upscaler_dx12.dll` and `amd_fidelityfx_framegeneration_dx12.dll` run a game's FSR 3.1+ upscaling
  and frame generation on XeSS, so a game without XeSS gets XeSS on XMX and multi-frame generation as well: `FSR=xess`
  in its launch options ([docs/fsr-frame-generation.md](docs/fsr-frame-generation.md)). A game that has XeSS itself
  should use that: this route can flicker more. With `dxgi.dll`, a game with FSR upscaling and no FSR frame generation
  gets XeSS frame generation as well: `FSR=xess FSR_FG=inject`.

Developed on an ONEXPLAYER 3 (Arc B390, Xe3) with SteamOS, Mesa 26.1.2 / 26.3 and GE-Proton 11-6 / Proton
Experimental; tested by users on Xe2 (Arc B580) and Xe-HPG (Arc A310). Super resolution renders correctly and is
temporally stable; frame generation runs at 2x/3x/4x on XMX as well (without the Intel extension, XeSS frame
generation is limited to 2x). With `XESS_FG` a game runs frame generation at a fixed multiplier, also above what its
menu offers: up to 4x as Intel's override does, and experimentally up to **8x**
([docs/launch-options.md](docs/launch-options.md)). With frame generation on, the shim keeps frames from queueing
before the GPU, which roughly halves the latency in a GPU-limited game at the same frame rate; on by default,
`IGDEXT_XELL_CAP=off` turns it off.

## Install

Download the release archive, unpack it somewhere it can stay (to move it: `uninstall.sh` first), and run:

    ./install.sh     # once; fetches the compiler bundle (about 95 MB), Intel's XeSS SDK libraries (77 MB) and
                     # AMD's FidelityFX loader and providers (87 MB), then reboot or log out and in
    ./uninstall.sh   # reverts everything

Every game started from Steam afterwards uses the patched driver, no launch options needed. In the game, select XeSS
(and XeSS frame generation) on its **DX12** renderer. The first launch compiles the game's kernels, a minute or two
once; after that they come from `~/.cache/xess-xmx`.

What `install.sh` does:

* writes `~/.config/environment.d/50-xess-xmx.conf`:
  - the patched driver as the 64-bit Intel Vulkan driver (other GPUs' drivers and the 32-bit Intel driver stay in the
    list) and the kernel compiler;
  - `VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer,VK_KHR_present_wait2`: the kernel binding code needs the first,
    and present wait 2 crashed games with frame generation (it applies to every D3D12 game of the session);
  - `force_vk_vendor=0`, because Mesa hides the Intel GPU from some games (see [docs/debugging.md](docs/debugging.md));
* puts the shim into every Proton that bundles an `igdext64.dll` and into every prefix, keeping the original as
  `.stock`, and enables a watcher that repeats this after a Proton update and for a new prefix. Every Steam library
  counts (SD card, second disk: `steamapps/libraryfolders.vdf`). A Proton installed by a distribution package (CachyOS'
  `proton-cachyos` in `/usr/share/steam/compatibilitytools.d`) belongs to root: `install.sh` lists it with the `sudo`
  commands that put the shim in, and games on it run on DP4a until then;
* puts `ControlLib.dll` into every prefix, for `XESS_FG`;
* puts a small `user_settings.py` into every Proton (yours, if any, is kept as `user_settings.user.py`), which gives
  each game the `XESS=...` and `FSR=...` launch options, e.g. `XESS=latest %command%`: Intel's newer XeSS libraries
  while the game runs, for a game whose XeSS is too old to see the Intel GPU
  ([docs/launch-options.md](docs/launch-options.md));
* enables `xess-xmx-check.service`: before every graphical session it checks that the patched driver loads and finds
  the GPU. If a system update broke it, the other driver of the package takes over; if both are broken, the session
  uses the stock drivers and XeSS runs as DP4a. `~/.cache/xess-xmx/driver-status` says so, and
  `tools/rebuild-driver.sh` rebuilds a driver for the new Mesa version. The check also copies the two host libraries
  the Steam Linux Runtime does not have (SPIRV-Tools, libdisplay-info) into `lib/deps/`, where the driver finds them
  inside the game's container too.

Only the native Steam client is supported (SteamOS, distribution packages), not Flatpak Steam.

**The system's own driver for one game (experimental):** `/path/to/package/xmx-stock.sh %command%` runs that game on
the distribution's unpatched Mesa ANV with this project's Vulkan layer (`layer/`) instead of the patched driver; Mesa
26.1, 26.3 and 27.0. It installs nothing ([docs/launch-options.md](docs/launch-options.md)).

**Per game instead of system-wide:** `XMX_APPID=<steam app id> ./enable.sh` (or `XMX_PREFIX=<prefix>` for other
launchers) installs the shim into that prefix and writes the variables to `~/.config/xess-xmx.conf`; put
`/path/to/package/xmx-launch.sh %command%` into the game's launch options. `disable.sh` undoes it.

### Two drivers: Mesa 26.1.2 and Mesa 26.3

The archive carries the patched driver for two Mesa versions; both run the same XeSS kernels:

* **Mesa 26.3.0-rc3** (the default): its new shader compiler Jay, the default on Xe2 and Xe3, makes the games' own
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
* The drivers in the archive (Mesa 26.1.2 and 26.3.0-rc3 with the patches) replace the system's 64-bit ANV for the user
  session only. For another Mesa version see [docs/building.md](docs/building.md).
* The drivers are built on Arch Linux and need glibc 2.38 and libwayland-client 1.24 or newer. SPIRV-Tools and
  libdisplay-info 0.3 come with the archive (`lib/bundled/`) for systems that lack them. Checked to load on SteamOS,
  Arch, openSUSE Tumbleweed, Void Linux, Fedora 43 and Ubuntu 25.10; Debian 13 and Ubuntu 24.04 have libwayland-client
  1.23 and need a driver built on the system (`tools/rebuild-driver.sh`).
* vkd3d-proton as in Proton 11 / GE-Proton 11.
* `install.sh` needs `curl`, `ar` (binutils), `tar` and `zstd` to unpack the compiler, `python3` (the compiler helper,
  the session check, `tools/fsr-fg.sh`) and `unzip` or `python3` for the XeSS SDK download.

## Games

See [docs/games.md](docs/games.md): Wuthering Waves (SR + FG), Forza Horizon 6 (SR; FG with `FSR_FG=inject`), Cyberpunk
2077 (SR + FG with `XESS=latest`, which replaces its older XeSS libraries while it runs; 4x and more with `XESS_FG`),
FSR 4 on XMX through upscaler builds from the OptiScaler Discord (no longer developed, the notes stay), notes on
OptiScaler and on a game without XeSS. FSR frame generation on XeSS-FG: CONTROL Resonant, Resident Evil Requiem;
Forza Horizon 6 from its FSR upscaling ([docs/fsr-frame-generation.md](docs/fsr-frame-generation.md)).

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

Arc B390, built-in benchmarks, one run each. "Without this project" is Mesa 26.1.2 with XeSS on its generic DP4a
path; 26.3 is this project's patched Mesa 26.3.0-rc3 (release 20261104):

| | Cyberpunk 2077 | Forza Horizon 6 |
|---|---|---|
| Without this project (26.1.2 + DP4a) | 47.7 fps | 43 fps (GPU 47.3) |
| 26.3 + XMX | 61.3 fps, +28% | 46 fps, +7% (GPU 55.0, +16%) |
| 26.3 + XMX + frame generation 3x | 117.0 fps, about 2.5x | 98 fps, about 2.3x |

* XMX itself is about 5% faster than DP4a in both games; most of the step from 26.1.2 to 26.3 is Jay, Mesa's new
  compiler backend. Forza Horizon 6 is held back by its render thread on this device, so its frame rate gains less
  than its GPU.
* With frame generation on, the shim's low latency mode (built on the XeLL API) cuts the latency of the frame queue
  by 36–50% at the same frame rate (Wuthering Waves).
* Mesa 26.3.0-rc3 unpatched, with this project's Vulkan layer (`xmx-stock.sh`, experimental), runs as fast as the
  patched driver.

Settings, every run and the latency tests: [docs/performance.md](docs/performance.md). #19 has a Final Fantasy VII
Rebirth comparison (about +15%, half of it from Jay). Not measured yet: image quality and the cost of disabling
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
                                  0003 Large GRF (Xe-HPG, Xe2), 0005 sampler descriptor tables (FSR 4 on XMX);
                                  patches/mesa-26.3/: the same for Mesa 26.3, plus 0004, the Jay crash fix for
                                  RE Engine games
    layer/, xmx-stock.sh          VK_LAYER_XESS_xmx: the XMX kernels on the system's unpatched ANV, per game (experimental)
    shim/                         igdext64.dll source: dxvk-igdext plus this project's code in src/dll/
    controllib/                   ControlLib.dll source: the frame generation override (XESS_FG)
    ffxfg/                        source of amd_fidelityfx_upscaler_dx12.dll (FSR upscaling on XeSS SR),
                                  amd_fidelityfx_framegeneration_dx12.dll (FSR frame generation on XeSS-FG) and
                                  dxgi.dll (XeSS-FG for a game without FSR frame generation, FSR_FG=inject);
                                  get-headers.sh fetches the AMD / Intel headers they are built against
    tools/cm-compile.sh           run-time compiler helper called by the driver
    tools/get-igc.sh              fetches IGC 2.10.10 + ocloc 25.18 into igc/
    tools/get-xess.sh             fetches Intel's XeSS SDK libraries into xess/ (XESS=latest)
    tools/get-ffx.sh              fetches AMD's FidelityFX loader and providers into ffx/ (FSR / FSR_FG with FSR 3.1.0-3.1.3)
    tools/proton-hook.py          XESS / XESS_LOG / FSR / FSR_FG launch options, run by Proton through user_settings.py
    tools/fsr-fg.sh               FSR on XeSS for one game, for Protons without the hook
    tools/steam-libraries.sh      every Steam library (libraryfolders.vdf), for the scripts
    tools/rebuild-driver.sh       rebuilds the patched driver for the system's Mesa version
    tools/switch-driver.sh        sets the default among the package's drivers (per game: MESA)
    icd/xmx-icd.c                 lib/libvulkan_intel.so: loads the driver MESA or the default names
    tools/session-check.sh        the driver check run before each session
    tools/cmk_pack.py             zebin -> .cmk packer (the driver has a C port; cm-compile.sh uses this one for older drivers)
    tools/xell-probe.sh           XeLL driver-mode probe (docs/debugging.md)
    tools/gen_placeholders.py     placeholder templates for run-time kernels;  tools/check_placeholders.py consistency check
    tools/split_patch.py          writes patches 0001 / 0002 from the marked source tree
    tools/build-deps.sh           builds the bundled SPIRV-Tools and libdisplay-info (lib/bundled/)
    tools/make-release.py         builds the release archive (the developer tools stay out of it)
    docs/                         how it works, building, debugging, games, Steam launch options, performance
    .github/workflows/build.yml   CI: builds the shim, ControlLib.dll, the FSR DLLs, the ICD, both
                                  drivers and the layer, checks the scripts

The release archive holds the binaries (`igdext64.dll`, `ControlLib.dll`, the three DLLs in `ffxfg/`, the drivers in
`lib/mesa-<version>/`, the ICD `lib/libvulkan_intel.so`, the layer `layer/libVkLayer_xess_xmx.so`) and the scripts.
It contains no Intel kernels; they are compiled on your machine from the SPIR-V inside the game's XeSS libraries.

## License

MIT (see LICENSE); the shim is based on dxvk-igdext (MIT), the patches apply to Mesa (MIT). See THIRD_PARTY.md.
