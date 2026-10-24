#!/bin/bash
# install.sh - system-wide install of the XeSS XMX package for the current user (SteamOS / any systemd user session):
#   1. compiler bundle (igc/) fetched if missing            -> run-time kernel compilation for any XeSS version;
#      Intel's XeSS SDK libraries (xess/) fetched if missing -> for games with XESS=latest in their launch options
#   2. ~/.config/environment.d/50-xess-xmx.conf               -> every process of the user session (Steam, gamescope, games)
#      gets the patched ANV as its Vulkan ICD and the kernel path; no per-game launch options needed any more
#   3. the shim igdext64.dll goes into every Proton that bundles its own igdext (Proton Experimental copies that file
#      into the prefix at each launch) and into every existing prefix that already has the igd_faux driver store
#   4. a user path unit re-installs the shim whenever a Proton update replaces its bundled copy
# Re-run after Steam installs a new Proton or after a game created a new prefix. uninstall.sh reverts everything.
# Environment: XMX_NO_ENV=1 skips step 2 (keep using enable.sh / launch options per game).
set -u
PKG="$(cd "$(dirname "$0")" && pwd)"
STEAM="${STEAM_DIR:-$HOME/.local/share/Steam}"
. "$PKG/tools/steam-libraries.sh"
LIBS=$(steam_libraries "$STEAM")   # every library root (SD card, second disk): Protons and prefixes live under each steamapps/
# Protons installed by a distribution package (CachyOS: proton-cachyos) - usually only root can write there
SYSTOOLS="/usr/share/steam/compatibilitytools.d /usr/local/share/steam/compatibilitytools.d"
ENVD="$HOME/.config/environment.d"
UNITD="$HOME/.config/systemd/user"
SHIM="$PKG/igdext64.dll"
for f in "$SHIM" "$PKG/lib/libvulkan_intel.so" "$PKG/tools/cm-compile.sh"; do
  [ -f "$f" ] || { echo "missing $f - run this from the unpacked package"; exit 1; }
done
mkdir -p "$PKG/run"
grep -q "\"library_path\": \"$PKG/lib/libvulkan_intel.so\"" "$PKG/run/intel_icd.json" 2>/dev/null || \
  printf '{ "ICD": { "api_version": "1.4.348", "library_arch": "64", "library_path": "%s/lib/libvulkan_intel.so" }, "file_format_version": "1.0.1" }\n' "$PKG" > "$PKG/run/intel_icd.json"

# 1. compiler bundle
if [ ! -x "$(ls "$PKG"/igc/neo/bin/ocloc* 2>/dev/null | head -1)" ]; then
  echo "== fetching the compiler bundle (IGC 2.10.10 + ocloc, ~230 MB)"
  bash "$PKG/tools/get-igc.sh" "$PKG" || { echo "compiler download failed - the run-time kernel path will be unavailable"; }
fi
# Intel's XeSS SDK libraries, used only by games whose launch options ask for them (XESS=latest, tools/proton-hook.py)
if [ ! -f "$PKG/xess/libxess.dll" ]; then
  echo "== fetching Intel's XeSS SDK libraries (~77 MB, for XESS=latest)"
  bash "$PKG/tools/get-xess.sh" "$PKG" || echo "XeSS SDK download failed - XESS=latest will use the games' own libraries"
