#!/bin/bash
# get-igc.sh [package dir]  - fetch the compiler the run-time kernel path needs into <package>/igc:
#   igc/igc  Intel Graphics Compiler 2.10.10 (igc-core + igc-opencl debs, unpacked; the version every shipped kernel
#            was built with - newer IGC releases fail on some XeSS kernels)
#   igc/neo  ocloc from intel-compute-runtime 25.18.1
# Only the files are unpacked (no installation); about 230 MB. Needs curl, ar (binutils) and tar.
set -eu
PKG="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
IGC_TAG="${IGC_TAG:-v2.10.10}"
NEO_TAG="${NEO_TAG:-25.18.33578.6}"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
fetch() { # <repo> <tag> <name regex> <exclude regex> <dest>
  local repo=$1 tag=$2 inc=$3 exc=$4 dest=$5 u
  mkdir -p "$dest"
  for u in $(curl -s "https://api.github.com/repos/$repo/releases/tags/$tag" | grep browser_download_url | grep -E "$inc" | grep -v -E "$exc" | cut -d\" -f4); do
    echo "fetching $(basename "$u")"
    curl -sL -o "$W/pkg.deb" "$u"
    (cd "$W" && ar x pkg.deb && tar -xf data.tar.* -C "$dest" && rm -f pkg.deb data.tar.* control.tar.* debian-binary)
  done
}
fetch intel/intel-graphics-compiler "$IGC_TAG" "igc-core-2_|igc-opencl-2_" "devel" "$W/igc"
fetch intel/compute-runtime "$NEO_TAG" "ocloc" "dbgsym|devel|sha|ddeb" "$W/neo"
mkdir -p "$PKG/igc"
rm -rf "$PKG/igc/igc" "$PKG/igc/neo"
mv "$W/igc/usr" "$PKG/igc/igc"
mv "$W/neo/usr" "$PKG/igc/neo"
ls "$PKG/igc/igc/local/lib" | head -3
ls "$PKG/igc/neo/bin"
echo "compiler installed under $PKG/igc"
