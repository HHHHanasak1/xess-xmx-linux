#!/bin/bash
# switch-driver.sh [version] - choose the default among the package's patched drivers (lib/mesa-<version>/).
# lib/libvulkan_intel.so loads the driver named in lib/active, unless a game sets MESA in its launch options
# (MESA=26.1 %command%), which wins for that game.
#   tools/switch-driver.sh          list the drivers, mark the default
#   tools/switch-driver.sh 26.3     make Mesa 26.3 the default (a prefix is enough: 26.3, 26.1.2, ...)
# The driver is checked like the session check does first. Games that are running keep the driver they loaded; the
# next game started uses the new default. No re-login needed.
set -u
PKG="$(cd "$(dirname "$0")/.." && pwd)"
. "$PKG/tools/steam-libraries.sh"
LIB="$PKG/lib"
STATUS="${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/driver-status"
active=$(cat "$LIB/active" 2>/dev/null)

versions() { local d; for d in "$LIB"/mesa-*/; do [ -f "$d/libvulkan_intel.so" ] && basename "$d" | sed 's/^mesa-//'; done; }

if [ -z "${1:-}" ]; then
  for v in $(versions); do [ "$v" = "$active" ] && echo "* $v (default)" || echo "  $v"; done
  exit 0
fi

# same choice as lib/libvulkan_intel.so: the exact version, else a final release before a release candidate, else the newest
match=$(versions | awk -v w="$1" 'index($0, w) == 1 { print ($0 == w ? 2 : ($0 ~ /-/ ? 0 : 1)) " " $0 }' | sort -k1,1n -k2,2V | tail -1 | cut -d' ' -f2)
[ -n "$match" ] || { echo "no driver for '$1' in $LIB (see: $0)"; exit 1; }
NEW="$LIB/mesa-$match/libvulkan_intel.so"

TMPICD=$(mktemp --suffix=.json)
trap 'rm -f "$TMPICD"' EXIT
xmx_icd_json "$NEW" > "$TMPICD"
# the check writes driver-status for the driver it tests: with the new one as the default, that line is the one the
# session check would write (same library path, same driverInfo), so no second check is needed on success; on failure
# the line the default driver had goes back
old_status=$(cat "$STATUS" 2>/dev/null)
if ! XMX_CHECK_LIB="$NEW" XMX_CHECK_ICD="$TMPICD" bash "$PKG/tools/session-check.sh" --dry-run; then
  echo "!! Mesa $match failed its check; nothing changed"
  if [ -n "$old_status" ]; then printf '%s\n' "$old_status" > "$STATUS"
  else bash "$PKG/tools/session-check.sh" --dry-run >/dev/null 2>&1; fi   # driver-status back to the default driver
  exit 1
fi
echo "$match" > "$LIB/active.new" && mv -f "$LIB/active.new" "$LIB/active"
echo "== default: Mesa $match. Games started from now on use it (unless their launch options set MESA)."
if command -v systemctl >/dev/null && ! systemctl --user show-environment 2>/dev/null | grep -q "^VK_DRIVER_FILES=$PKG/run/intel_icd.json"; then
  echo "   This session does not use the patched driver (session check failed, or install.sh not run): log out and in once."
fi
