# Minimal Transparent Overlay — Starter & Teaching Sample

The smallest working DisplayXR scene that demonstrates alpha-native
transparent overlay mode. Drop `MinimalTransparent.cs` into an empty scene,
build standalone (Windows or macOS), and run on a Leia SR display — you get
a 3D cube floating over the desktop with click-through outside its
silhouette.

This sample exists for **teaching**. The companion sample, `Transparent
Avatar`, is the polished real-world version (capsule with breathing
animation, click-through wired to specific renderers, full hit-test
plumbing). This one is stripped to the absolute minimum — about 60 lines
including comments — so you can see exactly which pieces you have to set up
and which the plugin / runtime handle for you.

## Quick start

1. Import this sample via Package Manager → DisplayXR → Samples → "Minimal
   Transparent". The script lands in
   `Assets/Samples/DisplayXR/x.y.z/MinimalTransparent/`.
2. Open or create an empty scene.
3. Make sure the OpenXR feature is enabled: *Project Settings → XR Plug-in
   Management → OpenXR*, check the **DisplayXR** feature.
4. Set `XR_RUNTIME_JSON` to a DisplayXR runtime build (with no 3D panel it
   uses sim_display automatically; `SIM_DISPLAY_OUTPUT=sbs` optionally picks
   the sim output format). The runtime is
   what implements the OpenXR API and the desktop-compose-under-tiles pass
   that makes transparency look right — see below.
5. **Build a standalone** (the editor preview window doesn't show
   transparent mode — `Application.isEditor` short-circuits the native
   window-restyling plumbing).
6. Run on a Leia SR machine. The cube floats; click outside it to talk to
   the desktop window underneath.

## What the script does (~20 lines of substance)

```csharp
[RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
static void Bootstrap()
{
    DisplayXRTransparentOverlay.RequestTransparentSession();   // (A)
}

[RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
static void Install()
{
    var cam = Camera.main;
    cam.gameObject.AddComponent<DisplayXRDisplay>();             // (B)
    cam.gameObject.AddComponent<DisplayXRTransparentOverlay>();  // (C)
}
```

Two `RuntimeInitializeOnLoadMethod` hooks at different lifecycle stages,
plus an `AddComponent` pair. That's the entire app contract.

- **(A)** flips a flag the plugin reads at `xrCreateSession`. Translates to
  `transparentBackgroundEnabled = 1` in the OpenXR window-binding extension
  AND opts the session into `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` so
  Unity preserves alpha through its render path.
- **(B)** adds the DisplayXR rig (display-centric here; swap for
  `DisplayXRCamera` if you want camera-centric / FOV-driven).
- **(C)** adds the overlay component. In `OnEnable` it flips the camera
  clear to `SolidColor (0, 0, 0, 0)` (fully transparent) and asks the
  native plugin to restyle the Unity HWND for click-through.

## What's actually happening end-to-end

The transparency pipeline is **three cooperating mechanisms**, not one.
Each must work or the illusion breaks.

| # | Mechanism | Owner | What it does |
|---|---|---|---|
| 1 | Camera clear → `alpha=0` in transparent regions | **App** (via `DisplayXRTransparentOverlay`) | Both eye views render with `Camera.backgroundColor = (0,0,0,0)`. The OpenXR session is in `ALPHA_BLEND` blend mode (set in `DisplayXRFeature.OnInstanceCreate`), so Unity emits real per-pixel alpha into the swapchain |
| 2 | Runtime DP: compose-under-bg + alpha-gate | **Runtime** | The DisplayXR runtime captures the desktop content under each tile, composes it under the atlas RGBA pre-weave, then alpha-gates post-weave so silhouettes carry true anti-aliased alpha. No chroma sentinel involved |
| 3 | DComp / Cocoa per-pixel-alpha window | **Plugin → OS** | On Windows the plugin creates the overlay HWND as top-level `WS_POPUP` with `WS_EX_NOREDIRECTIONBITMAP` so DWM has no opaque redirection surface; it composites the HWND purely from the runtime's DComp visuals. On macOS the plugin flips Unity's `NSWindow` to `setOpaque:NO` and lets Cocoa do the same job |

The plugin no longer paints a chroma color anywhere — anti-aliased
silhouettes get true soft alpha now that the runtime DP composes the
desktop background pre-weave.

## OpenXR extensions in use

