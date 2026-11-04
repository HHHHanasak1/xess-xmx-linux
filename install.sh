#!/bin/bash
# install.sh - system-wide install of the XeSS XMX package for the current user (SteamOS / any systemd user session):
#   1. compiler bundle (igc/) fetched if missing            -> run-time kernel compilation for any XeSS version;
#      Intel's XeSS SDK libraries (xess/) fetched if missing -> for games with XESS=latest in their launch options
#      AMD's FidelityFX loader + providers (ffx/) fetched if missing -> FSR=xess / FSR_FG=xess with FSR 3.1.0-3.1.3
#   2. ~/.config/environment.d/50-xess-xmx.conf               -> every process of the user session (Steam, gamescope, games)
#      gets the patched ANV as its Vulkan ICD and the kernel path; no per-game launch options needed any more
#   3. the shim igdext64.dll goes into every Proton that bundles its own igdext (Proton Experimental copies that file
#      into the prefix at each launch) and into every existing prefix that already has the igd_faux driver store
#   4. a user path unit re-installs the shim whenever a Proton update replaces its bundled copy
# Re-run after Steam installs a new Proton or after a game created a new prefix. uninstall.sh reverts everything.
# Environment: XMX_NO_ENV=1 skips step 2 and the session check (keep using enable.sh / launch options per game).
#   XMX_WAIT_PREFIX=<seconds> is how the watcher (step 4) runs this script: no downloads, the shim pass repeated cheaply
#   for that long (a new prefix gets its driver store folder a few seconds after it appears, Steam may still be
#   writing a Proton update), nothing else touched.
set -u
PKG="$(cd "$(dirname "$0")" && pwd)"
. "$PKG/tools/steam-libraries.sh"
STEAM="$(steam_root)"
LIBS=$(steam_libraries "$STEAM")   # every library root (SD card, second disk): Protons and prefixes live under each steamapps/
# Protons installed by a distribution package (CachyOS: proton-cachyos) - usually only root can write there
SYSTOOLS="/usr/share/steam/compatibilitytools.d /usr/local/share/steam/compatibilitytools.d"
ENVD="$HOME/.config/environment.d"
# the prefix folder that holds the shim (shim_pass below), as XeSS sees it
ALT_EXT='C:\windows\system32\driverstore\filerepository\igd_faux.inf_1'
UNITD="$HOME/.config/systemd/user"
SHIM="$PKG/igdext64.dll"
case "${XMX_WAIT_PREFIX:-0}" in 0|"") MANUAL=1 ;; *) MANUAL=0 ;; esac
for f in "$SHIM" "$PKG/lib/libvulkan_intel.so" "$PKG/tools/cm-compile.sh"; do
  [ -f "$f" ] || { echo "missing $f - run this from the unpacked package"; exit 1; }
done
mkdir -p "$PKG/run"
grep -q "\"library_path\": \"$PKG/lib/libvulkan_intel.so\"" "$PKG/run/intel_icd.json" 2>/dev/null || \
  xmx_icd_json "$PKG/lib/libvulkan_intel.so" > "$PKG/run/intel_icd.json"
# every igdext64.dll this package installed gets this file's modification time (an even whole second, so that any file
# system keeps it exactly): the watcher's poll then tells a file Steam wrote apart from ours without reading it
STAMP="$PKG/run/shim-stamp"
[ -f "$STAMP" ] || touch -d "@$(( $(date +%s) / 2 * 2 ))" "$STAMP"
# a manual run must not start a background one through the watcher (every shim copy is an event for it): the path
# unit is stopped while this runs and started again at the end (also when this run is interrupted)
if [ "$MANUAL" = 1 ] && systemctl --user stop xess-xmx-shim.path 2>/dev/null; then
  trap 'systemctl --user start xess-xmx-shim.path 2>/dev/null' EXIT
fi

# 1. compiler bundle (the watcher, which runs this script in the background, downloads nothing)
if [ "$MANUAL" = 1 ] && [ ! -x "$(ls "$PKG"/igc/neo/bin/ocloc* 2>/dev/null | head -1)" ]; then
  echo "== fetching the compiler bundle (IGC 2.10.10 + ocloc, ~95 MB)"
  bash "$PKG/tools/get-igc.sh" "$PKG" || { echo "compiler download failed - the run-time kernel path will be unavailable"; }
