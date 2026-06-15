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

Use the platform build scripts (they wrap CMake and place the shipping binary):

- **macOS (shipping binary):** `native~/build-mac.sh` → Universal (x86_64 + arm64) `Runtime/Plugins/macOS/displayxr_unity.bundle`.
- **Windows (shipping binary, MSVC):** `native~\build-win.bat` → `Runtime/Plugins/Windows/x64/displayxr_unity.dll`. Needs VS 2022 (or Build Tools) + "Desktop development with C++"; run from a Developer Command Prompt or any shell with MSVC on PATH.
- **Windows (MinGW, compile-check only):** `native~/build-win.sh` → leaves the DLL in `build-win/` (MinGW ABI, not shipped). Run on macOS as a cross-compile check.

Raw CMake (`cd native~ && mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && cmake --build . --config Release`) works too, but the scripts handle output placement.

**Claude Code: after modifying any file in `native~/`, run the build script for the current platform (`native~/build-mac.sh` on macOS, `native~\build-win.bat` on Windows; also `native~/build-win.sh` as a cross-compile check on macOS) to refresh the shipping binary. Then commit source + your platform's binary, push to a feature branch, and open a PR — CI builds both platforms.**

## Key Architecture

### Three Layers

1. **Runtime (C#)** — `DisplayXRFeature.cs` hooks into OpenXR lifecycle; `DisplayXRCamera.cs` and `DisplayXRDisplay.cs` are the two stereo rig modes; `DisplayXRRigManager.cs` coordinates multi-camera scenes; `DisplayXRPreview.cs` provides inline preview textures (SBS, readback, SharedTexture)
2. **Editor (C#)** — Custom inspectors, settings page, and the standalone preview system (`DisplayXRPreviewSession.cs` manages an independent OpenXR session; `DisplayXRPreviewWindow.cs` provides the editor UI with camera selector and rendering mode controls)
3. **Native (C/C++)** — Hook chain on `xrLocateViews`, `xrCreateSession`, `xrGetSystemProperties`, `xrEndFrame`; chains the runtime's view-rig descriptor (`XR_EXT_view_rig`) and consumes render-ready views — the runtime owns the Kooima math; thread-safe shared state

### Key Features

- **Two stereo rig modes**: Camera-centric (`DisplayXRCamera` — inherits camera FOV, inv. convergence distance tunable) and display-centric (`DisplayXRDisplay` — physical display geometry, virtual display height, scale-as-zoom)
- **Multi-camera support**: Multiple rigs coexist in one scene; `DisplayXRRigManager` coordinates which rig is active (see below)
- **Standalone editor preview**: Own OpenXR session bypassing Unity XR. Camera selector dropdown, dynamic rendering mode enumeration, zero-copy SharedTexture output (IOSurface/DXGI). Replaces Play Mode for DisplayXR workflows.
- **Play Mode conflict prevention**: Preview auto-removes Unity's OpenXR loader on Play entry, restores on exit (saved via SessionState)
- **2D UI overlay**: Canvas → `XrCompositionLayerWindowSpaceEXT` with stereo disparity
- **Runtime-owned Kooima math (`XR_EXT_view_rig`, #396 W7)**: the plugin no longer computes Kooima. It chains an `XrDisplayRigEXT`/`XrCameraRigEXT` descriptor (the handful of tunables) onto `xrLocateViews` and consumes render-ready `XrView{pose, fov}` — on both the built-app hook path and the standalone preview session. **This requires a runtime that advertises `XR_EXT_view_rig` (SPEC_VERSION 2)**; against an older runtime the plugin emits a one-shot WARN and passes raw views through (no stereo). The former vendored/`displayxr::math` display3d/camera3d math is gone (do not re-add — see the `no-vendored-math` drift guard).

### Render Pipeline Support (BiRP vs URP)

Both pipelines render from the **same source of truth** — the per-eye matrices from
`DisplayXRFeature.GetStereoMatrices` (`leftProj`/`rightProj`) — but inject the projection through
**two thin, non-shared adapters** (the injection mechanism is pipeline-specific and cannot be shared):

- **BiRP adapter**: the rig (`DisplayXRDisplay`/`DisplayXRCamera.OnCameraPreRender`) calls
  `Camera.SetStereoProjectionMatrix(leftProj/rightProj)`. BiRP honors it; the override carries the
  off-center frustum shear and the foreground-clip per-view far directly in the projection (#396/#57).
- **URP adapter**: URP **ignores** `SetStereoProjectionMatrix` (Unity #1328435) and builds each eye's
  projection from `views[i].fov`, which it mishandles for strongly off-center frustums (head x<0
  shifts/deforms). The fix lives in a **URP-guarded sub-assembly** (`Runtime/URP/`,
  `Editor/URP/` — asmdefs gated by `defineConstraints: ["DISPLAYXR_URP"]` + a `versionDefines` that
  defines `DISPLAYXR_URP` only for `com.unity.render-pipelines.universal >= 17.0.0`, so BiRP-only and
  older-URP projects never compile it):
  - **`KooimaProjectionFixFeature`** (universal, auto-wired) — a `ScriptableRendererFeature` that
    re-pushes the correct per-eye `leftProj`/`rightProj` via `cmd.SetViewProjectionMatrices` at
    `BeforeRenderingOpaques` (URP pushes the projection once per eye-pass at camera setup, not per
    draw, so it sticks). Has a NaN/identity startup guard. **Every URP DisplayXR app needs this** —
    `DisplayXRUrpAutoWire` adds it automatically when a URP DisplayXR rig is in an open scene (toggle:
    `DisplayXR > Auto-Wire URP Projection Fix`; or run `DisplayXR > Setup URP Projection Fix`).
  - **`DisplayXR/ForegroundClipURP`** (opt-in, transparent-overlay apps only) — a per-eye depth-based
    foreground clip. The rig publishes the two per-eye fars + eye positions via the `_DXRForegroundFar`
    / `_DXREyePosL`/`_DXREyePosR` globals (the rig's URP branch does this and nothing else — no
    projection/`farClipPlane` hacks); Unity's built-in `FullScreenPassRendererFeature` + a shipped
    material do the clip. Wire with `DisplayXR > Setup URP Foreground Clip`. The globals are inert if
    the pass isn't wired, so they're safe for normal URP apps. The clip shader lives in
    `Runtime/URP/Shaders/` and **must never move into `Runtime/Resources/`** — a URP-include shader in
    `Resources/` force-compiles in every build and breaks BiRP-only projects (the #130 revert).
  - **URP transparency** also requires the per-project Player Setting **Preserve Framebuffer Alpha**
    (`preserveFramebufferAlpha = 1`) — the plugin cannot set it at runtime; document it for URP
    transparent apps (without it, HDR+32-bit picks `B10G11R11`, no alpha → opaque black).
  - HDRP gets no off-axis fix (the RendererFeature is URP-only); see #127/#129.

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
- `xrLocateViews` → chains an `XR_EXT_view_rig` descriptor (built from scene transform + tunables) and consumes the runtime's render-ready `XrView{pose, fov}`. Also builds the BiRP view-matrix handedness shim and applies the URP head-pose comp (#115). Falls back to raw passthrough (one-shot WARN) if the runtime lacks `XR_EXT_view_rig`.
- `xrCreateSession` → injects window binding extension
- `xrGetSystemProperties` → extracts display info; detects `XR_EXT_view_rig`
- `xrCreateSwapchain` → downgrades sRGB color swapchains to UNORM in Gamma-space projects (DXGI/Vulkan/Metal) so output isn't double-gamma-encoded
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

### Resolved: window-drag phase-snap (#61)

**Window-drag phase-snap is solved** (issue #61, closed 2026-05-09, hardware-verified on both paths). The SR SDK weaver only phase-snaps to lenticular-aligned positions when its WndProc subclass sees the OS in-drag flag. Rather than hand the OS the modal drag loop (which our `WS_POPUP` + cloaked + `WS_EX_NOREDIRECTIONBITMAP`/`WS_EX_NOACTIVATE` overlay can't do — synthesized `WM_NCLBUTTONDOWN`/`SC_MOVE` need a DWM redirection surface), we keep the capture-based custom drag and **bracket it** by synthesizing `WM_ENTERSIZEMOVE`/`WM_EXITSIZEMOVE` via `SendMessageW` around the drag. The weaver's subclass sees the bracket and phase-snaps; Kooima stays live throughout (no stutter, no update-only-on-release). Implemented in both `displayxr_win32.c` `overlay_wnd_proc` (transparent overlay right-drag, #57) and `displayxr_standalone.cpp` `sa_wndproc` (preview window). The runtime "external drag" API (`DisplayXR/displayxr-runtime#193`) is no longer needed for this.

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

### /release — Tagged Release
Bumps the version, tags, pushes, monitors CI, and verifies the GitHub Release + `upm` branch. See "Creating a release" above. Skill at `.claude/skills/release/SKILL.md`.

Always include the related GitHub issue number in commit messages (e.g. `Fix linker error (#93)`).

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

- Runtime repo: [DisplayXR/displayxr-runtime](https://github.com/DisplayXR/displayxr-runtime). Use `DisplayXR/displayxr-runtime#N` to reference runtime issues.
- The runtime provides the OpenXR compositor, display drivers, and eye tracking; this plugin provides the Unity-side stereo rendering pipeline.
- **Decoupled from the runtime's `versions.json` bundle matrix** — this UPM package is a downstream consumer of the runtime's OpenXR wire protocol, not part of the co-released installer bundle (runtime/shell/leia-plugin/mcp/demos). The two ship on independent cadences. Spec: [`versions-json-autobump.md`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/runtime/versions-json-autobump.md).

### Test repos

Three sibling Unity projects exercise the plugin against different feature areas. Treat them as a regression net — when a plugin change risks affecting any of these, fetch and verify before tagging a release.

| Repo | Focus | Notes |
|------|-------|-------|
| [`DisplayXR/displayxr-unity-test`](https://github.com/DisplayXR/displayxr-unity-test) | Baseline rendering / stereo correctness | Plain cube + camera-centric and display-centric rigs |
| [`DisplayXR/displayxr-unity-test-transparent`](https://github.com/DisplayXR/displayxr-unity-test-transparent) | Transparent overlay + click-through (#57 family, alpha-native) | Tiger FBX clickable, foreground-only render |
| [`DisplayXR/displayxr-unity-test-2d-ui`](https://github.com/DisplayXR/displayxr-unity-test-2d-ui) | 2D UI window-space composition layer | Tuning panel built from `DisplayXRWindowSpaceUI` |

All three pin the plugin via `https://github.com/DisplayXR/displayxr-unity.git#upm` (floating; tracks latest release).

Each test repo also has its own `CLAUDE.md` describing its scene, scripts, and which plugin features it exercises — designed so an agent can work in the test repo without loading the plugin's context.

**Test-repo releases ship as NSIS installers, not zips** (#108). Each test repo's `installer/` dir (`.nsi` + `build-installer.bat`) mirrors the [`displayxr-demo-gaussiansplat`](https://github.com/DisplayXR/displayxr-demo-gaussiansplat) pattern: hard-prereqs the runtime, installs the Unity Player under `Program Files\DisplayXR\Unity\<Variant>\`, and drops a registered-mode `.displayxr.json` manifest under `%ProgramData%\DisplayXR\apps\` (renamed `icon_unity_test*.png` per variant) so the Shell launcher discovers it as a tile. Build flow is manual today (build Player → `installer\build-installer.bat` → `gh release create`); CI automation is blocked on Unity license activation.

#### Where to launch Claude Code when working on the test repos

- **Test-only work** (tweak a scene, polish a test-repo script, fix a test-repo bug) → launch from the test repo directly. Its `CLAUDE.md` auto-loads with focused context, git ops target the right repo by default, smaller context is cheaper and faster. The plugin's installed source is still readable at `Library/PackageCache/com.displayxr.unity@<hash>/` if a grep into plugin internals is needed.
- **Plugin work that also touches a test repo** (new plugin API + test repo update to consume it) → launch from `displayxr-unity`. The plugin is the primary surface; the test repos are reachable via adjacent `../displayxr-unity-test*` paths. Land the plugin change first, let CI publish `#upm`, then update the test repo.
