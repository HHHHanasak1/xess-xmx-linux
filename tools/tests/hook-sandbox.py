#!/usr/bin/env python3
# hook-sandbox.py - runs tools/proton-hook.py the way Proton does (user_settings.py's exec, "proton waitforexitandrun
# <exe>", a g_session with env and dlloverrides) on made-up game folders, one per kind of game the launch options are
# about, and checks what the hook swaps while "the game runs", which variables it gives the game, and that the game
# folder is as it was afterwards.
#   1. the launch lines that existed before XESS=auto (OLD): the links and variables are the ones that hook made
#   2. XESS=auto and the other new forms (NEW): the links of the old line for that kind of game, and the route and
#      the reason in the hook's lines
#   3. XESS_LL, XESS_LOG, the hook's own log
#   4. a run that is killed outright, and a second run of a game whose first run is still alive
# The sandbox is a temporary folder with a package made of this tree's hook plus stand-ins for the binaries, a HOME
# and a prefix: nothing outside it is touched, and no Proton, Steam, GPU or network is needed.
# Run: python3 tools/tests/hook-sandbox.py     (KEEP=1 keeps the sandbox and prints its path)
import hashlib, json, os, shutil, struct, subprocess, sys, tempfile, time

TREE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UP, FG, ONE, LOADER = ("amd_fidelityfx_upscaler_dx12.dll", "amd_fidelityfx_framegeneration_dx12.dll",
                       "amd_fidelityfx_dx12.dll", "amd_fidelityfx_loader_dx12.dll")
SDK = {"libxess.dll": (2, 0, 2, 68), "libxess_fg.dll": (1, 3, 1, 78), "libxell.dll": (1, 3, 2, 10),
       "libxess_dx11.dll": (2, 0, 2, 68)}
# kind of game -> the files next to its game.exe (a version: the file has a VS_FIXEDFILEINFO the hook reads)
GAMES = {
    # XeSS and XeSS-FG older than the SDK's, and FSR 3.1+ with frame generation
    "old-xess+fsr": {"libxess.dll": (2, 0, 1, 41), "libxess_dx11.dll": (2, 0, 1, 41), "libxess_fg.dll": (1, 1, 0, 19),
                     "libxell.dll": (1, 1, 0, 6), LOADER: None, UP: None, FG: None},
    # XeSS as new as the SDK's with XeSS-FG (and an older libxell.dll), and FSR 3.1.0-3.1.3 in one DLL
    "new-xess+fsr1": {"libxess.dll": (2, 0, 2, 68), "libxess_dx11.dll": (2, 0, 2, 68), "libxess_fg.dll": (1, 3, 1, 78),
                      "libxell.dll": (1, 3, 0, 5), ONE: (1, 0, 1, 41314)},
    # FSR 3.1+ upscaling and frame generation, no XeSS
    "fsr-fg": {LOADER: None, UP: None, FG: None},
    # FSR 3.1.0-3.1.3 in one DLL, no XeSS
    "fsr-one": {ONE: (1, 0, 1, 41314)},
    # XeSS super resolution, FSR 3.1+ upscaling, no frame generation an Intel GPU can use
    "xess-sr+fsr-up": {"libxess.dll": (2, 0, 2, 68), LOADER: None, UP: None},
    # XeSS-FG shipped next to FSR 3.1+ with frame generation (a game whose menu may not offer its XeSS-FG)
    "xess-fg+fsr-fg": {"libxess.dll": (2, 0, 2, 68), "libxess_fg.dll": (1, 2, 0, 87), "libxell.dll": (1, 2, 0, 9),
                       LOADER: None, UP: None, FG: None},
    # neither XeSS nor FSR 3.1+
    "neither": {},
    # a D3D11 game with XeSS
    "dx11": {"libxess_dx11.dll": (1, 3, 1, 32)},
    # a Vulkan game with XeSS (XESS_VK=1 stands for an executable that imports vulkan-1.dll)
    "vulkan": {"libxess.dll": (2, 0, 2, 68)},
}
# How an outcome is written below: the links a run made, then " | " and the variables it gave the game.
# A link: the file in the game folder and the package folder its target comes from.
LINKS = {"xess": ("libxess.dll", "xess"), "xefg": ("libxess_fg.dll", "xess"), "xell": ("libxell.dll", "xess"),
         "dx11": ("libxess_dx11.dll", "dx11bridge"), "vk": ("vulkan-1.dll", "vkwrap"),
         "up": (UP, "ffxfg"), "fg": (FG, "ffxfg"),             # the package's providers: XeSS SR, XeSS-FG
         "loader": (ONE, "ffx"), "amd-up": (UP, "ffx"),        # AMD's loader in place of the single DLL, AMD's upscaler
         "dxgi": ("dxgi.dll", "ffxfg"), "bridge": ("ffx_xefg_bridge.dll", "ffxfg")}
