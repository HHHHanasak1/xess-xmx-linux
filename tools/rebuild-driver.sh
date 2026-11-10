#!/bin/bash
# rebuild-driver.sh [mesa version] - rebuild the patched ANV for the Mesa version the system runs (after an OS update),
# test it, install it as lib/mesa-<version>/libvulkan_intel.so and make it the active driver (tools/switch-driver.sh).
# Patches: patches/mesa-<major.minor>/ when the package has that folder (26.3: patches/mesa-26.3/), else patches/.
# Needs a Mesa build environment (compiler, meson, ninja, LLVM, clang, libclc, SPIRV-LLVM-Translator, glslang, python-mako,
# libdrm, wayland, xcb headers). On SteamOS use an Arch Linux distrobox:
#     distrobox enter <box> -- /path/to/package/tools/rebuild-driver.sh
# The version defaults to the system's Mesa (read from the host's stock driver). If the patches do not apply to that
# version, nothing is changed and the script says so. Source: the release tarball from archive.mesa3d.org, checked
# against its sha256 (known for the versions CI builds; any other: XMX_MESA_SHA256=<sha256>); source and
# build tree: ${XMX_MESA_DIR:-~/.cache/xess-xmx/mesa-src}/<version>.
set -eu
PKG="$(cd "$(dirname "$0")/.." && pwd)"
. "$PKG/tools/steam-libraries.sh"
SRC="${XMX_MESA_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/xess-xmx/mesa-src}"
VER="${1:-}"
if [ -z "$VER" ]; then
  # the host's 64-bit driver first (Fedora/Bazzite: /usr/lib64; inside a distrobox the host is under /run/host);
  # /usr/lib/libvulkan_intel.so on Fedora is the 32-bit one, and inside a box /usr/lib* is the box's own Mesa
  for f in /run/host/usr/lib64/libvulkan_intel.so /run/host/usr/lib/x86_64-linux-gnu/libvulkan_intel.so /run/host/usr/lib/libvulkan_intel.so \
           /usr/lib64/libvulkan_intel.so /usr/lib/x86_64-linux-gnu/libvulkan_intel.so /usr/lib/libvulkan_intel.so; do
    [ -f "$f" ] && VER=$(grep -a -o -m1 'Mesa [0-9]*\.[0-9]*\.[0-9]*' "$f" | head -1 | cut -d' ' -f2) && [ -n "$VER" ] && break
  done
fi
[ -n "$VER" ] || { echo "could not detect the system's Mesa version; pass it: $0 26.1.2 (or 26.3.0)"; exit 1; }
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-rc[0-9]+)?$ ]] || { echo "!! '$VER' is not a Mesa version (26.1.2, 26.3.0)"; exit 1; }
echo "== Mesa $VER"
# the tarball's sha256: the versions CI builds (.github/workflows/build.yml, mesa job - keep both lists the same);
# another version needs XMX_MESA_SHA256=<sha256 from Mesa's release notes, docs/relnotes/<version>.rst>
case "$VER" in
  26.1.2) SHA=bac2bca9121897a2b8162e79636b50ac998fca799c8e6cf914edd85962babdf0 ;;
  26.3.0) SHA=91d729aa68bb89e4ec857841300bbe922bcb3e52b1f97566ca83930d3b28343a ;;
  *) SHA="" ;;
esac
[ -n "${XMX_MESA_SHA256:-}" ] && SHA=$XMX_MESA_SHA256
[[ "$SHA" =~ ^[0-9a-f]{64}$ ]] || {
  echo "!! no known sha256 for the Mesa $VER tarball: run again with XMX_MESA_SHA256=<its sha256> (Mesa's release notes"
  echo "   list it, docs/relnotes/$VER.rst; nothing was changed)"; exit 1; }
# the release tarball, not the git tag: upstream can delete a tag (mesa-26.3.0-rc1 went, together with its commit,
# when staging/26.3 was branched again). One tree per version, committed as "base" so the patches go on with git am.
T="$SRC/$VER"
if [ ! -d "$T/.git" ]; then
  rm -rf "$T"; mkdir -p "$T"
  TAR="$SRC/mesa-$VER.tar.xz"
  if ! curl -sfL -o "$TAR" "https://archive.mesa3d.org/mesa-$VER.tar.xz"; then
    rm -rf "$T" "$TAR"
    echo "!! could not download https://archive.mesa3d.org/mesa-$VER.tar.xz (nothing was changed)"
    exit 1
  fi
  if ! echo "$SHA  $TAR" | sha256sum -c --quiet - >/dev/null 2>&1; then
    rm -rf "$T" "$TAR"
    echo "!! mesa-$VER.tar.xz does not have the sha256 $SHA (nothing was changed)"
    exit 1
  fi
  if ! tar -xJf "$TAR" -C "$T" --strip-components=1; then
    rm -rf "$T" "$TAR"
    echo "!! could not unpack mesa-$VER.tar.xz (nothing was changed)"
    exit 1
  fi
  rm -f "$TAR"
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
# the vkd3d entries there Wuthering Waves hangs the GPU with ray tracing on.
# Run path lib/deps: copies of the host libraries the Steam Linux Runtime does not have (tools/session-check.sh fills it)
OPTS="-Dprefix=/usr -Dsysconfdir=/etc -Dbuildtype=release -Dvulkan-drivers=intel -Dgallium-drivers= -Dglx=disabled -Dgbm=disabled -Degl=disabled
      -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=enabled -Dintel-rt=enabled -Dvideo-codecs= -Dvulkan-layers=
      -Dtools= -Dcpp_link_args=-Wl,-rpath,\$ORIGIN/../deps"
if [ -d build ]; then meson setup --reconfigure build $OPTS >/dev/null; else meson setup build $OPTS >/dev/null; fi
ninja -C build src/intel/vulkan/libvulkan_intel.so
NEW="$T/build/src/intel/vulkan/libvulkan_intel.so"
if ! grep -aq '/usr/share/drirc.d' "$NEW"; then
  echo "!! the new driver does not read /usr/share/drirc.d (wrong prefix); not installed"
  exit 3
fi
# test the new driver before installing it (same checks as the session check)
TMPICD=$(mktemp --suffix=.json)
trap 'rm -f "$TMPICD"' EXIT
xmx_icd_json "$NEW" > "$TMPICD"
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
