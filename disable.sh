#!/bin/bash
# disable.sh - go back to the stock XeSS DP4a path (same settings as enable.sh: XMX_APPID, XMX_PREFIX, XMX_CONF, XMX_GAME)
ROOT="$(cd "$(dirname "$0")" && pwd)"
APPID="${XMX_APPID:-${WUWA_APPID:-3513350}}"
PFXROOT="${XMX_PREFIX:-${WUWA_PREFIX:-$HOME/.local/share/Steam/steamapps/compatdata/$APPID/pfx}}"
PFX="$PFXROOT/drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1"
CONF="${XMX_CONF:-$HOME/.config/wuwa-opti.conf}"
GAME="${XMX_GAME:-Client-Win64-Shipping}"
pgrep -f "[${GAME:0:1}]${GAME:1}" >/dev/null && { echo "The game is running - close it first."; exit 1; }
[ -f "$PFX/igdext64.dll.stock" ] && cp "$PFX/igdext64.dll.stock" "$PFX/igdext64.dll" && echo "stock igdext64.dll restored"
[ -f "$CONF" ] && sed -i '/# >>> XESS-XMX >>>/,/# <<< XESS-XMX <<</d' "$CONF" && echo "launcher config block removed from $CONF"
rm -rf "$HOME/.cache/xess-xmx-mesa"
echo "XeSS XMX disabled."
