# URP Basic Scene Sample

Smoke test that the DisplayXR stereo rig works on the **Universal Render
Pipeline (URP)**. Same scene content as the Basic Scene sample, but assumes
the host project is configured for URP.

This sample exists primarily to verify the SRP camera-callback path
(`RenderPipelineManager.beginCameraRendering`) — URP and HDRP do not fire
`Camera.onPreRender`, so without that path stereo rendering would silently
fall back to mono.

## What's Included

- `URPBasicSceneSetup.cs` — Spawns colored cubes at varying depths using
  the URP/Lit shader. Attach to any GameObject in a fresh scene.
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
3. Open a new empty scene; add a Main Camera with `DisplayXRCamera` (or
   `DisplayXRDisplay`) attached.
4. Add an empty GameObject and attach `URPBasicSceneSetup`.
5. Set `XR_RUNTIME_JSON` (or use `SIM_DISPLAY_ENABLE=1 SIM_DISPLAY_OUTPUT=sbs`).
6. Enter Play Mode — left/right eye matrices should differ (visible parallax
   on the cubes); toggle `logEyeTracking` on the rig to confirm the per-frame
   stereo callback is firing.

## Off-axis projection fix (URP 17 / Unity 6)

URP ignores `Camera.SetStereoProjectionMatrix` (Unity #1328435) and builds each
eye's projection from `views[i].fov`, which it mishandles for strongly off-center
(window-relative) Kooima frustums — the image is correct on-axis but shifts and
deforms when the head moves far off-center. The plugin fixes this with a
`ScriptableRendererFeature` (`KooimaProjectionFixFeature`) that re-pushes the
runtime's correct per-eye projection. It ships in a **URP-guarded sub-assembly**
(URP ≥ 17.0.0 only) and is **auto-wired** into your URP renderer asset when a
DisplayXR rig is present in an open scene.

- If it isn't applied automatically, run **DisplayXR > Setup URP Projection Fix**.
- To stop the auto-wiring, toggle **DisplayXR > Auto-Wire URP Projection Fix**.
- The fix requires **URP 17 / Unity 6** (RenderGraph). On older URP the assembly
  is excluded and off-center Kooima remains a known limitation.

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
.beginCameraRendering`), so the rig should work without code changes. The URP
off-axis projection fix above is URP-only (the RendererFeature targets URP); HDRP
end-to-end testing is not bundled here — file an issue with repro steps if
you hit problems.

## Note on Dependencies

This package does **not** declare URP as a dependency. The sample only
imports when a user explicitly picks it from the Package Manager sample list,
and it expects the host project to have URP already installed. If the URP/Lit
shader isn't found at runtime, the script falls back to `Standard` and logs a
warning.
