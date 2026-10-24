#!/bin/bash
# Build the native plugin for Android arm64-v8a (Vulkan) — #418.
#
# Produces, in Runtime/Plugins/Android/arm64-v8a/:
#  - libdisplayxr_unity.so  — the plugin, Vulkan provider backend (ENABLE_VULKAN);
#  - libopenxr_loader.so    — the Khronos OpenXR loader for Android it links
#                             against (fetched from Maven Central, pinned below);
#  - libVkLayer_DXR_queue_lock.so — the DisplayXR runtime's per-queue Vulkan layer
#                             (#418 P3b), taken from the runtime's released client
#                             AAR (pinned below). Not linked: the Android Vulkan
#                             loader discovers it by file name in the app's native
#                             lib dir, and the runtime enables it itself in
#                             xrCreateVulkanInstanceKHR (runtime android-build-guide,
#                             "The queue-lock layer ships in the client AAR").
# All three are gitignored (CI builds/fetches them, like the Linux .so); their .meta
# files are committed.
#
# NDK: $ANDROID_NDK_ROOT (or $ANDROID_NDK_HOME), else the NDK bundled with a
# Unity Hub editor install (macOS path below). Needs cmake + ninja (or make),
# curl and unzip. Runs on macOS and Linux. Pass --clean to start fresh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# --- Pins -------------------------------------------------------------------
# The loader is pinned on its own, NOT to the OpenXR-SDK header tag in CMakeLists.txt
# (release-1.0.34). Measured on device (#418 P3a): the 1.0.34 Android loader queries
# the runtime broker at .../runtimes/active/0, a URI shape the DisplayXR runtime's
# broker (OpenXRRuntimeBroker.kt) does not recognise -> null cursor ->
# xrCreateInstance fails with nothing else in the log. 1.1.x queries
# .../runtimes/active, which is what every DisplayXR client ships (the runtime's
# cube_handle_vk_android pins 1.1.63). A newer loader serves an app built against
# older headers (the app's requested apiVersion 1.0.x is accepted), and 1.1.63 is
# also 16 KB-page aligned. Keep this equal to the runtime's client pin.
OPENXR_LOADER_VERSION="${OPENXR_LOADER_VERSION:-1.1.63}"
ANDROID_ABI=arm64-v8a
ANDROID_PLATFORM="${ANDROID_PLATFORM:-29}"   # Android 10; Vulkan 1.1 + AHardwareBuffer
MAVEN="https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android"

# VK_LAYER_DXR_queue_lock: the runtime release whose DisplayXR-Client AAR we take the
# layer from, and that AAR's sha256 (GitHub release assets publish no checksum, so it
# is pinned here). Why the layer has to ride in the APP and not the runtime APK: on
# Adreno the app's and the runtime's vkQueueSubmit calls share one GSL context, and the
# Android Vulkan loader only searches the app's own lib path for layers. With it live
# the runtime turns on mid-frame fills + the non-blocking xrEndFrame hand-off
# (runtime #1905/#1910/#1923); without it everything works, at app rate, with a
# blocking hand-off. We ship the .so alone, not the AAR: the AAR's other content is an
# androidx.startup wake initializer, a dependency a stock Unity Gradle build does not
# carry (the runtime queries it needs are already in DisplayXR.androidlib).
QUEUE_LOCK_RUNTIME_VERSION="${QUEUE_LOCK_RUNTIME_VERSION:-2.47.0}"
QUEUE_LOCK_AAR_SHA256="${QUEUE_LOCK_AAR_SHA256:-38f716a17d262d8e3ac26f9c6cc52da306377941a7e6fc4f4b841d7e7a20daa0}"
QUEUE_LOCK_URL="https://github.com/DisplayXR/displayxr-runtime/releases/download/v${QUEUE_LOCK_RUNTIME_VERSION}/DisplayXR-Client-${QUEUE_LOCK_RUNTIME_VERSION}.aar"

