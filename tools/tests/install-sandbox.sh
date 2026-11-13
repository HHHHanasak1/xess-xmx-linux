#!/bin/bash
# install-sandbox.sh - runs the package's install scripts in a sandbox and checks what they do: install.sh (patched,
# stock, again, as the watcher), tools/switch-driver.sh, tools/session-check.sh and its copy outside the package,
# enable.sh / disable.sh / xmx-launch.sh, xmx-stock.sh, tools/fsr-fg.sh and uninstall.sh.
# The sandbox is a temporary folder with a made-up HOME, a Steam tree (two libraries, three Protons, four prefixes, two
# games) and a package made of this tree's scripts plus stand-ins for the binaries. systemctl, vulkaninfo, pgrep and
# curl are stubs that log their calls: nothing outside the folder is changed, and no network, GPU or root is needed
# (a C compiler is: the stand-in drivers are real, empty shared objects, so that the driver check can load them).
# At the end the Steam libraries must be, file for file, what they would be had the package never been installed.
# Run: bash tools/tests/install-sandbox.sh     (KEEP=1 keeps the sandbox and prints its path)
set -u
TREE="$(cd "$(dirname "$0")/../.." && pwd)"
CC_BIN=${CC:-cc}
command -v "$CC_BIN" >/dev/null || { echo "install-sandbox: needs a C compiler (cc)"; exit 2; }
S=$(mktemp -d "${TMPDIR:-/tmp}/xmx-sandbox.XXXXXX")
if [ -n "${KEEP:-}" ]; then echo "sandbox: $S"; else trap 'rm -rf "$S"' EXIT; fi
export HOME="$S/home"
PKG="$S/pkg dir"            # a space in the path: the scripts quote
ST="$HOME/.local/share/Steam"
L2="$S/lib2"
CALLS="$S/calls.log"
unset XDG_CACHE_HOME XDG_DATA_HOME STEAM_DIR MESA VK_DRIVER_FILES VK_ICD_FILENAMES VK_ADD_LAYER_PATH \
  VK_LOADER_LAYERS_ENABLE VKD3D_DISABLE_EXTENSIONS XMX_LAYER_HELPER XMX_IGC_DIR XMX_DRIVER XMX_NO_ENV XMX_WAIT_PREFIX \
  XMX_APPID XMX_PREFIX XMX_CONF XMX_GAME XMX_STOCK_ICD XMX_ICD_DIRS XMX_CHECK_LIB XMX_CHECK_ICD force_vk_vendor \
  INTC_ALT_DRIVER_EXTENSIONS_PATH 2>/dev/null

checks=0; fails=0
ok() { # <what> <command...>: the command must succeed
  local what=$1; shift; checks=$((checks + 1))
  "$@" >/dev/null 2>&1 || { echo "FAIL: $what"; fails=$((fails + 1)); }
}
no() { # <what> <command...>: the command must fail
  local what=$1; shift; checks=$((checks + 1))
  if "$@" >/dev/null 2>&1; then echo "FAIL: $what"; fails=$((fails + 1)); fi
}
has() { grep -qF -- "$2" "$1"; }          # <file> <text>
line() { grep -qxF -- "$2" "$1"; }        # <file> <whole line>
is() { [ "$(cat "$1" 2>/dev/null)" = "$2" ]; }   # <file> <content>
run() { # <output file> <command...>: stdout and stderr into the file, the status returned
  local out=$1; shift
  "$@" > "$out" 2>&1 < /dev/null
}

