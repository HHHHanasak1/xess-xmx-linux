#!/bin/bash
# tests/variants/build.sh <out dir> - the variant shaders of v.comp (see there)
set -eu
V="$(cd "$(dirname "$0")" && pwd)"; O="$1"; mkdir -p "$O"
g() { local n=$1; shift; glslangValidator -V --target-env vulkan1.1 -S comp "$@" "$V/v.comp" -o "$O/$n.spv" >/dev/null; }
g base
g ls842 -DLX=8 -DLY=4 -DLZ=2
g ls1611 -DLX=16 -DLY=1 -DLZ=1
g shared256 -DSHARED=256
g shared1024 -DSHARED=1024
g barrier -DBARRIER
g ls3211 -DLX=256 -DLY=1 -DLZ=1
g barrier32 -DBARRIER -DLX=256 -DLY=1 -DLZ=1
g sample -DSAMPLE
g numwg -DNUMWG
gr() { local n=$1; shift; glslangValidator -V --target-env vulkan1.1 -S comp "$@" "$V/regs.comp" -o "$O/$n.spv" >/dev/null; }
gr regs16 -DN=16
gr regs48 -DN=48
gr regs96 -DN=96
echo "variants in $O"
