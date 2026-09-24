#!/bin/bash
# install.sh - system-wide install of the XeSS XMX package for the current user (SteamOS / any systemd user session):
#   1. compiler bundle (igc/) fetched if missing            -> run-time kernel compilation for any XeSS version
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
  } > "$ENVD/50-xess-xmx.conf"
  systemctl --user import-environment 2>/dev/null
  systemctl --user set-environment "VK_DRIVER_FILES=$drivers" "VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer" \
    "ANV_CM_KERNEL_DIR=$PKG/kernels" "ANV_CM_HELPER=$PKG/tools/cm-compile.sh" 2>/dev/null
  echo "== environment: $ENVD/50-xess-xmx.conf (applies to Steam after a reboot / re-login)"
  echo "   Vulkan drivers: $(echo "$drivers" | tr ':' ' ')"
fi
mkdir -p "$PKG/kernels" "${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/kernels"
# a failed run-time compile makes the shim fall back to DP4a in that prefix; with a compiler present, try again
if [ -x "$(ls "$PKG"/igc/neo/bin/ocloc* 2>/dev/null | head -1)" ]; then
  rm -f "$STEAM"/steamapps/compatdata/*/pfx/drive_c/igdext_kernels/compile_failed.txt
fi
if [ -d "$HOME/.var/app/com.valvesoftware.Steam" ]; then
  echo "!! Flatpak Steam found: its games see neither ~/.config/environment.d nor this folder (sandbox)."
  echo "   install.sh only covers the native Steam (SteamOS, distribution package)."
fi

# 3. shim
n=0
install_shim() { # <igdext64.dll path>
  local f=$1
  [ -f "$f" ] || return
  cmp -s "$f" "$SHIM" && return
  [ -f "$f.stock" ] || cp -p "$f" "$f.stock"
  chmod u+w "$f" 2>/dev/null
  cp "$SHIM" "$f" && n=$((n + 1)) && echo "   shim -> $f"
}
for f in "$STEAM"/steamapps/common/Proton*/files/lib/wine/igdext/x86_64-windows/igdext64.dll \
         "$STEAM"/compatibilitytools.d/*/files/lib/wine/igdext/x86_64-windows/igdext64.dll; do install_shim "$f"; done
for d in "$STEAM"/steamapps/compatdata/*/pfx/drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1; do
  [ -d "$d" ] || continue
  if [ -f "$d/igdext64.dll" ]; then install_shim "$d/igdext64.dll"; else cp "$SHIM" "$d/igdext64.dll" && n=$((n + 1)) && echo "   shim -> $d/igdext64.dll"; fi
done
echo "== shim: $n file(s) updated"
# started by the watcher: a new prefix gets its driver store folder a few seconds after compatdata/<id> appears, so keep
# looking for a while and put the shim in as soon as the folder exists
if [ "${XMX_WAIT_PREFIX:-0}" -gt 0 ]; then
  end=$(( $(date +%s) + XMX_WAIT_PREFIX ))
  while [ "$(date +%s)" -lt "$end" ]; do
    for d in "$STEAM"/steamapps/compatdata/*/pfx/drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1; do
      [ -d "$d" ] || continue
      if [ -f "$d/igdext64.dll" ]; then install_shim "$d/igdext64.dll"; else cp "$SHIM" "$d/igdext64.dll" && echo "   shim -> $d/igdext64.dll"; fi
    done
    sleep 3
  done
fi

# 4. watcher: re-install after a Proton update (bundled igdext64.dll replaced), a newly installed Proton, or a new prefix
#    (a game started for the first time)
mkdir -p "$UNITD"
watch=""
for p in "$STEAM"/steamapps/common/Proton*/files/lib/wine/igdext/x86_64-windows/igdext64.dll; do [ -f "$p" ] && watch="$watch
PathChanged=$p"; done
for p in "$STEAM/steamapps/compatdata" "$STEAM/steamapps/common" "$STEAM/compatibilitytools.d"; do [ -d "$p" ] && watch="$watch
PathModified=$p"; done
if [ -n "$watch" ]; then
  { echo "[Unit]"; echo "Description=Re-install the XeSS XMX shim after a Proton update or for a new prefix"; echo "[Path]";
    printf '%s\n' "${watch#
}"; echo "TriggerLimitIntervalSec=10"; echo "TriggerLimitBurst=20"; echo "[Install]"; echo "WantedBy=default.target"; } > "$UNITD/xess-xmx-shim.path"
  { echo "[Unit]"; echo "Description=XeSS XMX shim re-install"; echo "[Service]"; echo "Type=oneshot";
    echo "ExecStart=/bin/bash $PKG/install.sh"; echo "Environment=XMX_NO_ENV=1 XMX_WAIT_PREFIX=90"; } > "$UNITD/xess-xmx-shim.service"
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
    echo "ExecStart=/bin/bash $PKG/tools/session-check.sh"
    echo "[Install]"
    echo "WantedBy=graphical-session-pre.target default.target"
  } > "$UNITD/xess-xmx-check.service"
  systemctl --user daemon-reload 2>/dev/null
  systemctl --user enable xess-xmx-check.service >/dev/null 2>&1
  bash "$PKG/tools/session-check.sh" --dry-run >/dev/null 2>&1 && echo "== driver check: loads fine (xess-xmx-check.service enabled)" \
    || echo "!! the patched driver does not load on this system - see ~/.cache/xess-xmx/driver-status"
fi
echo "done. Reboot (or log out and in) once so Steam picks up the environment; then any game with XeSS runs on XMX."
