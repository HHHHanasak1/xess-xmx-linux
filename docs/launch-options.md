# Steam launch options

Set launch options for one game in Steam under **Properties -> General -> Launch options**. Put variables before `%command%`, separated by spaces:

```sh
MESA=26.1 XESS=latest %command%
```

A launch option applies only to that game and takes effect the next time you start it. You do not need to log out and in. `install.sh` sets system-wide defaults in `~/.config/environment.d/50-xess-xmx.conf`; a launch option overrides them for that game.

`XESS`, `XESS_LOG`, `FSR`, `FSR_FG`, and `OFLOW_FG` require the system-wide install ([install.md](install.md)): `install.sh` puts a `user_settings.py` file into every Proton, which runs `tools/proton-hook.py` before starting a game. If you already had a `user_settings.py`, `install.sh` keeps it as `user_settings.user.py` next to it and still runs it.

With a per-game install (`enable.sh`, see [install.md](install.md)), put the wrapper after the variables:

```sh
MESA=26.1 /path/to/xess-xmx-linux/xmx-launch.sh %command%
```

The Proton hook shorthands are not available with `enable.sh`:

- For `XESS=dp4a` and `XESS_LOG=1`, use the underlying variables ([dev/debugging.md](dev/debugging.md)).
- For `FSR=xess` and `FSR_FG=xess`, run `tools/fsr-fg.sh` ([frame-generation.md](frame-generation.md)).
- `XESS=latest`, `FSR_FG=inject`, and `OFLOW_FG` have no `enable.sh` equivalent.

## Driver

See [drivers.md](drivers.md) for details on the bundled drivers and `tools/switch-driver.sh`, and [install.md](install.md#using-the-systems-own-driver-for-one-game-xmx-stocksh) for `xmx-stock.sh`.

| Launch option | What it does | When to use |
|---|---|---|
| `MESA=26.1 %command%` | Runs the game on the package's patched Mesa 26.1.2 driver (ANV, Mesa's Intel Vulkan driver). | A game misbehaves on Mesa 26.3 only (please report it). |
| `MESA=26.3 %command%` | Runs the game on the package's patched Mesa 26.3 driver. | You changed the default to 26.1 and want 26.3 for one game, or on Xe3 to compare against the unpatched default. |
| `/path/to/package/xmx-stock.sh %command%` | **Experimental**: runs the game on the system's own unpatched Mesa ANV with the package's Vulkan layer `VK_LAYER_XESS_xmx` (`layer/`), which injects the XMX kernels from outside the driver. Other games keep the default driver. | You want the distribution's own Mesa for that game, or want to test the layer against the patched driver. The layer supports Mesa 26.1, 26.3, and 27.0 (SteamOS ships 26.1.2); on other versions XeSS falls back to DP4a (dot-product instructions, the generic non-XMX path). |
| `INTEL_DEBUG=no-jay %command%` | Mesa 26.3 compiles the game's own shaders with the old compiler instead of Jay (Mesa's new shader compiler). XeSS is not affected. | A rendering bug or crash goes away with `MESA=26.1`: this tells you whether Jay is the cause. |

`MESA` accepts a version or a version prefix (`26.1`, `26.3`, `26.1.2`). An unknown version falls back to the default driver and prints `xess-xmx: MESA=...: no such driver` to the game's stderr. To change the default driver for all games, run `tools/switch-driver.sh` (with no argument it lists the drivers and marks the default with `*`; see [drivers.md](drivers.md)).

Put variables before `xmx-stock.sh`:

```sh
XESS=latest /path/to/package/xmx-stock.sh %command%
```

