# Window-Space UI Mouse Router

Makes the UI rendered by `DisplayXRWindowSpaceUI` **clickable**.

## Why you need it

`DisplayXRWindowSpaceUI` submits your Canvas to the runtime as an
`XrCompositionLayerWindowSpaceDXR` composition layer. That layer carries **pixels, not
input**. To render it, the component takes over your Canvas: switches it to `WorldSpace`,
parks it at world `(0, 100000, 0)` on a private layer, and renders it with a hidden
offscreen camera into a RenderTexture.

After that, the mouse position in your app window and the overlay camera's screen space are
two different coordinate systems. `GraphicRaycaster` never hits anything, so **buttons and
sliders are dead out of the box** — the panel renders perfectly and simply does not respond.

This router bridges the two coordinate systems.

## Usage

1. Import this sample (Package Manager ▸ DisplayXR ▸ Samples ▸ Window Space UI Mouse Router).
2. Add `DisplayXRWsuiMouseRouter` to the same GameObject as your `DisplayXRWindowSpaceUI`,
   or to any GameObject above it in the hierarchy.
3. If the automatic `GetComponentInChildren` lookup can't find your wsui, assign the
   **Window Space UI** field explicitly.

That's it. Buttons, toggles, sliders and drags work.

## What it does

1. Reads the cursor in **fractional window coordinates**.
2. Hit-tests the wsui layer's fractional rect (`positionX/Y`, `width`, `height`).
3. Maps the hit to **canvas-pixel coordinates** inside the `OverlayTexture`.
4. Synthesizes `PointerEventData` and dispatches click/drag events via `ExecuteEvents`.

## Things that will bite you if you fork it

- **`ignoreReversedGraphics` must be `false`.** The wsui's overlay camera uses
  `up = Vector3.down` to Y-flip the RT (the runtime's texture origin is top-left), which
  makes `Dot(camera.forward, canvas.forward) == -1`. `GraphicRaycaster` reads that as "the
  back of the graphic faces the camera" and silently skips **every** hit.
- **No extra Y flip in the router.** That same flipped up-vector already inverts
  `ScreenPointToRay`'s Y, so layer-fraction `y = 0` (top) maps to `screenY = 0`. Flipping
  again in the router puts your cursor exactly upside-down about the panel's midline —
  which looks like "clicks land on the wrong control" rather than an obvious error.
- **Where the cursor comes from depends on where the woven output is.** In a built player,
  and in the editor's default weave-to-texture path (v2.8.0+), the output is inside Unity's
  own window, so the Input System tracks the cursor normally. Under
  `DISPLAYXR_PROV_EXTERNAL_WINDOW=1` the output is a separate window Unity's input never
  sees, and the cursor must come from the overlay's own WndProc tracker. The router
  branches on `DisplayXRProviderDriver.GameViewTextureModeEnabled()` for exactly this
  reason — branching on `UNITY_EDITOR` alone is wrong since v2.8.0, and the failure is
  silent (the panel just never responds).
- **`pressEventCamera` is read-only** in Unity 6's UGUI: it derives from
  `pointerPressRaycast.module`. Wiring the raycast results is what makes `Slider.OnDrag`'s
  `ScreenPointToLocalPointInRectangle` project correctly.
- **The EventSystem needs no input module** — and on Input System Package projects a
  `StandaloneInputModule` throws every frame. The router strips it.

## Coordinating with your own input

While the cursor is inside the panel the router sets
`DisplayXRWindowSpaceUI.IsCursorOverInteractive = true`. Check that in your camera
controller and skip mouse handling when it's set, or a slider drag will also rotate your
scene. `Samples~/DefaultInputController` already does this.

## Why this is a sample, not a plugin component

Input policy is app-owned — the plugin ships the hooks (the overlay camera wired as the
canvas's event camera, and the `IsCursorOverInteractive` flag) and leaves the policy to you.
Same reasoning as `Samples~/DefaultInputController`. Fork freely: if your input model isn't
a mouse, steps 1 and 4 are the only parts you need to replace.
