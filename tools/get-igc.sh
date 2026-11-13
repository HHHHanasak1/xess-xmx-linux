#!/bin/bash
# get-igc.sh [package dir]  - fetch the compiler the run-time kernel path needs into <package>/igc:
#   igc/igc  Intel Graphics Compiler 2.10.10 (the igc-core deb, unpacked; the version every shipped kernel
#            was built with - newer IGC releases fail on XeSS kernels: IGC 2.41.5 with compute-runtime 26.35, its
#            pair, rejected 128 of 150 from five games, "spirv_read_verify: verify Module failed")
#   igc/neo  ocloc from intel-compute-runtime 25.18.1
# Only the files are unpacked (no installation); about 95 MB. The igc-opencl package is not needed: SPIR-V input never uses it. Needs curl, ar (binutils) and tar.
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
    curl -sfL -o "$W/pkg.deb" "$u" || { echo "get-igc: download failed: $u (nothing was changed)"; exit 1; }
    # the default versions are pinned; another IGC_TAG / NEO_TAG is the caller's choice and has no sum here
    want=""
    case "$(basename "$u")" in
      intel-igc-core-2_2.10.10*) want=ec3b8e6bb3d6371d3b9963c32ddb1d5315e6b0c3f4ae1d91a7515b5a8907ffe6 ;;
      intel-ocloc_25.18.33578.6*) want=c772db8891539890237d50c357af1480b22cbd859c0b8afb5355f605a818d758 ;;
    esac
    if [ -n "$want" ] && [ "$(sha256sum "$W/pkg.deb" | cut -d' ' -f1)" != "$want" ]; then
      echo "get-igc: checksum mismatch for $(basename "$u") (nothing was changed)"; exit 1
    fi
    (cd "$W" && ar x pkg.deb && tar -xf data.tar.* -C "$dest" && rm -f pkg.deb data.tar.* control.tar.* debian-binary)
  done
}
fetch intel/intel-graphics-compiler "$IGC_TAG" "igc-core-2_" "devel" "$W/igc"
fetch intel/compute-runtime "$NEO_TAG" "ocloc" "dbgsym|devel|sha|ddeb" "$W/neo"
# verify the download before touching the installed compiler (an empty GitHub API answer - offline, rate limited -
# makes fetch() do nothing without an error)
[ -d "$W/igc/usr" ] && [ -n "$(find "$W/igc/usr" -name 'libigc.so*' -print -quit)" ] || { echo "get-igc: IGC $IGC_TAG not downloaded (offline, or GitHub API rate limit); the installed compiler is unchanged" >&2; exit 1; }
[ -d "$W/neo/usr" ] && [ -n "$(find "$W/neo/usr" -name 'ocloc*' -print -quit)" ] || { echo "get-igc: ocloc $NEO_TAG not downloaded; the installed compiler is unchanged" >&2; exit 1; }
mkdir -p "$PKG/igc"
rm -rf "$PKG/igc/igc" "$PKG/igc/neo"
mv "$W/igc/usr" "$PKG/igc/igc"
mv "$W/neo/usr" "$PKG/igc/neo"
ls "$PKG/igc/igc/local/lib" | head -3
ls "$PKG/igc/neo/bin"
echo "compiler installed under $PKG/igc"
