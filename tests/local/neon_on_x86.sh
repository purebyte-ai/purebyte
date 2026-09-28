#!/bin/sh
# Validates the NEON kernel (engine/src/kernels/neon.cpp) on an x86-64 machine, bit for bit against the scalar
# kernel, by compiling it over SIMDe (https://github.com/simd-everywhere/simde), which implements the NEON intrinsics
# with SSE/AVX. LOCAL tool: CI runs the same tests natively on arm64 (macos-14).
#
#   tests/local/neon_on_x86.sh /path/to/simde [build-dir] [models-dir]
#
# 1. The kernel unit tests of tests/cpp/test_kernels.cpp over the scalar, AVX2 and emulated NEON tables.
# 2. With a models directory (the random models of tests/gen, e.g. build/tests/work/models): the whole engine built
#    with the emulated NEON table, and every model run with --kernel=neon and --kernel=scalar: the outputs, digests of
#    the hidden states included, must be identical.
# The x86 host must have FMA (SIMDe then maps vfmaq_f32 to a fused multiply-add, as on AArch64).
set -eu
SIMDE=${1:?usage: neon_on_x86.sh SIMDE_DIR [BUILD_DIR] [MODELS_DIR]}
OUT=${2:-build/neon-emulation}
MODELS=${3:-}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CXX=${CXX:-g++}
mkdir -p "$OUT/engine"
FLAGS="-std=c++17 -O2 -ffp-contract=off -fno-fast-math -Wall -Wextra -I$ROOT/engine/src -I$ROOT/engine/include"
DEFS="-DPUREBYTE_BUILDING -DPUREBYTE_VERSION=\"1.0.0\" -DPUREBYTE_HAVE_AVX2=1 -DPUREBYTE_HAVE_NEON=1 -DPUREBYTE_NEON_EMULATION=1"
NEON="-mavx2 -mfma -DSIMDE_ENABLE_NATIVE_ALIASES -I$SIMDE -Wno-unused-parameter"

for f in "$ROOT"/engine/src/*/*.cpp; do
    o="$OUT/engine/$(basename "$(dirname "$f")")_$(basename "$f" .cpp).o"
    case "$f" in
        */kernels/avx2.cpp) extra="-mavx2 -mfma" ;;
        */kernels/neon.cpp) extra="$NEON" ;;
        *) extra="" ;;
    esac
    $CXX $FLAGS $DEFS $extra -c "$f" -o "$o"
done
ar rcs "$OUT/libpurebyte_emulated.a" "$OUT"/engine/*.o
$CXX $FLAGS $DEFS -DPUREBYTE_TEST_ALL_KERNELS=1 "$ROOT/tests/cpp/test_main.cpp" "$ROOT/tests/cpp/test_kernels.cpp" \
    "$OUT/libpurebyte_emulated.a" -o "$OUT/kernel_tests" -lpthread
"$OUT/kernel_tests" kernels_

[ -n "$MODELS" ] || exit 0
$CXX $FLAGS $DEFS "$ROOT/tests/tools/model_dump.cpp" "$OUT/libpurebyte_emulated.a" -o "$OUT/model_dump" -lpthread
PYTHON=${PYTHON:-python3}
"$PYTHON" - "$OUT/model_dump" "$MODELS" <<'EOF'
import json, os, subprocess, sys
tool, models = sys.argv[1], sys.argv[2]
failures = 0
for name in sorted(f[:-5] for f in os.listdir(models) if f.endswith(".gguf")):
    extra = ["--query=user", "--query=password"] if name == "query-extraction" else []
    def run(kernel, threads):
        args = [tool, os.path.join(models, name + ".gguf"), os.path.join(models, name + ".inputs.bin"),
                f"--kernel={kernel}", f"--threads={threads}", f"--intra-threads={threads}", "--hidden", *extra]
        out = json.loads(subprocess.run(args, capture_output=True, check=True).stdout)
        out.pop("kernel")
        return out
    same = run("neon", 1) == run("scalar", 1) == run("neon", 3)
    failures += not same
    print(f"{'ok  ' if same else 'FAIL'} {name}: neon (1 and 3 threads) {'==' if same else '!='} scalar, bit for bit")
sys.exit(1 if failures else 0)
EOF
