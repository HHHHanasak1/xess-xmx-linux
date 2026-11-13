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
#                           docs/frame-generation.md), with the SDK's libxess.dll, libxess_fg.dll and libxell.dll
#                           next to them. FSR 3.1.0-3.1.3 has everything in one amd_fidelityfx_dx12.dll: AMD's loader
#                           takes its place, with the providers next to it (tools/get-ffx.sh, <package>/ffx)
#   FSR_FG=xess %command%   only the frame generation (the game's own FSR upscaler stays)
#   FSR=xess FSR_FG=inject %command%   for a game with FSR 3.1+ upscaling and no FSR frame generation: dxgi.dll next to
#                           the game gives it an XeSS-FG swap chain, fed from the upscaler's inputs (ffxfg/)
#   OFLOW_FG=1 %command%    experimental, any DX12 game: the same dxgi.dll next to the game's executable, XeSS-FG fed
#                           with motion vectors from optical flow on the final image (FFX_XEFG_OFLOW=1,
#                           docs/frame-generation.md); XESS_FG=<n> sets the frames shown per rendered frame
# Debug: FSR_VENDOR=amd (DXVK reports the GPU as AMD's, for a game that offers FSR only on AMD GPUs; XeSS still gets
# Intel's vendor id from the package's DLLs).
# (XESS_FG=<n>, the frame generation override, needs no hook: ControlLib.dll and the shim read it in the game)
# The game's own old libraries are not given the Intel GPU (DXVK_CONFIG=dxgi.customVendorId = 8086 would): Cyberpunk
# 2077's XeSS-FG 1.1.0.19 then crashes on a null context (libxess_fg +0x238ea4); with the SDK's 1.3.1.78 it works.
# The variables behind them go into Proton's user_settings, which never override one the launch options set themselves.
# XESS=latest: while the game runs, each libxess.dll / libxess_fg.dll / libxell.dll in the game folder that is older
# than the SDK's is renamed to <name>.xmx-orig and replaced by a link to the SDK's file; each libxess_dx11.dll by a link
# to the package's (dx11bridge/, XeSS for D3D11 on D3D12). Every link points to a copy made for the run
# (<lock file>.d/, removed with it), not into the package: Wuthering Waves' launcher 3.0 deleted package files through
# the links, with the SDK's libxess.dll added next to it if the game has
# none there. When Proton exits (the
# game ended, or Steam stopped it) the originals are put back. A run that was killed outright leaves the links; the
# next game start (with or without XESS) or uninstall.sh puts the originals back. Steam starts Proton more than once for
# some games (a launcher, install scripts), so each run that swaps holds a lock file listing its links for as long as
# it lives: links whose lock nobody holds any more are left over; links of a live run are not touched.
# FSR / FSR_FG swap the same way; a file the game does not have (libxess_fg.dll, libxell.dll, the upscaler and frame
# generation DLLs next to the loader) is added as a link (no .xmx-orig) and removed again.
import atexit, fcntl, os, shutil, signal, sys