# Variables: Proton's user_settings (applied unless the launch options have that variable), what the hook changed in
# Proton's environment, and its DLL overrides ("override <dll>")
VARS = {"ALT": {"INTC_ALT_DRIVER_EXTENSIONS_PATH": "C:\\windows\\system32\\driverstore\\filerepository\\igd_faux.inf_1"},
        "LAYER": {"VK_ADD_LAYER_PATH": "<pkg>/layer", "VK_LOADER_LAYERS_ENABLE": "VK_LAYER_XESS_xmx",
                  "XMX_LAYER_HELPER": "<pkg>/tools/cm-compile.sh", "XMX_IGC_DIR": "<pkg>/igc", "override vulkan-1": "n,b"},
        "DP4A": {"IGDEXT_FORCE_FALLBACK": "1"},
        "LOGS": {"IGDEXT_TRACE": "1", "IGDEXT_XELL_LOG": "1", "IGDEXT_XEFG_LOG": "1", "XESS_FG_LOG": "1"},
        "OFLOW": {"FFX_XEFG_OFLOW": "1"},
        "AMD": {"DXVK_CONFIG": "dxgi.customVendorId = 1002"}}
# What the hook did with each of its launch lines before XESS=auto (main 0988d44), per kind of game: outcome -> the
# lines that gave it. These pin the old behaviour: a change here changes what a launch line in somebody's Steam does.
OLD = {
    "old-xess+fsr": {
        "": ["", "XESS_FG=3"],
        "xess xefg xell dx11 | ALT": ["XESS=latest", "XESS=latest XESS_FG=3"],
        "xess xefg xell dx11 vk | ALT LAYER": ["XESS=latest XESS_VK=1"],
        " | DP4A": ["XESS=dp4a"],
        " | LOGS": ["XESS_LOG=1"],
        "xess xefg xell up fg | ALT": ["FSR=xess", "FSR=xess XESS_FG=4", "FSR=xess FSR_FG=inject", "OFLOW_FG=1 FSR=xess"],
        "xefg xell fg | ALT": ["FSR_FG=xess"],
        "xefg xell dxgi bridge | ALT OFLOW": ["OFLOW_FG=1", "OFLOW_FG=1 XESS_FG=2"],
        "xess xefg xell up fg | ALT AMD": ["FSR=xess FSR_VENDOR=amd"],
        "xess xefg xell dx11 up fg | ALT": ["XESS=latest FSR=xess", "XESS=latest FSR=xess FSR_FG=inject"],
        "xess xefg xell dx11 fg | ALT": ["XESS=latest FSR_FG=xess"],
    },
    "new-xess+fsr1": {
        "": ["", "XESS_FG=3"],
        "xell dx11 | ALT": ["XESS=latest", "XESS=latest XESS_FG=3"],
        "xell dx11 vk | ALT LAYER": ["XESS=latest XESS_VK=1"],
        " | DP4A": ["XESS=dp4a"],
        " | LOGS": ["XESS_LOG=1"],
        "xess xefg xell up fg loader | ALT": ["FSR=xess", "FSR=xess XESS_FG=4", "FSR=xess FSR_FG=inject", "OFLOW_FG=1 FSR=xess"],
        "xefg xell fg loader amd-up | ALT": ["FSR_FG=xess"],
        "xefg xell dxgi bridge | ALT OFLOW": ["OFLOW_FG=1", "OFLOW_FG=1 XESS_FG=2"],
        "xess xefg xell up fg loader | ALT AMD": ["FSR=xess FSR_VENDOR=amd"],
        "xess xefg xell dx11 up fg loader | ALT": ["XESS=latest FSR=xess", "XESS=latest FSR=xess FSR_FG=inject"],
        "xefg xell dx11 fg loader amd-up | ALT": ["XESS=latest FSR_FG=xess"],
    },
    "fsr-fg": {
        "": ["", "XESS_FG=3", "XESS=latest", "XESS=latest XESS_FG=3", "XESS=latest XESS_VK=1"],
        " | DP4A": ["XESS=dp4a"],
        " | LOGS": ["XESS_LOG=1"],
        "xess xefg xell up fg | ALT": ["FSR=xess", "FSR=xess XESS_FG=4", "FSR=xess FSR_FG=inject", "OFLOW_FG=1 FSR=xess",
                                       "XESS=latest FSR=xess", "XESS=latest FSR=xess FSR_FG=inject"],
        "xefg xell fg | ALT": ["FSR_FG=xess", "XESS=latest FSR_FG=xess"],
        "xefg xell dxgi bridge | ALT OFLOW": ["OFLOW_FG=1", "OFLOW_FG=1 XESS_FG=2"],
        "xess xefg xell up fg | ALT AMD": ["FSR=xess FSR_VENDOR=amd"],
    },
    "fsr-one": {
        "": ["", "XESS_FG=3", "XESS=latest", "XESS=latest XESS_FG=3", "XESS=latest XESS_VK=1"],
        " | DP4A": ["XESS=dp4a"],
        " | LOGS": ["XESS_LOG=1"],
        "xess xefg xell up fg loader | ALT": ["FSR=xess", "FSR=xess XESS_FG=4", "FSR=xess FSR_FG=inject", "OFLOW_FG=1 FSR=xess",
                                              "XESS=latest FSR=xess", "XESS=latest FSR=xess FSR_FG=inject"],
        "xefg xell fg loader amd-up | ALT": ["FSR_FG=xess", "XESS=latest FSR_FG=xess"],
        "xefg xell dxgi bridge | ALT OFLOW": ["OFLOW_FG=1", "OFLOW_FG=1 XESS_FG=2"],
        "xess xefg xell up fg loader | ALT AMD": ["FSR=xess FSR_VENDOR=amd"],
    },
    "xess-sr+fsr-up": {
        "": ["", "XESS_FG=3", "XESS=latest", "XESS=latest XESS_FG=3", "FSR_FG=xess", "XESS=latest FSR_FG=xess"],
        "vk | ALT LAYER": ["XESS=latest XESS_VK=1"],
        " | DP4A": ["XESS=dp4a"],
        " | LOGS": ["XESS_LOG=1"],
        "xess xefg xell up | ALT": ["FSR=xess", "FSR=xess XESS_FG=4", "OFLOW_FG=1 FSR=xess", "XESS=latest FSR=xess"],
        "xess xefg xell up dxgi bridge | ALT": ["FSR=xess FSR_FG=inject", "XESS=latest FSR=xess FSR_FG=inject"],
        "xefg xell dxgi bridge | ALT OFLOW": ["OFLOW_FG=1", "OFLOW_FG=1 XESS_FG=2"],
        "xess xefg xell up | ALT AMD": ["FSR=xess FSR_VENDOR=amd"],
    },
    "xess-fg+fsr-fg": {
        "": ["", "XESS_FG=3"],
        "xefg xell | ALT": ["XESS=latest", "XESS=latest XESS_FG=3"],
        "xefg xell vk | ALT LAYER": ["XESS=latest XESS_VK=1"],
        " | DP4A": ["XESS=dp4a"],
        " | LOGS": ["XESS_LOG=1"],
        "xess xefg xell up fg | ALT": ["FSR=xess", "FSR=xess XESS_FG=4", "FSR=xess FSR_FG=inject", "OFLOW_FG=1 FSR=xess",
                                       "XESS=latest FSR=xess", "XESS=latest FSR=xess FSR_FG=inject"],
        "xefg xell fg | ALT": ["FSR_FG=xess", "XESS=latest FSR_FG=xess"],
        "xefg xell dxgi bridge | ALT OFLOW": ["OFLOW_FG=1", "OFLOW_FG=1 XESS_FG=2"],
        "xess xefg xell up fg | ALT AMD": ["FSR=xess FSR_VENDOR=amd"],
    },
    "neither": {
        "": ["", "XESS_FG=3", "XESS=latest", "XESS=latest XESS_FG=3", "XESS=latest XESS_VK=1", "FSR=xess",
             "FSR=xess XESS_FG=4", "FSR_FG=xess", "FSR=xess FSR_FG=inject", "FSR=xess FSR_VENDOR=amd",
             "OFLOW_FG=1 FSR=xess", "XESS=latest FSR=xess", "XESS=latest FSR_FG=xess", "XESS=latest FSR=xess FSR_FG=inject"],
        " | DP4A": ["XESS=dp4a"],
        " | LOGS": ["XESS_LOG=1"],
        "xefg xell dxgi bridge | ALT OFLOW": ["OFLOW_FG=1", "OFLOW_FG=1 XESS_FG=2"],
    },
    "dx11": {
        "": ["", "XESS_FG=3", "FSR=xess", "FSR=xess XESS_FG=4", "FSR_FG=xess", "FSR=xess FSR_FG=inject",
             "FSR=xess FSR_VENDOR=amd", "OFLOW_FG=1 FSR=xess"],
        "xess dx11 | ALT": ["XESS=latest", "XESS=latest XESS_FG=3", "XESS=latest XESS_VK=1", "XESS=latest FSR=xess",
                            "XESS=latest FSR_FG=xess", "XESS=latest FSR=xess FSR_FG=inject"],
        " | DP4A": ["XESS=dp4a"],
        " | LOGS": ["XESS_LOG=1"],
        "xefg xell dxgi bridge | ALT OFLOW": ["OFLOW_FG=1", "OFLOW_FG=1 XESS_FG=2"],
    },
    "vulkan": {
        "": ["", "XESS_FG=3", "XESS=latest", "XESS=latest XESS_FG=3", "FSR=xess", "FSR=xess XESS_FG=4", "FSR_FG=xess",
             "FSR=xess FSR_FG=inject", "FSR=xess FSR_VENDOR=amd", "OFLOW_FG=1 FSR=xess", "XESS=latest FSR=xess",
             "XESS=latest FSR_FG=xess", "XESS=latest FSR=xess FSR_FG=inject"],
        "vk | ALT LAYER": ["XESS=latest XESS_VK=1"],
        " | DP4A": ["XESS=dp4a"],
        " | LOGS": ["XESS_LOG=1"],
        "xefg xell dxgi bridge | ALT OFLOW": ["OFLOW_FG=1", "OFLOW_FG=1 XESS_FG=2"],
    },
}
# The new forms: kind of game, launch line, the old line whose links it must make, what the XESS=auto line must say
NEW = [
    # the game ships XeSS frame generation: its own XeSS, FSR left alone
    ("old-xess+fsr", "XESS=auto XESS_FG=3", "XESS=latest", "its own XeSS is used, FSR is left alone"),
    ("new-xess+fsr1", "XESS=auto XESS_FG=3", "XESS=latest", "its own XeSS is used, FSR is left alone"),
    ("xess-fg+fsr-fg", "XESS=auto XESS_FG=4", "XESS=latest", "its own XeSS is used, FSR is left alone"),
    # no XeSS frame generation, FSR 3.1+ with frame generation: FSR on XeSS
    ("fsr-fg", "XESS=auto XESS_FG=3", "FSR=xess", "FSR runs on XeSS (as FSR=xess)"),
    ("fsr-fg", "XESS=auto", "FSR=xess", "FSR runs on XeSS (as FSR=xess)"),
    ("fsr-one", "XESS=auto XESS_FG=3", "FSR=xess", "FSR runs on XeSS (as FSR=xess)"),
    # an FSR upscaler without frame generation: XeSS-FG is added only when frame generation is asked for
    ("xess-sr+fsr-up", "XESS=auto XESS_FG=3", "XESS=latest FSR=xess FSR_FG=inject", "XeSS-FG is added to it"),
    ("xess-sr+fsr-up", "XESS=auto", "XESS=latest FSR=xess", "XESS_FG=<n> adds frame generation to it"),
    # neither: nothing beyond the game's own XeSS
    ("neither", "XESS=auto XESS_FG=2", "", "OFLOW_FG=1)"),
    ("neither", "XESS=auto", "", "only the game's own XeSS is looked after"),
    ("dx11", "XESS=auto", "XESS=latest", "only the game's own XeSS is looked after"),
    ("vulkan", "XESS=auto XESS_VK=1", "XESS=latest XESS_VK=1", "only the game's own XeSS is looked after"),
    # a route in the launch options decides, also against the game's own XeSS-FG
    ("xess-fg+fsr-fg", "XESS=auto FSR=xess XESS_FG=4", "XESS=latest FSR=xess", "the route is set in the launch options (FSR=xess)"),
    ("fsr-fg", "XESS=auto FSR_FG=xess", "XESS=latest FSR_FG=xess", "the route is set in the launch options (FSR_FG=xess)"),
    # OFLOW_FG=1 next to XESS=auto: only where nothing else fits; it asks for frame generation
    ("neither", "XESS=auto OFLOW_FG=1", "OFLOW_FG=1", "frame generation from optical flow"),
    ("fsr-fg", "XESS=auto OFLOW_FG=1", "FSR=xess", "OFLOW_FG=1 is left out"),
    ("old-xess+fsr", "XESS=auto OFLOW_FG=1", "XESS=latest", "OFLOW_FG=1 is left out"),
    ("xess-sr+fsr-up", "XESS=auto OFLOW_FG=1", "XESS=latest FSR=xess FSR_FG=inject", "OFLOW_FG=1 is left out"),
    # FSR_FG=inject alone means FSR=xess FSR_FG=inject (the old hook ignored it)
    ("xess-sr+fsr-up", "FSR_FG=inject XESS_FG=3", "FSR=xess FSR_FG=inject", None),
    ("fsr-fg", "FSR_FG=inject", "FSR=xess FSR_FG=inject", None),
    # XESS=auto_off: no XESS=auto for this game, and nothing else changes
    ("fsr-fg", "XESS=auto_off XESS_FG=3", "", None),
    ("old-xess+fsr", "XESS=auto_off", "", None),
    ("fsr-fg", "XESS=auto_off FSR=xess", "FSR=xess", None),
]


