# Standalone Preview Session

The standalone (SA) preview session is the primary editor workflow. It creates an independent OpenXR session that bypasses Unity's XR loader, giving full control over the session lifecycle without Play Mode.

## Session Lifecycle

```
Start()
  ├── Load runtime via xrNegotiateLoaderRuntimeInterface
  ├── xrCreateInstance (with display_info, metal/d3d12, window binding extensions)
  ├── xrGetSystem → xrGetSystemProperties (display info + create preview window)
  ├── xrCreateSession (pass plugin-owned NSView/HWND + graphics binding)
  ├── xrCreateReferenceSpace (LOCAL)
  ├── Enumerate rendering modes
  ├── Create atlas swapchain (worst-case size across all modes)
  ├── Create render rig (hidden cameras + atlas RenderTexture)
  └── Start polling (EditorApplication.update → FrameTick)

FrameTick() [each editor update]
  ├── poll_events (pumps macOS events + OpenXR session state)
  ├── Check deferred stop (re-entrancy guard)
  ├── Check window closed (user closed preview window)
  ├── begin_frame (xrWaitFrame + xrBeginFrame + update canvas from window rect)
  ├── Push rig parameters (tunables + display pose from scene camera)
  ├── Refresh mode info (may change via rendering mode switch)
  ├── Compute Kooima views (N views for current mode)
  ├── Render each eye to atlas tile (Camera.Render with Kooima matrices)
  ├── [Windows] Copy atlas RT → bridge texture (cross-device)
  ├── Submit atlas to swapchain (Metal blit or D3D12 copy)
  └── Refresh eye positions for UI

Stop()
  ├── Stop polling
  ├── Clear GameViewOverlay atlas reference
  ├── Destroy render rig + atlas RT
  ├── xrRequestExitSession → drain events → xrEndSession
  ├── xrDestroySession
  ├── Destroy preview window
  └── xrDestroyInstance
```

## Play Mode Integration

When the user presses Play with the preview running:

1. **ExitingEditMode:** Disable Unity's XR loader (`TryRemoveLoader`), save `PlayModeStartedKey = IsRunning`
2. **Domain reload:** `OnBeforeAssemblyReload` → `Stop()` (statics wiped, native session stopped)
3. **EnteredPlayMode:** Read `PlayModeStartedKey` from SessionState → `Start()` → `RestoreSelection()`
4. **Camera restore:** Try EntityId (fails after domain reload) → try camera name → fallback to first rig
5. **ExitingPlayMode:** `Stop()`, clear flag
6. **EnteredEditMode:** Re-enable XR loader (`TryAddLoader`), focus Scene View

## Re-Entrancy Guards

`poll_events()` pumps the macOS event loop, which can trigger Unity UI callbacks (e.g., user clicks Play button → `ExitPlayMode` → `Stop()`). If `Stop()` destroys the session while `poll_events` is still on the call stack → use-after-free.

Solution: `s_InPollEvents` flag. If `Stop()` is called while inside `poll_events`, it sets `s_DeferredStop = true` and returns immediately. `FrameTick` checks this flag after `poll_events` returns and calls `DoStop()`.

## Preview Window

See [ADR-003](../adr/ADR-003-native-preview-window.md) for the design decision.

**Key files:**
- `Editor/DisplayXRPreviewSession.cs` — C# session management
- `Editor/DisplayXRPreviewWindow.cs` — EditorWindow (tile debug view)
- `Runtime/DisplayXRGameViewOverlay.cs` — Game View tile display during Play Mode
- `native~/displayxr_standalone.cpp` — Native session implementation
- `native~/displayxr_standalone_metal.m` — macOS window + Metal blit
