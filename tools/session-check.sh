#!/bin/bash
# session-check.sh [--dry-run] - run by the user unit xess-xmx-check.service before the graphical session starts.
# The patched ANV links against system libraries (libSPIRV-Tools.so, glibc, libstdc++) and talks to the kernel driver.
# If a system update breaks one of them, the driver cannot be loaded or cannot create a device, and - because install.sh
# puts only our 64-bit Intel driver into VK_DRIVER_FILES - every 64-bit Vulkan program of the session, gamescope
# included, would lose the Intel GPU. This script
#   1. loads the driver once (every symbol resolved),
#   2. lets vulkaninfo create an instance and enumerate the GPU with only the patched driver,
# (plus two reports that change nothing: Mesa version against the system's, and whether the driver reads the system's
# per-game workarounds in /usr/share/drirc.d) and if either of the first two fails points the session back to the stock drivers (games still run; the shim's self-test then makes
# XeSS use DP4a). A Mesa version that differs from the system's is only reported (the patched driver is a complete
# driver; rebuild it with tools/rebuild-driver.sh when convenient).
# Result: ~/.cache/xess-xmx/driver-status and the journal (journalctl --user -u xess-xmx-check).
# Environment: XMX_CHECK_LIB / XMX_CHECK_ICD override the library / ICD file to test (tests only).
PKG="$(cd "$(dirname "$0")/.." && pwd)"
LIB="${XMX_CHECK_LIB:-$PKG/lib/libvulkan_intel.so}"
ICD="${XMX_CHECK_ICD:-$PKG/run/intel_icd.json}"
STATUS="${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/driver-status"
mkdir -p "$(dirname "$STATUS")"

fail() { # <reason>
  echo "FAILED $(date '+%F %T') $LIB: $1" > "$STATUS"
  echo "patched ANV unusable ($1) - using the stock Intel driver for this session (XeSS runs as DP4a)" >&2
  echo "fix: update the package or rebuild the driver (tools/rebuild-driver.sh); see docs/debugging.md" >&2
  [ "${DRY:-0}" = 1 ] && exit 1
  # unset-environment cannot remove variables that come from environment.d, so they are overwritten instead: the
  # system's own driver list (stock Intel driver included) and no disabled vkd3d-proton extension, i.e. stock behaviour
  local stock="" d j
  for d in /usr/share/vulkan/icd.d /usr/local/share/vulkan/icd.d /etc/vulkan/icd.d; do
    for j in "$d"/*.json; do [ -f "$j" ] && stock="${stock:+$stock:}$j"; done
  done
  systemctl --user set-environment "VK_DRIVER_FILES=$stock" "VKD3D_DISABLE_EXTENSIONS=" "ANV_CM_KERNEL_DIR=" "ANV_CM_HELPER="
  exit 0
}
[ "${1:-}" = --dry-run ] && DRY=1

# 1. load
err=$(python3 - "$LIB" 2>&1 <<'EOF'
import ctypes, sys
ctypes.CDLL(sys.argv[1], mode=ctypes.RTLD_LOCAL)   # CPython dlopens with RTLD_NOW: every symbol must resolve
EOF
)
rc=$?
[ $rc -eq 0 ] || fail "$(printf '%s' "$err" | tail -1)"

# 2. instance + device enumeration through the loader, with nothing but the patched driver
ours=""
if command -v vulkaninfo >/dev/null 2>&1; then
  out=$(VK_DRIVER_FILES="$ICD" VK_LOADER_LAYERS_DISABLE='*' timeout 20 vulkaninfo --summary 2>&1)
  ours=$(printf '%s\n' "$out" | grep -m1 "driverInfo" | sed 's/.*= *//')
  printf '%s\n' "$out" | grep -q "deviceName" || fail "vulkaninfo found no device ($(printf '%s' "$out" | grep -i -m1 "error" | cut -c1-120))"
fi

# 3. Mesa version against the system's (report only)
note=""
if [ -n "$ours" ] && command -v vulkaninfo >/dev/null 2>&1; then
  sys=$(VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json VK_LOADER_LAYERS_DISABLE='*' timeout 20 vulkaninfo --summary 2>/dev/null \
        | grep -m1 "driverInfo" | sed 's/.*= *//')
  v_ours=$(printf '%s' "$ours" | grep -o 'Mesa [0-9.]*'); v_sys=$(printf '%s' "$sys" | grep -o 'Mesa [0-9.]*')
  [ -n "$v_sys" ] && [ "$v_ours" != "$v_sys" ] && note=" (note: system has $v_sys, the patched driver is $v_ours - rebuild with tools/rebuild-driver.sh)"
fi
# 4. per-game workarounds: a driver built with meson's default /usr/local prefix never reads /usr/share/drirc.d
#    (report only; e.g. Wuthering Waves with ray tracing hangs the GPU without them)
if [ -d /usr/share/drirc.d ] && ! grep -aq '/usr/share/drirc.d' "$LIB"; then
  note="$note (note: this driver does not read /usr/share/drirc.d, so Mesa's per-game workarounds are off - update the package or rebuild with tools/rebuild-driver.sh)"
fi
echo "ok $(date '+%F %T') $LIB ${ours}${note}" > "$STATUS"
echo "patched ANV loads and finds the GPU: ${ours}${note}"
exit 0
