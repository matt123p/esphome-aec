#!/usr/bin/env bash
# Build and run the aec_speexdsp PC test suite.
#
#   1. generate realistic signals + ground truth (numpy, deterministic)
#   2. sanitizer build (ASan + UBSan) -> memory & undefined-behaviour checks
#   3. Release build (-O2)            -> functional correctness
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
BUILD_DIR="${BUILD_DIR:-$HERE/build}"
OUT_DIR="$BUILD_DIR/out"
mkdir -p "$OUT_DIR"

# Python for signal generation; must have numpy.
# Override with PYTHON=/path/to/python, otherwise fall back to python3.
PYTHON="${PYTHON:-python3}"
if ! "$PYTHON" -c "import numpy" >/dev/null 2>&1; then
    echo "error: $PYTHON cannot import numpy (pip install numpy, or set PYTHON)" >&2
    exit 1
fi

echo "== [1/3] generating test signals =="
"$PYTHON" "$HERE/tools/generate_signals.py"

echo "== [2/3] sanitizer build (ASan + UBSan) =="
cmake -S "$HERE" -B "$BUILD_DIR/asan" -DSANITIZE=ON -DCMAKE_BUILD_TYPE=Debug >"$OUT_DIR/cmake_asan.log"
cmake --build "$BUILD_DIR/asan" -j "$(nproc)" >"$OUT_DIR/build_asan.log" 2>&1

echo "==   running sanitizer suite (slower) =="
set +e
ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0 \
    "$BUILD_DIR/asan/aec_dsp_tests" \
    --label asan --data-dir "$HERE/test_data" --out-dir "$OUT_DIR" --no-outputs \
    2>&1 | tee "$OUT_DIR/asan_run.log"
ASAN_EXIT=${PIPESTATUS[0]}
set -e

echo "== [3/3] release build (functional correctness) =="
cmake -S "$HERE" -B "$BUILD_DIR/release" -DCMAKE_BUILD_TYPE=Release >"$OUT_DIR/cmake_release.log"
cmake --build "$BUILD_DIR/release" -j "$(nproc)" >"$OUT_DIR/build_release.log" 2>&1

echo "==   running release suite =="
set +e
"$BUILD_DIR/release/aec_dsp_tests" \
    --label release --data-dir "$HERE/test_data" --out-dir "$OUT_DIR" \
    2>&1 | tee "$OUT_DIR/release_run.log"
RELEASE_EXIT=${PIPESTATUS[0]}
set -e

echo
echo "Artifacts: $OUT_DIR"
[ "$RELEASE_EXIT" -eq 0 ] && [ "$ASAN_EXIT" -eq 0 ]
