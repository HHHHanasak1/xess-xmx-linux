#!/bin/bash
# fsr-fg.sh all|on|off|status <Steam app id | game folder> - a game's FSR 3.1+ on XeSS, kept until "off". Meant for
# a Proton without the package's hook (one install.sh could not write to, such as a distribution package's); with the
# hook (GE-Proton has it too), FSR=xess / FSR_FG=xess %command% do the same for each run only
# (docs/frame-generation.md).
#   all     upscaling on XeSS SR and frame generation on XeSS-FG: the game's amd_fidelityfx_upscaler_dx12.dll and
#           amd_fidelityfx_framegeneration_dx12.dll are replaced by the package's (ffxfg/), and the XeSS SDK's
#           libxess.dll / libxess_fg.dll / libxell.dll go next to them (a newer one of the game's own stays). FSR
#           3.1.0-3.1.3 (one amd_fidelityfx_dx12.dll): AMD's loader takes its place, the package's DLLs go next to it
#           (tools/get-ffx.sh). Originals are kept as <name>.ffxfg-orig; files the game did not have are listed in
#           .ffxfg-added in that folder.
#   on      frame generation only (FSR 3.1.0-3.1.3: with AMD's upscaler next to the loader)
#   off     the game's own files back
#   status  what each copy of the DLLs in the game folder is
# A game update or a file check in Steam brings the game's own files back: run "on" again. Close the game first.
# uninstall.sh runs "off" for every game.
set -u
PKG="$(cd "$(dirname "$0")/.." && pwd)"
. "$PKG/tools/steam-libraries.sh"
FG=amd_fidelityfx_framegeneration_dx12.dll
OURS="$PKG/ffxfg/$FG"
STORE="$PKG/xess"
SINGLE=amd_fidelityfx_dx12.dll   # FSR 3.1.0-3.1.3
LOADER="$PKG/ffx/amd_fidelityfx_loader_dx12.dll"
UP=amd_fidelityfx_upscaler_dx12.dll
UPSCALER="$PKG/ffx/$UP"   # AMD's, for "on" with FSR 3.1.0-3.1.3
OURS_UP="$PKG/ffxfg/$UP"
SR=0                       # "all": the upscaler as well

usage() { sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

game_dir() { # app id or folder -> the game's install folder
  local lib m dir
  if [ -d "$1" ]; then (cd "$1" && pwd); return 0; fi
  case "$1" in *[!0-9]*|"") return 1 ;; esac
  while IFS= read -r lib; do
    m="$lib/steamapps/appmanifest_$1.acf"
    [ -f "$m" ] || continue
    dir=$(grep -a '"installdir"' "$m" | sed 's/.*"installdir"[[:space:]]*"\(.*\)".*/\1/')
    [ -n "$dir" ] && [ -d "$lib/steamapps/common/$dir" ] && { printf '%s\n' "$lib/steamapps/common/$dir"; return 0; }
  done < <(steam_libraries)
  return 1
}

newer() { # <a> <b>: true when a's file version is above b's (both must have one)
  python3 - "$1" "$2" <<'EOF'
import sys
def version(path):
    d = open(path, "rb").read()
    i = d.find(b"\xbd\x04\xef\xfe")
    if i < 0:
        return None
    ms, ls = int.from_bytes(d[i + 8:i + 12], "little"), int.from_bytes(d[i + 12:i + 16], "little")
    return (ms >> 16, ms & 0xffff, ls >> 16, ls & 0xffff)
a, b = version(sys.argv[1]), version(sys.argv[2])
sys.exit(0 if a and b and a > b else 1)
EOF
}

fg_dlls() { find "$1" -maxdepth 11 -name "${2:-$FG}" -print0 2>/dev/null; }

place() { # <source> <file in the game> <label>: the game's own file kept as .ffxfg-orig, a new one noted in .ffxfg-added
  local src=$1 f=$2 d
  d=$(dirname "$f")
  cmp -s "$f" "$src" && { echo "   ${f#$GAME/}: already $3"; return 0; }
  if [ -f "$f" ]; then
    [ -f "$f.ffxfg-orig" ] || cp -p "$f" "$f.ffxfg-orig"
  else
    echo "$(basename "$f")" >> "$d/.ffxfg-added"
  fi
  cp "$src" "$f" && echo "   ${f#$GAME/} <- $3"
}

xess_libs() { # <folder>: the XeSS SDK's libxess_fg.dll / libxell.dll (and libxess.dll for "all"), unless the game's own are newer
  local name lib names="libxess_fg.dll libxell.dll"
  [ "$SR" = 1 ] && names="libxess.dll $names"
  for name in $names; do
    lib="$1/$name"
    if [ -f "$lib" ] && ! cmp -s "$lib" "$STORE/$name" && newer "$lib" "$STORE/$name"; then
      echo "   ${lib#$GAME/}: the game's own is newer, kept"; continue
    fi
    place "$STORE/$name" "$lib" "XeSS SDK $(cat "$STORE/VERSION" 2>/dev/null)"
  done
}

