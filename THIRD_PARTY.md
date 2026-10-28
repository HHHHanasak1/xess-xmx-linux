# Third-party components

| Component | Where | License | Notes |
|---|---|---|---|
| dxvk-igdext (GameTechDev) | `shim/` | MIT (`shim/LICENSE.md`) | base of the `igdext64.dll` shim; our additions live in `shim/src/dll/` |
| Intel D3D extension SDK headers | `shim/d3dext-sdk/` | MIT, Intel Corporation | shipped with dxvk-igdext |
| Mesa 3D (ANV Vulkan driver) | `patches/`, release archive `lib/` | MIT | `patches/*.patch` apply to the `mesa-26.1.2` release tarball, `patches/mesa-26.3/*.patch` to `mesa-26.3.0-rc1` (archive.mesa3d.org) |
| SPIRV-Tools (Khronos Group) | release archive `lib/bundled/libSPIRV-Tools.so`, built by `tools/build-deps.sh` | Apache-2.0 (`lib/bundled/LICENSE.SPIRV-Tools`) | tag `vulkan-sdk-1.4.350.1`, unmodified; used only on systems without a SPIRV-Tools shared library (`lib/bundled/SOURCES.txt`) |
| libdisplay-info (emersion) | release archive `lib/bundled/libdisplay-info.so.3`, built by `tools/build-deps.sh` | MIT (`lib/bundled/LICENSE.libdisplay-info`) | tag `0.3.0`, unmodified; used only on systems without `libdisplay-info.so.3` |
| Intel Graphics Compiler / compute-runtime (ocloc) | fetched by `tools/get-igc.sh` into `igc/` on the user's machine | MIT | not redistributed by this project |
| Intel XeSS SDK runtime libraries (`libxess.dll`, `libxess_dx11.dll`, `libxess_fg.dll`, `libxell.dll`) | fetched by `tools/get-xess.sh` from github.com/intel/xess into `xess/` on the user's machine | Intel Simplified Software License (`xess/LICENSE.txt`) | not redistributed by this project; `tools/proton-hook.py` puts them into a game's folder only while that game runs, and only when its launch options say `XESS=latest` |
| Intel Graphics Control Library interface | `controllib/ControlLib.c` | this project's own code (MIT) | `ControlLib.dll` implements the few functions XeSS frame generation calls for its override, with the structure layouts of Intel's public `igcl_api.h` (github.com/intel/drivers.gpu.control-library); no Intel code or header is included |
| Intel XeSS / XeSS-FG kernels | not included | Intel XeSS SDK license | The CM kernels are compiled on the user's machine from the SPIR-V that the game's own `libxess.dll` / `libxess_fg.dll` hand to the driver, and cached in `~/.cache/xess-xmx`. Neither the repository nor the release archive contains them. `tools/kernel_map_*.txt` only lists hashes and names. |
| Microsoft fxc (placeholder DXBC compute shaders) | `shim/src/dll/*_dummies.inc`, `tools/gen_*_dummies.py` | tool from the Windows SDK | the generated DXBC blobs are trivial one-store kernels written for this project |
