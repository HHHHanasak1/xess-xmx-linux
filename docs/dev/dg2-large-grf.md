# DG2 compute register policy in this fork

The Mesa 26.3 patch series enables the DG2 compute policy by default. Set
`INTEL_DG2_LARGE_GRF=0` to opt out. This changes Mesa userspace only; it does
not require an Xe or i915 kernel-module patch.

The policy applies to non-internal SIMD32 compute shaders on DG2. It retains
the guest subgroup size. Shaders with estimated pressure above 128 registers
and a supported fixed workgroup size first try normal 128-register allocation.
If the initial allocation fails, BRW retries with 256 registers before spilling.
Graphics, mesh/task, internal shaders and variable-size workgroups retain their
existing policy. Shaders compiled for indirect execution-set binding retain
128-register allocation. Ordinary bound compute shaders used without an execution
set select the mode from their compiled register count.

ANV uses the existing XeSS large-GRF tracker for both CM and BRW-generated kernels.
The policy is part of compute shader cache keys. Do not expect existing pipelines
compiled under the other policy to eliminate all first-run compilation.

## Tests

Build the harness and validate its SPIR-V without GPU access:

```sh
bash tools/tests/dg2-large-grf/run.sh --build-only
```

This needs a C++ compiler, Vulkan headers/loader, pkg-config, glslangValidator,
spirv-val and Python 3. Run against a private patched ICD:

```sh
bash tools/tests/dg2-large-grf/run.sh /absolute/path/to/private-intel-icd.json
```

The runner tests the opt-out and default-on policy separately with driver caching
disabled. It checks 4,096 output words across 16 submissions per policy, including
direct and indirect dispatch, secondary command buffers, workgroup barriers and
BLORP buffer fills on a dedicated compute queue. The enabled run must actually
compile a shader using registers above r127 and program both mode transitions.
`INTEL_DG2_GRF_TRACE=1` enables compiler and mode diagnostics.

This is a development fork. Synthetic validation is not broad daily-use or game
validation. DGC execution sets, multiple queues, graphics/compute overlap and the
XeSS CM path still need dedicated regression coverage with this combined policy.

Local A770M/Xe validation passed with Mesa 26.3.0 and Vulkan validation enabled:
both policies produced the expected output, and the enabled run used 256 GRFs.
In the pressure fixture, spills fell from 144 to 74 and scratch from 16 KiB to
8 KiB. A separate local captured shader compiled in 14.1 seconds with the opt-out
and 11.1 seconds with the policy enabled; spills fell from 2,745 to 347. Those
captures are not distributed, and these observations are not runtime FPS results.

## Nix

The overlay in `nix/overlay.nix` applies the whole Mesa 26.3 series to an existing
Mesa 26.3.0 package. With matching nixpkgs:

```sh
nix-build nix/mesa.nix
```

This builds a package and does not activate it. Adding the overlay to a NixOS
configuration and rebuilding/activating the system is a separate step. The overlay
rejects other Mesa versions rather than silently applying unvalidated patches.
