# Drivers

The release archive includes three Mesa **ANV** drivers (Intel's open-source Vulkan driver). They replace the system's 64-bit Intel Vulkan driver for your user session only, leaving other GPUs and 32-bit games on the system drivers. All three run the same XeSS kernels on the GPU's XMX (matrix) units.

## Drivers in the archive

| Driver | Path in archive | Default on | Notes |
|---|---|---|---|
| **Mesa 26.3.0 unpatched** + layer | `lib/mesa-stock-26.3.0/` | Xe3 (Panther Lake) | Mesa as upstream ships it. This project's Vulkan layer `VK_LAYER_XESS_xmx` (`layer/`) injects the XMX kernels from outside the driver. Performs like patched 26.3 ([performance.md](performance.md)) and needs no patches carried across Mesa versions. |
| **Mesa 26.3.0 patched** | `lib/mesa-26.3.0/` | Xe-HPG (Alchemist), Xe2 (Battlemage) | The layer supports Xe3 only, and 256-register kernels on Xe-HPG and Xe2 need Large GRF Mode (a hardware switch that doubles the registers per thread), which only the driver can toggle (`patches/0003`). Patches by @kas5852 (#19). |
| **Mesa 26.1.2 patched** | `lib/mesa-26.1.2/` | Not a default; used per game (`MESA=26.1`) or with `tools/switch-driver.sh`, and the session check switches to the other patched driver when the default one breaks | The driver from earlier releases, matching the Mesa version SteamOS ships. |

On both 26.3 drivers, Mesa's new shader compiler **Jay** is the default on Xe2 and Xe3. Jay makes the games' own shaders run much faster.

The session's Vulkan driver list (`VK_DRIVER_FILES` in `~/.config/environment.d/50-xess-xmx.conf`) points at `lib/libvulkan_intel.so` (referenced via `run/intel_icd.json`). This file is a small loader (an ICD, Installable Client Driver) built from `icd/xmx-icd.c`. When a game starts, it loads the driver selected by `MESA`, or the default version in `lib/active`, or any package driver that loads.

## Choosing a driver

### For the whole session

`install.sh` picks the session's driver based on your GPU: unpatched 26.3 + layer on Xe3, patched 26.3 on Xe-HPG and Xe2.

To override this choice for the whole session, run `install.sh` with `XMX_DRIVER` and then log out and back in (or reboot):

```sh
XMX_DRIVER=patched ./install.sh   # use the patched driver even on Xe3
XMX_DRIVER=stock ./install.sh     # use the unpatched 26.3 driver + layer
XMX_DRIVER=auto ./install.sh      # go back to the automatic choice by GPU
```

Your choice is saved for later runs of `install.sh`.

### Switching between patched drivers

When your session uses the patched drivers, `tools/switch-driver.sh` lists the available drivers and changes the default without logging out. Changes take effect the next time you start a game.

1. List the drivers in the package (`*` marks the active default):

   ```sh
   tools/switch-driver.sh
   ```

2. Set a different version as the default (the script checks that the driver loads first):

   ```sh
   tools/switch-driver.sh 26.1
   ```

### For one game

Set `MESA` in the game's Steam launch options (Game -> Properties -> General -> Launch options). It takes effect on the next launch with no re-login:

```sh
MESA=26.1 %command%         # run this game on patched Mesa 26.1.2
MESA=26.3 %command%         # run this game on patched Mesa 26.3
```

A version prefix (`26.1`, `26.3`) or a full version (`26.1.2`) works. The game's stderr logs `xess-xmx: using the Mesa ... driver`. If you give an unknown version, it logs `xess-xmx: MESA=...: no such driver` and falls back to the default driver.

Use `MESA=26.1 %command%` if a game misbehaves on Mesa 26.3 only, and report the issue. If a rendering bug or crash goes away with `MESA=26.1`, you can test whether the Jay compiler is the cause by running the game on 26.3 with the old compiler instead:

```sh
INTEL_DEBUG=no-jay %command%
```

This compiles the game's own shaders with the old compiler while leaving XeSS unaffected.

### Using the system's own driver for one game

To run a single game on your distribution's own unpatched Mesa ANV with `VK_LAYER_XESS_xmx` (experimental; supports Mesa 26.1, 26.3, and 27.0), put the `xmx-stock.sh` wrapper in the game's launch options:

```sh
/path/to/package/xmx-stock.sh %command%
```

See [install.md](install.md) and [launch-options.md](launch-options.md) for details.

## Session check and automatic fallback

`install.sh` enables a systemd user service, `xess-xmx-check.service` (which runs `~/.config/xess-xmx/session-check.sh`, a copy of `tools/session-check.sh`). Before each graphical session starts, it checks that the package's active driver loads and finds the GPU.

* **If a system update broke the active patched driver:** the check switches the default to the other patched driver in the package automatically.
* **If both patched drivers fail (or if the stock session driver fails):** the session falls back to your distribution's stock Vulkan drivers, and XeSS runs on its generic DP4a path. The same fallback happens if you delete or move the package folder without running `install.sh` from the new location.
* **Host library dependencies:** the driver requires `libSPIRV-Tools.so` and `libdisplay-info.so.3`, which the Steam Linux Runtime container does not include. During the check, the script copies both from the host into `lib/deps/` (the driver's `RUNPATH` is `$ORIGIN/../deps`), so the driver can load inside the game's container. If the host lacks them or names them differently (`libSPIRV-Tools-shared.so` on Void Linux, `libSPIRV-Tools-<version>.so` on openSUSE), the check uses the copies bundled in `lib/bundled/`.

To inspect the result of the last check or run it by hand:

```sh
cat ~/.cache/xess-xmx/driver-status
journalctl --user -u xess-xmx-check
tools/session-check.sh --dry-run      # reports status and populates lib/deps/ without changing the active driver
```

When invoked directly, `tools/session-check.sh` also accepts `XMX_CHECK_LIB` and `XMX_CHECK_ICD` in its environment to test a specific driver library or ICD file.

## Rebuilding a driver after a system update

The prebuilt drivers in the archive are compiled on Arch Linux and require **glibc 2.38** and **libwayland-client 1.24** or newer. They load as-is on SteamOS, Arch, openSUSE Tumbleweed, Void Linux, Fedora 43, and Ubuntu 25.10.

You need to rebuild the driver on your machine in two cases:

* A system update (such as a SteamOS update) upgraded system libraries so that neither prebuilt driver loads anymore (`~/.cache/xess-xmx/driver-status` says `FAILED`).
* Your distribution ships a `libwayland-client` older than 1.24, such as Debian 13 or Ubuntu 24.04 (`driver-status` or the Proton log shows `undefined symbol: wl_fixes_interface`).

`tools/rebuild-driver.sh` builds a patched driver for your system:

1. Downloads the Mesa release tarball from `archive.mesa3d.org`.
2. Applies the project's patches (`patches/mesa-26.3/` for 26.3, `patches/` for 26.1.2).
3. Compiles `libvulkan_intel.so` and tests it the same way the session check does.
4. Installs it to `lib/mesa-<version>/libvulkan_intel.so` and makes it the default driver via `tools/switch-driver.sh`.

If the patches do not apply to the target Mesa version, the script stops without changing anything and tells you. The patch series are written for Mesa 26.1.2 and 26.3.0; other versions need a rebase (see [dev/building.md](dev/building.md)).

### On desktop Linux

Install Mesa's build dependencies for your distribution, then run:

```sh
/path/to/package/tools/rebuild-driver.sh          # builds for the system's installed Mesa version
/path/to/package/tools/rebuild-driver.sh 26.3.0   # or specify a version (26.3.0 or 26.1.2)
```

### On SteamOS (using distrobox)

SteamOS does not include a compiler. Run the script inside an Arch Linux distrobox container that has the Mesa build dependencies installed:

```sh
distrobox enter <box> -- /path/to/package/tools/rebuild-driver.sh          # the system's Mesa version
distrobox enter <box> -- /path/to/package/tools/rebuild-driver.sh 26.3.0   # or a given version
```

By default, the script keeps its source and build trees in `~/.cache/xess-xmx/mesa-src`. Set `XMX_MESA_DIR` in the environment to use a different directory.

For manual Mesa builds, meson flags, and patch internals, see [dev/building.md](dev/building.md).
