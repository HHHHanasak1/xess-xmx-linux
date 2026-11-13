#!/bin/bash
# enable.sh - per-game alternative to install.sh: switch one Proton game from XeSS DP4a to XeSS on XMX
# What it does (all reversible with disable.sh):
#   1. copies the shim igdext64.dll into the game's Proton prefix (the original is backed up as igdext64.dll.stock)
#      and ControlLib.dll (the stand-in for Intel's Control Library, for XESS_FG) into its system32
#   2. writes a marked block into a shell file that your launch wrapper sources before the game starts (the patched
#      Mesa ANV as the Intel Vulkan driver, the run-time kernel compiler); xmx-launch.sh is such a wrapper
#   3. runs the driver check, which also copies the host libraries the Steam Linux Runtime lacks into lib/deps
# Settings (environment):
#   XMX_APPID   Steam app id of the game (or a non-Steam shortcut id)        } one of the two is required
#   XMX_PREFIX  Proton prefix (the folder that contains drive_c), any launcher }
#   XMX_CONF    shell file sourced by the launch wrapper (default ~/.config/xess-xmx.conf, the one xmx-launch.sh reads)
#   XMX_GAME    process name of the game; if set, enable/disable refuse to run while it is running
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
APPID="${XMX_APPID:-}"
PFXROOT="${XMX_PREFIX:-}"
. "$ROOT/tools/steam-libraries.sh"
# the prefix in whichever Steam library holds the game (SD card, second disk)
[ -z "$PFXROOT" ] && [ -n "$APPID" ] && PFXROOT="$(steam_prefix_of "$APPID" || printf '%s' "$(steam_root)/steamapps/compatdata/$APPID/pfx")"
[ -n "$PFXROOT" ] || { echo "Set XMX_APPID=<Steam app id> or XMX_PREFIX=<prefix folder>."; exit 1; }
CONF="${XMX_CONF:-$HOME/.config/xess-xmx.conf}"
GAME="${XMX_GAME:-}"
[ -d "$PFXROOT/drive_c" ] || { echo "Proton prefix not found ($PFXROOT). Start the game once first."; exit 1; }
for f in "$ROOT/igdext64.dll" "$ROOT/lib/libvulkan_intel.so" "$ROOT/tools/cm-compile.sh"; do
  [ -f "$f" ] || { echo "missing $f - this is the source tree; download the release archive or build the binaries first (see README)"; exit 1; }
done
xmx_have_compiler "$ROOT" || echo "note: no compiler bundle yet - run $ROOT/tools/get-igc.sh (kernels are compiled at run time)"
if [ -n "$GAME" ] && pgrep -i "^${GAME:0:15}" >/dev/null; then echo "The game is running - close it first."; exit 1; fi
# the driver store folder in every case it exists in (proton-cachyos: "DriverStore" next to "driverstore"), else new
DIRS="$(prefix_shim_dirs "$PFXROOT")"
[ -n "$DIRS" ] || { DIRS="$PFXROOT/drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1"; mkdir -p "$DIRS"; }

# 1. shim DLL
while IFS= read -r PFX; do
  [ -f "$PFX/igdext64.dll.stock" ] || { [ -f "$PFX/igdext64.dll" ] && cp "$PFX/igdext64.dll" "$PFX/igdext64.dll.stock"; }
  cp "$ROOT/igdext64.dll" "$PFX/igdext64.dll"
done <<< "$DIRS"
# ControlLib.dll: the stand-in for Intel's Control Library, for XESS_FG (frame generation override)
S32="$PFXROOT/drive_c/windows/system32"
if [ -f "$ROOT/ControlLib.dll" ] && { [ ! -f "$S32/ControlLib.dll" ] || xmx_is_controllib "$S32/ControlLib.dll"; }; then
  cp "$ROOT/ControlLib.dll" "$S32/ControlLib.dll"
fi
rm -f "$PFXROOT/drive_c/igdext_kernels/compile_failed.txt"

# 2. driver: ICD json with an absolute library path; every non-Intel driver stays visible
mkdir -p "$ROOT/run" "$ROOT/kernels"
xmx_icd_json "$ROOT/lib/libvulkan_intel.so" > "$ROOT/run/intel_icd.json"
drivers=$(xmx_driver_files "$ROOT/run/intel_icd.json")
mkdir -p "$(dirname "$CONF")"; touch "$CONF"
sed -i '/# >>> XESS-XMX >>>/,/# <<< XESS-XMX <<</d' "$CONF"
# the paths are quoted for the shell that sources the file (a package folder with spaces or quotes in its name)
{
  echo "# >>> XESS-XMX >>>"
  printf 'export VK_DRIVER_FILES=%q\n' "$drivers"
  echo "export VKD3D_DISABLE_EXTENSIONS=$XMX_VKD3D_OFF"
  printf 'export ANV_CM_KERNEL_DIR=%q\n' "$ROOT/kernels"
  printf 'export ANV_CM_HELPER=%q\n' "$ROOT/tools/cm-compile.sh"
  echo "export force_vk_vendor=0"
  echo "export INTC_ALT_DRIVER_EXTENSIONS_PATH='$XMX_ALT_EXT'"
  echo "# <<< XESS-XMX <<<"
} >> "$CONF"

# 3. the driver check; it also copies the host libraries the Steam Linux Runtime lacks into lib/deps
bash "$ROOT/tools/session-check.sh" --dry-run >/dev/null 2>&1 && echo "driver check: loads fine" \
  || echo "!! the patched driver does not load on this system - see ~/.cache/xess-xmx/driver-status"

echo "XeSS XMX enabled for prefix $PFXROOT"
echo "These variables must be in the game's environment (written to $CONF; Steam launch options:"
echo "  $ROOT/xmx-launch.sh %command%   ):"
sed -n "/XESS-XMX >>>/,/XESS-XMX <<</p" "$CONF" | grep export
echo "In the game: enable XeSS (and XeSS Frame Generation if you like). Undo with: $ROOT/disable.sh"
