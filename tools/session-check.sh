#!/bin/bash
# session-check.sh [--dry-run] - run by the user unit xess-xmx-check.service before the graphical session starts.
# The patched ANV links against system libraries (libSPIRV-Tools.so, glibc, libstdc++). If a system update breaks one
# of them, the driver cannot be loaded and - because install.sh puts only our 64-bit Intel driver into VK_DRIVER_FILES -
# every 64-bit Vulkan program of the session, gamescope included, would lose the Intel GPU. This script loads the driver
# once (all symbols resolved) and, if that fails, points the session back to the stock drivers, so XeSS falls back to
# DP4a and everything else keeps working. Result: ~/.cache/xess-xmx/driver-status and the journal (journalctl --user -u xess-xmx-check).
# Environment: XMX_CHECK_LIB overrides the library to test (tests only).
PKG="$(cd "$(dirname "$0")/.." && pwd)"
LIB="${XMX_CHECK_LIB:-$PKG/lib/libvulkan_intel.so}"
STATUS="${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/driver-status"
mkdir -p "$(dirname "$STATUS")"
err=$(python3 - "$LIB" 2>&1 <<'EOF'
import ctypes, sys
ctypes.CDLL(sys.argv[1], mode=ctypes.RTLD_LOCAL)   # CPython dlopens with RTLD_NOW: every symbol must resolve
EOF
)
rc=$?
err=$(printf "%s" "$err" | tail -1)
if [ $rc -eq 0 ]; then
  echo "ok $(date '+%F %T') $LIB" > "$STATUS"
  echo "patched ANV loads fine: $LIB"
  exit 0
fi
echo "FAILED $(date '+%F %T') $LIB: $err" > "$STATUS"
echo "patched ANV cannot be loaded ($err) - using the stock Intel driver for this session (XeSS runs as DP4a)" >&2
echo "fix: update the package (new release built for this system) or rebuild the driver; see docs/debugging.md" >&2
[ "${1:-}" = --dry-run ] && exit 1
# unset-environment cannot remove variables that come from environment.d, so they are overwritten instead: the system's
# own driver list (stock Intel driver included) and no disabled vkd3d-proton extension, i.e. stock behaviour
stock=""
for d in /usr/share/vulkan/icd.d /usr/local/share/vulkan/icd.d /etc/vulkan/icd.d; do
  for j in "$d"/*.json; do [ -f "$j" ] && stock="${stock:+$stock:}$j"; done
done
systemctl --user set-environment "VK_DRIVER_FILES=$stock" "VKD3D_DISABLE_EXTENSIONS=" "ANV_CM_KERNEL_DIR=" "ANV_CM_HELPER="
exit 0
