# Unity plugin — XR_EXT_display_zones port (match the VK avatar)

Goal: the tiger is Kooima-projected **into the 3D-zone rect** (not full display, then
surround-cropped), and the bubble is a Local2D layer in the 2D zone — exactly the
`displayxr-demo-avatar` design.

## Why this is tractable (key finding)
The Unity plugin **already implements the view-rig half**:
- `native~/displayxr_hooks.cpp:310 hooked_xrLocateViews` already chains an
  `XrDisplayRigEXT` descriptor onto `xrLocateViews` and returns the runtime's
  render-ready per-view pose+fov (Kooima).
- `Runtime/DisplayXRCamera.cs:131-136` consumes those — `SetStereoViewMatrix` +
  overrides the projection from the runtime fov (`dxr_projection_from_fov`).

So display-zones = chain a **zone** in front of the rig on locate, place the
projection layer into the zone rect on submit, and feed the demo the zone rect.

## Avatar reference (blueprint) — displayxr-demo-avatar
- Extensions: `xr_session.cpp:113-121` enables `XR_EXT_local_3d_zone`,
  `XR_EXT_view_rig`, `XR_EXT_display_zones`. Resolves `xrGetDisplayZoneCapabilitiesEXT`
  + `xrGetDisplayZoneRecommendedViewSizeEXT` (`xr_session.cpp:214-225`).
- Zone setup: `main.cpp:1057-1124 TryActivateZones()` — caps query, pre-sized zone
  swapchain (`capW=maxCols*dispW`, `capH=maxRows*dispH*3/4`).
- Per-frame zone rect: `main.cpp:1462-1465` — `zoneId=1`, rect.offset=(0, windowH/4),
  rect.extent=(windowW, windowH*3/4) = bottom 75%.
- Locate chain: `main.cpp:1494-1509` — `zone.next=&rig`, `locateInfo.next=&zone`,
  `viewState.next=&XrViewDisplayRawEXT` → runtime returns render-ready pose+fov +
  `canvasRectPx`.
- Projection from runtime: `main.cpp:1647-1664` — view = inverse(XrView.pose),
  proj from `XrView.fov` + app near/far (`nearZ=ez-vH`, `farZ` ZDP-relative via
  `RigLocalEyeZ`). **App does NOT compute Kooima** — runtime does.
- Submit: `main.cpp:1804-1814` — projection layer subImage.imageRect = zone tile,
  pose/fov from zone views.
- Bubble: `main.cpp:1856-1971` — `XrCompositionLayerLocal2DEXT`, rect = top-25% band.
- Convention: `main.cpp:1712 setPlainViewConvention(zonesFrame)` (plain +Y-up,
  negative-height viewport; disables the legacy view-stage mirror).

## Unity port — phases

### P1 — enablement
- `Runtime/DisplayXRFeature.cs ExtensionStrings` += `XR_EXT_display_zones`
  (view_rig already there; `XR_EXT_local_3d_zone` already added for the bubble).
- Native: resolve `xrGetDisplayZoneCapabilitiesEXT` +
  `xrGetDisplayZoneRecommendedViewSizeEXT` in `hooked_xrGetSystemProperties`
  (same place surround/output-rect PFNs resolve, ~line 584). Query caps once;
  gate the whole zone path on `caps.supported && caps.maxZones3D>=1`.

### P2 — zone-rect API
- New export `displayxr_set_3d_zone_rect(int x,int y,int w,int h)` +
  `displayxr_clear_3d_zone()` (mirror the canvas-rect setter pattern,
  `displayxr_hooks.cpp:213-219`). Store `s_zone_valid` + the rect.
- C# `DisplayXRNative` P/Invokes + a small `DisplayXR3DZone` helper (or the demo
  calls it directly). The demo pushes the 3D-zone rect each frame.

### P3 — locate-views zone chain (the projection)
- In `hooked_xrLocateViews` (where the rig is chained), when `s_zone_valid`:
  build `XrDisplayZoneEXT{zoneId=1, rect, next=&rig}` and set
  `modified_info.next = &zone` (zone in front of rig). Add
  `XrViewDisplayRawEXT` on `viewState.next` to capture `canvasRectPx`.
- Result: `DisplayXRCamera` already consumes the returned zone-scoped pose+fov →
  the tiger is Kooima-projected for the zone, no C# change needed.

### P4 — projection-layer zone placement (the hard part)
- The avatar renders into a zone swapchain at zone size and submits subImage=zone.
  Unity renders into its FULL swapchain. In `hooked_xrEndFrame`, set the projection
  layer views' `subImage.imageRect` to the zone tile (and/or chain the zone on the
  layer) so the runtime composites the projection INTO the zone rect rather than
  full-window. CONFIRM with the runtime team whether a full-swapchain projection +
  zone-rect subImage composites correctly, or whether Unity must render into a
  zone-sized sub-region first. This is the main unknown — prototype + eyeball early.

### P5 — demo
- `TigerSpeechBubble`: push the 3D-zone rect (the editor's 3D zone) via
  `displayxr_set_3d_zone_rect`; drop `displayxr_set_canvas_rect` (output-rect/surround
  path); drop the surround entirely (no longer needed — zones place the 3D).
- Bubble: Local2D in the 2D zone (already wired) — but FIX the
  `DisplayXRLocal2D` blank-RT bug first (the camera→RT render produced nothing;
  suspect the CanvasScaler/WorldSpace-canvas interaction or the manual
  `Camera.Render()` in a built player — compare against the working
  `DisplayXRWindowSpaceUI`).

### P6 — convention
- Verify `DisplayXRCamera`'s projection override is correct for zone fov (likely
  already fine — it builds from the returned fov). Watch for a Y-flip vs Unity's
  convention.

## Risks / unknowns
1. **P4 placement** — biggest unknown (full-swapchain projection → zone rect).
2. **DisplayXRLocal2D blank RT** — must fix for the bubble (independent of zones).
3. Resize: re-query `xrGetDisplayZoneRecommendedViewSizeEXT` on zone-rect/mode change.

## Current state (this session)
- Local2D plugin capability SHIPPED on branch `feat/local2d-layer` (native compiles,
  C# component + interop + `.meta`, rebuilt DLL). The Local2D LAYER composites in the
  runtime (proven) — only the Unity-side RT render is blank (P5 fix).
- Demo branch `feat/local2d-bubble` has the WIP (restored original + Local2D bubble);
  superseded by this zones port.
- Runtime #602 fix released v1.21.0; leia plugin v1.8.3.