fi
# Intel's XeSS SDK libraries, used only by games whose launch options ask for them (XESS=latest, tools/proton-hook.py)
if [ "$MANUAL" = 1 ] && [ ! -f "$PKG/xess/libxess.dll" ]; then
  echo "== fetching Intel's XeSS SDK libraries (~77 MB, for XESS=latest)"
  bash "$PKG/tools/get-xess.sh" "$PKG" || echo "XeSS SDK download failed - XESS=latest will use the games' own libraries"
fi
# AMD's FidelityFX loader and providers: FSR=xess / FSR_FG=xess in games with FSR 3.1.0-3.1.3 (one amd_fidelityfx_dx12.dll)
if [ "$MANUAL" = 1 ] && { [ ! -f "$PKG/ffx/amd_fidelityfx_loader_dx12.dll" ] || [ ! -f "$PKG/ffx/amd_fidelityfx_framegeneration_dx12.dll" ]; }; then
  echo "== fetching AMD's FidelityFX loader, upscaler and frame generation (~87 MB, for FSR_FG / FSR with FSR 3.1.0-3.1.3)"
  bash "$PKG/tools/get-ffx.sh" "$PKG" || echo "FidelityFX download failed - FSR=xess / FSR_FG=xess work only with FSR 3.1.4 and newer"
fi
chmod +x "$PKG"/tools/*.sh "$PKG"/tools/*.py 2>/dev/null

# 2. session environment
if [ "${XMX_NO_ENV:-0}" != 1 ]; then
  # VK_DRIVER_FILES replaces the loader's driver list: every other GPU's driver stays in it (hybrid laptops, eGPUs),
  # only the stock 64-bit Intel ANV is left out so that a game cannot pick it instead of the patched one
  drivers=$(xmx_driver_files "$PKG/run/intel_icd.json")
  mkdir -p "$ENVD"
  {
    echo "# XeSS XMX (xess-xmx-linux): patched ANV as the Intel Vulkan driver + native CM kernels for every game of this session"
    echo "VK_DRIVER_FILES=$drivers"
    echo "# vkd3d-proton: the classic descriptor heap (the kernel binding code needs it) and present wait 1 (with present wait 2,"
    echo "# frame generation's high present rate crashed Cyberpunk 2077 in vkWaitForPresent2KHR within minutes)"
    echo "VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer,VK_KHR_present_wait2"
    echo "ANV_CM_KERNEL_DIR=$PKG/kernels"
    echo "ANV_CM_HELPER=$PKG/tools/cm-compile.sh"
    echo "# Mesa hides the Intel vendor id from some games (force_vk_vendor=-1 profiles), which keeps XeSS on DP4a there;"
    echo "# an option set in the environment overrides the profile. One game back to Mesa's default: force_vk_vendor=-1 %command%"
    echo "force_vk_vendor=0"
    echo "# Protons without Intel's driver store (Proton 11.0) never load the shim from the prefix; XeSS looks here first"
    echo "INTC_ALT_DRIVER_EXTENSIONS_PATH='$ALT_EXT'"
  } > "$ENVD/50-xess-xmx.conf"
  systemctl --user set-environment "VK_DRIVER_FILES=$drivers" \
    "VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer,VK_KHR_present_wait2" "ANV_CM_KERNEL_DIR=$PKG/kernels" \
    "ANV_CM_HELPER=$PKG/tools/cm-compile.sh" "force_vk_vendor=0" "INTC_ALT_DRIVER_EXTENSIONS_PATH=$ALT_EXT" 2>/dev/null
  echo "== environment: $ENVD/50-xess-xmx.conf (applies to Steam after a reboot / re-login)"
  echo "   Vulkan drivers: $(echo "$drivers" | tr ':' ' ')"
fi
mkdir -p "$PKG/kernels" "${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/kernels"
# a failed run-time compile makes the shim fall back to DP4a in that prefix; with a compiler present, a manual run
# tries again (not the watcher: it would retry a compile that keeps failing at every Proton update)
if [ "$MANUAL" = 1 ] && [ -x "$(ls "$PKG"/igc/neo/bin/ocloc* 2>/dev/null | head -1)" ]; then
  while IFS= read -r lib; do rm -f "$lib"/steamapps/compatdata/*/pfx/drive_c/igdext_kernels/compile_failed.txt; done <<< "$LIBS"
