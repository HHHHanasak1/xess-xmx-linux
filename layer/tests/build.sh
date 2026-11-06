#!/bin/bash
# tests/build.sh [out dir] - build the test shaders and the unit tests of the layer, then run them (Linux, needs gcc and
# glslangValidator).
set -eu
T="$(cd "$(dirname "$0")" && pwd)"
O="${1:-$T/out}"
mkdir -p "$O"
spv() { # <name> <defines...>
  local n=$1; shift
  glslangValidator -V --target-env vulkan1.1 -S comp "$@" "$T/ph.comp" -o "$O/$n.spv" >/dev/null
}
spv kernel_vec     -DWX=1 -DWY=1 -DWZ=23 -DA=0x55667788u -DB=0x11223344u
spv kernel_scalar  -DWX=1 -DWY=1 -DWZ=23 -DA=0x55667788u -DB=0x11223344u -DSCALAR
spv canary         -DWX=1 -DWY=1 -DWZ=19 -DA=0x00414e43u -DB=0x7fa15952u
spv canary_bad     -DWX=1 -DWY=1 -DWZ=19 -DA=0x00414e43u -DB=0x00000000u
spv canary_legacy  -DWX=1 -DWY=1 -DWZ=17 -DA=0u -DB=0u
spv legacy         -DWX=3 -DWY=2 -DWZ=11 -DA=0u -DB=0u
spv plain          -DWX=8 -DWY=8 -DWZ=1 -DA=0u -DB=0u -DPLAIN
spv other          -DWX=8 -DWY=8 -DWZ=1 -DA=0u -DB=0u
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-format-truncation -o "$O/test_placeholder" "$T/test_placeholder.c" "$T/../src/placeholder.c"
"$O/test_placeholder" "$O"
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-format-truncation -o "$O/test_cmk" "$T/test_cmk.c" "$T/../src/cmk.c"
# the offline kernel set (zebins with the cmk_pack.py image of each), if present
CMK_SET="${CMK_SET:-$HOME/decouple-exp/out/nlpi}"
if [ -d "$CMK_SET" ]; then "$O/test_cmk" "$CMK_SET"; else echo "test_cmk: skipped, no $CMK_SET"; fi
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-format-truncation -o "$O/test_compile" "$T/test_compile.c" "$T/../src/compile.c" "$T/../src/cmk.c" \
  "$T/../src/placeholder.c"
# end to end with ocloc: needs a kernel folder (dyn_N.spv/.opt + map.txt), the -noLoadPayload reference set and an IGC
KSET="${KSET:-$HOME/decouple-exp/kernels/igdext_kernels}"
if [ -d "$KSET" ] && [ -d "$CMK_SET" ] && [ -n "${XMX_IGC_DIR:-}" ]; then
  K="$O/kset"; rm -rf "$K" "$O/cache"; mkdir -p "$K"
  cp "$KSET"/dyn_[0-9].spv "$KSET"/dyn_[0-9].opt "$KSET"/map.txt "$K/"
  OLD="$O/old-cm-compile.sh"   # the script without CM_NO_LOAD_PAYLOAD (before this change)
  sed '/CM_NO_LOAD_PAYLOAD:-/,/^  fi$/c\  "$OCLOC" compile -spirv_input -file "$SPV" -device "$DEV" -out_dir "$W" -output k -options "-vc-codegen $fl" > "$W/ocloc.log" 2>&1' \
    "$T/../../tools/cm-compile.sh" > "$OLD"
  ANV_CM_HELPER="$T/../../tools/cm-compile.sh" CM_DEVICE=ptl XDG_CACHE_HOME="$O/cache" CM_LOG="$O/compile.log" \
    "$O/test_compile" "$K" "$CMK_SET" "$OLD"
else
  echo "test_compile: skipped (KSET, CMK_SET or XMX_IGC_DIR missing)"
fi
# prologue: the C table against the assembler, for every kernel of the set (needs IGC for libiga64)
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-format-truncation -o "$O/test_prologue" "$T/test_prologue.c" \
  "$T/../src/prologue.c" "$T/../src/cmk.c"
if [ -d "$KSET" ] && [ -d "$CMK_SET" ] && [ -n "${IGC:-}" ]; then
  P="$O/pro"; rm -rf "$P"; mkdir -p "$P"
  same=0; diff=0; none=0; n=0
  for d in "$CMK_SET"/*/; do
    i=$(basename "$d"); z=$(ls "$d"k_*.bin 2>/dev/null | head -1); [ -n "$z" ] && [ -f "$KSET/dyn_$i.opt" ] || continue
    n=$((n + 1))
    "$O/test_prologue" cml "$z" "$KSET/dyn_$i.opt" "$P/$i.cml" || { none=$((none + 1)); continue; }
    if ! "$O/test_prologue" pro "$P/$i.cml" "$P/$i.c.bin"; then none=$((none + 1)); continue; fi
    python3 "$T/../tools/prologue_asm.py" "$P/$i.cml" "$P/$i.py.bin"
    if cmp -s "$P/$i.c.bin" "$P/$i.py.bin"; then same=$((same + 1)); else diff=$((diff + 1)); fi
  done
  echo "test_prologue: $n kernels, $same identical to the assembler, $diff different, $none without a prologue"
  [ "$diff" = 0 ] && [ "$none" = 0 ] && [ "$n" -gt 0 ]
else
  echo "test_prologue: skipped (KSET, CMK_SET or IGC missing)"
fi
# the driver's serialized shaders: round trip of exported blobs (TEMPLATES: a folder of *.blob from a Mesa 26.1.2 device)
B3="$T/../third_party/blake3"
B3SRC=("$B3/blake3.c" "$B3/blake3_dispatch.c" "$B3/blake3_portable.c")
B3DEF=(-DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0)
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-format-truncation "${B3DEF[@]}" -o "$O/test_anvbin" "$T/test_anvbin.c" \
  "$T/../src/anvbin.c" "${B3SRC[@]}"
if [ -n "${TEMPLATES:-}" ] && ls "$TEMPLATES"/*.blob >/dev/null 2>&1; then
  "$O/test_anvbin" "$TEMPLATES"/*.blob
else
  echo "test_anvbin: skipped (TEMPLATES not set)"
fi
