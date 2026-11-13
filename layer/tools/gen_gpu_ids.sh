#!/bin/sh
# gen_gpu_ids.sh <gpu-ids.txt> <out.inc> - the layer's GPU list (layer/gpu-ids.txt) as C initializers for src/gens.h.
# Fails on a line that is not "<xe2|xe3> 0x<hex id>" (comments and blank lines aside).
set -eu
awk '
  { sub(/\r$/, ""); sub(/#.*/, "") }
  NF == 0 { next }
  NF != 2 || ($1 != "xe2" && $1 != "xe3") || $2 !~ /^0x[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]$/ { printf "%s:%d: bad line\n", FILENAME, FNR > "/dev/stderr"; bad = 1; exit 1 }
  { printf "   { \"%s\", %s },\n", $1, tolower($2); n++ }
  END { if (bad || n == 0) exit 1 }
' "$1" > "$2.tmp"
mv -f "$2.tmp" "$2"
