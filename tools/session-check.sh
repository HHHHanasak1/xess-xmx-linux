#!/bin/bash
# session-check.sh [--dry-run] - run by the user unit xess-xmx-check.service before the graphical session starts.
# The patched ANV links against system libraries (libSPIRV-Tools.so, glibc, libstdc++) and talks to the kernel driver.
# If a system update breaks one of them, the driver cannot be loaded or cannot create a device, and - because install.sh
# puts only our 64-bit Intel driver into VK_DRIVER_FILES - every 64-bit Vulkan program of the session, gamescope
# included, would lose the Intel GPU. This script
#   1. checks that the ICD file (run/intel_icd.json) and the library it names exist (if not: stock drivers right away),
#      then loads the driver once (every symbol resolved),
#   2. lets vulkaninfo create an instance and enumerate the GPU with only the patched driver,
# (plus two reports that change nothing: Mesa version against the system's, and whether the driver reads the system's
# per-game workarounds in /usr/share/drirc.d) and if either of the first two fails points the session back to the stock drivers (games still run; the shim's self-test then makes
# XeSS use DP4a) - unless another driver of the package (lib/mesa-<version>/) passes; that one is then activated.
# A Mesa version that differs from the system's is only reported (the patched driver is a complete driver).
# Result: ~/.cache/xess-xmx/driver-status and the journal (journalctl --user -u xess-xmx-check).
# The unit starts it through ~/.config/xess-xmx/session-check.sh (install.sh), which falls back to the stock drivers
# by itself when the package folder is gone.
# The two loading checks run at every login. What only depends on a file's content (the drivers' library names, the
# drirc report, the system's Mesa version) is kept in ~/.cache/xess-xmx/ until that file changes.
# Environment: XMX_CHECK_LIB / XMX_CHECK_ICD override the library / ICD file to test (tests only).
PKG="$(cd "$(dirname "$0")/.." && pwd)"
# the default driver (lib/libvulkan_intel.so only forwards to the one lib/active names)
LIB="${XMX_CHECK_LIB:-$PKG/lib/mesa-$(cat "$PKG/lib/active" 2>/dev/null)/libvulkan_intel.so}"
# stock mode (install.sh's default on Xe3 and Xe2, or XMX_DRIVER=stock): the session's MESA=stock-<version> picks the
# package's unpatched driver
STOCK=0
CHECK_MESA=""
if [ "$(cat "$PKG/run/session-driver" 2>/dev/null)" = stock ] && [ -z "${XMX_CHECK_LIB:-}" ]; then
  STOCK=1
  CHECK_MESA="${MESA:-}"
  case "$CHECK_MESA" in
    stock-*) ;;
    *) # install.sh's dry run: not in the session yet, the newest one (what install.sh writes)
       CHECK_MESA=$(cd "$PKG/lib" && ls -d mesa-stock-*/ 2>/dev/null | sed 's|^mesa-||; s|/$||' | sort -V | tail -1) ;;
  esac
  LIB="$PKG/lib/mesa-$CHECK_MESA/libvulkan_intel.so"
fi
[ -f "$LIB" ] || LIB="$PKG/lib/libvulkan_intel.so"
ICD="${XMX_CHECK_ICD:-$PKG/run/intel_icd.json}"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx"
STATUS="$CACHE/driver-status"
mkdir -p "$CACHE"