if [ "${1:-}" = "--clean" ]; then
    echo "=== --clean: removing build-android/ ==="
    rm -rf build-android
fi

# --- NDK --------------------------------------------------------------------
NDK="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}"
if [ -z "$NDK" ]; then
    for cand in /Applications/Unity/Hub/Editor/*/PlaybackEngines/AndroidPlayer/NDK \
                "$HOME"/Unity/Hub/Editor/*/Editor/Data/PlaybackEngines/AndroidPlayer/NDK; do
        [ -f "$cand/build/cmake/android.toolchain.cmake" ] && NDK="$cand"
    done
fi
if [ -z "$NDK" ] || [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
    echo "ERROR: no Android NDK found. Set ANDROID_NDK_ROOT (r27c recommended)." >&2
    exit 1
fi
echo "=== NDK: $NDK ($(sed -n 's/^Pkg.Revision = //p' "$NDK/source.properties" 2>/dev/null)) ==="

# --- Khronos OpenXR loader for Android --------------------------------------
# The AAR is UNTRUSTED downloaded data: it lands in its own empty directory, only
# the one expected file is copied out, and nothing in it is executed.
LOADER_DIR="$SCRIPT_DIR/build-android/openxr_loader_${OPENXR_LOADER_VERSION}"
LOADER_SO="$LOADER_DIR/libopenxr_loader.so"
if [ ! -f "$LOADER_SO" ]; then
    echo "=== Fetching openxr_loader_for_android $OPENXR_LOADER_VERSION ==="
    rm -rf "$LOADER_DIR"
    mkdir -p "$LOADER_DIR/extract"
    AAR="$LOADER_DIR/openxr_loader_for_android-${OPENXR_LOADER_VERSION}.aar"
    curl -fsSL -o "$AAR" "$MAVEN/$OPENXR_LOADER_VERSION/openxr_loader_for_android-${OPENXR_LOADER_VERSION}.aar"
    # Maven Central publishes a .sha1 next to every artifact; check it.
    EXPECTED_SHA1="$(curl -fsSL "$MAVEN/$OPENXR_LOADER_VERSION/openxr_loader_for_android-${OPENXR_LOADER_VERSION}.aar.sha1" | tr -d '[:space:]' | cut -c1-40)"
    if command -v sha1sum >/dev/null 2>&1; then
        ACTUAL_SHA1="$(sha1sum "$AAR" | cut -d' ' -f1)"
    else
        ACTUAL_SHA1="$(shasum -a 1 "$AAR" | cut -d' ' -f1)"
    fi
    if [ "$EXPECTED_SHA1" != "$ACTUAL_SHA1" ]; then
        echo "ERROR: loader AAR sha1 mismatch (expected $EXPECTED_SHA1, got $ACTUAL_SHA1)" >&2
        exit 1
    fi
    # The loader AARs ship the library in the prefab layout
    # (prefab/modules/openxr_loader/libs/android.<abi>/); classic AARs use jni/<abi>/.
    # Extract exactly one of those two member paths, nothing else.
    for member in "prefab/modules/openxr_loader/libs/android.$ANDROID_ABI/libopenxr_loader.so" \
                  "jni/$ANDROID_ABI/libopenxr_loader.so"; do
        if unzip -q -o "$AAR" "$member" -d "$LOADER_DIR/extract" 2>/dev/null \
           && [ -f "$LOADER_DIR/extract/$member" ]; then
            cp -f "$LOADER_DIR/extract/$member" "$LOADER_SO"
            break
        fi
    done
    if [ ! -f "$LOADER_SO" ]; then
        echo "ERROR: no $ANDROID_ABI libopenxr_loader.so in the loader AAR" >&2
        exit 1
    fi
    # Sanity: an AArch64 ELF shared object.
    if ! file "$LOADER_SO" | grep -q 'ELF 64-bit.*aarch64'; then
        echo "ERROR: $LOADER_SO is not an arm64 ELF" >&2
        exit 1
    fi
fi

# --- VK_LAYER_DXR_queue_lock (from the runtime's client AAR) -----------------
# Same untrusted-download discipline as the loader: own directory, checksum, one
# member extracted, nothing executed.
QL_DIR="$SCRIPT_DIR/build-android/queue_lock_${QUEUE_LOCK_RUNTIME_VERSION}"
QL_SO="$QL_DIR/libVkLayer_DXR_queue_lock.so"
if [ ! -f "$QL_SO" ]; then
    echo "=== Fetching VK_LAYER_DXR_queue_lock from DisplayXR-Client-${QUEUE_LOCK_RUNTIME_VERSION}.aar ==="
    rm -rf "$QL_DIR"
    mkdir -p "$QL_DIR/extract"
    QL_AAR="$QL_DIR/DisplayXR-Client-${QUEUE_LOCK_RUNTIME_VERSION}.aar"
    curl -fsSL -o "$QL_AAR" "$QUEUE_LOCK_URL"
    if command -v sha256sum >/dev/null 2>&1; then
        QL_SHA="$(sha256sum "$QL_AAR" | cut -d' ' -f1)"
    else
        QL_SHA="$(shasum -a 256 "$QL_AAR" | cut -d' ' -f1)"
    fi
    if [ "$QL_SHA" != "$QUEUE_LOCK_AAR_SHA256" ]; then
        echo "ERROR: client AAR sha256 mismatch (expected $QUEUE_LOCK_AAR_SHA256, got $QL_SHA)" >&2
        exit 1
    fi
    member="jni/$ANDROID_ABI/libVkLayer_DXR_queue_lock.so"
    unzip -q -o "$QL_AAR" "$member" -d "$QL_DIR/extract"
    if [ ! -f "$QL_DIR/extract/$member" ]; then
        echo "ERROR: no $member in the client AAR" >&2
        exit 1
    fi
    cp -f "$QL_DIR/extract/$member" "$QL_SO"
    if ! file "$QL_SO" | grep -q 'ELF 64-bit.*aarch64'; then
        echo "ERROR: $QL_SO is not an arm64 ELF" >&2
        exit 1
    fi
fi

# --- Configure + build ------------------------------------------------------
GEN=()
if command -v ninja >/dev/null 2>&1; then GEN=(-G Ninja); fi
cmake -S . -B build-android "${GEN[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ANDROID_ABI" \
    -DANDROID_PLATFORM="android-$ANDROID_PLATFORM" \
    -DANDROID_STL=c++_static \
    -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DDXR_ANDROID_OPENXR_LOADER="$LOADER_SO"
cmake --build build-android --config Release -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

OUT="$SCRIPT_DIR/../Runtime/Plugins/Android/$ANDROID_ABI"
mkdir -p "$OUT"
cp -f "$LOADER_SO" "$OUT/libopenxr_loader.so"
cp -f "$QL_SO" "$OUT/libVkLayer_DXR_queue_lock.so"

SO="$OUT/libdisplayxr_unity.so"
LOADER_OUT="$OUT/libopenxr_loader.so"
READELF="$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-readelf | head -1)"
SYSROOT_LIBS="$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/sysroot/usr/lib/aarch64-linux-android/"$ANDROID_PLATFORM" | head -1)"

echo ""
echo "=== Build complete ==="
ls -la "$SO" "$LOADER_OUT"

# --- Gates (CI runs exactly these, via this script) -------------------------
# Same three gates as the Linux CI job, expressed with the NDK's llvm-readelf
# (no ldd for a foreign ABI). -fvisibility=hidden + --exclude-libs make the
# export check real: a TU dropped by a mis-set platform guard shows up here.
dynsyms() { "$READELF" --dyn-syms -W "$1"; }
defined_syms() { dynsyms "$1" | awk '$7 != "UND" && ($5 == "GLOBAL" || $5 == "WEAK") {print $8}' | sed 's/@.*//'; }
FAIL=0

echo ""
echo "=== Required exports ==="
for sym in UnityPluginLoad UnityPluginUnload XRSDKPreInit JNI_OnLoad; do
    if defined_syms "$SO" | grep -qx "$sym"; then echo "export OK: $sym"
    else echo "ERROR: required export missing: $sym"; FAIL=1; fi
done

# EXACT dependency allow-list. libvulkan is deliberately NOT here: it is dlopened
# (displayxr_vk_loader.cpp), like on desktop. libopenxr_loader.so IS: it is the
# Khronos loader shipped beside this library in the same APK lib dir, and on
# Android it is the runtime-discovery path itself (#418 D1).
echo ""
echo "=== NEEDED allow-list ==="
ALLOWED="libopenxr_loader.so libandroid.so liblog.so libdl.so libm.so libc.so"
NEEDED_LIBS="$("$READELF" -d "$SO" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')"
for lib in $NEEDED_LIBS; do
    case " $ALLOWED " in
        *" $lib "*) echo "NEEDED OK: $lib" ;;
        *) echo "ERROR: unexpected link dependency: $lib (allowed: $ALLOWED)"; FAIL=1 ;;
    esac
