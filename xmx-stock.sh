#!/bin/bash
# xmx-stock.sh - Steam launch wrapper (experimental): this game runs on the system's own, unpatched Mesa ANV with the
# package's Vulkan layer VK_LAYER_XESS_xmx (layer/), which puts XeSS' XMX kernels in from outside the driver, instead
# of on the package's patched driver. Steam launch options (variables may come before it):
#   /path/to/xess-xmx-linux/xmx-stock.sh %command%
# It sets everything before Steam's runtime container (pressure-vessel) starts: inside the container the driver list
# has already been rewritten, and a list changed there loaded two Intel drivers (the container's copy of the patched
# one and the stock one). Nothing is installed; the shim must be in the game's prefix (install.sh or enable.sh).
#   VK_DRIVER_FILES   the system's Intel ICDs (64-bit first, then 32-bit), then every other entry of the session's list
#                     (install.sh's environment: other GPUs' drivers) without any ICD of this package; no session list:
#                     every ICD of the system
#   VK_ADD_LAYER_PATH, VK_LOADER_LAYERS_ENABLE   the layer, in front of / added to the values already set
#   XMX_LAYER_HELPER, XMX_IGC_DIR   the compile script and the compiler bundle; a value set before the wrapper is kept
#   VKD3D_DISABLE_EXTENSIONS   VK_EXT_descriptor_buffer and VK_KHR_present_wait2 added if missing (the layer needs
#                     vkd3d-proton's classic descriptor heap; present wait 2 crashed games with frame generation)
#   force_vk_vendor, INTC_ALT_DRIVER_EXTENSIONS_PATH   install.sh's values, unless already set
#   ANV_CM_KERNEL_DIR, ANV_CM_HELPER   left as they are: only the patched driver reads them, and the layer prefers
#                     XMX_LAYER_HELPER
# XMX_STOCK_ICD=<json>[:<json>]: the Intel ICD(s) to use instead of the system's (e.g. a Mesa built by hand).
# If the layer or a system Intel ICD is missing, the game starts unchanged (one line on stderr says why).
PKG="$(cd "$(dirname "$0")" && pwd)"
LAYER="$PKG/layer"
say() { echo "xess-xmx: xmx-stock: $*" >&2; }
# the loader's ICD folders (XMX_ICD_DIRS, a colon list, replaces them: tests only)
ICDDIRS=(/usr/share/vulkan/icd.d /usr/local/share/vulkan/icd.d /etc/vulkan/icd.d "${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/icd.d")
[ -n "${XMX_ICD_DIRS:-}" ] && IFS=: read -r -a ICDDIRS <<< "$XMX_ICD_DIRS"

for f in "$LAYER/libVkLayer_xess_xmx.so" "$LAYER/VkLayer_xess_xmx.json"; do
  [ -f "$f" ] || { say "$f missing - the game starts unchanged"; exec "$@"; }
done

# the system's Intel ANV ICDs: 64-bit first (the one a game picks), then the 32-bit one
stock=()
if [ -n "${XMX_STOCK_ICD:-}" ]; then
  IFS=: read -r -a stock <<< "$XMX_STOCK_ICD"
else
  for n in intel_icd.x86_64.json intel_icd.json intel_icd.i686.json intel_icd.i386.json; do
    for d in "${ICDDIRS[@]}"; do
      [ -f "$d/$n" ] && { stock+=("$d/$n"); break; }
    done
  done
fi
have64=0
for j in "${stock[@]}"; do
  [ -f "$j" ] || { say "$j missing - the game starts unchanged"; exec "$@"; }
  case "${j##*/}" in *i686*|*i386*) ;; *) have64=1 ;; esac
done
[ "$have64" = 1 ] || { say "no 64-bit Intel Vulkan ICD of the system (intel_icd.x86_64.json in ${ICDDIRS[*]}) - the game starts unchanged"; exec "$@"; }

ours() { # <icd json>: an ICD of this package (any copy of it), which must not be in the list
  local lib
  [ "$1" = "$PKG/run/intel_icd.json" ] && return 0
  lib=$(sed -n 's/.*"library_path"[^"]*"\([^"]*\)".*/\1/p' "$1" 2>/dev/null | head -1)
  case "$lib" in "$PKG"/*) return 0 ;; esac
  # the package's forwarding ICD (icd/xmx-icd.c), also from a package in another folder
  [ -f "$lib" ] && grep -aq 'xess-xmx: using the Mesa' "$lib"
}
list=("${stock[@]}")
add() { # <icd json>: appended unless already there or one of the package's
  local e
  [ -n "$1" ] || return
  for e in "${list[@]}"; do [ "$e" = "$1" ] && return; done
  ours "$1" && return
  list+=("$1")
}
session="${VK_DRIVER_FILES:-${VK_ICD_FILENAMES:-}}"
if [ -n "$session" ]; then
  IFS=: read -r -a entries <<< "$session"
  for j in "${entries[@]}"; do add "$j"; done
else
  for d in "${ICDDIRS[@]}"; do
    for j in "$d"/*.json; do [ -f "$j" ] && add "$j"; done
  done
fi
VK_DRIVER_FILES=$(IFS=:; printf '%s' "${list[*]}")
export VK_DRIVER_FILES
unset VK_ICD_FILENAMES   # the older name; VK_DRIVER_FILES takes precedence anyway

export VK_ADD_LAYER_PATH="$LAYER${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
case ",${VK_LOADER_LAYERS_ENABLE:-}," in
  *,VK_LAYER_XESS_xmx,*) ;;
  *) export VK_LOADER_LAYERS_ENABLE="${VK_LOADER_LAYERS_ENABLE:+$VK_LOADER_LAYERS_ENABLE,}VK_LAYER_XESS_xmx" ;;
esac
export XMX_LAYER_HELPER="${XMX_LAYER_HELPER:-$PKG/tools/cm-compile.sh}"
export XMX_IGC_DIR="${XMX_IGC_DIR:-$PKG/igc}"
ext="${VKD3D_DISABLE_EXTENSIONS:-}"
for e in VK_EXT_descriptor_buffer VK_KHR_present_wait2; do
  case ",$ext," in *",$e,"*) ;; *) ext="${ext:+$ext,}$e" ;; esac
done
export VKD3D_DISABLE_EXTENSIONS="$ext"
export force_vk_vendor="${force_vk_vendor:-0}"
export INTC_ALT_DRIVER_EXTENSIONS_PATH="${INTC_ALT_DRIVER_EXTENSIONS_PATH:-C:\\windows\\system32\\driverstore\\filerepository\\igd_faux.inf_1}"

note=" (no compiler bundle in $XMX_IGC_DIR: kernels cannot be compiled until tools/get-igc.sh has run)"
for f in "$XMX_IGC_DIR"/neo/bin/ocloc*; do [ -x "$f" ] && { note=""; break; }; done
say "driver ${stock[0]}${XMX_STOCK_ICD:+ (XMX_STOCK_ICD)}, layer VK_LAYER_XESS_xmx from $LAYER$note"
exec "$@"