Do not combine `xmx-stock.sh` with `xmx-launch.sh`, and note that `MESA` has no effect with it. See [install.md](install.md#using-the-systems-own-driver-for-one-game-xmx-stocksh) for what `xmx-stock.sh` sets and requires.

## XeSS super resolution

See [games.md](games.md) for per-game setups, D3D11 games, Vulkan games, and games with old XeSS libraries.

| Launch option | What it does | When to use |
|---|---|---|
| `XESS=latest %command%` | Replaces the game's older XeSS libraries with Intel's XeSS SDK 3.0.2 libraries (XeSS 2.0.2.68, XeSS-FG 1.3.1.78, XeLL 1.3.2.10) while the game runs. In a **D3D11** game (or with OptiScaler's XeSS in D3D11), it replaces `libxess_dx11.dll` with the package's D3D12 bridge ([games.md](games.md#xess-in-a-d3d11-game)). In a **Vulkan** game, it puts the package's `vulkan-1.dll` next to the executable so the layer runs XeSS on XMX ([games.md](games.md#xess-in-a-vulkan-game)). | A game ships XeSS older than 2.0.2.68 (where DXVK hides the Intel GPU; see [games.md](games.md)), a game has an older XeSS build, or the game uses XeSS on D3D11 or Vulkan. |
| `XESS_VK=1` / `XESS_VK=0` (with `XESS=latest`) | Forces the Vulkan wrapper of `XESS=latest` on (for an executable that loads Vulkan at run time instead of importing `vulkan-1.dll`) or off. | A Vulkan game with XeSS stays on DP4a with `XESS=latest` alone (`XESS_VK=1`), or the Vulkan wrapper gets in the way in a non-Vulkan game (`XESS_VK=0`). |
| `XESS=dp4a %command%` | Runs XeSS without Intel's extension: super resolution uses DP4a and frame generation uses its generic path (2x only). | Comparing XMX against DP4a, or checking whether the XMX path causes a problem. |

### How `XESS=latest` swaps files

`XESS=latest` changes files in the game folder only while the game runs:

- Every `libxess.dll`, `libxess_fg.dll`, and `libxell.dll` older than the SDK copy is renamed to `<name>.xmx-orig` and replaced by a link to the SDK copy in `xess/` (downloaded once by `install.sh` or `tools/get-xess.sh` from `github.com/intel/xess`). Files as new as or newer than the SDK copy are left alone.
- Every `libxess_dx11.dll` is replaced by `dx11bridge/libxess_dx11.dll` regardless of its version, and gets the SDK's `libxess.dll` next to it if the game has none there.
- When the game exits, the hook restores the original files and removes any added files, so Steam updates and "Verify integrity of game files" see the unmodified game. If the game is killed before cleanup can run, the next start of that game (with or without `XESS`) restores the originals.
- With `PROTON_LOG=1`, the game's stderr lists each replaced file with both versions.

## Frame generation and FSR on XeSS

See [frame-generation.md](frame-generation.md) for how each frame generation path works, tested games, limitations, and extra DLL variables.

| Launch option | What it does | When to use |
|---|---|---|
| `XESS_FG=3 %command%`, `XESS_FG=4 %command%` | Sets XeSS frame generation to 3x or 4x (`XESS_FG=2` sets 2x), overriding the game's menu choice through `ControlLib.dll`. You still turn frame generation on and off in the game's menu, and the game needs XeSS-FG 1.2 or newer (combine with `XESS=latest` if older). | A game offers only 2x in its menu (such as Cyberpunk 2077), or you want to override its 2x/3x/4x menu. |
| `XESS_FG=5` ... `XESS_FG=8 %command%` | **Experimental**: sets XeSS frame generation to 5x through 8x (the maximum), beyond the 4x that Intel supports. All work; 7x and 8x are not recommended because of latency. | You have a high-refresh-rate display that 4x does not fill. |
| `FSR=xess %command%` | Runs the game's FSR 3.1+ upscaling on XeSS super resolution and its FSR frame generation on XeSS frame generation (combine with `XESS_FG` for multi-frame generation). | A game has FSR 3.1+ and no XeSS. If the game has native XeSS, use that instead; this route can flicker more. |
| `FSR_FG=xess %command%` | Runs only the game's FSR 3.1+ frame generation on XeSS frame generation; keeps the game's own FSR upscaling. | Same as `FSR=xess`, when you want to keep the game's FSR upscaling. |
| `FSR=xess FSR_FG=inject %command%` | Injects XeSS frame generation into a game that has FSR 3.1+ upscaling but no FSR frame generation, using inputs from the upscaler. Select FSR upscaling in the game. The HUD is interpolated too. | A game has FSR 3.1+ upscaling, while its frame generation is DLSS-G only (such as Forza Horizon 6). |
| `OFLOW_FG=1 %command%` | **Experimental**: runs XeSS frame generation in any DX12 game by computing motion vectors from optical flow on the final image. `XESS_FG=<n>` sets the multiplier (default 2). The HUD is interpolated too, and the rendered frame rate is paced automatically. | A DX12 game has neither XeSS nor FSR 3.1+, such as Resident Evil 4 (2023) or Viewfinder (add `-force-d3d12` after `%command%`). |
| `FSR_VENDOR=amd %command%` | Used with `FSR` or `FSR_FG`: makes DXVK report an AMD GPU to the game, while XeSS-FG and XeLL still see Intel. | A game shows FSR in its menu only on AMD GPUs (such as Wuthering Waves, though that game should use its own XeSS). |

Keep a frame rate limiter on (the game's own or gamescope's) when using frame generation, or generated frames will reach the screen unevenly. With XeSS frame generation on, the game cannot use exclusive fullscreen and runs borderless instead (which looks the same under gamescope).

## Low latency (XeLL)

Intel XeLL (`libxell.dll`) is Intel's low-latency library used alongside XeSS frame generation. By default, while a game's own XeSS frame generation is active, the shim paces frame starts so frames do not queue before the GPU. See [frame-generation.md](frame-generation.md) for latency measurements and details, and [dev/how-it-works.md](dev/how-it-works.md) for internals.

| Launch option | What it does | When to use |
|---|---|---|
| `IGDEXT_XELL_CAP=off %command%` | Turns off the shim's low-latency pacing (which is on by default while XeSS frame generation runs). | Comparing latency, or checking whether low-latency pacing costs frame rate in a game. |
| `IGDEXT_XELL_CAP=auto:<ms> %command%` | Keeps low-latency pacing on, starting each frame `<ms>` earlier (or later if negative) for more frame rate headroom at the cost of a little more latency. | A game loses frame rate with the default low-latency pacing. |
| `IGDEXT_XELL_CAP=<us> %command%` | Replaces low-latency pacing with a fixed minimum interval of `<us>` microseconds per displayed frame through XeLL's frame cap (up to `1000000`; any other value turns both off). | You want a steady fixed frame rate. |
| `IGDEXT_XELL_FIXCAP=1 %command%` | Applies the Wuthering Waves XeLL frame-cap fix (which ignores a 33.3 ms / 30 fps cap sent right after enabling frame generation) in this game. | Another game drops to 30 fps when enabling frame generation until you open and close the pause menu. |
| `IGDEXT_XELL_KEEPCAP=1 %command%` | Turns off the XeLL frame-cap fix (on by default for Wuthering Waves only). | You suspect the fix causes a problem. |

## Logs for a bug report

See [troubleshooting.md](troubleshooting.md) for common symptoms and fixes, and [dev/debugging.md](dev/debugging.md) for all developer debug variables.

| Launch option | What it writes |
|---|---|
| `XESS_LOG=1 %command%` | Writes four logs into `C:\` inside the game's prefix: `C:\igdext_trace.log` (detected GPU, every extension call, each kernel; if the file is missing, the shim was not loaded), `C:\igdext_xell.log` (what the game passes to XeLL, real frame rate, latency), `C:\igdext_xefg.log` (XeSS frame generation messages, maximum and active multiplier), and `C:\igcl_stub.log` (the `XESS_FG` override served by `ControlLib.dll`). |
| `PROTON_LOG=1 %command%` | Writes `~/steam-<appid>.log` with Proton's log and the game's stderr (`xess-xmx:` and `ANV CM:` lines). Slows the game down; use only for a bug report. |
| `ANV_CM_DEBUG=1 %command%` | Logs each kernel the driver injects to the game's stderr (combine with `PROTON_LOG=1`). |

The game's prefix is at `~/.local/share/Steam/steamapps/compatdata/<appid>/pfx/drive_c/` (or in the `steamapps` folder of the Steam library on another drive).

## GPU vendor visibility

| Launch option | What it does | When to use |
|---|---|---|
| `force_vk_vendor=-1 %command%` | Restores Mesa's own profile for this game, hiding the Intel vendor ID. (`install.sh` sets `force_vk_vendor=0` for all games so they see the Intel GPU; see [troubleshooting.md](troubleshooting.md).) | A game needs Mesa's vendor-hiding workaround to run. XeSS will then use DP4a in that game. |

If a game hides the Intel GPU because its bundled `libxess.dll` is older than 2.0.2.68, use `XESS=latest %command%` instead (see [games.md](games.md)).