PKG = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORE = os.path.join(PKG, "xess")
FFXFG = os.path.join(PKG, "ffxfg")   # the package's FidelityFX upscaler and frame generation on XeSS
FFX = os.path.join(PKG, "ffx")   # AMD's loader and providers (tools/get-ffx.sh), for games with FSR 3.1.0-3.1.3
# the package's libxess_dx11.dll: XeSS for D3D11 on the SDK's D3D12 XeSS (dx11bridge/); Intel's forwards to the XeSS of
# the Intel Windows driver, which does not exist here, whatever its version
DX11 = os.path.join(PKG, "dx11bridge")
# the package's vulkan-1.dll for a Vulkan game with XeSS (vkwrap/), and the layer that serves it (layer/)
VKWRAP = os.path.join(PKG, "vkwrap")
LAYER = os.path.join(PKG, "layer")
FG_DLL = "amd_fidelityfx_framegeneration_dx12.dll"
SINGLE_DLL = "amd_fidelityfx_dx12.dll"   # FSR 3.1.0-3.1.3: upscaler and frame generation in one
LOADER_DLL = "amd_fidelityfx_loader_dx12.dll"
UPSCALER_DLL = "amd_fidelityfx_upscaler_dx12.dll"
LIBS = ("libxess.dll", "libxess_dx11.dll", "libxess_fg.dll", "libxell.dll")
# the prefix folder that holds the shim, as XeSS sees it (install.sh: ALT_EXT)
ALT_EXT = "C:\\windows\\system32\\driverstore\\filerepository\\igd_faux.inf_1"
FFX_DLLS = (FG_DLL, UPSCALER_DLL, SINGLE_DLL)
# vkd3d-proton extensions the package turns off (install.sh's VKD3D_DISABLE_EXTENSIONS): the descriptor buffer, as the
# kernel binding code needs vkd3d-proton's classic descriptor heap, and present wait 2 (it crashed frame generation)
VKD3D_OFF = ("VK_EXT_descriptor_buffer", "VK_KHR_present_wait2")
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
    sig = b"\xbd\x04\xef\xfe"
    u16 = lambda b, o: int.from_bytes(b[o:o + 2], "little")
    u32 = lambda b, o: int.from_bytes(b[o:o + 4], "little")

    def resources(f):
        """the resource section's bytes (VS_FIXEDFILEINFO lives there; libxess.dll is tens of MB, its resources a few
        KB), or None when the file is not a PE image with one"""
        h = f.read(64)
        if len(h) < 64 or h[:2] != b"MZ":
            return None
        pe = u32(h, 0x3c)
        f.seek(pe)
        h = f.read(24 + 240)
        if len(h) < 24 or h[:4] != b"PE\0\0":
            return None
        nsec, optsize = u16(h, 6), u16(h, 20)
        opt = h[24:24 + optsize]
        dirs = {0x10b: 92, 0x20b: 108}.get(u16(opt, 0))   # NumberOfRvaAndSizes, PE32 / PE32+
        if dirs is None or len(opt) < dirs + 4 or u32(opt, dirs) < 3 or len(opt) < dirs + 4 + 24:
            return None
        rva, size = u32(opt, dirs + 4 + 16), u32(opt, dirs + 4 + 20)   # data directory 2: resources
        if not rva or not size:
            return None
        f.seek(pe + 24 + optsize)
        secs = f.read(40 * nsec)
        for s in range(min(nsec, len(secs) // 40)):
            va, raw, ptr = u32(secs, 40 * s + 12), u32(secs, 40 * s + 16), u32(secs, 40 * s + 20)
            if va <= rva < va + max(raw, u32(secs, 40 * s + 8)):
                f.seek(ptr + rva - va)
                return f.read(min(size, 64 << 20))
        return None

    def scan(f):
        """the first signature anywhere in the file, read in pieces (not a PE image: as before)"""
        f.seek(0)
        carry = b""
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                return None
            buf = carry + chunk
            i = buf.find(sig)
            if i >= 0:
                d = buf[i:i + 16]
                return d + f.read(16 - len(d)) if len(d) < 16 else d
            carry = buf[-(len(sig) - 1):]

    try:
        with open(path, "rb") as f:
            d = resources(f)
            if d is not None:
                i = d.find(sig)
                d = d[i:i + 16] if i >= 0 else None
            else:
                d = scan(f)
    except OSError:
        return None   # not kept: the file may appear later in this run (a link of ours)
    v = None
    if d is not None and len(d) >= 16:
        ms, ls = u32(d, 8), u32(d, 12)
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
    if d in (STORE, FFXFG, FFX, DX11, VKWRAP):
        return True   # a link of an older version of this script, straight to the package's file
    if os.path.basename(os.path.dirname(os.path.dirname(d))) == "xess-swap" and d.endswith(
            ("/xess", "/ffxfg", "/ffx", "/dx11bridge", "/vkwrap")) and os.path.dirname(d).endswith(".d"):
        return True   # a link to a run's copy (Swaps.link)
    return os.path.basename(d) in ("xess", "ffxfg", "ffx", "dx11bridge", "vkwrap") and (
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
        if name.endswith(".d"):
            if not os.path.exists(lf[:-2]):
                shutil.rmtree(lf, ignore_errors=True)   # the copies of a run whose lock file is gone
            continue
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
                shutil.rmtree(lf + ".d", ignore_errors=True)


class Swaps:
    """the links this run made; held in a lock file while the game runs (clean_left_over)"""

    def __init__(self):
        self.paths = []
        self.lock = None

    def copy(self, target):
        """this run's copy of the package file target (<lock>.d/<target's folder>/<name>). The game's links point here,
        not into the package: a launcher that repairs the game folder through a link (Wuthering Waves' launcher 3.0
        deleted the package's libxell.dll and libxess_dx11.dll that way) then only reaches this copy"""
        d = os.path.join(self.lock.name + ".d", os.path.basename(os.path.dirname(target)))
        c = os.path.join(d, os.path.basename(target))
        if not os.path.isfile(c):
            os.makedirs(d, exist_ok=True)
            shutil.copy2(target, c + ".tmp")
            os.replace(c + ".tmp", c)
        return c

    def link(self, f, target):
        """f (existing or not) becomes a link to this run's copy of target; an existing f is kept as f.xmx-orig"""
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
        os.symlink(self.copy(target), tmp)
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


def xess_dx11(game, f, swaps):
    """XESS=latest, a game's libxess_dx11.dll: the package's in its place, whatever the version, and the SDK's
    libxess.dll next to it if the game has none there (the package's runs XeSS through it)"""
    ours_dx11 = os.path.join(DX11, "libxess_dx11.dll")
    rel = os.path.relpath(f, game)
    if not os.path.isfile(ours_dx11):
        say("XESS=latest: %s kept (%s missing)" % (rel, ours_dx11))
        return
    swaps.link(f, ours_dx11)
    say("XESS=latest: %s %s -> XeSS for D3D11 on D3D12 (dx11bridge)" % (rel, vstr(version(f))))
    top = os.path.dirname(f)
    if not any(n.lower() == "libxess.dll" for n in os.listdir(top)):
        swaps.link(os.path.join(top, "libxess.dll"), os.path.join(STORE, "libxess.dll"))
        say("XESS=latest: %s added (the SDK's, for D3D11 XeSS)" % os.path.relpath(os.path.join(top, "libxess.dll"), game))


def pe_imports(path):
    """the DLL names (lower case) a PE file imports, empty if it cannot be read"""
    u16 = lambda b, o: int.from_bytes(b[o:o + 2], "little")
    u32 = lambda b, o: int.from_bytes(b[o:o + 4], "little")
    try:
        with open(path, "rb") as f:
            d = f.read(64 << 20)   # the import directory is near the start; executables of any size are read in part
    except OSError:
        return set()
    if d[:2] != b"MZ" or len(d) < 0x40:
        return set()
    pe = u32(d, 0x3c)
    if d[pe:pe + 4] != b"PE\0\0":
        return set()
    nsec, optsize = u16(d, pe + 6), u16(d, pe + 20)
    opt = pe + 24
    dd = opt + (112 if u16(d, opt) == 0x20b else 96)   # data directories: PE32+ / PE32
    imp = u32(d, dd + 8)
    secs = [(u32(d, s + 12), max(u32(d, s + 8), u32(d, s + 16)), u32(d, s + 20))
            for s in (opt + optsize + 40 * i for i in range(nsec))]

    def off(rva):
        for va, size, raw in secs:
            if va <= rva < va + size:
                return raw + rva - va
        return None
    names = set()
    o = off(imp) if imp else None
    while o is not None and o + 20 <= len(d) and u32(d, o + 12):
        n = off(u32(d, o + 12))
        if n is not None:
            names.add(d[n:d.find(b"\0", n)].decode("latin-1").lower())
        o += 20
    return names


def xess_vulkan(game, swaps, found):
    """XESS=latest, a Vulkan game with XeSS (its exe imports vulkan-1.dll; XESS_VK=1 forces, XESS_VK=0 skips): the
    package's vulkan-1.dll next to the exe (vkwrap/, Wine's own builtin otherwise wins: vulkan-1=n,b), and the layer,
    which serves XeSS' Vulkan kernels on the package's patched and unpatched Mesa (Xe3). Without the layer XeSS stays
    on DP4a: vkwrap checks that the layer serves the device."""
    force = os.environ.get("XESS_VK", "")
    exe = sys.argv[2] if len(sys.argv) > 2 else ""
    if force == "0" or sys.argv[1] != "waitforexitandrun" or not exe.lower().endswith(".exe") or not os.path.isfile(exe):
        return
    if not named(found, ("libxess.dll",)) or (force != "1" and "vulkan-1.dll" not in pe_imports(exe)):
        return
    ours_vk = os.path.join(VKWRAP, "vulkan-1.dll")
    s = proton_session()
    f = os.path.join(os.path.dirname(exe), "vulkan-1.dll")
    if not os.path.isfile(ours_vk) or not os.path.isfile(os.path.join(LAYER, "libVkLayer_xess_xmx.so")):
        say("XESS=latest (Vulkan): %s or the layer missing; XeSS stays on DP4a" % ours_vk)
        return
    if not s:
        say("XESS=latest (Vulkan): not run by Proton's script; XeSS stays on DP4a")
        return
    if os.path.lexists(f):
        say("XESS=latest (Vulkan): the game has a vulkan-1.dll of its own next to %s; XeSS stays on DP4a" % exe)
        return
    swaps.link(f, ours_vk)
    s.dlloverrides["vulkan-1"] = "n,b"
    # the layer serves XeSS' Vulkan path on the package's patched and unpatched drivers alike (on the patched one it
    # leaves D3D12 kernels to the driver), so the game keeps the driver the session or its launch options picked
    cur = s.env.get("VK_ADD_LAYER_PATH", "")
    if LAYER not in cur.split(":"):
        s.env["VK_ADD_LAYER_PATH"] = LAYER + (":" + cur if cur else "")
    cur = s.env.get("VK_LOADER_LAYERS_ENABLE", "")
    if "VK_LAYER_XESS_xmx" not in cur.split(","):
        s.env["VK_LOADER_LAYERS_ENABLE"] = "VK_LAYER_XESS_xmx" + ("," + cur if cur else "")
    s.env.setdefault("XMX_LAYER_HELPER", os.path.join(PKG, "tools", "cm-compile.sh"))
    s.env.setdefault("XMX_IGC_DIR", os.path.join(PKG, "igc"))
    say("XESS=latest (Vulkan): %s -> XeSS on XMX through vkwrap and the layer (Mesa %s)"
        % (os.path.relpath(f, game) if f.startswith(game) else f, s.env.get("MESA", "default")))


def xess_latest(game, swaps, found=None):
    """found: the files of game_libs(game), when the caller has walked the game folder already"""
    if found is None:
        found = game_libs(game)
    for f in named(found, LIBS):
        if not fresh(f):
            continue
        if os.path.basename(f).lower() == "libxess_dx11.dll":
            xess_dx11(game, f, swaps)
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


def oflow_fg(swaps, game):
    """OFLOW_FG=1: frame generation for any DX12 game, from optical flow on the final image. dxgi.dll and the frame
    generation DLL (as ffx_xefg_bridge.dll) with the SDK's libxess_fg.dll and libxell.dll go next to the executable
    Proton starts (its path is Proton's second argument). Returns True when it is set up."""
    exe = sys.argv[2] if len(sys.argv) > 2 else ""
    if sys.argv[1] != "waitforexitandrun":   # Steam's install scripts and redistributables run with "run"
        return False
    ours_fg, dxgi = os.path.join(FFXFG, FG_DLL), os.path.join(FFXFG, "dxgi.dll")
    if not (os.path.isfile(ours_fg) and os.path.isfile(dxgi) and os.path.isfile(os.path.join(STORE, "libxess_fg.dll"))):
        say("OFLOW_FG: %s, %s or the XeSS SDK (%s, tools/get-xess.sh) missing; no frame generation"
            % (ours_fg, dxgi, STORE))
        return False
    if not exe.lower().endswith(".exe") or not os.path.isfile(exe):
        say("OFLOW_FG: Proton was not given a game executable (%s); no frame generation" % (exe or "nothing"))
        return False
    top = os.path.dirname(os.path.realpath(exe))
    game = os.path.realpath(game)
    if not game or os.path.commonpath([top, game]) != game:
        say("OFLOW_FG: %s is not in the game folder; no frame generation" % exe)
        return False
    # anything there already (the game's own, or a link a killed run left that clean_left_over could not tell as
    # ours): left alone, as linking over a link would turn it into a lasting copy
    for name in ("dxgi.dll", "ffx_xefg_bridge.dll"):
        if os.path.lexists(os.path.join(top, name)):
            say("OFLOW_FG: %s already has a %s; no frame generation" % (top, name))
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


def vkd3d_extensions(s):
    """VKD3D_DISABLE_EXTENSIONS with VKD3D_OFF in it, while the package's driver or layer is on: a value of the user's
    own (launch options), or install.sh's of an older version, keeps its entries and gets the missing ones added"""
    env = s.env if s else os.environ
    if not env.get("ANV_CM_KERNEL_DIR") and "VK_LAYER_XESS_xmx" not in env.get("VK_LOADER_LAYERS_ENABLE", ""):
        return   # not installed, or session-check.sh switched to the stock drivers for this session
    cur = env.get("VKD3D_DISABLE_EXTENSIONS", "")
    have = [e.strip() for e in cur.replace(";", ",").split(",") if e.strip()]
    missing = [e for e in VKD3D_OFF if e not in have]
    if not missing:
        return
    if s:
        s.env["VKD3D_DISABLE_EXTENSIONS"] = ",".join(have + missing)
        if cur:
            say("VKD3D_DISABLE_EXTENSIONS=%s: %s added (XeSS' XMX kernels need them)" % (cur, ",".join(missing)))
    elif cur:
        say("VKD3D_DISABLE_EXTENSIONS=%s lacks %s: XeSS falls back to DP4a; add them" % (cur, ",".join(missing)))
    else:
        SETTINGS["VKD3D_DISABLE_EXTENSIONS"] = ",".join(missing)


def game_folder():
    """the game's folder: Steam's STEAM_COMPAT_INSTALL_PATH, else (a non-Steam shortcut) the folder of the executable
    Proton starts, unless that is too wide to walk and swap in: /, the home folder or one above it, Wine's windows
    folder. "" when there is none"""
    game = os.environ.get("STEAM_COMPAT_INSTALL_PATH", "")
    if game:
        return game
    exe = sys.argv[2] if len(sys.argv) > 2 else ""
    if sys.argv[1] != "waitforexitandrun" or not exe.lower().endswith(".exe") or not os.path.isfile(exe):
        return ""
    top = os.path.dirname(os.path.realpath(exe))
    home = os.path.realpath(os.path.expanduser("~"))
    pfx = os.environ.get("STEAM_COMPAT_DATA_PATH", "")
    win = os.path.realpath(os.path.join(pfx, "pfx", "drive_c", "windows")) if pfx else ""
    if top == os.sep or os.path.commonpath([top, home]) == top or (win and os.path.commonpath([top, win]) == win):
        say("XESS / FSR: no Steam game folder, and %s (the executable's) is too wide to use as one" % top)
        return ""
    say("XESS / FSR: no Steam game folder (a non-Steam game?): %s, the executable's folder, is used" % top)
    return top


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("waitforexitandrun", "run"):
        return
    want = os.environ.get("XESS", "")
    fg = os.environ.get("FSR_FG", "")
    sr = os.environ.get("FSR", "")
    oflow = os.environ.get("OFLOW_FG", "") not in ("", "0")
    clean_left_over()
    # a session started before install.sh added an extension still has the old value until the next login, and a
    # VKD3D_DISABLE_EXTENSIONS in the launch options replaces the session's
    s = proton_session()
    vkd3d_extensions(s)
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
    game = game_folder()
    if not game or not os.path.isdir(game):
        say("XESS / FSR: no game folder (STEAM_COMPAT_INSTALL_PATH, or the executable's for a non-Steam game); the "
            "game's own libraries are used")
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
                shutil.rmtree(swaps.lock.name + ".d", ignore_errors=True)
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
            xess_vulkan(game, swaps, found)
        else:
            say("XESS=latest: no XeSS SDK in %s (run %s/tools/get-xess.sh); the game's own libraries are used"
                % (STORE, PKG))
    if oflow:
        oflow_fg(swaps, game)
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