fi
if [ -d "$HOME/.var/app/com.valvesoftware.Steam" ]; then
  echo "!! Flatpak Steam found: its games see neither ~/.config/environment.d nor this folder (sandbox)."
  echo "   install.sh only covers the native Steam (SteamOS, distribution package)."
fi

# 3. shim
n=0
READONLY=""   # Protons this user cannot write to (a distribution package): reported at the end
changed() { # <igdext64.dll>: a file of the user's whose modification time is not the stamp's, i.e. one this package did
  # not write (Steam's copy, new or updated). Bash builtins only: the watcher's poll asks this every few seconds
  [ -f "$1" ] && [ -O "$1" ] && { [ "$1" -nt "$STAMP" ] || [ "$1" -ot "$STAMP" ]; }
}
put_shim() { # <igdext64.dll path>: the shim, stamped
  cp "$SHIM" "$1" || return
  touch -r "$STAMP" "$1" 2>/dev/null
  n=$((n + 1))
  echo "   shim -> $1"
}
install_shim() { # <igdext64.dll path>
  local f=$1
  [ -f "$f" ] || return
  if cmp -s "$f" "$SHIM"; then
    changed "$f" && touch -r "$STAMP" "$f" 2>/dev/null   # installed before the stamp existed
    return
  fi
  if [ ! -w "$f" ] && ! chmod u+w "$f" 2>/dev/null; then
    case "$READONLY" in *"$f"*) ;; *) READONLY="$READONLY
$f";; esac
    return
  fi
  # keep the original as .stock. Every build of the shim names its IGDEXT_* switches; a file without them is a stock one,
  # also a newer one that a Proton update just brought (then the old .stock is out of date and is replaced)
  if ! grep -aq 'IGDEXT_' "$f"; then
    if [ ! -f "$f.stock" ] || ! cmp -s "$f" "$f.stock"; then
      chmod u+w "$f.stock" 2>/dev/null
      cp -p "$f" "$f.stock"
    fi
  fi
  chmod u+w "$f" 2>/dev/null
  put_shim "$f"
}
install_hook() { # <Proton folder>: user_settings.py that runs tools/proton-hook.py (XESS=latest, FSR=xess, FSR_FG=xess)
  local p=$1 u="$1/user_settings.py"
  [ -f "$p/proton" ] && [ -w "$p" ] || return
  # a user_settings.py of the user's own (Proton's debug settings) keeps working: it becomes user_settings.user.py
  if [ -f "$u" ] && ! grep -q 'xess-xmx proton hook' "$u"; then mv -f "$u" "$p/user_settings.user.py"; fi
  cat > "$u.new" <<EOF
# xess-xmx proton hook - written by $PKG/install.sh, removed by uninstall.sh. Runs tools/proton-hook.py at every game
# start (XESS=latest, FSR=xess, FSR_FG=xess in a game's launch options). Your own Proton settings go into user_settings.user.py.
import os as _os
user_settings = {}
try:
    _h = "$PKG/tools/proton-hook.py"
    if _os.path.isfile(_h):
        exec(compile(open(_h).read(), _h, "exec"), {"__file__": _h, "__name__": "xess_xmx_hook", "SETTINGS": user_settings})
except Exception as _e:
    import sys as _sys
    _sys.stderr.write("xess-xmx: proton hook: %s\n" % _e)
_u = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "user_settings.user.py")
if _os.path.isfile(_u):
    _g = {"__file__": _u}
    exec(compile(open(_u).read(), _u, "exec"), _g)
    user_settings.update(_g.get("user_settings", {}))
