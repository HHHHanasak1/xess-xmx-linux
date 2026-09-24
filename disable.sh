#!/bin/bash
# disable.sh - go back to the stock XeSS DP4a path (same settings as enable.sh: XMX_APPID or XMX_PREFIX, XMX_CONF, XMX_GAME)
ROOT="$(cd "$(dirname "$0")" && pwd)"
APPID="${XMX_APPID:-${WUWA_APPID:-}}"
PFXROOT="${XMX_PREFIX:-${WUWA_PREFIX:-}}"
[ -z "$PFXROOT" ] && [ -n "$APPID" ] && PFXROOT="$HOME/.local/share/Steam/steamapps/compatdata/$APPID/pfx"
[ -n "$PFXROOT" ] || { echo "Set XMX_APPID=<Steam app id> or XMX_PREFIX=<prefix folder>."; exit 1; }
PFX="$PFXROOT/drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1"
CONF="${XMX_CONF:-$HOME/.config/xess-xmx.conf}"
GAME="${XMX_GAME:-}"
if [ -n "$GAME" ] && pgrep -i "^${GAME:0:15}" >/dev/null; then echo "The game is running - close it first."; exit 1; fi
[ -f "$PFX/igdext64.dll.stock" ] && cp "$PFX/igdext64.dll.stock" "$PFX/igdext64.dll" && echo "stock igdext64.dll restored"
[ -f "$CONF" ] && sed -i '/# >>> XESS-XMX >>>/,/# <<< XESS-XMX <<</d' "$CONF" && echo "launcher config block removed from $CONF"
rm -rf "$PFXROOT/drive_c/igdext_kernels"
echo "XeSS XMX disabled."
