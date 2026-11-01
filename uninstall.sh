#!/bin/bash
# uninstall.sh - undo install.sh: remove the session environment file and the shim watcher, put every stock igdext64.dll
# back (Proton bundles and prefixes), and delete the run-time kernel folders inside the prefixes. The package folder,
# the compiler bundle and per-game enable.sh blocks are left alone (use disable.sh for those).
set -u
PKG="$(cd "$(dirname "$0")" && pwd)"
STEAM="${STEAM_DIR:-$HOME/.local/share/Steam}"
rm -f "$HOME/.config/environment.d/50-xess-xmx.conf"
systemctl --user unset-environment VK_DRIVER_FILES VKD3D_DISABLE_EXTENSIONS ANV_CM_KERNEL_DIR ANV_CM_HELPER force_vk_vendor 2>/dev/null
systemctl --user disable --now xess-xmx-shim.path >/dev/null 2>&1
systemctl --user disable xess-xmx-check.service >/dev/null 2>&1
rm -f "$HOME/.config/systemd/user/xess-xmx-shim.path" "$HOME/.config/systemd/user/xess-xmx-shim.service" "$HOME/.config/systemd/user/xess-xmx-check.service"
systemctl --user daemon-reload 2>/dev/null
. "$PKG/tools/steam-libraries.sh"
LIBS=$(steam_libraries "$STEAM")   # every library root (SD card, second disk)
SYSTOOLS="/usr/share/steam/compatibilitytools.d /usr/local/share/steam/compatibilitytools.d"
n=0
restore_shim() { # <igdext64.dll path>
  local f=$1
  [ -f "$f" ] && [ -w "$f" ] || return
  if [ -f "$f.stock" ]; then mv -f "$f.stock" "$f" && n=$((n + 1)); elif cmp -s "$f" "$PKG/igdext64.dll"; then rm -f "$f" && n=$((n + 1)); fi
}
remove_hook() { # <Proton folder>: the XESS hook; the user's own Proton settings back under their name
  local u="$1/user_settings.py"
  [ -f "$u" ] && [ -w "$1" ] && grep -q 'xess-xmx proton hook' "$u" || return
  rm -f "$u"
  [ -f "${u%.py}.user.py" ] && mv -f "${u%.py}.user.py" "$u"
}
for s in "$STEAM/compatibilitytools.d" $SYSTOOLS; do
  for f in "$s"/*/files/lib/wine/igdext/x86_64-windows/igdext64.dll; do restore_shim "$f"; done
  for d in "$s"/*/; do remove_hook "${d%/}"; done
done
while IFS= read -r lib; do
  for f in "$lib"/steamapps/common/Proton*/files/lib/wine/igdext/x86_64-windows/igdext64.dll \
           "$lib"/steamapps/compatdata/*/pfx/drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1/igdext64.dll; do
    restore_shim "$f"
  done
  for d in "$lib"/steamapps/common/Proton*/; do remove_hook "${d%/}"; done
  rm -rf "$lib"/steamapps/compatdata/*/pfx/drive_c/igdext_kernels
  for c in "$lib"/steamapps/compatdata/*/pfx/drive_c/windows/system32/ControlLib.dll; do
    [ -f "$c" ] && grep -aq 'XESS_FG' "$c" && rm -f "$c"
  done
  # files a killed XESS=latest / FSR_FG=xess run left as links into the package (tools/proton-hook.py): originals back,
  # each in one rename over the link; links to files the game never had go
  while IFS= read -r -d '' f; do
    if [ -f "$f.xmx-orig" ]; then mv -f "$f.xmx-orig" "$f" && echo "   restored $f"; else rm -f "$f" && echo "   removed $f"; fi
  done < <(find "$lib"/steamapps/common -maxdepth 11 -type l \( -lname "$PKG/xess/*" -o -lname "$PKG/ffxfg/*" \) -print0 2>/dev/null)
  # games switched with tools/fsr-fg.sh on
  for d in "$lib"/steamapps/common/*/; do
    find "$d" -maxdepth 11 \( -name '*.ffxfg-orig' -o -name .ffxfg-added \) 2>/dev/null | grep -q . && bash "$PKG/tools/fsr-fg.sh" off "$d"
  done
done <<< "$LIBS"
rm -rf "${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/xess-swap"
echo "restored $n igdext64.dll file(s); environment and watcher removed. Reboot or re-login to drop the variables from Steam."
echo "Compiled kernels stay in ${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx (delete that folder to remove them)."
