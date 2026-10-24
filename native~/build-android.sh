#!/bin/bash
# Build the native plugin for Android arm64-v8a (Vulkan) — #418.
#
# Produces, in Runtime/Plugins/Android/arm64-v8a/:
#  - libdisplayxr_unity.so  — the plugin, Vulkan provider backend (ENABLE_VULKAN);
#  - libopenxr_loader.so    — the Khronos OpenXR loader for Android it links
#                             against, version-matched to the OpenXR headers the
#                             CMake build uses (fetched from Maven Central).
# Both are gitignored (CI builds them, like the Linux .so); their .meta files are
# committed.
#
# NDK: $ANDROID_NDK_ROOT (or $ANDROID_NDK_HOME), else the NDK bundled with a
# Unity Hub editor install (macOS path below). Needs cmake + ninja (or make),
# curl and unzip. Runs on macOS and Linux. Pass --clean to start fresh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# --- Pins -------------------------------------------------------------------
# The loader version MUST match the OpenXR-SDK header tag in CMakeLists.txt
# (release-X.Y.Z -> X.Y.Z): the loader and the headers share XR_CURRENT_API_VERSION.
OPENXR_HEADER_TAG="$(sed -n 's/^ *GIT_TAG release-\([0-9.]*\)$/\1/p' CMakeLists.txt | head -1)"
OPENXR_LOADER_VERSION="${OPENXR_LOADER_VERSION:-$OPENXR_HEADER_TAG}"
if [ -z "$OPENXR_LOADER_VERSION" ]; then
    echo "ERROR: could not read the OpenXR-SDK GIT_TAG from CMakeLists.txt" >&2
    exit 1
fi
ANDROID_ABI=arm64-v8a
ANDROID_PLATFORM="${ANDROID_PLATFORM:-29}"   # Android 10; Vulkan 1.1 + AHardwareBuffer
MAVEN="https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android"

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
for f in "$SO" "$LOADER_OUT"; do
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
