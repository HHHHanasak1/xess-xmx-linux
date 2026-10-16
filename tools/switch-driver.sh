#!/bin/bash
# switch-driver.sh [version] - choose which of the package's patched drivers the games use.
# The package keeps one driver per Mesa version in lib/mesa-<version>/libvulkan_intel.so; the active one is copied to
# lib/libvulkan_intel.so (the file the Vulkan driver list points at) and its name is kept in lib/active.
#   tools/switch-driver.sh          list the drivers, mark the active one
#   tools/switch-driver.sh 26.3     switch (any unique prefix of the version: 26.3, 26.1.2, ...)
# The new driver is checked like the session check does before it is activated. Games that are running keep the
# driver they loaded; the next game started uses the new one. No re-login needed.
set -u
PKG="$(cd "$(dirname "$0")/.." && pwd)"
LIB="$PKG/lib"
active=$(cat "$LIB/active" 2>/dev/null)

if [ -z "${1:-}" ]; then
  for d in "$LIB"/mesa-*/; do
    [ -f "$d/libvulkan_intel.so" ] || continue
    v=$(basename "$d"); v=${v#mesa-}
    [ "$v" = "$active" ] && echo "* $v (active)" || echo "  $v"
  done
  [ -n "$active" ] || echo "(no lib/active: lib/libvulkan_intel.so is a driver of an older package or a rebuild)"
  exit 0
fi

match=""
[ -f "$LIB/mesa-$1/libvulkan_intel.so" ] && match=$1   # an exact version wins (26.3.0 next to 26.3.0-rc1)
[ -n "$match" ] || for d in "$LIB"/mesa-"$1"*/; do
  [ -f "$d/libvulkan_intel.so" ] || continue
  [ -n "$match" ] && { echo "'$1' matches more than one driver; give more of the version"; exit 1; }
  match=$(basename "$d"); match=${match#mesa-}
done
[ -n "$match" ] || { echo "no driver for '$1' in $LIB (see: $0)"; exit 1; }
NEW="$LIB/mesa-$match/libvulkan_intel.so"

TMPICD=$(mktemp --suffix=.json)
trap 'rm -f "$TMPICD"' EXIT
printf '{ "ICD": { "api_version": "1.4.348", "library_arch": "64", "library_path": "%s" }, "file_format_version": "1.0.1" }\n' "$NEW" > "$TMPICD"
if ! XMX_CHECK_LIB="$NEW" XMX_CHECK_ICD="$TMPICD" bash "$PKG/tools/session-check.sh" --dry-run; then
  echo "!! Mesa $match failed its check; nothing changed"
  bash "$PKG/tools/session-check.sh" --dry-run >/dev/null 2>&1   # driver-status back to the active driver
  exit 1
fi
# copy + rename: a running game keeps the old file
cp "$NEW" "$LIB/.libvulkan_intel.so.new" && mv -f "$LIB/.libvulkan_intel.so.new" "$LIB/libvulkan_intel.so"
echo "$match" > "$LIB/active"
bash "$PKG/tools/session-check.sh" --dry-run >/dev/null 2>&1
echo "== active: Mesa $match. Games started from now on use it."
if command -v systemctl >/dev/null && ! systemctl --user show-environment 2>/dev/null | grep -q "^VK_DRIVER_FILES=$PKG/run/intel_icd.json"; then
  echo "   This session does not use the patched driver (session check failed, or install.sh not run): log out and in once."
fi
