#!/bin/bash
# steam-libraries.sh - print every Steam library root (the folder that holds steamapps/), one per line: the main
# install plus the libraries listed in steamapps/libraryfolders.vdf (SD card, second disk). Sourced or executed.
#   steam_libraries [steam dir]      default: $STEAM_DIR, else ~/.local/share/Steam
steam_libraries() {
  local steam="${1:-${STEAM_DIR:-$HOME/.local/share/Steam}}" vdf p
  printf '%s\n' "$steam"
  vdf="$steam/steamapps/libraryfolders.vdf"
  [ -f "$vdf" ] || vdf="$steam/config/libraryfolders.vdf"
  [ -f "$vdf" ] || return 0
  # lines like:   "path"   "/run/media/deck/CARD"   (backslash-escaped in the file)
  grep -a '"path"' "$vdf" | sed 's/.*"path"[[:space:]]*"\(.*\)".*/\1/; s/\\\\/\//g' | while IFS= read -r p; do
    [ -n "$p" ] && [ -d "$p/steamapps" ] && [ "$p" != "$steam" ] && printf '%s\n' "$p"
  done
}
# find the compatdata prefix of an app id across all libraries
steam_prefix_of() {
  local id="$1" lib
  while IFS= read -r lib; do
    [ -d "$lib/steamapps/compatdata/$id/pfx" ] && { printf '%s\n' "$lib/steamapps/compatdata/$id/pfx"; return 0; }
  done < <(steam_libraries)
  return 1
}
[ "${BASH_SOURCE[0]}" = "$0" ] && steam_libraries "$@"