fail() { # <reason> [stock]
  # another driver of the package (lib/mesa-<version>/) may still work: use it instead of the stock drivers (not with
  # "stock": the ICD file the session names is broken, which another driver would not mend)
  # (not in stock mode either: MESA=stock-<version> overrides lib/active, and the patched drivers do not go with the layer)
  if [ "${DRY:-0}" != 1 ] && [ -z "${XMX_CHECK_LIB:-}" ] && [ "${2:-}" != stock ] && [ "$STOCK" = 0 ]; then
    local d v
    for d in "$PKG"/lib/mesa-*/; do
      case "$d" in */mesa-stock-*/) continue ;; esac   # unpatched: no XMX without the layer (stock mode only)
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
  echo "the package's ANV unusable ($1) - using the system's stock Intel driver for this session (XeSS runs as DP4a)" >&2
  echo "fix: update the package or rebuild the driver (tools/rebuild-driver.sh); see docs/troubleshooting.md" >&2
  [ "${DRY:-0}" = 1 ] && exit 1
  # unset-environment cannot remove variables that come from environment.d, so they are overwritten instead: the
  # system's own driver list (stock Intel driver included) and no disabled vkd3d-proton extension, i.e. stock behaviour
  local stock="" d j
  for d in /usr/share/vulkan/icd.d /usr/local/share/vulkan/icd.d /etc/vulkan/icd.d "${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/icd.d"; do
    for j in "$d"/*.json; do [ -f "$j" ] && stock="${stock:+$stock:}$j"; done
  done
  systemctl --user set-environment "VK_DRIVER_FILES=$stock" "VKD3D_DISABLE_EXTENSIONS=" "ANV_CM_KERNEL_DIR=" "ANV_CM_HELPER="
  [ "$STOCK" = 1 ] && systemctl --user set-environment "MESA=" "VK_ADD_LAYER_PATH=" "VK_LOADER_LAYERS_ENABLE="
  exit 0
}
[ "${1:-}" = --dry-run ] && DRY=1

cached() { # <name> <file> <command...>: the command's output, kept in CACHE/<name> for as long as <file> keeps its
  # size, modification time and inode (a package unpacked again or an OS image changes those, a newer mtime alone
  # would miss a downgrade). The output goes to stdout either way
  local c="$CACHE/$1" key out lines=()
  key=$(stat -c '%s %.Y %i' "$2" 2>/dev/null)
  shift 2
  if [ -n "$key" ]; then
    mapfile -t lines 2>/dev/null < "$c"
    if [ "${lines[0]:-}" = "$key" ]; then
      [ ${#lines[@]} -gt 1 ] && printf '%s\n' "${lines[@]:1}"
      return 0
    fi
  fi
  out=$("$@")
  # nothing is not kept: vulkaninfo may have failed this once (a timeout), and the next login should ask again
  [ -n "$key" ] && [ -n "$out" ] && printf '%s\n' "$key" "$out" > "$c.new" && mv -f "$c.new" "$c"
  printf '%s\n' "$out"
}

# 0. lib/deps (the drivers' run path): copies of the host libraries that games under Proton cannot reach otherwise.
#    Inside the Steam Linux Runtime container the host's /usr is not visible; pressure-vessel brings in only the
#    libraries the host's own Vulkan drivers need, and the runtime has neither libSPIRV-Tools nor libdisplay-info. So a
#    system without another 64-bit Mesa Vulkan driver (radeon, nouveau) leaves the patched ANV unloadable in games, and
#    a system whose SPIRV-Tools has another name (libSPIRV-Tools-shared.so on Void Linux, libSPIRV-Tools-<version>.so
#    on openSUSE) has no libSPIRV-Tools.so at all. The driver uses only SPIRV-Tools' stable C API, so any of them does.
#    Refreshed every session, so a system update of either library reaches the copy. A system without the library
#    (Debian, Ubuntu and Fedora ship libdisplay-info 0.1/0.2; Debian and Ubuntu have no shared SPIRV-Tools) gets the
#    package's own build from lib/bundled/ (tools/build-deps.sh).
LDCACHE=$({ /sbin/ldconfig -p || ldconfig -p; } 2>/dev/null)   # the host's libraries, once
hostlib() { # <soname, or an awk regex with -r> -> path of the host's 64-bit library
  local p d
  if [ "$1" = -r ]; then
    p=$(awk -v r="$2" '$1 ~ r && /x86-64/ { print $NF; exit }' <<< "$LDCACHE")
    [ -n "$p" ] && [ -f "$p" ] && echo "$p"
    return
  fi
  p=$(awk -v n="$1" '$1 == n && /x86-64/ { print $NF; exit }' <<< "$LDCACHE")
  [ -n "$p" ] && [ -f "$p" ] && { echo "$p"; return; }
  for d in /usr/lib64 /usr/lib/x86_64-linux-gnu /lib64 /lib/x86_64-linux-gnu /usr/lib; do
    [ -f "$d/$1" ] && { echo "$d/$1"; return; }
  done
}
driver_facts() { # <driver>: what the deps and the reports need from the file, read once per build of it (cached):
  #   needed <name>    a library it links that the Steam Linux Runtime lacks (SPIRV-Tools, libdisplay-info)
  #   drirc yes|no     whether it reads /usr/share/drirc.d
  # objdump / readelf read only the ELF headers; without binutils the 25 MB file is scanned for the names
  local names
  if command -v objdump >/dev/null 2>&1; then
    names=$(objdump -p "$1" 2>/dev/null | awk '$1 == "NEEDED" { print $2 }')
  elif command -v readelf >/dev/null 2>&1; then
    names=$(readelf -d "$1" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p')
  else
    names=$(grep -aoE 'lib(SPIRV-Tools[A-Za-z-]*|display-info)\.so[.0-9]*' "$1")
  fi
  printf '%s\n' "$names" | grep -E '^lib(SPIRV-Tools[A-Za-z-]*|display-info)\.so' | sort -u | sed 's/^/needed /'
  if grep -aq '/usr/share/drirc.d' "$1"; then echo "drirc yes"; else echo "drirc no"; fi
}
facts_of() { # <driver> -> its driver_facts, from the cache when the file is unchanged
  local d=${1%/*}
  cached "driver-facts-${d##*/}" "$1" driver_facts "$1"
}
mkdir -p "$PKG/lib/deps"
needed=""
for so in "$PKG"/lib/mesa-*/libvulkan_intel.so; do
  [ -f "$so" ] || continue
  while read -r k v; do
    [ "$k" = needed ] || continue
    case " $needed " in *" $v "*) ;; *) needed="$needed $v" ;; esac
  done <<< "$(facts_of "$so")"
