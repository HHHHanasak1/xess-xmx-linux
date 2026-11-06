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
#   FSR=xess %command%      a game's FSR 3.1+ runs on XeSS: its amd_fidelityfx_upscaler_dx12.dll and
#                           amd_fidelityfx_framegeneration_dx12.dll become the package's (ffxfg/, XeSS SR and XeSS-FG,
#                           docs/fsr-frame-generation.md), with the SDK's libxess.dll, libxess_fg.dll and libxell.dll
#                           next to them. FSR 3.1.0-3.1.3 has everything in one amd_fidelityfx_dx12.dll: AMD's loader
#                           takes its place, with the providers next to it (tools/get-ffx.sh, <package>/ffx)
#   FSR_FG=xess %command%   only the frame generation (the game's own FSR upscaler stays)
#   FSR=xess FSR_FG=inject %command%   for a game with FSR 3.1+ upscaling and no FSR frame generation: dxgi.dll next to
#                           the game gives it an XeSS-FG swap chain, fed from the upscaler's inputs (ffxfg/)
#   OFLOW_FG=1 %command%    experimental, any DX12 game: the same dxgi.dll next to the game's executable, XeSS-FG fed
#                           with motion vectors from optical flow on the final image (FFX_XEFG_OFLOW=1,
#                           docs/optical-flow-frame-generation.md); XESS_FG=<n> sets the frames shown per rendered frame
# Debug: FSR_VENDOR=amd (DXVK reports the GPU as AMD's, for a game that offers FSR only on AMD GPUs; XeSS still gets
# Intel's vendor id from the package's DLLs).
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
# FSR / FSR_FG swap the same way; a file the game does not have (libxess_fg.dll, libxell.dll, the upscaler and frame
# generation DLLs next to the loader) is added as a link (no .xmx-orig) and removed again.
import atexit, fcntl, os, signal, sys

PKG = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORE = os.path.join(PKG, "xess")
FFXFG = os.path.join(PKG, "ffxfg")   # the package's FidelityFX upscaler and frame generation on XeSS
FFX = os.path.join(PKG, "ffx")   # AMD's loader and providers (tools/get-ffx.sh), for games with FSR 3.1.0-3.1.3
FG_DLL = "amd_fidelityfx_framegeneration_dx12.dll"
SINGLE_DLL = "amd_fidelityfx_dx12.dll"   # FSR 3.1.0-3.1.3: upscaler and frame generation in one
LOADER_DLL = "amd_fidelityfx_loader_dx12.dll"
UPSCALER_DLL = "amd_fidelityfx_upscaler_dx12.dll"
LIBS = ("libxess.dll", "libxess_dx11.dll", "libxess_fg.dll", "libxell.dll")
# the prefix folder that holds the shim, as XeSS sees it (install.sh: ALT_EXT)
ALT_EXT = "C:\\windows\\system32\\driverstore\\filerepository\\igd_faux.inf_1"
FFX_DLLS = (FG_DLL, UPSCALER_DLL, SINGLE_DLL)
# one lock file per run that swapped: its links, one path per line; the run holds an exclusive lock on it until it ends
LOCKS = os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "xess-xmx", "xess-swap")
# variables for the game: user_settings.py passes its dict in; Proton applies them unless the launch options set them
SETTINGS = globals().get("SETTINGS", {})


def say(msg):
    sys.stderr.write("xess-xmx: %s\n" % msg)


_versions = {}   # path -> version: the SDK's DLLs (libxess.dll is big) are asked once per run, not once per game file


def version(path):
    """a.b.c.d from the file's VS_FIXEDFILEINFO, or None"""
    if path in _versions:
        return _versions[path]
    try:
        with open(path, "rb") as f:
            d = f.read()
    except OSError:
        return None   # not kept: the file may appear later in this run (a link of ours)
    i = d.find(b"\xbd\x04\xef\xfe")
    v = None
    if i >= 0:
        ms, ls = int.from_bytes(d[i + 8:i + 12], "little"), int.from_bytes(d[i + 12:i + 16], "little")
        v = (ms >> 16, ms & 0xffff, ls >> 16, ls & 0xffff)
    _versions[path] = v
    return v


def vstr(v):
    return ".".join(map(str, v)) if v else "?"


