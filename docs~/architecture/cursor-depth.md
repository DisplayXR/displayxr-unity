# Depth-aware cursor (`XR_DXR_cursor_depth`, runtime ADR-046)

The OS cursor sits on the glass. Over content that pops out of the display it is drawn on
top of pixels whose disparity says they are in front of it, so the eyes get two depth cues
that disagree. The DisplayXR runtime fixes this by drawing the cursor itself, in stereo, just
in front of the nearest content under it. This page covers how the Unity plug-in uses that.

Spec and policy live in the runtime:
[ADR-046](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/adr/ADR-046-depth-aware-cursor.md)
(Amendment 2: one policy, the runtime draws; Amendment 3: measured disparity) and
[`XR_DXR_cursor_depth.md`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/extensions/XR_DXR_cursor_depth.md).

## Using it

```csharp
DisplayXRCursorDepth.Enabled = true;                                  // opt in (default off)
DisplayXRCursorDepth.Source  = DisplayXRCursorDepth.DepthSource.Measured; // the default
```

Or add a **DisplayXR Depth Cursor** component (`DisplayXRDepthCursor`) to any GameObject.
It enables the cursor while the component is enabled and exposes the same settings.

| Source | What supplies the depth | Needs |
|---|---|---|
| `Measured` (default) | The runtime block-matches a cursor-sized patch between the two views Unity rendered. No colliders, no code. Low-texture areas (sky, a flat wall, a flat-shaded face) are "not confident" and read as on the glass, i.e. the plain OS cursor. | Runtime spec v7 (v2.39.0+) |
| `AppPoint` | The app says what is under the cursor. By default the plug-in raycasts the cursor footprint (both eyes x hotspot + a ring around the cursor body, 20 rays) against 3D physics colliders. Or set `CustomHitTest`, or call `SetHitPoint(worldPos)` / `ClearHitPoint()` from `Update` each frame, ideally casting the rays from `GetFootprintRays`. | Runtime spec v5 (v2.36.0+) and an active `DisplayXRDisplay` / `DisplayXRCamera` rig |

Read-only state for HUDs: `State` (`Off`, `Unsupported`, `Idle`, `OnGlass`, `Lifted`),
`WillDraw`, `Disparity`, `RuntimeSpecVersion`, `Supported`, and the `DepthCursorShownChanged`
event. `Margin` overrides the runtime default of 0.015 eye baselines. Leave it at 0 unless there
is a reason, because every DisplayXR app shares the default.

## What the runtime owns, and what the plug-in owns

