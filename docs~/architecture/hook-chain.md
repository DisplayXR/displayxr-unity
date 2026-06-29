# OpenXR Hook Chain (built-app path)

The native plugin (`displayxr_unity` DLL/bundle) is an **OpenXR API-layer hook**. Unity's
own OpenXR loader still drives the session; the plugin intercepts a handful of calls via
`xrGetInstanceProcAddr` to chain the runtime's view-rig descriptor, inject the window
binding, reconcile the runtime's tiling/atlas model, and append overlay layers.

This document is the **built-application** rendering path. The editor **standalone preview**
is a *different* mechanism (its own OpenXR session + multi-camera rig) — see
[preview-session.md](preview-session.md) and the "Two rendering paths" section below.

## Hook chain

```
Unity OpenXR Loader
    ↓
displayxr_hook_xrGetInstanceProcAddr (our wrapper)
    ├── "xrLocateViews"                    → hooked_xrLocateViews          (chain XR_EXT_view_rig; consume render-ready views)
    ├── "xrCreateSession"                  → hooked_xrCreateSession        (inject window-binding ext; overlay HWND)
    ├── "xrGetSystemProperties"            → hooked_xrGetSystemProperties  (extract display info; detect XR_EXT_view_rig)
    ├── "xrEnumerateViewConfigurationViews"→ hooked_… (one-time eye-RT size override when a 3D zone is pre-seeded)
    ├── "xrCreateSwapchain"                → hooked_xrCreateSwapchain      (sRGB→UNORM downgrade in Gamma projects)
    ├── "xrEndFrame"                       → hooked_xrEndFrame             (atlas/imageRect reconcile (D3D11); overlay/wsui/Local2D layers)
    ├── "xrDestroySession" / "xrDestroyInstance" → deferred destruction (ADR-001)
    ├── "xrPollEvent"                      → guard post-destroy
    └── others                             → pass through
```

## Key hooks

### `xrLocateViews` — chain the runtime's view-rig (runtime owns Kooima)

The plugin **does not compute Kooima** (that math was removed — see the `no-vendored-math`
drift guard in `CLAUDE.md`). Instead it chains an `XR_EXT_view_rig` descriptor
(`XrDisplayRigEXT` or `XrCameraRigEXT`, built from the scene transform + a handful of
tunables) onto `xrLocateViews`, and consumes the runtime's **render-ready** `XrView{pose, fov}`:

1. Call the real `xrLocateViews` with the rig descriptor chained, in the plugin's LOCAL
   (display-relative) space; the runtime returns render-ready per-eye pose+fov + a raw-eyes
   side channel (`XrViewDisplayRawEXT`) for gizmos/eye cache.
