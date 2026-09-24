#!/bin/bash
# uninstall.sh - undo install.sh: remove the session environment file and the shim watcher, put every stock igdext64.dll
# back (Proton bundles and prefixes), and delete the run-time kernel folders inside the prefixes. The package folder,
# the compiler bundle and per-game enable.sh blocks are left alone (use disable.sh for those).
set -u
PKG="$(cd "$(dirname "$0")" && pwd)"
STEAM="${STEAM_DIR:-$HOME/.local/share/Steam}"
rm -f "$HOME/.config/environment.d/50-xess-xmx.conf"
systemctl --user unset-environment VK_DRIVER_FILES VKD3D_DISABLE_EXTENSIONS ANV_CM_KERNEL_DIR 2>/dev/null
systemctl --user disable --now xess-xmx-shim.path >/dev/null 2>&1
rm -f "$HOME/.config/systemd/user/xess-xmx-shim.path" "$HOME/.config/systemd/user/xess-xmx-shim.service"
systemctl --user daemon-reload 2>/dev/null
n=0
for f in "$STEAM"/steamapps/common/Proton*/files/lib/wine/igdext/x86_64-windows/igdext64.dll \
         "$STEAM"/compatibilitytools.d/*/files/lib/wine/igdext/x86_64-windows/igdext64.dll \
         "$STEAM"/steamapps/compatdata/*/pfx/drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1/igdext64.dll; do
  [ -f "$f" ] || continue
  if [ -f "$f.stock" ]; then mv -f "$f.stock" "$f" && n=$((n + 1)); elif cmp -s "$f" "$PKG/igdext64.dll"; then rm -f "$f" && n=$((n + 1)); fi
done
rm -rf "$STEAM"/steamapps/compatdata/*/pfx/drive_c/igdext_kernels
echo "restored $n igdext64.dll file(s); environment and watcher removed. Reboot or re-login to drop the variables from Steam."