DisplayXR is an OpenXR runtime that implements the standard Khronos API
plus a few **vendor extensions** for 3D-display-specific concerns. The
transparent overlay relies on:

### `XR_DXR_win32_window_binding` (spec v5) and `XR_DXR_cocoa_window_binding`

The app passes the application's `HWND` (or `NSView*`) to the runtime via
these extensions, chained off `XrSessionCreateInfo.next` at
`xrCreateSession`. The plugin's native code (`displayxr_hooks.cpp`)
intercepts session creation, fills in the struct, and forwards. Relevant
field for transparency:

| Field | Type | Set when | Effect |
|---|---|---|---|
| `transparentBackgroundEnabled` | `XrBool32` | `RequestTransparentSession()` | Runtime picks a transparent-capable swapchain (D3D11/D3D12 with DComp on Windows; CAMetalLayer on macOS) and the DP's compose-under-bg path |
| `chromaKeyColor` | `uint32_t` | always `0` now | Legacy post-weave chroma→alpha conversion is disabled. Kept in the ABI for backward compat; the plugin sends `0` unconditionally |

The struct definition lives in `native~/displayxr_extensions.h`. v5 is what
the plugin and runtime both implement.

### `XR_DXR_display_info`

Not used directly by this sample, but every DisplayXR scene depends on it.
The runtime sends back the physical display dimensions, supported eye
tracking modes, etc. via `xrGetSystemProperties`. The plugin reads these to
size swapchains and feed Kooima asymmetric frustum projection. You don't
have to call anything to enable it — the rig components do it.

## Plugin features in use

### `DisplayXRFeature` (auto-enabled)

The OpenXR `OpenXRFeature` subclass that hooks the OpenXR pipeline at the
C# layer. Drives swapchain configuration, projection-layer submission, and
display-info plumbing. **You enable it in Project Settings**, not in code.
In transparent mode it also calls
`SetEnvironmentBlendMode(XrEnvironmentBlendMode.AlphaBlend)` so Unity
emits real alpha to the swapchain.

### `DisplayXRDisplay` / `DisplayXRCamera`

Two flavors of stereo rig. Both push Kooima projection tunables to the
native hook chain via a static rig manager (`DisplayXRRigManager`). The
minimal sample uses `DisplayXRDisplay` (display-centric, scale-as-zoom).
Swap to `DisplayXRCamera` for camera-centric (FOV-driven) behavior — see
`docs~/adr/ADR-004-camera-vs-display-mode.md` in the package source.

### `DisplayXRTransparentOverlay` (the focus of this sample)

The MonoBehaviour that wires up everything app-side. Three responsibilities:

1. **Camera clear** — In `OnEnable`, sets `m_Camera.clearFlags = SolidColor`
   and `m_Camera.backgroundColor = (0,0,0,0)` so transparent regions emit
   alpha=0.
2. **Native window plumbing** — In `OnEnable` (skipped in editor), calls
   `displayxr_set_transparent_overlay()` in the native plugin which mutates
   the Unity main HWND (`WS_POPUP`, DWM cloak, off-screen move), snaps the
   pre-created top-level `WS_EX_NOREDIRECTIONBITMAP` overlay HWND to
   Unity's former rect, and installs the overlay's WndProc with click-
   forwarding + drag-with-phase-snap. On macOS the equivalent is
   `displayxr_macos_configure_unity_nswindow(1)`.