# ---- the package: this tree's scripts, stand-ins for the binaries
mkdir -p "$PKG/lib/bundled" "$PKG/igc/neo/bin" "$PKG/igc/igc/local/lib" "$PKG/xess" "$PKG/ffx" "$PKG/ffxfg" "$PKG/layer" "$S/bin" "$S/icd"
cp "$TREE"/install.sh "$TREE"/uninstall.sh "$TREE"/enable.sh "$TREE"/disable.sh "$TREE"/xmx-launch.sh "$TREE"/xmx-stock.sh "$PKG/"
mkdir "$PKG/tools" && cp "$TREE"/tools/*.sh "$TREE"/tools/*.py "$PKG/tools/"
cp "$TREE/layer/gpu-ids.txt" "$TREE/layer/VkLayer_xess_xmx.json" "$PKG/layer/"
printf 'MZ the shim, IGDEXT_TRACE\n' > "$PKG/igdext64.dll"
printf 'MZ the stand-in for the Control Library, XESS_FG\n' > "$PKG/ControlLib.dll"
driver() { # <file> <text>: a shared object that loads and holds the text the scripts look for in a driver
  printf 'const char xmx_test_driver[] = "%s";\n' "$2" | "$CC_BIN" -shared -fPIC -x c - -o "$1"
}
for v in 26.1.2 26.3.0 stock-26.3.0; do
  mkdir -p "$PKG/lib/mesa-$v"
  driver "$PKG/lib/mesa-$v/libvulkan_intel.so" "Mesa ${v#stock-} /usr/share/drirc.d cmk2-c1" || { echo "install-sandbox: the C compiler failed"; exit 2; }
done
driver "$PKG/lib/libvulkan_intel.so" "xess-xmx: using the Mesa"
echo 26.3.0 > "$PKG/lib/active"
printf '#!/bin/sh\nexit 0\n' > "$PKG/igc/neo/bin/ocloc-25.18.1"; chmod +x "$PKG/igc/neo/bin/ocloc-25.18.1"
touch "$PKG/igc/igc/local/lib/libigc.so.2"
for f in libxess.dll libxess_fg.dll libxell.dll libxess_dx11.dll; do echo "the SDK's $f" > "$PKG/xess/$f"; done
for f in amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll amd_fidelityfx_framegeneration_dx12.dll; do echo "AMD's $f" > "$PKG/ffx/$f"; done
for f in amd_fidelityfx_framegeneration_dx12.dll amd_fidelityfx_upscaler_dx12.dll dxgi.dll; do echo "the package's $f" > "$PKG/ffxfg/$f"; done
echo layer > "$PKG/layer/libVkLayer_xess_xmx.so"

# ---- stubs: every call is logged; curl must never be called (no downloads: the package has everything)
cat > "$S/bin/systemctl" <<EOF
#!/bin/sh
echo "systemctl \$*" >> "$CALLS"
exit 0
EOF
cat > "$S/bin/vulkaninfo" <<EOF
#!/bin/sh
echo "vulkaninfo \$*" >> "$CALLS"
printf 'deviceName = Intel(R) Graphics (PTL)\ndriverInfo = Mesa 26.3.0\n'
EOF
for c in curl pgrep; do printf '#!/bin/sh\necho "%s $*" >> "%s"\nexit 1\n' "$c" "$CALLS" > "$S/bin/$c"; done
chmod +x "$S/bin"/*
export PATH="$S/bin:$PATH"
: > "$CALLS"

# ---- Steam: the main library and a second one. Protons: one with its own igdext64.dll, one without (and with the
# user's own user_settings.py), one in compatibilitytools.d. Prefixes: 111 with a stock igdext64.dll, 222 (second
# library) with "DriverStore" and somebody else's ControlLib.dll, 333 without the driver store folder
IGD=files/lib/wine/igdext/x86_64-windows
FAUX=drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1
mkdir -p "$ST/steamapps/common/Proton 10.0/$IGD" "$ST/steamapps/common/Proton 11.0" "$ST/compatibilitytools.d/GE-Proton10-20/$IGD" \
  "$ST/steamapps/compatdata/111/pfx/$FAUX" "$ST/steamapps/compatdata/111/pfx/drive_c/igdext_kernels" \
  "$L2/steamapps/compatdata/222/pfx/drive_c/windows/system32/DriverStore/FileRepository/igd_faux.inf_1" \
  "$ST/steamapps/compatdata/333/pfx/drive_c/windows/system32" "$L2/steamapps/common/Game Two/bin" "$L2/steamapps/common/Game [3] x*y/bin"
printf '"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n\t"1"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n}\n' "$ST" "$L2" > "$ST/steamapps/libraryfolders.vdf"
touch "$ST/steam.sh" "$ST/steamapps/common/Proton 10.0/proton" "$ST/steamapps/common/Proton 11.0/proton" "$ST/compatibilitytools.d/GE-Proton10-20/proton"
echo "stock igdext of Proton 10" > "$ST/steamapps/common/Proton 10.0/$IGD/igdext64.dll"
echo "stock igdext of GE-Proton" > "$ST/compatibilitytools.d/GE-Proton10-20/$IGD/igdext64.dll"
echo 'user_settings = {"WINEDEBUG": "+timestamp"}' > "$ST/steamapps/common/Proton 11.0/user_settings.py"
echo "stock igdext in prefix 111" > "$ST/steamapps/compatdata/111/pfx/$FAUX/igdext64.dll"
echo failed > "$ST/steamapps/compatdata/111/pfx/drive_c/igdext_kernels/compile_failed.txt"
echo "somebody else's ControlLib" > "$L2/steamapps/compatdata/222/pfx/drive_c/windows/system32/ControlLib.dll"
printf '"AppState"\n{\n\t"appid"\t"222"\n\t"installdir"\t"Game Two"\n}\n' > "$L2/steamapps/appmanifest_222.acf"
for g in "Game Two" "Game [3] x*y"; do
  echo "the game's frame generation" > "$L2/steamapps/common/$g/bin/amd_fidelityfx_framegeneration_dx12.dll"
  echo "the game's upscaler" > "$L2/steamapps/common/$g/bin/amd_fidelityfx_upscaler_dx12.dll"
done
# the reference: the libraries as they are without the package. What the test itself changes below (a Proton update, a
# new prefix folder) is done to the reference too; uninstall.sh deletes the prefixes' igdext_kernels folders
mkdir "$S/ref" && cp -a "$ST" "$S/ref/Steam" && cp -a "$L2" "$S/ref/lib2"
rm -rf "$S/ref/Steam/steamapps/compatdata/111/pfx/drive_c/igdext_kernels"
P10="$ST/steamapps/common/Proton 10.0"; GE="$ST/compatibilitytools.d/GE-Proton10-20"
P111="$ST/steamapps/compatdata/111/pfx"; P222="$L2/steamapps/compatdata/222/pfx"; P333="$ST/steamapps/compatdata/333/pfx"
ENVF="$HOME/.config/environment.d/50-xess-xmx.conf"; UNITS="$HOME/.config/systemd/user"; O="$S/out"; mkdir "$O"
tree_state() { ( cd "$S" && find home/.local home/.config lib2 \( -type f -o -type l \) -exec sha256sum {} + 2>/dev/null | sort -k2 ); }

# ---- 1. install.sh, the patched driver
ok "install.sh runs" run "$O/install" env XMX_DRIVER=patched bash "$PKG/install.sh"
ok "environment file" test -f "$ENVF"
ok "environment: the package's ICD first" grep -q "^VK_DRIVER_FILES=$PKG/run/intel_icd.json\(:\|\$\)" "$ENVF"
ok "environment: vkd3d-proton extensions" line "$ENVF" "VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer,VK_EXT_descriptor_heap,VK_KHR_present_wait2"
ok "environment: kernel folder" line "$ENVF" "ANV_CM_KERNEL_DIR=$PKG/kernels"
ok "environment: compile script" line "$ENVF" "ANV_CM_HELPER=$PKG/tools/cm-compile.sh"
ok "environment: vendor id" line "$ENVF" "force_vk_vendor=0"
ok "environment: the shim's folder" line "$ENVF" "INTC_ALT_DRIVER_EXTENSIONS_PATH='C:\\windows\\system32\\driverstore\\filerepository\\igd_faux.inf_1'"
no "environment: no stock-mode variables" grep -q "^MESA=\|^VK_ADD_LAYER_PATH=\|^XMX_LAYER_HELPER=" "$ENVF"
ok "ICD file names the package's library" has "$PKG/run/intel_icd.json" "\"library_path\": \"$PKG/lib/libvulkan_intel.so\""
ok "mode kept for later runs" is "$PKG/run/driver-mode" patched
ok "Proton 10: shim in" cmp "$PKG/igdext64.dll" "$P10/$IGD/igdext64.dll"
ok "Proton 10: original kept" is "$P10/$IGD/igdext64.dll.stock" "stock igdext of Proton 10"
ok "GE-Proton: shim in" cmp "$PKG/igdext64.dll" "$GE/$IGD/igdext64.dll"
ok "prefix 111: shim in" cmp "$PKG/igdext64.dll" "$P111/$FAUX/igdext64.dll"
ok "prefix 111: original kept" is "$P111/$FAUX/igdext64.dll.stock" "stock igdext in prefix 111"
ok "prefix 111: ControlLib.dll in" cmp "$PKG/ControlLib.dll" "$P111/drive_c/windows/system32/ControlLib.dll"
ok "prefix 111: failed-compile marker gone" test ! -e "$P111/drive_c/igdext_kernels/compile_failed.txt"
ok "prefix 222 (DriverStore): shim in" cmp "$PKG/igdext64.dll" "$P222/drive_c/windows/system32/DriverStore/FileRepository/igd_faux.inf_1/igdext64.dll"
ok "prefix 222: no second driver store folder made" test ! -e "$P222/drive_c/windows/system32/driverstore"
ok "prefix 222: somebody else's ControlLib.dll stays" is "$P222/drive_c/windows/system32/ControlLib.dll" "somebody else's ControlLib"
ok "prefix 333: no driver store folder made" test ! -e "$P333/drive_c/windows/system32/driverstore"
for p in "$P10" "$ST/steamapps/common/Proton 11.0" "$GE"; do
  ok "hook in ${p##*/}" has "$p/user_settings.py" "xess-xmx proton hook"
