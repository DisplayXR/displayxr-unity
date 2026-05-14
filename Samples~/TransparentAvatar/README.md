# Transparent Avatar Sample

Alpha-native transparent overlay for desktop avatar use cases. The capsule
floats above the desktop with click-through outside its silhouette. Works
on Windows and macOS standalone builds.

Issue: [#57 — Add transparent overlay mode for desktop avatar use case](https://github.com/DisplayXR/displayxr-unity/issues/57)

## How transparency works

The OpenXR session is opted into `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND`,
so Unity emits per-pixel alpha into the swapchain. The DisplayXR runtime
captures the desktop content under each tile pre-weave, composes it under
the atlas RGBA, then alpha-gates post-weave so anti-aliased silhouettes
carry true soft alpha. The OS composition layer (DComp on Windows via a
top-level `WS_EX_NOREDIRECTIONBITMAP` overlay HWND; CAMetalLayer on macOS
via Unity's `setOpaque:NO` NSWindow) does the final blend against the
desktop.

No chroma color, no `LWA_COLORKEY`, no chroma-key fringing on silhouette
edges. See the longer-form walk-through in
`Samples~/MinimalTransparent/README.md` (the layer-ownership map and the
three-mechanism breakdown).

## What's included

- `TransparentAvatarSetup.cs` — programmatic scene setup. Creates a
  capsule avatar with a subtle breathing animation, a `DisplayXRDisplay`
  rig, and a `DisplayXRTransparentOverlay` on the Main Camera.

## Quick start

1. Import this sample via *Package Manager > DisplayXR > Samples >
   Transparent Avatar*.
2. Create an empty scene, add an empty GameObject, attach
   `TransparentAvatarSetup`.
3. **Build a standalone player** (Windows or macOS) — the native window
   restyling path only runs in a build, not in the editor preview.
4. Run. The capsule appears above the desktop; transparent regions blend
   the desktop through with true per-pixel alpha.

## Verification checklist

- Capsule renders with no rectangular background — the surround is
  truly transparent (you can see the desktop / other windows through).
- Clicks on the transparent region land on the underlying app (taskbar,
  browser, etc.).
- Clicks on the capsule reach Unity (`onPointerClick` fires).
- Capsule pops convincingly in stereo. Anti-aliased silhouette edges
  are clean — no chroma fringe, no hard-mask jaggies.

## Mouse-wheel handling (v1.2.2+)

The plugin previously resized the overlay window on scroll-wheel events
as a quick test. That was removed in v1.2.2. To use the wheel for
anything, poll `overlay.ConsumeWheelDelta()` each frame — the plugin
still consumes the raw `WM_MOUSEWHEEL` (so it doesn't bubble to the
underlying app when the overlay is foreground) and accumulates the delta
for you to read. Common pattern: drive a `DisplayXRDisplay` rig's
`virtualDisplayHeight` to get a zoom-in-window effect (smaller vHeight
= more zoom) without changing the window size. See
`displayxr-unity-test-transparent` for a working example.

## Plugin / runtime requirements

- Plugin **v1.6.0+** (`DisplayXRTransparentOverlay` no longer paints a
  chroma color; the `RequestChromaKey` / `chromaKeyColor` API was
  removed in v1.6.0 — `DisplayXR/displayxr-unity#103`).
- DisplayXR runtime that advertises `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND`
  on the Windows D3D11/D3D12 service compositor and implements the
  compose-under-bg + alpha-gate DP path. Older runtimes fail
  `xrEndFrame` validation (same signature as the v1.5.6 → v1.5.12
  regression: Player.log "is not supported for current Runtime").

## Limitations

- Click-through forwarding is HWND-level. Fullscreen-exclusive games
  below the overlay hide all topmost windows — that's a Windows
  compositor behavior, not a plugin issue.
- On Vulkan, transparency depends on the ICD exposing a non-OPAQUE
  `compositeAlpha`. Most Win32 ICDs only expose OPAQUE — alpha is
  dropped at WSI present in that case. D3D11 / D3D12 / Metal work on
  all GPUs.
- On OpenGL Win32, transparency requires `WGL_NV_DX_interop2` (NVIDIA /
  AMD). Intel iGPUs fall back to opaque presentation.