| | Owner |
|---|---|
| Placement policy: never behind the glass, margin, rise/sink filter, hybrid anchor, on-glass = OS cursor | runtime |
| Drawing the stereo cursor (the user's own OS cursor shape and size), crosstalk softening (blur + fade with lift) | runtime |
| Measuring the disparity (`Measured`) | runtime |
| Opt-in, choosing the source, the pointer position, the app-point hits | plug-in (C#) |
| Chaining the request on `xrLocateViews` and reading `willDraw` | plug-in (native) |
| Hiding the OS cursor exactly while `willDraw` | plug-in (native overlay + C# `Cursor.visible`) |

The plug-in never draws a cursor itself, so it uses the runtime-drawn path (spec v5) only.
Against a runtime below v5 nothing is chained and the cursor is the plain OS cursor, exactly
as without the feature. `Measured` against a v5/v6 runtime also chains nothing (`State =
Unsupported`); it does not fall back to an `AppPoint` hint with no point.

`SUBMITTED_DEPTH` (v2, the runtime reads a submitted depth layer) is not used. It needs
`XR_KHR_composition_layer_depth`, which the runtime still builds off by default
(`XRT_FEATURE_OPENXR_LAYER_DEPTH`, runtime PR #1957).

## How it is wired

**C# (`Runtime/DisplayXRCursorDepth.cs`).** `DisplayXRProviderDriver.LateUpdate` calls
`DisplayXRCursorDepth.Tick()` after the depth-budget poll:

1. pushes the configuration on change only (`dxr_prov_cursor_configure`);
2. pushes the pointer in Unity screen pixels (Input System `Mouse.current`, else the legacy
   `Input.mousePosition`; nothing while `Cursor.lockState == Locked`) with `Screen.width/height`;
3. for `AppPoint`, fetches the footprint rays (`dxr_prov_cursor_get_footprint_rays`, Unity
   world, built from the previous frame's located views), raycasts them, and pushes the hits;
4. reads `willDraw` (`dxr_prov_cursor_get_state`) and sets `Cursor.visible = false` while it is
   true. It restores only a cursor it hid itself, so an app that hides its own cursor never gets
   it shown back.

**Native (`native~/displayxr_xrprovider/displayxr_provider_session.cpp`).**

- The probe records the advertised `extensionVersion`; the extension is enabled when it is
  ≥ 5 (enabling costs nothing: ADR-046 §0).
- `dxr_prov_begin_frame` calls `ps_cursor_chain` before the primary `xrLocateViews`. While
  the app has opted in and the gate passes (`dxr_cursor::chain_allowed`), it prepends the same
  layout `dxr::CursorDepth::chain` (displayxr-common) uses:
  `locateInfo → hint → drawInfo [→ options] [→ source] → (zone → rig)` and
  `viewState → drawState → placement → (budget → raw)`. `drawInfo.image = XR_NULL_HANDLE`
  selects the runtime's default look, which mirrors the OS cursor.
- `ps_cursor_after_locate` publishes `willDraw` and the outer views, and on each transition
  tells the overlay window to hide or show the OS cursor (`displayxr_set_overlay_cursor_hidden`).
  This is needed because the overlay's `WM_SETCURSOR` sets a cursor explicitly, so Unity's
  `Cursor.visible` cannot reach it.
- The pure math (pointer to canvas UV, Unity ↔ OpenXR, footprint, view rays, nearest hit,
  spec gate) is in `displayxr_cursor_depth_math.h`, unit-tested by
  `native~/tests/cursor_depth_math_test.cpp`, which CI runs in the Linux job. Its rules match
  `dxr::CursorDepth`; keep the two in step.

**Canvas UV.** `cursorUV` is normalised to the canvas the locate frames. The provider's
primary locate frames what Unity renders to the screen: the window, the editor Game-view pane
(zone-glue), or the workspace tile. On an **app-authored** 3D zone
(`DisplayXRProvider.SetZoneRect`, client px) it frames that zone, so the UV is made
zone-relative. Extra zones (`SetZoneCount` > 1) do not get the hint (see below).

**Spaces.** With a rig pushing its pose, the provider chains that pose as the display pose,
so the locate space is Unity world with Z negated. The `AppPoint` hits and the footprint rays
cross that boundary with the same flip. Without a rig there is no defined mapping, so
`AppPoint` hits are dropped (logged once) and the cursor stays on the glass. `Measured` works
without a rig.

## Limits (by design or deferred)

- **Display zones.** The hint rides the primary locate only. A pointer over an extra 3D zone
  gets the plain OS cursor. `Measured` in any zones frame reports no content, which is a
  runtime limit (Amendment 3, point 6), so the cursor stays on the glass. Use `AppPoint` with
  zones.
- **Custom sprite.** Not exposed. The runtime's default look, the user's own OS cursor, is
  what ADR-046 A.6 asks for. An `XrSwapchain` sprite is possible later, but over IPC the
  runtime refuses it today (`SPRITE_UNUSABLE`).
- **Under the shell (IPC).** The service lifts the shell's workspace cursor onto the content
  instead of drawing a second one, and reports `willDraw` back.

## Verifying on Windows (3D panel)

Prerequisites: runtime **v2.39.0 or later** (spec v7) installed and active (`displayxr-cli
info`), with a real 3D panel. Use this branch's package in a sample: in
`samples/<sample>/Packages/manifest.json` of `displayxr-unity-samples`, point
`com.displayxr.unity` at a `file:` path to this checkout. Don't commit that.

1. **AppPoint on the cube: the exact, deterministic check.** In `samples/birp-multipass`
   (`Assets/CubeTest.unity`; the cube has a BoxCollider), add **DisplayXR Depth Cursor** to
   `Main Camera`, set Source = **AppPoint**, and press Play (or build the player, which is
   better because it runs the overlay-window path). Zoom so the cube's front sits **in front
   of** the glass (scroll wheel with the sample input controller).
   - Hover the cube: the OS cursor disappears and a stereo arrow of the same size floats just
     in front of the cube face. Console: `Depth-aware cursor: OnGlass ...`. The provider log
     shows `cursor_depth: willDraw 0 -> 1`.
   - Move off the cube onto the background: the flat OS cursor comes back on the glass
     (`willDraw 1 -> 0`). There must never be two cursors, and never none (a 1-2 frame
     hand-over is expected).
   - Zoom the cube back behind the glass: hovering it now leaves the plain OS cursor, because
     the cursor is never drawn behind the glass.
   - **Softening:** as the cube comes further out, the lifted cursor gets a slightly blurred
     edge and fades to about half opacity. With `DXR_CURSOR_SOFTEN=off` in the player's
     environment (it is in-process, so set it on the app, not the service) it is crisp, for an
     A/B comparison.
2. **Measured: no colliders.** Same scene, Source = **Measured**. Over the cube's
   **edges/corners** (texture), the cursor lifts the same way. Over a flat, evenly lit face
   or the clear-colour background it stays the OS cursor, which is the confidence rule working.
   `samples/hdrp-singlepass-ui` (a textured crate) is a better Measured target: the cursor
   should lift over the whole crate face.
3. **Old runtime.** On a runtime below v2.36.0 (spec < 5), the console shows
   `Unsupported ... keeping the plain OS cursor`, and nothing changes visually.
4. **Optional, under the shell.** Launch the built player as a workspace app. The shell's
   cursor should lift onto the cube when it is over the content.

Logs: provider lines are in the plug-in log (`[DisplayXR-PROV] XR_DXR_cursor_depth: AVAILABLE
(spec vN ...)` at session start, `cursor_depth: ENABLED, source=...`, the first 16 willDraw
transitions). Runtime refusals (one WARN per reason) are in
`%LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.<pid>_<ts>.log`.