done
ok "Proton 11: the user's settings kept" is "$ST/steamapps/common/Proton 11.0/user_settings.user.py" 'user_settings = {"WINEDEBUG": "+timestamp"}'
ok "Proton 11: the hook file runs and hands on the user's settings" python3 -c '
import runpy, sys
d = runpy.run_path(sys.argv[1])
assert d["user_settings"].get("WINEDEBUG") == "+timestamp", d["user_settings"]' "$ST/steamapps/common/Proton 11.0/user_settings.py"
ok "watcher: Proton 10's igdext64.dll" line "$UNITS/xess-xmx-shim.path" "PathChanged=$P10/$IGD/igdext64.dll"
ok "watcher: prefixes of the second library" line "$UNITS/xess-xmx-shim.path" "PathModified=$L2/steamapps/compatdata"
ok "watcher: compatibilitytools.d" line "$UNITS/xess-xmx-shim.path" "PathModified=$ST/compatibilitytools.d"
ok "watcher: runs install.sh" line "$UNITS/xess-xmx-shim.service" "ExecStart=/bin/bash \"$PKG/install.sh\""
ok "watcher: without downloads and environment" line "$UNITS/xess-xmx-shim.service" "Environment=XMX_NO_ENV=1 XMX_WAIT_PREFIX=90"
ok "session check: unit" line "$UNITS/xess-xmx-check.service" "ExecStart=/bin/bash \"$HOME/.config/xess-xmx/session-check.sh\""
ok "session check: its copy outside the package" test -f "$HOME/.config/xess-xmx/session-check.sh"
ok "driver check passed" has "$O/install" "== driver check: loads fine"
ok "driver status" grep -q "^ok .*Mesa 26.3.0" "$HOME/.cache/xess-xmx/driver-status"
ok "systemd: session environment set" grep -q "^systemctl --user set-environment VK_DRIVER_FILES=$PKG/run/intel_icd.json" "$CALLS"
ok "systemd: watcher enabled" line "$CALLS" "systemctl --user enable xess-xmx-shim.path"
ok "systemd: session check enabled" line "$CALLS" "systemctl --user enable xess-xmx-check.service"
no "nothing downloaded" grep -q "^curl" "$CALLS"