def game_libs(game, names=LIBS + FFX_DLLS):
    """every file of those names in the game folder, walked once (10 levels deep), in walk order"""
    found = []
    for top, dirs, files in os.walk(game):
        if top[len(game):].count(os.sep) >= 10:
            dirs[:] = []
        for name in files:
            if name.lower() in names:
                found.append(os.path.join(top, name))
    return found


def named(found, names):
    """the files of game_libs() with one of these names"""
    return [f for f in found if os.path.basename(f).lower() in names]


def ours(path, listed=False):
    """a link of this package, or of one in another folder (moved or unpacked elsewhere) that is still there or that a
    lock file lists (listed); a link of the user's own that merely looks alike stays"""
    if not os.path.islink(path):
        return False
    d = os.path.dirname(os.readlink(path))
    if d in (STORE, FFXFG, FFX):
        return True
    return os.path.basename(d) in ("xess", "ffxfg", "ffx") and (
        listed or os.path.isfile(os.path.join(os.path.dirname(d), "tools", "proton-hook.py")))


def put_back(path, listed=False):
    if not os.path.lexists(path) and os.path.isfile(path + ".xmx-orig"):
        os.rename(path + ".xmx-orig", path)   # a run stopped between the two steps of an older link()
        return True
    if not ours(path, listed):
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
                if put_back(p, True):
                    say("XESS / FSR_FG: %s put back (left by a run that was killed)" % p)
            if not any(ours(p, True) for p in paths):
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
            path = os.path.join(LOCKS, "%d-%x" % (os.getpid(), id(self)))
            while True:
                # clean_left_over of a run starting right now may take a still empty file for a dead one and remove it
                self.lock = open(path, "a")
                fcntl.flock(self.lock.fileno(), fcntl.LOCK_EX)
                if os.path.exists(path) and os.path.samestat(os.fstat(self.lock.fileno()), os.stat(path)):
                    break
                self.lock.close()
        self.lock.write(f + "\n")
        self.lock.flush()
        # the link first (a folder without links fails here, with nothing changed); the original stays in place as a
        # hard link until the new link replaces it, so f exists at every moment
        tmp = f + ".xmx-tmp"
        if os.path.lexists(tmp):
            os.remove(tmp)
        os.symlink(target, tmp)
        self.paths.append(f)
        if os.path.exists(f):
            try:
                os.link(f, f + ".xmx-orig")
            except OSError:
                os.rename(f, f + ".xmx-orig")
        os.replace(tmp, f)


def fresh(f):
    """False while another run of this game has f swapped; an .xmx-orig a killed run left behind goes back if f is
    missing, and is dropped if f is there (the game was updated or verified since: f is the original)"""
    if os.path.islink(f):
        return False
    if os.path.exists(f + ".xmx-orig"):
        if os.path.lexists(f):
            os.remove(f + ".xmx-orig")
        else:
            os.rename(f + ".xmx-orig", f)
    return True


def xess_latest(game, swaps, found=None):
    """found: the files of game_libs(game), when the caller has walked the game folder already"""
    if found is None:
        found = game_libs(game)
    for f in named(found, LIBS):
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


def inject_fg(top, swaps, ours_fg):
    """FSR_FG=inject, for a game with FSR upscaling and no frame generation it can use here: our dxgi.dll gives it an
    XeSS-FG swap chain and our upscaler feeds that. Beside the game's DLLs, none of which changes."""
    dxgi = os.path.join(FFXFG, "dxgi.dll")
    f = os.path.join(top, "dxgi.dll")
    if not os.path.isfile(dxgi):
        say("FSR_FG=inject: %s missing" % dxgi)
    elif not any(n.lower().endswith(".exe") for n in os.listdir(top)):
        say("FSR_FG=inject: no game exe next to the upscaler in %s; no frame generation" % top)
    elif os.path.lexists(f):
        say("FSR_FG=inject: the game folder already has a dxgi.dll; no frame generation")
    else:
        swaps.link(f, dxgi)
        swaps.link(os.path.join(top, "ffx_xefg_bridge.dll"), ours_fg)
        say("FSR_FG=inject: XeSS-FG swap chain through %s" % f)


