# DisplayXR — Unity Plugin

[![Build Native Plugin](https://github.com/DisplayXR/displayxr-unity/actions/workflows/build-native.yml/badge.svg)](https://github.com/DisplayXR/displayxr-unity/actions/workflows/build-native.yml)
[![License: BSL-1.0](https://img.shields.io/badge/License-BSL--1.0-blue.svg)](LICENSE)

Unity plugin for rendering on eye-tracked 3D light field displays via the DisplayXR OpenXR runtime. Works with any OpenXR-compatible 3D display.

## Table of Contents

- [Overview](#overview)
- [Requirements](#requirements)
- [Installing the Plugin](#installing-the-plugin)
- [Enabling the Feature](#enabling-the-feature)
- [Scene Setup](#scene-setup)
  - [Camera-Centric Mode (Recommended)](#camera-centric-mode-recommended)
  - [Display-Centric Mode](#display-centric-mode)
- [Stereo Tunables Reference](#stereo-tunables-reference)
- [2D UI Overlay](#2d-ui-overlay)
- [Editor Preview (Standalone Session)](#editor-preview-standalone-session)
- [Building Your App](#building-your-app)
  - [Windows Standalone](#windows-standalone)
  - [macOS Standalone](#macOS-standalone)
  - [Cross-Compiling (macOS Editor to Windows Build)](#cross-compiling-macos-editor-to-windows-build)
- [Deploying to End Users](#deploying-to-end-users)
- [Testing Without Hardware](#testing-without-hardware)
- [Troubleshooting](#troubleshooting)
- [Architecture](#architecture)

> **New to the plugin?** Start with the [Quick Start Guide](docs~/quick-start-guide.md) — a step-by-step walkthrough that covers installation, demo scenes for both stereo modes, building standalone apps, and end-to-end testing on Windows and macOS.

> **Want a ready-to-open Unity project?** Clone [displayxr-unity-test](https://github.com/DisplayXR/displayxr-unity-test) — a minimal Unity 6 project that depends on this plugin via UPM. Open it in Unity, hit Play, and you're rendering on a spatial display.

---

## Overview

The plugin intercepts Unity's OpenXR pipeline at the native layer to provide:

- **Eye-tracked stereo rendering** — Kooima asymmetric frustum projection from real-time eye positions
- **Two stereo rig modes** — Camera-centric (add to existing camera) or display-centric (place a virtual display in the scene)
- **2D UI overlay** — Route any Canvas to a window-space composition layer with stereo disparity
- **Standalone editor preview** — Live composited 3D output in a dedicated editor window, no Play Mode required. Camera selector dropdown, rendering mode switching, and zero-copy GPU texture sharing.

The plugin works by hooking `xrLocateViews` before Unity sees the results, replacing the runtime's FOV data with Kooima-computed asymmetric frustums. Unity then builds correct projection matrices through its normal rendering pipeline — no `Camera.SetStereoProjectionMatrix` hacks required.

---

## Requirements

| Requirement | Details |
|-------------|---------|
| **Unity** | 2022.3 LTS or later (including Unity 6) |
| **OpenXR Plugin** | `com.unity.xr.openxr` 1.9.1+ (installed via Package Manager) |
| **XR Plugin Management** | `com.unity.xr.management` 4.4.0+ (auto-installed with OpenXR) |
| **DisplayXR Runtime** | Pre-built from [displayxr-runtime](https://github.com/DisplayXR/displayxr-runtime) CI — see [Deploying to End Users](#deploying-to-end-users) |

### Platform Support

| Editor Platform | Build Target | Native Plugin | Status |
|----------------|--------------|---------------|--------|
| Windows | Windows x64 | `displayxr_unity.dll` | Supported |
| macOS | macOS | `libdisplayxr_unity.dylib` | Supported |
| macOS | Windows x64 | `displayxr_unity.dll` | Supported (cross-compile) |

---

## Installing the Plugin

### Option A: From Local Folder (Development)

Clone and build the native plugin, then add the package from disk:

```bash
git clone https://github.com/DisplayXR/displayxr-unity.git
cd displayxr-unity/native~
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release    # on Windows, add: -A x64
cmake --build . --config Release
```

In Unity: **Window > Package Manager > + > Add package from disk...** → select `displayxr-unity/package.json`.

### Option B: From Git URL (Recommended)

> **Git must be in PATH.** Unity's Package Manager requires Git installed and accessible to GUI apps.
> - **Windows:** Install [Git for Windows](https://gitforwindows.org/) and ensure it's in your system PATH.
> - **macOS:** If Git is installed via Homebrew, Unity launched from Hub/Finder may not find it (GUI apps don't inherit your shell PATH). Fix: run `xcode-select --install` to ensure `/usr/bin/git` works, or launch Unity from a terminal. See [Troubleshooting](#troubleshooting) for details.

1. In Unity: **Window > Package Manager**
2. Click **+** > **Add package from git URL...**
3. Enter one of:

   **Pinned to a specific version (recommended for production):**
   ```
   https://github.com/DisplayXR/displayxr-unity.git#upm/v1.0.0
   ```

   **Always latest release:**
   ```
   https://github.com/DisplayXR/displayxr-unity.git#upm
   ```

Both URLs install from the `upm` branch, which includes pre-built native binaries (Windows x64 + macOS Universal) — no local build required. See the [releases page](https://github.com/DisplayXR/displayxr-unity/releases) for available versions.

To **update** to a newer version later: change the URL fragment (e.g., `#upm/v1.0.1`) in `Packages/manifest.json` and use **Package Manager → Refresh**, or re-add the package with the new URL.

### Option C: From Release Tarball

1. Download the `.tgz` file from the [latest release](https://github.com/DisplayXR/displayxr-unity/releases)
2. In Unity: **Window > Package Manager**
3. Click **+** > **Add package from tarball...**
4. Select the downloaded `.tgz` file

After installation, the package appears as **DisplayXR** in the Package Manager.

---

## Enabling the Feature

1. Go to **Edit > Project Settings > XR Plug-in Management**
2. Under the **Standalone** tab (Windows or macOS), check **OpenXR**
3. Click the gear icon next to OpenXR (or expand **OpenXR > Features**)
4. Enable **DisplayXR**

You can verify the runtime connection under **Project Settings > XR Plug-in Management > OpenXR > DisplayXR** — a status panel shows whether `XR_RUNTIME_JSON` is set.

---

## Scene Setup

### Camera-Centric Mode (Recommended)

This is the simplest setup. Your existing Main Camera stays in place and becomes the nominal viewing position. The plugin creates stereo eye offsets around it.

**Step 1:** Select your **Main Camera** in the hierarchy.

**Step 2:** Add Component > **DisplayXRCamera** (found under DisplayXR category).

That's it. The component:
- Reads eye tracking data from the runtime each frame
- Computes Kooima asymmetric frustums around the camera position
- Pushes modified FOVs into the OpenXR pipeline before Unity renders

```
Hierarchy:
  Main Camera              <-- has Camera + DisplayXRCamera
    ├── Scene objects...
    └── (everything else in your scene)
```

**Inspector tunables:**

| Parameter | Default | Description |
|-----------|---------|-------------|
| IPD Factor | 1.0 | Scales inter-eye distance. <1 = reduced stereo, >1 = exaggerated |
| Parallax Factor | 1.0 | Scales eye X/Y offset from viewing axis |
| Inv. Convergence Distance | 0 | 1/meters. 0 = infinity (parallel projection). Higher values = screen plane closer to camera. The inspector shows the equivalent distance in meters. |

> **Note:** The component inherits the camera's vertical FOV automatically. Change it on the Camera component, not on DisplayXRCamera.

**When to use camera-centric mode:**
- First-person games and apps
- Retrofitting stereo into an existing project (just add the component)
- When the camera moves freely through the scene

### Display-Centric Mode

The display is a fixed object in the scene. Eye positions are transformed relative to this virtual display.

**Step 1:** Create an empty GameObject where the virtual display should be:
- **GameObject > Create Empty**, name it "VirtualDisplay"
- Position and orient it in the scene (e.g., at `(0, 1.5, 2)` facing the player)

**Step 2:** Add Component > **DisplayXRDisplay**

**Step 3:** Make your Main Camera a child of this object, or position it separately — the display's world transform is sent to the native plugin as the "scene transform" applied to raw eye positions.

```
Hierarchy:
  VirtualDisplay           <-- has DisplayXRDisplay
    └── Main Camera        <-- standard Camera component
```

**Inspector tunables:**

| Parameter | Default | Description |
|-----------|---------|-------------|
| IPD Factor | 1.0 | Scales inter-eye distance |
| Parallax Factor | 1.0 | Scales eye X/Y offset from display center |
| Perspective Factor | 1.0 | Scales eye Z only (depth intensity without changing baseline) |
| Virtual Display Height | 0 | Virtual display height in meters. 0 = use physical display height. The inspector shows the computed virtual size. The parent transform's scale acts as zoom. |

**When to use display-centric mode:**
- Digital signage, museum exhibits, kiosks
- The virtual display is a physical object in the scene (e.g., a TV on a wall)
- You want the display's position and orientation to be an explicit scene element

> **Try it:** Import the **Display Scene** sample from Package Manager for a ready-made tabletop turntable demo.

---

## Stereo Tunables Reference

Both modes share a common tunable interface that maps to the native plugin's Kooima computation. Here's what each parameter does physically:

### IPD Factor (both modes)
Scales the horizontal distance between left and right eye positions.
- `1.0` = natural inter-pupillary distance from eye tracker
- `0.5` = half the real IPD (gentler stereo, less eye strain)
- `2.0` = double the real IPD (exaggerated depth, "macro" effect)
- `0.0` = mono rendering (both eyes at center)

### Parallax Factor (both modes)
Scales the eye's X and Y offset from the viewing axis (or display center).
- `1.0` = natural head parallax
- `0.0` = no parallax (stereo still works via IPD, but no motion parallax)
- `>1.0` = exaggerated motion parallax

### Perspective Factor (display-centric only)
Scales the eye's Z position (distance from display) without changing the baseline.
- `1.0` = natural depth
- `<1.0` = eyes appear closer to display (stronger perspective)
- `>1.0` = eyes appear farther (flatter perspective)

### Virtual Display Height (display-centric only)
Sets the virtual display height in meters for the Kooima projection.
- `0` = use the physical display's actual height (default)
- `0.3` = 30 cm virtual display (scene objects appear larger — magnifier effect)
- `0.6` = 60 cm virtual display (scene objects appear smaller)

The parent transform's scale acts as zoom: scaling up the DisplayXRDisplay object zooms into the scene.

### Inv. Convergence Distance (camera-centric only)
Inverse convergence distance in 1/meters. Controls where the virtual screen plane sits relative to the camera. Objects at the convergence distance appear at the display surface; closer objects pop out, farther objects recede.
- `0` = infinity (parallel projection — no convergence)
- `2.0` = screen plane at 0.5 m from camera
- `1.0` = screen plane at 1.0 m from camera
- The inspector shows the equivalent distance in meters or "∞"

> **Note:** Camera-centric FOV is inherited automatically from the Camera component. There is no separate FOV tunable.

---

## 2D UI Overlay

Route a Canvas to a window-space composition layer that the DisplayXR compositor overlays on both eyes with per-eye disparity shift (renders pre-interlace).

1. Create a **Canvas** (any render mode)
2. Add Component > **DisplayXRWindowSpaceUI** to the Canvas
3. Configure position and size in fractional window coordinates [0..1]:

| Parameter | Default | Description |
|-----------|---------|-------------|
| Position X | 0.0 | Left edge of overlay in window (0 = left, 1 = right) |
| Position Y | 0.0 | Bottom edge (0 = bottom, 1 = top) |
| Width | 1.0 | Fractional width |
| Height | 1.0 | Fractional height |
| Disparity | 0.0 | Stereo disparity in pixels. 0 = at screen plane, positive = in front |
| Resolution | 512 | Render texture resolution (square) |

The overlay is submitted as `XrCompositionLayerWindowSpaceEXT` and composited by DisplayXR before display processing. This means 2D UI text stays sharp and is not interlaced — ideal for HUDs, menus, and status displays.

---

## Editor Preview (Standalone Session)

The standalone preview is the primary editor workflow for DisplayXR. It creates its own OpenXR session directly against the DisplayXR runtime — no Play Mode needed. This bypasses Unity's XR subsystem entirely, eliminating session conflicts, crashes, and rendering artifacts.

### Opening the Preview

**Window > DisplayXR > Preview Window**

### Toolbar

| Control | Description |
|---------|-------------|
| **Start / Stop** | Connects to the DisplayXR runtime and begins rendering |
| **Camera dropdown** | Lists all scene cameras, categorized by rig type: DisplayRig (`DisplayXRDisplay`), CameraRig (`DisplayXRCamera`), or Regular Camera. Switching rig types auto-requests the appropriate rendering mode. |
| **Auto Refresh** | When enabled, continuously repaints the preview |
| **Runtime status** | Shows "Connected" or "Not Connected" |

### Rendering Modes

Modes are dynamically enumerated from the runtime and depend on the connected display hardware. Controls:
- **V** — cycle through available modes
- **0–8** — select a specific mode directly

### Output

The preview uses zero-copy GPU texture sharing (IOSurface on macOS, DXGI on Windows) to display exactly what the physical display sees. The footer shows resolution, physical dimensions, eye tracking status, and current rendering mode.

### Play Mode Integration

If you enter Play Mode while the preview is running, the plugin automatically removes Unity's OpenXR loader to prevent session conflicts, then restores it when you exit Play Mode. This means Play Mode runs without XR — useful for testing game logic — while the standalone preview handles all 3D display output.

---

## Building Your App

### Windows Standalone

1. **File > Build Settings**
2. Select **Windows, Mac, Linux** platform
3. Set **Target Platform** to **Windows** and **Architecture** to **x86_64**
4. Verify in **Player Settings > XR Plug-in Management > Standalone** that OpenXR is enabled with the DisplayXR feature
5. Click **Build** or **Build And Run**

The build output includes `displayxr_unity.dll` in the `Plugins/` folder alongside your executable.

### macOS Standalone

1. **File > Build Settings**
2. Select **macOS** platform
3. Verify OpenXR + DisplayXR feature enabled in Standalone XR settings
4. Click **Build**

The `.app` bundle includes `libdisplayxr_unity.dylib` in the plugins folder.

### Cross-Compiling (macOS Editor to Windows Build)

**Yes, you can build a Windows app from Unity for Mac.** Unity's cross-compilation support handles this:

1. Install the **Windows Build Support** module in Unity Hub:
   - Unity Hub > Installs > your Unity version > Add Modules > **Windows Build Support (Mono)**
2. In Unity: **File > Build Settings > Windows, Mac, Linux**
3. Set **Target Platform** to **Windows**
4. Build as normal

The plugin includes both platform binaries (`displayxr_unity.dll` for Windows, `libdisplayxr_unity.dylib` for macOS). Unity automatically selects the correct one based on the build target.

**Important:** The built Windows `.exe` still requires the DisplayXR runtime installed on the **target Windows machine** — see [Deploying to End Users](#deploying-to-end-users). You cannot run the Windows build on macOS.

---

## Deploying to End Users

Your built app is a standard OpenXR application. It needs an OpenXR runtime on the target machine. The plugin is hardware-agnostic — it communicates only through the OpenXR API and does not depend on any specific display vendor SDK.

### Windows Deployment

Install the DisplayXR runtime via the `SRDisplayXRInstaller.exe` from the [displayxr-runtime](https://github.com/DisplayXR/displayxr-runtime) CI build artifact (registers the runtime JSON and copies DLLs system-wide).

Or, for development/testing, set the environment variable:
```cmd
set XR_RUNTIME_JSON=C:\path\to\openxr_displayxr-dev.json
```

### macOS Deployment

Set before launching the app:
```bash
export XR_RUNTIME_JSON=/path/to/DisplayXR-macOS/share/openxr/1/openxr_displayxr.json
```

### What Happens Without the Runtime

If the DisplayXR runtime is not installed, Unity's OpenXR loader fails to find a runtime and logs:
```
[OpenXR] No OpenXR runtime found
```
The `DisplayXRFeature` logs a warning but doesn't crash — your app runs in mono (non-stereo) mode. You can check `DisplayXRFeature.Instance` being null to detect this and show a user-facing message.

---

## Testing Without Hardware

The **sim_display** driver provides a software 3D display for development:

```bash
# Windows
set SIM_DISPLAY_ENABLE=1
set SIM_DISPLAY_OUTPUT=sbs          # side-by-side stereo
# or: anaglyph (red-cyan), blend (50/50 alpha)

# macOS
export SIM_DISPLAY_ENABLE=1
export SIM_DISPLAY_OUTPUT=sbs
```

With sim_display:
- Eye tracking is simulated via keyboard/mouse (qwerty driver)
- Display dimensions are synthetic (configurable)
- Output renders as SBS, anaglyph, or alpha-blend in a regular window

This lets you develop and test the full stereo pipeline on any machine.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| "No OpenXR runtime found" | `XR_RUNTIME_JSON` not set or points to missing file | Set the env var to the DisplayXR runtime JSON path |
| Black screen | DisplayXR feature not enabled | Check Project Settings > XR Plug-in Management > OpenXR > Features |
| No stereo (flat image) | Eye tracking not running | Verify the DisplayXR runtime is configured with a display that supports eye tracking, or use sim_display for testing |
| Stereo looks wrong | Tunables misconfigured | Reset to defaults (IPD=1, Parallax=1, Scale=1) |
| UPM "cannot add package from git URL" (Windows) | Git not installed or not in PATH | Install [Git for Windows](https://gitforwindows.org/), restart Unity. Verify with `git --version` in a terminal. |
| UPM "cannot add package from git URL" (macOS) | GUI apps can't find Homebrew Git | Run `xcode-select --install` to set up `/usr/bin/git`, or launch Unity from terminal (`open -a Unity`). Check `~/Library/Logs/Unity/upm.log` for details. |
| `DllNotFoundException: displayxr_unity` | Native plugin not found by Unity | Ensure the plugin binaries are in `Runtime/Plugins/Windows/x64/` or `Runtime/Plugins/macOS/` |
| HDRP stereo artifacts | Single-pass instanced issue | Verify both eye views have correct FOVs in Frame Debugger |
| Preview shows "Not Connected" | `XR_RUNTIME_JSON` not set or runtime not running | Set the env var before launching Unity; verify the runtime process is active |
| Preview shows black after Start | Runtime connected but no output | Check runtime logs; verify sim_display or hardware is configured |
| `VK_ERROR_EXTENSION_NOT_PRESENT` on macOS | MoltenVK limitation | Known issue — use sim_display for testing |

### Debug Logging

Enable **Log Eye Tracking** on the DisplayXRCamera or DisplayXRDisplay component to see per-frame eye positions in the Console:
```
[DisplayXR] Eyes: L=(0.032, 0.001, 0.504), R=(-0.031, 0.001, 0.504), tracked=True
```

### Checking Runtime Status in Editor

**Project Settings > XR Plug-in Management > OpenXR > DisplayXR** shows:
- Runtime JSON path and whether the file exists
- Runtime connection status
- Connected display properties (resolution, physical size, nominal viewer distance)
- Eye tracking status

---

## Architecture

```
Unity Editor / Player
┌──────────────────────────────────────────────────────────┐
│  C# Layer                                                │
│                                                          │
│  DisplayXRFeature : OpenXRFeature                         │
│    HookGetInstanceProcAddr → install native hooks        │
│    OnSystemChange → query XrDisplayInfoEXT               │
│    API: SetTunables(), SetSceneTransform()               │
│                                                          │
│  DisplayXRCamera          DisplayXRDisplay                 │
│  (camera-centric)        (display-centric)               │
│  Attach to Camera        Place in scene                  │
│                                                          │
│  DisplayXRWindowSpaceUI   DisplayXRPreviewSession           │
│  (2D overlay)            (standalone editor preview)     │
│                          DisplayXRPreviewWindow           │
│                          (editor UI + camera selector)   │
└──────────────────────────────────────────────────────────┘
        │ P/Invoke (displayxr_unity.dll / .dylib)
        ▼
┌──────────────────────────────────────────────────────────┐
│  Native Plugin (C/C++)                                   │
│  Hook chain:                                             │
│    xrLocateViews → scene transform → tunables → Kooima  │
│    xrCreateSession → inject window binding               │
│    xrGetSystemProperties → extract display info          │
│    xrEndFrame → submit overlay layers                    │
│  Thread-safe double-buffered shared state                │
└──────────────────────────────────────────────────────────┘
        │ Standard OpenXR API
        ▼
┌──────────────────────────────────────────────────────────┐
│  DisplayXR Runtime                                          │
│  Compositor → Display Processor → Output                 │
└──────────────────────────────────────────────────────────┘
```

### Transform Chain (per frame)

```
Eye Tracker → raw positions in DISPLAY space
    ↓
Scene Transform (parent camera pose, zoom)
    ↓
Tunables (IPD factor, parallax, perspective, scale)
    ↓
Kooima Asymmetric Frustum → XrFovf angles
    ↓
Unity builds projection matrices → renders stereo
```

---

## Package Contents

| Path | Purpose |
|------|---------|
| `Runtime/DisplayXRFeature.cs` | OpenXR Feature — lifecycle hooks, P/Invoke, public API |
| `Runtime/DisplayXRCamera.cs` | Camera-centric stereo rig MonoBehaviour |
| `Runtime/DisplayXRDisplay.cs` | Display-centric stereo rig MonoBehaviour |
| `Runtime/DisplayXRDisplayInfo.cs` | Display properties data struct |
| `Runtime/DisplayXRTunables.cs` | Tunable parameters struct |
| `Runtime/DisplayXRWindowSpaceUI.cs` | 2D UI overlay routing |
| `Runtime/DisplayXRPreview.cs` | Inline preview textures (SBS, readback, SharedTexture) |
| `Runtime/DisplayXRNative.cs` | P/Invoke bindings to native plugin |
| `Runtime/Plugins/Windows/x64/` | Windows native plugin (DLL) |
| `Runtime/Plugins/macOS/` | macOS native plugin (dylib) |
| `Editor/DisplayXRDisplayEditor.cs` | Custom inspector for display-centric mode |
| `Editor/DisplayXRCameraEditor.cs` | Custom inspector for camera-centric mode |
| `Editor/DisplayXRPreviewSession.cs` | Standalone OpenXR session for editor preview (no Play Mode needed) |
| `Editor/DisplayXRPreviewWindow.cs` | Editor preview window with camera selector and rendering mode controls |
| `Editor/DisplayXRSettingsProvider.cs` | Project Settings page |
| `native~/` | Native C/C++ plugin source + CMakeLists.txt |
