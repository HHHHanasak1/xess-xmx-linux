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
n=0
for f in "$STEAM"/steamapps/common/Proton*/files/lib/wine/igdext/x86_64-windows/igdext64.dll \
         "$STEAM"/compatibilitytools.d/*/files/lib/wine/igdext/x86_64-windows/igdext64.dll \
         "$STEAM"/steamapps/compatdata/*/pfx/drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1/igdext64.dll; do
  [ -f "$f" ] || continue
  if [ -f "$f.stock" ]; then mv -f "$f.stock" "$f" && n=$((n + 1)); elif cmp -s "$f" "$PKG/igdext64.dll"; then rm -f "$f" && n=$((n + 1)); fi
done
rm -rf "$STEAM"/steamapps/compatdata/*/pfx/drive_c/igdext_kernels
# the XESS=latest hook in every Proton; the user's own Proton settings back under their name
for u in "$STEAM"/steamapps/common/Proton*/user_settings.py "$STEAM"/compatibilitytools.d/*/user_settings.py; do
  [ -f "$u" ] && grep -q 'xess-xmx proton hook' "$u" || continue
  rm -f "$u"
  [ -f "${u%.py}.user.py" ] && mv -f "${u%.py}.user.py" "$u"
done
rm -f "${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/xess-swapped"
# XeSS libraries a killed XESS=latest run left as links into the package (tools/proton-hook.py): originals back
while IFS= read -r -d '' o; do
  f="${o%.xmx-orig}"
  if [ -L "$f" ] && [ "$(readlink "$f")" = "$PKG/xess/$(basename "$f" | tr '[:upper:]' '[:lower:]')" ]; then
    rm -f "$f" && mv -f "$o" "$f" && echo "   restored $f"
  fi
done < <(find "$STEAM"/steamapps/common -maxdepth 11 -name '*.xmx-orig' -print0 2>/dev/null)
echo "restored $n igdext64.dll file(s); environment and watcher removed. Reboot or re-login to drop the variables from Steam."
echo "Compiled kernels stay in ${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx (delete that folder to remove them)."
