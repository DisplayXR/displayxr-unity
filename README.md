# DisplayXR — Unity Plugin

[![Build Native Plugin](https://github.com/DisplayXR/displayxr-unity/actions/workflows/build-native.yml/badge.svg)](https://github.com/DisplayXR/displayxr-unity/actions/workflows/build-native.yml)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](LICENSE)

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
- [Preview (Play Mode)](#preview-play-mode)
- [Building Your App](#building-your-app)
  - [Windows Standalone](#windows-standalone)
  - [macOS Standalone](#macOS-standalone)
  - [Cross-Compiling (macOS Editor to Windows Build)](#cross-compiling-macos-editor-to-windows-build)
- [Deploying to End Users](#deploying-to-end-users)
- [Testing Without Hardware](#testing-without-hardware)
- [Troubleshooting](#troubleshooting)
- [Architecture](#architecture)

> **New to the plugin?** Start with the [Quick Start Guide](docs~/quick-start-guide.md) — a step-by-step walkthrough that covers installation, demo scenes for both stereo modes, building standalone apps, and end-to-end testing on Windows and macOS.

> **Want a ready-to-open Unity project?** Clone [displayxr-unity-samples](https://github.com/DisplayXR/displayxr-unity-samples) and open `samples/birp-multipass/` — a minimal Unity 6 project that depends on this plugin via UPM. Open it in Unity, hit Play, and you're rendering on a spatial display. (The repo also holds URP, HDRP, and Desktop Avatar samples.)

---

## Overview

DisplayXR ships as a custom **Unity display provider** (`IUnityXRDisplay`, the same integration route as Oculus/Varjo/Cardboard) that drives the DisplayXR OpenXR runtime directly while Unity keeps rendering the scene. It provides:

- **Eye-tracked stereo rendering** — runtime-owned Kooima asymmetric frustum projection from real-time eye positions (`XR_DXR_view_rig`)
- **Two stereo rig modes** — Camera-centric (add to existing camera) or display-centric (place a virtual display in the scene)
- **2D/3D display zones + 2D UI overlay** — frame 3D content to window-pixel zones and route any Canvas to a window-space composition layer with stereo disparity
- **BiRP, URP, and HDRP** — off-axis projection on all three pipelines (the provider hands Unity a full per-eye projection matrix, so no per-pipeline fix is needed), plus Single-Pass-Instanced (SPI) on URP+Windows+D3D12 and Multi-Pass elsewhere
- **In-editor Play Mode preview** — pressing Play runs the provider itself, giving full parity with built apps (#171). Play Mode *is* the preview — there is no separate edit-mode preview window.

**How it works:** the provider registers a `DisplayXR Display` display subsystem, binds an OpenXR session to Unity's graphics device, chains the runtime's `XR_DXR_view_rig` descriptor onto `xrLocateViews`, and submits Unity's rendered eye textures back to the runtime compositor via `xrEndFrame`. Enable it under **XR Plug-in Management > Standalone > DisplayXR Display** (see [Enabling the Feature](#enabling-the-feature)). See [`docs~/architecture/xr-display-provider.md`](docs~/architecture/xr-display-provider.md) for the full provider design.

---

## Requirements

| Requirement | Details |
|-------------|---------|
| **Unity** | 2022.3 LTS or later (including Unity 6) |
| **OpenXR Plugin** | `com.unity.xr.openxr` 1.9.1+ (installed via Package Manager) |
| **XR Plugin Management** | `com.unity.xr.management` 4.4.0+ (auto-installed with OpenXR) |
| **DisplayXR Runtime** | [`DisplayXRSetup-*.exe`](https://github.com/DisplayXR/displayxr-runtime/releases/latest) (v1.4.0+) — required on every target machine. See [Deploying to End Users](#deploying-to-end-users). |
| **Vendor display plug-in** (Leia hardware only) | [`DisplayXRLeiaSRSetup-*.exe`](https://github.com/DisplayXR/displayxr-leia-plugin/releases/latest) — required for Leia SR hardware. Not needed for sim_display testing or non-Leia displays. Install **after** the runtime. |

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

DisplayXR is enabled as a display provider (the shipping path):

1. Go to **Edit > Project Settings > XR Plug-in Management**
2. Under the **Standalone** tab (Windows or macOS), check **DisplayXR Display**
3. Make sure **OpenXR** is **not** also checked on the same tab (the provider drives the runtime itself; running both loaders conflicts)
4. Check **Initialize XR on Startup** so the provider comes up in Play Mode and in built apps

You can verify the runtime status in the **DisplayXR** rig inspectors and the **DisplayXR** settings panel (display resolution, eye-tracking state).

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

The overlay is submitted as `XrCompositionLayerWindowSpaceDXR` and composited by DisplayXR before display processing. This means 2D UI text stays sharp and is not interlaced — ideal for HUDs, menus, and status displays.

---

## Preview (Play Mode)

**Press Play — Play Mode is the preview.** With the **DisplayXR Display** provider active (the shipping path), Play Mode runs the provider itself: the same backend as a built app, weaving to the display in a dedicated editor window (#171). This gives true parity with built apps — provider behavior and bugs surface directly in the editor — with no separate preview session or Play/Stop-free workflow to manage.

Rendering modes are enumerated dynamically from the runtime and depend on the connected display hardware; mode switching is app policy (see `DisplayXRModeSwitch`). The runtime status (resolution, physical dimensions, eye-tracking state, current mode) is visible in the DisplayXR rig inspectors and settings panel.

---

## Building Your App

### Windows Standalone

1. **File > Build Settings**
2. Select **Windows, Mac, Linux** platform
3. Set **Target Platform** to **Windows** and **Architecture** to **x86_64**
4. Verify in **Player Settings > XR Plug-in Management > Standalone** that **DisplayXR Display** is enabled
5. Click **Build** or **Build And Run**

The build output includes `displayxr_unity.dll` in the `Plugins/` folder alongside your executable.

### macOS Standalone

1. **File > Build Settings**
2. Select **macOS** platform
3. Verify **DisplayXR Display** is enabled in Standalone XR settings
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

Two installers, in order:

**1. DisplayXR Runtime** (always required):

Install [`DisplayXRSetup-*.exe`](https://github.com/DisplayXR/displayxr-runtime/releases/latest) from the [displayxr-runtime](https://github.com/DisplayXR/displayxr-runtime/releases) releases. Registers the OpenXR runtime JSON, drops the runtime DLLs at `C:\Program Files\DisplayXR\Runtime\`, sets `HKLM\Software\Khronos\OpenXR\1\ActiveRuntime` to the DisplayXR manifest, and auto-starts `displayxr-service.exe` at user logon.

The runtime ships with the `sim_display` plug-in (vendor-neutral SBS/anaglyph/blend fallback for any GPU). For real 3D hardware you also need the matching vendor plug-in below.

**2. Leia SR plug-in** (required for Leia SR hardware only):

Install [`DisplayXRLeiaSRSetup-*.exe`](https://github.com/DisplayXR/displayxr-leia-plugin/releases/latest) from the [displayxr-leia-plugin](https://github.com/DisplayXR/displayxr-leia-plugin/releases) releases. Drops `DisplayXR-LeiaSR.dll` at `C:\Program Files\DisplayXR\Plugins\LeiaSR\` and registers it at `HKLM\Software\DisplayXR\DisplayProcessors\leia-sr` (`ProbeOrder=50`). The runtime's registry-driven discovery picks it up at `xrCreateInstance` time; on Leia hardware it wins the probe and renders through the SR weaver, on non-Leia hardware it declines and the runtime falls back to `sim_display`.

**Install order matters** — the Leia plug-in installer hard-prereqs `HKLM\Software\DisplayXR\Runtime\InstallPath` and refuses install if the runtime isn't already present.

The plug-in cascade-uninstalls with the runtime: uninstalling the runtime via `Add/Remove Programs` automatically uninstalls all registered vendor plug-ins (#286 fixed cleanup in runtime v1.4.1+).

> **Why two installers?** Pre-v1.4.0 the runtime installer bundled the Leia plug-in. As of v1.4.0 (`ADR-019` / [#263](https://github.com/DisplayXR/displayxr-runtime/issues/263)), vendor display drivers ship as separate plug-in installers from their own repos so the runtime stays vendor-source-clean and additional vendors don't bloat the runtime install set.

Or, for development/testing without installation, set the environment variable to your dev runtime JSON:
```cmd
set XR_RUNTIME_JSON=C:\path\to\openxr_displayxr-dev.json
```

### macOS Deployment

Set before launching the app:
```bash
export XR_RUNTIME_JSON=/path/to/DisplayXR-macOS/share/openxr/1/openxr_displayxr.json
```

#### Unsigned builds and the runtime symlink

If you have a runtime symlink at `~/Library/Application Support/openxr/1/active_runtime.json` (the OpenXR loader's standard discovery path), it works for the Unity editor but **not for unsigned built `.app` bundles**. macOS' file-access protections block unsigned apps from reading other apps' Application Support directories, so the loader can't find the runtime manifest. Symptom:

```
Loading OpenXR loader library at path: openxr_loader
[XR] [FAILURE] xrCreateInstance: XR_ERROR_RUNTIME_UNAVAILABLE
[DisplayXR] provider not active.
```

Two ways around it:

**1. Set `XR_RUNTIME_JSON` explicitly when launching** (works with any build, signed or not):
```bash
XR_RUNTIME_JSON=/path/to/openxr_displayxr.json /path/to/MyApp.app/Contents/MacOS/MyApp
```

**2. Code-sign the `.app`** so macOS grants it normal user-file access. Ad-hoc signing is enough for local development — no Apple Developer account needed:
```bash
codesign --deep --force --sign - MyApp.app
```
After signing, the runtime symlink is reachable and you can launch the app without `XR_RUNTIME_JSON`.

For distributing to other users, sign with a Developer ID and notarize:
```bash
codesign --deep --force --sign "Developer ID Application: Your Name (TEAMID)" \
         --options runtime --timestamp MyApp.app
xcrun notarytool submit MyApp.app --apple-id you@example.com --team-id TEAMID --wait
xcrun stapler staple MyApp.app
```

### What Happens Without the Runtime

If the DisplayXR runtime is not installed, the provider fails to bind an OpenXR session and logs:
```
[OpenXR] No OpenXR runtime found
```
The plugin logs a warning but doesn't crash — your app runs in mono (non-stereo) mode. Query the DisplayXR runtime status (via the rig components / `DisplayXRProvider`) to detect this and show a user-facing message.

---

## Testing Without Hardware

The **sim_display** driver provides a software 3D display for development. When no Leia 3D panel is present, the runtime **falls back to `sim_display` automatically** — you don't need to enable it. Optionally pick the sim output format with `SIM_DISPLAY_OUTPUT`:

```bash
# Windows
set SIM_DISPLAY_OUTPUT=sbs          # side-by-side stereo
# or: anaglyph (red-cyan), blend (50/50 alpha)

# macOS
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
| Black screen | DisplayXR not enabled | Check **Project Settings > XR Plug-in Management > Standalone > DisplayXR Display**, and **Initialize XR on Startup**. |
| Scene renders 2D side-by-side on Leia hardware (no depth) | Leia plug-in not installed/registered | Install `DisplayXRLeiaSRSetup-*.exe` from [displayxr-leia-plugin](https://github.com/DisplayXR/displayxr-leia-plugin/releases). Verify `HKLM\Software\DisplayXR\DisplayProcessors\leia-sr` exists. Without the plug-in, the runtime falls back to `sim_display` (SBS output) on any hardware. |
| No stereo (flat image) | Eye tracking not running | Verify the DisplayXR runtime is configured with a display that supports eye tracking, or use sim_display for testing |
| Stereo looks wrong | Tunables misconfigured | Reset to defaults (IPD=1, Parallax=1, Scale=1) |
| UPM "cannot add package from git URL" (Windows) | Git not installed or not in PATH | Install [Git for Windows](https://gitforwindows.org/), restart Unity. Verify with `git --version` in a terminal. |
| UPM "cannot add package from git URL" (macOS) | GUI apps can't find Homebrew Git | Run `xcode-select --install` to set up `/usr/bin/git`, or launch Unity from terminal (`open -a Unity`). Check `~/Library/Logs/Unity/upm.log` for details. |
| `DllNotFoundException: displayxr_unity` | Native plugin not found by Unity | Ensure the plugin binaries are in `Runtime/Plugins/Windows/x64/` or `Runtime/Plugins/macOS/` |
| macOS: `XR_ERROR_RUNTIME_UNAVAILABLE` in built `.app` despite the symlink working in editor | Unsigned `.app` bundles can't read `~/Library/Application Support/openxr/` due to macOS sandbox protections | Either set `XR_RUNTIME_JSON` when launching, or ad-hoc sign with `codesign --deep --force --sign - MyApp.app`. See [macOS Deployment](#macos-deployment). |
| HDRP stereo artifacts | Single-pass instanced issue | Verify both eye views have correct FOVs in Frame Debugger |
| Play Mode shows "Not Connected" | `XR_RUNTIME_JSON` not set or runtime not running | Set the env var before launching Unity; verify the runtime process is active |
| Play Mode shows black | Runtime connected but no output | Check runtime logs; verify sim_display or hardware is configured |
| `VK_ERROR_EXTENSION_NOT_PRESENT` on macOS | MoltenVK limitation | Known issue — use sim_display for testing |

### Debug Logging

Enable **Log Eye Tracking** on the DisplayXRCamera or DisplayXRDisplay component to see per-frame eye positions in the Console:
```
[DisplayXR] Eyes: L=(0.032, 0.001, 0.504), R=(-0.031, 0.001, 0.504), tracked=True
```

### Checking Runtime Status in Editor

With the provider active, the **DisplayXRCamera / DisplayXRDisplay inspectors** and the **DisplayXR settings panel** show runtime status:
- Connected display properties (resolution, physical size, nominal viewer distance)
- Eye tracking status

---

## Architecture

DisplayXR ships as a custom **`IUnityXRDisplay` display provider** (the shipping path). The provider
registers a `DisplayXR Display` subsystem, owns an OpenXR session on Unity's graphics device, and hands
Unity's rendered eye textures to the runtime compositor — Unity keeps rendering the scene (BiRP/URP/HDRP,
SPI/Multi-Pass), the runtime owns the Kooima math (`XR_DXR_view_rig`).

```
Unity Editor / Player
┌──────────────────────────────────────────────────────────┐
│  C# Layer                                                │
│                                                          │
│  DisplayXRDisplayLoader : XRLoaderHelper                 │
│    starts the "DisplayXR Display" display subsystem      │
│  DisplayXRProvider / DisplayXRProviderDriver             │
│    push rig tunables, modes/zones, events (dxr_prov_*)   │
│                                                          │
│  DisplayXRCamera          DisplayXRDisplay                 │
│  (camera-centric)        (display-centric)               │
│                                                          │
│  DisplayXRWindowSpaceUI / DisplayXRLocal2D  (2D layers)   │
└──────────────────────────────────────────────────────────┘
        │ P/Invoke (displayxr_unity.dll / .dylib)
        ▼
┌──────────────────────────────────────────────────────────┐
│  Native Plugin (C/C++)                                   │
│  Display provider (IUnityXRDisplay):                     │
│    session on Unity's device → render params (SPI/MP)   │
│    xrLocateViews → chain XR_DXR_view_rig descriptor     │
│    xrEndFrame → submit projection + zone + 2D layers    │
└──────────────────────────────────────────────────────────┘
        │ Standard OpenXR API
        ▼
┌──────────────────────────────────────────────────────────┐
│  DisplayXR Runtime                                          │
│  Compositor → Display Processor → Output                 │
└──────────────────────────────────────────────────────────┘
```

See [`docs~/architecture/xr-display-provider.md`](docs~/architecture/xr-display-provider.md) for the full
provider design, and [`docs~/adr/ADR-007-render-path-by-view-count.md`](docs~/adr/ADR-007-render-path-by-view-count.md)
for the roadmap to many-view (light-field / quilt) displays.

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
| `Runtime/Provider/` | **Display provider (shipping path):** `DisplayXRDisplayLoader` (XR Plug-in Management loader), `DisplayXRProvider`/`DisplayXRProviderDriver` (facade + per-frame driver), `DisplayXRProviderNative` (P/Invoke) |
| `Editor/Provider/DisplayXRDisplayPackage.cs` | Registers the "DisplayXR Display" provider toggle in XR Plug-in Management |
| `Runtime/DisplayXRCamera.cs` | Camera-centric stereo rig MonoBehaviour |
| `Runtime/DisplayXRDisplay.cs` | Display-centric stereo rig MonoBehaviour |
| `Runtime/DisplayXRDisplayInfo.cs` | Display properties data struct |
| `Runtime/DisplayXRTunables.cs` | Tunable parameters struct |
| `Runtime/DisplayXRWindowSpaceUI.cs` | 2D UI overlay routing |
| `Runtime/DisplayXRNative.cs` | P/Invoke bindings to native plugin |
| `Runtime/Plugins/Windows/x64/` | Windows native plugin (DLL) |
| `Runtime/Plugins/macOS/` | macOS native plugin (dylib) |
| `Editor/DisplayXRDisplayEditor.cs` | Custom inspector for display-centric mode |
| `Editor/DisplayXRCameraEditor.cs` | Custom inspector for camera-centric mode |
| `Editor/DisplayXRSettingsProvider.cs` | Project Settings page |
| `native~/` | Native C/C++ plugin source + CMakeLists.txt |
