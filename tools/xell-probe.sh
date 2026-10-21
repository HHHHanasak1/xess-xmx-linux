#!/bin/bash
# xell-probe.sh - one-launch capture of how Intel XeLL (libxell.dll) talks to the shim under Proton.
#
# Steam launch options for the game:   /path/to/package/tools/xell-probe.sh %command%
# (keep xmx-launch.sh in front if the game uses the per-game install:  xmx-launch.sh xell-probe.sh %command%)
#
# It turns on, for this launch only:
#   IGDEXT_TRACE=1            shim trace            -> C:\igdext_trace.log      (prefix)
#   IGDEXT_XELL_LOG=1         XeLL hook log         -> C:\igdext_xell.log       (prefix)
#   IGDEXT_XELL_PROBE=1       the shim claims latency-reduction support v1 and logs every call and buffer access
#   XELL_FORCE_LOGS_TO_FILE=1 libxell's own log     -> libxell.log              (game folder or working directory)
# and after the game exits writes ~/xess-xmx-probe-<date>.tar.gz with those files plus the session's driver status,
# and prints the lines that matter (which path XeLL loaded the extension library from, the mode it chose, what it
# called on the shim). Send the archive with a bug report / probe result.
#
# Variables:  XELL_PROBE_VERSION=<n>   interface version to claim (default 1; try 2, 3 if XeLL rejects 1)
#             XELL_PROBE_TELEMETRY=1   also answer GetDisplayTelemetry with a zeroed buffer (IGDEXT_XELL_TELEMETRY)
#             XELL_PROBE_KEEP=1        do not delete the raw log files from the prefix afterwards
set -u
[ $# -gt 0 ] || { echo "usage: xell-probe.sh <command...>   (Steam: tools/xell-probe.sh %command%)" >&2; exit 2; }

pfx="${STEAM_COMPAT_DATA_PATH:-}/pfx/drive_c"
[ -d "$pfx" ] || pfx="${WINEPREFIX:-$HOME/.wine}/drive_c"
stamp="$(date +%Y%m%d-%H%M%S)"
out="$HOME/xess-xmx-probe-$stamp"
mkdir -p "$out"

# start clean so the archive holds this launch only
for f in igdext_trace.log igdext_xell.log; do [ -f "$pfx/$f" ] && mv -f "$pfx/$f" "$pfx/$f.prev"; done
rm -f ./libxell.log

export IGDEXT_TRACE=1 IGDEXT_XELL_LOG=1 XELL_FORCE_LOGS_TO_FILE=1
export IGDEXT_XELL_PROBE="${XELL_PROBE_VERSION:-1}"
[ "${XELL_PROBE_TELEMETRY:-0}" = 1 ] && export IGDEXT_XELL_TELEMETRY=1

echo "xell-probe: prefix $pfx, probe version $IGDEXT_XELL_PROBE, logs -> $out" >&2
"$@"
rc=$?

# collect: shim + hook logs from the prefix, libxell.log wherever the game put it, session driver status
cp -f "$pfx/igdext_trace.log" "$out/" 2>/dev/null
cp -f "$pfx/igdext_xell.log" "$out/" 2>/dev/null
for f in ./libxell.log "$pfx/libxell.log" "$(dirname "$(command -v "$1" 2>/dev/null || echo "$1")")/libxell.log"; do
  [ -f "$f" ] && { cp -f "$f" "$out/libxell.log"; break; }
done
[ -f "$out/libxell.log" ] || find "$(pwd)" -maxdepth 3 -name libxell.log -newer "$out" -exec cp -f {} "$out/" \; 2>/dev/null
cp -f "${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/driver-status" "$out/" 2>/dev/null
{
  echo "date: $stamp"; echo "exit code: $rc"; echo "command: $*"; echo "prefix: $pfx"
  echo "probe version: $IGDEXT_XELL_PROBE  telemetry: ${IGDEXT_XELL_TELEMETRY:-0}"
  echo "kernel: $(uname -r)"; echo "mesa: $(grep -a -m1 -o 'Mesa [0-9.a-z-]*' "$out/driver-status" 2>/dev/null)"
  env | grep -a -E '^(INTC_|XELL_|IGDEXT_|PROTON_|VKD3D_|VK_DRIVER|STEAM_COMPAT_TOOL|STEAM_COMPAT_DATA)' | sort
} > "$out/environment.txt"

echo >&2; echo "xell-probe: what XeLL did" >&2
if [ -f "$out/libxell.log" ]; then
  grep -a -i -E 'Loading Intel Extensions|Intel Extensions (initialized|initialization failed|library loaded)|Cannot load Intel|Driver version|Latency reduction|Using (built-in|external|driver DLL|game DLL) context|unsupported (device|driver)|Driver timestamps|Driver timings|GetRenderStartTimestamp' \
    "$out/libxell.log" | head -40 >&2
else
  echo "  (no libxell.log found: the game may keep it elsewhere; look for '[XeLL] Logs output file' in its folder)" >&2
fi
echo >&2; echo "xell-probe: what the shim saw" >&2
grep -a 'XELLPROBE' "$out/igdext_trace.log" 2>/dev/null | head -60 >&2
[ -s "$out/igdext_trace.log" ] || echo "  (no igdext_trace.log: the shim is not installed in this prefix; run install.sh / enable.sh)" >&2

tar -C "$HOME" -czf "$out.tar.gz" "$(basename "$out")" && rm -rf "$out"
[ "${XELL_PROBE_KEEP:-0}" = 1 ] || rm -f "$pfx/igdext_trace.log" "$pfx/igdext_xell.log"
echo >&2; echo "xell-probe: archive $out.tar.gz" >&2
exit $rc
