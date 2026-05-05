# Minimal Transparent Overlay (Windows) — Starter & Teaching Sample

The smallest working DisplayXR scene that demonstrates chroma-key transparent
overlay mode. Drop the `MinimalTransparent.cs` script into an empty scene,
build Windows standalone, and run on a Leia SR display — you get a 3D cube
floating over the desktop with click-through outside its silhouette.

This sample exists for **teaching**. The companion sample, `Transparent
Avatar (Windows)`, is the polished real-world version (capsule with breathing
animation, click-through wired to specific renderers, full hit-test plumbing).
This one is stripped to the absolute minimum — about 70 lines including
comments — so you can see exactly which pieces you have to set up and which
the plugin handles for you.

## Quick start

1. Import this sample via Package Manager → DisplayXR → Samples → "Minimal
   Transparent (Windows)". The script lands in
   `Assets/Samples/DisplayXR/x.y.z/MinimalTransparent/`.
2. Open or create an empty scene.
3. Make sure the OpenXR feature is enabled: *Project Settings → XR Plug-in
   Management → OpenXR (Windows)*, check the **DisplayXR** feature.
4. Set `XR_RUNTIME_JSON` to a DisplayXR runtime build (or use
   `SIM_DISPLAY_ENABLE=1 SIM_DISPLAY_OUTPUT=sbs` for a sim). The runtime is
   what implements the OpenXR API and the post-weave alpha pass — see below.
5. **Build a Windows standalone** (the editor preview window doesn't show
   transparent mode — `Application.isEditor` short-circuits the layered-
   window plumbing).
6. Run on a Leia SR machine. The cube floats; click outside it to talk to
   the desktop window underneath.

## What the script does (~30 lines of substance)

```csharp
static readonly Color s_ChromaKey = new Color(128f/255f, 127f/255f, 129f/255f, 0f);

[RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
static void Bootstrap()
{
    DisplayXRTransparentOverlay.RequestTransparentSession();   // (A)
    DisplayXRTransparentOverlay.RequestChromaKey(s_ChromaKey); // (B)
}

[RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
static void Install()
{
    var cam = Camera.main;
    cam.gameObject.AddComponent<DisplayXRDisplay>();             // (C)
    var overlay = cam.gameObject.AddComponent<DisplayXRTransparentOverlay>();
    overlay.chromaKeyColor = s_ChromaKey;                        // (D)
}
```

Two `RuntimeInitializeOnLoadMethod` hooks at different lifecycle stages, plus
an `AddComponent` pair. That's the entire app contract.

- **(A)** flips a flag the plugin reads at `xrCreateSession`. Translates to
  `transparentBackgroundEnabled = 1` in the OpenXR window-binding extension.
- **(B)** packs the color into a Win32 `COLORREF` (`0x00BBGGRR`) and stores
  it for the same `xrCreateSession` step. Translates to `chromaKeyColor =
  0x00818081` (gray) in the binding struct.
- **(C)** adds the DisplayXR rig (display-centric here; swap for
  `DisplayXRCamera` if you want camera-centric / FOV-driven).
- **(D)** assignment goes through the `chromaKeyColor` property setter (added
  in v1.2.1), which re-pushes camera background + native overlay state. All
  three sides — Unity camera clear, Win32 `LWA_COLORKEY`, runtime post-weave
  shader — now agree on the chroma color.

## What's actually happening end-to-end

The "transparency pipeline" is **four cooperating mechanisms**, not one. Each
must work or the illusion breaks. Conflating them is the easiest way to get
stuck debugging.

| # | Mechanism | Owner | What it does |
|---|---|---|---|
| 1 | Camera clear → chroma color in transparent regions | **App** (via `DisplayXRTransparentOverlay`) | Both eye views render the chroma color where no geometry is drawn. `L == R` per sub-pixel ensures the SR weaver passes those pixels through unchanged |
| 2 | OpenXR session config: transparency flag + chroma color | **Plugin → Runtime** (extension struct) | Plugin packs `transparentBackgroundEnabled=1` and `chromaKeyColor` into `XrWin32WindowBindingCreateInfoEXT` (spec v5) at `xrCreateSession` |
| 3 | Post-weave alpha conversion shader | **Runtime** | After the SR weaver writes opaque RGB, a shader pass alpha-zeroes pixels matching the chroma color before Present. Lets DComp blend per-pixel |
| 4 | Layered-window punch-through | **OS / DWM** (set up by Plugin) | `WS_EX_LAYERED + LWA_COLORKEY` on the parent HWND tells DWM to treat matching pixels as fully transparent and route mouse input through to the desktop window underneath |