2. Cache the per-eye matrices in shared state (consumed by C# `GetStereoMatrices`).
3. Build the BiRP view-matrix handedness shim and apply the URP head-pose compensation (#115).
4. Pass the render-ready views through.

If the runtime does **not** advertise `XR_EXT_view_rig` (SPEC_VERSION 2), the hook emits a
one-shot WARN and passes raw views through (no stereo).

### `xrCreateSession` — window binding

Injects `XrWin32WindowBindingCreateInfoEXT` / `XrCocoaWindowBindingCreateInfoEXT` with the
plugin-made **overlay** window handle (over Unity's main HWND). Built apps need the overlay
because Unity already presents its own swapchain on its HWND (DXGI one-swapchain-per-HWND
for D3D12 → `E_ACCESSDENIED`) and its flat present would fight the weave; Unity's own OpenXR
plugin never emits the binding, so the hook injects it.

### `xrGetSystemProperties` — display info + view-rig detection

Extracts physical display geometry / pixel dims, and detects whether the runtime advertises
`XR_EXT_view_rig`.

### `xrCreateSwapchain` — color space

Downgrades sRGB color swapchains to UNORM in Gamma-space projects so output isn't
double-gamma-encoded.

### `xrEndFrame` — tiling/atlas reconcile + overlay layers

See "Rendering model" below for the atlas/`imageRect` reconciliation. Also appends overlay
composition layers: `XrCompositionLayerWindowSpaceEXT` (2D UI), `XrCompositionLayerLocal2DEXT`
(#439/#491), and zone framing.

## Two rendering paths (don't conflate them)

| Path | OpenXR session | Hooks installed? | Views the app renders |
|------|----------------|------------------|-----------------------|
| **Built app** (this doc) | Unity's loader session | yes (`hooked_*`) | **always 2** eyes |
| **Standalone preview / Play Mode** | plugin-owned SA session | **no** (calls the real entry points directly) | **N** (2/4/8) via a multi-camera atlas rig |

A built Unity app **renders and submits only its 2 eyes**; the runtime's Display Processor
synthesises the extra quad/lenticular views (4/8) downstream. Distinct N-view *rendering*
exists only in the standalone preview, where the plugin runs its **own** OpenXR session
(view capacity 32) and renders one Unity `Camera` per view into an atlas RenderTexture —
not Unity's stereo pipeline. (`hooked_xrLocateViews` may receive `*viewCountOutput` = the
display's **max-mode** view count — 2 on a Leia 2D/3D panel, up to 4/8 on quad/lenticular —
so the `count > 2` loop is **real on multi-view displays**, not dead; but Unity still
submits 2-eye projection layers. See the note below.)

> **The view-configuration type is *not* what makes this 2-view.** The DisplayXR runtime
> advertises only `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO` to **every** app — native
> handle apps (e.g. `cube_handle_d3d12_win`) included — and `xrLocateViews` always returns
> `view_count` = the **max across all rendering modes** (e.g. 8), not 2 (`xrEndFrame` then
> accepts a projection `viewCount` matching *any* mode's `view_count`). The difference is
> what each app *does* with that: a native handle app renders the active mode's **N views as
> tiles in one worst-case-sized swapchain** (multiview tiling, ADR-010); the Unity built app
> renders **only the 2 eyes** and lets the DP synthesise the rest. So "always 2-view" is a
> property of the **Unity built-app submission model**, not of `PRIMARY_STEREO`.

## Rendering model: native handle app vs Unity plugin

This is the part most often misunderstood — **they do NOT work the same way.**

The DisplayXR runtime's tiling/atlas/swapchain model (runtime `docs/specs/runtime/`):
a display has rendering modes, each with a tile grid (`cols × rows`) and a per-view
`scaleXY`; the app writes the active mode's tiles (each `window × scaleXY`) into a single
**worst-case-sized** swapchain (sized once over all modes so a mode switch / resize needs
no reallocation — ADR-010), and submits a per-view `subImage.imageRect` per tile.

- **Native handle apps** (e.g. `test_apps/cube_handle_d3d12_win`) follow this directly:
  each frame they render each view at `window × scaleXY` into its tile and submit the tile
  rect. Their per-view *render resolution* is genuinely **adaptive** per mode.

- **The Unity plugin does NOT.** Unity allocates its per-eye swapchains **once** at session
  start from `xrEnumerateViewConfigurationViews` (the plugin passes the runtime's
  recommendation straight through, except a one-time override to a 3D-zone extent when a
  zone was seeded before XR init). Unity then renders each eye at that **fixed** resolution
  every frame; it is **not** re-driven per mode by `scaleXY` (there is no per-frame
  `renderViewportScale` from the mode). The plugin reconciles the mode's tiling at
  **`xrEndFrame`**:
  - **D3D11:** the plugin builds the runtime's atlas itself — copies each fixed-size Unity
    eye texture (clamped, `min(source, tile)`) into the active mode's tile and patches the
    view's `subImage.imageRect` to the atlas tile (`native~/displayxr_d3d11_backend.cpp`).
  - **D3D12:** the plugin submits Unity's per-eye swapchains and the **runtime's** D3D12
    compositor does the per-view atlas blit (`comp_d3d12_renderer.cpp` `draw_projection_pass`).

  Net effect (ADR-006, "legacy app compromise"): Unity renders a fixed eye size; the
  compositor/plugin reconciles the per-mode geometry. **A 2D↔3D switch does not change
  Unity's render resolution** — only the atlas tiling/`imageRect` it's composited into.

### Single-Pass-Instanced (SPI) corollary

Render mode (MultiPass vs SPI) only changes **how the 2 eyes are delivered**, not the above:

- **MultiPass:** two single-layer swapchains (or one rendered twice), one eye each.
- **SPI:** **one** swapchain with `arraySize=2` — the eyes are array **layers** 0 (left) /
  1 (right), each submitted with `subImage.imageArrayIndex` 0/1.

Wherever the atlas composite reads the per-view source it must sample the layer named by
`imageArrayIndex`, not a hardcoded layer 0. The D3D12 runtime compositor did the latter
(both eyes read layer 0 → flat); fixed in `comp_d3d12_renderer.cpp` by creating a
`TEXTURE2DARRAY` SRV with `FirstArraySlice = view.subImage.array_index` when
`DepthOrArraySize > 1` (single-layer / MultiPass path unchanged). See
[../experiments/spi-single-pass.md](../experiments/spi-single-pass.md).

**SPI eligibility:** SPI is structurally a 2-view feature, and what matters is Unity's
**rendered** view count — `xr.viewCount` (the URP `XRPass`), which is the 2 eyes Unity
submits, *not* the native locate `*viewCountOutput` (the display's max-mode count). Unity
renders 2 eyes regardless of the display's lenticular view count (those are DP-synthesised),
so SPI is valid; the per-frame `xr.viewCount == 2` guard in `KooimaProjectionFixFeature`
enforces it.

## Thread-safe shared state

Game thread (C#) and render thread (native hooks) communicate via double-buffered state:

| Buffer | Writer | Reader | Pattern |
|--------|--------|--------|---------|
| Tunables | Game thread | Render thread | Atomic swap on write |
| Scene Transform | Game thread | Render thread | Atomic swap on write |
| Display Info | Render thread (once) | Game thread | Write-once |
| Eye Positions | Render thread | Game thread | Atomic swap on write |
| Stereo Matrices | Render thread | Game thread | Atomic swap on write |

Single-writer, single-reader per buffer; `memory_order_relaxed` reads, `memory_order_release`
writes; no locks.

**Key files:**
- `native~/displayxr_hooks.cpp` — hook implementations + `XR_EXT_view_rig` chaining
- `native~/displayxr_d3d11_backend.cpp` / `displayxr_d3d12_backend.cpp` — backend atlas/present
- `native~/displayxr_shared_state.h` — double-buffered state
- `native~/displayxr_extensions.h` — wire-protocol structs (must match the runtime)