done
for n in $needed; do
  src=$(hostlib "$n")
  [ -z "$src" ] && [ "$n" = libSPIRV-Tools.so ] && src=$(hostlib libSPIRV-Tools-shared.so)
  [ -z "$src" ] && [ "$n" = libSPIRV-Tools.so ] && src=$(hostlib -r '^libSPIRV-Tools-[0-9][^ ]*\.so$')
  [ -z "$src" ] && [ -f "$PKG/lib/bundled/$n" ] && src="$PKG/lib/bundled/$n"
  [ -n "$src" ] || continue
  cmp -s "$src" "$PKG/lib/deps/$n" || { cp -L "$src" "$PKG/lib/deps/$n.new" && mv -f "$PKG/lib/deps/$n.new" "$PKG/lib/deps/$n"; }
done

# 0b. run-time compiled kernels of an older toolchain: the driver names them <hash>_<pci id>_<gen>.cmk, <gen> being
#     anv_cm_toolchain_gen() of the patch (FNV-1a over the igc file names, both helper scripts and the .cmk packer
#     version; CM_DEVICE also goes in). The same value is computed here for every packer version the package knows
#     and kept in CACHE/kernel-gens; a value from the previous session that is not computed any more belongs to a
#     toolchain no driver can use again, and only the files with that value go. Kernels compiled with CM_DEVICE set or
#     before this file existed are never touched (nor those of another package folder's toolchain). Not in a dry run.
if [ "${DRY:-0}" != 1 ] && [ -z "${XMX_CHECK_LIB:-}" ]; then
  packers=$( { grep -ho 'ANV_CM_PACKER_VERSION "[^"]*"' "$PKG"/patches/0001-*.patch "$PKG"/patches/*/0001-*.patch 2>/dev/null |
                 sed 's/.*"\(.*\)"/\1/'
               for so in "$PKG"/lib/mesa-*/libvulkan_intel.so; do [ -f "$so" ] && grep -aoE 'cmk2-[A-Za-z0-9]+' "$so"; done
               echo cmk2-c1; } | sort -u)
  # shellcheck disable=SC2086   # one argument per packer version
  python3 - "$PKG/tools" "$CACHE" $packers <<'EOF' 2>&1 | sed 's/^/kernel cache: /'
import glob, os, sys
tools, cache, packers = sys.argv[1], sys.argv[2], sys.argv[3:]
M = 0xffffffff
def fnv(h, data):
    for b in data:
        h = ((h ^ b) * 16777619) & M
    return h
# anv_cm_toolchain_gen(): the igc file names in readdir order (os.listdir keeps it), each script's bytes followed by
# its size as one uint32, the packer version, CM_DEVICE (unset: nothing)
h = 2166136261
for sub in ("/../igc/neo/bin", "/../igc/igc/local/lib"):
    try:
        names = os.listdir(tools + sub)
    except OSError:
        continue
    for n in names:
        if n.startswith("ocloc") or n.startswith("libigc.so."):
            h = fnv(h, os.fsencode(n))
