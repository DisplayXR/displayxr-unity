# Transparent Avatar Sample (Windows only)

Chroma-key transparent overlay for desktop avatar use cases. The capsule
floats above the Windows desktop with click-through outside its silhouette.

Issue: [#57 — Add transparent overlay mode for desktop avatar use case](https://github.com/DisplayXR/displayxr-unity/issues/57)

## Why chroma key (not alpha)?

The Leia weaver writes opaque RGB only and the DisplayXR D3D11 compositor uses
`DXGI_ALPHA_MODE_IGNORE`, so per-pixel alpha doesn't survive end-to-end.
Workaround: render a magic color in transparent regions of *both* eye views.
Because `L == R` per sub-pixel in those regions, the weaver passes the magic
color through unchanged. Then `WS_EX_LAYERED + LWA_COLORKEY` on the top-level
HWND tells DWM to punch those pixels through to the desktop and route mouse
input below.

This sample uses **near-mid-gray `RGB(128, 127, 129)`** instead of the more
conventional magenta. Both work as keys; gray makes the silhouette-edge halo
(anti-aliased pixels that partially blend the key color) blend invisibly into
typical desktop backgrounds, while magenta produces a visible pink fringe.
Trade-off: avatar materials must avoid `(128, 127, 129)` ±1 — any pixel that
happens to land exactly on the key after weaving will go transparent. Pure
magenta is unlikely to appear in real content, gray is not, so be aware of
the avatar palette.

Full chroma-key rationale, math, and ACT (anti-crosstalk) interactions:
`displayxr-runtime:docs/reference/chroma-key-transparent-overlay.md`.

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
4. Run on a Leia SR machine. The capsule appears above the desktop; the gray
   chroma key is invisible (DWM punches it).

## Verification checklist

- Capsule renders with no rectangular background — the chroma key is gone.
- Clicks on the chroma-key region land on the underlying app (taskbar, browser).
- Clicks on the capsule reach Unity (`OnMouseDown` fires).
- Capsule pops convincingly in stereo. Transparent regions stay clean
  (no shimmer, `L == R`).
- No speckle holes inside the capsule (would mean avatar pixels are landing
  exactly on `(128, 127, 129)` — adjust palette or tweak the chroma color).

## Known limitations (v1)

- Rectangular hit-test (bounding box) only. Per-pixel alpha-mask hit-testing
  is a future upgrade.
- Silhouette edges may show a faint chroma halo. With the gray key it's
  usually invisible; with a saturated key (e.g. magenta) you'd want a
  feathered border shader or `SetWindowRgn` for a hard polygonal cutout.
- Don't include the chroma-key color in the avatar's palette — clamp shader
  outputs near it. With a gray key the constraint is meaningful (gray is
  common in lit 3D scenes); with magenta it was nearly free.
- Fullscreen-exclusive games hide topmost layered windows. Document, don't fix.
- Windows only for v1. macOS equivalent (`NSWindow.isOpaque = NO` +
  `CAColorMatrixFilter`) is a TODO.