on() {
  local game=$1 f d n=0
  [ -f "$OURS" ] || { echo "!! $OURS missing: this package has no FSR frame generation DLL"; exit 1; }
  [ -f "$STORE/libxess_fg.dll" ] && [ -f "$STORE/libxell.dll" ] || { echo "!! no XeSS SDK in $STORE: run $PKG/tools/get-xess.sh"; exit 1; }
  if [ "$SR" = 1 ]; then
    [ -f "$OURS_UP" ] || { echo "!! $OURS_UP missing: this package has no FSR upscaler DLL"; exit 1; }
    [ -f "$STORE/libxess.dll" ] || { echo "!! no XeSS SDK in $STORE: run $PKG/tools/get-xess.sh"; exit 1; }
  fi
  while IFS= read -r -d '' f; do
    n=$((n + 1))
    [ -L "$f" ] && { echo "!! $f is a link (the game runs with FSR_FG=xess right now): close it first"; exit 1; }
    place "$OURS" "$f" XeSS-FG
    d=$(dirname "$f")
    [ "$SR" = 1 ] && [ -f "$d/$UP" ] && place "$OURS_UP" "$d/$UP" "XeSS SR"
    xess_libs "$d"
  done < <(fg_dlls "$game")
  if [ "$n" = 0 ]; then
    while IFS= read -r -d '' f; do
      n=$((n + 1))
      [ -L "$f" ] && { echo "!! $f is a link (the game runs with FSR_FG=xess right now): close it first"; exit 1; }
      [ -f "$LOADER" ] && { [ "$SR" = 1 ] || [ -f "$UPSCALER" ]; } || { echo "!! the game has FSR 3.1.0-3.1.3 (one $SINGLE): it needs AMD's loader, run $PKG/tools/get-ffx.sh"; exit 1; }
      d=$(dirname "$f")
      place "$LOADER" "$f" "FidelityFX loader (FSR SDK 2.4.0)"
      if [ "$SR" = 1 ]; then place "$OURS_UP" "$d/$UP" "XeSS SR"
      else place "$UPSCALER" "$d/$UP" "AMD's upscaler (FSR SDK 2.4.0)"; fi
      place "$OURS" "$d/$FG" XeSS-FG
      xess_libs "$d"
    done < <(fg_dlls "$game" "$SINGLE")
  fi
  if [ "$n" = 0 ]; then
    echo "!! no $FG in $game: the game has no FSR 3.1+ frame generation"
    exit 1
  fi
  [ "$SR" = 1 ] && echo "FSR upscaling runs on XeSS SR in $game: choose FSR (3.1 or 4) upscaling in the game."
  echo "FSR frame generation runs on XeSS-FG in $game. Turn on FSR frame generation in the game; XESS_FG=<n> in the"
  echo "launch options sets the number of frames (docs/frame-generation.md)."
}

package_copy() { # <file>: true when it is a copy of one of the package's DLLs of that name (all the files "on" can add)
  local name=${1##*/} p
  for p in "$PKG/ffxfg/$name" "$PKG/ffx/$name" "$STORE/$name"; do
    [ -f "$p" ] && cmp -s "$1" "$p" && return 0
  done
  return 1
}

off() {
  local game=$1 o a f name n=0
  while IFS= read -r -d '' o; do
    f="${o%.ffxfg-orig}"; mv -f "$o" "$f" && n=$((n + 1)) && echo "   restored ${f#$game/}"
  done < <(find "$game" -maxdepth 11 -name '*.ffxfg-orig' -print0 2>/dev/null)
  # .ffxfg-added lists the files "on" put in that folder: only plain names are taken, and only a file that is still
  # the package's copy goes (one of the game's own, or one changed since, stays)
  while IFS= read -r -d '' a; do
    while IFS= read -r name; do
      case "$name" in ""|.|..|*/*) continue ;; esac
      f="$(dirname "$a")/$name"
      [ -f "$f" ] || continue
      if ! package_copy "$f"; then echo "   kept $(dirname "${a#$game/}")/$name (not the package's copy)"; continue; fi
      rm -f "$f" && n=$((n + 1)) && echo "   removed $(dirname "${a#$game/}")/$name"
    done < "$a"
    rm -f "$a"
  done < <(find "$game" -maxdepth 11 -name .ffxfg-added -print0 2>/dev/null)
  echo "$n file(s) put back in $game"
}

status() {
  local game=$1 f
  while IFS= read -r -d '' f; do
    if [ -L "$f" ]; then echo "${f#$game/}: XeSS-FG for this run (FSR / FSR_FG=xess)"
    elif cmp -s "$f" "$OURS"; then echo "${f#$game/}: XeSS-FG (fsr-fg.sh on)"
    else echo "${f#$game/}: the game's own FSR frame generation"; fi
  done < <(fg_dlls "$game")
  while IFS= read -r -d '' f; do
    if [ -L "$f" ]; then echo "${f#$game/}: XeSS SR or AMD's upscaler for this run (FSR=xess / FSR_FG=xess)"
    elif cmp -s "$f" "$OURS_UP"; then echo "${f#$game/}: XeSS SR (fsr-fg.sh all)"
    elif cmp -s "$f" "$UPSCALER"; then echo "${f#$game/}: AMD's upscaler of FSR SDK 2.4.0 (fsr-fg.sh on)"
    else echo "${f#$game/}: the game's own FSR upscaler"; fi
  done < <(fg_dlls "$game" "$UP")
  while IFS= read -r -d '' f; do
    if [ -L "$f" ]; then echo "${f#$game/}: FidelityFX loader with XeSS-FG for this run (FSR_FG=xess)"
    elif cmp -s "$f" "$LOADER"; then echo "${f#$game/}: FidelityFX loader with XeSS-FG (fsr-fg.sh on)"
    else echo "${f#$game/}: the game's own FSR 3.1.0-3.1.3"; fi
  done < <(fg_dlls "$game" "$SINGLE")
}

[ $# -eq 2 ] || usage
case "$1" in all|on|off|status) ;; *) usage ;; esac
GAME=$(game_dir "$2") || { echo "!! no game folder for '$2' (a Steam app id or a folder)"; exit 1; }
cmd=$1
[ "$cmd" = all ] && { SR=1; cmd=on; }
"$cmd" "$GAME"
