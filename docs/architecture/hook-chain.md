# OpenXR Hook Chain

The native plugin intercepts OpenXR function calls to inject Kooima stereo projection, display information extraction, window binding, and overlay composition. This is the rendering path for **built applications** (not the editor SA preview).

## How It Works

Unity's OpenXR Feature system calls `HookGetInstanceProcAddr` during initialization. We return a wrapper function that intercepts specific OpenXR calls:

```
Unity OpenXR Loader
    ↓
displayxr_hook_xrGetInstanceProcAddr (our wrapper)
    ├── "xrLocateViews"        → hooked_xrLocateViews (Kooima projection)
    ├── "xrGetSystemProperties" → hooked_xrGetSystemProperties (display info)
    ├── "xrCreateSession"      → hooked_xrCreateSession (window binding)
    ├── "xrEndFrame"           → hooked_xrEndFrame (overlay layers)
    ├── "xrDestroySession"     → hooked_xrDestroySession (deferred)
    ├── "xrDestroyInstance"    → hooked_xrDestroyInstance (deferred)
    ├── "xrPollEvent"          → hooked_xrPollEvent (guard post-destroy)
    └── others                 → pass through to real function
```

## Key Hooks

### xrLocateViews — Kooima Projection
1. Call the real `xrLocateViews` to get raw eye positions from the runtime
2. Apply scene transform (camera/display pose from Unity)
3. Apply window-relative adjustment (see [ADR-006](../adr/ADR-006-window-relative-kooima.md))
4. Select camera-centric or display-centric path based on tunables flag
5. Compute Kooima stereo matrices + asymmetric FOV angles
6. Cache matrices in shared state (for C# stereo override)
7. Write modified FOV + eye positions back to the views array

### xrCreateSession — Window Binding
- **Editor mode:** Create a plugin-owned preview window and pass via binding extension
- **Built app:** Auto-detect the app's main window, create overlay NSView/HWND
- Inject `XrCocoaWindowBindingCreateInfoEXT` or `XrWin32WindowBindingCreateInfoEXT` into the session creation chain

### xrEndFrame — Overlay Layers
Append window-space composition layers (for 2D UI overlay):
- Count active overlay layers from shared state
- Build `XrCompositionLayerWindowSpaceEXT` structs with fractional positioning + disparity
- Extend the layer array and call the real `xrEndFrame`

### xrDestroySession / xrDestroyInstance — Deferred Destruction
See [ADR-001](../adr/ADR-001-deferred-destruction.md).

## Thread-Safe Shared State

Game thread (C#) and render thread (native hooks) communicate via double-buffered state:

| Buffer | Writer | Reader | Pattern |
|--------|--------|--------|---------|
| Tunables | Game thread | Render thread | Atomic swap on write |
| Scene Transform | Game thread | Render thread | Atomic swap on write |
| Display Info | Render thread (once) | Game thread | Write-once |
| Eye Positions | Render thread | Game thread | Atomic swap on write |
| Stereo Matrices | Render thread | Game thread | Atomic swap on write |

No locks — single-writer, single-reader per buffer with `memory_order_relaxed` reads and `memory_order_release` writes.

**Key files:**
- `native~/displayxr_hooks.cpp` — Hook implementations
- `native~/displayxr_shared_state.h` — Double-buffered state definitions
- `native~/displayxr_kooima.cpp` — Scene transform + tunables application
- `native~/display3d_view.c` / `camera3d_view.c` — Kooima math libraries