def oflow_fg(swaps):
    """OFLOW_FG=1: frame generation for any DX12 game, from optical flow on the final image. dxgi.dll and the frame
    generation DLL (as ffx_xefg_bridge.dll) with the SDK's libxess_fg.dll and libxell.dll go next to the executable
    Proton starts (its path is Proton's second argument). Returns True when it is set up."""
    exe = sys.argv[2] if len(sys.argv) > 2 else ""
    ours_fg, dxgi = os.path.join(FFXFG, FG_DLL), os.path.join(FFXFG, "dxgi.dll")
    if not (os.path.isfile(ours_fg) and os.path.isfile(dxgi) and os.path.isfile(os.path.join(STORE, "libxess_fg.dll"))):
        say("OFLOW_FG: %s, %s or the XeSS SDK (%s, tools/get-xess.sh) missing; no frame generation"
            % (ours_fg, dxgi, STORE))
        return False
    if not exe.lower().endswith(".exe") or not os.path.isfile(exe):
        say("OFLOW_FG: Proton was not given a game executable (%s); no frame generation" % (exe or "nothing"))
        return False
    top = os.path.dirname(exe)
    if os.path.lexists(os.path.join(top, "dxgi.dll")) and not ours(os.path.join(top, "dxgi.dll")):
        say("OFLOW_FG: %s already has a dxgi.dll; no frame generation" % top)
        return False
    if os.path.lexists(os.path.join(top, FG_DLL)) or os.path.lexists(os.path.join(top, SINGLE_DLL)):
        say("OFLOW_FG: the game has FSR frame generation of its own (FSR=xess runs it on XeSS-FG with the game's inputs)")
    swaps.link(os.path.join(top, "dxgi.dll"), dxgi)
    swaps.link(os.path.join(top, "ffx_xefg_bridge.dll"), ours_fg)
    xess_libs(top, swaps, ["libxess_fg.dll", "libxell.dll"])
    SETTINGS["FFX_XEFG_OFLOW"] = "1"
    say("OFLOW_FG: XeSS-FG from optical flow next to %s" % os.path.basename(exe))
    return True


def fsr(game, swaps, fg, sr, inject=False, found=None):
    """FSR=xess (upscaling on XeSS SR and frame generation on XeSS-FG) and FSR_FG=xess (frame generation only) in
    every folder with FidelityFX DLLs. FSR 3.1.4+ has a loader with the providers next to it, and only those are
    swapped. FSR 3.1.0-3.1.3 has everything in one amd_fidelityfx_dx12.dll: AMD's loader (the same five exports; it
    loads the providers by name from its own folder) takes its place, with an upscaler (ours, or AMD's for FSR_FG=xess)
    and a frame generation provider next to it. found: the files of game_libs(game), when the caller has walked the
    game folder already. Returns True when something was swapped."""
    ours_up, ours_fg = os.path.join(FFXFG, UPSCALER_DLL), os.path.join(FFXFG, FG_DLL)
    loader, amd_up, amd_fg = (os.path.join(FFX, n) for n in (LOADER_DLL, UPSCALER_DLL, FG_DLL))
    if fg and not (os.path.isfile(ours_fg) and os.path.isfile(os.path.join(STORE, "libxess_fg.dll"))):
        say("FSR: %s or the XeSS SDK (%s, tools/get-xess.sh) missing; the game's own frame generation is used"
            % (ours_fg, STORE))
        fg = False
    if sr == "xess" and not (os.path.isfile(ours_up) and os.path.isfile(os.path.join(STORE, "libxess.dll"))):
        say("FSR=xess: %s or the XeSS SDK (%s, tools/get-xess.sh) missing; the game's own upscaler is used"
            % (ours_up, STORE))
        sr = ""
    up = ours_up if sr == "xess" else None
    if not (fg or up):
        return False
    if found is None:
        found = game_libs(game)
    folders = {}
    for f in named(found, FFX_DLLS):
        folders.setdefault(os.path.dirname(f), set()).add(os.path.basename(f).lower())
    if not folders:
        say("FSR: no FidelityFX DLL in the game folder (the game has no FSR 3.1+)")
    done = False
    for top, names in sorted(folders.items()):
        rel = os.path.relpath(top, game)
        swapped = False
        if SINGLE_DLL in names and not names & {FG_DLL, UPSCALER_DLL}:
            single = os.path.join(top, SINGLE_DLL)
            need = [loader] + ([] if up else [amd_up]) + ([] if fg else [amd_fg])
            if not all(os.path.isfile(f) for f in need):
                say("FSR: %s has FSR 3.1.0-3.1.3 (one %s); it needs AMD's loader and providers in %s "
                    "(run %s/tools/get-ffx.sh)" % (rel, SINGLE_DLL, FFX, PKG))
                continue
            if not fresh(single):
                continue
            swaps.link(single, loader)
            swaps.link(os.path.join(top, UPSCALER_DLL), up or amd_up)
            swaps.link(os.path.join(top, FG_DLL), ours_fg if fg else amd_fg)
            # the game's DLL is now single.xmx-orig (link() keeps it there); its version only for this line
            say("FSR: %s FidelityFX %s in one DLL -> AMD's loader, upscaler %s, frame generation %s"
                % (rel, vstr(version(single + ".xmx-orig")), "XeSS SR" if up else "AMD's FSR 3.1",
                   "XeSS-FG" if fg else "AMD's"))
            swapped = True
        else:
            if fg and FG_DLL in names and fresh(os.path.join(top, FG_DLL)):
                swaps.link(os.path.join(top, FG_DLL), ours_fg)
                say("FSR: %s -> XeSS-FG" % os.path.join(rel, FG_DLL))
                swapped = True
            if up and UPSCALER_DLL in names and fresh(os.path.join(top, UPSCALER_DLL)):
                swaps.link(os.path.join(top, UPSCALER_DLL), up)
                say("FSR: %s -> XeSS SR" % os.path.join(rel, UPSCALER_DLL))
                swapped = True
        if not swapped:
            continue
        done = True
        xess_libs(top, swaps, (["libxess_fg.dll", "libxell.dll"] if fg else []) + (["libxess.dll"] if up else []))
        if inject and fg and up:
            if names & {FG_DLL, SINGLE_DLL}:
                say("FSR_FG=inject: %s has FSR frame generation of its own, which FSR=xess runs on XeSS-FG; nothing to inject"
                    % rel)
            else:
                inject_fg(top, swaps, ours_fg)
    return done


