# Steam Launch Options

Launch options are set for a single game in Steam (**Properties → General → Launch options**). Put variables before `%command%`, separated by spaces:

```sh
XESS=auto XESS_FG=3 %command%
```

A launch option applies only to that game and takes effect the next time you start it. You do not need to log out or reboot. `install.sh` writes the system-wide defaults to `~/.config/environment.d/50-xess-xmx.conf` ([install.md](install.md)); a launch option overrides them for that game.

Most games need nothing, or one line from [Common](#common). The rest of the page is for the other cases:

* [When Something Does Not Work](#when-something-does-not-work): by what you see.
* [Advanced](#advanced): choosing the route, the driver, and the tuning yourself.
* [How the Options Are Applied](#how-the-options-are-applied): which options need the Proton hook, and what to use without it.
* [Earlier Names](#earlier-names): the options of earlier releases. All of them still work.

---

## Common

A Direct3D 12 (D3D12) game that ships XeSS 2.0.2.68 or newer needs no launch option: turn on XeSS (and XeSS Frame Generation, if offered) in its menu.

| Launch option | What it does | When |
|---|---|---|
| `XESS=auto %command%` | Sets the game up for XeSS on XMX from what its folder holds, while the game runs ([what it chooses](#what-xessauto-chooses)). XeSS libraries older than those of Intel's XeSS SDK 3.0.2 are replaced by the SDK's. A D3D11 game gets the package's D3D11 bridge, a Vulkan game its Vulkan wrapper. A game with FSR 3.1+ and no XeSS frame generation runs its FSR upscaling and frame generation on XeSS. The game's own files are put back when it exits | XeSS runs on DP4a, or the game's menu has no XeSS Frame Generation entry, because the game ships XeSS older than 2.0.2.68 (such as Cyberpunk 2077). A D3D11 or Vulkan game with XeSS. A game with FSR 3.1+ and no XeSS frame generation (such as CONTROL Resonant) |
| `XESS_FG=<n> %command%` | Frame generation shows `<n>` frames per rendered frame, whatever the game's menu offers or selects; you still switch frame generation on and off in the menu. `2` to `4` are what Intel Graphics Software offers on Windows. `5` and `6` go beyond the 4x Intel supports; if the change cannot be verified, the game stays at 4x. `7` and `8` are **experimental** and not recommended due to high latency | A game that offers only 2x in its menu (such as Cyberpunk 2077), or setting the multiplier where FSR runs on XeSS. With `XESS=auto` in a game that has FSR 3.1+ upscaling and no frame generation an Intel GPU can use (such as Forza Horizon 6), it also adds XeSS frame generation: choose FSR upscaling in the game |
| `OFLOW_FG=1 %command%` | Frame generation at 2x for a DX12 game from optical flow on the final image, without motion vectors from the game; the HUD is interpolated too. With `XESS_FG=3` or more it is **experimental** ([frame-generation.md](frame-generation.md#5-frame-generation-for-any-dx12-game-oflow_fg1)) | A DX12 game with neither XeSS nor FSR 3.1+ (such as Resident Evil 4 (2023), or Viewfinder with `-force-d3d12` after `%command%`) |
| `XESS_LL=off %command%`, `XESS_LL=auto:<ms> %command%` | The low latency, which is on by default while frame generation runs. `off` turns it off. `auto:<ms>` keeps it on and starts each frame `<ms>` milliseconds earlier (later if negative), which gives more frame-rate headroom for a little more latency ([frame-generation.md](frame-generation.md#2-low-latency-with-frame-generation)) | The frame rate with frame generation is lower than expected, or the pacing is uneven; comparing latency and frame rate |
| `XESS_LOG=1 %command%`, `XESS_LOG=2 %command%` | Logs for a bug report ([troubleshooting.md](troubleshooting.md#logs-for-a-bug-report)). `1`: the package's own logs in the game's prefix. `2`: also Proton's log with the driver's lines, and the layer's log; the game runs slower | Reporting a problem |

**In the game's menu:** choose XeSS, and XeSS Frame Generation, when the menu has them. If it has no XeSS Frame Generation, choose FSR and FSR frame generation: with `XESS=auto` they run on XeSS. If the game has no frame generation an Intel GPU can use, choose FSR upscaling; `XESS_FG=<n>` next to `XESS=auto` then adds XeSS frame generation.

Keep a frame rate limiter on (the game's own or gamescope's) when using frame generation so generated frames reach the screen evenly, and run in windowed or borderless mode (XeSS-FG refuses exclusive fullscreen).

**Game files.** This is about `XESS=auto`, `OFLOW_FG=1`, and the options under [Choosing the Route Yourself](#choosing-the-route-yourself). These options change or add DLLs in the game's folder while it runs. A game with anti-cheat can see that as tampering and refuse to start, or flag the account in online play: do not use them in online games that use anti-cheat. `XESS_FG`, `XESS_LL`, and `XESS_LOG` change no game files.

`XESS=auto`, `XESS_LL`, and `XESS_LOG=2` are in releases after 20261113, as are `XESS=auto_off`, `FSR_FG=inject` without `FSR=xess`, and the hook's log.

### What `XESS=auto` Chooses

First, the game's own XeSS is looked after, exactly as [`XESS=latest`](#choosing-the-route-yourself) does it: older XeSS libraries, the D3D11 bridge, the Vulkan wrapper. Then the hook takes the first line below that fits what the game's folder holds:

| The game's folder holds | What happens | The same as |
|---|---|---|
| `libxess_fg.dll`: the game ships XeSS frame generation | Nothing more. The game's own XeSS is the route, and its FSR is left alone | `XESS=latest` |
| No `libxess_fg.dll`, but FSR 3.1+ frame generation (`amd_fidelityfx_framegeneration_dx12.dll`, or the single `amd_fidelityfx_dx12.dll` of FSR 3.1.0 to 3.1.3) | The game's FSR upscaling runs on XeSS super resolution and its FSR frame generation on XeSS frame generation | `XESS=latest FSR=xess` |
| No `libxess_fg.dll`, and of FSR 3.1+ only the upscaler (`amd_fidelityfx_upscaler_dx12.dll`) | The game's FSR upscaling runs on XeSS super resolution. If the launch options ask for frame generation (`XESS_FG=<n>`, or `OFLOW_FG=1`), XeSS frame generation is added to it | `XESS=latest FSR=xess`; with frame generation asked for, `XESS=latest FSR=xess FSR_FG=inject` |
| Neither | Nothing more | `XESS=latest` |

* **The game's own XeSS frame generation comes first.** What a game hands to FSR is not always what XeSS works best with, so a game that ships both is left on its own XeSS ([frame-generation.md](frame-generation.md#3-fsr-31-on-xess-fsrxess-and-fsr_fgxess)). A game can ship XeSS frame generation and still not offer it in its menu (such as Resident Evil Requiem); nothing in its folder tells such a game from one that does. For it, add `FSR=xess` ([below](#when-something-does-not-work)).
* **A route in the launch options decides.** With `FSR=xess`, `FSR_FG=xess`, or `FSR_FG=inject` next to `XESS=auto`, that route is used, and the game's own XeSS is still looked after.
* **Optical flow is the last resort.** `OFLOW_FG=1` next to `XESS=auto` is used only in a game of the last line; in the others it is left out, because the game's own motion vectors are better. `XESS=auto OFLOW_FG=1 %command%` can therefore be used without knowing what a DX12 game ships. Without `OFLOW_FG=1`, `XESS=auto` never uses optical flow.
* **The injected frame generation needs to be asked for.** It cannot be switched off in the game's menu, so the third line adds it only with `XESS_FG=<n>` or `OFLOW_FG=1`.
* **What it chose** is in the hook's log, `~/.cache/xess-xmx/hook-<appid>.log`, with the reason and every file it replaced ([How the Options Are Applied](#how-the-options-are-applied)).

A game with DLSS only, or with FSR 2 or FSR 3.0 (which have no FidelityFX DLLs to replace), is a game of the last line. XeSS frame generation runs on D3D12 only, so `XESS_FG` and `OFLOW_FG=1` do nothing in a D3D11 or Vulkan game.

---

## When Something Does Not Work

See [troubleshooting.md](troubleshooting.md) for symptoms that are not about a launch option, and for collecting logs.

| What you see | Launch option | What it does |
|---|---|---|
| With `XESS=auto`, the game's menu has no XeSS Frame Generation entry, and the hook's log says `the game ships XeSS frame generation` (such as Resident Evil Requiem) | `FSR=xess %command%`, with or without `XESS=auto` | Runs the game's FSR upscaling and frame generation on XeSS although the game ships XeSS frame generation of its own. Choose FSR and FSR frame generation in the menu |
| With `XESS=auto`, an FSR entry of the game's menu runs on XeSS, and you want the game's own FSR | `XESS=latest %command%` | Only the game's own XeSS libraries are looked after. FSR is not touched |
| With `XESS=auto` or `XESS=latest`, the game's launcher reports changed or missing files and checks them again at every start (such as Wuthering Waves, whose XeSS needs no launch option) | No `XESS=auto` for this game; `XESS=auto_off %command%` where `XESS=auto` is set for every game | The hook then changes no file of this game. The launcher had found the links the hook puts in place of the game's libraries while it runs |
| A Vulkan game with XeSS stays on DP4a with `XESS=auto` | `XESS=auto XESS_VK=1 %command%` | Forces the Vulkan part on, for a game whose executable loads `vulkan-1.dll` at run time rather than importing it in its PE header |
| A game that does not use Vulkan misbehaves with `XESS=auto`, and a `vulkan-1.dll` link is next to its executable while it runs | `XESS=auto XESS_VK=0 %command%` | Turns the Vulkan part off |
| The game offers FSR in its menu only on AMD GPUs (such as Wuthering Waves, which should rather use its own XeSS) | `FSR_VENDOR=amd`, next to `FSR=xess` or `FSR_FG=xess`, or where `XESS=auto` runs FSR on XeSS | Adds `dxgi.customVendorId = 1002` to `DXVK_CONFIG` so DXVK reports an AMD GPU to the game, while the package's DLLs still report Intel's `0x8086` vendor ID to XeSS-FG and XeLL |
| 30 fps with frame generation until the pause menu is opened and closed, in a game other than Wuthering Waves | `IGDEXT_XELL_FIXCAP=1 %command%` | Applies the XeLL 30 fps frame-cap fix (ignoring a non-zero cap set within 1.5 s of enabling frame generation) in this game. It is on by default for Wuthering Waves only |
| A rendering bug or crash on Mesa 26.3 only | `MESA=26.1 %command%` (please report it) | Runs this game on the package's patched Mesa 26.1.2 driver ([The Driver for One Game](#the-driver-for-one-game)). `INTEL_DEBUG=no-jay %command%` tells whether Mesa 26.3's Jay compiler is the cause: it compiles the game's own shaders with the older `brw` compiler (XeSS is not affected) |
| A game needs Mesa's own workaround that hides the Intel GPU from it | `force_vk_vendor=-1 %command%` | Restores Mesa's `drirc` profile for this game, hiding the Intel Vulkan vendor ID. XeSS then runs on DP4a. `install.sh` and `enable.sh` set `force_vk_vendor=0` globally so games see the Intel GPU ([troubleshooting.md](troubleshooting.md#the-game-sees-no-intel-gpu)) |
| You want to know whether the XMX path is the cause of a problem | `XESS=dp4a %command%` | Sets `IGDEXT_FORCE_FALLBACK=1` so the shim declines Intel's extension: XeSS super resolution runs on DP4a and XeSS frame generation on its generic path (2x only) |
| `XESS=auto` changes nothing, and `~/.cache/xess-xmx/hook-<appid>.log` gets no new lines | | The game runs on a Proton without the hook ([How the Options Are Applied](#how-the-options-are-applied)) |

---

## Advanced

### Choosing the Route Yourself

`XESS=auto` picks one of these. Each can also be set by itself, and then decides.

| Launch option | What it does | When |
|---|---|---|
| `XESS=latest %command%` | Runs the game with the XeSS libraries of Intel's XeSS SDK 3.0.2 in place of its older ones. The SDK files are in `xess/`, fetched by `install.sh` / `tools/get-xess.sh` (XeSS 2.0.2.68, XeSS-FG 1.3.1.78, XeLL 1.3.2.10). For a **D3D11** game, replaces `libxess_dx11.dll` with the package's D3D11-on-D3D12 bridge (`dx11bridge/libxess_dx11.dll`, [games.md](games.md#xess-in-a-d3d11-game)). For a **Vulkan** game, places the package's `vulkan-1.dll` (`vkwrap/`) next to the executable and enables the layer so XeSS runs on XMX on the Xe3 GPUs of `layer/gpu-ids.txt`, and on its Xe2 GPUs in stock mode ([games.md](games.md#xess-in-a-vulkan-game)). FSR is never touched | You want the game's own XeSS and nothing else |
| `FSR=xess %command%` | Runs the game's FSR 3.1+ upscaling on XeSS super resolution (`amd_fidelityfx_upscaler_dx12.dll`) and its FSR frame generation on XeSS frame generation (`amd_fidelityfx_framegeneration_dx12.dll`); combine with `XESS_FG=<n>` for multi-frame generation ([frame-generation.md](frame-generation.md#3-fsr-31-on-xess-fsrxess-and-fsr_fgxess)) | A game whose own XeSS frame generation is not offered in its menu. If a game has native XeSS that works, use that instead; translating FSR can flicker more |
| `FSR_FG=xess %command%` | Runs only the game's FSR 3.1+ frame generation on XeSS frame generation; keeps the game's own FSR upscaling. `XESS=auto` never picks this | You want to keep the game's native FSR upscaler |
| `FSR_FG=inject %command%` | Injects an XeSS-FG swap chain (`dxgi.dll` + `ffx_xefg_bridge.dll`) into a game with FSR 3.1+ upscaling and no FSR frame generation, fed from the game's FSR upscaler, which runs on XeSS for it: the same as `FSR=xess FSR_FG=inject`. Choose FSR upscaling in the game; the HUD is interpolated too ([frame-generation.md](frame-generation.md#4-a-game-with-fsr-upscaling-and-no-fsr-frame-generation-fsr_fginject)) | A game whose frame generation is NVIDIA DLSS-G only, such as Forza Horizon 6 |
| `OFLOW_FG=1 %command%` | Without `XESS=auto`: optical-flow frame generation whatever the game ships ([Common](#common)). Left out next to `FSR=xess` or `FSR_FG` | A DX12 game with neither XeSS nor FSR 3.1+ |
| `XESS=auto_off %command%` | No `XESS=auto` for this game. It turns nothing else off: XeSS, `XESS_FG`, and the other options work as without it | You have set `XESS=auto` for every game ([below](#the-same-options-for-every-game)), and one game should be left alone |

While the game runs, each `libxess.dll`, `libxess_fg.dll`, and `libxell.dll` in the game folder is compared with the SDK's. If it is older, it is renamed to `<name>.xmx-orig` and replaced by a link to a copy of the SDK's file made for that run (in `~/.cache/xess-xmx/xess-swap/`; a launcher that repairs the game folder through the link then cannot delete the package's file). Files as new as or newer than the SDK's are left alone. Every `libxess_dx11.dll` is replaced by `dx11bridge/libxess_dx11.dll` regardless of its version (with the SDK's `libxess.dll` added next to it if missing). The FidelityFX DLLs are replaced the same way. When the game exits (or on the next start of any game if this one was killed outright), the originals are put back so Steam updates and file verification see the unmodified game ([games.md](games.md)).

### The Driver for One Game

See [drivers.md](drivers.md) for the drivers in the package and how to switch the default across all games (`tools/switch-driver.sh`, `XMX_DRIVER`), and [install.md](install.md) for the full details of `xmx-stock.sh`.

| Launch option | What it does | When |
|---|---|---|
| `MESA=26.1 %command%` | Runs this game on the package's patched Mesa 26.1.2 driver | A game misbehaves on Mesa 26.3 only (please report it) |
| `MESA=26.3 %command%` | Runs this game on the package's patched Mesa 26.3 driver | You changed the default to 26.1 and one game should keep 26.3, or on Xe3 or Xe2 in stock mode you want to compare the patched 26.3 driver against the unpatched default |
| `/path/to/package/xmx-stock.sh %command%` | Runs this game on your distribution's own unpatched Mesa ANV (Intel's Vulkan driver) with the package's Vulkan layer `VK_LAYER_XESS_xmx` (`layer/`), which injects the XMX (Xe Matrix Extensions) kernels from outside the driver; the session's default driver stays in place for every other game | You want to keep your distribution's own Mesa for that game, or test the layer against the patched driver. The layer supports the Xe3 and Xe2 GPUs of `layer/gpu-ids.txt` (Xe2: kernels at 128 GRF, untested on real hardware) on Mesa 26.1, 26.3, and 27.0 (SteamOS ships 26.1.2); with another Mesa version or GPU, XeSS runs on DP4a (dot-product fallback) |
| `INTEL_DEBUG=no-jay %command%` | Compiles the game's own shaders on Mesa 26.3 with the older `brw` compiler instead of `Jay` (XeSS is not affected) | A rendering bug or crash that goes away with `MESA=26.1`: tells whether Jay is the cause |

`MESA` takes a version or a prefix of one (`26.1`, `26.3`, `26.1.2`). An unknown version falls back to the default driver and prints `xess-xmx: MESA=...: no such driver` to the game's stderr.

`xmx-stock.sh` is a command wrapper, not a variable, because it must rewrite `VK_DRIVER_FILES` before Steam's runtime container (`pressure-vessel`) starts. Put variables before the wrapper:

```sh
XESS=auto /path/to/package/xmx-stock.sh %command%
```

If the layer files or a 64-bit system Intel ICD (Installable Client Driver) are missing, `xmx-stock.sh` logs a line to Steam's output and starts the game unchanged on the session's driver. On the Xe3 and Xe2 GPUs of `layer/gpu-ids.txt`, that is the package's unpatched driver + layer in stock mode. Otherwise it is the patched driver. `MESA` has no effect when using `xmx-stock.sh`. To use a custom unpatched Intel ICD JSON file instead of the system's, set `XMX_STOCK_ICD=<json>[:<json>]` before the wrapper. For the layer's own switches, see [layer/README.md](../layer/README.md) and [dev/debugging.md](dev/debugging.md).

### Tuning

| Launch option | What it does | When |
|---|---|---|
| `IGDEXT_XELL_CAP=<us> %command%` | Instead of the low latency's wait, raises `minimumIntervalUs` to at least `<us>` microseconds per displayed frame when the game calls `xellSetSleepMode`. Valid values: `1` to `1000000`; `0` = off. Any other value turns both off. If a game never calls `xellSetSleepMode`, no cap is applied. It has no short name; `XESS_LL` does not take a number | Locking a steady fixed frame rate through XeLL |
| `FFX_XEFG_HUDLESS=0`, `FFX_XEFG_HDR10=1`, `FFX_XESS_QUALITY=<n>`, `FFX_XEFG_CAP=...` | What the FSR routes and optical flow hand to XeSS, and optical flow's frame cap ([frame-generation.md](frame-generation.md#6-logs-and-configuration-variables)) | A game where FSR runs on XeSS, or with `OFLOW_FG=1` |

Every other switch of the package is for debugging: [dev/debugging.md](dev/debugging.md).

### The Same Options for Every Game

A variable in a file of your own in `~/.config/environment.d/` applies to every game of the session after the next login, and a game's launch options still override it. Use a file of your own, such as `~/.config/environment.d/60-xess-xmx-user.conf`: `install.sh` rewrites `50-xess-xmx.conf`.

```sh
XESS=auto
XESS_FG=3
```

With `XESS=auto` there, the hook looks at every game you start and changes files in the folders of those it finds a route for; mind the note about [game files](#common) and anti-cheat. `XESS=auto_off %command%` in a game's launch options leaves that game alone.

---

## How the Options Are Applied

`XESS_FG` and `MESA` are read inside the game's process, by `ControlLib.dll`, `igdext64.dll`, and the frame generation DLL, and by the package's driver. They work wherever the package is installed.

The other options on this page are handled by the Proton hook, which needs the system-wide install ([install.md](install.md)):

* `XESS`, `XESS_LL`, and `XESS_LOG`
* `FSR`, `FSR_FG`, and `OFLOW_FG`
* their modifiers `XESS_VK` and `FSR_VENDOR`

`install.sh` puts a small `user_settings.py` into every Proton it can write to, which Proton runs before it starts a game (`tools/proton-hook.py`). If you already had a `user_settings.py` of your own, `install.sh` keeps it as `user_settings.user.py` next to it so your settings still apply. `XESS=dp4a`, `XESS_LL`, and `XESS_LOG` are shorthands for variables the package's DLLs read; any variable you set explicitly in the launch options wins over the variable a shorthand expands to.

The hook swaps files in the game's folder, which Steam gives it (`STEAM_COMPAT_INSTALL_PATH`). For a non-Steam game added as a shortcut, Steam may give none; the hook then uses the folder of the `.exe` the shortcut starts (and everything below it), so point the shortcut at the game's own executable. It refuses a folder that is too wide to touch (`/`, your home folder or one above it, the prefix's `windows` folder) and starts the game with its own libraries.

**The hook's log.** What the hook was started with and every line it prints are kept in `~/.cache/xess-xmx/hook-<appid>.log` (for a non-Steam game the name of its executable takes the place of the app id): the route `XESS=auto` chose and why, each file it replaced, and that the game's own files are back. The same lines go to Steam's own output (the user journal in SteamOS Game Mode: `journalctl --user | grep xess-xmx`), not to the file `PROTON_LOG=1` writes.

If you set `VKD3D_DISABLE_EXTENSIONS` in the launch options (it replaces the system-wide value), the hook adds the extensions the XMX kernels need (`VK_EXT_descriptor_buffer`, `VK_EXT_descriptor_heap`, `VK_KHR_present_wait2`) to your list while the package's driver or layer is active; your own entries stay.

**GE-Proton's own upgrades.** GE-Proton has variables of its own that bring newer upscaler libraries (`PROTON_XESS_UPGRADE=1`, `PROTON_FFX4_UPGRADE=1`, `PROTON_USE_OPTISCALER=1`): its loader then takes its copies in place of a game's libraries of that name. They have not been tested together with the options of this page. When one of them is set and the hook has replaced files, its log says that those files may not be the ones the game loads.

### Without the Hook

With the per-game install (`enable.sh`, [install.md](install.md)), put the wrapper last, after your variables:

```sh
MESA=26.1 XESS_FG=3 /path/to/xess-xmx-linux/xmx-launch.sh %command%
```

With the per-game install (or on a root-owned distribution Proton where `install.sh` could not install `user_settings.py`), the hook's options are not available:

* Instead of `XESS=dp4a`, `XESS_LL`, and `XESS_LOG=1`, pass their underlying variables: `IGDEXT_FORCE_FALLBACK=1`, `IGDEXT_XELL_CAP=off` or `auto:<ms>`, and the `IGDEXT_*` log variables with `XESS_FG_LOG=1` ([dev/debugging.md](dev/debugging.md)).
* Instead of `FSR=xess` and `FSR_FG=xess`, run `tools/fsr-fg.sh` ([frame-generation.md](frame-generation.md)). Note that `tools/fsr-fg.sh all` only works on game folders that have the FSR frame generation DLL or the single `amd_fidelityfx_dx12.dll`. It refuses folders with only `amd_fidelityfx_upscaler_dx12.dll`.
* `XESS=auto`, `FSR_FG=inject`, `OFLOW_FG`, and the D3D11 and Vulkan parts of `XESS=latest` have no alternative without the hook. For old D3D12 XeSS, replace the DLLs by hand ([games.md](games.md#replacing-the-files-manually-without-the-hook)).

---

## Earlier Names

Every option of earlier releases works as it did. Where a shorter form exists now, the table names it.

| Option | Now also |
|---|---|
| `XESS=latest` | `XESS=auto` does the same and, in a game without XeSS frame generation, runs FSR 3.1+ on XeSS as well |
| `FSR=xess` | Chosen by `XESS=auto` in a game with FSR 3.1+ and no XeSS frame generation |
| `FSR=xess FSR_FG=inject` | `FSR_FG=inject` alone means the same. Chosen by `XESS=auto XESS_FG=<n>` in a game with an FSR 3.1+ upscaler and no frame generation beside it |
| `IGDEXT_XELL_CAP=off`, `IGDEXT_XELL_CAP=auto:<ms>` | `XESS_LL=off`, `XESS_LL=auto:<ms>`. If both names are set, `IGDEXT_XELL_CAP` decides |
| `XESS_LOG=1 PROTON_LOG=1 ANV_CM_DEBUG=1 XMX_LAYER_LOG=1` | `XESS_LOG=2` |
| `FSR_FG=xess`, `OFLOW_FG=1`, `FSR_VENDOR=amd`, `XESS=dp4a`, `XESS_VK`, `XESS_FG`, `XESS_LOG=1`, `MESA`, `xmx-stock.sh`, `xmx-launch.sh`, `INTEL_DEBUG=no-jay`, `force_vk_vendor=-1` | Unchanged |
| `IGDEXT_XELL_CAP=<us>`, `IGDEXT_XELL_FIXCAP=1` | Unchanged; now under [Tuning](#tuning) and [When Something Does Not Work](#when-something-does-not-work) |
| `IGDEXT_XELL_CHECK=0`, `IGDEXT_XELL_POLLS=0`, `IGDEXT_XELL_KEEPCAP=1` | Unchanged; they are for debugging and are listed in [dev/debugging.md](dev/debugging.md) |
| The `FFX_XESS_*` / `FFX_XEFG_*` switches | Unchanged; the ones that tune are in [frame-generation.md](frame-generation.md#6-logs-and-configuration-variables), all of them in [dev/debugging.md](dev/debugging.md) |