EOF
  if cmp -s "$u.new" "$u"; then rm -f "$u.new"; else mv -f "$u.new" "$u" && echo "   XESS hook -> $u"; fi
}
install_controllib() { # <prefix system32>: ControlLib.dll, the stand-in for Intel's Control Library (XESS_FG)
  local s="$1/ControlLib.dll"
  [ -f "$PKG/ControlLib.dll" ] || return
  [ -f "$s" ] && ! grep -aq 'XESS_FG' "$s" && return   # a ControlLib.dll that is not ours stays
  cmp -s "$PKG/ControlLib.dll" "$s" && return
  cp "$PKG/ControlLib.dll" "$s" && n=$((n + 1)) && echo "   ControlLib.dll -> $s"
}
shim_pass() { # every Proton that bundles the library, and every prefix that has the driver store folder.
  # With "poll": the same places, but each file is read (cmp) or written only when a bash test says it must be - the
  # watcher's loop below asks this every few seconds, and a full pass forks several times per Proton and prefix
  local poll=${1:-} f d lib s
  # Protons: Valve's in every library, the user's own (compatibilitytools.d), a distribution's (SYSTOOLS). The hook goes
  # into every Proton, also one without its own igdext (Proton 11.0): the launch options work the same there
  while IFS= read -r lib; do
    for f in "$lib"/steamapps/common/Proton*/files/lib/wine/igdext/x86_64-windows/igdext64.dll; do
      [ -z "$poll" ] || changed "$f" && install_shim "$f"
    done
    for d in "$lib"/steamapps/common/Proton*/; do
      [ -z "$poll" ] || [ ! -f "$d/user_settings.py" ] && install_hook "${d%/}"   # a new Proton (hook not yet there)
    done
  done <<< "$LIBS"
  for s in "$STEAM/compatibilitytools.d" $SYSTOOLS; do
    for f in "$s"/*/files/lib/wine/igdext/x86_64-windows/igdext64.dll; do
      [ -z "$poll" ] || changed "$f" && install_shim "$f"
    done
    for d in "$s"/*/; do
      [ -z "$poll" ] || [ ! -f "$d/user_settings.py" ] && install_hook "${d%/}"
    done
  done
  # prefixes in every library (a new one has the folder a few seconds after compatdata/<id> appears, without the
  # file; Proton Experimental copies its own igdext64.dll over ours at each launch). Any case: Wine finds a file
  # case-insensitively, and a prefix can hold both "driverstore" and "DriverStore" (proton-cachyos makes the latter,
  # with its own igdext64.dll); the shim goes into each, whichever Wine picks. The brackets make bash glob those
  # components, nocaseglob applies only to globbed ones
  shopt -s nocaseglob
  while IFS= read -r lib; do
    for d in "$lib"/steamapps/compatdata/*/pfx/drive_c/windows/system32/[d]riverstore/[f]ilerepository/[i]gd_faux.inf_1; do
      [ -d "$d" ] || continue
      if [ ! -f "$d/igdext64.dll" ]; then put_shim "$d/igdext64.dll"
      elif [ -z "$poll" ] || changed "$d/igdext64.dll"; then install_shim "$d/igdext64.dll"; fi
      s=${d%/*/*/*}
      [ -z "$poll" ] || { [ -f "$PKG/ControlLib.dll" ] && [ ! -f "$s/ControlLib.dll" ]; } && install_controllib "$s"
    done
  done <<< "$LIBS"
  shopt -u nocaseglob
}
shim_pass
echo "== shim: $n file(s) updated"
if [ -n "$READONLY" ] && [ "$MANUAL" = 1 ]; then
  echo "!! These Protons belong to a distribution package and only root can change them; games that run on them get"
  echo "   no XMX (Proton copies its own igdext64.dll into the prefix). Either use a Proton in"
  echo "   $STEAM/compatibilitytools.d (CachyOS offers its builds there too), or put the shim in as root"
  echo "   (again after every update of that package):"
  printf '%s\n' "${READONLY#?}" | while IFS= read -r f; do echo "     sudo cp '$f' '$f.stock' && sudo cp '$SHIM' '$f'"; done
fi
# started by the watcher: keep looking for a while. A new prefix gets its driver store folder a few seconds after
# compatdata/<id> appears, and Steam may write a Proton update's files after the event that started this run - the path
# unit does not report changes while its service is running, so they have to be picked up here. The poll costs no
# forks; there is no telling when Steam is done, so the whole time is waited
if [ "$MANUAL" = 0 ]; then
  end=$(( SECONDS + XMX_WAIT_PREFIX ))
  while [ "$SECONDS" -lt "$end" ]; do
    sleep 3
    shim_pass poll
  done
fi

# 4. watcher: re-install after a Proton update (bundled igdext64.dll replaced), a newly installed Proton, or a new prefix
#    (a game started for the first time)
mkdir -p "$UNITD"
unit_changed=0
put_unit() { # <unit file>: <unit file>.new becomes the file, unless the file already has that content (then systemd need not reload)
  if cmp -s "$1.new" "$1"; then rm -f "$1.new"; else mv -f "$1.new" "$1" && unit_changed=1; fi
}
watch=""
while IFS= read -r lib; do
  for p in "$lib"/steamapps/common/Proton*/files/lib/wine/igdext/x86_64-windows/igdext64.dll; do [ -f "$p" ] && watch="$watch
PathChanged=$p"; done
  for p in "$lib/steamapps/compatdata" "$lib/steamapps/common"; do [ -d "$p" ] && watch="$watch
PathModified=$p"; done
done <<< "$LIBS"
[ -d "$STEAM/compatibilitytools.d" ] && watch="$watch
PathModified=$STEAM/compatibilitytools.d"
if [ -n "$watch" ]; then
  { echo "[Unit]"; echo "Description=Re-install the XeSS XMX shim after a Proton update or for a new prefix"; echo "[Path]";
    printf '%s\n' "${watch#
}"; echo "TriggerLimitIntervalSec=10"; echo "TriggerLimitBurst=20"; echo "[Install]"; echo "WantedBy=default.target"; } > "$UNITD/xess-xmx-shim.path.new"
  put_unit "$UNITD/xess-xmx-shim.path"
  { echo "[Unit]"; echo "Description=XeSS XMX shim re-install"; echo "[Service]"; echo "Type=oneshot";
    echo "ExecStart=/bin/bash \"$PKG/install.sh\""; echo "Environment=XMX_NO_ENV=1 XMX_WAIT_PREFIX=90"; } > "$UNITD/xess-xmx-shim.service.new"
  put_unit "$UNITD/xess-xmx-shim.service"
  # the watcher itself leaves systemd alone unless a unit changed (a new library or Proton to watch)
  if [ "$unit_changed" = 1 ] || [ "$MANUAL" = 1 ]; then
    [ "$unit_changed" = 1 ] && systemctl --user daemon-reload 2>/dev/null
    systemctl --user enable xess-xmx-shim.path >/dev/null 2>&1
    systemctl --user restart xess-xmx-shim.path >/dev/null 2>&1 && echo "== watcher: xess-xmx-shim.path active"
  fi
fi
# 5. safety net: before each graphical session, check that the patched driver still loads (a system update could break
#    its library dependencies); if not, the session falls back to the stock driver instead of losing the Intel GPU
if [ "${XMX_NO_ENV:-0}" != 1 ]; then
  mkdir -p "$UNITD"
  unit_changed=0
  {
    echo "[Unit]"
    echo "Description=Check that the XeSS XMX Vulkan driver loads (falls back to the stock driver if not)"
    echo "Before=graphical-session-pre.target gamescope-session.service gamescope-session-plus@steam.service steam-launcher.service plasma-workspace.target"
    echo "[Service]"
    echo "Type=oneshot"
    echo "RemainAfterExit=yes"
    echo "ExecStart=/bin/bash \"$PKG/tools/session-check.sh\""
    echo "[Install]"
    echo "WantedBy=graphical-session-pre.target default.target"
  } > "$UNITD/xess-xmx-check.service.new"
  put_unit "$UNITD/xess-xmx-check.service"
  [ "$unit_changed" = 1 ] && systemctl --user daemon-reload 2>/dev/null
  systemctl --user enable xess-xmx-check.service >/dev/null 2>&1
  bash "$PKG/tools/session-check.sh" --dry-run >/dev/null 2>&1 && echo "== driver check: loads fine (xess-xmx-check.service enabled)" \
    || echo "!! the patched driver does not load on this system - see ~/.cache/xess-xmx/driver-status"
fi
echo "done. Reboot (or log out and in) once so Steam picks up the environment; then any game with XeSS runs on XMX."
