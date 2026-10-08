#!/bin/bash
# Build the native plugin for desktop Linux x86_64 (#249).
#
# Produces, in Runtime/Plugins/Linux/x86_64/:
#  - libdisplayxr_unity.so — the shipping plugin, with the provider Vulkan backend
#    compiled in (ENABLE_VULKAN);
#  - libdisplayxr_unity_wayland.so — its native-Wayland support library, the only
#    binary that links libwayland-client; the plugin dlopens it for a
#    native-Wayland player only.
#
# Needs cmake and a C++17 compiler; the Wayland library also needs pkg-config and
# libwayland-dev (without them CMake warns and skips it). There is NO Vulkan SDK
# requirement: the Vulkan headers are fetched by CMake and every entry point is
# resolved from libvulkan.so.1 at runtime (displayxr_vk_loader.cpp), so the .so
# carries no hard dependency on a Vulkan ICD being installed.
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
WL="$SCRIPT_DIR/../Runtime/Plugins/Linux/x86_64/libdisplayxr_unity_wayland.so"

echo ""
echo "=== Build complete ==="
ls -la "$SO"
ls -la "$WL" 2>/dev/null || echo "(no $WL: native-Wayland support not built)"

# The build uses -fvisibility=hidden, so an export check is a real gate — a TU
# excluded by a mis-set platform guard shows up here and nowhere else.
echo ""
echo "=== Exported entry points ==="
nm -D --defined-only "$SO" | grep -E 'UnityPluginLoad|XRSDKPreInit|dxr_prov_' | head -20
