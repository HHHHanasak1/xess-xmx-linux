#!/bin/bash
# enable.sh - per-game alternative to install.sh: switch one Proton game from XeSS DP4a to XeSS on XMX
# What it does (all reversible with disable.sh):
#   1. copies the shim igdext64.dll into the game's Proton prefix (the original is backed up as igdext64.dll.stock)
#   2. writes a marked block into a shell file that your launch wrapper sources before the game starts (the patched
#      Mesa ANV as the Intel Vulkan driver, the run-time kernel compiler); xmx-launch.sh is such a wrapper
# Settings (environment):
#   XMX_APPID   Steam app id of the game (or a non-Steam shortcut id)        } one of the two is required
#   XMX_PREFIX  Proton prefix (the folder that contains drive_c), any launcher }
#   XMX_CONF    shell file sourced by the launch wrapper (default ~/.config/xess-xmx.conf, the one xmx-launch.sh reads)
#   XMX_GAME    process name of the game; if set, enable/disable refuse to run while it is running
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
APPID="${XMX_APPID:-${WUWA_APPID:-}}"
PFXROOT="${XMX_PREFIX:-${WUWA_PREFIX:-}}"
[ -z "$PFXROOT" ] && [ -n "$APPID" ] && PFXROOT="$HOME/.local/share/Steam/steamapps/compatdata/$APPID/pfx"
[ -n "$PFXROOT" ] || { echo "Set XMX_APPID=<Steam app id> or XMX_PREFIX=<prefix folder>."; exit 1; }
PFX="$PFXROOT/drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1"
CONF="${XMX_CONF:-$HOME/.config/xess-xmx.conf}"
GAME="${XMX_GAME:-}"
[ -d "$PFXROOT/drive_c" ] || { echo "Proton prefix not found ($PFXROOT). Start the game once first."; exit 1; }
for f in "$ROOT/igdext64.dll" "$ROOT/lib/libvulkan_intel.so" "$ROOT/tools/cm-compile.sh"; do
  [ -f "$f" ] || { echo "missing $f - this is the source tree; download the release archive or build the binaries first (see README)"; exit 1; }
done
[ -x "$(ls "$ROOT"/igc/neo/bin/ocloc* 2>/dev/null | head -1)" ] || echo "note: no compiler bundle yet - run $ROOT/tools/get-igc.sh (kernels are compiled at run time)"
if [ -n "$GAME" ] && pgrep -i "^${GAME:0:15}" >/dev/null; then echo "The game is running - close it first."; exit 1; fi
mkdir -p "$PFX"

# 1. shim DLL
[ -f "$PFX/igdext64.dll.stock" ] || { [ -f "$PFX/igdext64.dll" ] && cp "$PFX/igdext64.dll" "$PFX/igdext64.dll.stock"; }
cp "$ROOT/igdext64.dll" "$PFX/igdext64.dll"
rm -f "$PFXROOT/drive_c/igdext_kernels/compile_failed.txt"

# 2. driver: ICD json with an absolute library path; every non-Intel driver stays visible
mkdir -p "$ROOT/run" "$ROOT/kernels"
cat > "$ROOT/run/intel_icd.json" <<EOF
{ "ICD": { "api_version": "1.4.348", "library_arch": "64", "library_path": "$ROOT/lib/libvulkan_intel.so" }, "file_format_version": "1.0.1" }
EOF
drivers="$ROOT/run/intel_icd.json"
for d in /usr/share/vulkan/icd.d /usr/local/share/vulkan/icd.d /etc/vulkan/icd.d "${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/icd.d"; do
  for j in "$d"/*.json; do
    [ -f "$j" ] || continue
    case "$(basename "$j")" in intel_icd.x86_64.json|intel_icd.json) continue ;; esac   # 32-bit Intel ICD stays (32-bit games)
    drivers="$drivers:$j"
  done
done
mkdir -p "$(dirname "$CONF")"; touch "$CONF"
sed -i '/# >>> XESS-XMX >>>/,/# <<< XESS-XMX <<</d' "$CONF"
cat >> "$CONF" <<EOF
# >>> XESS-XMX >>>
export VK_DRIVER_FILES=$drivers
export VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer
export ANV_CM_KERNEL_DIR=$ROOT/kernels
export ANV_CM_HELPER=$ROOT/tools/cm-compile.sh
# <<< XESS-XMX <<<
EOF

echo "XeSS XMX enabled for prefix $PFXROOT"
echo "These variables must be in the game's environment (written to $CONF; Steam launch options:"
echo "  $ROOT/xmx-launch.sh %command%   ):"
sed -n "/XESS-XMX >>>/,/XESS-XMX <<</p" "$CONF" | grep export
echo "In the game: enable XeSS (and XeSS Frame Generation if you like). Undo with: $ROOT/disable.sh"