def xess_libs(top, swaps, names):
    """the SDK's libraries next to the FidelityFX providers that use them, unless the game's own are newer"""
    for name in names:
        lib = os.path.join(top, name)
        if os.path.lexists(lib) and not fresh(lib):
            continue   # already a link of ours (XESS=latest)
        vg, vs = version(lib), version(os.path.join(STORE, name))
        if vg and vs and vg > vs:
            say("FSR: %s %s kept (newer than the SDK's %s)" % (name, vstr(vg), vstr(vs)))
            continue
        swaps.link(lib, os.path.join(STORE, name))


def prefix_has_shim():
    """an igdext64.dll in the prefix's driver store, the folders in any case: Wine finds files case-insensitively, and
    proton-cachyos makes "DriverStore" next to the "driverstore" other Protons and install.sh use"""
    level = [os.path.join(os.environ.get("STEAM_COMPAT_DATA_PATH", ""), "pfx", "drive_c", "windows", "system32")]
    for part in ("driverstore", "filerepository", "igd_faux.inf_1"):
        nxt = []
        for d in level:
            try:
                nxt += [os.path.join(d, e) for e in os.listdir(d) if e.lower() == part]
            except OSError:
                pass
        level = nxt
    return any(os.path.isfile(os.path.join(d, "igdext64.dll")) for d in level)


def proton_session():
    """Proton's Session while it imports user_settings.py: its env is a copy of the environment made before that, and its
    dlloverrides go into WINEDLLOVERRIDES after it"""
    s = getattr(sys.modules.get("__main__"), "g_session", None)
    return s if isinstance(getattr(s, "env", None), dict) and isinstance(getattr(s, "dlloverrides", None), dict) else None


