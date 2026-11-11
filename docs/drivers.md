# Drivers

The release archive includes three builds of **Mesa ANV** (Intel's open-source Vulkan driver) in `lib/mesa-<version>/`. All three run the same Intel XeSS C-for-Metal (CM) kernels.

When a game starts, the session's Vulkan ICD (Installable Client Driver, the file the Vulkan loader reads to find a GPU driver) points to `lib/libvulkan_intel.so` (built from `icd/xmx-icd.c`). That file is a small forwarding ICD that loads one of the three drivers based on your session mode, `lib/active`, or the `MESA` launch option.

For initial installation and `xmx-stock.sh` (running a single game on your Linux distribution's own driver), see [install.md](install.md). For building the drivers by hand and maintaining the patch series, see [dev/building.md](dev/building.md).

---

## The Three Drivers in the Archive

| Driver in `lib/` | Description | How it is selected |
|---|---|---|
| **Mesa 26.3.0 unpatched** (`lib/mesa-stock-26.3.0/`) | Upstream Mesa 26.3.0 as Mesa ships it, with no patches. This project's Vulkan layer `VK_LAYER_XESS_xmx` (`layer/libVkLayer_xess_xmx.so`) injects the XMX kernels from outside the driver. It performs like the patched 26.3.0 driver ([performance.md](performance.md)) and needs no patch carried across Mesa versions. | Session default on **Xe3** (`0xB080`–`0xB08F` Panther Lake, `0xFD80`–`0xFD8F` Wildcat Lake) |
| **Mesa 26.3.0 patched** (`lib/mesa-26.3.0/`) | Mesa 26.3.0 with `patches/mesa-26.3/` applied (initial 26.3 patches by @kas5852, [#19](https://github.com/HHHHanasak1/xess-xmx-linux/issues/19)). Used on Xe-HPG and Xe2 for two reasons. First, `VK_LAYER_XESS_xmx` serves Xe3 only. Xe3 here means `0xB080`–`0xB08F` and `0xFD80`–`0xFD8F`. Second, 256-register CM kernels on Xe-HPG and Xe2 require hardware Large GRF Mode, which only the driver can switch (`patches/0003`). That patch was contributed in [#16](https://github.com/HHHHanasak1/xess-xmx-linux/issues/16) for Xe2 and extended to Xe-HPG in [#31](https://github.com/HHHHanasak1/xess-xmx-linux/issues/31), [#32](https://github.com/HHHHanasak1/xess-xmx-linux/issues/32), and [#39](https://github.com/HHHHanasak1/xess-xmx-linux/issues/39). | Session default on **Xe-HPG** (Alchemist), **Xe2** (Battlemage, Lunar Lake), and every other PCI ID, including Panther Lake `0xB090`–`0xB0FF` |
| **Mesa 26.1.2 patched** (`lib/mesa-26.1.2/`) | Mesa 26.1.2 with `patches/` applied. This is the driver from earlier releases and is the same Mesa version as SteamOS's own. | Per game (`MESA=26.1`), chosen with `tools/switch-driver.sh 26.1`, or switched to by the session check if 26.3.0 fails to load |

On both 26.3.0 drivers, Mesa's new shader compiler backend **Jay** (the default on Xe2 and Xe3) makes the games' own shaders much faster ([performance.md](performance.md)).

### Library Requirements

The prebuilt drivers in the archive are compiled on Arch Linux and require **glibc 2.38** and **libwayland-client 1.24** or newer:
* Verified to load on SteamOS, Arch Linux, openSUSE Tumbleweed, Void Linux, Fedora 43, and Ubuntu 25.10.
* Debian 13 and Ubuntu 24.04 ship `libwayland-client` 1.23 (`undefined symbol: wl_fixes_interface`); on those distributions, build a driver against your system's libraries with `tools/rebuild-driver.sh` ([below](#rebuilding-a-driver-after-a-system-update)).
* `libSPIRV-Tools.so` and `libdisplay-info.so.3` are bundled in `lib/bundled/` for distributions that lack them or name them differently.

---

## Choosing a Driver

### 1. Session-Wide Driver Mode (`XMX_DRIVER`)

When you run `./install.sh`, it checks your GPU's PCI device ID. The Xe3 test is `(device_id & 0xfff0) == 0xb080` or `0xfd80`. That is `0xB080`–`0xB08F` and `0xFD80`–`0xFD8F`. These are the same Xe3 IDs that `VK_LAYER_XESS_xmx`'s self-test serves. The shim's GPU table in `GpuInfo.cpp` also covers `0xB090`–`0xB0FF`, which use patched mode. Then `install.sh` selects the session mode automatically:
* **`stock` mode (default on Xe3):** Sets `MESA=stock-26.3.0` and `VK_ADD_LAYER_PATH=<package>/layer` in `~/.config/environment.d/50-xess-xmx.conf`. It also sets `VK_LOADER_LAYERS_ENABLE=VK_LAYER_XESS_xmx`, `XMX_LAYER_HELPER`, and `XMX_IGC_DIR` there. Xe3 here means `0xB080`–`0xB08F` and `0xFD80`–`0xFD8F`.
* **`patched` mode (default on Xe-HPG, Xe2, and any other PCI ID):** Leaves `MESA` unset in the session environment so `lib/libvulkan_intel.so` loads the patched driver named in `lib/active` (`26.3.0` by default).

To override the automatic choice and save it in `run/driver-mode` for future runs of `install.sh`, run one of the following and then **log out and back in** (or reboot):

```bash
XMX_DRIVER=patched ./install.sh   # use the patched driver named in lib/active for the whole session
XMX_DRIVER=stock ./install.sh     # use stock-26.3.0 + VK_LAYER_XESS_xmx (outside 0xB080-0xB08F / 0xFD80-0xFD8F the layer declines its self-test and XeSS runs on DP4a)
XMX_DRIVER=auto ./install.sh      # remove run/driver-mode and return to the automatic choice by GPU
```

### 2. Switching the Default Patched Driver (`tools/switch-driver.sh`)

In a patched session, `lib/active` controls which driver in `lib/mesa-<version>/` is the default. Use `tools/switch-driver.sh` to list the drivers or switch the default:

```bash
./tools/switch-driver.sh          # list available drivers in lib/ (* marks the default in lib/active)
./tools/switch-driver.sh 26.1     # make patched Mesa 26.1.2 the default
./tools/switch-driver.sh 26.3     # make patched Mesa 26.3.0 the default
```

A version prefix (`26.1`, `26.3`, `26.1.2`) is enough: the script picks an exact match first, then a final release before a release candidate, then the newest matching version. Before updating `lib/active`, `tools/switch-driver.sh` runs `tools/session-check.sh --dry-run` on the chosen driver; if the check fails, nothing is changed.

Switching with `tools/switch-driver.sh` takes effect at the next game start—no re-login is needed. Running games keep the driver they already loaded.

*(Note: on Xe3 in the default `stock` mode, `~/.config/environment.d/50-xess-xmx.conf` exports `MESA=stock-26.3.0`, which overrides `lib/active`. On Xe3, either set `MESA=` per game below or switch the session to `XMX_DRIVER=patched ./install.sh` first.)*

### 3. Per-Game Driver Choice (`MESA` and `INTEL_DEBUG`)

To pick a patched driver for a single game, put `MESA=<version>` in its Steam launch options (**Properties → General → Launch options**). This takes effect on the next game start without logging out and overrides both `lib/active` and the session's `MESA`:

```text
MESA=26.1 %command%   # run this game on the patched Mesa 26.1.2 driver
MESA=26.3 %command%   # run this game on the patched Mesa 26.3.0 driver
```

When `MESA` is set, `lib/libvulkan_intel.so` prints `xess-xmx: using the Mesa <version> driver` to the game's standard error (`PROTON_LOG=1`). If no matching driver exists in `lib/`, it prints `xess-xmx: MESA=...: no such driver in ..., using the default` and loads the default driver.

If a game has a rendering bug or crash on Mesa 26.3 that goes away with `MESA=26.1 %command%`, you can test whether Mesa 26.3's Jay shader compiler is the cause by running the game's own shaders with the old compiler instead of Jay (XeSS kernels are not affected):

```text
INTEL_DEBUG=no-jay %command%
```

To run one game on your distribution's own unpatched system Mesa ANV instead of any driver in `lib/`, use `/path/to/package/xmx-stock.sh %command%` ([install.md](install.md), [launch-options.md](launch-options.md)).

---

## Pre-Session Driver Check and Automatic Fallback

Because `install.sh` replaces the system's 64-bit Intel ICD with the package's (`run/intel_icd.json`) in `VK_DRIVER_FILES`, a system update that broke the driver's library dependencies would otherwise leave the desktop session (including `gamescope` on SteamOS) without Vulkan on the Intel GPU.

To prevent this, `install.sh` enables the systemd user unit `xess-xmx-check.service`. Before every graphical session, it runs `~/.config/xess-xmx/session-check.sh` (calling `tools/session-check.sh`). It runs before these session units: `graphical-session-pre.target`, `gamescope-session.service`, `gamescope-session-plus@steam.service`, `steam-launcher.service`, and `plasma-workspace.target`.

### What `session-check.sh` Does

1. **Populates `lib/deps/` for the Steam Linux Runtime:**
   Proton games run inside the Steam Linux Runtime (`pressure-vessel`) container, which hides the host's `/usr`. The container does not include `libSPIRV-Tools.so` or `libdisplay-info.so.3` unless another host Vulkan driver (such as `radeon` or `nouveau`) pulls them in. Some distributions also name SPIRV-Tools `libSPIRV-Tools-shared.so` (Void Linux) or `libSPIRV-Tools-<version>.so` (openSUSE). Every driver in the package has `RUNPATH` set to `$ORIGIN/../deps`. At each login, `session-check.sh` copies the host's 64-bit `libSPIRV-Tools.so` and `libdisplay-info.so.3` into `lib/deps/`. If the host lacks them, it copies the archive's fallback builds from `lib/bundled/` instead.
2. **Cleans stale cached `.cmk` kernels:**
   Computes the current compiler toolchain generation hashes for the patched drivers and stores them in `~/.cache/xess-xmx/kernel-gens`. If an update changed the toolchain generation, it removes the patched drivers' old `*_*_<old_gen>.cmk` files from `~/.cache/xess-xmx/kernels/` (the layer's `.cml` files are not removed; new ones get new names).
3. **Verifies that the driver loads and finds the GPU:**
   * Checks that `run/intel_icd.json` and the library it points to exist (and in `stock` mode, that `layer/libVkLayer_xess_xmx.so` exists).
   * Loads the active driver (`lib/mesa-<active>/libvulkan_intel.so` in patched mode, or `lib/mesa-stock-<version>/libvulkan_intel.so` in stock mode) in Python with `RTLD_NOW` so every dynamic symbol must resolve.
   * Runs `vulkaninfo --summary` (if `vulkaninfo` is installed) with `VK_DRIVER_FILES` set to the package's ICD and checks that `deviceName` is found.
4. **Switches or falls back on failure:**
   * **In `patched` mode:** If the active patched driver fails to load or enumerate the GPU, `session-check.sh` switches to the other patched driver of the package (`lib/mesa-26.1.2/` or `lib/mesa-26.3.0/`) via `tools/switch-driver.sh`. If it passes, it becomes the new default in `lib/active` automatically; if none loads, the session falls back to the system's drivers.
   * **In `stock` mode, when no patched driver loads, or when the package folder was deleted or moved without running `uninstall.sh`:** The script falls back to the system's drivers. To do so, it overwrites `VK_DRIVER_FILES` in the user systemd environment with the system's stock Vulkan ICDs (`/usr/share/vulkan/icd.d/*.json`, etc.). It also clears `VKD3D_DISABLE_EXTENSIONS`, `ANV_CM_KERNEL_DIR`, and `ANV_CM_HELPER`. It clears `MESA`, `VK_ADD_LAYER_PATH`, and `VK_LOADER_LAYERS_ENABLE` only when they were set, that is in `stock` mode. Your session and games start normally on the system's stock Intel driver, and the shim's self-test puts XeSS on its generic DP4a path.

### Checking Driver Status

`session-check.sh` writes its result (`ok` or `FAILED`, the driver path, the Mesa version string, and notes if the host's Mesa version differs or if `/usr/share/drirc.d` is not read) to `~/.cache/xess-xmx/driver-status`:

```bash
cat ~/.cache/xess-xmx/driver-status
journalctl --user -u xess-xmx-check
```

You can also run the check manually without changing the session environment (this also refreshes `lib/deps/`):

```bash
./tools/session-check.sh --dry-run
```

---

## Rebuilding a Driver After a System Update

If a system update (such as a new SteamOS update) breaks the prebuilt drivers in the archive—or if your distribution ships `libwayland-client` older than 1.24 (Debian 13, Ubuntu 24.04)—use `tools/rebuild-driver.sh` to build a patched driver against your system's libraries. On Xe3 in `stock` mode (the default there), the rebuilt patched driver is used only after you switch the session to it with `XMX_DRIVER=patched ./install.sh` and log in again.

### What `tools/rebuild-driver.sh` Does

1. Detects your host system's 64-bit Mesa version from `/run/host/usr/lib*/libvulkan_intel.so` or `/usr/lib*/libvulkan_intel.so`, or uses the version you pass as an argument (`26.1.2`, `26.3.0`).
2. Downloads the official `mesa-<version>.tar.xz` release tarball from `archive.mesa3d.org` into `${XMX_MESA_DIR:-~/.cache/xess-xmx/mesa-src}/<version>` and verifies its SHA-256 checksum.
3. Applies the matching patch series (`patches/mesa-26.3/` for 26.3, `patches/` otherwise) with `git am`. If the patches do not apply cleanly to that Mesa version, the script aborts without changing anything.
4. Compiles `libvulkan_intel.so` with Meson and Ninja. The build is configured with `-Dprefix=/usr -Dsysconfdir=/etc` so `/usr/share/drirc.d` game profiles are read. It is also configured with `-Dintel-rt=enabled` for ray tracing and with `RUNPATH` `$ORIGIN/../deps`.
5. Tests the newly built driver with `tools/session-check.sh --dry-run`.
6. Installs it into `lib/mesa-<version>/libvulkan_intel.so` (keeping any previous file as `.prev`) and makes it the default patched driver via `tools/switch-driver.sh`.

### Running `tools/rebuild-driver.sh`

Building Mesa requires a C/C++ toolchain and Mesa's build dependencies:
* `gcc`, `g++`, `meson`, `ninja`
* `python-mako`, `python-yaml`, `python-packaging`, `python-ply`
* `glslang`, `spirv-tools`, `spirv-headers`, `spirv-llvm-translator`
* `llvm`, `clang`, `libclc`
* `libdrm`, `wayland`, `wayland-protocols`
* `libx11`, `libxext`, `libxrandr`, `libxcb`, `libxshmfence`
* `expat`, `zlib`, `zstd`, `libelf`

On a desktop distribution with those packages installed, run:

```bash
./tools/rebuild-driver.sh           # builds for the system's Mesa version
./tools/rebuild-driver.sh 26.3.0    # or specify a version
```

SteamOS does not ship a compiler on the host image. On SteamOS, run the script inside an Arch Linux `distrobox` container that has those packages installed:

```bash
distrobox enter <box> -- /path/to/package/tools/rebuild-driver.sh           # detects the host's Mesa version
distrobox enter <box> -- /path/to/package/tools/rebuild-driver.sh 26.3.0    # or specify a version
```

The script has built-in SHA-256 checksums for `26.1.2` (`bac2bca9121897a2b8162e79636b50ac998fca799c8e6cf914edd85962babdf0`) and `26.3.0` (`91d729aa68bb89e4ec857841300bbe922bcb3e52b1f97566ca83930d3b28343a`). To build another Mesa point release that the patch series applies to, pass its tarball SHA-256 from Mesa's release notes (`docs/relnotes/<version>.rst`):

```bash
XMX_MESA_SHA256=<sha256> ./tools/rebuild-driver.sh <version>
```

For manual Meson commands and patch details, see [dev/building.md](dev/building.md).
