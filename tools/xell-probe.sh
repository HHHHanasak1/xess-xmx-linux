#!/bin/bash
# xell-probe.sh - one-launch capture of how Intel XeLL (libxell.dll) talks to the shim under Proton.
#
# Steam launch options for the game:   /path/to/package/tools/xell-probe.sh %command%
# (keep xmx-launch.sh in front if the game uses the per-game install:  xmx-launch.sh xell-probe.sh %command%)
#
# It turns on, for this launch only:
#   IGDEXT_TRACE=1            shim trace            -> C:\igdext_trace.log      (prefix)
#   IGDEXT_XELL_LOG=1         XeLL hook log         -> C:\igdext_xell.log       (prefix)
#   IGDEXT_XELL_PROBE=1       the shim hides Wine from libxell.dll, claims latency-reduction support v1 and logs every
#                             call and buffer access (shim/src/dll/XellDriver.cpp)
#   XELL_FORCE_LOGS_TO_FILE=1 libxell's own log     -> libxell.log  (libxell 1.3.2+ only; 1.3.0.x has no file log)
#   PROTON_LOG=1              Proton log            -> ~/steam-<appid>.log: libxell's OutputDebugString messages
#                             ("[XeLL] Using built-in context", "Intel Extensions initialized.", "Latency reduction
#                             enabled ...") land here with every libxell version; only the XeLL lines are kept
# and after the game exits writes ~/xess-xmx-probe-<date>.tar.gz with those files plus the session's driver status,
# and prints the lines that matter (the mode XeLL chose, why, what it called on the shim). Send the archive with a bug
# report / probe result.
#
# Variables:  XELL_PROBE_VERSION=<n>   interface version to claim (default 1; try 2, 3 if XeLL rejects 1)
#             XELL_PROBE_TELEMETRY=0   answer GetDisplayTelemetry with E_NOTIMPL instead of the display model (A/B)
#             XELL_PROBE_GPUSTART=0    GPU start = clock at submit instead of the measured timestamp (A/B)
#             XELL_PROBE_FLIPQ=1       display telemetry with the present queue (frames waiting for the GPU and for
#                                      their vsync); off by default, it made XeLL sleep less in the first test (A/B)
#             XELL_PROBE_GUARD=1       timing buffers on guard pages: log every access XeLL makes to them
#             XELL_PROBE_HZ=<hz>, XELL_PROBE_VRR=<min>,<max>   the display model's refresh rate(s) (default: current mode)
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
# Proton writes $PROTON_LOG_DIR/steam-$SteamGameId.log (SteamGameId is set for non-Steam shortcuts too); PROTON_LOG=0
# from the user's launch options would switch it off, so the probe always sets 1
export PROTON_LOG=1
protonlog="${PROTON_LOG_DIR:-$HOME}/steam-${SteamGameId:-${SteamAppId:-0}}.log"
[ -f "$protonlog" ] && mv -f "$protonlog" "$protonlog.prev"
[ "${XELL_PROBE_TELEMETRY:-1}" = 0 ] && export IGDEXT_XELL_TELEMETRY=0
[ "${XELL_PROBE_GUARD:-0}" = 1 ] && export IGDEXT_XELL_GUARD=1
[ "${XELL_PROBE_GPUSTART:-1}" = 0 ] && export IGDEXT_XELL_GPUSTART=0
[ "${XELL_PROBE_FLIPQ:-0}" = 1 ] && export IGDEXT_XELL_FLIPQ=1
[ -n "${XELL_PROBE_HZ:-}" ] && export IGDEXT_XELL_HZ="$XELL_PROBE_HZ"
[ -n "${XELL_PROBE_VRR:-}" ] && export IGDEXT_XELL_VRR="$XELL_PROBE_VRR"

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
[ -f "$protonlog" ] && grep -a -i -E 'xell|intel extensions|latency reduction|igdext' "$protonlog" > "$out/proton-xell.log"
cp -f "${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/driver-status" "$out/" 2>/dev/null
{
  echo "date: $stamp"; echo "exit code: $rc"; echo "command: $*"; echo "prefix: $pfx"
  echo "probe version: $IGDEXT_XELL_PROBE  telemetry: ${IGDEXT_XELL_TELEMETRY:-1}  gpustart: ${IGDEXT_XELL_GPUSTART:-1}  flipq: ${IGDEXT_XELL_FLIPQ:-0}  guard: ${IGDEXT_XELL_GUARD:-0}"
  echo "kernel: $(uname -r)"; echo "mesa: $(grep -a -m1 -o 'Mesa [0-9.a-z-]*' "$out/driver-status" 2>/dev/null)"
  env | grep -a -E '^(INTC_|XELL_|IGDEXT_|PROTON_|VKD3D_|VK_DRIVER|STEAM_COMPAT_TOOL|STEAM_COMPAT_DATA)' | sort
} > "$out/environment.txt"