def add_env(key, value, has):
    """value for key; one the environment already has (launch options) gets it appended, unless it has <has> in it"""
    s = proton_session()
    cur = (s.env if s else os.environ).get(key)
    if cur is None:
        SETTINGS[key] = value
    elif has in cur:
        say("%s=%s from the launch options stays as it is" % (key, cur))
    elif s:
        s.env[key] = cur + "; " + value
    else:
        say("%s is set in the launch options, so it stays as it is; add %s there" % (key, value))


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("waitforexitandrun", "run"):
        return
    want = os.environ.get("XESS", "")
    fg = os.environ.get("FSR_FG", "")
    sr = os.environ.get("FSR", "")
    oflow = os.environ.get("OFLOW_FG", "") not in ("", "0")
    game = os.environ.get("STEAM_COMPAT_INSTALL_PATH", "")
    clean_left_over()
    # a session started before install.sh disabled present wait 2 as well (frame generation crashed in
    # vkWaitForPresent2KHR) still has the old value until the next login
    s = proton_session()
    if s and s.env.get("VKD3D_DISABLE_EXTENSIONS") == "VK_EXT_descriptor_buffer":
        s.env["VKD3D_DISABLE_EXTENSIONS"] = "VK_EXT_descriptor_buffer,VK_KHR_present_wait2"
    # SteamOS Game Mode hands the session's environment to Steam through a systemd EnvironmentFile
    # ($XDG_RUNTIME_DIR/gamescope-environment, written with `env`), which drops the backslashes of an unquoted value:
    # install.sh's path then arrives as C:windowssystem32... and XeSS never finds the shim under Proton 11.0
    if s and s.env.get("INTC_ALT_DRIVER_EXTENSIONS_PATH") == ALT_EXT.replace("\\", ""):
        s.env["INTC_ALT_DRIVER_EXTENSIONS_PATH"] = ALT_EXT
    if os.environ.get("XESS_LOG", "") not in ("", "0"):
        SETTINGS.update({"IGDEXT_TRACE": "1", "IGDEXT_XELL_LOG": "1", "IGDEXT_XEFG_LOG": "1", "XESS_FG_LOG": "1"})
        say("XESS_LOG: C:\\igdext_trace.log, igdext_xell.log, igdext_xefg.log, igcl_stub.log in the prefix")
    if want == "dp4a":
        SETTINGS["IGDEXT_FORCE_FALLBACK"] = "1"
    elif want and want != "latest":
        say("XESS=%s: unknown value (latest, dp4a); ignored" % want)
    inject = fg == "inject"
    if inject and sr != "xess":
        say("FSR_FG=inject needs FSR=xess; ignored")
    if fg and fg not in ("xess", "inject"):
        say("FSR_FG=%s: unknown value (xess, inject); ignored" % fg)
    if sr and sr != "xess":
        say("FSR=%s: unknown value (xess); ignored" % sr)
        sr = ""
    if sr == "xess":
        fg = "xess"
    if oflow and (fg or sr):
        say("OFLOW_FG=1 with FSR / FSR_FG: OFLOW_FG is left out (the game's own inputs are better than optical flow)")
        oflow = False
    if want != "latest" and fg != "xess" and not sr and not oflow:
        return
    if not game or not os.path.isdir(game):
        say("XESS / FSR: Steam gave no game folder (STEAM_COMPAT_INSTALL_PATH); the game's own libraries are used")
        return
    swaps = Swaps()
    # before the first swap: if one of them fails, the top level catches it and the game still starts, and what was
    # swapped must go back when it exits
    def restore():
        n = sum(1 for f in swaps.paths if put_back(f))
        swaps.paths.clear()
        try:
            if swaps.lock:
                os.remove(swaps.lock.name)
                swaps.lock.close()
        except OSError:
            pass
        if n:
            say("XESS / FSR: the game's own files are back")

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
    old = [(s, signal.signal(s, on_signal)) for s in sigs]

    found = game_libs(game)   # the game folder walked once for every option
    if want == "latest":
        if os.path.isfile(os.path.join(STORE, "libxess.dll")):
            xess_latest(game, swaps, found)
        else:
            say("XESS=latest: no XeSS SDK in %s (run %s/tools/get-xess.sh); the game's own libraries are used"
                % (STORE, PKG))
    if oflow:
        oflow_fg(swaps)
    if fsr(game, swaps, fg == "xess", sr, inject, found) and os.environ.get("FSR_VENDOR", "") == "amd":
        # debug, for a game that offers FSR only on AMD GPUs; XeSS still gets Intel's vendor id from the package's DLLs
        add_env("DXVK_CONFIG", "dxgi.customVendorId = 1002", "customVendorId")
    if swaps.paths:
        # XeSS finds the shim in the prefix's driver store only through this variable under Valve's Proton 11;
        # GE-Proton sets it itself
        s = proton_session()
        env = s.env if s else os.environ
        if prefix_has_shim() and "INTC_ALT_DRIVER_EXTENSIONS_PATH" not in env:
            SETTINGS["INTC_ALT_DRIVER_EXTENSIONS_PATH"] = ALT_EXT
    if not swaps.paths:
        for sig, h in old:
            signal.signal(sig, h)   # nothing to put back: this run does not touch how Proton ends


try:
    main()
except Exception as e:  # never stop a game from starting
    say("XESS: %s" % e)