for s in ("/cmk_pack.py", "/cm-compile.sh"):
    try:
        with open(tools + s, "rb") as f:
            data = f.read()
    except OSError:
        continue
    h = ((fnv(h, data) ^ (len(data) & M)) * 16777619) & M
gens = sorted({"%08x" % fnv(h, p.encode()) for p in packers})
state = os.path.join(cache, "kernel-gens")
try:
    with open(state) as f:
        old = set(f.read().split())
except OSError:
    old = None
if old is not None:
    n = 0
    for g in old - set(gens):
        for k in glob.glob(os.path.join(glob.escape(cache), "kernels", "*_*_%s.cmk" % g)):
            try:
                os.remove(k)
                n += 1
            except OSError:
                pass
    if n:
        print("removed %d kernel(s) of an older toolchain" % n)
if old != set(gens):
    with open(state + ".new", "w") as f:
        f.write("\n".join(gens) + "\n")
    os.replace(state + ".new", state)
EOF
fi

[ "$STOCK" = 1 ] && [ ! -f "$PKG/layer/libVkLayer_xess_xmx.so" ] && fail "stock mode without $PKG/layer/libVkLayer_xess_xmx.so" stock
# 1. the ICD file the session's VK_DRIVER_FILES names, and the library it points to (the loader gets nothing else)
icdlib=$(sed -n 's/.*"library_path"[^"]*"\([^"]*\)".*/\1/p' "$ICD" 2>/dev/null | head -1)
[ -f "$ICD" ] || fail "no ICD file $ICD" stock
[ -n "$icdlib" ] && [ -f "$icdlib" ] || fail "the ICD file's library ${icdlib:-(none)} is missing" stock
# load
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
  out=$(MESA="$CHECK_MESA" VK_DRIVER_FILES="$ICD" VK_LOADER_LAYERS_DISABLE='*' timeout 20 vulkaninfo --summary 2>&1)
  ours=$(printf '%s\n' "$out" | grep -m1 "driverInfo" | sed 's/.*= *//')
  printf '%s\n' "$out" | grep -q "deviceName" || fail "vulkaninfo found no device ($(printf '%s' "$out" | grep -i -m1 "error" | cut -c1-120))"
else
  note=" (note: vulkaninfo not installed, device enumeration not checked)"
fi

# 3. Mesa version against the system's (report only; asked of the stock driver once per build of it)
note="${note:-}"
system_mesa() { # the driverInfo of the stock Intel driver
  VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json VK_LOADER_LAYERS_DISABLE='*' timeout 20 vulkaninfo --summary 2>/dev/null \
    | grep -m1 "driverInfo" | sed 's/.*= *//'
}
if [ -n "$ours" ] && command -v vulkaninfo >/dev/null 2>&1; then
  # the stock driver's file, from its ICD; a bare name there (Fedora) is looked up in the usual places
  syslib=$(sed -n 's/.*"library_path"[^"]*"\([^"]*\)".*/\1/p' /usr/share/vulkan/icd.d/intel_icd.x86_64.json 2>/dev/null | head -1)
  case "$syslib" in
    /*) ;;
    *) for d in /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/lib; do
         [ -f "$d/${syslib:-libvulkan_intel.so}" ] && { syslib="$d/${syslib:-libvulkan_intel.so}"; break; }
       done ;;
  esac
  sys=$(cached system-mesa "$syslib" system_mesa)
  v_ours=$(printf '%s' "$ours" | grep -o 'Mesa [0-9.]*'); v_sys=$(printf '%s' "$sys" | grep -o 'Mesa [0-9.]*')
  [ -n "$v_sys" ] && [ "$v_ours" != "$v_sys" ] && note=" (note: system has $v_sys, the package's driver is $v_ours)"
fi
# 4. per-game workarounds: a driver built with meson's default /usr/local prefix never reads /usr/share/drirc.d
#    (report only; e.g. Wuthering Waves with ray tracing hangs the GPU without them)
if [ -d /usr/share/drirc.d ] && facts_of "$LIB" | grep -q '^drirc no$'; then
  note="$note (note: this driver does not read /usr/share/drirc.d, so Mesa's per-game workarounds are off - update the package or rebuild with tools/rebuild-driver.sh)"
fi
echo "ok $(date '+%F %T') $LIB ${ours}${note}" > "$STATUS"
echo "${CHECK_MESA:+stock mode, $CHECK_MESA: }the package's ANV loads and finds the GPU: ${ours}${note}"
exit 0
