#!/bin/bash
# build-kernels.sh <dump dir> <map file> <out dir>
# Compiles every unique XeSS CM kernel of a shim dump (C:\igdext_dump of the game prefix, see README) for Intel Xe3 and
# packs them as <out dir>/wg<1+id%16>_<1+id/16>_5.cmk, the layout the patched ANV loads from ANV_CM_KERNEL_DIR.
#   <map file> comes from s16_map.py: "id dumpindex hash name lx ly lz typed" per unique kernel
# Toolchain (environment):
#   OCLOC        path to ocloc (compute-runtime 25.18.1 works)                      default: first ocloc* in $NEO_DIR/usr/bin
#   NEO_DIR      unpacked intel-compute-runtime debs (libigdrcl, libze_intel_gpu)   default: ~/xmx/neo2518
#   IGC_DIRS     unpacked IGC debs to try in order (space separated)               default: "~/xmx/igc2100 ~/xmx/igc_i2125 ~/xmx/igc_d216"
#                IGC 2.10.10 is the primary compiler; 2.12.5 / 2.16.0 are fallbacks for kernels it crashes on.
#   DEVICE       ocloc device name                                                  default: ptl
#   JOBS         parallel compiles                                                  default: 4
set -u
DUMP=$1; MAP=$2; OUT=$3
NEO_DIR="${NEO_DIR:-$HOME/xmx/neo2518}"
IGC_DIRS="${IGC_DIRS:-$HOME/xmx/igc2100 $HOME/xmx/igc_i2125 $HOME/xmx/igc_d216}"
OCLOC="${OCLOC:-$(ls "$NEO_DIR"/usr/bin/ocloc* 2>/dev/null | head -1)}"
DEVICE="${DEVICE:-ptl}"
JOBS="${JOBS:-4}"
TOOLS="$(cd "$(dirname "$0")" && pwd)"
[ -x "$OCLOC" ] || { echo "ocloc not found (set OCLOC or NEO_DIR)"; exit 1; }
ZS="$OUT.work"; mkdir -p "$OUT" "$ZS"
one() {
  set -- $1
  id=$1; n=$(printf "%04d" $2); name=$4; lx=$5; ly=$6; lz=$7
  x=$((1 + id % 16)); y=$((1 + id / 16))
  [ -s "$OUT/wg${x}_${y}_5.cmk" ] && { echo "kernel $id already built -> wg${x}_${y}_5"; return; }
  f="$DUMP/cs_${n}_type2.bin"
  fl=$(tr " " "\n" < "$DUMP/cs_${n}_options.txt" | grep -E "^-(doubleGRF|ze-)" | tr "\n" " ")
  ok=""
  for IG in $IGC_DIRS; do
    export LD_LIBRARY_PATH="$IG/usr/local/lib:$NEO_DIR/usr/lib/x86_64-linux-gnu"
    if "$OCLOC" compile -spirv_input -file "$f" -device "$DEVICE" -out_dir "$ZS" -output "k${id}" -options "-vc-codegen $fl" > "$ZS/k${id}_$(basename $IG).log" 2>&1 && [ -s "$ZS/k${id}_${DEVICE}.bin" ]; then ok=$(basename $IG); break; fi
    rm -f "$ZS/k${id}_${DEVICE}.bin"
  done
  [ -n "$ok" ] || { echo "kernel $id ($name, dump $n) FAILED with every IGC"; return; }
  if CMK_GC=1,1,1 python3 "$TOOLS/cmk_pack.py" "$ZS/k${id}_${DEVICE}.bin" auto "$lx" "$ly" "$lz" "$OUT/wg${x}_${y}_5.cmk" --heap auto --surf auto > "$ZS/pack_$id.log" 2>&1; then
    echo "kernel $id $name lws ${lx}x${ly}x${lz} -> wg${x}_${y}_5 ($ok)"
  else
    echo "kernel $id ($name) PACK FAILED"
  fi
}
export -f one; export OCLOC NEO_DIR IGC_DIRS DEVICE OUT ZS DUMP TOOLS
xargs -P "$JOBS" -I{} bash -c 'one "{}"' < "$MAP" | tee "$ZS/build.log"
echo "built: $(grep -c "\->" "$ZS/build.log")  failed: $(grep -ci "FAILED" "$ZS/build.log")"
python3 "$TOOLS/check-kernels.py" "$OUT" | tail -1
