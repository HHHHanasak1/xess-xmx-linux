#!/bin/bash
# steam-libraries.sh - print every Steam library root (the folder that holds steamapps/), one per line: the main
# install plus the libraries listed in steamapps/libraryfolders.vdf (SD card, second disk). Sourced or executed.
#   steam_libraries [steam dir]      default: $STEAM_DIR, else ~/.local/share/Steam
# Sourced, it also provides the helpers every script of the package shares (steam_prefix_of, xmx_icd_json,
# xmx_driver_files).
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
# the Vulkan ICD manifest for a driver of the package, on stdout
#   xmx_icd_json <libvulkan_intel.so path>
xmx_icd_json() {
  printf '{ "ICD": { "api_version": "1.4.348", "library_arch": "64", "library_path": "%s" }, "file_format_version": "1.0.1" }\n' "$1"
}
# the VK_DRIVER_FILES list for a session on the patched driver: the package's ICD, then every other driver of the
# system (hybrid laptops, eGPUs) - only the stock 64-bit Intel ANV is left out so that a game cannot pick it instead of
# the patched one; the 32-bit Intel ICD stays (32-bit games)
#   xmx_driver_files <package ICD json>
xmx_driver_files() {
  local drivers="$1" d j
  for d in /usr/share/vulkan/icd.d /usr/local/share/vulkan/icd.d /etc/vulkan/icd.d "${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/icd.d"; do
    for j in "$d"/*.json; do
      [ -f "$j" ] || continue
      case "${j##*/}" in intel_icd.x86_64.json|intel_icd.json) continue ;; esac
      drivers="$drivers:$j"
    done
  done
  printf '%s\n' "$drivers"
}
# run directly: list the libraries. Sourced (enable.sh and rebuild-driver.sh run with set -e): the status must be 0,
# which the if below gives when its condition is false
if [ "${BASH_SOURCE[0]}" = "$0" ]; then steam_libraries "$@"; fi