# ---- 2. again: nothing to do, nothing changed
tree_state > "$O/state1"; cp "$ENVF" "$O/env1"
ok "install.sh again" run "$O/install2" bash "$PKG/install.sh"
ok "again: no file updated" has "$O/install2" "== shim: 0 file(s) updated"
tree_state > "$O/state2"
ok "again: every file as it was" cmp "$O/state1" "$O/state2"

# ---- 3. the default among the patched drivers (lib/active), by prefix
ok "switch-driver.sh 26.1" run "$O/switch" bash "$PKG/tools/switch-driver.sh" 26.1
ok "switch: lib/active" is "$PKG/lib/active" 26.1.2
ok "switch: list" run "$O/list" bash "$PKG/tools/switch-driver.sh"
ok "switch: the default is marked" line "$O/list" "* 26.1.2 (default)"
ok "switch: the unpatched driver is listed" line "$O/list" "  stock-26.3.0"
no "switch: no such driver" run "$O/switch2" bash "$PKG/tools/switch-driver.sh" 99
ok "switch: lib/active stays" is "$PKG/lib/active" 26.1.2
ok "switch-driver.sh 26.3" run "$O/switch3" bash "$PKG/tools/switch-driver.sh" 26.3
ok "switch: back" is "$PKG/lib/active" 26.3.0

