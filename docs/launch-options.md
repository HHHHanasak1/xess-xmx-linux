# Steam launch options

Everything a single game can be given in Steam (game → Properties → General → Launch options). Variables go before
`%command%`, separated by spaces:

    MESA=26.1 XESS=latest %command%

A launch option applies to that game only and takes effect at its next start. No re-login is needed.
`install.sh` sets the system-wide defaults (`~/.config/environment.d/50-xess-xmx.conf`). A launch option overrides
them for one game.

`XESS` and `XESS_LOG` need the system-wide install: `install.sh` puts a small `user_settings.py` into every Proton,
which Proton runs before it starts a game (`tools/proton-hook.py`). A `user_settings.py` of your own is kept as
`user_settings.user.py` next to it and still applies. With the per-game install (`enable.sh`) the wrapper comes last,
after the variables (`MESA=26.1 /path/to/xess-xmx-linux/xmx-launch.sh %command%`), and `XESS` / `XESS_LOG` are not
available: `XESS=dp4a` and `XESS_LOG=1` have their long variables (debugging.md), `XESS=latest` has none.

## Driver

| Launch option | What it does | When |
|---|---|---|
| `MESA=26.1 %command%` | this game uses the package's Mesa 26.1.2 driver | a game misbehaves on Mesa 26.3 only (please report it) |
| `MESA=26.3 %command%` | this game uses the package's Mesa 26.3 driver | the default was changed to 26.1 and one game should keep 26.3 |
| `INTEL_DEBUG=no-jay %command%` | Mesa 26.3 compiles the game's own shaders with the old compiler instead of Jay (XeSS is not affected) | a rendering bug or crash that goes away with `MESA=26.1`: tells whether Jay is the cause |

`MESA` takes a version or a prefix of one (`26.1`, `26.3`, `26.1.2`). An unknown version falls back to the default
driver; the game's stderr then says `xess-xmx: MESA=...: no such driver`. The default for all games is set with
`tools/switch-driver.sh` (no argument: list the drivers, `*` marks the default).

## XeSS

| Launch option | What it does | When |
|---|---|---|
| `XESS=latest %command%` | the game runs with the XeSS libraries of Intel's XeSS SDK (3.0.2: XeSS 2.0.2.68, XeSS-FG 1.3.1.78, XeLL 1.3.2.10) instead of its own older ones | a game with XeSS older than 2.0.2.68 (below), or one whose XeSS is simply old |
| `XESS=dp4a %command%` | XeSS without Intel's extension: super resolution on DP4a, frame generation on its generic path (2x only) | comparing XMX against DP4a, or ruling the XMX path out as the cause of a problem |
| `IGDEXT_XELL_FIXCAP=1 %command%` | applies the XeLL frame-cap fix (30 fps with frame generation until the pause menu is opened and closed) in this game | another game shows the Wuthering Waves symptom |
| `IGDEXT_XELL_KEEPCAP=1 %command%` | turns that fix off (it is on for Wuthering Waves only) | the fix is suspected to cause a problem |

**XeSS older than 2.0.2.68:** DXVK reports an Intel GPU as an AMD one to a game whose `libxess.dll` is older than
2.0.2.68, because early XeSS 2.0 builds crash under Proton on Intel. XeSS then runs on DP4a, and XeSS frame generation
older than 1.2, which is Intel-only, is not offered at all (Cyberpunk 2077 2.31: XeSS 2.0.1.41, XeSS-FG 1.1.0.19).
The crash is real: with the Intel GPU made visible to Cyberpunk 2077's own libraries (DXVK's
`dxgi.customVendorId = 8086`), its XeSS-FG 1.1.0.19 crashes at start. So such a game needs newer libraries:
`XESS=latest`. The symptom without it: XeSS super resolution is there but looks like DP4a, and the game offers no XeSS frame
generation.

`XESS=latest` changes the game's files only while it runs: each `libxess.dll`, `libxess_dx11.dll`, `libxess_fg.dll`
and `libxell.dll` in the game's folder that is older than the SDK's is renamed to `<name>.xmx-orig` and replaced by a
link to the SDK's copy in the package (`xess/`, one copy for all games; `install.sh` downloads it from
github.com/intel/xess, `tools/get-xess.sh` again). When the game exits, the originals are back, so Steam updates and
"Verify integrity of game files" see the game as shipped. A file as new as the SDK's or newer is left alone. If the
game was killed so hard that nothing could clean up, the next start of that game (with or without `XESS`) puts the
originals back. The game's stderr (`PROTON_LOG=1`) lists each file with both versions.

## Logs for a bug report

| Launch option | What it writes |
|---|---|
| `XESS_LOG=1 %command%` | `C:\igdext_trace.log` (the detected GPU, every extension call, each kernel; no file at all means the shim was not loaded) and `C:\igdext_xell.log` (what the game passes to XeLL, the real frame rate, the latency) in the game's prefix |
| `PROTON_LOG=1 %command%` | `~/steam-<appid>.log`: Proton's log including the game's stderr (`xess-xmx:` and `ANV CM:` lines). Makes the game slow; only for a report |
| `ANV_CM_DEBUG=1 %command%` | each kernel the driver injects, in the game's stderr (use with `PROTON_LOG=1`) |

The prefix is `~/.local/share/Steam/steamapps/compatdata/<appid>/pfx/drive_c/` (for a game on another drive, the
`steamapps` folder of that library).

## The game sees no Intel GPU

| Launch option | What it does | When |
|---|---|---|
| `force_vk_vendor=-1 %command%` | Mesa's own profile for this game again: the Intel vendor id is hidden | a game that needs Mesa's workaround; XeSS then runs on DP4a. `install.sh` sets `force_vk_vendor=0` for all games (debugging.md) |

A game with XeSS older than 2.0.2.68 does not see the Intel GPU either: `XESS=latest` (XeSS section above).

Every other switch, the tools and the logs outside the game: [debugging.md](debugging.md).
