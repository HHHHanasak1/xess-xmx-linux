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
# XeSS use DP4a) - unless another driver of the package (lib/mesa-<version>/) passes; that one is then activated.
# A Mesa version that differs from the system's is only reported (the patched driver is a complete driver).
# Result: ~/.cache/xess-xmx/driver-status and the journal (journalctl --user -u xess-xmx-check).
# Environment: XMX_CHECK_LIB / XMX_CHECK_ICD override the library / ICD file to test (tests only).
PKG="$(cd "$(dirname "$0")/.." && pwd)"
# the default driver (lib/libvulkan_intel.so only forwards to the one lib/active names)
LIB="${XMX_CHECK_LIB:-$PKG/lib/mesa-$(cat "$PKG/lib/active" 2>/dev/null)/libvulkan_intel.so}"
[ -f "$LIB" ] || LIB="$PKG/lib/libvulkan_intel.so"
ICD="${XMX_CHECK_ICD:-$PKG/run/intel_icd.json}"
STATUS="${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/driver-status"
mkdir -p "$(dirname "$STATUS")"

fail() { # <reason>
  # another driver of the package (lib/mesa-<version>/) may still work: use it instead of the stock drivers
  if [ "${DRY:-0}" != 1 ] && [ -z "${XMX_CHECK_LIB:-}" ]; then
    local d v
    for d in "$PKG"/lib/mesa-*/; do
      [ -f "$d/libvulkan_intel.so" ] || continue
      cmp -s "$d/libvulkan_intel.so" "$LIB" && continue
      v=$(basename "$d"); v=${v#mesa-}
      if bash "$PKG/tools/switch-driver.sh" "$v" >/dev/null 2>&1; then
        echo "patched ANV unusable ($1) - switched to the package's Mesa $v driver" >&2
        printf '%s (the previous driver failed: %s; switched to Mesa %s)\n' "$(head -1 "$STATUS")" "$1" "$v" > "$STATUS.new" &&
          mv -f "$STATUS.new" "$STATUS"
        exit 0
      fi
    done
  fi
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

# 0. lib/deps (the drivers' run path): copies of the host libraries that games under Proton cannot reach otherwise.
#    Inside the Steam Linux Runtime container the host's /usr is not visible; pressure-vessel brings in only the
#    libraries the host's own Vulkan drivers need, and the runtime has neither libSPIRV-Tools nor libdisplay-info. So a
#    system without another 64-bit Mesa Vulkan driver (radeon, nouveau) leaves the patched ANV unloadable in games, and
#    a system whose SPIRV-Tools has another name (libSPIRV-Tools-shared.so on Void Linux, libSPIRV-Tools-<version>.so
#    on openSUSE) has no libSPIRV-Tools.so at all. The driver uses only SPIRV-Tools' stable C API, so any of them does.
#    Refreshed every session, so a system update of either library reaches the copy.
hostlib() { # <soname, or an awk regex with -r> -> path of the host's 64-bit library
  local p d
  if [ "$1" = -r ]; then
    p=$({ /sbin/ldconfig -p || ldconfig -p; } 2>/dev/null | awk -v r="$2" '$1 ~ r && /x86-64/ { print $NF; exit }')
    [ -n "$p" ] && [ -f "$p" ] && echo "$p"
    return
  fi
  p=$({ /sbin/ldconfig -p || ldconfig -p; } 2>/dev/null | awk -v n="$1" '$1 == n && /x86-64/ { print $NF; exit }')
  [ -n "$p" ] && [ -f "$p" ] && { echo "$p"; return; }
  for d in /usr/lib64 /usr/lib/x86_64-linux-gnu /lib64 /lib/x86_64-linux-gnu /usr/lib; do
    [ -f "$d/$1" ] && { echo "$d/$1"; return; }
  done
}
mkdir -p "$PKG/lib/deps"
for n in $(cat "$PKG"/lib/mesa-*/libvulkan_intel.so 2>/dev/null | grep -aoE 'lib(SPIRV-Tools[A-Za-z-]*|display-info)\.so[.0-9]*' | sort -u); do
  src=$(hostlib "$n")
  [ -z "$src" ] && [ "$n" = libSPIRV-Tools.so ] && src=$(hostlib libSPIRV-Tools-shared.so)
  [ -z "$src" ] && [ "$n" = libSPIRV-Tools.so ] && src=$(hostlib -r '^libSPIRV-Tools-[0-9][^ ]*\.so$')
  [ -n "$src" ] || continue
  cmp -s "$src" "$PKG/lib/deps/$n" || { cp -L "$src" "$PKG/lib/deps/$n.new" && mv -f "$PKG/lib/deps/$n.new" "$PKG/lib/deps/$n"; }
done

# 1. load
err=$(python3 - "$LIB" 2>&1 <<'EOF'
import ctypes, sys
ctypes.CDLL(sys.argv[1], mode=ctypes.RTLD_LOCAL)   # CPython dlopens with RTLD_NOW: every symbol must resolve
EOF
)
rc=$?
[ $rc -eq 0 ] || { err=$(printf '%s' "$err" | tail -1); fail "${err:-cannot be loaded (exit $rc)}"; }

# 2. instance + device enumeration through the loader, with nothing but the patched driver
ours=""
if command -v vulkaninfo >/dev/null 2>&1; then
  out=$(MESA= VK_DRIVER_FILES="$ICD" VK_LOADER_LAYERS_DISABLE='*' timeout 20 vulkaninfo --summary 2>&1)
  ours=$(printf '%s\n' "$out" | grep -m1 "driverInfo" | sed 's/.*= *//')
  printf '%s\n' "$out" | grep -q "deviceName" || fail "vulkaninfo found no device ($(printf '%s' "$out" | grep -i -m1 "error" | cut -c1-120))"
fi

# 3. Mesa version against the system's (report only)
note=""
if [ -n "$ours" ] && command -v vulkaninfo >/dev/null 2>&1; then
  sys=$(VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json VK_LOADER_LAYERS_DISABLE='*' timeout 20 vulkaninfo --summary 2>/dev/null \
        | grep -m1 "driverInfo" | sed 's/.*= *//')
  v_ours=$(printf '%s' "$ours" | grep -o 'Mesa [0-9.]*'); v_sys=$(printf '%s' "$sys" | grep -o 'Mesa [0-9.]*')
  [ -n "$v_sys" ] && [ "$v_ours" != "$v_sys" ] && note=" (note: system has $v_sys, the patched driver is $v_ours)"
fi
# 4. per-game workarounds: a driver built with meson's default /usr/local prefix never reads /usr/share/drirc.d
#    (report only; e.g. Wuthering Waves with ray tracing hangs the GPU without them)
if [ -d /usr/share/drirc.d ] && ! grep -aq '/usr/share/drirc.d' "$LIB"; then
  note="$note (note: this driver does not read /usr/share/drirc.d, so Mesa's per-game workarounds are off - update the package or rebuild with tools/rebuild-driver.sh)"
fi
echo "ok $(date '+%F %T') $LIB ${ours}${note}" > "$STATUS"
echo "patched ANV loads and finds the GPU: ${ours}${note}"
exit 0