# ---- 4. stock mode: the unpatched driver and the layer
ok "install.sh, stock mode" run "$O/install3" env XMX_DRIVER=stock bash "$PKG/install.sh"
ok "stock: unpatched Mesa" line "$ENVF" "MESA=stock-26.3.0"
ok "stock: layer folder" line "$ENVF" "VK_ADD_LAYER_PATH=$PKG/layer"
ok "stock: layer on" line "$ENVF" "VK_LOADER_LAYERS_ENABLE=VK_LAYER_XESS_xmx"
ok "stock: compile script" line "$ENVF" "XMX_LAYER_HELPER=$PKG/tools/cm-compile.sh"
ok "stock: compiler" line "$ENVF" "XMX_IGC_DIR=$PKG/igc"
ok "stock: mode kept" is "$PKG/run/driver-mode" stock
ok "stock: what the session check tests" is "$PKG/run/session-driver" stock
ok "stock: the driver check tested the unpatched driver" has "$HOME/.cache/xess-xmx/driver-status" "$PKG/lib/mesa-stock-26.3.0/libvulkan_intel.so"
ok "install.sh, mode by GPU again" run "$O/install4" env XMX_DRIVER=auto bash "$PKG/install.sh"
ok "auto: the kept mode is gone" test ! -e "$PKG/run/driver-mode"
ok "install.sh, patched again" run "$O/install5" env XMX_DRIVER=patched bash "$PKG/install.sh"
ok "patched again: the environment file as in the first run" cmp "$O/env1" "$ENVF"

