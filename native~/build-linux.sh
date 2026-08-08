#!/bin/bash
# Build the native plugin for desktop Linux x86_64 (#249).
#
# Produces Runtime/Plugins/Linux/x86_64/libdisplayxr_unity.so — the shipping
# binary, with the provider Vulkan backend compiled in (ENABLE_VULKAN).
#
# Needs only cmake + a C++17 compiler. There is NO Vulkan SDK requirement: the
# Vulkan headers are fetched by CMake and every entry point is resolved from
# libvulkan.so.1 at runtime (displayxr_vk_loader.cpp), so the .so carries no
# hard dependency on a Vulkan ICD being installed.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Incremental rebuild — keep build-linux/ across runs so the FetchContent'd
# OpenXR-SDK / Vulkan-Headers clones are reused. Pass --clean to force fresh.
if [ "${1:-}" = "--clean" ]; then
    echo "=== --clean: removing build-linux/ ==="
    rm -rf build-linux
fi
mkdir -p build-linux
cd build-linux
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j"$(nproc)"

SO="$SCRIPT_DIR/../Runtime/Plugins/Linux/x86_64/libdisplayxr_unity.so"

echo ""
echo "=== Build complete ==="
ls -la "$SO"

# The build uses -fvisibility=hidden, so an export check is a real gate — a TU
# excluded by a mis-set platform guard shows up here and nowhere else.
echo ""
echo "=== Exported entry points ==="
nm -D --defined-only "$SO" | grep -E 'UnityPluginLoad|XRSDKPreInit|dxr_prov_' | head -20
