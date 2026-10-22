#!/bin/bash
# rebuild-driver.sh [mesa version] - rebuild the patched ANV for the Mesa version the system runs (after an OS update),
# test it, install it as lib/mesa-<version>/libvulkan_intel.so and make it the active driver (tools/switch-driver.sh).
# Patches: patches/mesa-<major.minor>/ when the package has that folder (26.3: patches/mesa-26.3/), else patches/.
# Needs a Mesa build environment (compiler, meson, ninja, LLVM, clang, libclc, SPIRV-LLVM-Translator, glslang, python-mako,
# libdrm, wayland, xcb headers). On SteamOS use an Arch Linux distrobox:
#     distrobox enter <box> -- /path/to/package/tools/rebuild-driver.sh
# The version defaults to the system's Mesa (read from the host's stock driver). If the patches do not apply to that
# version, nothing is changed and the script says so. Source: the release tarball from archive.mesa3d.org; source and
# build tree: ${XMX_MESA_DIR:-~/.cache/xess-xmx/mesa-src}/<version>.
set -eu
PKG="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${XMX_MESA_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/mesa-src}"
VER="${1:-}"
if [ -z "$VER" ]; then
  for f in /run/host/usr/lib/libvulkan_intel.so /usr/lib/libvulkan_intel.so /usr/lib/x86_64-linux-gnu/libvulkan_intel.so; do
    [ -f "$f" ] && VER=$(grep -a -o -m1 'Mesa [0-9]*\.[0-9]*\.[0-9]*' "$f" | head -1 | cut -d' ' -f2) && [ -n "$VER" ] && break
  done
fi
[ -n "$VER" ] || { echo "could not detect the system's Mesa version; pass it: $0 26.1.2 (or 26.3.0-rc1)"; exit 1; }
echo "== Mesa $VER"
# the release tarball, not the git tag: upstream can delete a tag (mesa-26.3.0-rc1 went, together with its commit,
# when staging/26.3 was branched again). One tree per version, committed as "base" so the patches go on with git am.
T="$SRC/$VER"
if [ ! -d "$T/.git" ]; then
  rm -rf "$T"; mkdir -p "$T"
  if ! curl -sfL "https://archive.mesa3d.org/mesa-$VER.tar.xz" | tar -xJ -C "$T" --strip-components=1; then
    rm -rf "$T"
    echo "!! could not download https://archive.mesa3d.org/mesa-$VER.tar.xz (nothing was changed)"
    exit 1
  fi
  git -C "$T" init -q
  git -C "$T" add -A
  git -C "$T" -c user.name=xess-xmx -c user.email=xmx@localhost commit -qm "mesa $VER"
  git -C "$T" tag base
fi
cd "$T"
git am --abort 2>/dev/null || true
git checkout --quiet -f -B xess-xmx base
PATCHES="$PKG/patches"
MM=$(printf '%s' "$VER" | grep -o '^[0-9]*\.[0-9]*')
[ -d "$PKG/patches/mesa-$MM" ] && PATCHES="$PKG/patches/mesa-$MM"
echo "== patches: $PATCHES"
if ! git -c user.name=xess-xmx -c user.email=xmx@localhost am --quiet "$PATCHES"/0*.patch; then
  git am --abort 2>/dev/null || true
  echo "!! the patches do not apply to Mesa $VER - they need to be rebased (nothing was changed)"
  exit 2
fi
# prefix/sysconfdir as the system's Mesa: the driver reads its per-game workarounds from <datadir>/drirc.d, and without
# the vkd3d entries there Wuthering Waves hangs the GPU with ray tracing on
OPTS="-Dprefix=/usr -Dsysconfdir=/etc -Dbuildtype=release -Dvulkan-drivers=intel -Dgallium-drivers= -Dglx=disabled -Dgbm=disabled -Degl=disabled
      -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=enabled -Dintel-rt=enabled -Dvideo-codecs= -Dvulkan-layers=
      -Dtools="
if [ -d build ]; then meson setup --reconfigure build $OPTS >/dev/null; else meson setup build $OPTS >/dev/null; fi
ninja -C build src/intel/vulkan/libvulkan_intel.so
NEW="$SRC/build/src/intel/vulkan/libvulkan_intel.so"
if ! grep -aq '/usr/share/drirc.d' "$NEW"; then
  echo "!! the new driver does not read /usr/share/drirc.d (wrong prefix); not installed"
  exit 3
fi
# test the new driver before installing it (same checks as the session check)
TMPICD=$(mktemp --suffix=.json)
trap 'rm -f "$TMPICD"' EXIT
printf '{ "ICD": { "api_version": "1.4.348", "library_arch": "64", "library_path": "%s" }, "file_format_version": "1.0.1" }\n' "$NEW" > "$TMPICD"
if ! XMX_CHECK_LIB="$NEW" XMX_CHECK_ICD="$TMPICD" bash "$PKG/tools/session-check.sh" --dry-run; then
  echo "!! the new driver failed its check; not installed"
  exit 3
fi
D="$PKG/lib/mesa-$VER"
mkdir -p "$D"
[ -f "$D/libvulkan_intel.so" ] && cp -p "$D/libvulkan_intel.so" "$D/libvulkan_intel.so.prev"
cp "$NEW" "$D/libvulkan_intel.so.new" && mv "$D/libvulkan_intel.so.new" "$D/libvulkan_intel.so"
echo "== installed: $D/libvulkan_intel.so (Mesa $VER + patches)"
bash "$PKG/tools/switch-driver.sh" "$VER"