**Mechanism 4 alone** is enough for visual transparency on a vanilla Windows
window — and it gives you the magenta-pink-fringe edge halo that the v1.2.0
release shipped with, because anti-aliased silhouette pixels are partial-
blends with the key color. **Mechanism 3** layers per-pixel alpha conversion
on top so DComp gets a real alpha channel, and switching to a near-neutral
gray chroma color (v1.2.1) makes the residual edge artifacts blend invisibly
into typical desktop backgrounds.

## OpenXR extensions in use

DisplayXR is an OpenXR runtime that implements the standard Khronos API plus
a few **vendor extensions** for 3D-display-specific concerns. The transparent
overlay relies on:

### `XR_EXT_win32_window_binding` (spec v5)

The app passes the application's `HWND` to the runtime via this extension's
struct, chained off `XrSessionCreateInfo.next` at `xrCreateSession`. The
plugin's native code (`displayxr_hooks.cpp`) intercepts session creation,
fills in the struct, and forwards. Spec v5 added two fields used here:

| Field | Type | Set when | Effect |
|---|---|---|---|
| `transparentBackgroundEnabled` | `XrBool32` | `RequestTransparentSession()` | Runtime picks a transparent-capable swapchain (D3D11 with DComp/flip-model instead of `DXGI_ALPHA_MODE_IGNORE`) |
| `chromaKeyColor` | `uint32_t` (Win32 COLORREF, `0x00BBGGRR`) | `RequestChromaKey(Color)` | Runtime installs a post-weave alpha-conversion shader pass keyed on this RGB; pixels matching → `(0,0,0,0)` premultiplied |

The struct definition lives in `native~/displayxr_extensions.h` and must
match the runtime's view byte-for-byte. Versioned by extension spec version;
v5 is what the v1.2.1 plugin and current runtime both implement.

### `XR_EXT_display_info`

Not used directly by this sample, but every DisplayXR scene depends on it.
The runtime sends back the physical display dimensions, supported eye
tracking modes, etc. via `xrGetSystemProperties`. The plugin reads these to
size swapchains and feed Kooima asymmetric frustum projection. You don't
have to call anything to enable it — the rig components do it.

(Cross-platform variants exist for Cocoa / Android — `XR_EXT_cocoa_window_binding`
and `XR_EXT_android_surface_binding` — but transparent overlay mode is
Windows-only for now.)

## Plugin features in use

### `DisplayXRFeature` (auto-enabled)

The OpenXR `OpenXRFeature` subclass that hooks the OpenXR pipeline at the C#
layer. Drives swapchain configuration, projection-layer submission, and
display-info plumbing. **You enable it in Project Settings**, not in code.
Without it, none of the runtime extensions take effect — the OpenXR loader
won't know to pass the v5 struct fields.

### `DisplayXRDisplay` / `DisplayXRCamera`

Two flavors of stereo rig. Both push Kooima projection tunables to the native
hook chain via a static rig manager (`DisplayXRRigManager`). The minimal
sample uses `DisplayXRDisplay` (display-centric, scale-as-zoom). Swap to
`DisplayXRCamera` for camera-centric (FOV-driven) behavior — see
`docs~/adr/ADR-004-camera-vs-display-mode.md` in the package source.

### `DisplayXRTransparentOverlay` (the focus of this sample)

The MonoBehaviour that wires up everything app-side. Three responsibilities:

1. **Camera clear** — In `OnEnable`, sets `m_Camera.clearFlags = SolidColor`
   and `m_Camera.backgroundColor = chromaKeyColor`. That's mechanism (1)
   from the table.
2. **Win32 plumbing** — In `OnEnable` (skipped in editor), calls
   `displayxr_set_transparent_overlay()` in the native plugin which mutates
   the Unity main HWND (`WS_POPUP + WS_EX_LAYERED + LWA_COLORKEY`, DWM
   cloak), creates an overlay HWND on top (`WS_EX_NOREDIRECTIONBITMAP +
   WS_EX_NOACTIVATE` for per-pixel alpha + non-stealing-focus), and installs
   the overlay's WndProc with click-forwarding + drag-with-phase-snap.
   That's mechanism (4).
