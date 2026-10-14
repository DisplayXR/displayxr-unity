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
DisplayXRCursorDepth.Source  = DisplayXRCursorDepth.DepthSource.Auto; // the default
```

Or add a **DisplayXR Depth Cursor** component (`DisplayXRDepthCursor`) to any GameObject.
It enables the cursor while the component is enabled and exposes the same settings.

| Source | What supplies the depth | Needs |
|---|---|---|
| `Auto` (default) | `Depth` wherever the render pipeline exposes scene depth, `Measured` otherwise. | Runtime spec v5. With v5/v6 there is no measured fallback, so the plain cursor shows where depth is unavailable |
| `Depth` | Unity's own scene depth. A 32×32 patch of the camera depth texture around the cursor is read back asynchronously (it never stalls the GPU, and arrives 1–2 frames late) and unprojected to a point. Exact on any surface, textured or flat. | Runtime spec v5 (v2.36.0+), a DisplayXR rig, and a pipeline that exposes depth (table below) |
| `Measured` | The runtime block-matches a cursor-sized patch between the two rendered views. Low-texture areas (sky, a flat face) are "not confident" and read as on the glass. | Runtime spec v7 (v2.39.0+) |
| `Physics` | Raycasts the cursor footprint (both eyes × hotspot + a ring round the arrow body, 20 rays) against 3D physics colliders, or calls `CustomHitTest`. | Runtime spec v5 and a DisplayXR rig |

`SetHitPoint(worldPos)` / `ClearHitPoint()` override any source for the frame they are called
in (call them from `Update`); `GetFootprintRays()` hands out the footprint rays to cast.

Read-only state for HUDs: `State` (`Off`, `Unsupported`, `Idle`, `OnGlass`, `Lifted`),
`ActiveSource` (which branch answered: `Depth`, `Measured`, `Physics`, `Manual`, or `NoDepth`
while the cursor is held on the glass), `WillDraw`, `Disparity`, `RuntimeSpecVersion`,
`Supported`, and the `DepthCursorShownChanged` event. `Margin` overrides the runtime default of
0.015 eye baselines. Leave it at 0 unless there is a reason, because every DisplayXR app shares
the default.

### Scene depth per render pipeline

| Pipeline | How the patch is read | Available when |
|---|---|---|
| URP (17+) | A render-graph raster pass, enqueued on the active camera from `beginCameraRendering` (no renderer feature needed), reads `cameraDepthTexture` at `BeforeRenderingPostProcessing`. | Always: while the cursor uses depth, the plug-in sets the camera's **Depth Texture** override to On (logged once) and restores it when the cursor is disabled. Render-graph mode only; URP's deprecated compatibility mode is not supported. |
| HDRP (17+) | A global custom pass (`CustomPassVolume.RegisterUniqueGlobalCustomPass`, no volume in the scene) at `BeforePostProcess` reads the camera depth buffer. | Always, except an MSAA depth buffer, which falls back to Measured. |
| BiRP | A camera command buffer at `BeforeImageEffects` reads `_CameraDepthTexture` in the left-eye pass. | Deferred: yes (the plug-in asks for the depth texture, which costs a copy, not a pass). Forward: only if the camera **already** renders a depth texture (`depthTextureMode` includes Depth). The plug-in never forces a depth pre-pass, so forward BiRP otherwise uses Measured. |
| Other SRPs | – | No; Measured. |

Each hook logs once when scene depth becomes available or unavailable, and why.

The fully native alternative is not built. It would read the XR eye texture's own depth
through `QueryTextureDesc` or a provider-allocated depth texture. URP and HDRP fill that depth
(and keep it from being discarded) only when `XRRenderPass.shouldFillOutDepth` is true, and it
is not known what makes Unity set it. On D3D12 a depth texture cannot be copied in a
sub-region. To answer the first question for free, the plug-in logs
`XRRenderPass.shouldFillOutDepth=...` once per session (about one second after the cursor is
enabled), with the pipeline and graphics API.

## What the runtime owns, and what the plug-in owns

| | Owner |
|---|---|
| Placement policy: never behind the glass, margin, rise/sink filter, hybrid anchor, on-glass = OS cursor | runtime |
| Drawing the stereo cursor (the user's own OS cursor shape and size), crosstalk softening (blur + fade with lift) | runtime |
| Measuring the disparity (`Measured`) | runtime |
| Opt-in, choosing the source, the pointer, the scene-depth / physics / manual point | plug-in (C#) |
| Chaining the request on `xrLocateViews` and reading `willDraw` | plug-in (native) |
| Hiding the OS cursor exactly while `willDraw` | plug-in (native overlay + C# `Cursor.visible`) |

The plug-in never draws a cursor itself, so it uses the runtime-drawn path (spec v5) only.
Against a runtime below v5 nothing is chained and the cursor is the plain OS cursor, exactly
as without the feature. `SUBMITTED_DEPTH` (v2, the runtime reads a submitted depth layer) is
not used: it needs `XR_KHR_composition_layer_depth`, which the runtime still builds off by
default (`XRT_FEATURE_OPENXR_LAYER_DEPTH`, runtime PR #1957).

## How it is wired

**Per frame, C#** (`DisplayXRProviderDriver.LateUpdate` → `DisplayXRCursorDepth.Tick()`):

1. Push the pointer in Unity screen pixels (Input System `Mouse.current`, else the legacy
   `Input.mousePosition`; nothing while `Cursor.lockState == Locked`) with `Screen.width/height`.
2. Depth/Auto (`DisplayXRCursorDepthPatch.Tick`):
   1. Request an `AsyncGPUReadback` of last frame's patch (at most 3 in flight; never waited on).
   2. Ask native for the pointer's canvas UV and for eye 0 exactly as Unity renders it this frame
      (pose, the projection matrix the provider hands Unity, and the rendered viewport fraction),
      and remember it under this frame's stamp.
   3. Set the patch rect: a square of 1.6 cursor heights round the arrow body, which covers the
      runtime's footprint (a 0.75 h ring round the body, plus the tip).
   4. The pipeline hook draws the patch with `Hidden/DisplayXR/CursorDepthPatch`. That is plain
      HLSL with no pipeline includes, so it is safe in `Resources/` for every pipeline. Each
      texel stores (raw depth, sample u, sample v, stamp). Carrying its own UV makes the result
      independent of render-target row order, and the stamp pairs the readback with the eye it
      was rendered with. It also rejects a patch that was never drawn.
   5. Readback callback: pick the nearest valid texel (largest raw depth on reversed-Z, smallest
      on OpenGL; the cleared value is "nothing here"). Unproject it through the remembered
      projection and eye pose, then convert from OpenXR to Unity world (Z negated).
3. Choose the source for this frame (below) and push either the measured-disparity
   configuration or the app point (0 or 1 hits).
4. Read `willDraw` and set `Cursor.visible = false` while it is true. C# restores only a
   cursor it hid itself.

**Unprojection** lives in `Runtime/DisplayXRCursorDepthMath.cs`: pure C#, no UnityEngine. It
is unit-tested in `Tests~/CursorDepthMath`, which CI runs as its own job (`C# unit tests`).
- The test vectors cover reversed-Z (D3D11/12, Metal, Vulkan: near = 1, far = 0) and OpenGL
  (near = 0, far = 1), symmetric and off-axis (Kooima) frusta, pose transforms, nearest-texel
  selection, and patch geometry.
