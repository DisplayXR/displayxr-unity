# CLAUDE.md

This file provides guidance to Claude Code when working with this repository.

## Project Overview

Unity plugin for eye-tracked 3D light field displays via the **DisplayXR OpenXR runtime**. This is a Unity Package Manager (UPM) package that intercepts Unity's OpenXR pipeline at the native layer to provide Kooima asymmetric frustum projection for stereo rendering. The primary editor workflow is a **standalone preview window** that creates its own OpenXR session — no Play Mode needed.

The plugin works with the **DisplayXR runtime** ([DisplayXR/displayxr-runtime](https://github.com/DisplayXR/displayxr-runtime)) but has **no source dependency** on it — native code fetches OpenXR headers independently from Khronos.

## Repository Structure

This repo root IS the UPM package root (`package.json` is at the top level).

```
displayxr-unity/               # repo root = UPM package root
├── package.json               # UPM manifest
├── Runtime/                   # C# runtime scripts + native plugin binaries
│   ├── *.cs                   # MonoBehaviours and OpenXR Feature
│   └── Plugins/               # Built native binaries (Windows/macOS)
├── Editor/                    # C# editor-only scripts (inspectors, settings)
├── Samples~/                  # UPM samples (lazy-imported by user)
├── native~/                   # Native C/C++ plugin source (not imported by Unity)
│   └── CMakeLists.txt         # Independent CMake build
└── .github/workflows/         # CI for building native plugin
```

## Building the Native Plugin

```bash
cd native~
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

Output: `displayxr_unity.dll` (Windows) or `libdisplayxr_unity.dylib` (macOS).

Copy built binary to `Runtime/Plugins/Windows/x64/` or `Runtime/Plugins/macOS/`.

### Local builds

**macOS (native, produces shipping binary):**
```bash
native~/build-mac.sh
```
Builds Universal binary (x86_64 + arm64) → `Runtime/Plugins/macOS/displayxr_unity.bundle`.

**Windows (native MSVC, produces shipping binary):**
```bat
native~\build-win.bat
```
Builds x64 DLL with MSVC → `Runtime/Plugins/Windows/x64/displayxr_unity.dll`. Requires Visual Studio 2022 (or Build Tools) with the "Desktop development with C++" workload. Run from a Developer Command Prompt or any shell with MSVC on PATH.

**Windows (MinGW cross-compile, compile check only):**
```bash
native~/build-win.sh
```
Verifies the code compiles for Windows but the DLL stays in `build-win/` (MinGW ABI, not shipped). For native Windows builds, use `build-win.bat` instead.

**Claude Code: After modifying any file in `native~/`, always run the local build script for the current platform — `native~/build-mac.sh` on macOS or `native~\build-win.bat` on Windows — to update the shipping binary. On macOS, also run `native~/build-win.sh` as a cross-compile check. Then commit (source + your platform's binary), push to a feature branch, and open a PR — CI builds both platforms automatically and reports back on the PR.**

## Key Architecture

### Three Layers

1. **Runtime (C#)** — `DisplayXRFeature.cs` hooks into OpenXR lifecycle; `DisplayXRCamera.cs` and `DisplayXRDisplay.cs` are the two stereo rig modes; `DisplayXRRigManager.cs` coordinates multi-camera scenes; `DisplayXRPreview.cs` provides inline preview textures (SBS, readback, SharedTexture)
2. **Editor (C#)** — Custom inspectors, settings page, and the standalone preview system (`DisplayXRPreviewSession.cs` manages an independent OpenXR session; `DisplayXRPreviewWindow.cs` provides the editor UI with camera selector and rendering mode controls)
3. **Native (C/C++)** — Hook chain on `xrLocateViews`, `xrCreateSession`, `xrGetSystemProperties`, `xrEndFrame`; Kooima projection math; thread-safe shared state

### Key Features

- **Two stereo rig modes**: Camera-centric (`DisplayXRCamera` — inherits camera FOV, inv. convergence distance tunable) and display-centric (`DisplayXRDisplay` — physical display geometry, virtual display height, scale-as-zoom)
- **Multi-camera support**: Multiple rigs coexist in one scene; `DisplayXRRigManager` coordinates which rig is active (see below)
- **Standalone editor preview**: Own OpenXR session bypassing Unity XR. Camera selector dropdown, dynamic rendering mode enumeration, zero-copy SharedTexture output (IOSurface/DXGI). Replaces Play Mode for DisplayXR workflows.
- **Play Mode conflict prevention**: Preview auto-removes Unity's OpenXR loader on Play entry, restores on exit (saved via SessionState)
- **2D UI overlay**: Canvas → `XrCompositionLayerWindowSpaceEXT` with stereo disparity
- **Native Kooima math**: `display3d_view.c` (screen-edge frustum) and `camera3d_view.c` (tangent-space frustum) — pure C, no DisplayXR dependency

### Multi-Camera Rig Management

Scenes can contain multiple cameras with different rig types (display-centric, camera-centric, or plain cameras). A static registry coordinates which rig is active at any time.

**`DisplayXRRigManager`** (static class, no scene object needed):
- Rigs self-register in `OnEnable`, self-deregister in `OnDisable`
- First registered camera is auto-elected as `ActiveCamera`
- `CycleNext()` advances to the next registered camera (used by Tab key)
- `ActiveCamera` property is the single source of truth for rig gating and input

**Rig gating**: `DisplayXRDisplay.LateUpdate()` and `DisplayXRCamera.LateUpdate()` check `DisplayXRRigManager.ActiveCamera` before pushing tunables to the native hook chain. Only the active rig pushes — prevents multi-rig conflicts (wrong projection, FOV feedback loops).

**Input isolation**: `DisplayXRInputController.IsActiveCamera()` returns true only for the active camera's controller. Inactive controllers clear their drag state to prevent rotation jumps on reactivation.

**Component reference:**

| Component | Required | Purpose |
|-----------|----------|---------|
| `DisplayXRDisplay` | One of | Display-centric stereo rig (scale-as-zoom) |
| `DisplayXRCamera` | One of | Camera-centric stereo rig (FOV-based) |
| `DisplayXRRigManager` | Automatic | Static camera registry — no scene object, rigs self-register |
| `DisplayXRInputController` | Optional | Sample WASD/mouse/scroll controller. Tab cycles cameras via `DisplayXRRigManager.CycleNext()`. Developers typically replace this with their own input. |
| `DisplayXRGameViewOverlay` | Optional | Editor play-mode only: draws shared texture in Game View, suppresses scene rendering. Not needed in built apps. |

### OpenXR Hook Chain

The native plugin intercepts OpenXR calls via `xrGetInstanceProcAddr` hooking:
- `xrLocateViews` → applies scene transform + tunables + Kooima projection
- `xrCreateSession` → injects window binding extension
- `xrGetSystemProperties` → extracts display info
- `xrEndFrame` → submits overlay composition layers

### Shared Texture Architecture (IOSurface / DXGI)

The compositor shares a GPU texture with the Unity plugin for preview and game overlay output. Two consumers display this texture:

- **Standalone Preview Window** (`Editor/DisplayXRPreviewWindow.cs`) — EditorWindow, no Play Mode
- **Game View Overlay** (`Runtime/DisplayXRGameViewOverlay.cs`) — MonoBehaviour, Play Mode

**IOSurface / shared texture contract:**

| Property | Value | Source |
|----------|-------|--------|
| Texture size | Display pixel dimensions (worst-case) | Created once at session start, never resized |
| Content region | Top-left corner, canvas.w × canvas.h | App decides canvas, tells runtime |
| UV crop | (canvasW / surfaceW, canvasH / surfaceH) | App computes from known dims |
| Letterbox aspect | canvasW / canvasH | Canvas aspect, not surface aspect |

**Flow each frame:**
1. App computes canvas = view area in backing pixels (preview rect or Screen size)
2. App calls `xrSetSharedTextureOutputRectEXT(session, x, y, w, h)` via `displayxr_standalone_set_canvas_rect()` — tells the runtime where to render and at what size
3. Runtime/compositor writes interlaced output to IOSurface at (0, 0, canvasW, canvasH)
4. App creates `Texture2D.CreateExternalTexture()` at full IOSurface dims, but samples only the canvas portion via UV scaling: `Rect(0, vMax, uMax, -vMax)` (Y-flipped for Metal)
5. App letterboxes using canvas aspect ratio (since canvas = view size, typically no letterbox)

**Screen position (x, y):** Required for pixel-precise interlacing alignment on lenticular displays. The runtime uses this internally in the Display Processor — the app doesn't need it back.

### Wire Protocol

Extension struct definitions in `native~/displayxr_extensions.h` must match the runtime's implementation. These are versioned by extension spec version. When changing extensions, update the runtime first, then the plugin.

## Development Workflow

### Testing in Unity

1. Open any Unity 2022.3+ project
2. Add this package via Package Manager (local path or git URL)
3. Enable the feature: Project Settings > XR Plug-in Management > OpenXR > DisplayXR
4. Set `XR_RUNTIME_JSON` environment variable to point to a DisplayXR runtime build (or use `SIM_DISPLAY_ENABLE=1 SIM_DISPLAY_OUTPUT=sbs` for testing without hardware)
5. **Open Window > DisplayXR > Preview Window, click Start** — this is the primary workflow
6. Play Mode still works but the standalone preview is preferred (avoids XR session conflicts)

### Critical: OpenXR Package Version

**You MUST use `com.unity.xr.openxr` version 1.16.1 or later.** The minimum version in `package.json` is 1.9.1 for broad compatibility, but versions before 1.16.1 ignore the `XR_RUNTIME_JSON` environment variable and the system `active_runtime.json` in editor play mode — they silently fall back to Unity's built-in Mock Runtime. This causes `0x0` display resolution, no IOSurface/shared texture, and no display info from the runtime. The failure is silent (no error logged, just mock runtime loaded). Always pin `1.16.1+` in your test project's `Packages/manifest.json`.

### Known Issues

**Windows preview window: stutter vs. real-time Kooima during drag (parked).**
The standalone preview window on Windows has two mutually-exclusive behaviors during a window move:

1. **DefWindowProc modal drag** (current default): the SR SDK weaver phase-snaps the window to lenticular-aligned positions (no stutter), but Unity's main thread is blocked, so FrameTick can't run and Kooima only updates on drag release.
2. **SC_MOVE intercept** (capture-based custom move): FrameTick keeps running so Kooima updates in real time, but the SR weaver doesn't get a chance to phase-snap, producing visible stutter.

We tried calling `xrSetSharedTextureOutputRectEXT` from `WM_MOVE` to push canvas updates to the runtime — this is required for the weaver to interlace at all in windowed mode (without it, weaving only works in fullscreen) — but it doesn't trigger phase-snapping. The SR SDK only phase-snaps when it owns the modal drag loop.

A proper fix likely needs runtime API support: an "external drag" mode where the app proposes a position (e.g. via `xrSetSharedTextureOutputRectEXT` extended) and the runtime returns the snapped position. Tracked in `DisplayXR/displayxr-runtime#193`. Until then, the WndProc in `displayxr_standalone.cpp` keeps SC_MOVE handling commented out / parked. See the long comment in `sa_wndproc`'s `WM_SYSCOMMAND` case for context.

**Transparent overlay (#57) right-drag — same SR phase-snap blocker as the standalone preview, no workaround possible from the plugin side.** Regular opaque DisplayXR built apps don't stutter when dragged because Unity has a real title bar — drag the title, `DefWindowProc` enters the OS modal drag loop, the SR weaver phase-snaps inside that loop. In transparent overlay mode Unity is `WS_POPUP` + cloaked with no title bar, so the user drags via right-click on the cube body. Two attempts to inherit opaque-mode behavior both failed:

1. `SendMessage(unity_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0)` from the overlay's `WM_RBUTTONDOWN` — silently ignored by `DefWindowProc` on cloaked `WS_POPUP` Unity. No `WM_ENTERSIZEMOVE`, no drag.
2. `SendMessage(overlay, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0)` — also silently ignored. The overlay has `WS_EX_NOREDIRECTIONBITMAP` (mandatory for per-pixel transparency) + `WS_EX_NOACTIVATE`, and `DefWindowProc`'s modal drag needs a DWM redirection surface to render the drag preview, so it bails. Adding `WS_CAPTION` to the overlay would change the client-area math and break the C#-supplied hit-rect coordinates.

Current implementation is back to capture-based custom drag (`SetCapture` + manual `SetWindowPos(unity)` per `WM_MOUSEMOVE`) in `overlay_wnd_proc`. Cube/Kooima keep animating during drag, but no SR weaver phase-snap → 3D stutter visible during motion. Same fundamental blocker as the standalone preview — the SR SDK only phase-snaps when it owns the modal drag loop, and we can't induce one on a window with our style requirements. Resolution tracked in `DisplayXR/displayxr-runtime#193` (external drag API).

### Code Style

- C# follows Unity conventions (PascalCase for public members, camelCase for private)
- C/C++ follows the DisplayXR coding style (snake_case, C11/C++17)
- Native code in `native~/` uses tabs for indentation

## CI and Releases

### Repository

| Remote | URL |
|--------|-----|
| `origin` | `https://github.com/DisplayXR/displayxr-unity.git` |

> **History:** This repo was transferred from `dfattal/unity-3d-display` to `DisplayXR/displayxr-unity`. GitHub redirects the old URL.

### CI policy: contributor-friendly (this is a public repo)

Public-repo CI is **free** on GitHub-hosted runners, so `build-native.yml`
fires freely — favoring fast contributor feedback over minute-burn. Mirrors
the policy used by `DisplayXR/displayxr-runtime`:

| Trigger | Effect |
|---|---|
| **Push to `main`** | Builds Windows x64 DLL + macOS Universal bundle. Sanity check that catches the rare admin-bypass merge that lands red. |
| **All PRs (drafts included)** | Same build runs on the PR head. No path filter — the build is ~45s, not worth the gating complexity. PRs report status before merge. |
| **`v*` tag push** | Builds + runs the `release` job (UPM tarball, `upm` branch update, GitHub Release). |
| **`workflow_dispatch`** | Manual run from the Actions tab. |
| **Concurrency** | `cancel-in-progress: true` — rapid pushes to a PR cancel the in-progress run; only the latest commit's CI completes. |

#### Standard contributor flow

For native or workflow changes, use a feature branch + PR — don't direct-push
to `main`. The PR run gives you the green checkmark before merge.

```bash
git checkout -b fix/short-description
# ... edit native~/, run native~\build-win.bat or native~/build-mac.sh ...
git add native~/<files> Runtime/Plugins/<platform>/<binary>
git commit -m "Short subject (#123)"     # always include the issue number
git push -u origin fix/short-description
gh pr create --fill                       # opens PR; CI fires on the PR head
```

C#-only changes can be merged the same way. The native build still fires
on the PR (cheap and free), and you get a green check before merge.

For tagged releases, use the `/release` skill — see below.

### Creating a release: use the `/release` skill (don't tag manually)

`/release` is the official release path for this repo. It bumps the version,
updates `CHANGELOG.md` if needed, tags, pushes, monitors CI, and verifies the
GitHub Release + `upm` branch were created. Don't tag and push manually —
you'll skip the verification steps the skill performs.

```
/release v1.2.0      # explicit version
/release patch       # auto-bump from latest v* (e.g. v1.0.0 → v1.0.1)
/release minor       # auto-bump minor       (e.g. v1.0.0 → v1.1.0)
/release major       # auto-bump major       (e.g. v1.0.0 → v2.0.0)
```

The skill triggers the CI `release` job which:
- Builds both platform binaries.
- Creates a `.tgz` UPM tarball with binaries included.
- Pushes the `upm` branch (binaries committed) for git URL installs.
- Creates a `upm/vX.Y.Z` tag for version-pinned installs.
- Publishes a GitHub Release with changelog notes and the `.tgz` attached.

Since CI is free here, run releases freely — no need to batch.

### Fixing a bad release
Tags are cheap and deletable:
```bash
git tag -d v0.1.0                         # delete local
git push origin :refs/tags/v0.1.0         # delete on origin
# delete GitHub Release in the web UI
# fix the issue, then re-run /release
```

### Install paths for users

| Method | URL | Notes |
|--------|-----|-------|
| Git URL (latest release) | `https://github.com/DisplayXR/displayxr-unity.git#upm` | Tracks latest release |
| Git URL (pinned version) | `https://github.com/DisplayXR/displayxr-unity.git#upm/v0.1.0` | For production |
| Tarball | Download `.tgz` from Releases page | Offline installs |
| Local dev | Clone repo + build `native~/` yourself | For contributors |

The `upm` branch and release tarball only exist after the first `v*` tag is pushed.

## Claude Code Skills

### /release - Tagged Release
Bumps the version, tags, pushes, monitors CI, and verifies the GitHub Release
+ `upm` branch were created. See the "Creating a release" section above for
usage. Skill at `.claude/skills/release/SKILL.md`.

> **Note:** A `/ci-monitor` skill previously automated direct-push-to-main +
> watch-the-build. It was retired when the CI policy moved to PR-driven
> builds — open a PR and let CI report on the PR head. Always include the
> related GitHub issue number in commit messages (e.g. `Fix linker error
> (#93)`).

## Documentation Index

For detailed architecture and design decisions, see `docs~/`:

- Understand the preview system → `docs~/architecture/preview-session.md`
- Understand OpenXR hooking → `docs~/architecture/hook-chain.md`
- Understand stereo math → `docs~/architecture/kooima-pipeline.md`
- Why deferred destruction? → `docs~/adr/ADR-001-deferred-destruction.md`
- Why SA session vs XR loader? → `docs~/adr/ADR-002-dual-session.md`
- Why native window? → `docs~/adr/ADR-003-native-preview-window.md`
- Why two rig modes? → `docs~/adr/ADR-004-camera-vs-display-mode.md`
- Full docs index → `docs~/README.md`

## Cross-Repo References

- Runtime repo: [DisplayXR/displayxr-runtime](https://github.com/DisplayXR/displayxr-runtime)
- Use `DisplayXR/displayxr-runtime#N` syntax to reference runtime issues
- The runtime provides the OpenXR compositor, display drivers, and eye tracking
- The plugin provides the Unity-side stereo rendering pipeline

### Test repos

Three sibling Unity projects exercise the plugin against different feature areas. Treat them as a regression net — when a plugin change risks affecting any of these, fetch and verify before tagging a release.

| Repo | Focus | Notes |
|------|-------|-------|
| [`DisplayXR/displayxr-unity-test`](https://github.com/DisplayXR/displayxr-unity-test) | Baseline rendering / stereo correctness | Plain cube + camera-centric and display-centric rigs |
| [`DisplayXR/displayxr-unity-test-transparent`](https://github.com/DisplayXR/displayxr-unity-test-transparent) | Transparent overlay + chroma key + click-through (#57 family) | Tiger FBX clickable, foreground-only render |
| [`DisplayXR/displayxr-unity-test-2d-ui`](https://github.com/DisplayXR/displayxr-unity-test-2d-ui) | 2D UI window-space composition layer | Tuning panel built from `DisplayXRWindowSpaceUI` |

All three pin the plugin via `https://github.com/DisplayXR/displayxr-unity.git#upm` (floating; tracks latest release).
