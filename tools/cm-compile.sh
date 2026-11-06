#!/bin/bash
# cm-compile.sh <kernel.spv> <options.txt> <out.cmk>
# Compiles one XeSS CM kernel (SPIR-V + the compile options the shim recorded) for the local Intel GPU with the bundled
# IGC/ocloc. Called by the driver at run time (ANV_CM_HELPER, default <package>/tools/cm-compile.sh) as `/bin/sh
# cm-compile.sh`, which may be dash: POSIX shell only.
# With CM_OUT_DIR (set by the driver) the script only runs ocloc into that folder and leaves the device and the flags
# it used in CM_OUT_DIR/info; the driver packs the zebin into the CMK2 format itself and writes <out.cmk> and the log
# line. On that path ocloc is the only program it starts. Without CM_OUT_DIR (a driver of an older package, or by hand)
# it packs with tools/cmk_pack.py into <out.cmk>. Exit 0 = ok.
# Environment: XMX_IGC_DIR (default <package>/igc); CM_DEVICE (ocloc -device, overrides the detection); CM_PCI_ID (PCI device
# id, passed by the driver; otherwise the first Intel render node is used); CM_GRF_BYTES (GRF size in bytes, passed by
# the driver; the cmk_pack.py path needs it); CM_OUT_DIR (see above); CM_LOG (append log path, default
# ~/.cache/xess-xmx/compile.log); CM_NO_LOAD_PAYLOAD=1 (CM_OUT_DIR path only, set by the layer for an unpatched
# driver): the kernel does not load its payload from indirect data but expects it in registers, where the layer's
# prologue puts it. It goes in as an internal option because the embedded -vc-payload options of some kernels replace
# whatever -options says.
set -u
SPV=$1; OPT=$2; OUT=$3
case $0 in */*) HERE=${0%/*} ;; *) HERE=. ;; esac
PKG=$HERE/..
IGC="${XMX_IGC_DIR:-$PKG/igc}"
LOG="${CM_LOG:-${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/compile.log}"   # outside the Steam runtime container too

# target: ocloc takes a family name or a PCI device id (it picks the exact target for an id)
pci="${CM_PCI_ID:-}"
if [ -z "$pci" ]; then
  for d in /sys/class/drm/renderD*/device; do
    v=; [ -r "$d/vendor" ] && read -r v < "$d/vendor"
    [ "$v" = 0x8086 ] && { read -r pci < "$d/device"; break; }
  done
fi
DEV="${CM_DEVICE:-}"
if [ -z "$DEV" ]; then
  case "$pci" in
    0xb0[89a-f]?|0xfd8?) DEV=ptl ;;    # Panther Lake / Wildcat Lake (Xe3): the tested target, family name kept for identical output
    "") DEV=ptl ;;
    *) DEV=$pci ;;
  esac
fi

OCLOC=
for f in "$IGC"/neo/bin/ocloc*; do [ -x "$f" ] && { OCLOC=$f; break; }; done
[ -n "$OCLOC" ] || { echo "cm-compile: ocloc not found under $IGC (run tools/get-igc.sh)" >> "$LOG"; exit 1; }
export LD_LIBRARY_PATH="$IGC/igc/local/lib:$IGC/neo/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# the options file as one string (the shim writes it without NUL bytes or a newline)
opts=
while IFS= read -r line || [ -n "$line" ]; do opts="$opts$line
"; done < "$OPT"
# the ocloc flags XeSS passes: -doubleGRF and -ze-*
fl=
set -f
for o in $opts; do case $o in -doubleGRF*|-ze-*) fl="$fl$o " ;; esac; done
set +f

if [ -n "${CM_OUT_DIR:-}" ]; then
  W=$CM_OUT_DIR
  if [ "${CM_NO_LOAD_PAYLOAD:-}" = 1 ]; then
    "$OCLOC" compile -spirv_input -file "$SPV" -device "$DEV" -out_dir "$W" -output k -options "-vc-codegen $fl" \
      -internal_options "-llvm-options -finalizer-opts=-noLoadPayload" > "$W/ocloc.log" 2>&1
    fl="$fl-noLoadPayload "
  else
    "$OCLOC" compile -spirv_input -file "$SPV" -device "$DEV" -out_dir "$W" -output k -options "-vc-codegen $fl" > "$W/ocloc.log" 2>&1
  fi
  printf '%s\n%s\n' "$DEV" "$fl" > "$W/info"
  for BIN in "$W"/k_*.bin; do break; done
  if [ ! -s "$BIN" ]; then
    { echo "cm-compile FAILED: $SPV (${OPT##*/}) device $DEV flags [$fl]"; tail -5 "$W/ocloc.log"; } >> "$LOG"
    exit 1
  fi
  exit 0
fi

# ---- packing here (a driver of an older package): unchanged from before the driver packed itself
# per-thread payload stride = GRF size, passed by the driver; for an older driver that does not pass it: 64 bytes on
# Xe2 and later, 32 bytes on Xe-HPG / Xe-LPG
GRF="${CM_GRF_BYTES:-}"
if [ -z "$GRF" ]; then
  case "$pci" in
    0x56??|0x7d??) GRF=32 ;;
    *) GRF=64 ;;
  esac
fi
W=$(mktemp -d "${TMPDIR:-/tmp}/cmk.XXXXXX")
trap 'rm -rf "$W"' EXIT
lws=$(printf '%s' "$opts" | grep -o -E 'LWS_SIZE_X=[0-9]+ -DLWS_SIZE_Y=[0-9]+ -DLWS_SIZE_Z=[0-9]+' | head -1 | grep -o -E '[0-9]+' | tr '\n' ' ')
set -- $lws
lx=${1:-1}; ly=${2:-1}; lz=${3:-1}
t0=$(date +%s.%N)
"$OCLOC" compile -spirv_input -file "$SPV" -device "$DEV" -out_dir "$W" -output k -options "-vc-codegen $fl" > "$W/ocloc.log" 2>&1
BIN=$(ls "$W"/k_*.bin 2>/dev/null | head -1)
if [ -z "$BIN" ] || [ ! -s "$BIN" ]; then
  { echo "cm-compile FAILED: $SPV ($(basename "$OPT")) device $DEV lws ${lx}x${ly}x${lz}"; tail -5 "$W/ocloc.log"; } >> "$LOG"
  exit 1
fi
# a private temporary name: two compiles of the same kernel (XeSS and XeFG contexts on different threads) must not
# truncate each other's output before the rename; mv within one directory is atomic, the last writer wins intact
TMP="$OUT.$$.tmp"
if CMK_GC=1,1,1 CMK_GRF_BYTES=$GRF python3 "$HERE/cmk_pack.py" "$BIN" auto "$lx" "$ly" "$lz" "$TMP" --heap auto --surf auto >> "$W/pack.log" 2>&1 && mv -f "$TMP" "$OUT"; then
  echo "cm-compile ok: $(basename "$OUT") from $(basename "$SPV") device $DEV lws ${lx}x${ly}x${lz} flags [$fl] in $(awk "BEGIN{printf \"%.1f\", $(date +%s.%N) - $t0}") s" >> "$LOG"
  exit 0
fi
{ echo "cm-compile PACK FAILED: $SPV"; tail -3 "$W/pack.log"; } >> "$LOG"
rm -f "$TMP"
exit 1
