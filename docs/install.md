# Installing, Updating, and Uninstalling

This guide covers how to install `xess-xmx-linux` system-wide for your user session, how to update or remove it, what the installer changes on your system, and how to set it up for a single game instead.

---

## System-Wide Install

1. Download the release archive and unpack it into a folder where it can stay. (If you want to move the folder later, run `./uninstall.sh` first.)
2. Run the installer from inside that folder:
   ```bash
   ./install.sh
   ```
   On first run, `install.sh` downloads the Intel Graphics Compiler bundle (`igc/`, about 95 MB), Intel's XeSS SDK libraries (`xess/`, about 77 MB), and AMD's FidelityFX loader and providers (`ffx/`, about 87 MB).
3. Reboot, or log out of your desktop session and log back in, so Steam picks up the new session environment variables.

Every Direct3D 12 (DX12) game you start from Steam afterwards uses the package's driver automatically without any Steam launch options. In the game's graphics settings, select the **DX12** renderer and enable **XeSS** (and **XeSS Frame Generation** if offered).

For Direct3D 11 (DX11) or Vulkan games with XeSS, or games that ship an older XeSS version, add `XESS=latest %command%` to the game's launch options ([games.md](games.md), [launch-options.md](launch-options.md)).

The first time you launch a game with XeSS enabled, the driver (or the Vulkan layer) compiles that game's C-for-Metal (CM, Intel's GPU kernel language) kernels into `~/.cache/xess-xmx/kernels`. Startup can pause for a minute or two once; after that, the compiled kernels load from the cache.

---

## Updating and Uninstalling

### Uninstalling

To revert everything `install.sh` did, run:

```bash
./uninstall.sh
```

Then reboot or log out and back in so Steam drops the session environment variables.

`uninstall.sh` does the following:
* Removes `~/.config/environment.d/50-xess-xmx.conf`.
* Disables and removes the systemd user units `xess-xmx-shim.path`, `xess-xmx-shim.service`, and `xess-xmx-check.service`. It also removes `~/.config/xess-xmx/session-check.sh`.
* Restores every backed-up `igdext64.dll.stock` in your Proton installations and game prefixes.
* Removes `ControlLib.dll` and the `user_settings.py` Proton hook (restoring any `user_settings.user.py` of your own).
* Undoes any leftover `XESS` / `FSR` file links or `tools/fsr-fg.sh` changes in your game folders.
* Deletes the `drive_c/igdext_kernels` folders inside your prefixes.

`uninstall.sh` leaves the unpacked package folder and the downloaded bundles (`igc/`, `xess/`, `ffx/`). It also leaves per-game `enable.sh` blocks in `~/.config/xess-xmx.conf` (use `./disable.sh` for those). The compiled kernel cache in `~/.cache/xess-xmx` stays as well (delete that folder if you want to remove the cached kernels).

### Updating or Moving the Package Folder

* **Updating to a new release:** Unpack the new release over the previous version (into the same folder), run `./install.sh`, and reboot or re-login. If the compiler toolchain or `.cmk` packer format changed, `xess-xmx-check.service` removes the patched driver's `*.cmk` files from the old toolchain in `~/.cache/xess-xmx/kernels` (the layer's `*.cml` files are not removed; new ones get new file names); the driver or layer compiles them again on next use.
* **Moving or deleting the package folder:** Before you move or delete the package folder, run `./uninstall.sh` from it; after moving, run `./install.sh` from the new location. If you delete or move the folder without running `./uninstall.sh`, `~/.config/xess-xmx/session-check.sh` detects that the folder is missing at your next login, points the session back to the system's stock Vulkan drivers so your desktop and games keep working (on DP4a, the 8-bit integer dot-product fallback), and writes `FAILED ... package folder ... not found` to `~/.cache/xess-xmx/driver-status`.

---

## Requirements

### Hardware

