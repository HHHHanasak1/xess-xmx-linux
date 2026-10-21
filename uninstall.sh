#!/bin/bash
# uninstall.sh - undo install.sh: remove the session environment file and the shim watcher, put every stock igdext64.dll
# back (Proton bundles and prefixes), and delete the run-time kernel folders inside the prefixes. The package folder,
# the compiler bundle and per-game enable.sh blocks are left alone (use disable.sh for those).
set -u
PKG="$(cd "$(dirname "$0")" && pwd)"
STEAM="${STEAM_DIR:-$HOME/.local/share/Steam}"
. "$PKG/tools/steam-libraries.sh"
LIBS=$(steam_libraries "$STEAM")
rm -f "$HOME/.config/environment.d/50-xess-xmx.conf"
systemctl --user unset-environment VK_DRIVER_FILES VKD3D_DISABLE_EXTENSIONS ANV_CM_KERNEL_DIR ANV_CM_HELPER force_vk_vendor 2>/dev/null
systemctl --user disable --now xess-xmx-shim.path >/dev/null 2>&1
systemctl --user disable xess-xmx-check.service >/dev/null 2>&1
rm -f "$HOME/.config/systemd/user/xess-xmx-shim.path" "$HOME/.config/systemd/user/xess-xmx-shim.service" "$HOME/.config/systemd/user/xess-xmx-check.service"
systemctl --user daemon-reload 2>/dev/null
n=0
restore() { [ -f "$1" ] || return 0; if [ -f "$1.stock" ]; then mv -f "$1.stock" "$1" && n=$((n + 1)); elif cmp -s "$1" "$PKG/igdext64.dll"; then rm -f "$1" && n=$((n + 1)); fi; }
for f in "$STEAM"/steamapps/common/Proton*/files/lib/wine/igdext/x86_64-windows/igdext64.dll \
         "$STEAM"/compatibilitytools.d/*/files/lib/wine/igdext/x86_64-windows/igdext64.dll; do restore "$f"; done
while IFS= read -r lib; do
  for f in "$lib"/steamapps/compatdata/*/pfx/drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1/igdext64.dll; do restore "$f"; done
  rm -rf "$lib"/steamapps/compatdata/*/pfx/drive_c/igdext_kernels
done <<< "$LIBS"
echo "restored $n igdext64.dll file(s); environment and watcher removed. Reboot or re-login to drop the variables from Steam."
echo "Compiled kernels stay in ${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx (delete that folder to remove them)."
