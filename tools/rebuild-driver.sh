#!/bin/bash
# rebuild-driver.sh [mesa version] - rebuild the patched ANV for the Mesa version the system runs (after an OS update),
# test it, and install it into the package's lib/ (the previous one is kept as lib/libvulkan_intel.so.prev).
# Needs a Mesa build environment (compiler, meson, ninja, LLVM, clang, libclc, SPIRV-LLVM-Translator, glslang, python-mako,
# libdrm, wayland, xcb headers). On SteamOS use an Arch Linux distrobox:
#     distrobox enter <box> -- /path/to/package/tools/rebuild-driver.sh
# The version defaults to the system's Mesa (read from the host's stock driver). If the patches do not apply to that
# version, nothing is changed and the script says so. Source and build tree: ${XMX_MESA_DIR:-~/.cache/xess-xmx/mesa-src}.
set -eu
PKG="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${XMX_MESA_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/mesa-src}"
VER="${1:-}"
if [ -z "$VER" ]; then
  for f in /run/host/usr/lib/libvulkan_intel.so /usr/lib/libvulkan_intel.so /usr/lib/x86_64-linux-gnu/libvulkan_intel.so; do
    [ -f "$f" ] && VER=$(grep -a -o -m1 'Mesa [0-9]*\.[0-9]*\.[0-9]*' "$f" | head -1 | cut -d' ' -f2) && [ -n "$VER" ] && break
  done
fi
[ -n "$VER" ] || { echo "could not detect the system's Mesa version; pass it: $0 26.1.2"; exit 1; }
echo "== Mesa $VER"
if [ ! -d "$SRC/.git" ]; then
  git clone --quiet https://gitlab.freedesktop.org/mesa/mesa.git "$SRC"
fi
cd "$SRC"
git fetch --quiet origin "refs/tags/mesa-$VER:refs/tags/mesa-$VER" 2>/dev/null || true
git am --abort 2>/dev/null || true
git checkout --quiet -f -B xess-xmx "mesa-$VER"
if ! git -c user.name=xess-xmx -c user.email=xmx@localhost am --quiet "$PKG"/patches/0*.patch; then
  git am --abort 2>/dev/null || true
  echo "!! the patches do not apply to Mesa $VER - they need to be rebased (nothing was changed)"
  exit 2
fi
OPTS="-Dbuildtype=release -Dvulkan-drivers=intel -Dgallium-drivers= -Dglx=disabled -Dgbm=disabled -Degl=disabled
      -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=enabled -Dintel-rt=enabled -Dvideo-codecs= -Dvulkan-layers=
      -Dtools="
if [ -d build ]; then meson setup --reconfigure build $OPTS >/dev/null; else meson setup build $OPTS >/dev/null; fi
ninja -C build src/intel/vulkan/libvulkan_intel.so
NEW="$SRC/build/src/intel/vulkan/libvulkan_intel.so"
# test the new driver before installing it (same checks as the session check)
TMPICD=$(mktemp --suffix=.json)
trap 'rm -f "$TMPICD"' EXIT
printf '{ "ICD": { "api_version": "1.4.348", "library_arch": "64", "library_path": "%s" }, "file_format_version": "1.0.1" }\n' "$NEW" > "$TMPICD"
if ! XMX_CHECK_LIB="$NEW" XMX_CHECK_ICD="$TMPICD" bash "$PKG/tools/session-check.sh" --dry-run; then
  echo "!! the new driver failed its check; not installed"
  exit 3
fi
cp -p "$PKG/lib/libvulkan_intel.so" "$PKG/lib/libvulkan_intel.so.prev"
cp "$NEW" "$PKG/lib/libvulkan_intel.so.new" && mv "$PKG/lib/libvulkan_intel.so.new" "$PKG/lib/libvulkan_intel.so"
bash "$PKG/tools/session-check.sh" --dry-run || true
echo "== installed: $PKG/lib/libvulkan_intel.so (Mesa $VER + patches); previous kept as libvulkan_intel.so.prev"
echo "   Restart the game (or re-login if the previous driver had failed its session check)."