* An Intel GPU with **XMX** (Xe Matrix Extensions, hardware matrix engines) running on the Linux `xe` or `i915` kernel driver:
  - **Xe3 (Panther Lake, Wildcat Lake):** The shim's GPU table (`shim/src/dll/GpuInfo.cpp`) covers PCI IDs `0xB080`–`0xB0FF` and `0xFD80`–`0xFD8F`. Both `install.sh` and `VK_LAYER_XESS_xmx` use the same list, `layer/gpu-ids.txt`, with the IDs Mesa 26.3 lists: Panther Lake `0xB080`–`0xB087`, `0xB08F`, `0xB090`, `0xB0A0`, `0xB0B0` and Wildcat Lake `0xFD80`, `0xFD81`. Only those IDs get stock mode by default and are served by `VK_LAYER_XESS_xmx`. Another ID of the shim's ranges is detected by the shim and runs on the patched driver. Tested on an Arc B390 (ONEXPLAYER 3); Wildcat Lake is detected but untested.
  - **Xe2 (Battlemage, Lunar Lake):** The shim covers PCI IDs `0xE200`–`0xE2FF` (Battlemage, tested by a user on an Arc B580 with the patched driver, [#16](https://github.com/HHHHanasak1/xess-xmx-linux/issues/16)) and `0x6420`–`0x64FF` (Lunar Lake, detected, untested). `install.sh` and `VK_LAYER_XESS_xmx` serve the Xe2 IDs Mesa 26.3 lists (Lunar Lake `0x6420`, `0x64A0`, `0x64B0`; Battlemage `0xE202`, `0xE209`, `0xE20B`–`0xE20D`, `0xE210`–`0xE212`, `0xE215`, `0xE216`, `0xE220`–`0xE223`). Those IDs get stock mode by default, with every kernel compiled for 128 GRF. Stock mode on Xe2 is untested on real Xe2 hardware.
  - **Xe-HPG (Alchemist / DG2):** PCI IDs `0x5690`–`0x56FF`. Tested by a user on an Arc A310 ([#28](https://github.com/HHHHanasak1/xess-xmx-linux/issues/28)).
  - **Xe-LPG+ (Arrow Lake-H):** PCI IDs `0x7D51`, `0x7DD1` (detected, untested).
* **Xe-LPG (Meteor Lake and non-H Arrow Lake, `0x7D40`–`0x7DFF` except `0x7D51` / `0x7DD1`)** has no XMX units; the shim's trace shows `XMX 0` and games stay on DP4a. Unknown Intel PCI IDs automatically fall back to DP4a unless declared with `IGDEXT_GMD` ([troubleshooting.md](troubleshooting.md)).

### Software

* **Native Linux Steam client** (SteamOS, or a distribution package). **Flatpak Steam is not supported**: its sandbox blocks access to `~/.config/environment.d` and the package folder.
* **Proton 11 / GE-Proton 11 / Proton Experimental** (for their `vkd3d-proton`, Proton's D3D12-to-Vulkan translation layer).
* **System libraries for the prebuilt drivers:** The drivers in the release archive (`lib/mesa-stock-26.3.0/`, `lib/mesa-26.3.0/`, `lib/mesa-26.1.2/`) are built on Arch Linux and require **glibc 2.38** and **libwayland-client 1.24** or newer.
  - Checked to load on SteamOS, Arch Linux, openSUSE Tumbleweed, Void Linux, Fedora 43, and Ubuntu 25.10.
  - Debian 13 and Ubuntu 24.04 ship `libwayland-client` 1.23 (`undefined symbol: wl_fixes_interface`). On those distributions, build the driver against your system libraries with `tools/rebuild-driver.sh` ([drivers.md](drivers.md), [dev/building.md](dev/building.md)). On Xe3 and Xe2, then switch to it with `XMX_DRIVER=patched ./install.sh`.
  - `libSPIRV-Tools.so` and `libdisplay-info.so.3` are bundled in `lib/bundled/` for distributions that lack them or name them differently.
* **Command-line tools for `install.sh`:**
  - To unpack the compiler bundle: `curl`, `ar` (from `binutils`), `tar`, and `zstd`.
  - For the Proton hook, compiler helper, session check, and `tools/fsr-fg.sh`: `python3`.
  - To unpack the XeSS SDK download: `unzip` or `python3`.

---

## What `install.sh` Does

1. **Fetches runtime bundles (if not already present):**
   - `igc/` (~95 MB via `tools/get-igc.sh`): Intel Graphics Compiler `v2.10.10` and `ocloc` `25.18.33578.6` for runtime CM kernel compilation.
   - `xess/` (~77 MB via `tools/get-xess.sh`): Intel's XeSS SDK `v3.0.2` runtime DLLs. They are used when a game's launch options include `XESS=latest`, `FSR=xess`, `FSR_FG=xess`, or `OFLOW_FG=1`. The DLLs are `libxess.dll` 2.0.2.68, `libxess_dx11.dll`, `libxess_fg.dll` 1.3.1.78, and `libxell.dll` 1.3.2.10.
   - `ffx/` (~87 MB via `tools/get-ffx.sh`): AMD's signed FidelityFX SDK `v2.4.0` loader (`amd_fidelityfx_loader_dx12.dll`), upscaler, and frame generation DLLs, used when `FSR=xess` or `FSR_FG=xess` is set on a game with FSR 3.1.0–3.1.3 (which ships a single `amd_fidelityfx_dx12.dll`).
2. **Writes `~/.config/environment.d/50-xess-xmx.conf`** (and applies the variables to the current user `systemd` manager):
   - `VK_DRIVER_FILES`: puts the package's 64-bit Intel Vulkan ICD (`run/intel_icd.json`) first while keeping every other GPU's Vulkan driver (for hybrid laptops, eGPUs, or `lavapipe`) and the 32-bit Intel driver (`intel_icd.i686.json` / `intel_icd.i386.json`) in the list.
   - `VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer,VK_EXT_descriptor_heap,VK_KHR_present_wait2`: the kernel binding code requires `vkd3d-proton`'s classic descriptor heap rather than `VK_EXT_descriptor_buffer` or the `VK_EXT_descriptor_heap` path of vkd3d-proton 3.1 and newer, and disabling `VK_KHR_present_wait2` prevents crashes in `vkWaitForPresent2KHR` under frame generation's high present rate. This applies to every D3D12 game in the session.
   - `ANV_CM_KERNEL_DIR=<package>/kernels` and `ANV_CM_HELPER=<package>/tools/cm-compile.sh`.
   - `force_vk_vendor=0`: overrides Mesa game profiles (`drirc`) that set `force_vk_vendor=-1` to hide the Intel GPU from certain games ([troubleshooting.md](troubleshooting.md)).
   - `INTC_ALT_DRIVER_EXTENSIONS_PATH='C:\windows\system32\driverstore\filerepository\igd_faux.inf_1'`: tells XeSS where to load `igdext64.dll` on Protons that do not set up an Intel driver store in the registry (such as Valve's Proton 11.0).
   - **Driver mode selection:** On **Xe3** and **Xe2** (the Lunar Lake and Battlemage IDs above), `install.sh` defaults to `stock` mode. Stock mode runs the package's unpatched Mesa 26.3.0 driver with `VK_LAYER_XESS_xmx`. It sets `MESA=stock-26.3.0`, `VK_ADD_LAYER_PATH=<package>/layer`, and `VK_LOADER_LAYERS_ENABLE=VK_LAYER_XESS_xmx`. It also sets `XMX_LAYER_HELPER` and `XMX_IGC_DIR`. On Xe2 the layer compiles every kernel for 128 GRF, the register count an unpatched driver runs them with there ([drivers.md](drivers.md)). On **all other GPUs** (Xe-HPG, and Xe3 / Xe2 IDs outside `layer/gpu-ids.txt`), `install.sh` defaults to `patched` mode, running the patched driver named in `lib/active`. You can override this choice with `XMX_DRIVER=patched ./install.sh`, `XMX_DRIVER=stock ./install.sh`, or `XMX_DRIVER=auto ./install.sh`. (`XMX_DRIVER=stock` is accepted on any GPU, but on a GPU the layer does not serve, it declines its self-test and XeSS runs on DP4a; see [drivers.md](drivers.md).)
3. **Installs `igdext64.dll`, `ControlLib.dll`, and the Proton hook across all Steam libraries:**
   - Finds your main Steam installation and every secondary Steam library in `steamapps/libraryfolders.vdf` (SD cards, second disks). The main installation is `$STEAM_DIR`, or the first existing folder among `~/.local/share/Steam`, `~/.steam/steam`, `~/.steam/root`, and `~/.steam/debian-installation`.
   - Replaces `igdext64.dll` in every Proton that bundles one and in every existing game prefix that already has the `igd_faux.inf_1` folder, backing up the original file as `igdext64.dll.stock`. In a Proton, the file is `files/lib/wine/igdext/x86_64-windows/igdext64.dll`. In a prefix, the folder is `drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1` (matching both `driverstore` and `DriverStore` casing).
   - In every prefix that already has that `igd_faux.inf_1` folder, copies `ControlLib.dll` into its `drive_c/windows/system32/` for `XESS_FG`. It leaves any existing `ControlLib.dll` that is not this package's alone (see [frame-generation.md](frame-generation.md)). On a manual run with the `igc/` compiler bundle present, it also removes any `drive_c/igdext_kernels/compile_failed.txt` markers so previously failed compiles are retried.
   - Places a small `user_settings.py` into every Proton folder (keeping any existing `user_settings.py` of your own as `user_settings.user.py`). At game start, this `user_settings.py` runs `tools/proton-hook.py`. The hook handles the `XESS=`, `XESS_LOG=`, and `FSR=` launch options. It also handles `FSR_FG=` and `OFLOW_FG=` ([launch-options.md](launch-options.md)).
   - **Protons installed by a distribution package:** A Proton in `/usr/share/steam/compatibilitytools.d` or `/usr/local/share/steam/compatibilitytools.d` (such as CachyOS's `proton-cachyos`) is owned by `root`. As your user, `install.sh` cannot write to it; it lists those Proton paths and prints the `sudo cp` commands to install `igdext64.dll` into them. Until you run those commands (or use a Proton in `~/.local/share/Steam/compatibilitytools.d`), games using that system Proton run on DP4a. The `user_settings.py` hook options (`XESS=latest`, `FSR=xess`, `OFLOW_FG=1`) will not run on it either.
4. **Enables the shim watcher (`xess-xmx-shim.path` and `xess-xmx-shim.service`):**
   - Watches your Proton and `compatdata` folders across all Steam libraries and automatically re-installs `igdext64.dll`, `ControlLib.dll`, and `user_settings.py` whenever a Proton updates or a newly launched game creates a prefix.
5. **Enables the pre-session driver check (`xess-xmx-check.service`):**
   - Runs `~/.config/xess-xmx/session-check.sh` (`tools/session-check.sh`) before every graphical session starts.
   - Copies `libSPIRV-Tools.so` and `libdisplay-info.so.3` from the host (or from `lib/bundled/`) into `lib/deps/` so the driver can load inside the Steam Linux Runtime (`pressure-vessel`) container.
   - Verifies that the package's driver loads and enumerates the GPU. In a patched session, if a system update broke the active driver, it switches to the other patched driver of the package (`lib/mesa-26.1.2` or `lib/mesa-26.3.0`); if none loads, the session falls back to the system's drivers. In a stock session, if the check fails, the session falls back to the system's drivers. It writes the result to `~/.cache/xess-xmx/driver-status` ([drivers.md](drivers.md)).

### Installer Environment Options

* `STEAM_DIR=/path/to/Steam ./install.sh` (also for `uninstall.sh`): overrides the Steam root directory.
* `XMX_DRIVER=patched|stock|auto ./install.sh`: chooses the session driver mode and remembers it in `run/driver-mode` ([drivers.md](drivers.md)).
* `XMX_NO_ENV=1 ./install.sh`: installs the shim, `ControlLib.dll`, the Proton hooks, and the shim watcher, but skips writing `~/.config/environment.d/50-xess-xmx.conf` and `xess-xmx-check.service` (useful if you want to enable the driver per game instead of session-wide).

---

## Per-Game Install Instead of System-Wide (`enable.sh` / `disable.sh`)

If you prefer not to set the driver for your entire desktop session, you can install the shim into a single game's prefix and use a launch wrapper for that game:

1. Run `enable.sh` with either the game's Steam App ID (`XMX_APPID`, searched across all Steam libraries) or the path to a Wine/Proton prefix folder (`XMX_PREFIX`, for non-Steam launchers; the folder containing `drive_c`):
   ```bash
   XMX_APPID=<steam app id> ./enable.sh
   # or, for another launcher:
   XMX_PREFIX=/path/to/prefix ./enable.sh
   ```
   (Start the game once first if its prefix does not exist yet. If you have not run `install.sh`, also run `./tools/get-igc.sh` once to download the kernel compiler.)
2. `enable.sh` backs up and replaces `igdext64.dll` in that prefix and copies `ControlLib.dll` into its `system32`. It then runs `tools/session-check.sh --dry-run` (which also populates `lib/deps/`). Finally, it writes the environment variable block to `~/.config/xess-xmx.conf`.
3. Set the game's Steam launch options to:
   ```text
   /path/to/package/xmx-launch.sh %command%
   ```
   Any per-game variables go before the wrapper:
   ```text
   MESA=26.1 XESS_FG=4 /path/to/package/xmx-launch.sh %command%
   ```
4. To remove the shim from that prefix and delete the block from `~/.config/xess-xmx.conf`, run:
   ```bash
   XMX_APPID=<steam app id> ./disable.sh
   # or:
   XMX_PREFIX=/path/to/prefix ./disable.sh
   ```

Optional variables for `enable.sh`, `disable.sh`, and `xmx-launch.sh`:
* `XMX_CONF=/path/to/file`: overrides the config file path (default `~/.config/xess-xmx.conf`).
* `XMX_GAME=<process name>`: if set, `enable.sh` and `disable.sh` refuse to run while a process matching that name (first 15 characters, case-insensitive) is running.

Note: Without `install.sh`, Proton's `user_settings.py` hook is not installed, so the hook shorthands are not available. These shorthands are:
* `XESS=latest`, `XESS=dp4a`, and `XESS_LOG=1`
* `FSR=xess`, `FSR_FG=xess`, and `FSR_FG=inject`
* `OFLOW_FG=1`

Use these instead ([frame-generation.md](frame-generation.md), [launch-options.md](launch-options.md)):
* `IGDEXT_FORCE_FALLBACK=1` instead of `XESS=dp4a`
* the `IGDEXT_*` log variables instead of `XESS_LOG=1`
* `tools/fsr-fg.sh` for games that have FSR 3.1+ frame generation

---

## Using the System's Own Driver for One Game (`xmx-stock.sh`)

**Experimental.** If you want a single game to run on your Linux distribution's own unpatched Mesa ANV driver (instead of the drivers in this package) while still running XeSS on XMX through the package's Vulkan layer `VK_LAYER_XESS_xmx` (`layer/`), put `xmx-stock.sh` in the game's Steam launch options:

```text
/path/to/package/xmx-stock.sh %command%
```

Put any launch option variables before the wrapper:

```text
XESS=latest /path/to/package/xmx-stock.sh %command%
```

`xmx-stock.sh` installs nothing and changes nothing outside that game's process environment:
* It must be used as a command wrapper around `%command%` because `VK_DRIVER_FILES` has to be rewritten before Steam's Linux Runtime container (`pressure-vessel`) starts; changing `VK_DRIVER_FILES` inside the container loads two Intel drivers at once.
* The game's prefix must have `igdext64.dll` and the package must have `igc/`. Running `./install.sh` sets up both. Without `./install.sh`, run `XMX_APPID=<app id> ./enable.sh` and `./tools/get-igc.sh`, and do **not** combine `xmx-stock.sh` with `xmx-launch.sh`.
* `VK_LAYER_XESS_xmx` supports the **Xe3** and **Xe2** GPUs of `layer/gpu-ids.txt` (on Xe2 kernels compiled for 128 GRF; untested on real Xe2 hardware) on unpatched Mesa **26.1**, **26.3**, and **27.0** (SteamOS ships 26.1.2). On other GPUs or other Mesa versions, the layer leaves the self-test unanswered and XeSS runs on DP4a.
* It writes one `xess-xmx: xmx-stock:` line naming the driver and the layer to Steam's output (the terminal Steam was started from; in Game Mode, the user journal), not to `PROTON_LOG`'s file. If the layer files or a 64-bit Intel ICD of the system are missing, that line says so and the game starts unchanged (on the session's driver: the package's unpatched driver + layer in stock mode on Xe3 and Xe2, or the patched driver otherwise).
* `MESA` has no effect when using `xmx-stock.sh`.

See [drivers.md](drivers.md) and [launch-options.md](launch-options.md) for full details on `xmx-stock.sh` and its variables. These variables are `XMX_STOCK_ICD`, `XMX_LAYER_LOG`, and `XMX_LAYER_PROF`.
