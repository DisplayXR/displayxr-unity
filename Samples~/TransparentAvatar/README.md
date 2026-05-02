# Transparent Avatar Sample (Windows only)

Chroma-key transparent overlay for desktop avatar use cases. The capsule
floats above the Windows desktop with click-through outside its silhouette.

Issue: [#57 — Add transparent overlay mode for desktop avatar use case](https://github.com/DisplayXR/displayxr-unity/issues/57)

## Why chroma key (not alpha)?

The Leia weaver writes opaque RGB only and the DisplayXR D3D11 compositor uses
`DXGI_ALPHA_MODE_IGNORE`, so per-pixel alpha doesn't survive end-to-end.
Workaround: render a magic color (default magenta `RGB(255, 0, 255)`) in
transparent regions of *both* eye views. Because `L == R` per sub-pixel in
those regions, the weaver passes the magic color through unchanged. Then
`WS_EX_LAYERED + LWA_COLORKEY` on the top-level HWND tells DWM to punch those
pixels through to the desktop and route mouse input below.

Full chroma-key rationale, math, and ACT (anti-crosstalk) interactions:
`displayxr-runtime-pvt:docs/reference/chroma-key-transparent-overlay.md`.

## What's included

- `TransparentAvatarSetup.cs` — programmatic scene setup. Creates a capsule
  avatar with a subtle breathing animation, a `DisplayXRDisplay` rig, and a
  `DisplayXRTransparentOverlay` on the Main Camera.

## Quick start

1. Import this sample via *Package Manager > DisplayXR > Samples > Transparent
   Avatar (Windows)*.
2. Create an empty scene, add an empty GameObject, attach `TransparentAvatarSetup`.
3. **Build a Windows standalone player** — the layered-window path only runs
   in a build, not in the editor preview.
4. Run on a Leia SR machine. The capsule appears above the desktop; magenta is
   invisible (DWM punches it).

## Verification checklist

- Capsule renders with no rectangular background — magenta is gone.
- Clicks on magenta land on the underlying app (taskbar, browser).
- Clicks on the capsule reach Unity (`OnMouseDown` fires).
- Capsule pops convincingly in stereo. Transparent regions stay clean
  (no shimmer, `L == R`).

## Known limitations (v1)

- Rectangular hit-test (bounding box) only. Per-pixel alpha-mask hit-testing
  is a future upgrade.
- Silhouette edges may show a chroma halo. Mitigation: feathered magenta
  border in shader, or fall back to `SetWindowRgn` for a hard polygonal cutout.
- Don't include the chroma-key color in the avatar's palette — clamp shader
  outputs near it.
- Fullscreen-exclusive games hide topmost layered windows. Document, don't fix.
- Windows only for v1. macOS equivalent (`NSWindow.isOpaque = NO` +
  `CAColorMatrixFilter`) is a TODO.
