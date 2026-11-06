#!/bin/bash
# tests/build-probe.sh <out dir> - build the device probe (tests/probe.c) and the template SPIR-V. VULKAN_HEADERS as for
# build.sh.
set -eu
T="$(cd "$(dirname "$0")" && pwd)"; S="$T/../src"; B3="$T/../third_party/blake3"
O="$1"; mkdir -p "$O"
INC=(); [ -n "${VULKAN_HEADERS:-}" ] && INC=(-I"$VULKAN_HEADERS/include")
glslangValidator -V --target-env vulkan1.1 -S comp "$S/template.comp" -o "$O/template.spv" >/dev/null
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-format-truncation -Wno-missing-field-initializers "${INC[@]}" \
  -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0 \
  -o "$O/probe" "$T/probe.c" "$S/anvbin.c" "$S/cmk.c" "$S/compile.c" "$S/placeholder.c" "$S/prologue.c" \
  "$B3/blake3.c" "$B3/blake3_dispatch.c" "$B3/blake3_portable.c" -ldl
echo "built $O/probe $O/template.spv"
