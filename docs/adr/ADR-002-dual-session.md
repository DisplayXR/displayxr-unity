---
status: Accepted
date: 2025-03-01
---

# ADR-002: Dual-Session Architecture (SA Preview vs Unity XR Loader)

## Context

Unity's OpenXR loader manages the XR session lifecycle during Play Mode. It creates the session at Play entry, submits frames through `xrEndFrame`, and tears down at Play exit. The editor preview needs an independent session that runs *without* Play Mode — otherwise every preview iteration requires Play/Stop cycles.

Running two OpenXR sessions simultaneously (Unity's loader + our preview) on the same display causes resource conflicts, double compositing, and undefined behavior.

## Decision

Two mutually exclusive session paths:

1. **Standalone (SA) session** — for editor preview (Edit Mode and Play Mode in editor)
   - Loads the runtime directly via `xrNegotiateLoaderRuntimeInterface`
   - Manages its own instance, session, swapchain, and frame loop
   - Creates a plugin-owned native window (NSWindow on macOS, HWND on Windows)
   - Renders via hidden camera rig + atlas RenderTexture → swapchain blit

2. **Unity XR loader** — for built applications only
   - Uses Unity's standard OpenXR pipeline
   - `DisplayXRFeature` hooks inject Kooima matrices via `xrLocateViews`
   - Runtime auto-detects the app's window and creates an overlay

**Conflict prevention:** During editor Play Mode, the Unity XR loader is disabled (`TryRemoveLoader`) before entering Play. The SA session runs instead. The loader is restored when returning to Edit Mode.

## Consequences

- **Editor preview works without Play Mode** — the primary workflow
- **Play Mode in editor uses the same SA path** — consistent behavior between Edit and Play
- **Built apps use a different rendering path** (Unity XR loader) — testing parity requires actual builds
- **SessionState flags** persist across domain reload to track XR loader enable/disable state
- The `DisplayXRFeature` lifecycle hooks (OnInstanceCreate, OnSessionCreate, etc.) only fire for built apps, not for editor preview
- Future: revisiting the Unity XR loader for editor Play Mode requires solving the Game View `RenderPlayModeViewCamerasInternal` teardown crash (see ADR-001)