fi
chmod +x "$PKG"/tools/*.sh "$PKG"/tools/*.py 2>/dev/null

# 2. session environment
if [ "${XMX_NO_ENV:-0}" != 1 ]; then
  # VK_DRIVER_FILES replaces the loader's driver list: every other GPU's driver stays in it (hybrid laptops, eGPUs),
  # only the stock 64-bit Intel ANV is left out so that a game cannot pick it instead of the patched one
  drivers="$PKG/run/intel_icd.json"
  for d in /usr/share/vulkan/icd.d /usr/local/share/vulkan/icd.d /etc/vulkan/icd.d "${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/icd.d"; do
    for j in "$d"/*.json; do
      [ -f "$j" ] || continue
      case "$(basename "$j")" in intel_icd.x86_64.json|intel_icd.json) continue ;; esac   # 32-bit Intel ICD stays (32-bit games)
      drivers="$drivers:$j"
    done
  done
  mkdir -p "$ENVD"
  {
    echo "# XeSS XMX (xess-xmx-linux): patched ANV as the Intel Vulkan driver + native CM kernels for every game of this session"
    echo "VK_DRIVER_FILES=$drivers"
    echo "VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer"
    echo "ANV_CM_KERNEL_DIR=$PKG/kernels"
    echo "ANV_CM_HELPER=$PKG/tools/cm-compile.sh"
    echo "# Mesa hides the Intel vendor id from some games (force_vk_vendor=-1 profiles), which keeps XeSS on DP4a there;"
    echo "# an option set in the environment overrides the profile. One game back to Mesa's default: force_vk_vendor=-1 %command%"
    echo "force_vk_vendor=0"
  } > "$ENVD/50-xess-xmx.conf"
  systemctl --user import-environment 2>/dev/null
  systemctl --user set-environment "VK_DRIVER_FILES=$drivers" "VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer" \
    "ANV_CM_KERNEL_DIR=$PKG/kernels" "ANV_CM_HELPER=$PKG/tools/cm-compile.sh" "force_vk_vendor=0" 2>/dev/null
  echo "== environment: $ENVD/50-xess-xmx.conf (applies to Steam after a reboot / re-login)"
  echo "   Vulkan drivers: $(echo "$drivers" | tr ':' ' ')"
fi
mkdir -p "$PKG/kernels" "${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/kernels"
# a failed run-time compile makes the shim fall back to DP4a in that prefix; with a compiler present, try again
if [ -x "$(ls "$PKG"/igc/neo/bin/ocloc* 2>/dev/null | head -1)" ]; then
  while IFS= read -r lib; do rm -f "$lib"/steamapps/compatdata/*/pfx/drive_c/igdext_kernels/compile_failed.txt; done <<< "$LIBS"
fi
if [ -d "$HOME/.var/app/com.valvesoftware.Steam" ]; then
  echo "!! Flatpak Steam found: its games see neither ~/.config/environment.d nor this folder (sandbox)."
  echo "   install.sh only covers the native Steam (SteamOS, distribution package)."
fi

