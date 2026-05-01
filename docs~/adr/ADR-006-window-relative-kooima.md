---
status: Accepted
date: 2026-03-20
source: "DisplayXR/displayxr-unity#41, runtime ADR-012"
---

# ADR-006: Window-Relative Kooima Projection

## Context

Kooima projection computes stereo matrices from the eye position relative to the display surface. When the app window covers the entire display, eye positions are relative to the display center. But when the window is smaller or offset (e.g., editor preview, windowed mode), the eye positions must be relative to the *window* center, not the display center.

Without this adjustment, the parallax is computed as if the window were at the display center, producing incorrect perspective when the window is at a display edge. The interlacing pattern is also display-position-dependent — the weaver needs the window's screen position for pixel-precise alignment.

## Decision

Before Kooima computation, adjust eye positions from display-center to window-center coordinates:

1. Compute pixel size from display physical dimensions (`pixel_size_x = display_width_m / display_pixel_width`)
2. Compute display center in pixels
3. Compute window center from viewport position + size
4. Shift eye X/Y by `(window_center - display_center) * pixel_size` (in meters)
5. On Windows, invert Y offset (top-down convention vs Y-up)

The viewport position and size come from:
- **SA session:** Queried from the native preview window each frame (`displayxr_sa_metal_get_window_rect` / `GetClientRect`)
- **Built app:** Set via `displayxr_set_viewport_size()` from C# `Screen.width/height`

## Consequences

- Correct parallax in windowed mode and editor preview
- The runtime's weaver receives screen position via the window binding for pixel-precise interlacing
- Moving/resizing the preview window continuously updates the projection
- `displayxr_set_viewport_size()` has a "native authoritative" variant that prevents stale C# values from overwriting native window-tracking values during resize
