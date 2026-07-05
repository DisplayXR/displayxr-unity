# CLAUDE.md

This file provides guidance to Claude Code when working with this repository.

## Project Overview

Unity plugin for eye-tracked 3D light field displays via the **DisplayXR runtime**. This is a Unity Package Manager (UPM) package that provides stereo via a custom **`IUnityXRDisplay` display provider** which drives the DisplayXR runtime for Kooima asymmetric frustum projection. The editor workflow is **Play Mode**: pressing Play runs the provider — that *is* the preview. There is no separate edit-mode preview window.

The plugin works with the **DisplayXR runtime** ([DisplayXR/displayxr-runtime](https://github.com/DisplayXR/displayxr-runtime)) but has **no source dependency** on it — native code fetches OpenXR headers independently from Khronos.

## Repository Structure

This repo root IS the UPM package root (`package.json` is at the top level).

```
displayxr-unity/               # repo root = UPM package root
├── package.json               # UPM manifest
├── Runtime/                   # C# runtime scripts + native plugin binaries
│   ├── *.cs                   # MonoBehaviours + IUnityXRDisplay provider (Provider/)
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

1. **Runtime (C#)** — the **`IUnityXRDisplay` display provider** (`Runtime/Provider/`): `DisplayXRDisplayLoader` is the XR-Management loader/subsystem, `DisplayXRProvider` is the app-facing facade, `DisplayXRProviderDriver` runs the per-frame pump. `DisplayXRCamera.cs` and `DisplayXRDisplay.cs` are the two stereo rig modes; `DisplayXRRigManager.cs` coordinates multi-camera scenes.
2. **Editor (C#)** — Custom inspectors and the settings/XR-Management page. (No standalone preview system — Play Mode runs the provider directly.)
3. **Native (C/C++)** — the display provider (`native~/displayxr_xrprovider/`): opens the OpenXR session on **Unity's D3D12 device**, creates an `arraySize=2` swapchain (SPI or MultiPass), chains the runtime's view-rig descriptor (`XR_EXT_view_rig`) onto `xrLocateViews` and consumes render-ready `XrView{pose, fov}` — the runtime owns the Kooima math — and submits via `xrEndFrame`. The win32 overlay, wsui composition layer, and Local2D paths survive alongside it; thread-safe shared state. The SA render-to-atlas core (`displayxr_standalone*`) is kept on disk but **not compiled** (dormant), reserved as the seed for a future many-view "quilt" render path (see `docs~/adr/ADR-007-render-path-by-view-count.md`).

### Key Features

- **Two stereo rig modes**: Camera-centric (`DisplayXRCamera` — inherits camera FOV, inv. convergence distance tunable) and display-centric (`DisplayXRDisplay` — physical display geometry, virtual display height, scale-as-zoom)
- **Multi-camera support**: Multiple rigs coexist in one scene; `DisplayXRRigManager` coordinates which rig is active (see below)
- **Play Mode == built app**: the `IUnityXRDisplay` provider runs identically in-editor and in a built player. Pressing Play *is* the preview — there is no separate preview window.
- **2D UI overlay**: Canvas → `XrCompositionLayerWindowSpaceEXT` with stereo disparity
- **Runtime-owned Kooima math (`XR_EXT_view_rig`, #396 W7)**: the plugin no longer computes Kooima. It chains an `XrDisplayRigEXT`/`XrCameraRigEXT` descriptor (the handful of tunables) onto `xrLocateViews` and consumes render-ready `XrView{pose, fov}` in the provider's per-frame pump (Play Mode and built player alike). **This requires a runtime that advertises `XR_EXT_view_rig` (SPEC_VERSION 2)**; against an older runtime the plugin emits a one-shot WARN and passes raw views through (no stereo). The former vendored/`displayxr::math` display3d/camera3d math is gone (do not re-add — see the `no-vendored-math` drift guard).

### Render Pipeline Support (BiRP / URP / HDRP)

All three pipelines render from the **same source of truth**: the provider hands Unity a **full
per-eye projection matrix** (`kUnityXRProjectionTypeMatrix`) built from the runtime's render-ready
`XrView.fov` — see `dxr_prov_build_projection` in `native~/displayxr_xrprovider/`. Because the
off-center (asymmetric) Kooima frustum is carried in the matrix itself, **BiRP, URP, and HDRP all
consume it correctly with no per-pipeline projection fix.** (Historically the provider handed
half-angle FOVs, which URP re-derived into a projection via a builder that mangled strongly
off-center frustums — head x<0 shift/deform, Unity #1328435. Handing the full matrix instead
eliminates that path, so the old **`KooimaProjectionFixFeature`** URP RendererFeature is gone.)

The rig also still calls `Camera.SetStereoProjectionMatrix(leftProj/rightProj)` on BiRP (BiRP honors
it; harmless/ignored on URP/HDRP), and exposes the per-eye matrices via `DisplayXRProvider` for any
app that wants them.

The **only** pipeline-specific piece left is the opt-in URP foreground clip, which lives in a
**URP-guarded sub-assembly** (`Runtime/URP/`, `Editor/URP/` — asmdefs gated by
`defineConstraints: ["DISPLAYXR_URP"]` + a `versionDefines` that defines `DISPLAYXR_URP` only for
`com.unity.render-pipelines.universal >= 17.0.0`, so BiRP-only and older-URP projects never compile it):

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
- **HDRP** needs no off-axis fix and no clip feature — it consumes the provider's projection matrix
  natively (hardware-verified, #22). The BiRP on-camera per-eye foreground clip (`OnRenderImage`)
  covers BiRP; HDRP transparent-overlay foreground clipping is not yet wired (no HDRP equivalent of
  the URP full-screen pass ships today).

### Multi-Camera Rig Management

Scenes can contain multiple cameras with different rig types (display-centric, camera-centric, or plain cameras). A static registry coordinates which rig is active at any time.

**`DisplayXRRigManager`** (static class, no scene object needed):
- Rigs self-register in `OnEnable`, self-deregister in `OnDisable`
- First registered camera is auto-elected as `ActiveCamera`
- `CycleNext()` advances to the next registered camera (used by Tab key)
- `ActiveCamera` property is the single source of truth for rig gating and input

**Rig gating**: `DisplayXRDisplay.LateUpdate()` and `DisplayXRCamera.LateUpdate()` check `DisplayXRRigManager.ActiveCamera` before pushing tunables to the native provider. Only the active rig pushes — prevents multi-rig conflicts (wrong projection, FOV feedback loops).

**Input isolation**: `DisplayXRInputController.IsActiveCamera()` returns true only for the active camera's controller. Inactive controllers clear their drag state to prevent rotation jumps on reactivation.

**Component reference:**

| Component | Required | Purpose |
|-----------|----------|---------|
| `DisplayXRDisplay` | One of | Display-centric stereo rig (scale-as-zoom) |
| `DisplayXRCamera` | One of | Camera-centric stereo rig (FOV-based) |
| `DisplayXRRigManager` | Automatic | Static camera registry — no scene object, rigs self-register |
| `DisplayXRInputController` | Optional | Sample WASD/mouse/scroll controller. Tab cycles cameras via `DisplayXRRigManager.CycleNext()`. Developers typically replace this with their own input. |

### OpenXR path: the display provider

The plugin drives OpenXR through the custom `IUnityXRDisplay` provider (`native~/displayxr_xrprovider/`) — it is the **sole** backend. The provider opens the session on Unity's D3D12 device, drives an `arraySize=2` swapchain (SPI/MultiPass), chains `XR_EXT_view_rig` onto `xrLocateViews` for runtime-owned Kooima, downgrades sRGB→UNORM swapchains in Gamma-space projects (so output isn't double-gamma-encoded), and submits overlay/wsui composition layers via `xrEndFrame`.

> **The legacy OpenXR API-layer hook (`DisplayXRFeature` + `displayxr_hooks.cpp`) and the standalone (SA) editor-preview session/window were hard-removed in #166.** The provider replaced them. See `docs~/architecture/xr-display-provider.md`. Renamed native headers: `displayxr_hooks.h`→`displayxr_exports.h`, `displayxr_hooks_internal.h`→`displayxr_backend.h`; re-homed glue lives in `displayxr_native_shared.cpp`.

### Wire Protocol

Extension struct definitions in `native~/displayxr_extensions.h` must match the runtime's implementation. These are versioned by extension spec version. When changing extensions, update the runtime first, then the plugin.

## Development Workflow

### Testing in Unity

1. Open any Unity 2022.3+ project
2. Add this package via Package Manager (local path or git URL)
3. Enable **DisplayXR** in Project Settings > XR Plug-in Management (Standalone tab) — it's the `IUnityXRDisplay` provider toggle, **not** under OpenXR
4. Set `XR_RUNTIME_JSON` environment variable to point to a DisplayXR runtime build (or use `SIM_DISPLAY_ENABLE=1 SIM_DISPLAY_OUTPUT=sbs` for testing without hardware)
5. **Press Play** — Play Mode runs the provider and *is* the preview (identical to a built player); there is no separate preview window

### No dependency on Unity's OpenXR package (the provider drives OpenXR itself)

The plugin has **no dependency on `com.unity.xr.openxr`** — it was dropped once the last
consumer (`DisplayXRLocal2D`'s `OpenXRRuntime.IsExtensionEnabled` gate, always false in
provider mode) was provider-ized. The provider is a custom **`IUnityXRDisplay`** subsystem
that drives the DisplayXR runtime directly (its own OpenXR loader/instance in native) and
**does not enable Unity's OpenXR loader**. So the old hook-era caveat (pre-1.16.1 OpenXR
silently ignoring `XR_RUNTIME_JSON` in editor play mode and falling back to the Mock Runtime
→ `0x0` resolution) does not apply. Enable **DisplayXR** (not OpenXR) in XR Plug-in
Management. The package's only XR dependency is `com.unity.xr.management`.

### Resolved: window-drag phase-snap (#61)

**Window-drag phase-snap is solved** (issue #61, closed 2026-05-09, hardware-verified on both paths). The SR SDK weaver only phase-snaps to lenticular-aligned positions when its WndProc subclass sees the OS in-drag flag. Rather than hand the OS the modal drag loop (which our `WS_POPUP` + cloaked + `WS_EX_NOREDIRECTIONBITMAP`/`WS_EX_NOACTIVATE` overlay can't do — synthesized `WM_NCLBUTTONDOWN`/`SC_MOVE` need a DWM redirection surface), we keep the capture-based custom drag and **bracket it** by synthesizing `WM_ENTERSIZEMOVE`/`WM_EXITSIZEMOVE` via `SendMessageW` around the drag. The weaver's subclass sees the bracket and phase-snaps; Kooima stays live throughout (no stutter, no update-only-on-release). Implemented in `displayxr_win32.c` `overlay_wnd_proc` (transparent overlay right-drag, #57). (The former `displayxr_standalone.cpp` `sa_wndproc` preview-window path is dormant/uncompiled after the #166 provider migration.) The runtime "external drag" API (`DisplayXR/displayxr-runtime#193`) is no longer needed for this.

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

- Understand the display provider → `docs~/architecture/xr-display-provider.md`
- Understand stereo math → `docs~/architecture/kooima-pipeline.md`
- Transparent-overlay click-through mask (mask/weave rect-alignment invariant + debugging pitfalls) → `docs~/architecture/click-through-mask.md`
- Why render-path by view count (provider ≤8 vs quilt >8)? → `docs~/adr/ADR-007-render-path-by-view-count.md`
- Why window-relative Kooima? → `docs~/adr/ADR-006-window-relative-kooima.md`
- Why two rig modes? → `docs~/adr/ADR-004-camera-vs-display-mode.md`
- Why deferred destruction? → `docs~/adr/ADR-001-deferred-destruction.md`
- ADR-002 (dual-session) and ADR-003 (native preview window) are **superseded** by the #166 provider migration — historical only.
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
| [`DisplayXR/displayxr-unity-test-transparent`](https://github.com/DisplayXR/displayxr-unity-test-transparent) | Transparent overlay + click-through (#57 family, alpha-native), now **URP + `XR_EXT_display_zones` / Local2D bubble** in a floating window | Tiger FBX clickable, foreground-only render. `main` = URP/zones (v2.0.0+); the Built-in (BiRP) baseline lives on the `legacy-birp` branch |
| [`DisplayXR/displayxr-unity-test-2d-ui`](https://github.com/DisplayXR/displayxr-unity-test-2d-ui) | 2D UI window-space composition layer | Tuning panel built from `DisplayXRWindowSpaceUI` |

All three pin the plugin via `https://github.com/DisplayXR/displayxr-unity.git#upm` (floating; tracks latest release).

Each test repo also has its own `CLAUDE.md` describing its scene, scripts, and which plugin features it exercises — designed so an agent can work in the test repo without loading the plugin's context.

**Test-repo releases ship as NSIS installers, not zips** (#108). Each test repo's `installer/` dir (`.nsi` + `build-installer.bat`) mirrors the [`displayxr-demo-gaussiansplat`](https://github.com/DisplayXR/displayxr-demo-gaussiansplat) pattern: hard-prereqs the runtime, installs the Unity Player under `Program Files\DisplayXR\Unity\<Variant>\`, and drops a registered-mode `.displayxr.json` manifest under `%ProgramData%\DisplayXR\apps\` (renamed `icon_unity_test*.png` per variant) so the Shell launcher discovers it as a tile. Build flow is manual today (build Player → `installer\build-installer.bat` → `gh release create`); CI automation is blocked on Unity license activation.

#### Where to launch Claude Code when working on the test repos

- **Test-only work** (tweak a scene, polish a test-repo script, fix a test-repo bug) → launch from the test repo directly. Its `CLAUDE.md` auto-loads with focused context, git ops target the right repo by default, smaller context is cheaper and faster. The plugin's installed source is still readable at `Library/PackageCache/com.displayxr.unity@<hash>/` if a grep into plugin internals is needed.
- **Plugin work that also touches a test repo** (new plugin API + test repo update to consume it) → launch from `displayxr-unity`. The plugin is the primary surface; the test repos are reachable via adjacent `../displayxr-unity-test*` paths. Land the plugin change first, let CI publish `#upm`, then update the test repo.
