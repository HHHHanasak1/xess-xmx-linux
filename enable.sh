#!/bin/bash
# enable.sh - switch a Proton game from XeSS DP4a to the XeSS XMX path (Intel Xe2/Xe3, patched Mesa ANV)
# What it does (all reversible with disable.sh):
#   1. copies the shim igdext64.dll into the game's Proton prefix (the original is backed up as igdext64.dll.stock)
#   2. appends a marked block to a launcher config file (sourced before the game starts) that selects the patched
#      Mesa ANV driver and the kernel set
#   3. clears the shader cache of the patched driver
# Settings (environment):
#   XMX_APPID   Steam app id (default 3513350 = Wuthering Waves; a non-Steam shortcut id works too)
#   XMX_PREFIX  Proton prefix (the folder that contains drive_c) for other launchers (Heroic, Lutris, Bottles ...)
#   XMX_CONF    shell file sourced by your launch wrapper (default ~/.config/wuwa-opti.conf); if you have no wrapper,
#               put the printed export lines into the game's launch options / environment yourself
#   XMX_GAME    process name of the game (matched against the first 15 characters, default Client-Win64-Shipping)
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
APPID="${XMX_APPID:-${WUWA_APPID:-3513350}}"
PFXROOT="${XMX_PREFIX:-${WUWA_PREFIX:-$HOME/.local/share/Steam/steamapps/compatdata/$APPID/pfx}}"
PFX="$PFXROOT/drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1"
CONF="${XMX_CONF:-$HOME/.config/wuwa-opti.conf}"
GAME="${XMX_GAME:-Client-Win64-Shipping}"
[ -d "$PFXROOT/drive_c" ] || { echo "Proton prefix not found ($PFXROOT). Start the game once first, or set XMX_APPID / XMX_PREFIX."; exit 1; }
for f in "$ROOT/igdext64.dll" "$ROOT/lib/libvulkan_intel.so"; do
  [ -f "$f" ] || { echo "missing $f - this is the source tree; download the release archive or build the binaries first (see README)"; exit 1; }
done
[ -n "$(ls "$ROOT/kernels"/*.cmk 2>/dev/null)" ] || { echo "kernels/ is empty - download the release archive or build the kernel set (see README)"; exit 1; }
mkdir -p "$PFX"
pgrep -i "^${GAME:0:15}" >/dev/null && { echo "The game is running - close it first."; exit 1; }

# 1. shim DLL
[ -f "$PFX/igdext64.dll.stock" ] || { [ -f "$PFX/igdext64.dll" ] && cp "$PFX/igdext64.dll" "$PFX/igdext64.dll.stock"; }
cp "$ROOT/igdext64.dll" "$PFX/igdext64.dll"

# 2. driver + kernels: ICD json with an absolute library path
mkdir -p "$ROOT/run"
cat > "$ROOT/run/intel_icd.json" <<EOF
{ "ICD": { "api_version": "1.4.348", "library_arch": "64", "library_path": "$ROOT/lib/libvulkan_intel.so" }, "file_format_version": "1.0.1" }
EOF
mkdir -p "$(dirname "$CONF")"; touch "$CONF"
sed -i '/# >>> XESS-XMX >>>/,/# <<< XESS-XMX <<</d' "$CONF"
cat >> "$CONF" <<EOF
# >>> XESS-XMX >>>
export VK_DRIVER_FILES=$ROOT/run/intel_icd.json
export VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer
export ANV_CM_KERNEL_DIR=$ROOT/kernels
export MESA_SHADER_CACHE_DIR=$HOME/.cache/xess-xmx-mesa
# <<< XESS-XMX <<<
EOF

# 3. cache
rm -rf "$HOME/.cache/xess-xmx-mesa"

echo "XeSS XMX enabled for prefix $PFXROOT"
echo "These variables must be in the game's environment (written to $CONF; source it from your launch wrapper):"
sed -n "/XESS-XMX >>>/,/XESS-XMX <<</p" "$CONF" | grep export
echo "In the game: enable XeSS (and XeSS Frame Generation if you like). Undo with: $ROOT/disable.sh"