done

# NO UNRESOLVED SYMBOLS: every UND symbol must be defined by one of the NEEDED
# libraries — the NDK's API-$ANDROID_PLATFORM stubs for the system ones, the
# shipped loader for libopenxr_loader.so. (The link also uses --no-undefined;
# keep both, as on Linux — the flag is easy to drop by accident.)
echo ""
echo "=== Undefined-symbol check ==="
PROVIDED="$(mktemp)"; UNDEF="$(mktemp)"
for lib in $NEEDED_LIBS; do
    if [ "$lib" = "libopenxr_loader.so" ]; then src="$LOADER_OUT"; else src="$SYSROOT_LIBS/$lib"; fi
    if [ ! -f "$src" ]; then echo "ERROR: cannot find $lib to resolve against ($src)"; FAIL=1; continue; fi
    defined_syms "$src" >> "$PROVIDED"
done
dynsyms "$SO" | awk '$7 == "UND" && $8 != "" && $5 != "WEAK" {print $8}' | sed 's/@.*//' | sort -u > "$UNDEF"
MISSING="$(sort -u "$PROVIDED" | comm -23 "$UNDEF" -)"
rm -f "$PROVIDED" "$UNDEF"
if [ -n "$MISSING" ]; then
    echo "ERROR: unresolved symbols — the plugin would fail to load:"; echo "$MISSING"; FAIL=1
