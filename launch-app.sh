#!/bin/bash
# Launch a built Unity app with DisplayXR runtime (simulated display, anaglyph output)
# Usage: ./launch-app.sh [path-to-app]
#
# Logs:
#   stdout/stderr → displayxr-app.log (in this directory)
#
# In a second terminal, run:
#   tail -f /Users/david.fattal/Documents/GitHub/displayxr-unity/displayxr-app.log

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP="${1:-/Users/david.fattal/Documents/Unity/DisplayXR-test/CubeTest.app}"
LOGFILE="$SCRIPT_DIR/displayxr-app.log"

: > "$LOGFILE"   # truncate

EXECUTABLE="$APP/Contents/MacOS/$(defaults read "$APP/Contents/Info" CFBundleExecutable 2>/dev/null || basename "$APP" .app)"

if [ ! -x "$EXECUTABLE" ]; then
    echo "Error: executable not found at $EXECUTABLE"
    exit 1
fi

# Unity macOS builds expect openxr_loader.dylib (no "lib" prefix) in PlugIns/,
# but the OpenXR package ships it as libopenxr_loader.dylib so it doesn't get
# included in the build. Copy it if missing.
PLUGINS_DIR="$APP/Contents/PlugIns"
if [ ! -f "$PLUGINS_DIR/openxr_loader.dylib" ]; then
    # Try the project's package cache first, fall back to the system DisplayXR runtime
    LOADER_SRC=""
    for candidate in \
        /Users/david.fattal/Documents/Unity/DisplayXR-test/Library/PackageCache/com.unity.xr.openxr*/RuntimeLoaders/osx/libopenxr_loader.dylib \
        /Users/david.fattal/Documents/GitHub/displayxr-runtime/_package/DisplayXR-macOS/lib/libopenxr_loader.dylib; do
        if [ -f "$candidate" ]; then
            LOADER_SRC="$candidate"
            break
        fi
    done
    if [ -n "$LOADER_SRC" ]; then
        echo "Copying OpenXR loader to app bundle: $LOADER_SRC → $PLUGINS_DIR/openxr_loader.dylib"
        cp "$LOADER_SRC" "$PLUGINS_DIR/openxr_loader.dylib"
    else
        echo "Warning: openxr_loader.dylib not found in app bundle and no source to copy from"
    fi
fi

XR_RUNTIME_JSON=/Users/david.fattal/Documents/GitHub/displayxr-runtime/_package/DisplayXR-macOS/openxr_displayxr.json \
SIM_DISPLAY_ENABLE=1 \
SIM_DISPLAY_OUTPUT=anaglyph \
XRT_LOG_LEVEL=debug \
"$EXECUTABLE" \
  >"$LOGFILE" 2>&1
