#!/bin/bash
# xmx-launch.sh - generic Steam launch wrapper for the XeSS XMX package: sources the config block written by enable.sh
# (XMX_CONF, default ~/.config/xess-xmx.conf) and starts the game. Steam launch options:  /path/to/xess-xmx-linux/xmx-launch.sh %command%
CONF="${XMX_CONF:-$HOME/.config/xess-xmx.conf}"
[ -f "$CONF" ] && . "$CONF"
exec "$@"
