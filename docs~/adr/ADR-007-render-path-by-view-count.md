# ADR-007: Render-Path Selection by Display View Count

**Status:** Accepted (design; quilt path not yet implemented) — 2026-07-03

## Context

DisplayXR-Unity renders through a custom `IUnityXRDisplay` display provider (epic
#166). Unity's XR display pipeline caps at
`kUnityXRMaxNumRenderPasses (4) × kUnityXRMaxNumUnityXRRenderParams (2) = 8 views/frame`
(`native~/unity_pluginapi/IUnityXRDisplay.h`; the header explicitly lists "Quad
pass wide FOV stereo rendering (4 RenderPass × 1 RenderParams)" as a supported
config). This covers eye-tracked stereo (2 views) and quad (4 views) — the
eye-tracked light-field displays DisplayXR targets today.

It **cannot** cover many-view light-field displays (Looking Glass: 45–100+ views),
which fundamentally require rendering N camera views into an atlas/quilt and
compositing them — the model Looking Glass's own Unity plugin uses (their "quilt"
is our "atlas"). Converting Looking Glass content to DisplayXR is an active target,
so this ceiling is a real constraint, not a hypothetical.

## Decision

The plugin selects a render path from the display's advertised maximum view count
(from `XR_EXT_display_info` / the enumerated modes' view counts, already read at
subsystem start):

- **≤ 8 views — the provider path** (Unity `IUnityXRDisplay`) — eye-tracked stereo
  + quad. Full Unity integration; this is the shipping path (v1.24.x → v2.0.0).
- **> 8 views — the quilt/atlas path** — the app renders N cameras into an atlas
  handed to the runtime, bypassing Unity's XR display cap. **Not yet implemented.**

The **selector** is the advertised max view count. The app-facing API
(`DisplayXRDisplay`/`DisplayXRCamera` rigs, tunables, transparency, zones,
`DisplayXRModeSwitch`) **must stay path-agnostic** — the render path is an internal
implementation detail keyed off view count, never something apps branch on.

## The preserved quilt seed

The standalone (SA) session's `RenderEyeToAtlas` manual-camera-loop → atlas is
exactly the quilt technique (render N cameras to tiles, submit an atlas). SA was an
editor-preview-only OpenXR session; the #166 hook-removal pass removed the SA
*editor-preview UI* (Preview window/session — Play Mode replaces it) but **kept the
`displayxr_standalone*` source in-tree, out of the CMake build** (dormant), banner-
marked as the quilt seed. Whoever promotes it must:

1. Re-home the hook helpers it still references (`displayxr_get_hooked_backend` /
   `displayxr_get_hooked_session`, `displayxr_hooked_*` rendering-mode wrappers) —
   they were removed with `displayxr_hooks.cpp` in the same pass.
2. Make it a **built-app, scene-integrated** renderer. SA is the *reference*, not
   the implementation — it was editor-only.

## Consequences

- One plugin spans two display classes (few-view eye-tracked + many-view light
  field) behind a single, path-agnostic app API.
- The main design cost is that **unified path-agnostic API**, not the raw
  rendering: every feature (rigs, tunables, transparency, zones, foreground clip,
  mode-switch) must behave identically whether Unity renders the app's cameras
  (provider path) or the app renders N cameras into an atlas (quilt path).
- Until the quilt path exists, a display advertising > 8 views is detected at
  subsystem start and emits a one-shot WARN (the provider renders up to 8 views).
- **Near-term, separate from the quilt path:** extending the *provider* from stereo
  to quad (≤ 8 views) is the easy win — generalize the existing multi-zone
  N-render-pass machinery in `displayxr_display_provider.cpp` `PopulateNextFrameDesc`
  from "N zones each 2-view" to "1 display, N views" (arraySize=N swapchain, submit
  views `imageArrayIndex` 0..N-1). It needs the runtime `XR_EXT_view_rig` to return
  N views + an N-view DP weave. The multi-zone code is the proof-of-concept.

## References

- Epic #166 (custom `IUnityXRDisplay` provider replacing the OpenXR hook).
- `docs~/architecture/xr-display-provider.md` — the shipping render path.
- Looking Glass's quilt model — the concrete driver for the > 8-view path.
