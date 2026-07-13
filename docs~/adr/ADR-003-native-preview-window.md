---
status: Superseded
date: 2026-04-07
source: "preview-own-window branch"
---

# ADR-003: Plugin-Owned Native Window Instead of IOSurface/DXGI Sharing

> **Status: Superseded (2026-07-03, #166).** The native editor preview window this ADR covers was removed in the hook-removal pass; the custom IUnityXRDisplay provider is now the sole backend. Kept for historical decision context. See [ADR-007](ADR-007-render-path-by-view-count.md) and [xr-display-provider.md](../architecture/xr-display-provider.md).

## Context

The original editor preview used a shared texture approach: the runtime composited into an IOSurface (macOS) or DXGI shared handle (Windows), and Unity displayed it in an EditorWindow or Game View overlay. This required:

- IOSurface creation + MTLTexture wrapping on macOS
- D3D12 cross-device shared texture + DXGI handle on Windows
- Canvas rect signaling (`xrSetSharedTextureOutputRectDXR`)
- UV cropping for aspect ratio mismatch
- Y-flip handling for Metal
- Complex teardown (destroy IOSurface before session, but after compositor)

The built app path worked differently: the app creates its own window, passes the HWND/NSView to the runtime, and the runtime composites directly into it.

## Decision

Replace the shared texture approach with a plugin-owned native window:

1. **macOS:** Create an `NSWindow` with `NSView` content, pass the `NSView` to `mac_binding.viewHandle`
2. **Windows:** Create an `HWND` with `WS_OVERLAPPEDWINDOW`, pass to `win_binding.windowHandle`
3. Set `sharedIOSurface = NULL` / `sharedTextureHandle = NULL` — no shared texture
4. The runtime composites directly into the plugin's window (CAMetalLayer overlay on macOS, D3D11/D3D12 on Windows)
5. The editor preview tab shows the raw eye tile atlas (debug view) instead of compositor output

This mirrors the built-app model: app creates window -> passes handle to runtime -> runtime renders into it.

### Window behavior (macOS)

- `canBecomeMainWindow: NO` — Unity keeps main window status
- `canBecomeKeyWindow: YES` — required for mouse events (Input System)
- `preservesContentDuringLiveResize: YES` — freeze content during resize (macOS blocks main thread)
- `orderFront:nil` — show without stealing keyboard focus
- `windowWillClose:` delegate detects user closing -> stops SA session

### Input isolation

- `window_is_interacting()` checks if cursor is over window frame (title bar, resize edges) vs content area
- Frame area -> suppress `DisplayXRInputController` input (prevents rotation during move/resize)
- Content area -> allow input (camera rotation via click-drag)
- `ShouldIgnoreInput()` allows input when no EditorWindow is focused but SA is running (user is in the native window)

## Consequences

- **Simpler architecture** — no IOSurface, no DXGI shared handles, no canvas rect signaling, no UV cropping for compositor output
- **WYSIWYG potential** — the preview window uses the same window binding path as built apps
- **Atlas bridge still needed on Windows** — D3D12 cross-device blit for atlas RT -> swapchain (Unity's device vs SA device)
- **Editor tab shows tiles** — raw eye atlas with grid overlay, not the final composited output
- **Window management complexity** — focus handling, input forwarding, resize behavior, close detection
- **macOS live resize limitation** — content freezes during resize (main thread blocked by AppKit), snaps to correct size after
