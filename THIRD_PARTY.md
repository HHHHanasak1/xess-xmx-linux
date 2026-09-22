# Third-party components

| Component | Where | License | Notes |
|---|---|---|---|
| dxvk-igdext (GameTechDev) | `shim/` | MIT (`shim/LICENSE.md`) | base of the `igdext64.dll` shim; our additions live in `shim/src/dll/` |
| Intel D3D extension SDK headers | `shim/d3dext-sdk/` | MIT, Intel Corporation | shipped with dxvk-igdext |
| Mesa 3D (ANV Vulkan driver) | `patches/anv_cm_injection.patch`, release archive `lib/libvulkan_intel.so` | MIT | patch applies to tag `mesa-26.1.2` |
| Intel Graphics Compiler / compute-runtime (ocloc) | build-time only | MIT | used to compile the kernels; not redistributed |
| Intel XeSS | not included in git | Intel XeSS SDK license | The kernel files in the release archive (`kernels/*.cmk`) are compiled from the CM SPIR-V kernels that the game's own `libxess.dll` hands to the driver. They are provided for convenience only, are specific to XeSS 2.0.2.68 / XeFG 1.3.1.78, and the rights to that code belong to Intel. If you cannot accept that, build your own set from your own copy of the game with the tools in `tools/` (see README). |
| Microsoft fxc (dummy DXBC compute shaders) | `shim/src/dll/xess_dummies.inc`, `tools/gen_all_dummies.py` | tool from the Windows SDK | the generated DXBC blobs are trivial one-store kernels written for this project |
