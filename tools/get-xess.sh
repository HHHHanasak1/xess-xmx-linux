#!/bin/bash
# get-xess.sh [package dir]  - fetch Intel's XeSS SDK release from github.com/intel/xess into <package>/xess: the four
# runtime libraries (libxess.dll, libxess_dx11.dll, libxess_fg.dll, libxell.dll) and Intel's license. One copy for all
# games; tools/proton-hook.py puts them into a game's folder while it runs when its launch options ask for it (XESS=latest,
# see docs/launch-options.md). SDK 3.0.2: XeSS 2.0.2.68, XeSS-FG 1.3.1.78, XeLL 1.3.2.10. About 77 MB download.
# Needs curl and unzip (or python3). XESS_TAG / XESS_SHA256 select another release.
set -eu
PKG="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
XESS_TAG="${XESS_TAG:-v3.0.2}"
XESS_SHA256="${XESS_SHA256:-88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990}"
LIBS="libxess.dll libxess_dx11.dll libxess_fg.dll libxell.dll"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
url="https://github.com/intel/xess/releases/download/$XESS_TAG/XeSS_SDK_${XESS_TAG#v}.zip"
echo "fetching $url"
curl -sfL -o "$W/sdk.zip" "$url" || { echo "!! download failed (nothing was changed)"; exit 1; }
sum=$(sha256sum "$W/sdk.zip" | cut -d' ' -f1)
[ "$sum" = "$XESS_SHA256" ] || { echo "!! checksum mismatch: $sum (nothing was changed)"; exit 1; }
mkdir -p "$W/x"
if command -v unzip >/dev/null; then
  unzip -q -o "$W/sdk.zip" $(for l in $LIBS; do echo "bin/$l"; done) LICENSE.txt third-party-programs.txt -d "$W/x"
else
  python3 - "$W/sdk.zip" "$W/x" $LIBS <<'EOF'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for n in ["bin/" + l for l in sys.argv[3:]] + ["LICENSE.txt", "third-party-programs.txt"]:
    z.extract(n, sys.argv[2])
EOF
fi
for l in $LIBS; do [ -s "$W/x/bin/$l" ] || { echo "!! $l missing in the SDK (nothing was changed)"; exit 1; }; done
# replaced as a whole: a game that runs right now keeps the files it has open
rm -rf "$PKG/xess.new"; mkdir -p "$PKG/xess.new"
mv "$W"/x/bin/*.dll "$W/x/LICENSE.txt" "$W/x/third-party-programs.txt" "$PKG/xess.new/"
chmod 644 "$PKG"/xess.new/*
echo "$XESS_TAG" > "$PKG/xess.new/VERSION"
rm -rf "$PKG/xess"; mv "$PKG/xess.new" "$PKG/xess"
echo "XeSS SDK $XESS_TAG installed under $PKG/xess (Intel Simplified Software License: $PKG/xess/LICENSE.txt)"