else
    echo "no unresolved symbols: OK ($(dynsyms "$SO" | awk '$7 == "UND" && $8 != ""' | wc -l | tr -d ' ') imports, all resolved)"
fi

# 16 KB page-size devices (Android 15+) refuse a library whose LOAD segments are
# aligned below 16 KB. Ours is built for it (ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES);
# the loader's alignment is whatever its Khronos release shipped — reported, not
# gated (1.0.x loaders are 4 KB; 1.1.4x+ are 16 KB).
echo ""
echo "=== LOAD alignment (16 KB page devices) ==="
for f in "$SO" "$LOADER_OUT" "$OUT/libVkLayer_DXR_queue_lock.so"; do
    ALIGN="$("$READELF" -lW "$f" | awk '/LOAD/{print $NF}' | sort -u | tr '\n' ' ')"
    echo "$(basename "$f"): $ALIGN"
done
if "$READELF" -lW "$SO" | awk '/LOAD/{print $NF}' | grep -qvx '0x4000'; then
    echo "ERROR: libdisplayxr_unity.so LOAD segments are not 16 KB aligned"; FAIL=1
fi

echo ""
echo "=== NEEDED (libopenxr_loader.so, for reference) ==="
"$READELF" -d "$LOADER_OUT" | grep -E 'NEEDED|SONAME'

if [ "$FAIL" != 0 ]; then
    echo ""
    echo "Android gates FAILED" >&2
    exit 1
fi
echo ""
echo "Android gates: OK"