- The raw → NDC remap is the inverse of `GL.GetGPUProjectionMatrix`'s z remap, and the tests
  check that equivalence.
- UVs are Unity's bottom-left render-texture convention. `DISPLAYXR_CURSOR_DEPTH_FLIPY=1` is a
  diagnostic for the panel run: it samples upside down, for the case where a pipeline/API pair
  turns out to store depth the other way.

**Native** (`native~/displayxr_xrprovider/displayxr_provider_session.cpp`):
- **Probe and enable.** The probe records the advertised `extensionVersion`. The extension is
  enabled at ≥ 5 (enabling costs nothing: ADR-046 §0).
- **Chaining.** `ps_cursor_chain` prepends the request on the primary `xrLocateViews`, in the
  same layout `dxr::CursorDepth::chain` (displayxr-common) uses:
  `locateInfo → hint → drawInfo [→ options] [→ MEASURED source] → (zone → rig)` and
  `viewState → drawState → placement → (budget → raw)`.
  - `drawInfo.image = XR_NULL_HANDLE` selects the runtime's default look, which mirrors the
    OS cursor.
  - App-point hits arrive in Unity world and are flipped into the locate space; the nearest
    one becomes `nearestPoint`.
- **Exports.** `dxr_prov_cursor_get_eye` and `dxr_prov_cursor_get_canvas_uv` feed the patch.
- **Hiding the OS cursor.** On each `willDraw` transition the overlay window hides or shows
  the cursor (`WM_SETCURSOR`). This is needed because the overlay's own `WM_SETCURSOR` always
  sets a cursor, so `Cursor.visible` cannot reach it.
