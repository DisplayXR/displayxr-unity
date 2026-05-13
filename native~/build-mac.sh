#!/bin/bash
# Build native plugin for macOS (Universal: x86_64 + arm64)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Incremental rebuild — keep the build/ directory across runs so the
# FetchContent'd OpenXR-SDK clone is reused. Re-running the build is now
# seconds instead of re-downloading the SDK each time. Pass --clean as the
# first arg to force a fresh build (e.g. after toolchain or CMake changes).
if [ "${1:-}" = "--clean" ]; then
    echo "=== --clean: removing build/ ==="
    rm -rf build
fi
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"
cmake --build . --config Release

echo ""
echo "=== Build complete ==="
ls -la "$SCRIPT_DIR/../Runtime/Plugins/macOS/displayxr_unity.bundle"