def dll(path, ver=None, text=""):
    """a stand-in for a DLL; with a version, one the hook's version() reads"""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = ("stand-in %s %s\n" % (os.path.basename(path), text)).encode()
    if ver:
        data += b"\xbd\x04\xef\xfe" + struct.pack("<III", 0x10000, ver[0] << 16 | ver[1], ver[2] << 16 | ver[3])
    with open(path, "wb") as f:
        f.write(data)


def make(sb, hook=None):
    """the sandbox: a package (hook: another proton-hook.py than this tree's), a prefix with the shim, the games"""
    pkg = os.path.join(sb, "pkg dir")   # a space in the path
    for n, v in SDK.items():
        dll(os.path.join(pkg, "xess", n), v, "SDK")
    for n in (UP, FG, "dxgi.dll"):
        dll(os.path.join(pkg, "ffxfg", n), None, "package")
    for n in (LOADER, UP, FG):
        dll(os.path.join(pkg, "ffx", n), None, "AMD")
    dll(os.path.join(pkg, "dx11bridge", "libxess_dx11.dll"), (2, 0, 2, 68), "bridge")
    dll(os.path.join(pkg, "vkwrap", "vulkan-1.dll"), None, "vkwrap")
    dll(os.path.join(pkg, "layer", "libVkLayer_xess_xmx.so"), None, "layer")
    os.makedirs(os.path.join(pkg, "tools"))
    shutil.copy(hook or os.path.join(TREE, "tools", "proton-hook.py"), os.path.join(pkg, "tools", "proton-hook.py"))
    # with the shim in the prefix the hook hands the game INTC_ALT_DRIVER_EXTENSIONS_PATH whenever it swapped
    dll(os.path.join(sb, "compat", "pfx", "drive_c", "windows", "system32", "driverstore", "filerepository",
                     "igd_faux.inf_1", "igdext64.dll"), None, "shim")
    for g, files in GAMES.items():
        dll(os.path.join(sb, "games", g, "b", "game.exe"), None, "exe")
        for n, v in files.items():
            dll(os.path.join(sb, "games", g, "b", n), v, "game")
    return pkg


