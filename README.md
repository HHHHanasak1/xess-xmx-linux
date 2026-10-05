# XeSS XMX on Linux (Proton) for Intel Xe2 / Xe3

Real Intel **XeSS** super resolution and **XeSS frame generation** inside Proton games, running on the GPU's XMX
(matrix) units instead of the DP4a fallback that stock Wine/Proton gets. No Windows driver, no change to Proton or
vkd3d-proton:

* `igdext64.dll`, a shim built on Intel's open source [dxvk-igdext](https://github.com/GameTechDev/dxvk-igdext), answers
  XeSS' driver queries like an Intel driver and hands every C-for-Metal (CM) kernel XeSS submits over as a placeholder;
* a patched Mesa **ANV** Vulkan driver compiles those kernels with Intel's compiler (IGC) on first use and runs them in
  place of the placeholders, inside the normal vkd3d-proton queue.

Developed on an ONEXPLAYER 3 (Arc B390, Xe3) with SteamOS, Mesa 26.1.2 and GE-Proton 11-6 / Proton Experimental.
Super resolution renders correctly and is temporally stable; frame generation runs at 2x/3x/4x on XMX as well
(without the Intel extension, XeSS frame generation is limited to 2x).

## Install

Download the release archive, unpack it somewhere it can stay, and run:

    ./install.sh     # once; fetches the compiler bundle (about 230 MB), then reboot or log out and in
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
  `.stock`, and enables a watcher that repeats this after a Proton update and for a new prefix;
* enables `xess-xmx-check.service`: before every graphical session it checks that the patched driver loads and finds
  the GPU. If a system update broke it, the session uses the stock drivers and XeSS runs as DP4a;
  `~/.cache/xess-xmx/driver-status` says so, and `tools/rebuild-driver.sh` rebuilds the driver for the new Mesa version.

Only the native Steam client is supported (SteamOS, distribution packages), not Flatpak Steam.

**Per game instead of system-wide:** `XMX_APPID=<steam app id> ./enable.sh` (or `XMX_PREFIX=<prefix>` for other
launchers) installs the shim into that prefix and writes the variables to `~/.config/xess-xmx.conf`; put
`/path/to/package/xmx-launch.sh %command%` into the game's launch options. `disable.sh` undoes it.

### Requirements

* Intel Xe3 (Panther Lake; tested on an Arc B390) or Xe2 (Battlemage; tested by a contributor on an Arc B580, #16) on
  the `xe` or `i915` kernel driver. Lunar Lake is untested; Xe-HPG (Alchemist) and Xe-LPG are detected but untested.
* The driver in the archive is Mesa 26.1.2 with the patches; it replaces the system's 64-bit ANV for the user session
  only. For another Mesa version see [docs/building.md](docs/building.md).
* vkd3d-proton as in Proton 11 / GE-Proton 11.

## Games

See [docs/games.md](docs/games.md): Wuthering Waves (SR + FG), Forza Horizon 6 (SR; FG through OptiScaler), Cyberpunk
2077 (SR + FG after replacing its older XeSS libraries), notes on OptiScaler and on a game without XeSS.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| looks like DP4a, nothing changed | the shim is not in the prefix: re-run `install.sh` |
| DP4a in one game only, or no XeSS frame generation option | the game sees no Intel GPU (a Mesa game profile, or DXVK with XeSS older than 2.0.2.68): docs/debugging.md |
| noise or black patches in the image | a kernel failed to compile; the next launch retries once and then uses XeSS' generic paths. See `~/.cache/xess-xmx/compile.log`; a missing compiler: `tools/get-igc.sh` |
| XMX gone after a SteamOS update | the patched driver no longer loads: `~/.cache/xess-xmx/driver-status`; run `tools/rebuild-driver.sh` (docs/building.md) |
| first launch hangs for a minute or two | kernels are being compiled (once) |

The full table, the logs and every switch: [docs/debugging.md](docs/debugging.md).

## Performance

* Wuthering Waves, Arc B390 at 22 W, frame generation set to 4x in the game, same scene, alternating runs:
  - XMX path: the game gets its 4x (3 generated frames per real frame): 22.4 / 23.2 real fps, about 90 displayed;
  - generic path (`IGDEXT_FORCE_FALLBACK=1`, what stock Proton gets): frame generation falls back to 2x:
    26.5 / 26.8 real fps, about 53 displayed.

  The two runs generate different numbers of frames, so their real frame rates and latency are not comparable.
* Not measured yet: XMX against DP4a super resolution alone, frame generation at the same multiplier on both paths,
  image quality, and the cost of disabling `VK_EXT_descriptor_buffer`.
* Kernel compile: Wuthering Waves' 90 kernels take 65-70 s in total, once per machine and XeSS version.

## Known limitations

* **XeLL** (Intel's latency reduction, used with frame generation) runs in its cross-vendor mode; its driver mode needs
  a component of Intel's Windows driver. Details in [docs/how-it-works.md](docs/how-it-works.md).
* The fallback (self-test failed, kernels that did not compile) gives XeSS' generic paths, whose frame generation is
  limited to 2x: a game set to 3x/4x runs at 2x until the XMX path works again.
* The driver patches are written against Mesa 26.1.2 and rely on vkd3d-proton internals: how it compiles the
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
                                  0003 Large GRF Mode for Xe2
    shim/                         igdext64.dll source: dxvk-igdext plus this project's code in src/dll/
    tools/cm-compile.sh           run-time compiler helper called by the driver
    tools/get-igc.sh              fetches IGC 2.10.10 + ocloc 25.18 into igc/
    tools/rebuild-driver.sh       rebuilds the patched driver for the system's Mesa version
    tools/session-check.sh        the driver check run before each session
    tools/cmk_pack.py             zebin -> .cmk packer;  tools/check-kernels.py validates a kernel folder
    tools/gen_dyn_dummies.py      placeholder table for run-time kernels;  tools/check_dyn_ids.py consistency check
    tools/split_patch.py          writes patches 0001 / 0002 from the marked source tree
    tools/gen_all_dummies.py, s16_map.py, build-kernels.sh, kernel_map_*.txt   optional static kernel set
    docs/                         how it works, building, debugging, games
    .github/workflows/build.yml   CI: builds the shim and the driver, checks the scripts

The release archive holds the two binaries (`igdext64.dll`, `lib/libvulkan_intel.so`) and the scripts. It contains no
Intel kernels; they are compiled on your machine from the SPIR-V inside the game's XeSS libraries.

## License

MIT (see LICENSE); the shim is based on dxvk-igdext (MIT), the patches apply to Mesa (MIT). See THIRD_PARTY.md.