- **Pure math** for the hint lives in `displayxr_cursor_depth_math.h` and is unit-tested in
  `native~/tests/cursor_depth_math_test.cpp` (CI's Linux job).

### Auto, and why switching sources does not jump

The runtime keeps **one** depth filter per session whatever the source: rise τ = 30 ms,
sink τ = 250 ms, re-primed after 0.5 s without a hint (`u_cursor_depth_filter_step`). Auto is
built so it does not switch per frame at all:
- **No switching at content edges.** A depth readback covers the whole footprint, so it
  always has an answer: content, or "nothing here". An edge therefore never alternates
  between two sources the way a physics hit/miss would.
- **Brief gaps hold the branch.** Once scene depth has produced results (`Working`), a late
  answer (the pointer re-entering the canvas, a dropped readback) is sent as "nothing under
  the cursor". That is a 250 ms sink for a frame or two, about 6% of the lift per 60 Hz frame.
  The alternative, flipping to measured and back, would let the runtime read its last measured
  patch. A measured result is reused while it is fewer than 8 *measured* frames old, however
  long ago those were, so a flip could feed a stale depth into the fast 30 ms rise.
- **When Auto does fall back.** It uses Measured only when scene depth is unavailable: wrong
  pipeline or setting, no rig, or the patch shader is unsupported. That state is logged and
  changes rarely.

## Limits (by design or deferred)

- **Display zones.** The hint rides the primary locate only. A pointer over an extra 3D zone
  gets the plain OS cursor. `Measured` in any zones frame reports no content, which is a
  runtime limit, so the cursor stays on the glass. Depth works in the primary zone.
- **Custom sprite.** Not exposed. The runtime's default look, the user's own OS cursor, is
  what ADR-046 A.6 asks for.
- **Rig-less cameras.** `Depth` and `Physics` need a DisplayXR rig: only the rig pose defines
  Unity world for the locate space. Without one, Auto uses Measured.
- **Under the shell (IPC).** The service lifts the shell's workspace cursor onto the content
  instead of drawing a second one, and reports `willDraw` back.

## Verifying on Windows (3D panel)

Prerequisites: runtime **v2.39.0 or later** (spec v7) installed and active (`displayxr-cli
info`), with a real 3D panel. Use this branch's package in a sample: in
`samples/<sample>/Packages/manifest.json` of `displayxr-unity-samples`, point
`com.displayxr.unity` at a `file:` path to this checkout. Don't commit that.

1. **Auto on the cube (`samples/birp-multipass`, `Assets/CubeTest.unity`).**
   - BiRP renders forward, so first make the camera render depth: add a one-line script
     setting `GetComponent<Camera>().depthTextureMode |= DepthTextureMode.Depth`, or switch
     the camera to Deferred.
   - Add **DisplayXR Depth Cursor** to `Main Camera` (Source = Auto) and build the player,
     which exercises the overlay-window path. Scroll so the cube sits **in front of** the glass.
   - Console: `reading scene depth on BiRP` and `Depth-aware cursor: OnGlass, source Auto ->
     Depth`, plus the one-time `shouldFillOutDepth=...` probe line. Please paste the probe
     line back.
   - **Over the cube, including its flat faces:** the OS cursor disappears and a same-size
     stereo arrow floats just in front of the face. The provider log shows `cursor_depth:
     willDraw 0 -> 1`.
   - **Off the cube:** the flat OS cursor returns. Never two cursors, never none (a 1–2 frame
     hand-over is expected).
   - **Cube behind the glass:** the OS cursor only, because the cursor is never drawn behind
     the glass.
   - **Softening:** with more lift the arrow edge blurs and fades to about half opacity.
     `DXR_CURSOR_SOFTEN=off` on the app (it runs in-process) gives a crisp A/B.
   - **Remove the depth line** (plain forward): the console says scene depth is unavailable,
     and `source Auto -> Measured`. The cube's flat faces now keep the OS cursor (not
     confident); its edges lift.
2. **URP (`samples/urp-singlepass-ui`) and HDRP (`samples/hdrp-singlepass-ui`).**
   Add the component; no other setup. URP logs that it enabled the camera Depth Texture.
   Expect `-> Depth` and lift over any popped-out surface. If the lifted cursor appears over
   the wrong spot, mirrored vertically, rerun with `DISPLAYXR_CURSOR_DEPTH_FLIPY=1` and report
   which pipeline/API needed it.
3. **Source = Physics** in birp-multipass: it lifts over the cube (BoxCollider) without the
   depth line.
4. **Older runtime (< v2.36.0):** the console shows `Unsupported ... keeping the plain OS
   cursor`, and nothing changes.

Logs: provider lines are in the plug-in log (`[DisplayXR-PROV] XR_DXR_cursor_depth: AVAILABLE
(spec vN ...)` at session start, the first 16 configuration changes and willDraw transitions).
Runtime refusals (one WARN per reason) are in
`%LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.<pid>_<ts>.log`.
