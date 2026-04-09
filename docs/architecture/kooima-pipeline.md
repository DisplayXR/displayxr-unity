# Kooima Projection Pipeline

Kooima asymmetric frustum projection computes per-eye stereo matrices for 3D light field displays. The projection is "off-axis" — each eye sees a different frustum based on its physical position relative to the display surface.

## Transform Chain

Each frame, the hook chain processes eye positions through this pipeline:

```
Raw eye positions (from xrLocateViews, LOCAL reference space)
    ↓
Scene transform (camera/display pose from Unity scene hierarchy)
    ↓
Window-relative adjustment (shift eyes to window center on display)
    ↓
Tunables (IPD, parallax, perspective, convergence/virtual height)
    ↓
Kooima library (display3d or camera3d)
    ↓
Stereo view + projection matrices (per eye)
    ↓
FOV angles written to xrLocateViews output (Unity applies them)
```

## Two Kooima Variants

See [ADR-004](../adr/ADR-004-camera-vs-display-mode.md) for the design decision.

### Display-centric (`display3d_view.c`)
- Input: physical display dimensions (meters), eye position, virtual display height
- Compute: screen corners relative to eye → asymmetric frustum angles via `atan`
- The `m2v` (meters-to-virtual) factor from `virtual_display_height / physical_height` provides zoom
- Anisotropic scale corrections: `ax = sy/sx`, `az = sy/sz` handle non-uniform camera scale

### Camera-centric (`camera3d_view.c`)
- Input: camera FOV (half-tangent), convergence distance, eye position
- Compute: virtual screen extents from FOV × convergence distance → tangent-half-angle shifts
- Handles `inv_convergence_distance = 0` (infinite convergence / parallel projection)
- For finite convergence, equivalent to placing a virtual display at that distance

## Tunables

| Tunable | Display-centric | Camera-centric | Effect |
|---------|----------------|----------------|--------|
| IPD Factor (0-1) | Yes | Yes | Scales inter-eye distance |
| Parallax Factor (0-1) | Yes | Yes | Scales eye X/Y offset from center |
| Perspective Factor | Yes | Yes | Scales eye Z displacement |
| Virtual Display Height | Yes | — | Override physical height (zoom) |
| Inv. Convergence Distance | — | Yes | 1/meters to virtual screen |
| FOV Override | — | Yes | Camera vertical FOV |
| Near/Far Z | Yes | Yes | Clip planes |

## Coordinate Conversions

**OpenXR → Unity view matrix:** Negate column 2 (Z-flip) to convert from right-hand -Z forward to left-hand +Z forward.

**macOS Metal:** Additionally flip projection Y (row 1) and set `GL.invertCulling = true` because Metal RenderTextures are Y-inverted.

**Windows D3D12:** No additional flips needed.

**Key files:**
- `native~/displayxr_kooima.cpp` — Scene transform + tunables application
- `native~/display3d_view.h/c` — Display-centric Kooima (arctan-based)
- `native~/camera3d_view.h/c` — Camera-centric Kooima (tangent-based)
- `native~/displayxr_hooks.cpp:hooked_xrLocateViews` — Pipeline orchestration
