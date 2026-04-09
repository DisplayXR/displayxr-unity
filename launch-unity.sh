#!/bin/bash
# Launch Unity with DisplayXR runtime (simulated display, anaglyph output)
# Usage: ./launch-unity.sh [project-path]
#
# Package path in test project points to:
#   /Users/david.fattal/Documents/GitHub/displayxr-unity
#
# Logs:
#   stderr (native plugin [DisplayXR] + runtime) → displayxr.log (in this directory)
#   Unity Editor.log → ~/Library/Logs/Unity/Editor.log
#
# In a second terminal, run:
#   tail -f /Users/david.fattal/Documents/GitHub/displayxr-unity/displayxr.log

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT="${1:-/Users/david.fattal/Documents/Unity/DisplayXR-test}"
LOGFILE="$SCRIPT_DIR/displayxr.log"

: > "$LOGFILE"   # truncate

# Remove stale lock file (left behind if Unity crashed or was killed)
rm -f "$PROJECT/Temp/UnityLockfile"

XR_RUNTIME_JSON=/Users/david.fattal/Documents/GitHub/displayxr-runtime-pvt/_package/DisplayXR-macOS/openxr_displayxr.json \
SIM_DISPLAY_ENABLE=1 \
SIM_DISPLAY_OUTPUT=anaglyph \
XRT_LOG_LEVEL=debug \
/Applications/Unity/Hub/Editor/6000.3.10f1/Unity.app/Contents/MacOS/Unity \
  -force-metal \
  -disableHubProcessCommunication \
  -projectPath "$PROJECT" \
  >"$LOGFILE" 2>&1
