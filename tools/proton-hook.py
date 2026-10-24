# proton-hook.py - run by Proton at every game start through the user_settings.py that install.sh puts next to each
# Proton's `proton` script (Proton imports that file before it starts the game). It does something only when the game's
# launch options say so (docs/launch-options.md):
#   XESS=latest %command%   the game runs with the XeSS libraries of Intel's SDK (tools/get-xess.sh, <package>/xess)
#                           instead of its own older ones. Meant for a game with XeSS older than 2.0.2.68: DXVK reports
#                           an Intel GPU to such a game as an AMD one (DP4a; XeSS-FG older than 1.2 is not offered)
#   XESS=dp4a %command%     XeSS without the shim's extension: DP4a, frame generation on its generic path
#                           (IGDEXT_FORCE_FALLBACK=1)
#   XESS_LOG=1 %command%    the shim's trace and the XeLL, XeSS-FG and ControlLib.dll logs (IGDEXT_TRACE=1
#                           IGDEXT_XELL_LOG=1 IGDEXT_XEFG_LOG=1 XESS_FG_LOG=1)
# (XESS_FG=<n>, the frame generation override, needs no hook: ControlLib.dll and the shim read it in the game)
# The game's own old libraries are not given the Intel GPU (DXVK_CONFIG=dxgi.customVendorId = 8086 would): Cyberpunk
# 2077's XeSS-FG 1.1.0.19 then crashes on a null context (libxess_fg +0x238ea4); with the SDK's 1.3.1.78 it works.
# The variables behind them go into Proton's user_settings, which never override one the launch options set themselves.
# XESS=latest: while the game runs, each libxess.dll / libxess_dx11.dll / libxess_fg.dll / libxell.dll in the game folder that is
# older than the SDK's is renamed to <name>.xmx-orig and replaced by a link to the SDK's file. When Proton exits (the
# game ended, or Steam stopped it) the originals are put back. A run that was killed outright leaves the links; the
# next game start (with or without XESS) or uninstall.sh puts the originals back. Steam starts Proton more than once for
# some games (a launcher, install scripts), so each run that swaps holds a lock file listing its links for as long as
# it lives: links whose lock nobody holds any more are left over; links of a live run are not touched.
import atexit, fcntl, os, signal, sys

PKG = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORE = os.path.join(PKG, "xess")
LIBS = ("libxess.dll", "libxess_dx11.dll", "libxess_fg.dll", "libxell.dll")
# one lock file per run that swapped: its links, one path per line; the run holds an exclusive lock on it until it ends
LOCKS = os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "xess-xmx", "xess-swap")
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
        # one rename over the link: there is no moment without the file, whatever interrupts this
        os.replace(path + ".xmx-orig", path)
        return True
    return False


def clean_left_over():
    """put back the links of runs that ended without restoring them (their lock is free); live runs are left alone"""
    try:
        names = os.listdir(LOCKS)
    except OSError:
        return
    for name in names:
        lf = os.path.join(LOCKS, name)
        try:
            fd = os.open(lf, os.O_RDWR)
        except OSError:
            continue
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(fd)   # a live run
            continue
        with os.fdopen(fd) as f:
            paths = [l.rstrip("\n") for l in f if l.strip()]
            for p in paths:
                if put_back(p):
                    say("XESS=latest: %s put back (left by a run that was killed)" % p)
            if not any(ours(p) for p in paths):
                os.remove(lf)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("waitforexitandrun", "run"):
        return
    want = os.environ.get("XESS", "")
    game = os.environ.get("STEAM_COMPAT_INSTALL_PATH", "")
    clean_left_over()
    if os.environ.get("XESS_LOG", "") not in ("", "0"):
        SETTINGS.update({"IGDEXT_TRACE": "1", "IGDEXT_XELL_LOG": "1", "IGDEXT_XEFG_LOG": "1", "XESS_FG_LOG": "1"})
        say("XESS_LOG: C:\\igdext_trace.log, igdext_xell.log, igdext_xefg.log, igcl_stub.log in the prefix")
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
    lock = None
    for f in game_libs(game):
        if os.path.islink(f):
            continue   # another run of this game swapped it and is still running
        orig = f + ".xmx-orig"
        if os.path.exists(orig):
            os.remove(orig)   # from a killed run, and the game was updated or verified since: f is the original now
        sdk = os.path.join(STORE, os.path.basename(f).lower())
        vg, vs = version(f), version(sdk)
        rel = os.path.relpath(f, game)
        if vg and vs and vg < vs:
            if lock is None:
                os.makedirs(LOCKS, exist_ok=True)
                lock = open(os.path.join(LOCKS, "%d-%x" % (os.getpid(), id(swapped))), "w")
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
            lock.write(f + "\n")
            lock.flush()
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
        swapped.clear()
        try:
            os.remove(lock.name)
            lock.close()
        except OSError:
            pass
        if n:
            say("XESS=latest: the game's own XeSS libraries are back")

    sigs = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)

    def on_signal(sig, frame):
        # Steam stops a game with SIGINT and SIGTERM right after each other: the second one must not interrupt the
        # restore the first one started (that left a library missing), so further signals are ignored from here on
        for s in sigs:
            signal.signal(s, signal.SIG_IGN)
        # files a running game has open stay valid when they are renamed, so this is safe before the game is gone
        restore()
        signal.signal(sig, signal.SIG_DFL)
        os.kill(os.getpid(), sig)

    atexit.register(restore)
    for s in sigs:
        signal.signal(s, on_signal)


try:
    main()
except Exception as e:  # never stop a game from starting
    say("XESS: %s" % e)
