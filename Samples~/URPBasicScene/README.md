# URP Basic Scene Sample

Smoke test that the DisplayXR stereo rig works on the **Universal Render
Pipeline (URP)**. Same scene content as the Basic Scene sample, but assumes
the host project is configured for URP.

This sample exists primarily to verify the SRP camera-callback path
(`RenderPipelineManager.beginCameraRendering`) — URP and HDRP do not fire
`Camera.onPreRender`, so without that path stereo rendering would silently
fall back to mono.

## What's Included

- `URPBasicScene.unity` — The scene to open: a Main Camera carrying
  `DisplayXRCamera`, and a **URP Scene Setup** object carrying the script below.
- `URPBasicSceneSetup.cs` — Spawns colored cubes at varying depths using the
  URP/Lit shader. It builds them in `Start()`, so **the content exists in Play
  mode only** — a scene that looks empty when you stop is expected, not a broken
  import.
- `Editor/URPBasicSceneShaderRegistration.cs` — Editor-only. Before any
  standalone build (via `IPreprocessBuildWithReport`) and on demand via
  **Tools > DisplayXR > Register URP/Lit in Always Included Shaders**, adds
  URP/Lit to **Project Settings > Graphics > Always Included Shaders** so the
  shader ships in standalone builds. Without this, Unity's build-time shader
  stripper drops URP/Lit — the script resolves it via `Shader.Find` at runtime,
  with no static reference visible to the stripper — and the cubes render as
  the magenta error material in the built `.app`. (Issue #72.) Idempotent.

## Quick Start

1. In a Unity project that already has URP set up:
   - `com.unity.render-pipelines.universal` installed via Package Manager.
   - A URP Render Pipeline Asset assigned in Project Settings > Graphics
     (and Quality > Render Pipeline Asset).
2. Import this sample via Package Manager > DisplayXR > Samples > URP Basic Scene.
3. Open `Assets/Samples/DisplayXR/<version>/URP Basic Scene/URPBasicScene.unity`.
4. Set `XR_RUNTIME_JSON` (with no 3D panel, sim_display is the automatic fallback; `SIM_DISPLAY_OUTPUT=sbs` optionally picks the sim output format).
5. Enter Play Mode — left/right eye matrices should differ (visible parallax
   on the cubes); toggle `logEyeTracking` on the rig to confirm the per-frame
   stereo callback is firing.

To try the display-centric rig instead, swap `DisplayXRCamera` on the Main Camera
for `DisplayXRDisplay`.

To turn the runtime-built content into real, editable scene objects, select the
**Scene Setup** object and press **Create Scene Content** in the inspector: it authors
the cubes, floor and light into the scene (material assets are written to a `Materials/`
folder next to the scene), then removes the setup component. Press Play without it and
the content is still built at runtime as before.

## Transparent overlays on URP

Two extra steps for transparent-overlay apps (not needed for this opaque sample):

- **Preserve Framebuffer Alpha** — enable it in *Project Settings > Player >
  Other Settings*. The plugin can't set it at runtime; without it the URP color
  target loses its alpha channel (HDR + 32-bit → `B10G11R11`) and the overlay
  renders opaque black.
- **Per-eye foreground clip** (optional) — run **DisplayXR > Setup URP Foreground
  Clip** to create the clip material and add Unity's Full Screen Pass feature. The
  rig publishes the per-eye fars automatically when `foregroundOnlyClip` is on.

## HDRP

HDRP routes through the same SRP callback (`RenderPipelineManager
.beginCameraRendering`), so the rig works without code changes. HDRP end-to-end
testing is not bundled here; file an issue with repro steps if you hit problems.

## Note on Dependencies

This package does **not** declare URP as a dependency. The sample only
imports when a user explicitly picks it from the Package Manager sample list,
and it expects the host project to have URP already installed. If the URP/Lit
shader isn't found at runtime, the script falls back to `Standard` and logs a
warning.
