# Third-party components

| Component | Where | License | Notes |
|---|---|---|---|
| dxvk-igdext (GameTechDev) | `shim/` | MIT (`shim/LICENSE.md`) | base of the `igdext64.dll` shim; our additions live in `shim/src/dll/` |
| Intel D3D extension SDK headers | `shim/d3dext-sdk/` | MIT, Intel Corporation | shipped with dxvk-igdext |
| Mesa 3D (ANV Vulkan driver) | `patches/*.patch`, release archive `lib/libvulkan_intel.so` | MIT | patches apply to tag `mesa-26.1.2` |
| Intel Graphics Compiler / compute-runtime (ocloc) | fetched by `tools/get-igc.sh` into `igc/` on the user's machine | MIT | not redistributed by this project |
| Intel XeSS / XeSS-FG kernels | not included | Intel XeSS SDK license | The CM kernels are compiled on the user's machine from the SPIR-V that the game's own `libxess.dll` / `libxess_fg.dll` hand to the driver, and cached in `~/.cache/xess-xmx`. Neither the repository nor the release archive contains them. `tools/kernel_map_*.txt` only lists hashes and names. |
| Microsoft fxc (placeholder DXBC compute shaders) | `shim/src/dll/*_dummies.inc`, `tools/gen_*_dummies.py` | tool from the Windows SDK | the generated DXBC blobs are trivial one-store kernels written for this project |
