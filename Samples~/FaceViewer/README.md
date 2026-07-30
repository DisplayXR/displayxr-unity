# Face Viewer (Billboard)

Rotates a GameObject so it always faces the tracked viewer's head — a billboard effect driven by the **real eye-tracked viewer position**, not just the camera transform. As the user moves left/right in front of the display, the object turns to follow them.

## How to use

1. Package Manager → DisplayXR → Samples → "Face Viewer (Billboard)" → **Import**.
2. The script lands at `Assets/Samples/DisplayXR/<version>/Face Viewer (Billboard)/FaceViewer.cs`.
3. Attach `FaceViewer` to any world-space object (label quad, avatar, floating panel).
4. Press Play on a tracking display — the object follows your head.

## Inspector fields

- `yawOnly` — rotate only around world Y (upright billboard). Enable for labels and standing avatars; leave off for full 3-axis facing.
- `turnSpeed` — degrees per second toward the target orientation; `0` snaps instantly.
- `forwardAwayFromViewer` — when true (default) the object's +Z points *away* from the viewer, which is correct for Unity UI, TextMesh, and Quad primitives (their visible face looks down −Z). Disable to point +Z at the viewer instead.

## How it works

The viewer's head is the midpoint of the two eye positions the provider is rendering with, read straight from the plugin:

```csharp
if (DisplayXRProvider.TryGetViewerHead(out Vector3 head)) { /* world-space head */ }

// or both eyes, e.g. for per-eye effects:
DisplayXRProvider.TryGetViewerEyes(out Vector3 eyeL, out Vector3 eyeR);
```

These are the exact per-eye poses the display provider renders with (runtime-owned Kooima via `XR_DXR_view_rig`), in Unity world space, so the billboard is always consistent with what the viewer actually sees. When no viewer data is available — provider not running, viewer not tracked yet, 2D mode — both calls return false and the component falls back to the camera's transform position, so behavior stays sensible everywhere.

`FaceViewer.TryGetViewerHead(cam, out head)` is public and static — reuse it for your own head-coupled effects (proximity triggers, lean-to-zoom, parallax UI) — though app code is better off calling `DisplayXRProvider.TryGetViewerHead` directly.

> **Prefer this over `Camera.GetStereoViewMatrix()`.** That API has been reported returning the **same matrix for both eyes** under the provider ([#236](https://github.com/DisplayXR/displayxr-unity/issues/236)) — which makes any head-coupled effect built on it silently freeze, since the two eyes never differ. The plugin itself never writes that cache (`Camera.SetStereoViewMatrix` went away with the #166 provider migration); whether Unity populates it is Unity's business and evidently varies by configuration — it *does* get populated in a built player on Windows/D3D12, and it was reported not to in the reporter's setup. `DisplayXRProvider.TryGetViewerHead` reads the provider's own per-eye data and does not depend on that behaviour either way.

## Physical-space alternative (advanced)

If you need the viewer position in **real-world meters relative to the screen or window** (independent of the rig's world mapping and tunables), the plugin exposes the raw tracker data:

- `DisplayXRNative.displayxr_get_eye_positions(...)` — raw per-eye positions from the last `xrLocateViews`: meters, origin at the physical panel center, +X right, +Y up, viewer at +Z.
- `DisplayXRProvider.TryGetDisplayInfo(out info)` — physical panel size (meters) and resolution.
- `DisplayXRNative.displayxr_get_kooima_canvas(...)` — the window's rect on the panel (panel pixels) and physical size (meters), published every frame; use it to rebase panel-center-relative eyes to window-relative.

For world-space billboarding, `DisplayXRProvider.TryGetViewerHead` above is simpler and already consistent with rendering — reach for the raw APIs only when the effect must be expressed in physical units.

## Cross-references

- `DisplayXRRigManager.ActiveCamera` (plugin Runtime) — the active rig camera the component reads from
- `DisplayXRProvider.TryGetViewerHead` / `TryGetViewerEyes` (plugin Runtime) — the world-space viewer position this component is built on
- `DisplayXRProvider.IsEyeTracked` / `EyeTrackingStateChanged` (plugin Runtime) — whether a face is currently tracked
