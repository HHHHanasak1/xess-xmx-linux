#!/bin/bash
# get-ffx.sh [package dir]  - fetch AMD's signed FidelityFX loader, upscaler and frame generation DLLs (FSR SDK 2.4.0,
# github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, Kits/FidelityFX/signedbin) and AMD's license into <package>/ffx.
# Games with FSR 3.1.0 to 3.1.3 ship everything in one amd_fidelityfx_dx12.dll: with FSR=xess or FSR_FG=xess the loader
# takes that name and an upscaler and a frame generation DLL go next to it (the package's ones where asked for, AMD's
# otherwise). About 87 MB download. Needs curl. AMD's license allows redistribution in binary form only, with its notice
# (ffx/license.md); the files are not part of the repository or the release archive.
set -eu
PKG="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
FFX_TAG=v2.4.0
BASE="https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/$FFX_TAG/Kits/FidelityFX"
FILES="
signedbin/amd_fidelityfx_loader_dx12.dll e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb
signedbin/amd_fidelityfx_upscaler_dx12.dll aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace
signedbin/amd_fidelityfx_framegeneration_dx12.dll e6843252f58b0341a0d80f6a20bc0d840643ee95bc48c22cac08745344577bb6
docs/license.md 32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb
"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
while read -r path sha; do
  [ -n "$path" ] || continue
  f="$W/$(basename "$path")"
  echo "fetching $BASE/$path"
  curl -sfL -o "$f" "$BASE/$path" || { echo "!! download failed (nothing was changed)"; exit 1; }
  sum=$(sha256sum "$f" | cut -d' ' -f1)
  [ "$sum" = "$sha" ] || { echo "!! checksum mismatch for $path: $sum (nothing was changed)"; exit 1; }
done <<< "$FILES"
# replaced as a whole: a game that runs right now keeps the files it has open
rm -rf "$PKG/ffx.new"; mkdir -p "$PKG/ffx.new"
mv "$W"/* "$PKG/ffx.new/"
chmod 644 "$PKG"/ffx.new/*
echo "FSR SDK $FFX_TAG" > "$PKG/ffx.new/VERSION"
rm -rf "$PKG/ffx"; mv "$PKG/ffx.new" "$PKG/ffx"
echo "FidelityFX loader, upscaler and frame generation ($FFX_TAG) installed under $PKG/ffx (AMD's license: $PKG/ffx/license.md)"
