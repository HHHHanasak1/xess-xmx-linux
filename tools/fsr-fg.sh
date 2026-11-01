#!/bin/bash
# fsr-fg.sh on|off|status <Steam app id | game folder> - a game's FSR 3.1+ frame generation on XeSS-FG, kept until
# "off". Meant for Protons without the package's hook (GE-Proton, a Proton install.sh could not write to); with the hook,
# FSR_FG=xess %command% does the same for each run only (docs/fsr-frame-generation.md).
#   on      the game's amd_fidelityfx_framegeneration_dx12.dll is replaced by the package's (ffxfg/), and the XeSS SDK's
#           libxess_fg.dll / libxell.dll go next to it (a newer one of the game's own stays). Originals are kept as
#           <name>.ffxfg-orig; files the game did not have are listed in .ffxfg-added in that folder.
#   off     the game's own files back
#   status  which frame generation each copy of the DLL in the game folder is
# A game update or a file check in Steam brings the game's own files back: run "on" again. Close the game first.
# uninstall.sh runs "off" for every game.
set -u
PKG="$(cd "$(dirname "$0")/.." && pwd)"
. "$PKG/tools/steam-libraries.sh"
FG=amd_fidelityfx_framegeneration_dx12.dll
OURS="$PKG/ffxfg/$FG"
STORE="$PKG/xess"

usage() { sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

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

fg_dlls() { find "$1" -maxdepth 11 -name "$FG" -print0 2>/dev/null; }

on() {
  local game=$1 f d name lib n=0
  [ -f "$OURS" ] || { echo "!! $OURS missing: this package has no FSR frame generation DLL"; exit 1; }
  [ -f "$STORE/libxess_fg.dll" ] && [ -f "$STORE/libxell.dll" ] || { echo "!! no XeSS SDK in $STORE: run $PKG/tools/get-xess.sh"; exit 1; }
  while IFS= read -r -d '' f; do
    n=$((n + 1))
    [ -L "$f" ] && { echo "!! $f is a link (the game runs with FSR_FG=xess right now): close it first"; exit 1; }
    d=$(dirname "$f")
    if cmp -s "$f" "$OURS"; then
      echo "   ${f#$game/}: already XeSS-FG"
    else
      [ -f "$f.ffxfg-orig" ] || cp -p "$f" "$f.ffxfg-orig"
      cp "$OURS" "$f" && echo "   ${f#$game/} -> XeSS-FG"
    fi
    for name in libxess_fg.dll libxell.dll; do
      lib="$d/$name"
      if [ -f "$lib" ]; then
        cmp -s "$lib" "$STORE/$name" && continue
        if newer "$lib" "$STORE/$name"; then echo "   ${lib#$game/}: the game's own is newer, kept"; continue; fi
        [ -f "$lib.ffxfg-orig" ] || cp -p "$lib" "$lib.ffxfg-orig"
      else
        echo "$name" >> "$d/.ffxfg-added"
      fi
      cp "$STORE/$name" "$lib" && echo "   ${lib#$game/} <- XeSS SDK $(cat "$STORE/VERSION" 2>/dev/null)"
    done
  done < <(fg_dlls "$game")
  if [ "$n" = 0 ]; then
    if find "$game" -maxdepth 11 -name amd_fidelityfx_dx12.dll | grep -q .; then
      echo "!! the game has FSR 3.1.0-3.1.3 (one amd_fidelityfx_dx12.dll): not supported yet"
    else
      echo "!! no $FG in $game: the game has no FSR 3.1+ frame generation"
    fi
    exit 1
  fi
  echo "FSR frame generation runs on XeSS-FG in $game. Turn on FSR frame generation in the game; XESS_FG=<n> in the"
  echo "launch options sets the number of frames (docs/fsr-frame-generation.md)."
}

off() {
  local game=$1 o a f name n=0
  while IFS= read -r -d '' o; do
    f="${o%.ffxfg-orig}"; mv -f "$o" "$f" && n=$((n + 1)) && echo "   restored ${f#$game/}"
  done < <(find "$game" -maxdepth 11 -name '*.ffxfg-orig' -print0 2>/dev/null)
  while IFS= read -r -d '' a; do
    while IFS= read -r name; do
      [ -n "$name" ] && rm -f "$(dirname "$a")/$name" && n=$((n + 1)) && echo "   removed $(dirname "${a#$game/}")/$name"
    done < "$a"
    rm -f "$a"
  done < <(find "$game" -maxdepth 11 -name .ffxfg-added -print0 2>/dev/null)
  echo "$n file(s) put back in $game"
}

status() {
  local game=$1 f
  while IFS= read -r -d '' f; do
    if [ -L "$f" ]; then echo "${f#$game/}: XeSS-FG for this run (FSR_FG=xess)"
    elif cmp -s "$f" "$OURS"; then echo "${f#$game/}: XeSS-FG (fsr-fg.sh on)"
    else echo "${f#$game/}: the game's own FSR frame generation"; fi
  done < <(fg_dlls "$game")
}

[ $# -eq 2 ] || usage
case "$1" in on|off|status) ;; *) usage ;; esac
GAME=$(game_dir "$2") || { echo "!! no game folder for '$2' (a Steam app id or a folder)"; exit 1; }
"$1" "$GAME"
