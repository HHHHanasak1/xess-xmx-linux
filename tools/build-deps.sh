#!/bin/bash
# build-deps.sh <out dir> - builds the two libraries the release bundles in lib/bundled/: SPIRV-Tools (the shared
# library, renamed to the libSPIRV-Tools.so the drivers link against) and libdisplay-info.so.3, plus their licenses.
# tools/session-check.sh uses them only when the system has no library of that name (Debian, Ubuntu and Fedora ship
# libdisplay-info 0.1/0.2 and, on Debian/Ubuntu, no shared SPIRV-Tools). Same versions the release drivers are built
# against. Build on an old system so the libraries run on newer ones: the release uses Debian 12 (glibc 2.36), e.g.
#     docker run --rm -v "$PWD":/src -w /src debian:bookworm tools/build-deps.sh lib/bundled
set -eu
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
SPIRV_TAG=vulkan-sdk-1.4.350.1
DI_TAG=0.3.0
if command -v apt-get >/dev/null && [ "$(id -u)" = 0 ]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq && apt-get install -y -qq build-essential cmake ninja-build meson pkg-config python3 hwdata curl \
    ca-certificates >/dev/null
fi
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT; cd "$W"
get() { # <url> <dir>
  mkdir -p "$2"; curl -sfL "$1" | tar -xz -C "$2" --strip-components=1
}
get "https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/$SPIRV_TAG.tar.gz" spirv-tools
get "https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/$SPIRV_TAG.tar.gz" spirv-headers
get "https://gitlab.freedesktop.org/emersion/libdisplay-info/-/archive/$DI_TAG/libdisplay-info-$DI_TAG.tar.gz" libdisplay-info

cmake -S spirv-tools -B spirv-tools/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPIRV-Headers_SOURCE_DIR="$W/spirv-headers" \
  -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF >/dev/null
ninja -C spirv-tools/build SPIRV-Tools-shared >/dev/null
meson setup libdisplay-info/build libdisplay-info --buildtype=release >/dev/null
ninja -C libdisplay-info/build >/dev/null

install -m 755 spirv-tools/build/source/libSPIRV-Tools-shared.so "$OUT/libSPIRV-Tools.so"
install -m 755 "$(readlink -f libdisplay-info/build/libdisplay-info.so.3)" "$OUT/libdisplay-info.so.3"
strip --strip-unneeded "$OUT/libSPIRV-Tools.so" "$OUT/libdisplay-info.so.3"
install -m 644 spirv-tools/LICENSE "$OUT/LICENSE.SPIRV-Tools"
install -m 644 libdisplay-info/LICENSE "$OUT/LICENSE.libdisplay-info"
printf 'SPIRV-Tools %s (https://github.com/KhronosGroup/SPIRV-Tools, Apache-2.0)\nlibdisplay-info %s (https://gitlab.freedesktop.org/emersion/libdisplay-info, MIT)\nbuilt with tools/build-deps.sh on %s, glibc %s\n' \
  "$SPIRV_TAG" "$DI_TAG" "$(. /etc/os-release; echo "$PRETTY_NAME")" "$(getconf GNU_LIBC_VERSION | cut -d' ' -f2)" > "$OUT/SOURCES.txt"
for f in "$OUT"/*.so*; do
  echo "$(basename "$f"): $(objdump -T "$f" | grep -o 'GLIBC_2\.[0-9]*' | sort -t. -k2 -n -u | tail -1)" \
    "$(objdump -T "$f" | grep -o 'GLIBCXX_3\.4\.[0-9]*' | sort -t. -k3 -n -u | tail -1)"
done
