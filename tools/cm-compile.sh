#!/bin/bash
# cm-compile.sh <kernel.spv> <options.txt> <out.cmk>
# Compiles one XeSS CM kernel (SPIR-V + the compile options the shim recorded) for the local Intel GPU with the bundled
# IGC/ocloc and packs it into the CMK2 format the patched ANV loads. Called by the driver at run time for kernels that are
# not in the static kernel set (ANV_CM_HELPER, default <package>/tools/cm-compile.sh). Exit 0 and <out.cmk> present = ok.
# Environment: XMX_IGC_DIR (default <package>/igc), CM_DEVICE (default ptl), CM_LOG (append log path).
set -u
SPV=$1; OPT=$2; OUT=$3
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(dirname "$HERE")"
IGC="${XMX_IGC_DIR:-$PKG/igc}"
DEV="${CM_DEVICE:-ptl}"
LOG="${CM_LOG:-${XDG_RUNTIME_DIR:-/tmp}/xess-xmx-compile.log}"
OCLOC=$(ls "$IGC"/neo/bin/ocloc* 2>/dev/null | head -1)
[ -x "$OCLOC" ] || { echo "cm-compile: ocloc not found under $IGC" >> "$LOG"; exit 1; }
export LD_LIBRARY_PATH="$IGC/igc/local/lib:$IGC/neo/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
W=$(mktemp -d "${TMPDIR:-/tmp}/cmk.XXXXXX")
trap 'rm -rf "$W"' EXIT
opts=$(tr -d '\0' < "$OPT")
fl=$(printf '%s' "$opts" | tr ' ' '\n' | grep -E '^-(doubleGRF|ze-)' | tr '\n' ' ')
lws=$(printf '%s' "$opts" | grep -o -E 'LWS_SIZE_X=[0-9]+ -DLWS_SIZE_Y=[0-9]+ -DLWS_SIZE_Z=[0-9]+' | head -1 | grep -o -E '[0-9]+' | tr '\n' ' ')
set -- $lws
lx=${1:-1}; ly=${2:-1}; lz=${3:-1}
t0=$(date +%s.%N)
if ! "$OCLOC" compile -spirv_input -file "$SPV" -device "$DEV" -out_dir "$W" -output k -options "-vc-codegen $fl" > "$W/ocloc.log" 2>&1 || [ ! -s "$W/k_$DEV.bin" ]; then
  { echo "cm-compile FAILED: $SPV ($(basename "$OPT")) lws ${lx}x${ly}x${lz}"; tail -5 "$W/ocloc.log"; } >> "$LOG"
  exit 1
fi
if CMK_GC=1,1,1 python3 "$HERE/cmk_pack.py" "$W/k_$DEV.bin" auto "$lx" "$ly" "$lz" "$OUT.tmp" --heap auto --surf auto >> "$W/pack.log" 2>&1 && mv -f "$OUT.tmp" "$OUT"; then
  echo "cm-compile ok: $(basename "$OUT") from $(basename "$SPV") lws ${lx}x${ly}x${lz} flags [$fl] in $(awk "BEGIN{printf \"%.1f\", $(date +%s.%N) - $t0}") s" >> "$LOG"
  exit 0
fi
{ echo "cm-compile PACK FAILED: $SPV"; tail -3 "$W/pack.log"; } >> "$LOG"
rm -f "$OUT.tmp"
exit 1
