# Click-Through Silhouette Mask (transparent overlay)

> **Status: Shipping.** The per-pixel click-through silhouette mask for transparent
> overlay apps (issue #57 family; alpha-native since #103). This doc explains the
> end-to-end pipeline, the **one invariant that makes it work** (mask/weave rect
> alignment), and the debugging pitfalls that cost real time when a v2.0.0 hook-removal
> regression (#166) silently broke that invariant.

## Why this exists

A transparent overlay app (the avatar / tiger demos) renders clickable 3D content over
the live desktop. Clicks **on** the visible silhouette must act; clicks **off** it (the
gaps between the tiger's legs, around the hat, the empty corners of the window) must fall
**through** to whatever desktop window is underneath.

The overlay is a cloaked, top-level `WS_EX_NOREDIRECTIONBITMAP` window composited purely
from the runtime's DComp visuals — it is **not** OS-foreground, so Unity's normal input
(and the OS's own per-pixel hit routing) can't decide click-through for us. Instead we
shape the overlay window's **input region** to the visible silhouette with `SetWindowRgn`.
Outside the region the OS treats the overlay as if it weren't there (full native
click-through); inside, the overlay catches the click and Unity's per-pixel raycast
decides whether to act.

## The pipeline (end to end)

```
DisplayXRTransparentOverlay.LateUpdate (C#)                 native (win32.c)
──────────────────────────────────────────                 ─────────────────
1. render clickable renderers' silhouette
   PER EYE (union) → small R8 RT, using the
   provider's stereo matrices
   (displayxr_get_stereo_matrices), then a
   dilate pass → AsyncGPUReadback
                        │
2. OnHitMaskReadback → displayxr_set_overlay_hit_mask(
     mask, mask_w=256, mask_h=144,
     dst_w, dst_h = overlay client size) ───────────────►  3. stamp the mask into the
                                                              TARGET RECT (see below):
                                                              RLE rows → RECTs (edges
                                                              rounded OUTWARD) →
                                                              ExtCreateRegion →
                                                              SetWindowRgn(overlay, rgn)
                                                              — POST-WEAVE, on the final
                                                              composited window.
```

- **`mask`** is a low-res (256×144) R8 coverage bitmap, row 0 = top. It only needs to be
  silhouette-accurate, not feature-accurate. `DXR_DUMP_HIT_MASK=1` dumps it to
  `%TEMP%\displayxr_hitmask.png`.
- **`dst_w`/`dst_h`** is the overlay's *client* size from `displayxr_get_overlay_size`
  (NOT `Screen.width/height` — Unity's real HWND is parked off-screen with a frozen size).
- The union is built **per eye** (and per zone in multi-zone mode) because the lenticular
  displays a column-by-column union of the eyes; a cyclopean-only mask is narrower than the
  visible silhouette on high-disparity foreground geometry (hands, hat) and would clip.

## The alignment invariant (the one thing that must hold)

The mask is rasterized **full-window** (in overlay client pixels), but the woven 3D content
does **not** fill the window — it lives in a **sub-rect**: a display *zone* or a *canvas*
rect, e.g. `(0, 478) 840×970` in the tiger demo. The runtime weaves the 3D into that
sub-rect; the surrounding area is transparent desktop.

So `displayxr_set_overlay_hit_mask` must **scale + offset** the full-window mask into that
same sub-rect, or the region won't line up with the woven pixels. Native learns the sub-rect
two ways (`displayxr_win32.c`):

| Case | Source of the target rect |
|---|---|
| **Multi-zone** (`dxr_prov_get_zone_count() > 1`) | `dxr_prov_get_zone_rect_px(zone, …)` — stamp into each zone's rect (union). |
| **Single zone / canvas** | `displayxr_get_canvas_rect_px(…)` — the app-set canvas sub-rect, or the **full window** when unset. |

**The invariant:** the rect the mask is stamped into (`cv`) must equal the rect the runtime
weaves the 3D into. The app is responsible for keeping them equal:

- **Weave placement** ← the provider zone: `DisplayXRProvider.SetZoneRect(…)` →
  `dxr_prov_set_3d_zone_rect` → `XrDisplayZoneEXT`.
- **Mask placement** ← the canvas rect: `DisplayXRNative.displayxr_set_canvas_rect(zoneRect)`
  → `s_canvas_rect` (in `displayxr_native_shared.cpp`), read back by
  `displayxr_get_canvas_rect_px`.

A transparent zoned app **must set both** to the same rect (see
`TigerSpeechBubble.cs`, which calls `SetZoneRect` *and* `set_canvas_rect`).

### The failure mode

If the canvas rect is **not** set, `displayxr_get_canvas_rect_px` returns 0 → native stamps
the mask **full-window** while the tiger is woven into `(0,478 840×970)` → the region
overshoots the woven pixels and the mask **clips the visible tiger**.

> **This was the #166 regression.** The hook hard-removal deleted the `displayxr_set_canvas_rect`
> export along with `displayxr_hooks.cpp`, but the app still called it (a try-guarded P/Invoke,
> so the missing export threw `EntryPointNotFound` and was **swallowed silently**). The canvas
> rect stayed unset → full-window mask → clip. Fix: re-home the setter into
> `displayxr_native_shared.cpp` (the same TU that owns the `s_canvas_rect` statics and the reader).

**Crucial mental model:** both the mask *and* the weave derive from the **same provider
matrices**, so under head tracking they move together. A clip is therefore **never** a matrix
problem — it is a **rect mismatch** between where the mask is stamped and where the content
is woven.

## Debugging pitfalls (hard-won, #166)

1. **The `I`-key atlas capture is PRE-WEAVE.** It shows the projection-layer tiles before
   weaving. The mask is `SetWindowRgn` applied **POST-WEAVE** to the final window. The atlas
   **cannot** show the mask — do not use it to diagnose a clip. (This dead end cost the most time.)
2. **Kooima is head-tracked.** Every capture at a different head position is a different
   projection. The only confound-free state is **head OFF the tracker** (default pose). Always
   A/B at default pose.
3. **Window size drifts across runs.** `DemoWindowController` persists the overlay W/H in
   `HKCU:\Software\DisplayXR\<app>`; it changes `dst_w/dst_h`. **Clear that key before every
   A/B run** (`reg delete … /f`).
4. **Diagnose with the native stamp geometry, not the mask PNG or the matrices.** The mask PNG
   is built entirely C#-side (from the matrices, *before* native stamps it) and the matrices are
   identical at a fixed pose — so **neither** reveals a rect mismatch. Log, inside
   `displayxr_set_overlay_hit_mask`, the `zone_count`/`use_zones`, the resolved target rect
   (`cv`), and the region bounding box. That single line localizes it: known-good logged
   `cv=(0,478 840×970)`; the regressed build logged `cv=(0,0 840×1448)` (full-window).
5. **When native looks byte-identical yet behaves differently, suspect a shared-state coupling,
   not a code path.** A removed C# **setter** whose native shared-state a surviving **reader**
   depends on (here: `displayxr_set_canvas_rect` writing `s_canvas_rect`, read by the mask stamp)
   is invisible to a code diff *and* to a dumpbin-exports-vs-callers audit. Grep for exports the C#
   still calls that the DLL no longer defines — **try-guarded P/Invokes fail silently**, so the
   reader just sees the default value.
6. **Rule out the toolchain cheaply.** If "identical source, different behavior" is the mystery,
   rebuild the known-good tag **locally**: a matching DLL **size** means the same MSVC/flags, so
   the divergence is real source/state, not compiler nondeterminism. (For #166, the local rebuild
   of v1.24.1 was byte-size-identical to the shipped one — toolchain ruled out in one build.)

## Key files

| File | Role |
|---|---|
| `Runtime/DisplayXRTransparentOverlay.cs` | C# per-eye silhouette render + async readback (`RenderHitMaskAndRequestReadback`, `OnHitMaskReadback`); `DumpHitMaskPng` (`DXR_DUMP_HIT_MASK=1`). |
| `native~/displayxr_win32.c` | `displayxr_set_overlay_hit_mask` — stamp mask into the target rect → `ExtCreateRegion` → `SetWindowRgn`; `region_target_hwnd`/`region_target_ready`. |
| `native~/displayxr_native_shared.cpp` | `displayxr_get_canvas_rect_px` (reader) + `displayxr_set_canvas_rect` (writer, re-homed in #166) over the `s_canvas_rect` statics. |
| `native~/displayxr_xrprovider/displayxr_provider_session.cpp` | `dxr_prov_get_zone_count`/`dxr_prov_get_zone_rect_px` (multi-zone rects); `dxr_prov_set_3d_zone_rect` (zone weave); publishes the stereo matrices the mask reads. |

## Cross-references

- [XR Display Provider](xr-display-provider.md) — the provider that publishes the stereo
  matrices and drives zone weaving.
- [Kooima Pipeline](kooima-pipeline.md) — the stereo projection the silhouette is rendered from.
- [ADR-006: Window-Relative Kooima](../adr/ADR-006-window-relative-kooima.md) — why the projection
  (and hence the silhouette) is window/sub-rect relative.
