---
status: Accepted
date: 2025-04-01
---

# ADR-004: Two Stereo Rig Modes (Camera-Centric vs Display-Centric)

## Context

Kooima asymmetric frustum projection requires knowing the relationship between the viewer's eyes and the display surface. There are two natural ways to set this up in a 3D engine, depending on the app's camera model.

## Decision

Two rig components, each with its own Kooima variant:

### Display-centric (`DisplayXRDisplay`)
- **Concept:** The camera's parent transform represents the physical display's pose in the scene
- **FOV:** Determined by physical display geometry (atan of screen edge / eye distance)
- **Zoom:** Via `virtual_display_height` — scales the display, changing the screen-edge angles
- **Library:** `display3d_view.c` — arctan-based Kooima from physical dimensions
- **Use case:** The display is a "window into the scene" at a fixed location

### Camera-centric (`DisplayXRCamera`)
- **Concept:** The camera transform represents the viewer's pose; FOV is a camera parameter
- **FOV:** From Unity's camera FOV (half-tangent of vertical FOV)
- **Zoom:** Via convergence distance (`inv_convergence_distance`) — virtual screen plane distance
- **Library:** `camera3d_view.c` — tangent-half-angle Kooima, handles infinite convergence (parallel projection)
- **Use case:** First-person or free-camera apps where the display moves with the viewer

### Key difference
Display-centric computes FOV from geometry (screen size / eye distance). Camera-centric takes FOV as input and derives a virtual screen at the convergence distance. For finite convergence, camera-centric is equivalent to computing a virtual display at that distance and applying display-centric.

## Consequences

- Each component has distinct tunables (virtual_display_height vs inv_convergence_distance)
- The native hook chain selects the library based on the `camera_centric` flag in tunables
- Only the active rig (via `DisplayXRRigManager`) pushes tunables — prevents conflicts
- Camera-centric must cache the camera FOV *before* the XR subsystem overwrites it each frame
- Both modes share the same scene transform pipeline (position, orientation, scale)