def state(top):
    """every file and link of a folder, with its content's hash or its target"""
    out = {}
    for d, _, files in os.walk(top):
        for n in files:
            p = os.path.join(d, n)
            out[os.path.relpath(p, top)] = ("-> " + os.readlink(p) if os.path.islink(p)
                                            else hashlib.sha1(open(p, "rb").read()).hexdigest())
    return out


def outcome(links, env):
    """what a run did, written with the names of LINKS and VARS; anything they do not know stays as it is"""
    out = [t for t, (n, d) in LINKS.items() if links.get(n) == d]
    out += sorted("%s=%s" % i for i in links.items() if i not in LINKS.values())
    env, names = dict(env), []
    for t, d in VARS.items():
        if all(env.get(k) == v for k, v in d.items()):
            names.append(t)
            for k in d:
                del env[k]
    names += sorted("%s=%s" % i for i in env.items())
    return " ".join(out) + (" | " + " ".join(names) if names else "")


def child():
    """one run of the hook as Proton makes it: prints what is swapped while "the game runs". --die: the run ends
    without the hook's restore (killed outright). --hold <file>: it stays alive until that file exists"""
    hook, game, exe = sys.argv[2:5]
    more = sys.argv[5:]

    class Session:   # Proton's g_session, as far as the hook uses it
        env = dict(os.environ)
        dlloverrides = {}
    sys.modules["__main__"].g_session = Session
    sys.argv = ["proton", "waitforexitandrun", exe]
    settings, lines, err = {}, [], sys.stderr

    class Tee:
        def write(self, s):
            lines.append(s)

        def flush(self):
            pass
    sys.stderr = Tee()
    exec(compile(open(hook).read(), hook, "exec"), {"__file__": hook, "__name__": "xess_xmx_hook", "SETTINGS": settings})
    sys.stderr = err
    pkg = os.path.dirname(os.path.dirname(hook))
    links = {}
    for d, _, files in os.walk(game):
        for n in files:
            p = os.path.join(d, n)
            if os.path.islink(p):   # -> <cache>/xess-swap/<lock>.d/<package folder>/<file>
                links[n] = os.path.basename(os.path.dirname(os.readlink(p)))
    env = {k: v for k, v in Session.env.items() if os.environ.get(k) != v}
    env.update({k: v for k, v in settings.items() if k not in Session.env})   # as Proton applies user_settings
    env.update({"override " + k: v for k, v in Session.dlloverrides.items()})
    env = {k: str(v).replace(pkg, "<pkg>") for k, v in env.items()}
    print(json.dumps({"out": outcome(links, env), "log": "".join(lines).replace(pkg, "<pkg>").splitlines()}), flush=True)
    if "--die" in more:
        os._exit(9)
    if "--hold" in more:
        f = more[more.index("--hold") + 1]
        while not os.path.exists(f):
            time.sleep(0.05)