# ---- 5. the session check, when a driver stops loading
: > "$CALLS"
ok "session check: fine" run "$O/check" bash "$PKG/tools/session-check.sh"
no "session check: fine, the session is left alone" grep -q "set-environment" "$CALLS"
cp "$PKG/lib/mesa-26.3.0/libvulkan_intel.so" "$S/good.so"
echo "not a library" > "$PKG/lib/mesa-26.3.0/libvulkan_intel.so"
no "session check, dry run: the broken driver fails" run "$O/check2" bash "$PKG/tools/session-check.sh" --dry-run
ok "session check, dry run: status" grep -q "^FAILED " "$HOME/.cache/xess-xmx/driver-status"
ok "session check, dry run: nothing switched" is "$PKG/lib/active" 26.3.0
ok "session check: another driver of the package takes over" run "$O/check3" bash "$PKG/tools/session-check.sh"
ok "session check: lib/active" is "$PKG/lib/active" 26.1.2
ok "session check: says so" has "$O/check3" "switched to the package's Mesa 26.1.2 driver"
no "session check: the session keeps the package's driver" grep -q "set-environment" "$CALLS"
cp "$PKG/lib/mesa-26.1.2/libvulkan_intel.so" "$S/good2.so"
echo "not a library" > "$PKG/lib/mesa-26.1.2/libvulkan_intel.so"
ok "session check: no driver loads, the session goes on" run "$O/check4" bash "$PKG/tools/session-check.sh"
ok "session check: the session gets the system's drivers back" grep -q "^systemctl --user set-environment VK_DRIVER_FILES=.* VKD3D_DISABLE_EXTENSIONS= ANV_CM_KERNEL_DIR= ANV_CM_HELPER=\$" "$CALLS"
no "session check: the package's ICD is not in that list" grep -q "set-environment VK_DRIVER_FILES=[^ ]*$PKG" "$CALLS"
cp "$S/good.so" "$PKG/lib/mesa-26.3.0/libvulkan_intel.so"; cp "$S/good2.so" "$PKG/lib/mesa-26.1.2/libvulkan_intel.so"
echo 26.3.0 > "$PKG/lib/active"
# the copy outside the package: runs the package's check, and without the package folder points the session back
ok "session check copy: runs the package's" run "$O/stub" bash "$HOME/.config/xess-xmx/session-check.sh" --dry-run
mv "$PKG" "$PKG.gone"; : > "$CALLS"
no "session check copy, package gone, dry run" run "$O/stub2" bash "$HOME/.config/xess-xmx/session-check.sh" --dry-run
ok "session check copy, package gone: says so" has "$O/stub2" "package folder $PKG not found"
ok "session check copy, package gone: the session goes on" run "$O/stub3" bash "$HOME/.config/xess-xmx/session-check.sh"
ok "session check copy, package gone: the system's drivers" grep -q "^systemctl --user set-environment VK_DRIVER_FILES=.* MESA= VK_ADD_LAYER_PATH= VK_LOADER_LAYERS_ENABLE=\$" "$CALLS"
mv "$PKG.gone" "$PKG"

# ---- 6. the watcher's run: a Proton update brought its own igdext64.dll back, a prefix got its driver store folder
for t in "$ST" "$S/ref/Steam"; do
  echo "stock igdext of Proton 10, updated" > "$t/steamapps/common/Proton 10.0/$IGD/igdext64.dll"
  mkdir -p "$t/steamapps/compatdata/333/pfx/$FAUX"
done
ok "install.sh as the watcher" run "$O/watch" env XMX_NO_ENV=1 XMX_WAIT_PREFIX=1 bash "$PKG/install.sh"
ok "watcher: shim back in Proton 10" cmp "$PKG/igdext64.dll" "$P10/$IGD/igdext64.dll"
ok "watcher: the new original kept" is "$P10/$IGD/igdext64.dll.stock" "stock igdext of Proton 10, updated"
ok "watcher: shim in the new prefix" cmp "$PKG/igdext64.dll" "$P333/$FAUX/igdext64.dll"
ok "watcher: ControlLib.dll in the new prefix" cmp "$PKG/ControlLib.dll" "$P333/drive_c/windows/system32/ControlLib.dll"
ok "watcher: the environment file is not written" cmp "$O/env1" "$ENVF"

