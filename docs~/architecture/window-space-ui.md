# Window-space 2D UI (`DisplayXRWindowSpaceUI`)

A flat UI panel composited by the runtime at a fixed rect in **window space**, at a chosen
stereo depth. Use it for HUDs, tuning panels, and controls that should stay crisp and
readable rather than living in the 3D scene.

For a whole scene that should be flat, see [`two-dimensional-scenes.md`](two-dimensional-scenes.md)
— that's a different mechanism (a runtime mode switch), not this one.

## Setup

1. Add a `Canvas` to a GameObject. **Don't share it** with any other UI — the component
   takes the Canvas over completely.
2. Add `DisplayXRWindowSpaceUI` to the same GameObject.
3. Add `DisplayXRWsuiMouseRouter` — **without it the panel renders but does not respond to
   clicks.** See "Input" below.

| Field | Default | Meaning |
|---|---|---|
| `positionX`, `positionY` | `0.02` | Top-left of the panel, as a **fraction** of the window, top-left origin |
| `width`, `height` | `0.3`, `0.15` | Panel size, also fractional |
| `disparity` | `0` | Stereo depth. `0` = the zero-disparity plane (at the screen) |
| `resolution` | `1024x1024` | The RenderTexture the Canvas is rendered into |
| `OverlayTexture` | — | The RenderTexture itself (read-only) |
| `IsCursorOverInteractive` | `false` | **Static.** Set by input routers; read by scene controllers |

## How it renders

The component does not render your Canvas where you put it. It:

1. Switches the Canvas to `RenderMode.WorldSpace`.
2. Parks it at world **`(0, 100000, 0)`** on **layer 30**, a private layer, well away from
   your scene. The overlay camera culls to that layer alone, so the layer is **re-applied
   to every descendant each `LateUpdate`** — anything you `Instantiate` under the canvas at
   runtime (list items, tiles, a file dialog, a dropdown's blocker) is born on its prefab's
   layer and would otherwise be silently culled. You never need to set layers yourself;
   the first time the component has to fix a stray object it logs once.
3. Sizes it so **1 RT pixel == 1 UI unit** (`sizeDelta = resolution`, `localScale = 0.01`).
4. Renders it with a dedicated hidden orthographic camera into `OverlayTexture`.
5. Hands that texture to the runtime as an `XrCompositionLayerWindowSpaceDXR` composition
   layer, which the runtime composites at your window-space rect and disparity.

All of that is restored on `OnDisable` — original render mode, position, rotation, scale,
layer and `worldCamera`.

**Why WorldSpace and not ScreenSpaceCamera?** URP's RenderGraph runs the camera stack in one
pass and a `ScreenSpaceCamera` canvas gets pulled into it. WorldSpace plus a dedicated
camera with a `targetTexture` is the arrangement that renders predictably on BiRP, URP and
HDRP alike. The overlay camera is left **enabled** deliberately (rather than driven by a
manual `Camera.Render()`), because under URP's RenderGraph a manual render targets the
backbuffer and blanks the whole XR mirror — a pure-black docked Game view.

## Input

**The composition layer carries pixels, not input.** Nothing about submitting a layer tells
the runtime or Unity where your buttons are.

Because the Canvas has been moved to a private WorldSpace location and is rendered by an
offscreen camera, the mouse position in your app window and the overlay camera's screen
space are two unrelated coordinate systems. `GraphicRaycaster` finds nothing, and **every
button, toggle and slider is dead** while the panel looks perfectly fine.

The plugin ships the *hooks*:

- the overlay camera is wired as the Canvas's `worldCamera`, so a raycaster has something
  to project against;
- `DisplayXRWindowSpaceUI.IsCursorOverInteractive` is a static flag routers set and scene
  controllers read.

The *policy* — reading a cursor, mapping it, synthesizing events — is app-owned and ships as
a sample: **`Samples~/WindowSpaceUI`** (`DisplayXRWsuiMouseRouter`). Import it, drop it on
the same GameObject, and the panel becomes interactive. Fork it if your input model isn't a
mouse.

### Traps if you write your own router

- **`ignoreReversedGraphics` must be `false` — on every raycaster.** The overlay camera uses
  `up = Vector3.down` to Y-flip the RT (the runtime's texture origin is top-left), which
  makes `Dot(camera.forward, canvas.forward) == -1`. `GraphicRaycaster` reads that as "the
  back of the graphic faces the camera" and silently skips **every** hit. A nested canvas
  arrives with Unity's default of `true`.
- **Raycast every `GraphicRaycaster` under the wsui, and give nested canvases the overlay
  camera.** A child Canvas (a file browser, a modal, a dropdown blocker) brings its own
  raycaster the root never consults, and it does not inherit `worldCamera` — its raycaster
  falls back to `Camera.main` and projects with the wrong camera. Either way the dialog is
  dead to clicks with no error. The shipped router collects all raycasters each frame,
  re-points every nested canvas at the overlay camera, and orders hits by `sortingOrder`
  then depth.
- **Do not add a second Y flip.** That same flipped up-vector already inverts
  `ScreenPointToRay`'s Y, so layer-fraction `y = 0` (top) maps to `screenY = 0`. Flipping
  again mirrors your cursor about the panel's midline, which presents as "clicks land on the
  wrong control" rather than as an obvious error.
- **Read the cursor from the right place.** In a built player, and in the editor's default
  weave-to-texture path (v2.8.0+), the output is inside Unity's own window and the Input
  System tracks the cursor normally. Under `DISPLAYXR_PROV_EXTERNAL_WINDOW=1` the output is
  a separate window Unity's input never sees, and the cursor must come from the overlay's
  own WndProc tracker (`displayxr_get_overlay_pointer`). Branch on
  `DisplayXRProviderDriver.GameViewTextureModeEnabled()`, **not** on `UNITY_EDITOR` — the
  latter has been wrong since v2.8.0 and the failure is silent.
- **`pressEventCamera` is read-only** in Unity 6's UGUI; it derives from
  `pointerPressRaycast.module`. Wiring the raycast results is what makes `Slider.OnDrag`'s
  `ScreenPointToLocalPointInRectangle` project correctly.
- **The EventSystem needs no input module,** and on Input System Package projects a
  `StandaloneInputModule` throws every frame trying to read legacy `UnityEngine.Input`.

### Coordinating with scene input

Set `IsCursorOverInteractive` while the cursor is over an actual UI graphic (or a press is
held), and check it in your camera controller:

```csharp
if (DisplayXRWindowSpaceUI.IsCursorOverInteractive) return;  // UI owns the mouse
```

Without this a slider drag also rotates your scene. `Samples~/DefaultInputController`
already does it. Set the flag from a raycast **hit**, not from "inside the layer rect": the
recommended 2D-scene recipe is a full-rect wsui, and the rect test would then block scene
input over the entire window.

## See also

- `Samples~/WindowSpaceUI` — the shipped mouse router
- [`two-dimensional-scenes.md`](two-dimensional-scenes.md) — making a *whole scene* 2D
- `Runtime/DisplayXRLocal2D.cs` — a flat 2D band *inside* the 3D scene
