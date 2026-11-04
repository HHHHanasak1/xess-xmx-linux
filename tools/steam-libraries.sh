#!/bin/bash
# steam-libraries.sh - print every Steam library root (the folder that holds steamapps/), one per line: the main
# install plus the libraries listed in steamapps/libraryfolders.vdf (SD card, second disk). Sourced or executed.
#   steam_libraries [steam dir]      default: steam_root
# Sourced, it also provides the helpers every script of the package shares (steam_root, steam_prefix_of,
# xmx_icd_json, xmx_driver_files).
# The Steam install folder: $STEAM_DIR, else the first that exists of ~/.local/share/Steam (Valve's installer,
# SteamOS, Arch), ~/.steam/steam and ~/.steam/root (links every Steam client keeps; Debian's and Ubuntu's steam package
# installs to ~/.steam/debian-installation), with links resolved, so it matches the paths in libraryfolders.vdf
steam_root() {
  [ -n "${STEAM_DIR:-}" ] && { printf '%s\n' "$STEAM_DIR"; return 0; }
  local d
  for d in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" "$HOME/.steam/debian-installation"; do
    if [ -d "$d/steamapps" ] || [ -f "$d/steam.sh" ]; then readlink -f "$d"; return 0; fi
  done
  printf '%s\n' "$HOME/.local/share/Steam"
}
steam_libraries() {
  local steam="${1:-$(steam_root)}" vdf p
  printf '%s\n' "$steam"
  vdf="$steam/steamapps/libraryfolders.vdf"
  [ -f "$vdf" ] || vdf="$steam/config/libraryfolders.vdf"
  [ -f "$vdf" ] || return 0
  # lines like:   "path"   "/run/media/deck/CARD"   (backslash-escaped in the file)
  grep -a '"path"' "$vdf" | sed 's/.*"path"[[:space:]]*"\(.*\)".*/\1/; s/\\\\/\//g' | while IFS= read -r p; do
    [ -n "$p" ] && [ -d "$p/steamapps" ] && [ "$p" != "$steam" ] && printf '%s\n' "$p"
  done
}
# every igd_faux.inf_1 folder of a prefix's driver store, in any case (Wine finds files case-insensitively, and
# proton-cachyos makes "DriverStore" next to the "driverstore" of other Protons), one per line; none: nothing
#   prefix_shim_dirs <pfx folder>
prefix_shim_dirs() {
  ( shopt -s nocaseglob nullglob
    for d in "$1"/drive_c/windows/system32/[d]riverstore/[f]ilerepository/[i]gd_faux.inf_1; do
      [ -d "$d" ] && printf '%s\n' "$d"
    done )
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