3. **Hit-testing & input** — Each `Update`, polls cursor via
   `displayxr_get_overlay_pointer` (Win32 `GetCursorPos`, bypassing Unity's
   broken-for-cloaked-HWNDs input system), builds a cyclopean Kooima ray
   from the cursor + both eye matrices, and `Physics.Raycast`s against
   `clickableRenderers`' colliders. Dispatches `onPointerEnter` /
   `onPointerExit` / `onPointerDown` / `onPointerUp` / `onPointerClick`
   UnityEvents. **Use these instead of `OnMouseDown`** — `Mouse.current` is
   frozen for cloaked windows.

The static `RequestTransparentSession()` and `RequestChromaKey(Color)`
methods are separate from the component because they have to fire **before**
`xrCreateSession`, not at `OnEnable`. Hence the `SubsystemRegistration`
hook above.

## Layer ownership map

```
APP  (your Unity scene + bootstrap script)
 ├─ RequestTransparentSession()    ──► flag in plugin shared state
 ├─ RequestChromaKey(color)        ──► color in plugin shared state
 └─ AddComponent<DisplayXRTransparentOverlay>() with chromaKeyColor

PLUGIN  (DisplayXR Unity package — C# in Runtime/, native in native~/)
 ├─ C# component:
 │   ├─ Camera clearFlags + backgroundColor (= mechanism 1)
 │   ├─ Cursor polling, cyclopean ray, hit-test, UnityEvents
 │   └─ chromaKeyColor property setter → ApplyChromaKey()
 └─ Native:
     ├─ Hooks xrCreateSession; fills XrWin32WindowBindingCreateInfoEXT
     │   spec v5 fields (= mechanism 2 — passed into runtime)
     ├─ Mutates Unity main HWND for WS_POPUP + LWA_COLORKEY + DWM cloak
     ├─ Creates the transparent overlay HWND
     └─ overlay_wnd_proc: NCHITTEST routing, right-button drag with
         WM_ENTERSIZEMOVE/EXITSIZEMOVE bracketing for SR phase-snap
         (issue #61), click-forward to desktop window (Approach C catch+forward)

OPENXR RUNTIME  (DisplayXR/displayxr-runtime — separate repo, separate process)
 ├─ Reads v5 fields off the binding extension at xrCreateSession
 ├─ Picks a transparent-capable swapchain (D3D11 with DComp/flip-model)
 ├─ Composes both eyes via SR SDK weaver (writes opaque RGB)
 └─ Post-weave alpha-conversion shader pass (= mechanism 3)
     keyed on chromaKeyColor; matching pixels → (0,0,0,0) premultiplied

OS / DWM  (Windows)
 ├─ LWA_COLORKEY: punches matching pixels invisible (= mechanism 4)
 ├─ Mouse routing: cursor over a punched pixel hits the window below
 └─ DComp: composes the overlay's per-pixel-alpha output over the desktop
```

## Customizing this sample

| To do | Change |
|---|---|
| Different chroma color | Update `s_ChromaKey` (single source of truth — the bootstrap and the component setter both read it) |
| Camera-centric rig instead of display-centric | Swap `DisplayXRDisplay` → `DisplayXRCamera` in `Install()` |
| Specific renderers receive clicks | Set `overlay.clickableRenderers = new[] { ... }` to your renderer array; everything else falls through to the desktop |
| Click handler | `overlay.onPointerClick.AddListener(r => Debug.Log($"clicked {r.name}"))` |
| Drag the cube | Read `overlay.PointerPosition` and `overlay.PointerDelta` each frame in your own `MonoBehaviour.Update` (see how `DragRotateCube` does it in the test-transparent project) |
| Resize the cube on scroll wheel | Already wired in the native overlay; controlled by the plugin's overlay HWND, no app code needed |
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
- Transparent-overlay design notes:
  `docs~/roadmap/transparent-overlay-handoff.md` (long-form session history,
  the rejected approaches, and the four-mechanism dissection in detail)
- Runtime extension specs: see the [DisplayXR runtime
  repo](https://github.com/DisplayXR/displayxr-runtime), `docs/specs/`
  directory — `XR_EXT_display_info.md`, `XR_EXT_win32_window_binding.md`,
  `chroma-key-transparent-overlay.md`
- Issue #57 on `DisplayXR/displayxr-unity` — the original feature request
  with the full problem framing and rejected approaches.
