# proton-hook.py - run by Proton at every game start through the user_settings.py that install.sh puts next to each
# Proton's `proton` script (Proton imports that file before it starts the game). It does something only when the game's
# launch options say so (docs/launch-options.md):
#   XESS=latest %command%   the game runs with the XeSS libraries of Intel's SDK (tools/get-xess.sh, <package>/xess)
#                           instead of its own older ones. Meant for a game with XeSS older than 2.0.2.68: DXVK reports
#                           an Intel GPU to such a game as an AMD one (DP4a; XeSS-FG older than 1.2 is not offered)
#   XESS=dp4a %command%     XeSS without the shim's extension: DP4a, frame generation on its generic path
#                           (IGDEXT_FORCE_FALLBACK=1)
#   XESS_LOG=1 %command%    the shim's trace and the XeLL log (IGDEXT_TRACE=1 IGDEXT_XELL_LOG=1)
# The game's own old libraries are not given the Intel GPU (DXVK_CONFIG=dxgi.customVendorId = 8086 would): Cyberpunk
# 2077's XeSS-FG 1.1.0.19 then crashes on a null context (libxess_fg +0x238ea4); with the SDK's 1.3.1.78 it works.
# The variables behind them go into Proton's user_settings, which never override one the launch options set themselves.
# XESS=latest: while the game runs, each libxess.dll / libxess_dx11.dll / libxess_fg.dll / libxell.dll in the game folder that is
# older than the SDK's is renamed to <name>.xmx-orig and replaced by a link to the SDK's file. When Proton exits (the
# game ended, or Steam stopped it) the originals are put back. A run that was killed outright leaves the links; the
# next start of that game (with or without XESS) or uninstall.sh puts the originals back.
import atexit, os, signal, sys

PKG = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORE = os.path.join(PKG, "xess")
LIBS = ("libxess.dll", "libxess_dx11.dll", "libxess_fg.dll", "libxell.dll")
# the links of runs still in progress (or killed), one path per line; read at every start, so a start without XESS
# does not have to search the game folder
STATE = os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "xess-xmx", "xess-swapped")
# variables for the game: user_settings.py passes its dict in; Proton applies them unless the launch options set them
SETTINGS = globals().get("SETTINGS", {})


def say(msg):
    sys.stderr.write("xess-xmx: %s\n" % msg)


def version(path):
    """a.b.c.d from the file's VS_FIXEDFILEINFO, or None"""
    try:
        with open(path, "rb") as f:
            d = f.read()
    except OSError:
        return None
    i = d.find(b"\xbd\x04\xef\xfe")
    if i < 0:
        return None
    ms, ls = int.from_bytes(d[i + 8:i + 12], "little"), int.from_bytes(d[i + 12:i + 16], "little")
    return (ms >> 16, ms & 0xffff, ls >> 16, ls & 0xffff)


def vstr(v):
    return ".".join(map(str, v)) if v else "?"


def game_libs(game):
    for top, dirs, files in os.walk(game):
        if top[len(game):].count(os.sep) >= 10:
            dirs[:] = []
        for name in files:
            if name.lower() in LIBS:
                yield os.path.join(top, name)


def ours(path):
    return os.path.islink(path) and os.readlink(path) == os.path.join(STORE, os.path.basename(path).lower())


def put_back(path):
    if ours(path) and os.path.isfile(path + ".xmx-orig"):
        os.remove(path)
        os.rename(path + ".xmx-orig", path)
        return True
    return False


def state(add=(), drop=()):
    try:
        with open(STATE) as f:
            cur = [l.rstrip("\n") for l in f if l.strip()]
    except OSError:
        cur = []
    cur = [p for p in cur if p not in drop] + [p for p in add if p not in cur]
    os.makedirs(os.path.dirname(STATE), exist_ok=True)
    with open(STATE + ".new", "w") as f:
        f.write("".join(p + "\n" for p in cur))
    os.replace(STATE + ".new", STATE)
    return cur


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("waitforexitandrun", "run"):
        return
    want = os.environ.get("XESS", "")
    game = os.environ.get("STEAM_COMPAT_INSTALL_PATH", "")
    if game and os.path.isdir(game) and os.path.isfile(STATE):
        # links left by a killed run of this game: originals back first (also when this start does not ask for XESS)
        left = [p for p in state() if p.startswith(game.rstrip("/") + "/")]
        if left:
            for p in left:
                put_back(p)
            state(drop=left)
    if os.environ.get("XESS_LOG", "") not in ("", "0"):
        SETTINGS.update({"IGDEXT_TRACE": "1", "IGDEXT_XELL_LOG": "1"})
        say("XESS_LOG: C:\\igdext_trace.log and C:\\igdext_xell.log in the prefix")
    if want == "dp4a":
        SETTINGS["IGDEXT_FORCE_FALLBACK"] = "1"
        return
    if want != "latest":
        if want:
            say("XESS=%s: unknown value (latest, dp4a); ignored" % want)
        return
    if not game or not os.path.isdir(game):
        say("XESS=latest: Steam gave no game folder (STEAM_COMPAT_INSTALL_PATH); the game's own libraries are used")
        return
    if not os.path.isfile(os.path.join(STORE, "libxess.dll")):
        say("XESS=latest: no XeSS SDK in %s (run %s/tools/get-xess.sh); the game's own libraries are used" % (STORE, PKG))
        return
    swapped = []
    for f in game_libs(game):
        if os.path.islink(f):
            continue
        orig = f + ".xmx-orig"
        if os.path.exists(orig):
            os.remove(orig)   # from a killed run, and the game was updated or verified since: f is the original now
        sdk = os.path.join(STORE, os.path.basename(f).lower())
        vg, vs = version(f), version(sdk)
        rel = os.path.relpath(f, game)
        if vg and vs and vg < vs:
            state(add=[f])
            os.rename(f, orig)
            os.symlink(sdk, f)
            swapped.append(f)
            say("XESS=latest: %s %s -> %s" % (rel, vstr(vg), vstr(vs)))
        else:
            say("XESS=latest: %s %s kept (the SDK has %s)" % (rel, vstr(vg), vstr(vs)))
    if not swapped:
        return

    def restore():
        n = sum(1 for f in swapped if put_back(f))
        state(drop=swapped)
        swapped.clear()
        if n:
            say("XESS=latest: the game's own XeSS libraries are back")

    def on_signal(sig, frame):
        # files a running game has open stay valid when they are renamed, so this is safe before the game is gone
        restore()
        signal.signal(sig, signal.SIG_DFL)
        os.kill(os.getpid(), sig)

    atexit.register(restore)
    for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(s, on_signal)


try:
    main()
except Exception as e:  # never stop a game from starting
    say("XESS: %s" % e)