3. **Hit-testing & input** — Each `Update`, polls cursor via
   `displayxr_get_overlay_pointer` (Win32 `GetCursorPos`, bypassing
   Unity's broken-for-cloaked-HWNDs input system), builds a cyclopean
   Kooima ray from the cursor + both eye matrices, and `Physics.Raycast`s
   against `clickableRenderers`' colliders. Dispatches `onPointerEnter` /
   `onPointerExit` / `onPointerDown` / `onPointerUp` / `onPointerClick`
   UnityEvents. **Use these instead of `OnMouseDown`** — `Mouse.current`
   is frozen for cloaked windows.

The static `RequestTransparentSession()` method is separate from the
component because it has to fire **before** `xrCreateSession`, not at
`OnEnable`. Hence the `SubsystemRegistration` hook above.

## Layer ownership map

```
APP  (your Unity scene + bootstrap script)
 ├─ RequestTransparentSession()    ──► flag in plugin shared state
 └─ AddComponent<DisplayXRTransparentOverlay>()  (no chroma color to set)

PLUGIN  (DisplayXR Unity package — C# in Runtime/, native in native~/)
 ├─ C# component:
 │   ├─ Camera clearFlags + backgroundColor=(0,0,0,0)  (= mechanism 1)
 │   └─ Cursor polling, cyclopean ray, hit-test, UnityEvents
 ├─ DisplayXRFeature.OnInstanceCreate:
 │   └─ SetEnvironmentBlendMode(AlphaBlend) so Unity preserves alpha
 └─ Native:
     ├─ Hooks xrCreateSession; fills XrWin32WindowBindingCreateInfoDXR
     │   with transparentBackgroundEnabled=1, chromaKeyColor=0
     ├─ Creates top-level WS_EX_NOREDIRECTIONBITMAP overlay HWND
     ├─ Cloaks + moves Unity main HWND off-screen for click-through
     └─ overlay_wnd_proc: NCHITTEST routing, right-button drag with
         WM_ENTERSIZEMOVE/EXITSIZEMOVE bracketing for SR phase-snap
         (issue #61), click-forward to desktop (Approach C catch+forward)

OPENXR RUNTIME  (DisplayXR/displayxr-runtime — separate repo, separate process)
 ├─ Reads v5 fields off the binding extension at xrCreateSession
 ├─ Picks a transparent-capable swapchain (D3D11/D3D12 with DComp; Metal)
 ├─ DP compose-under-bg pre-weave: captures the desktop content under each
 │   tile and composes it under the atlas RGBA  (= mechanism 2)
 ├─ Composes both eyes via SR SDK weaver (sees only real RGB now —
 │   no chroma sentinel to round-trip)
 └─ DP post-weave alpha-gate: derives transparency mask in screen space
     so silhouettes get true anti-aliased alpha

OS / DWM (Windows) / Cocoa (macOS)
 ├─ DComp / CAMetalLayer: composes the per-pixel-alpha overlay surface
 │   over the desktop  (= mechanism 3)
 └─ Mouse routing: cursor over alpha=0 hits the window below
```

## Customizing this sample

| To do | Change |
|---|---|
| Camera-centric rig instead of display-centric | Swap `DisplayXRDisplay` → `DisplayXRCamera` in `Install()` |
| Specific renderers receive clicks | Set `overlay.clickableRenderers = new[] { ... }` to your renderer array; everything else falls through to the desktop |
| Click handler | `overlay.onPointerClick.AddListener(r => Debug.Log($"clicked {r.name}"))` |
| Drag the cube | Read `overlay.PointerPosition` and `overlay.PointerDelta` each frame in your own `MonoBehaviour.Update` (see how `DragRotateCube` does it in the test-transparent project) |
| Zoom on scroll wheel | Read `overlay.ConsumeWheelDelta()` each frame (returns Win32 raw delta, 120 per notch) and apply to your `DisplayXRDisplay.virtualDisplayHeight` — smaller vHeight = more zoom |
| Replace the cube with your own model | Drop your prefab in the scene — the `FindAnyObjectByType<MeshRenderer>` guard in `Install()` will skip the placeholder cube. Wire `overlay.clickableRenderers` to your model's renderer(s) |

## What this sample deliberately does NOT do

- **No breathing animation** (see `TransparentAvatar` sample).
- **No `DragRotateCube`** (see the `displayxr-unity-test-transparent` repo).
- **No multi-camera rig coordination** (see `DisplayXRRigManager` for that —
  if you have multiple rigs in one scene, only the active one drives the
  Kooima projection; this sample assumes single-rig).
- **No editor-mode preview** (transparent overlay is build-only — the
  editor preview window isn't the right HWND target).

## Further reading

- Architecture: `docs~/adr/ADR-001-deferred-destruction.md`,
  `docs~/adr/ADR-003-native-preview-window.md`,
  `docs~/adr/ADR-004-camera-vs-display-mode.md`
- Runtime extension specs: see the [DisplayXR runtime
  repo](https://github.com/DisplayXR/displayxr-runtime), `docs/specs/`
  directory — `XR_DXR_display_info.md`, `XR_DXR_win32_window_binding.md`
- Issue #57 on `DisplayXR/displayxr-unity` — the original feature request
  with the full problem framing.
- Issue #103 on `DisplayXR/displayxr-unity` — the tracking ticket for
  removing the chroma-color workaround (resolved when this code shipped).
