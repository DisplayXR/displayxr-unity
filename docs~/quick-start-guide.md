# Quick Start Guide

Step-by-step walkthrough: install the plugin, build two demo scenes, test both stereo rig modes, build standalone apps for Windows and macOS, and verify the pipeline end-to-end.

**Time estimate:** ~30 minutes (less if you already have a Unity project open).

---

## Table of Contents

- [Prerequisites](#prerequisites)
- [Step 1: Install the Plugin](#step-1-install-the-plugin)
  - [Option A: From Git URL (recommended)](#option-a-from-git-url-recommended)
  - [Option B: From Release Tarball](#option-b-from-release-tarball)
  - [Option C: From Local Folder (for contributors)](#option-c-from-local-folder-for-contributors)
- [Step 2: Enable the Feature](#step-2-enable-the-feature)
- [Step 3: Set Up the Runtime](#step-3-set-up-the-runtime)
- [Step 4: Demo Scene A — Camera-Centric Mode](#step-4-demo-scene-a--camera-centric-mode)
- [Step 5: Demo Scene B — Display-Centric Mode](#step-5-demo-scene-b--display-centric-mode)
- [Step 6: Test in the Editor](#step-6-test-in-the-editor)
- [Step 7: Build a Standalone App](#step-7-build-a-standalone-app)
  - [Windows Build](#windows-build)
  - [macOS Build](#macos-build)
  - [Cross-Compiling (macOS to Windows)](#cross-compiling-macos-to-windows)
- [Step 8: Run the Built App](#step-8-run-the-built-app)
- [What to Look For](#what-to-look-for)
- [Next Steps](#next-steps)

---

## Prerequisites

| Requirement | Details |
|-------------|---------|
| **Unity** | 2022.3 LTS or later (including Unity 6). Install via [Unity Hub](https://unity.com/download). |
| **Build Support Modules** | In Unity Hub, add modules for your target platform(s): **Windows Build Support (Mono)** and/or **macOS Build Support (Mono)**. |
| **DisplayXR Runtime** | Either a hardware-connected runtime from [openxr-3d-display](https://github.com/dfattal/openxr-3d-display), or the sim_display driver for testing without hardware. |

You do **not** need a 3D display to complete this guide — the sim_display driver provides a software display for development.

---

## Step 1: Install the Plugin

### Option A: From Git URL (recommended)

The simplest method — no cloning or building required. Pre-built native binaries are included.

> **Prerequisites:** Git must be accessible to Unity. **Windows:** Install [Git for Windows](https://gitforwindows.org/). **macOS:** Run `xcode-select --install` if needed, or launch Unity from a terminal if Git is installed via Homebrew. See [Troubleshooting](../README.md#troubleshooting) for details.

1. Open (or create) a Unity project — any render pipeline (Built-in, URP, HDRP) works.

2. In Unity: **Window > Package Manager**.

3. Click the **+** button (top-left) and choose **Add package from git URL...**

4. Enter:
   ```
   https://github.com/DisplayXR/displayxr-unity.git#upm
   ```
   Or for a pinned version:
   ```
   https://github.com/DisplayXR/displayxr-unity.git#upm/v0.1.0
   ```

5. The package appears as **DisplayXR** in the Package Manager list — ready to use.

### Option B: From Release Tarball

1. Download the `.tgz` file from the [latest release](https://github.com/DisplayXR/displayxr-unity/releases).
2. In Unity: **Window > Package Manager > + > Add package from tarball...**
3. Select the downloaded `.tgz` file.

### Option C: From Local Folder (for contributors)

Clone and build the native plugin yourself:

**macOS:**
```bash
git clone https://github.com/DisplayXR/displayxr-unity.git
cd displayxr-unity
native~/build-mac.sh
```

**Windows** (requires [CMake](https://cmake.org/download/) and Visual Studio):
```cmd
git clone https://github.com/DisplayXR/displayxr-unity.git
cd displayxr-unity\native~
mkdir build && cd build
cmake .. -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

Then in Unity: **Window > Package Manager > + > Add package from disk...** → select `displayxr-unity/package.json`.

---

## Step 2: Enable the Feature

This step is the same on both platforms:

1. Go to **Edit > Project Settings > XR Plug-in Management**.

2. Under the **Standalone** tab (desktop icon), check **DisplayXR** — the custom `IUnityXRDisplay` provider (subsystem id `DisplayXR Display`). It drives the DisplayXR runtime directly, so you do **not** enable Unity's OpenXR plugin.

3. That's the whole setup — the provider is a single toggle. The plugin's post-build processor auto-deploys the native library + `UnitySubsystemsManifest.json` into player builds, so no manual copying is needed.

---

## Step 3: Set Up the Runtime

You need an OpenXR runtime active for the plugin to connect. For development without hardware, use the **sim_display** driver:

### macOS

Open Terminal **before** launching Unity (environment variables must be set in the process that spawns Unity):

```bash
# Point to your DisplayXR runtime build
export XR_RUNTIME_JSON=/path/to/SRDisplayXR-macOS/share/openxr/1/openxr_displayxr.json

# With no 3D panel present, the runtime uses sim_display automatically.
# Optionally pick the sim output format (default: sbs):
export SIM_DISPLAY_OUTPUT=sbs

# Launch Unity from this terminal
open -a "Unity Hub"
# Or launch Unity directly:
# /Applications/Unity/Hub/Editor/2022.3.XXf1/Unity.app/Contents/MacOS/Unity -projectPath /path/to/your/project
```

### Windows

Open Command Prompt or PowerShell **before** launching Unity:

**Command Prompt:**
```cmd
set XR_RUNTIME_JSON=C:\path\to\openxr_displayxr-dev.json

REM With no 3D panel, sim_display is used automatically; SIM_DISPLAY_OUTPUT is optional (default: sbs)
set SIM_DISPLAY_OUTPUT=sbs

start "" "C:\Program Files\Unity Hub\Unity Hub.exe"
```

**PowerShell:**
```powershell
$env:XR_RUNTIME_JSON = "C:\path\to\openxr_displayxr-dev.json"
$env:SIM_DISPLAY_OUTPUT = "sbs"   # optional; sim_display is the automatic fallback when no 3D panel is present

& "C:\Program Files\Unity Hub\Unity Hub.exe"
```

> **Tip:** On Windows, if the DisplayXR runtime was installed system-wide via `DisplayXRSetup-*.exe`, you don't need to set `XR_RUNTIME_JSON` — it's already registered. (For Leia hardware also install `DisplayXRLeiaSRSetup-*.exe` from the [Leia plug-in releases](https://github.com/DisplayXR/displayxr-leia-plugin/releases).)

---

## Step 4: Demo Scene A — Camera-Centric Mode

Camera-centric mode works like a standard Unity camera with added stereo. This is the right choice for **first-person views, free cameras, and fly-throughs** — anywhere the camera moves through the scene.

### Use the built-in sample

1. Open **Window > Package Manager > DisplayXR**.
2. Expand the **Samples** section and click **Import** next to **Basic Scene**.
3. Open the imported scene at `Assets/Samples/DisplayXR/<version>/Basic Scene/BasicScene.unity`
   (`<version>` is the installed package version, e.g. `2.13.2`).
4. The Main Camera already has a **DisplayXRCamera** component.

### Or build it from scratch

This takes about 5 minutes and teaches you the setup:

1. **File > New Scene > Basic (Built-in)**. Save it as `CameraDemo.unity`.

2. Select **Main Camera** in the hierarchy.

3. **Add Component > DisplayXR > Camera-Centric Rig** (the `DisplayXRCamera` component).

4. In the Inspector, you'll see:
   - A help box explaining camera-centric mode
   - **Connected Display** info (populated when runtime is active)
   - **Stereo Tunables**: IPD Factor, Parallax Factor
   - **Camera-Centric Parameters**: Inv. Convergence Distance (with meters readout)

5. Leave all defaults (IPD Factor = 1.0, Inv. Convergence Distance = 0). The camera's FOV is inherited from the Camera component automatically.

6. Build some test content at varying depths (the stereo effect depends on depth variation):

   ```
   Hierarchy:
     Main Camera          [DisplayXRCamera] at (0, 0, 0)
     NearCube             Red cube at (−0.3, 0, 0.3)    — pops out
     MidCube              Green cube at (0, 0, 0.5)      — at screen plane
     FarCube              Blue cube at (0.3, 0, 1.0)     — recedes behind screen
     Floor                Plane at (0, −0.3, 0.5)
     Directional Light
   ```

   To create each cube: **GameObject > 3D Object > Cube**, position it, scale to ~0.15–0.25, assign a colored material.

7. **Why this content suits camera-centric mode:** The camera is at the origin looking forward — like a person standing in a room. Objects are placed at varying distances to show the stereo depth gradient: near objects pop out of the screen, objects at the convergence distance sit on the screen plane, and far objects recede. Moving the camera (or the viewer's head) creates natural motion parallax through the scene.

### Why the Game view looks wrong until you press Play

This rig treats its transform as the **virtual display plane**, not as a viewpoint — that
exact pose is what the driver sends the runtime, and the Kooima projection puts the eyes in
front of it. Outside Play there is no provider and no Kooima, so Unity renders a plain
perspective camera sitting *on* the display plane: the Game view shows the scene from the
wrong place, usually near-clipping straight through your content. The framing only snaps to
what you authored once you press Play.

The rig's inspector has an **Edit-Mode Framing Preview** toggle (on by default, also under
**DisplayXR > Edit-Mode Framing Preview**) that frames the Game view as if a 2D camera were
viewing the virtual display, so authoring matches Play. It overrides the camera's view
matrix for rendering only — it never moves the transform, because for this rig the transform
*is* the display plane and moving it would change what the scene means in Play.

It is a **framing** preview, not a stereo preview: it answers "am I pointing at the right
thing", not "how does the depth feel". Play Mode runs the provider and remains the real
preview.

> Don't "fix" the framing by moving the camera back yourself. That moves the virtual display
> with it, and the scene will be wrong in Play in a way that is hard to spot.

### Key parameters to experiment with

| Parameter | Try this | Effect |
|-----------|----------|--------|
| Inv. Convergence Distance | 3.0 (= 0.33 m) | Screen plane moves closer — more content "pops out" |
| Inv. Convergence Distance | 1.0 (= 1.0 m) | Screen plane moves farther — most content is in front of screen |
| IPD Factor | 0.5 | Reduces stereo intensity (gentler depth) |
| IPD Factor | 2.0 | Exaggerates stereo (dramatic depth, may cause discomfort) |

---

## Step 5: Demo Scene B — Display-Centric Mode

Display-centric mode anchors a virtual display in the scene. The viewer looks "into" it like a window or display case. This is the right choice for **tabletop views, product viewers, museum exhibits, and AR-like object displays**.

### Use the built-in sample

1. Open **Window > Package Manager > DisplayXR**.
2. Import the **Display Scene** sample.
3. Follow the README in the imported folder to set up the hierarchy.

### Or build it from scratch

1. **File > New Scene > Basic (Built-in)**. Save it as `DisplayDemo.unity`.

2. Select **Main Camera** in the hierarchy.

3. **Add Component > DisplayXR > Display-Centric Rig** (the `DisplayXRDisplay` component).

4. Position Main Camera at `(0, 0, 0)` — this is now the virtual display's pose. The camera's transform represents where the display surface sits in the scene.

5. **Build tabletop content** — objects arranged around the camera's origin:

   ```
   Hierarchy:
     Main Camera          [DisplayXRDisplay] at (0, 0, 0)
     Crate                Textured cube at (0, 0.03, 0) — 0.06m, slowly rotating
     GroundGrid           Quad at (0, 0, 0) facing up — checker grid
     Directional Light
   ```

   Or simply add `DisplaySceneSetup.cs` to any object — it creates all content programmatically, plus a second camera with `DisplayXRCamera` for A/B comparison (press C to toggle).

6. **Why this content suits display-centric mode:** The display is a fixed anchor in the scene — like a glass case on a table. Objects at the display's origin sit "at the glass." Objects in front pop out toward the viewer; objects behind recede into the case. The 3D effect is stable regardless of where the viewer stands, because depth is computed relative to the display, not the camera.

### Key parameters to experiment with

| Parameter | Try this | Effect |
|-----------|----------|--------|
| Perspective Factor | 0.5 | Compresses perceived depth (less distortion at edges) |
| Virtual Display Height | 0.6 m | Larger virtual display — scene objects appear smaller |
| Virtual Display Height | 0.15 m | Smaller virtual display — objects appear larger (magnifier effect) |
| Parallax Factor | 0.0 | Disables motion parallax — stereo still works via IPD |

---

## Step 6: Test in the Editor

**Press Play.** The DisplayXR provider creates its runtime session and weaves the
composited stereo output to a dedicated window on the DisplayXR display — Play Mode
*is* the preview, there is no separate preview window. Your scene's
`DisplayXRDisplay` / `DisplayXRCamera` rigs drive the stereo projection, and normal
Unity input, physics, and game logic run alongside.

1. Verify the runtime is configured: the **DisplayXR** status panel (Project
   Settings > XR Plug-in Management > DisplayXR) should show a connected display or
   the sim_display info.

2. Open one of the demo scenes and press **Play**.

3. The sample `DisplayXRInputController` maps **V** to toggle 2D / 3D; add your own
   input for anything else (keybindings are app policy, not plugin policy).

4. **Enable Log Eye Tracking** on a rig component to see per-frame eye positions in
   the Console:
   ```
   [DisplayXR] Eyes: L=(0.032, 0.001, 0.504), R=(-0.031, 0.001, 0.504), tracked=True
   ```

> Displays advertising more than 8 views (many-view light fields, e.g. Looking
> Glass) are not yet supported — the provider caps at 8 views and logs a one-shot
> warning. See [ADR-007](adr/ADR-007-render-path-by-view-count.md).

---

## Step 7: Build a Standalone App

> **No scene component required.** With the DisplayXR provider enabled (Step 2), the
> plugin's post-build processor auto-deploys the native library + subsystem manifest,
> and the provider weaves the composited 3D output to the built app's window
> automatically.

### Windows Build

1. **File > Build Settings**.
2. Select **Windows, Mac, Linux** as the platform. Click **Switch Platform** if needed.
3. Set:
   - **Target Platform:** Windows
   - **Architecture:** x86_64
4. Add your demo scene(s) to the **Scenes In Build** list (drag from Project window or click **Add Open Scenes**).
5. Click **Player Settings** and verify:
   - **XR Plug-in Management > Standalone**: DisplayXR is checked
6. Click **Build**. Choose an output folder (e.g., `Builds/Windows/`).
7. Unity produces:
   ```
   Builds/Windows/
     YourApp.exe                     ← main executable
     YourApp_Data/
       Plugins/x86_64/
         displayxr_unity.dll          ← native plugin (auto-included)
     UnityPlayer.dll
     ...
   ```

The output is a standard `.exe` — no installer is needed for testing. For distribution, you can zip the folder or use an installer tool (Inno Setup, NSIS, etc.) if desired, but for pipeline testing, running the `.exe` directly is sufficient.

### macOS Build

1. **File > Build Settings**.
2. Select **macOS** as the platform. Click **Switch Platform** if needed.
3. Add your demo scene(s) to **Scenes In Build**.
4. Verify XR settings (same as Windows above).
5. Click **Build**. Choose an output location.
6. Unity produces a `.app` bundle:
   ```
   YourApp.app/
     Contents/
       Plugins/
         libdisplayxr_unity.dylib     ← native plugin (auto-included)
       MacOS/
         YourApp                     ← main executable
       ...
   ```

> **macOS Gatekeeper:** The first time you run an unsigned `.app`, macOS may block it. Right-click > Open, or: `xattr -cr YourApp.app` to strip the quarantine flag.

### Cross-Compiling (macOS to Windows)

You can build a Windows `.exe` from the macOS editor:

1. Install **Windows Build Support (Mono)** module via Unity Hub:
   - Unity Hub > Installs > (your version) > Add Modules > Windows Build Support (Mono)
2. In Build Settings, set **Target Platform** to **Windows**, architecture **x86_64**.
3. Build as normal. Unity includes the Windows DLL automatically.

The resulting `.exe` must be run on a Windows machine with the DisplayXR runtime installed.

---

## Step 8: Run the Built App

### Running on Windows

**With sim_display (no hardware):**

Open Command Prompt in the build folder:
```cmd
set XR_RUNTIME_JSON=C:\path\to\openxr_displayxr-dev.json
REM With no 3D panel, sim_display is used automatically; SIM_DISPLAY_OUTPUT is optional (default: sbs)
set SIM_DISPLAY_OUTPUT=sbs

YourApp.exe
```

**With the installed runtime:**

If the DisplayXR runtime was installed system-wide (`DisplayXRSetup-*.exe`), just double-click `YourApp.exe` — no environment variables needed. For Leia hardware, also install `DisplayXRLeiaSRSetup-*.exe` from the [Leia plug-in releases](https://github.com/DisplayXR/displayxr-leia-plugin/releases) (in addition to, not instead of, the runtime).

**With runtime in a custom path:**
```cmd
set XR_RUNTIME_JSON=C:\path\to\openxr_displayxr-dev.json
YourApp.exe
```

### Running on macOS

Open Terminal:
```bash
export XR_RUNTIME_JSON=/path/to/SRDisplayXR-macOS/share/openxr/1/openxr_displayxr.json

# Without a 3D panel the runtime uses sim_display automatically.
# Optionally pick the sim output format (default: sbs):
export SIM_DISPLAY_OUTPUT=sbs

# Launch the app
open YourApp.app

# Or launch directly (useful if 'open' doesn't pass env vars):
./YourApp.app/Contents/MacOS/YourApp
```

> **Important:** On macOS, `open` may not forward environment variables to the app. If the app doesn't connect to the runtime, use the direct executable path instead.

### Verifying the Pipeline

Once the app is running, verify each stage:

| Check | What to look for |
|-------|-----------------|
| **Runtime connects** | No "No OpenXR runtime found" error in the Player.log |
| **Stereo renders** | With `SIM_DISPLAY_OUTPUT=sbs`, you see a side-by-side stereo pair |
| **Depth is correct** | Near objects appear shifted between left/right eyes; far objects are nearly identical |
| **Camera-centric mode** | Scene parallax shifts as (simulated) head moves |
| **Display-centric mode** | Objects rotate smoothly on the turntable; depth is relative to the display anchor |
| **Log file location** | Windows: `%APPDATA%\..\LocalLow\<Company>\<Product>\Player.log` |
| | macOS: `~/Library/Logs/<Company>/<Product>/Player.log` |

### Reading the Log

Search the Player.log for these key lines:

```
[DisplayXR-PROV] provider registered, subsystem 'DisplayXR Display' started
[DisplayXR] Display info: 1920x1080, 27.0x15.2 cm, viewer at 500 mm
[DisplayXR] Eye tracking active
```

If you see `[DisplayXR] Feature not active` or no DisplayXR lines at all, the feature is not enabled in XR settings — go back to Step 2.

---

## What to Look For

### Camera-centric demo
- **Stereo depth gradient:** In the preview window, the red near cube should appear to float in front of the screen. The green mid cube sits at the screen plane. The blue far cube is behind the screen.
- **Motion parallax:** When the viewer moves their head (or the sim_display keyboard controls move the simulated eye position), the scene shifts naturally — near objects move more than far objects.
- **Inv. convergence distance:** Adjusting this value moves where the "screen plane" sits in depth. Higher values (e.g. 3.0 = 0.33 m) make content pop out more; lower values (e.g. 1.0 = 1.0 m) push most content behind the screen.

### Display-centric demo
- **Crate stability:** As the crate rotates, the 3D effect should remain stable — the crate doesn't wobble or swim. This is because depth is computed relative to the display anchor, not the camera.
- **Pop-out vs. recede:** The crate sits slightly above the display plane — parts pop out toward the viewer, parts recede behind the glass.
- **Scale effect:** Changing Virtual Display Height makes the whole scene appear larger or smaller, like zooming a magnifying glass. The parent transform's scale also acts as zoom.
- **A/B comparison:** Press C to switch to the camera-centric camera and compare the same scene from both paradigms.

---

## Next Steps

- **Tune the stereo parameters** — see the [Stereo Tunables Reference](../README.md#stereo-tunables-reference) for what each parameter does physically.
- **Add a 2D UI overlay** — see [2D UI Overlay](../README.md#2d-ui-overlay) for routing a Canvas to a compositor layer.
- **Preview in the editor** — just press Play; the provider weaves to a dedicated window on the DisplayXR display (there is no separate preview window).
- **Deploy to end users** — see [Deploying to End Users](../README.md#deploying-to-end-users) for runtime installation on target machines.
- **Build for both platforms** — the plugin includes both Windows and macOS binaries. Unity selects the correct one per build target.