# 3. shim
n=0
READONLY=""   # Protons this user cannot write to (a distribution package): reported at the end
install_shim() { # <igdext64.dll path>
  local f=$1
  [ -f "$f" ] || return
  cmp -s "$f" "$SHIM" && return
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
  cp "$SHIM" "$f" && n=$((n + 1)) && echo "   shim -> $f"
}
install_hook() { # <Proton folder>: user_settings.py that runs tools/proton-hook.py (XESS=latest in launch options)
  local p=$1 u="$1/user_settings.py"
  [ -f "$p/proton" ] && [ -w "$p" ] || return
  # a user_settings.py of the user's own (Proton's debug settings) keeps working: it becomes user_settings.user.py
  if [ -f "$u" ] && ! grep -q 'xess-xmx proton hook' "$u"; then mv -f "$u" "$p/user_settings.user.py"; fi
  cat > "$u.new" <<EOF
# xess-xmx proton hook - written by $PKG/install.sh, removed by uninstall.sh. Runs tools/proton-hook.py at every game
# start (XESS=latest in a game's launch options). Your own Proton settings go into user_settings.user.py next to it.
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
shim_pass() { # every Proton that bundles the library, and every prefix that has the driver store folder
  local f d lib s
  # Protons: Valve's in every library, the user's own (compatibilitytools.d), a distribution's (SYSTOOLS)
  while IFS= read -r lib; do
    for f in "$lib"/steamapps/common/Proton*/files/lib/wine/igdext/x86_64-windows/igdext64.dll; do
      install_shim "$f"
      [ -f "$f" ] && install_hook "${f%/files/lib/wine/igdext/x86_64-windows/igdext64.dll}"
    done
  done <<< "$LIBS"
  for s in "$STEAM/compatibilitytools.d" $SYSTOOLS; do
    for f in "$s"/*/files/lib/wine/igdext/x86_64-windows/igdext64.dll; do
      install_shim "$f"
      [ -f "$f" ] && install_hook "${f%/files/lib/wine/igdext/x86_64-windows/igdext64.dll}"
    done
  done
  # prefixes in every library
  while IFS= read -r lib; do
    for d in "$lib"/steamapps/compatdata/*/pfx/drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1; do
      [ -d "$d" ] || continue
      if [ -f "$d/igdext64.dll" ]; then install_shim "$d/igdext64.dll"; else cp "$SHIM" "$d/igdext64.dll" && n=$((n + 1)) && echo "   shim -> $d/igdext64.dll"; fi
      install_controllib "${d%/driverstore/filerepository/igd_faux.inf_1}"
    done
  done <<< "$LIBS"
}
shim_pass
echo "== shim: $n file(s) updated"
if [ -n "$READONLY" ] && [ "${XMX_WAIT_PREFIX:-0}" = 0 ]; then
  echo "!! These Protons belong to a distribution package and only root can change them; games that run on them get"
  echo "   no XMX (Proton copies its own igdext64.dll into the prefix). Either use a Proton in"
  echo "   ~/.local/share/Steam/compatibilitytools.d (CachyOS offers its builds there too), or put the shim in as root"
  echo "   (again after every update of that package):"
  printf '%s\n' "${READONLY#?}" | while IFS= read -r f; do echo "     sudo cp '$f' '$f.stock' && sudo cp '$SHIM' '$f'"; done
fi
# started by the watcher: keep looking for a while. A new prefix gets its driver store folder a few seconds after
# compatdata/<id> appears, and Steam may write a Proton update's files after the event that started this run - the path
# unit does not report changes while its service is running, so they have to be picked up here.
if [ "${XMX_WAIT_PREFIX:-0}" -gt 0 ]; then
  end=$(( $(date +%s) + XMX_WAIT_PREFIX ))
  while [ "$(date +%s)" -lt "$end" ]; do
    sleep 3
    shim_pass
  done
fi

# 4. watcher: re-install after a Proton update (bundled igdext64.dll replaced), a newly installed Proton, or a new prefix
#    (a game started for the first time)
mkdir -p "$UNITD"
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
}"; echo "TriggerLimitIntervalSec=10"; echo "TriggerLimitBurst=20"; echo "[Install]"; echo "WantedBy=default.target"; } > "$UNITD/xess-xmx-shim.path"
  { echo "[Unit]"; echo "Description=XeSS XMX shim re-install"; echo "[Service]"; echo "Type=oneshot";
    echo "ExecStart=/bin/bash \"$PKG/install.sh\""; echo "Environment=XMX_NO_ENV=1 XMX_WAIT_PREFIX=90"; } > "$UNITD/xess-xmx-shim.service"
  systemctl --user daemon-reload 2>/dev/null
  systemctl --user enable xess-xmx-shim.path >/dev/null 2>&1
  systemctl --user restart xess-xmx-shim.path >/dev/null 2>&1 && echo "== watcher: xess-xmx-shim.path active"
fi
# 5. safety net: before each graphical session, check that the patched driver still loads (a system update could break
#    its library dependencies); if not, the session falls back to the stock driver instead of losing the Intel GPU
if [ "${XMX_NO_ENV:-0}" != 1 ]; then
  mkdir -p "$UNITD"
  {
    echo "[Unit]"
    echo "Description=Check that the XeSS XMX Vulkan driver loads (falls back to the stock driver if not)"
    echo "Before=graphical-session-pre.target gamescope-session.service steam-launcher.service plasma-workspace.target"
    echo "[Service]"
    echo "Type=oneshot"
    echo "RemainAfterExit=yes"
    echo "ExecStart=/bin/bash \"$PKG/tools/session-check.sh\""
    echo "[Install]"
    echo "WantedBy=graphical-session-pre.target default.target"
  } > "$UNITD/xess-xmx-check.service"
  systemctl --user daemon-reload 2>/dev/null
  systemctl --user enable xess-xmx-check.service >/dev/null 2>&1
  bash "$PKG/tools/session-check.sh" --dry-run >/dev/null 2>&1 && echo "== driver check: loads fine (xess-xmx-check.service enabled)" \
    || echo "!! the patched driver does not load on this system - see ~/.cache/xess-xmx/driver-status"
fi
echo "done. Reboot (or log out and in) once so Steam picks up the environment; then any game with XeSS runs on XMX."