echo >&2; echo "xell-probe: what XeLL did" >&2
pat='Loading Intel Extensions|Intel Extensions (initialized|initialization failed|library loaded)|Cannot load Intel|Driver version|Latency reduction|Using (built-in|external|driver DLL|game DLL) context|unsupported (device|driver)|Driver timestamps|Driver timings|GetRenderStartTimestamp|context creat|Display telemetry|telemetry corrupted|XeFG (enabled|disabled)|Display config|match not found|not available'
found=0
for f in "$out/libxell.log" "$out/proton-xell.log"; do
  [ -s "$f" ] || continue
  echo "  [$(basename "$f")]" >&2; grep -a -i -E "$pat" "$f" | head -40 >&2; found=1
done
[ $found = 1 ] || echo "  (no libxell.log and no XeLL lines in $protonlog: did the game use XeSS frame generation?)" >&2
echo >&2; echo "xell-probe: what the shim saw" >&2
grep -a -E 'XELLPROBE|GPU: PCI|device info' "$out/igdext_trace.log" 2>/dev/null |
  grep -a -v -E 'gpu start frame [0-9]+: measured .* after submit|execute delay frame|flip queue: [0-9]+ frame' | head -60 >&2
echo >&2; echo "xell-probe: what XeLL's driver-mode sleep sees (its decision messages never reach any log: derived from the measurements)" >&2
qw=$(grep -a -o -E '[-+][0-9.]+ ms after submit' "$out/igdext_trace.log" 2>/dev/null | awk '{print $1+0}' | sort -n | awk '{a[NR]=$1} END{if(NR) printf "%.1f", a[int((NR+1)/2)]}')
ss=$(grep -a 'xell latency' "$out/igdext_xell.log" 2>/dev/null | tail -1 | grep -a -o -E 'sim -> render submit [0-9.]+' | awk '{print $NF}')
if [ -n "$qw" ] && [ -n "$ss" ]; then
  echo "  render-queue wait (GPU start - hand-in, median): $qw ms; CPU prepare (sim -> render submit, XeLL): $ss ms" >&2
  awk -v q="$qw" -v s="$ss" 'BEGIN{ if (q+0 < s+0) printf "  -> CPU bound: the GPU is free %.1f ms before the CPU has the next frame ready; XeLL sleeps nothing, same as the Windows driver path would\n", s-q; else printf "  -> GPU bound by %.1f ms: XeLL should sleep that much at simulation start; if Sleep blocked stays ~0.5 ms, pacing is not acting on the timings\n", q-s }' >&2
else
  echo "  (needs the gpu start lines in igdext_trace.log and a 'xell latency' line in igdext_xell.log)" >&2
fi
med() { sort -n | awk '{a[NR]=$1} END{if(NR) printf "%.2f", a[int((NR+1)/2)]}'; }
ed=$(grep -a -o -E 'execute delay frame [0-9]+: [0-9.]+ ms' "$out/igdext_trace.log" 2>/dev/null | awk '{print $(NF-1)}' | med)
fqn=$(grep -a -o -E 'flip queue: [0-9]+ frame' "$out/igdext_trace.log" 2>/dev/null | awk '{print $3}' | med)
fqm=$(grep -a -o -E 'last flip in queue [0-9.]+ ms' "$out/igdext_trace.log" 2>/dev/null | awk '{print $(NF-1)}' | med)
[ -n "$ed" ] && echo "  RenderSubmitStart -> first ExecuteCommandLists (XeLL's \"execute delay\", median): $ed ms" >&2
[ -n "$fqn" ] && echo "  present queue told to XeLL (median): $fqn frame(s) waiting, flip in queue $fqm ms (XELL_PROBE_FLIPQ=1 run)" >&2
echo >&2; echo "xell-probe: latency XeLL measured (simulation start -> present end; compare with a run without the probe)" >&2
grep -a 'xell latency' "$out/igdext_xell.log" 2>/dev/null | tail -8 >&2
[ -s "$out/igdext_trace.log" ] || echo "  (no igdext_trace.log: the shim is not installed in this prefix; run install.sh / enable.sh)" >&2

tar -C "$HOME" -czf "$out.tar.gz" "$(basename "$out")" && rm -rf "$out"
[ "${XELL_PROBE_KEEP:-0}" = 1 ] || rm -f "$pfx/igdext_trace.log" "$pfx/igdext_xell.log"
echo >&2; echo "xell-probe: archive $out.tar.gz" >&2
exit $rc