class Run:
    def __init__(self, sb, pkg):
        self.sb, self.pkg = sb, pkg

    def env(self, game, line):
        home = os.path.join(self.sb, "home")
        env = {"PATH": os.environ["PATH"], "HOME": home, "XDG_CACHE_HOME": os.path.join(home, ".cache"),
               "STEAM_COMPAT_INSTALL_PATH": os.path.join(self.sb, "games", game),
               "STEAM_COMPAT_DATA_PATH": os.path.join(self.sb, "compat"), "SteamGameId": "4242"}
        env.update(dict(o.split("=", 1) for o in line.split()))
        return env

    def cmd(self, game, *more):
        top = os.path.join(self.sb, "games", game)
        return [sys.executable, os.path.abspath(__file__), "--child", os.path.join(self.pkg, "tools", "proton-hook.py"),
                top, os.path.join(top, "b", "game.exe")] + list(more)

    def __call__(self, game, line, *more):
        """the hook's run for a launch line -> its outcome, its lines, and whether the game folder is as before"""
        top = os.path.join(self.sb, "games", game)
        before = state(top)
        r = subprocess.run(self.cmd(game, *more), env=self.env(game, line), capture_output=True, text=True)
        if not r.stdout.strip():
            return {"out": "ERROR " + r.stderr[-600:], "log": [], "restored": False}
        res = json.loads(r.stdout.strip().splitlines()[-1])
        res["restored"] = state(top) == before
        return res


