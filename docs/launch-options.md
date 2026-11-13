# Steam Launch Options

This page lists every Steam launch option you can set for a single game (**Properties → General → Launch options**). Put variables before `%command%`, separated by spaces:

```sh
MESA=26.1 XESS=latest %command%
```

A launch option applies only to that game and takes effect the next time you start it. You do not need to log out or reboot. `install.sh` writes the system-wide defaults to `~/.config/environment.d/50-xess-xmx.conf` ([install.md](install.md)); a launch option overrides them for that game.

## How the Proton Hook Works

The following options need the system-wide install ([install.md](install.md)):

* `XESS` and `XESS_LOG`
* `FSR`, `FSR_FG`, and `OFLOW_FG`
* their modifiers `XESS_VK` and `FSR_VENDOR`

`install.sh` puts a small `user_settings.py` into every Proton it can write to, which Proton runs before it starts a game (`tools/proton-hook.py`). If you already had a `user_settings.py` of your own, `install.sh` keeps it as `user_settings.user.py` next to it so your settings still apply. Any variable you set explicitly in the launch options wins over the variable a shorthand expands to.

The hook swaps files in the game's folder, which Steam gives it (`STEAM_COMPAT_INSTALL_PATH`). For a non-Steam game added as a shortcut, Steam may give none; the hook then uses the folder of the `.exe` the shortcut starts (and everything below it), so point the shortcut at the game's own executable. It refuses a folder that is too wide to touch (`/`, your home folder or one above it, the prefix's `windows` folder) and starts the game with its own libraries.

If you set `VKD3D_DISABLE_EXTENSIONS` in the launch options (it replaces the system-wide value), the hook adds the extensions the XMX kernels need (`VK_EXT_descriptor_buffer`, `VK_EXT_descriptor_heap`, `VK_KHR_present_wait2`) to your list while the package's driver or layer is active; your own entries stay.

With the per-game install (`enable.sh`, [install.md](install.md)), put the wrapper last, after your variables:

```sh
MESA=26.1 /path/to/xess-xmx-linux/xmx-launch.sh %command%
```

With the per-game install (or on a root-owned distribution Proton where `install.sh` could not install `user_settings.py`), these five shorthands are not available:
* Instead of `XESS=dp4a` and `XESS_LOG=1`, pass their underlying variables (`IGDEXT_*`, `XESS_FG_LOG`) ([dev/debugging.md](dev/debugging.md)).
* Instead of `FSR=xess` and `FSR_FG=xess`, run `tools/fsr-fg.sh` ([frame-generation.md](frame-generation.md)). Note that `tools/fsr-fg.sh all` only works on game folders that have the FSR frame generation DLL or the single `amd_fidelityfx_dx12.dll`. It refuses folders with only `amd_fidelityfx_upscaler_dx12.dll`.
* `FSR_FG=inject`, `OFLOW_FG`, and the D3D11 and Vulkan parts of `XESS=latest` have no alternative without the hook. For old D3D12 XeSS, replace the DLLs by hand ([games.md](games.md#replacing-the-files-manually-without-the-hook)). (`XESS_FG` does not need the hook because `ControlLib.dll` and `igdext64.dll` read it inside the game process.)

---

## Driver

See [drivers.md](drivers.md) for the drivers in the package and how to switch the default across all games (`tools/switch-driver.sh`, `XMX_DRIVER`), and [install.md](install.md) for the full details of `xmx-stock.sh`.

| Launch option | What it does | When |
|---|---|---|
| `MESA=26.1 %command%` | Runs this game on the package's patched Mesa 26.1.2 driver | A game misbehaves on Mesa 26.3 only (please report it) |
| `MESA=26.3 %command%` | Runs this game on the package's patched Mesa 26.3 driver | You changed the default to 26.1 and one game should keep 26.3, or on Xe3 (`0xB080`–`0xB08F`, `0xFD80`–`0xFD8F`) you want to compare the patched 26.3 driver against the unpatched default |
| `/path/to/package/xmx-stock.sh %command%` | **Experimental**: runs this game on your distribution's own unpatched Mesa ANV (Intel's Vulkan driver) with the package's Vulkan layer `VK_LAYER_XESS_xmx` (`layer/`), which injects the XMX (Xe Matrix Extensions) kernels from outside the driver; the session's default driver stays in place for every other game | You want to keep your distribution's own Mesa for that game, or test the layer against the patched driver. The layer supports Xe3 GPUs with PCI device IDs `0xB080`–`0xB08F` or `0xFD80`–`0xFD8F` on Mesa 26.1, 26.3, and 27.0 (SteamOS ships 26.1.2); with another Mesa version or GPU, XeSS runs on DP4a (dot-product fallback) |
| `INTEL_DEBUG=no-jay %command%` | Compiles the game's own shaders on Mesa 26.3 with the older `brw` compiler instead of `Jay` (XeSS is not affected) | A rendering bug or crash that goes away with `MESA=26.1`: tells whether Jay is the cause |

`MESA` takes a version or a prefix of one (`26.1`, `26.3`, `26.1.2`). An unknown version falls back to the default driver and prints `xess-xmx: MESA=...: no such driver` to the game's stderr.

`xmx-stock.sh` is a command wrapper, not a variable, because it must rewrite `VK_DRIVER_FILES` before Steam's runtime container (`pressure-vessel`) starts. Put variables before the wrapper:

```sh
XESS=latest /path/to/package/xmx-stock.sh %command%
```

If the layer files or a 64-bit system Intel ICD (Installable Client Driver) are missing, `xmx-stock.sh` logs a line to Steam's output and starts the game unchanged on the session's driver. On Xe3 (`0xB080`–`0xB08F` / `0xFD80`–`0xFD8F`) and Xe2, that is the package's unpatched driver + layer in stock mode. Otherwise it is the patched driver. `MESA` has no effect when using `xmx-stock.sh`. To use a custom unpatched Intel ICD JSON file instead of the system's, set `XMX_STOCK_ICD=<json>[:<json>]` before the wrapper. The layer has logging and profiling switches: `XMX_LAYER_LOG=1` or `<file>`, `XMX_LAYER_PROF=1|2`, and `XMX_LAYER_PROF_SEC=<sec>`, and `XMX_LAYER_FORCE_GRF=128` (Xe3: compile at 128 GRF as on Xe2, for comparisons). For these, see [layer/README.md](../layer/README.md) and [dev/debugging.md](dev/debugging.md). For full setup details of `xmx-stock.sh`, see [install.md](install.md).

---

## XeSS Super Resolution

See [games.md](games.md) for per-game guides, why DXVK (DirectX-to-Vulkan) hides the Intel GPU from XeSS older than 2.0.2.68, and how the D3D11 (Direct3D 11) and Vulkan paths work.

| Launch option | What it does | When |
|---|---|---|
| `XESS=latest %command%` | Runs the game with the XeSS libraries of Intel's XeSS SDK 3.0.2 in place of its older ones. The SDK files are in `xess/`, fetched by `install.sh` / `tools/get-xess.sh` (XeSS 2.0.2.68, XeSS-FG 1.3.1.78, XeLL 1.3.2.10). For a **D3D11** game, replaces `libxess_dx11.dll` with the package's D3D11-on-D3D12 bridge (`dx11bridge/libxess_dx11.dll`, [games.md](games.md#xess-in-a-d3d11-game)). For a **Vulkan** game, places the package's `vulkan-1.dll` (`vkwrap/`) next to the executable and enables the layer so XeSS runs on XMX on Xe3 ([games.md](games.md#xess-in-a-vulkan-game)). Xe3 here means `0xB080`–`0xB08F` and `0xFD80`–`0xFD8F`. Restores the game's original files when it exits | A game with XeSS older than the SDK's (required below 2.0.2.68, such as Cyberpunk 2077), a D3D11 game with XeSS (or OptiScaler's XeSS in a D3D11 game), or a Vulkan game with XeSS |
| `XESS_VK=1` / `XESS_VK=0` (with `XESS=latest`) | Forces the Vulkan part of `XESS=latest` on (`1`) for a game whose executable loads `vulkan-1.dll` dynamically at runtime rather than importing it in its PE header, or turns it off (`0`) | `XESS_VK=1`: a Vulkan game with XeSS that stays on DP4a with `XESS=latest` alone. `XESS_VK=0`: a non-Vulkan game where the Vulkan wrapper gets in the way |
| `XESS=dp4a %command%` | Sets `IGDEXT_FORCE_FALLBACK=1` so the shim declines Intel's extension: XeSS super resolution runs on DP4a and XeSS frame generation runs on its generic path (2x only) | Comparing XMX against DP4a, or ruling out the XMX path as the cause of a problem |

With `XESS=latest`, each `libxess.dll`, `libxess_fg.dll`, and `libxell.dll` in the game folder is compared with the SDK's. If it is older, it is renamed to `<name>.xmx-orig` and replaced, while the game runs, by a link to a copy of the SDK's file made for that run (in `~/.cache/xess-xmx/xess-swap/`; a launcher that repairs the game folder through the link then cannot delete the package's file). Files as new as or newer than the SDK's are left alone. Every `libxess_dx11.dll` is replaced by `dx11bridge/libxess_dx11.dll` regardless of its version (with the SDK's `libxess.dll` added next to it if missing). When the game exits (or on the next start if the game was killed outright), the originals are put back so Steam updates and file verification see the unmodified game ([games.md](games.md)).

---

## Frame Generation and Low Latency

See [frame-generation.md](frame-generation.md) for full instructions, tested games, HUD/camera notes, and all `FFX_XESS_*` / `FFX_XEFG_*` tuning variables.

| Launch option | What it does | When |
|---|---|---|
| `XESS_FG=3 %command%`, `XESS_FG=4 %command%` | Overrides XeSS frame generation to produce 3x or 4x frames (`XESS_FG=2` for 2x) through `ControlLib.dll` and the shim, regardless of what the game's menu offers or selects; you still switch frame generation on and off in the game menu | A game that offers only 2x in its menu (such as Cyberpunk 2077), or setting the multiplier for `FSR=xess`, `FSR_FG=xess`, or `OFLOW_FG=1`. Requires XeSS-FG 1.2 or newer (combine with `XESS=latest` if older) |
| `XESS_FG=5` ... `XESS_FG=8 %command%` | **Experimental**: 5x to 8x (the maximum), raised by the shim inside `libxess_fg.dll` at runtime beyond Intel's supported 4x limit. All work; 7x and 8x are not recommended due to high latency | Filling a high-refresh-rate display that 4x does not fill |
| `FSR=xess %command%` | Runs the game's FSR 3.1+ upscaling on XeSS super resolution (`amd_fidelityfx_upscaler_dx12.dll`) and its FSR frame generation on XeSS frame generation (`amd_fidelityfx_framegeneration_dx12.dll`); combine with `XESS_FG=<n>` for multi-frame generation | A DX12 game with FSR 3.1+ / FSR 4 and no native XeSS. If a game has native XeSS, use that instead; translating FSR can flicker more ([frame-generation.md](frame-generation.md)) |
| `FSR_FG=xess %command%` | Runs only the game's FSR 3.1+ frame generation on XeSS frame generation; keeps the game's own FSR upscaling | Same as `FSR=xess`, when you want to keep the game's native FSR upscaler |
| `FSR=xess FSR_FG=inject %command%` | Injects an XeSS-FG swap chain (`dxgi.dll` + `ffx_xefg_bridge.dll`) fed from the `FSR=xess` upscaler into a game with FSR 3.1+ upscaling and no FSR frame generation. Choose FSR upscaling in the game; the HUD is interpolated too ([frame-generation.md](frame-generation.md)) | A game whose frame generation is NVIDIA DLSS-G only, such as Forza Horizon 6 |
| `OFLOW_FG=1 %command%` | **Experimental**: injects XeSS frame generation (`dxgi.dll` + `ffx_xefg_bridge.dll`) into any DX12 game using motion vectors computed from FidelityFX optical flow on the final image instead of game motion vectors (`FFX_XEFG_OFLOW=1`). `XESS_FG=<n>` sets the frames shown per rendered frame; the HUD is interpolated too, and the rendered frame rate is paced automatically ([frame-generation.md](frame-generation.md)) | A DX12 game with neither XeSS nor FSR 3.1+, such as Resident Evil 4 (2023) or Viewfinder (`-force-d3d12` after `%command%`) |
| `FSR_VENDOR=amd %command%` | With `FSR=xess` or `FSR_FG=xess`: adds `dxgi.customVendorId = 1002` to `DXVK_CONFIG` so DXVK reports an AMD GPU to the game, while the package's DLLs still report Intel's `0x8086` vendor ID to XeSS-FG and XeLL | A game that offers FSR in its menu only on AMD GPUs, such as Wuthering Waves (which should rather use its own native XeSS) |
| `IGDEXT_XELL_CAP=off %command%` | Turns the low latency off. It is on by default while frame generation runs, on every path: a game's own XeSS-FG, the FSR routes (`FSR=xess`, `FSR_FG`) and `OFLOW_FG`. Off means no wait where a frame starts, and the swap chain answers the game itself (with `OFLOW_FG`, the frame cap stays; `FFX_XEFG_CAP=0` turns that off) | Comparing latency and frame rate, or if the low latency costs frame rate in a game |
| `IGDEXT_XELL_CAP=auto:<ms> %command%` | Keeps the low latency on, starting each frame `<ms>` milliseconds earlier (or later if negative): gives more frame-rate headroom at the cost of a little more latency | A game whose frame rate drops with the default low latency on |
| `IGDEXT_XELL_CHECK=0 %command%` | Keeps the low latency's wait without checking it. By default the wait is left out now and then for about 100 frames, and stays only if it costs no more than 1% of the frame rate and lowers the latency | Ruling out the check when debugging |
| `IGDEXT_XELL_POLLS=0 %command%` | The swap chain answers the game's per-frame questions itself (`GetFrameStatistics`, `CheckColorSpaceSupport`). By default the game gets the answers as of its last Present, without waiting for XeSS-FG | Ruling out the answers when debugging |
| `IGDEXT_XELL_CAP=<us> %command%` | Instead of the low latency's wait, raises `minimumIntervalUs` to at least `<us>` microseconds per displayed frame when the game calls `xellSetSleepMode`. Valid values: `1` to `1000000`; `0` = off. Any other value turns both off. If a game never calls `xellSetSleepMode`, no cap is applied | Locking a steady fixed frame rate through XeLL |
| `IGDEXT_XELL_FIXCAP=1 %command%` | Applies the XeLL 30 fps frame-cap fix (ignoring a non-zero cap set within 1.5 s of enabling frame generation) in this game | Another game shows the Wuthering Waves symptom (30 fps with frame generation until the pause menu is opened and closed) |
| `IGDEXT_XELL_KEEPCAP=1 %command%` | Turns off the XeLL 30 fps frame-cap fix (by default it is on for Wuthering Waves only) | Testing whether the cap fix causes a problem |

Keep a frame rate limiter on (the game's own or gamescope's) when using frame generation so generated frames reach the screen evenly, and run in windowed or borderless mode (XeSS-FG refuses exclusive fullscreen). The low latency (`IGDEXT_XELL_CAP=auto`) is one feature on all frame generation paths, with the same options and the same log lines. For a game's own XeSS frame generation the shim holds the game back in XeLL's sleep. On the FSR routes (`FSR=xess`, `FSR_FG`) the package's frame generation DLL holds it back in Present. With `OFLOW_FG`, the DLL's frame cap paces the frames, and the answers are the same. Details and measurements are in [frame-generation.md](frame-generation.md).

---

## Logs for a Bug Report

See [troubleshooting.md](troubleshooting.md) for symptom-by-symptom fixes and [dev/debugging.md](dev/debugging.md) for every internal debug and trace variable.

| Launch option | What it writes |
|---|---|
| `XESS_LOG=1 %command%` | Sets `IGDEXT_TRACE=1`, `IGDEXT_XELL_LOG=1`, `IGDEXT_XEFG_LOG=1`, and `XESS_FG_LOG=1`. Writes these logs in the game's prefix (`drive_c/`). The file `C:\igdext_trace.log` holds the detected GPU, every extension call, and each kernel; no file at all means the shim was not loaded. The file `C:\igdext_xell.log` holds what the game passes to XeLL, the real frame rate, and the latency. The file `C:\igdext_xefg.log` holds XeSS frame generation's own messages and the maximum and active number of generated frames. The file `C:\igcl_stub.log` holds the `XESS_FG` override given by `ControlLib.dll` |
| `PROTON_LOG=1 %command%` | Writes `~/steam-<appid>.log`: Proton's log including the game's stderr (`xess-xmx:` and `ANV CM:` lines). Slows the game down; use only for a report |
| `ANV_CM_DEBUG=1 %command%` | Logs each placeholder and kernel the patched driver injects to the game's stderr (use with `PROTON_LOG=1`) |

The prefix folder is `~/.local/share/Steam/steamapps/compatdata/<appid>/pfx/drive_c/` (or under the `steamapps` folder of the Steam library on another drive).

---

## The Game Sees No Intel GPU

| Launch option | What it does | When |
|---|---|---|
| `force_vk_vendor=-1 %command%` | Restores Mesa's own `drirc` profile for this game, hiding the Intel Vulkan vendor ID | A game that needs Mesa's vendor-hiding workaround; XeSS then runs on DP4a. `install.sh` and `enable.sh` set `force_vk_vendor=0` globally so games see the Intel GPU ([troubleshooting.md](troubleshooting.md)) |

A game that ships XeSS older than 2.0.2.68 also sees no Intel GPU because DXVK hides the Intel vendor ID from old `libxess.dll` builds: use `XESS=latest %command%` (see [XeSS super resolution](#xess-super-resolution) above and [games.md](games.md)).