# ---- 7. per game: enable.sh, xmx-launch.sh, disable.sh, on a prefix of another launcher
OTHER="$S/other/pfx"; CONF="$S/other/launch.conf"; mkdir -p "$OTHER/drive_c/windows/system32"
echo "# the user's own line" > "$CONF"
ok "enable.sh" run "$O/enable" env XMX_PREFIX="$OTHER" XMX_CONF="$CONF" XMX_GAME=game.exe bash "$PKG/enable.sh"
ok "enable: shim in" cmp "$PKG/igdext64.dll" "$OTHER/$FAUX/igdext64.dll"
ok "enable: ControlLib.dll in" cmp "$PKG/ControlLib.dll" "$OTHER/drive_c/windows/system32/ControlLib.dll"
ok "enable: the user's line stays" line "$CONF" "# the user's own line"
ok "enable: block" line "$CONF" "export VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer,VK_EXT_descriptor_heap,VK_KHR_present_wait2"
ok "xmx-launch.sh" run "$O/launch" env XMX_CONF="$CONF" bash "$PKG/xmx-launch.sh" env
ok "launch: the package's ICD first" grep -q "^VK_DRIVER_FILES=$PKG/run/intel_icd.json\(:\|\$\)" "$O/launch"
ok "launch: kernel folder (a path with a space)" line "$O/launch" "ANV_CM_KERNEL_DIR=$PKG/kernels"
ok "launch: compile script" line "$O/launch" "ANV_CM_HELPER=$PKG/tools/cm-compile.sh"
ok "launch: the shim's folder" line "$O/launch" 'INTC_ALT_DRIVER_EXTENSIONS_PATH=C:\windows\system32\driverstore\filerepository\igd_faux.inf_1'
no "enable.sh without a game" run "$O/enable2" bash "$PKG/enable.sh"
ok "disable.sh" run "$O/disable" env XMX_PREFIX="$OTHER" XMX_CONF="$CONF" bash "$PKG/disable.sh"
ok "disable: shim gone" test ! -e "$OTHER/$FAUX/igdext64.dll"
ok "disable: ControlLib.dll gone" test ! -e "$OTHER/drive_c/windows/system32/ControlLib.dll"
ok "disable: only the user's line is left" is "$CONF" "# the user's own line"

# ---- 8. xmx-stock.sh: the system's driver and the layer for one game
printf '{ "ICD": { "library_path": "/usr/lib/libvulkan_intel.so" } }\n' > "$S/icd/intel_icd.x86_64.json"
printf '{ "ICD": { "library_path": "/usr/lib32/libvulkan_intel.so" } }\n' > "$S/icd/intel_icd.i686.json"
printf '{ "ICD": { "library_path": "/usr/lib/libvulkan_radeon.so" } }\n' > "$S/icd/radeon_icd.x86_64.json"
ok "xmx-stock.sh" run "$O/stock" env XMX_ICD_DIRS="$S/icd" bash "$PKG/xmx-stock.sh" env
ok "stock wrapper: Intel first, then the others" line "$O/stock" "VK_DRIVER_FILES=$S/icd/intel_icd.x86_64.json:$S/icd/intel_icd.i686.json:$S/icd/radeon_icd.x86_64.json"
ok "stock wrapper: layer" line "$O/stock" "VK_LOADER_LAYERS_ENABLE=VK_LAYER_XESS_xmx"
ok "stock wrapper: extensions" line "$O/stock" "VKD3D_DISABLE_EXTENSIONS=VK_EXT_descriptor_buffer,VK_EXT_descriptor_heap,VK_KHR_present_wait2"
ok "xmx-stock.sh in a session of the package" run "$O/stock2" env XMX_ICD_DIRS="$S/icd" VKD3D_DISABLE_EXTENSIONS=VK_EXT_foo \
  VK_DRIVER_FILES="$PKG/run/intel_icd.json:$S/icd/radeon_icd.x86_64.json" bash "$PKG/xmx-stock.sh" env
ok "stock wrapper: the package's ICD left out" line "$O/stock2" "VK_DRIVER_FILES=$S/icd/intel_icd.x86_64.json:$S/icd/intel_icd.i686.json:$S/icd/radeon_icd.x86_64.json"
ok "stock wrapper: the user's extensions kept" line "$O/stock2" "VKD3D_DISABLE_EXTENSIONS=VK_EXT_foo,VK_EXT_descriptor_buffer,VK_EXT_descriptor_heap,VK_KHR_present_wait2"
ok "xmx-stock.sh without a system driver: the game still starts" run "$O/stock3" env XMX_ICD_DIRS="$S/none" bash "$PKG/xmx-stock.sh" echo started
ok "stock wrapper: started unchanged" line "$O/stock3" started

