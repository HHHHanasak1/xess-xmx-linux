#!/bin/bash
# disable.sh - go back to the stock XeSS DP4a path (same settings as enable.sh: XMX_APPID or XMX_PREFIX, XMX_CONF, XMX_GAME)
ROOT="$(cd "$(dirname "$0")" && pwd)"
APPID="${XMX_APPID:-}"
PFXROOT="${XMX_PREFIX:-}"
. "$ROOT/tools/steam-libraries.sh"
# the prefix in whichever Steam library holds the game (SD card, second disk)
[ -z "$PFXROOT" ] && [ -n "$APPID" ] && PFXROOT="$(steam_prefix_of "$APPID" || printf '%s' "$(steam_root)/steamapps/compatdata/$APPID/pfx")"
[ -n "$PFXROOT" ] || { echo "Set XMX_APPID=<Steam app id> or XMX_PREFIX=<prefix folder>."; exit 1; }
CONF="${XMX_CONF:-$HOME/.config/xess-xmx.conf}"
GAME="${XMX_GAME:-}"
if [ -n "$GAME" ] && pgrep -i "^${GAME:0:15}" >/dev/null; then echo "The game is running - close it first."; exit 1; fi
# every case variant of the driver store folder (enable.sh puts the shim into each)
while IFS= read -r PFX; do
  [ -n "$PFX" ] || continue
  if [ -f "$PFX/igdext64.dll.stock" ]; then
    cp "$PFX/igdext64.dll.stock" "$PFX/igdext64.dll" && echo "stock igdext64.dll restored in $PFX"
  elif [ -f "$PFX/igdext64.dll" ] && grep -aq 'IGDEXT_' "$PFX/igdext64.dll"; then
    rm -f "$PFX/igdext64.dll" && echo "igdext64.dll removed from $PFX"   # a prefix without a stock one (every build of the shim names its IGDEXT_* switches)
  fi
done <<< "$(prefix_shim_dirs "$PFXROOT")"
C="$PFXROOT/drive_c/windows/system32/ControlLib.dll"
[ -f "$C" ] && grep -aq 'XESS_FG' "$C" && rm -f "$C" && echo "ControlLib.dll removed"
[ -f "$CONF" ] && sed -i '/# >>> XESS-XMX >>>/,/# <<< XESS-XMX <<</d' "$CONF" && echo "launcher config block removed from $CONF"
rm -rf "$PFXROOT/drive_c/igdext_kernels"
echo "XeSS XMX disabled."
