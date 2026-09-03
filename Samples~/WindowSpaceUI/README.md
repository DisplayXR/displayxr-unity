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

That's it. Buttons, toggles, sliders, drags, hover highlights and scroll wheels work —
including inside **nested canvases** (a file browser, a modal dialog, a dropdown blocker)
spawned under the wsui at runtime.

## What it does

1. Reads the cursor in **fractional window coordinates**.
2. Hit-tests the wsui layer's fractional rect (`positionX/Y`, `width`, `height`).
3. Maps the hit to **canvas-pixel coordinates** inside the `OverlayTexture`.
4. Raycasts **every** `GraphicRaycaster` under the wsui canvas — not just the root's — and
   orders the hits by `sortingOrder` (override-sorted dialogs win) then graphic depth.
5. Synthesizes `PointerEventData` and dispatches enter/exit, scroll, click and drag events
   via `ExecuteEvents`.

## Things that will bite you if you fork it

- **`ignoreReversedGraphics` must be `false` — on every raycaster.** The wsui's overlay
  camera uses `up = Vector3.down` to Y-flip the RT (the runtime's texture origin is
  top-left), which makes `Dot(camera.forward, canvas.forward) == -1`. `GraphicRaycaster`
  reads that as "the back of the graphic faces the camera" and silently skips **every**
  hit. A nested canvas arrives with Unity's default of `true`, so set it per raycaster,
  not once on the root.
- **Raycast every raycaster, not only the root's.** A nested Canvas carries its own
  `GraphicRaycaster`, and the root raycaster never sees its graphics. Anything that brings
  its own Canvas — most third-party dialogs do — is dead to clicks otherwise, with no error.
- **Nested canvases need the overlay camera as their `worldCamera`.** A child Canvas does
  not inherit the root's `worldCamera`; its `GraphicRaycaster` then falls back to
  `Camera.main` and projects with the wrong camera, so the dialog's graphics are never hit.
  The router points every canvas under the wsui at the overlay camera each frame.
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
- **An `InputSystemUIInputModule` is a second, mirrored pointer.** It raycasts the wsui
  canvas through `worldCamera` — the Y-flipped overlay camera — with raw screen coordinates,
  so its hit lands mirrored about the panel's midline: hover one row, two highlight; clicks
  go intermittent. The router disables it (not destroys — re-enable it yourself if non-wsui
  UI elsewhere needs it). It was invisible while the RT aspect differed from the window's,
  because the mirrored hit fell off-target; at the live panel aspect it lands on real
  controls.

## Coordinating with your own input

While the cursor is over an **actual UI graphic** (or a press begun on one is still held)
the router sets `DisplayXRWindowSpaceUI.IsCursorOverInteractive = true`. Check that in your
camera controller and skip mouse handling when it's set, or a slider drag will also rotate
your scene. `Samples~/DefaultInputController` already does this.

The flag is set from a real raycast hit, **not** from "the cursor is inside the wsui layer
rect". The recommended 2D-scene recipe is a full-rect wsui (position `0,0`, size `1,1`),
and the rect test would then be true over the entire window — every orbit, pan and zoom
in your scene would be blocked while the panel is up. Empty space inside the layer passes
input through to the scene; only graphics claim it.

## Why this is a sample, not a plugin component

Input policy is app-owned — the plugin ships the hooks (the overlay camera wired as the
canvas's event camera, and the `IsCursorOverInteractive` flag) and leaves the policy to you.
Same reasoning as `Samples~/DefaultInputController`. Fork freely: if your input model isn't
a mouse, steps 1 and 5 are the only parts you need to replace.
