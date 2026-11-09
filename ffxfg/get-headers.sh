#!/bin/bash
# get-headers.sh - fetch the public headers ffx_xefg.cpp (and dx11bridge/) is built against into ffxfg/inc (not kept in git):
#   AMD FSR SDK 2.4.0 (GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, MIT): the FFX API, its upscaler and frame generation types
#   Intel XeSS SDK 3.0.2 headers (intel/xess): XeSS SR and XeLL (MIT), XeSS-FG (Intel Simplified Software License)
# Every file is checked against its SHA-256; nothing is written unless all of them match. Needs curl.
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
FFX=https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX
# the commit after the 3.0.2 tag that put the XeLL headers under MIT; same API as 3.0.2
XESS=https://raw.githubusercontent.com/intel/xess/de0fb9c1c510661c571164e1418ceca8101dab69/inc
FILES="
ffx/api/include/ffx_api.h 4d9dcf243ef5efbf03340a02f16b51a22053243a22e8a9a0b3ce91c26982b4bf
ffx/api/include/ffx_api_types.h 53f07c113a9ba493f297656e4481b6539aed0afc9f3cc8da682aa477d270c46c
ffx/api/include/dx12/ffx_api_dx12.h c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530
ffx/framegeneration/include/ffx_framegeneration.h bc78a18aa95f5157b4699b0602700592b624788c5bc55d1762cb46a1b6557ca1
ffx/framegeneration/include/ffx_framegeneration_api_types.h 396596af3df63310674e8036abd2ad0fcd061e61334e7a2c76653a2be3c686b4
ffx/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h 8ddb1cb78f83caf3598509df4968e84c851504cd6c00d9923261a5fc2c7ef0ed
ffx/upscalers/include/ffx_upscale.h 13091e7fe5fd9c82b1fb7c044b9f08f664d9653f599d2ea500738709236fca4d
xess/xess/xess.h d6f09f2dfa7050c09c679fbada2281cef421cdb14a51afd94efd9ac366d71677
xess/xess/xess_d3d12.h e740a3fc97e3e42faf7b3cbf23d64c5484a297e70523fc44b98433d31774fb98
xess/xess/xess_d3d11.h 9b9e716aa0d0abef278165e8b64049095ed25e0efbb6acd74815bbb1f9a9a1ca
xess/xess/xess_debug.h 6f4d847efe47e775bf3512fa1f924f6e750dd02372d42f81d48becb601a2e630
xess/xess/xess_vk.h 60d677e15444d92af78c6751c0d1e94deac27d92640894495ed82030bd7c2e72
xess/xess_fg/xefg_swapchain.h 39f20fc1eef0b5d3fd41e25a2c5c04734d1a2cbdcf3f3da1c86cefef181ce976
xess/xess_fg/xefg_swapchain_d3d12.h 1aa9a1c31214aaefb2c4ca4fd3bce29a166c8384148466d69f679ea554cf2dbc
xess/xell/xell.h d6d713a708b546a7189277923f28c9b7f746eb033f1ebc80ba76c9f57a93b398
xess/xell/xell_d3d12.h d948779f3cd9f0e31652377346017b9feffea5d1a64346149c048b218e2bd84b
"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
while read -r f sum; do
  [ -n "$f" ] || continue
  case "$f" in ffx/*) url="$FFX/${f#ffx/}" ;; xess/*) url="$XESS/${f#xess/}" ;; esac
  mkdir -p "$W/$(dirname "$f")"
  curl -sfL -o "$W/$f" "$url" || { echo "!! download failed: $url (nothing was changed)"; exit 1; }
  got=$(sha256sum "$W/$f" | cut -d' ' -f1)
  [ "$got" = "$sum" ] || { echo "!! checksum mismatch: $f $got (nothing was changed)"; exit 1; }
done <<< "$FILES"
rm -rf "$DIR/inc"
mv "$W" "$DIR/inc"
trap - EXIT
echo "headers in $DIR/inc"
