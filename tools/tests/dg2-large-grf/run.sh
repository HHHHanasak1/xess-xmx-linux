#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
OUT=${DG2_TEST_BUILD_DIR:-$ROOT/.build}
mkdir -p "$OUT"
glslangValidator -V --target-env vulkan1.3 "$ROOT/pressure.comp" -o "$OUT/pressure.spv"
spirv-val --target-env vulkan1.3 "$OUT/pressure.spv"
"${CXX:-c++}" -std=c++17 -O2 "$ROOT/execute.cpp" -o "$OUT/execute" $(pkg-config --cflags --libs vulkan)
if [ "${1:-}" = --build-only ]; then
  echo 'Built the DG2 execution harness and validated its SPIR-V; GPU execution was not tested.'
  exit 0
fi
if [ "$#" != 1 ]; then
  echo "usage: $0 --build-only | /absolute/path/to/private-intel-icd.json" >&2
  exit 2
fi
export VK_DRIVER_FILES=$1 MESA_SHADER_CACHE_DISABLE=true INTEL_DG2_GRF_TRACE=1
INTEL_DG2_LARGE_GRF=0 "$OUT/execute" "$OUT/pressure.spv" > "$OUT/disabled.log" 2>&1
cat "$OUT/disabled.log"
env -u INTEL_DG2_LARGE_GRF "$OUT/execute" "$OUT/pressure.spv" > "$OUT/default.log" 2>&1
cat "$OUT/default.log"
python3 - "$OUT/default.log" "$OUT/disabled.log" <<'PY'
from pathlib import Path
import re
import sys
enabled, disabled = (Path(p).read_text() for p in sys.argv[1:])
assert 'PASS:' in enabled and 'PASS:' in disabled
assert any(int(r) > 128 for r in re.findall(r'DG2_GRF_PROBE budget=256 simd=32 grf=(\d+)', enabled)), 'The test did not exercise registers above r127'
assert 'INTEL_LARGE_GRF_MODE enable=1' in enabled, 'Large mode was not programmed'
assert 'INTEL_LARGE_GRF_MODE enable=0' in enabled, 'Normal mode was not restored'
assert 'budget=256' not in disabled, 'The opt-out did not take effect'
print('PASS: default-on upper-register execution and opt-out verified')
PY
