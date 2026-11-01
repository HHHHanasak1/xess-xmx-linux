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
#   FSR_FG=xess %command%   a game's FSR 3.1+ frame generation runs on XeSS-FG: its
#                           amd_fidelityfx_framegeneration_dx12.dll becomes the package's (ffxfg/, docs/fsr-frame-generation.md),
#                           with the SDK's libxess_fg.dll and libxell.dll next to it
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
# FSR_FG=xess swaps the same way; a libxess_fg.dll / libxell.dll the game does not have is added as a link (no
# .xmx-orig) and removed again.
import atexit, fcntl, os, signal, sys

PKG = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORE = os.path.join(PKG, "xess")
FFXFG = os.path.join(PKG, "ffxfg")
FG_DLL = "amd_fidelityfx_framegeneration_dx12.dll"
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


def game_libs(game, names=LIBS):
    for top, dirs, files in os.walk(game):
        if top[len(game):].count(os.sep) >= 10:
            dirs[:] = []
        for name in files:
            if name.lower() in names:
                yield os.path.join(top, name)


def ours(path):
    name = os.path.basename(path).lower()
    return os.path.islink(path) and os.readlink(path) in (os.path.join(STORE, name), os.path.join(FFXFG, name))


def put_back(path):
    if not ours(path):
        return False
    if os.path.isfile(path + ".xmx-orig"):
        # one rename over the link: there is no moment without the file, whatever interrupts this
        os.replace(path + ".xmx-orig", path)
    else:
        os.remove(path)   # added by FSR_FG=xess: the game has no such file of its own
    return True


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
                    say("XESS / FSR_FG: %s put back (left by a run that was killed)" % p)
            if not any(ours(p) for p in paths):
                os.remove(lf)


class Swaps:
    """the links this run made; held in a lock file while the game runs (clean_left_over)"""

    def __init__(self):
        self.paths = []
        self.lock = None

    def link(self, f, target):
        """f (existing or not) becomes a link to target; an existing f is kept as f.xmx-orig"""
        if self.lock is None:
            os.makedirs(LOCKS, exist_ok=True)
            self.lock = open(os.path.join(LOCKS, "%d-%x" % (os.getpid(), id(self))), "w")
            fcntl.flock(self.lock.fileno(), fcntl.LOCK_EX)
        self.lock.write(f + "\n")
        self.lock.flush()
        if os.path.exists(f):
            os.rename(f, f + ".xmx-orig")
        os.symlink(target, f)
        self.paths.append(f)


def fresh(f):
    """False while another run of this game has f swapped; drops an .xmx-orig a killed run left behind"""
    if os.path.islink(f):
        return False
    if os.path.exists(f + ".xmx-orig"):
        os.remove(f + ".xmx-orig")   # from a killed run, and the game was updated or verified since: f is the original
    return True


def xess_latest(game, swaps):
    for f in game_libs(game):
        if not fresh(f):
            continue
        sdk = os.path.join(STORE, os.path.basename(f).lower())
        vg, vs = version(f), version(sdk)
        rel = os.path.relpath(f, game)
        if vg and vs and vg < vs:
            swaps.link(f, sdk)
            say("XESS=latest: %s %s -> %s" % (rel, vstr(vg), vstr(vs)))
        else:
            say("XESS=latest: %s %s kept (the SDK has %s)" % (rel, vstr(vg), vstr(vs)))


def fsr_fg(game, swaps):
    """FSR_FG=xess: the FFX frame generation provider -> ffxfg/, with the SDK's XeSS-FG and XeLL next to it"""
    ours_dll = os.path.join(FFXFG, FG_DLL)
    if not os.path.isfile(ours_dll):
        say("FSR_FG=xess: %s missing (package without ffxfg/); the game's own frame generation is used" % ours_dll)
        return
    found = list(game_libs(game, (FG_DLL,)))
    if not found:
        if any(game_libs(game, ("amd_fidelityfx_dx12.dll",))):
            say("FSR_FG=xess: the game has FSR 3.1.0-3.1.3 (one amd_fidelityfx_dx12.dll), not supported yet")
        else:
            say("FSR_FG=xess: no %s in the game folder (no FSR 3.1+ frame generation)" % FG_DLL)
        return
    for f in found:
        if not fresh(f):
            continue
        swaps.link(f, ours_dll)
        say("FSR_FG=xess: %s -> XeSS-FG" % os.path.relpath(f, game))
        top = os.path.dirname(f)
        for name in ("libxess_fg.dll", "libxell.dll"):
            lib = os.path.join(top, name)
            if os.path.lexists(lib) and not fresh(lib):
                continue   # already a link of ours (XESS=latest)
            vg, vs = version(lib), version(os.path.join(STORE, name))
            if vg and vs and vg > vs:
                say("FSR_FG=xess: %s %s kept (newer than the SDK's %s)" % (name, vstr(vg), vstr(vs)))
                continue
            swaps.link(lib, os.path.join(STORE, name))


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("waitforexitandrun", "run"):
        return
    want = os.environ.get("XESS", "")
    fg = os.environ.get("FSR_FG", "")
    game = os.environ.get("STEAM_COMPAT_INSTALL_PATH", "")
    clean_left_over()
    if os.environ.get("XESS_LOG", "") not in ("", "0"):
        SETTINGS.update({"IGDEXT_TRACE": "1", "IGDEXT_XELL_LOG": "1", "IGDEXT_XEFG_LOG": "1", "XESS_FG_LOG": "1"})
        say("XESS_LOG: C:\\igdext_trace.log, igdext_xell.log, igdext_xefg.log, igcl_stub.log in the prefix")
    if want == "dp4a":
        SETTINGS["IGDEXT_FORCE_FALLBACK"] = "1"
    elif want and want != "latest":
        say("XESS=%s: unknown value (latest, dp4a); ignored" % want)
    if fg and fg != "xess":
        say("FSR_FG=%s: unknown value (xess); ignored" % fg)
    if want != "latest" and fg != "xess":
        return
    if not game or not os.path.isdir(game):
        say("XESS / FSR_FG: Steam gave no game folder (STEAM_COMPAT_INSTALL_PATH); the game's own libraries are used")
        return
    if not os.path.isfile(os.path.join(STORE, "libxess.dll")):
        say("XESS / FSR_FG: no XeSS SDK in %s (run %s/tools/get-xess.sh); the game's own libraries are used" % (STORE, PKG))
        return
    swaps = Swaps()
    if want == "latest":
        xess_latest(game, swaps)
    if fg == "xess":
        fsr_fg(game, swaps)
    if not swaps.paths:
        return

    def restore():
        n = sum(1 for f in swaps.paths if put_back(f))
        swaps.paths.clear()
        try:
            os.remove(swaps.lock.name)
            swaps.lock.close()
        except OSError:
            pass
        if n:
            say("XESS / FSR_FG: the game's own files are back")

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
