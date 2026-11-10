# Install, update, and uninstall

This guide covers requirements, system-wide installation, per-game installation, and running a game on the system's stock driver.

## Requirements

* **GPU:** An Intel GPU with XMX (Intel's matrix engines) on the `xe` or `i915` kernel driver:
  * **Xe3** (Panther Lake): tested on an Arc B390 (ONEXPLAYER 3).
  * **Xe2** (Battlemage): tested by a user on an Arc B580 (#16).
  * **Xe-HPG** (Alchemist): tested by a user on an Arc A310 (#28).
  * **Lunar Lake** and **Arrow Lake-H**: detected by the shim, but untested.
  * **Xe-LPG** (Meteor Lake) has no XMX units.
* **Steam client:** Only the native Steam client is supported (SteamOS, distribution packages). Flatpak Steam is not supported.
* **Proton:** vkd3d-proton (Proton's D3D12-to-Vulkan translator) as in Proton 11 / GE-Proton 11 (tested with GE-Proton 11-6 and Proton Experimental).
* **System libraries:** The drivers in the archive (Mesa 26.3.0 unpatched, Mesa 26.1.2 and 26.3.0 with the patches) replace the system's 64-bit ANV (Mesa's Intel Vulkan driver) for the user session only. They are built on Arch Linux and need `glibc` 2.38 and `libwayland-client` 1.24 or newer.
  * Checked to load on SteamOS, Arch, openSUSE Tumbleweed, Void Linux, Fedora 43, and Ubuntu 25.10.
  * Debian 13 and Ubuntu 24.04 have `libwayland-client` 1.23 and need a driver built on the system with `tools/rebuild-driver.sh` (see [drivers.md](drivers.md) and [dev/building.md](dev/building.md)).
  * `SPIRV-Tools` and `libdisplay-info` 0.3 come with the archive (`lib/bundled/`) for systems that lack them.
  * For another Mesa version, see [dev/building.md](dev/building.md).
* **Tools for `install.sh`:**
  * `curl`, `ar` (binutils), `tar`, and `zstd` to unpack the compiler.
  * `python3` for the compiler helper, the session check, and `tools/fsr-fg.sh`.
  * `unzip` or `python3` for the XeSS SDK download.

## System-wide install

1. Download the release archive and unpack it into a permanent folder. (If you want to move the folder later, run `./uninstall.sh` first, move the folder, and run `./install.sh` again from the new location.)
2. Run the installer from the unpacked folder:

   ```sh
   ./install.sh
   ```

   This runs once. It fetches the compiler bundle (about 95 MB), Intel's XeSS SDK libraries (77 MB), and AMD's FidelityFX loader and providers (87 MB).
3. Reboot, or log out and log back in.

Every game started from Steam afterwards uses the package's driver with no launch options needed:

* In **DX12** games, select XeSS (and XeSS frame generation) in the game menu.
* In **DX11** or **Vulkan** games with XeSS, set the game's Steam launch options to `XESS=latest %command%` (see [games.md](games.md)).

The first launch of a game compiles its XMX kernels. This takes a minute or two once; after that, the compiled kernels load from `~/.cache/xess-xmx`.

## Updating and uninstalling

To revert everything:

```sh
./uninstall.sh
```

`uninstall.sh` reverts everything, and switches back every game modified with `tools/fsr-fg.sh`.

To update to a new release, unpack the new release archive over the previous version (into the same folder), run `./install.sh`, and reboot or log out and back in. Moving the package to another folder requires running `./uninstall.sh` from the old folder first, because the installation points to the folder.

If you install a new Vulkan driver on your system later (for example, for an eGPU or `lavapipe`), re-run `./install.sh` and log out and in so `VK_DRIVER_FILES` picks it up.

## What `install.sh` does

* **Writes `~/.config/environment.d/50-xess-xmx.conf`:**
  * Sets `VK_DRIVER_FILES` so the package's driver (`run/intel_icd.json`) is the 64-bit Intel Vulkan driver, while keeping other GPUs' drivers and the 32-bit Intel driver in the list. On Xe3, the default driver is unpatched Mesa 26.3 with this project's Vulkan layer (`layer/`); on Xe-HPG and Xe2, it is the patched Mesa 26.3 driver (see [drivers.md](drivers.md)).
  * Sets `ANV_CM_KERNEL_DIR` and `ANV_CM_HELPER` for the kernel compiler.
  * Sets `VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer,VK_KHR_present_wait2` for every D3D12 game in the session. The kernel binding code needs the classic descriptor heap (`VK_EXT_descriptor_buffer` disabled), and `VK_KHR_present_wait2` crashed games with frame generation.
  * Sets `force_vk_vendor=0`, overriding Mesa game profiles that hide the Intel GPU from some games (see [troubleshooting.md](troubleshooting.md)).
  * Sets `INTC_ALT_DRIVER_EXTENSIONS_PATH` to the prefix folder containing the shim (`C:\windows\system32\driverstore\filerepository\igd_faux.inf_1`) for Proton versions like Proton 11.0 that have no driver store.
* **Installs the shim (`igdext64.dll`) and `ControlLib.dll`:**
  * Puts `igdext64.dll` (the D3D12 extension shim) into every Proton that bundles an `igdext64.dll` and into every Wine prefix (the per-game Windows folder Steam creates under `compatdata/`), keeping the original file as `.stock`.
  * Puts `ControlLib.dll` into every prefix (`C:\windows\system32\ControlLib.dll`) so `XESS_FG` can override the frame generation multiplier (see [frame-generation.md](frame-generation.md)).
  * Enables a background watcher that repeats this after a Proton update and for new prefixes.
  * Covers every Steam library, including SD cards and secondary disks (`steamapps/libraryfolders.vdf`).
  * **Distribution-packaged Proton builds:** A Proton installed by a system package (such as CachyOS' `proton-cachyos` in `/usr/share/steam/compatibilitytools.d`) belongs to `root`. `install.sh` cannot write to it directly; it prints the `sudo` commands you need to run to put the shim in. Until you run them (and again after each package update), games using that Proton run on the DP4a fallback.
* **Installs the Proton launch-option hook:**
  * Puts a small `user_settings.py` into every Proton (`tools/proton-hook.py`). If you already have a `user_settings.py`, it is kept as `user_settings.user.py` and still runs.
  * This hook enables the `XESS=...`, `XESS_LOG=1`, `FSR=...`, `FSR_FG=...`, `FSR_VENDOR=...`, and `OFLOW_FG=1` launch options (see [launch-options.md](launch-options.md)).
* **Fetches third-party bundles:**
  * Runs `tools/get-igc.sh` to fetch Intel Graphics Compiler (IGC 2.10.10) and `ocloc` (compute-runtime 25.18) into `igc/`.
  * Runs `tools/get-xess.sh` to fetch Intel's XeSS SDK libraries into `xess/` (used by `XESS=latest`, `FSR=xess`, and `FSR_FG=xess`).
  * Runs `tools/get-ffx.sh` to fetch AMD's FidelityFX loader and providers into `ffx/` (used by `FSR=xess` and `FSR_FG=xess` on games with FSR 3.1.0–3.1.3).
* **Enables `xess-xmx-check.service`:**
  * Runs `~/.config/xess-xmx/session-check.sh` (`tools/session-check.sh`) before every graphical session to verify that the package's driver loads and finds the GPU.
  * Copies `SPIRV-Tools` and `libdisplay-info` from the host (or `lib/bundled/`) into `lib/deps/`, so the driver finds them inside the Steam Linux Runtime container.
  * Writes the result to `~/.cache/xess-xmx/driver-status`. If a system update broke the active driver, the other driver in the package takes over; if both are broken (or if the package folder was moved or deleted), the session falls back to the system's stock drivers and XeSS runs on DP4a (see [drivers.md](drivers.md)).

### Installer environment variables

| Variable | Effect |
|---|---|
| `STEAM_DIR=<path>` | For `install.sh` and `uninstall.sh`. Overrides the Steam directory. By default, the scripts use the first of `~/.local/share/Steam`, `~/.steam/steam`, or `~/.steam/root` that holds a Steam install (which also covers the Debian/Ubuntu `steam` package). |
| `XMX_DRIVER=patched\|stock\|auto` | For `install.sh`. Forces the session driver choice instead of picking by GPU (`stock` = unpatched Mesa 26.3 + layer; `patched` = patched Mesa 26.3; `auto` = default by GPU). Kept for later runs. Requires re-login. See [drivers.md](drivers.md). |
| `XMX_NO_ENV=1` | For `install.sh`. Skips writing the session environment (`50-xess-xmx.conf`) and enabling the session check service, while still installing the shim, the Proton hook, and the prefix watcher. |

If the compiler, XeSS SDK, or FidelityFX downloads need to be re-run later, you can run `tools/get-igc.sh`, `tools/get-xess.sh`, or `tools/get-ffx.sh` directly. (`IGC_TAG` and `NEO_TAG` select other compiler releases for `get-igc.sh`, though others are untested; `XESS_TAG` and `XESS_SHA256` select another XeSS SDK release for `get-xess.sh`.)

## Per-game install instead of system-wide

If you do not want to replace the Intel Vulkan driver for your entire desktop session, you can install the shim into a single game's prefix and use a launch wrapper for that game only.

1. Run `enable.sh` with the game's Steam App ID (or `XMX_PREFIX` for non-Steam launchers):

   ```sh
   XMX_APPID=<steam app id> ./enable.sh
   ```

   For another launcher, pass the Wine prefix path instead:

   ```sh
   XMX_PREFIX=<prefix> ./enable.sh
   ```

   `enable.sh` installs `igdext64.dll` and `ControlLib.dll` into that prefix, populates `lib/deps/`, and writes the environment variables to `~/.config/xess-xmx.conf`. If you have not run `install.sh` before, also fetch the compiler bundle once with `./tools/get-igc.sh`.

2. In Steam, set the game's launch options to:

   ```sh
   /path/to/package/xmx-launch.sh %command%
   ```

   If you pass per-game variables, put them before the wrapper:

   ```sh
   MESA=26.1 /path/to/package/xmx-launch.sh %command%
   ```

To remove the per-game install from that prefix:

```sh
XMX_APPID=<steam app id> ./disable.sh
```

(Or pass `XMX_PREFIX=<prefix>`.)

### Options and limitations of the per-game install

* **Script variables for `enable.sh` and `disable.sh`:**
  * `XMX_APPID` or `XMX_PREFIX`: one is required.
  * `XMX_CONF`: path to the config file (default `~/.config/xess-xmx.conf`).
  * `XMX_GAME`: process name; the script refuses to run while that process is running.
* **Proton hook shorthands are not available:** Because `enable.sh` does not install `user_settings.py` into Proton, the launch-option shorthands `XESS`, `XESS_LOG`, `FSR`, `FSR_FG`, and `OFLOW_FG` do not work with `xmx-launch.sh`:
  * Instead of `XESS=dp4a` and `XESS_LOG=1`, use their full environment variables (`IGDEXT_FORCE_FALLBACK=1`, `IGDEXT_TRACE=1 IGDEXT_XELL_LOG=1 IGDEXT_XEFG_LOG=1 XESS_FG_LOG=1`; see [launch-options.md](launch-options.md) and [dev/debugging.md](dev/debugging.md)).
  * Instead of `FSR=xess` and `FSR_FG=xess`, run `tools/fsr-fg.sh all <app id>` or `tools/fsr-fg.sh on <app id>` (requires `tools/get-xess.sh` and `tools/get-ffx.sh`; see [frame-generation.md](frame-generation.md)).
  * `XESS=latest`, `FSR_FG=inject`, and `OFLOW_FG=1` have no equivalent without the Proton hook (though for `XESS=latest` in a D3D12 game you can replace the game's XeSS DLLs by hand; see [games.md](games.md)).

## Using the system's own driver for one game (`xmx-stock.sh`)

**Experimental.** On an **Xe3** GPU, you can run a single game on your Linux distribution's own unpatched Mesa ANV driver with this project's Vulkan layer (`VK_LAYER_XESS_xmx` in `layer/`), which puts the XMX kernels in from outside the driver, while keeping the package's driver as the default for every other game. The layer serves Xe3 only. It knows Mesa 26.1, 26.3, and 27.0 (SteamOS ships 26.1.2); with another version, XeSS runs on DP4a.

`xmx-stock.sh` is a wrapper script rather than a launch variable because it must set the driver list before Steam's runtime container starts. Inside the container the driver list has already been rewritten, and changing it there loaded two Intel drivers at once.

1. Make sure the game's prefix has the shim and the package has the compiler bundle (`igc/`). A system-wide `./install.sh` provides both. Without a system-wide install, run `XMX_APPID=<app id> ./enable.sh` to put the shim in and `./tools/get-igc.sh` to fetch the compiler. (Do not combine `xmx-stock.sh` with `xmx-launch.sh`, whose file points at the patched driver.)
2. In the game's Steam launch options, put:

   ```sh
   /path/to/package/xmx-stock.sh %command%
   ```

   Put any launch variables before the wrapper:

   ```sh
   XESS=latest /path/to/package/xmx-stock.sh %command%
   ```

`xmx-stock.sh` installs nothing and changes nothing outside the game's environment. What it sets:

* `VK_DRIVER_FILES`: the system's Intel ICDs (`intel_icd.x86_64.json`, then `intel_icd.i686.json`, from `/usr/share/vulkan/icd.d` and the loader's other folders), followed by every other entry of the session's list (other GPUs' drivers), without the package's ICD (`run/intel_icd.json`, which `install.sh` puts first). Without a session list, it includes every ICD of the system. Setting `XMX_STOCK_ICD=<json>` uses that Intel ICD instead (for example, a Mesa built by hand).
* `VK_ADD_LAYER_PATH=<package>/layer` and `VK_LOADER_LAYERS_ENABLE=VK_LAYER_XESS_xmx`, added to any values already set so MangoHud and other layers keep working.
* `XMX_LAYER_HELPER=<package>/tools/cm-compile.sh` and `XMX_IGC_DIR=<package>/igc`, unless already set.
* `VK_EXT_descriptor_buffer` and `VK_KHR_present_wait2` added to `VKD3D_DISABLE_EXTENSIONS`, plus `force_vk_vendor=0` and `INTC_ALT_DRIVER_EXTENSIONS_PATH` as `install.sh` sets them, unless already set. `ANV_CM_KERNEL_DIR` and `ANV_CM_HELPER` stay as they are (the stock driver does not read them).

`xmx-stock.sh` writes one `xess-xmx: xmx-stock:` line naming the driver and the layer to Steam's output (the terminal Steam was started from, or the user journal in Game Mode), not to `PROTON_LOG`'s file. If the layer files or a 64-bit Intel ICD of the system are missing, that line says so and the game starts unchanged (on the patched driver after `install.sh`). `MESA` has no effect with `xmx-stock.sh`.

For the layer's own log, set `XMX_LAYER_LOG=1` (stderr) or `XMX_LAYER_LOG=<file>`. Its other switches are in [../layer/README.md](../layer/README.md). See also [launch-options.md](launch-options.md) and [drivers.md](drivers.md).