# ---- 9. fsr-fg.sh: by app id, and by a folder whose name has pattern characters
for g in 222 "$L2/steamapps/common/Game [3] x*y"; do
  case "$g" in 222) G="$L2/steamapps/common/Game Two" ;; *) G=$g ;; esac
  n=${G##*/}
  ok "fsr-fg all ($n)" run "$O/fsr" bash "$PKG/tools/fsr-fg.sh" all "$g"
  ok "fsr-fg ($n): names inside the game folder" line "$O/fsr" "   bin/amd_fidelityfx_framegeneration_dx12.dll <- XeSS-FG"
  ok "fsr-fg ($n): frame generation swapped" cmp "$PKG/ffxfg/amd_fidelityfx_framegeneration_dx12.dll" "$G/bin/amd_fidelityfx_framegeneration_dx12.dll"
  ok "fsr-fg ($n): upscaler swapped" cmp "$PKG/ffxfg/amd_fidelityfx_upscaler_dx12.dll" "$G/bin/amd_fidelityfx_upscaler_dx12.dll"
  ok "fsr-fg ($n): original kept" is "$G/bin/amd_fidelityfx_framegeneration_dx12.dll.ffxfg-orig" "the game's frame generation"
  ok "fsr-fg ($n): XeSS libraries added" cmp "$PKG/xess/libxess_fg.dll" "$G/bin/libxess_fg.dll"
  ok "fsr-fg status ($n)" run "$O/fsr2" bash "$PKG/tools/fsr-fg.sh" status "$g"
  ok "fsr-fg ($n): status" line "$O/fsr2" "bin/amd_fidelityfx_framegeneration_dx12.dll: XeSS-FG (fsr-fg.sh on)"
done
ok "fsr-fg off" run "$O/fsr3" bash "$PKG/tools/fsr-fg.sh" off 222
ok "fsr-fg off: names inside the game folder" line "$O/fsr3" "   restored bin/amd_fidelityfx_framegeneration_dx12.dll"
ok "fsr-fg off: the game's own files" diff -r "$L2/steamapps/common/Game Two" "$S/ref/lib2/steamapps/common/Game Two"
no "fsr-fg without arguments" run "$O/fsr4" bash "$PKG/tools/fsr-fg.sh"
ok "fsr-fg: usage to its last line" has "$O/fsr4" 'uninstall.sh runs "off" for every game.'
# the other game stays switched: uninstall.sh has to put it back

# ---- 10. uninstall.sh: the libraries as if the package had never been there
ok "uninstall.sh" run "$O/uninstall" bash "$PKG/uninstall.sh"
ok "uninstall: environment file gone" test ! -e "$ENVF"
ok "uninstall: units gone" test -z "$(ls "$UNITS" 2>/dev/null)"
ok "uninstall: session check copy gone" test ! -e "$HOME/.config/xess-xmx"
ok "uninstall: session variables dropped" grep -q "^systemctl --user unset-environment VK_DRIVER_FILES " "$CALLS"
ok "uninstall: main library untouched by the package" diff -r "$ST" "$S/ref/Steam"
ok "uninstall: second library untouched by the package" diff -r "$L2" "$S/ref/lib2"
ok "uninstall.sh again" run "$O/uninstall2" bash "$PKG/uninstall.sh"
ok "uninstall again: libraries as before" diff -r "$ST" "$S/ref/Steam"
no "nothing downloaded in any run" grep -q "^curl" "$CALLS"

if [ "$fails" = 0 ]; then echo "install-sandbox: all $checks checks passed"; else echo "install-sandbox: $fails of $checks checks FAILED (KEEP=1 keeps the sandbox)"; fi
[ "$fails" = 0 ]