def main():
    sb = tempfile.mkdtemp(prefix="xmx-hook-sandbox.")
    if os.environ.get("KEEP"):
        print("sandbox: " + sb)
    run = Run(sb, make(sb))
    checks = fails = 0

    def ok(what, cond, detail=""):
        nonlocal checks, fails
        checks += 1
        if not cond:
            fails += 1
            print("FAIL: %s%s" % (what, " (%s)" % detail if detail else ""))

    # ---- 1. the old launch lines
    old = {(g, l): want for g, outcomes in OLD.items() for want, lines in outcomes.items() for l in lines}
    for (game, line), want in old.items():
        r = run(game, line)
        ok("%s, old line [%s]" % (game, line), r["out"] == want, "[%s], before XESS=auto [%s]" % (r["out"], want))
        ok("%s, old line [%s]: the game folder as before" % (game, line), r["restored"])

    # ---- 2. the new forms: the links of the old line for that kind of game, and the reason
    for game, line, same, says in NEW:
        r = run(game, line)
        ok("%s [%s] makes the links of [%s]" % (game, line, same), r["out"].split(" | ")[0] == old[(game, same)].split(" | ")[0],
           "[%s], want [%s]" % (r["out"], old[(game, same)]))
        ok("%s [%s]: the game folder as before" % (game, line), r["restored"])
        if says:
            ok("%s [%s] says \"%s\"" % (game, line, says), any("XESS=auto: " in l and says in l for l in r["log"]),
               "; ".join(r["log"]))
    r = run("old-xess+fsr", "XESS=auto")
    ok("XESS=auto: the lines about the game's own XeSS name XESS=auto",
       any(l.startswith("xess-xmx: XESS=auto: b/libxess.dll 2.0.1.41 -> 2.0.2.68") for l in r["log"])
       and not any("XESS=latest" in l for l in r["log"]), "; ".join(r["log"]))
    r = run("old-xess+fsr", "XESS=off")
    ok("XESS=off is not a value: said so, nothing swapped", r["out"] == "" and any("unknown value" in l for l in r["log"]), r["out"])

    # ---- 3. the short names and the logs
    for value, cap in (("off", "off"), ("0", "off"), ("on", "auto"), ("auto", "auto"), ("auto:2", "auto:2"), ("auto:-1.5", "auto:-1.5")):
        r = run("neither", "XESS_LL=" + value)
        ok("XESS_LL=%s -> IGDEXT_XELL_CAP=%s" % (value, cap), r["out"] == " | IGDEXT_XELL_CAP=" + cap, r["out"])
    for value in ("5000", "auto:", "auto:fast", "low"):
        r = run("neither", "XESS_LL=" + value)
        ok("XESS_LL=%s: refused, nothing set" % value, r["out"] == "" and any("unknown value" in l for l in r["log"]), r["out"])
    r = run("neither", "XESS_LL=off IGDEXT_XELL_CAP=auto:3")
    ok("XESS_LL next to IGDEXT_XELL_CAP: the old name decides, and the hook says so",
       r["out"] == "" and any("decides" in l for l in r["log"]), r["out"])
    r = run("fsr-fg", "XESS=auto XESS_FG=3 XESS_LL=auto:2")
    ok("XESS_LL next to XESS=auto", r["out"] == old[("fsr-fg", "FSR=xess")] + " IGDEXT_XELL_CAP=auto:2", r["out"])
    layer_log = os.path.join(sb, "home", ".cache", "xess-xmx", "layer.log")
    r = run("neither", "XESS_LOG=2")
    ok("XESS_LOG=2: also Proton's log, the driver's lines, the layer's log as a file, vkwrap's and the D3D11 bridge's",
       r["out"] == " | LOGS ANV_CM_DEBUG=1 PROTON_LOG=1 VKWRAP_LOG=1 XESS_DX11_TRACE=1 XMX_LAYER_LOG=" + layer_log
       and os.path.isfile(layer_log), r["out"])
    r = run("neither", "XESS_LOG=2 PROTON_LOG=0")
    ok("XESS_LOG=2: a PROTON_LOG of the launch options stays", "PROTON_LOG" not in r["out"], r["out"])
    with open(layer_log, "w") as f:
        f.write("the lines of the run before\n")
    r = run("neither", "XESS_LOG=2 XMX_LAYER_LOG=1")
    ok("XESS_LOG=2: an XMX_LAYER_LOG of the launch options stays; the hook names it and leaves its own file alone",
       "XMX_LAYER_LOG" not in r["out"] and any("XMX_LAYER_LOG=1 puts it" in l for l in r["log"])
       and os.path.getsize(layer_log) > 0, r["out"] + " " + "; ".join(r["log"]))
    hooklog = os.path.join(sb, "home", ".cache", "xess-xmx", "hook-4242.log")
    text = open(hooklog).read() if os.path.isfile(hooklog) else ""
    ok("the hook's own log: a first line per run with its options, then the route and what became of the files",
       "| XESS=auto XESS_FG=3 XESS_LL=auto:2\n" in text and "XESS=auto: no XeSS frame generation in the game" in text
       and "the game's own files are back" in text, text[-400:])

    cache = os.path.join(sb, "home", ".cache", "xess-xmx")
    for line in ("XESS_LL=off", "XESS=auto_off XESS_FG=5"):
        run("neither", "SteamGameId=4444 " + line)
        text = open(os.path.join(cache, "hook-4444.log")).read() if os.path.isfile(os.path.join(cache, "hook-4444.log")) else ""
        ok("the hook's own log: a run with %s gets its first line, with nothing else to say" % line.split()[0],
           text.endswith("| %s\n" % line), text[-300:])
    run("neither", "SteamGameId=4445 XESS_FG=3")
    ok("the hook's own log: none for a run without an option of the hook", not os.path.exists(os.path.join(cache, "hook-4445.log")))
    run("neither", "SteamGameId= STEAM_COMPAT_APP_ID=4343 XESS_LOG=1")
    ok("the hook's own log: named after STEAM_COMPAT_APP_ID where Steam gives no other id (its install-script helper)",
       os.path.isfile(os.path.join(cache, "hook-4343.log")), " ".join(sorted(os.listdir(cache))))
    run("neither", "SteamGameId= XESS_LOG=1")
    ok("the hook's own log: named after the executable where there is no id at all",
       os.path.isfile(os.path.join(cache, "hook-game.exe.log")), " ".join(sorted(os.listdir(cache))))

    # ---- 4. a run that is killed outright leaves its links; the next start of any game puts the files back
    top = os.path.join(sb, "games", "old-xess+fsr")
    before = state(top)
    r = run("old-xess+fsr", "XESS=auto", "--die")
    ok("killed run: its links are left", not r["restored"] and r["out"].startswith("xess xefg xell dx11"), r["out"])
    r = run("neither", "")
    ok("killed run: the next start puts the files back and says so",
       state(top) == before and any("left by a run that was killed" in l for l in r["log"]), "; ".join(r["log"]))
    # a game's second run while its first is alive (a launcher, then the game): the libxess_fg.dll the first run
    # added next to the FidelityFX DLLs is not the game's own XeSS frame generation
    top = os.path.join(sb, "games", "fsr-fg")
    before, go = state(top), os.path.join(sb, "go")
    first = subprocess.Popen(run.cmd("fsr-fg", "--hold", go), env=run.env("fsr-fg", "XESS=auto"), stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, text=True)
    first.stdout.readline()   # it has swapped
    r = run("fsr-fg", "XESS=auto XESS_FG=3")
    open(go, "w").close()
    first.wait()
    ok("second run of a game: the first run's libxess_fg.dll is not taken for the game's own",
       any("FSR runs on XeSS" in l for l in r["log"]), "; ".join(r["log"]))
    ok("second run of a game: the game folder as before when both have ended", state(top) == before)

    if not os.environ.get("KEEP"):
        shutil.rmtree(sb, ignore_errors=True)
    print("hook-sandbox: all %d checks passed" % checks if not fails else
          "hook-sandbox: %d of %d checks FAILED (KEEP=1 keeps the sandbox)" % (fails, checks))
    return 1 if fails else 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--child":
        child()
    else:
        sys.exit(main())
