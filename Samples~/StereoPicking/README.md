# Stereo Picking (Pointer Events)

Makes 3D objects and world-space uGUI canvases **clickable where the viewer sees them**.

## Why you need it

Unity's pointer path (`PhysicsRaycaster`, a `GraphicRaycaster` on a world-space canvas,
`OnMouseDown`) builds its ray with `Camera.ScreenPointToRay`. Under DisplayXR the picture is
drawn with two off-axis frusta that the runtime computes from the tracked eye positions. The
camera's own projection and transform know nothing about them, so that ray misses what the
viewer sees: a button floating behind the display highlights when the cursor is beside it, and
the error changes as the head moves. On the display plane the error is zero, which is why a
scene can look fine until something is placed in depth.

`DisplayXRPicking` (in the plugin) gives the ray through the views that are actually rendered.
`DisplayXRPointerEvents` (this sample) uses it to deliver ordinary pointer events.

## Usage

1. Import this sample (Package Manager ▸ DisplayXR ▸ Samples ▸ Stereo Picking).
2. Add `DisplayXRPointerEvents` to any GameObject in the scene.
3. Press Play.

With the default settings it points through the active rig camera at every collider and every
world-space canvas in the scene:

- **3D objects** need a collider. Any `IPointerEnterHandler`, `IPointerExitHandler`,
  `IPointerDownHandler`, `IPointerUpHandler`, `IPointerClickHandler`, `IDragHandler` or
  `IScrollHandler` on the object or its parents receives the event. Nothing else is needed on
  the camera. Remove a `PhysicsRaycaster` if the scene has one, or the EventSystem's input
  module sends the same events a second time with Unity's ray.
- **World-space canvases** work as they are: `Button`, `Toggle`, `Slider`, `Scrollbar`,
  `ScrollRect`, `Dropdown`, input fields, masks and `CanvasGroup`s.
- **Without handlers**, wire the component's `onEnter`, `onExit`, `onDown`, `onUp` and
  `onClick` UnityEvents. Each passes the GameObject under the pointer. From script, read
  `Hovered`, `HasHit`, `HitPoint`, `HitDistance` and `PointerRay`.

Screen-space canvases are not touched. They are 2D, Unity's input module already handles them
correctly. For a canvas shown through `DisplayXRWindowSpaceUI`, use the **Window Space UI Mouse
Router** sample. While that HUD has the pointer, this component stays out of the way.

## Settings

| Field | Default | Meaning |
|---|---|---|
| `pickingCamera` | empty | The camera being pointed at. Empty = `DisplayXRRigManager.ActiveCamera`, else `Camera.main`. |
| `fallBackToCameraRay` | on | With no DisplayXR session, use `Camera.ScreenPointToRay` so the scene still responds. `RayIsStereoCorrect` reports which ray was used. |
| `blockedByScreenSpaceUI` | on | Do not pick while `EventSystem.IsPointerOverGameObject()` is true, so a screen-space canvas on top of the scene blocks the pick. See "Next to Unity's input module". |
| `raycastPhysics`, `physicsLayers`, `maxDistance`, `triggerInteraction` | on, default layers, 1000, ignore | The `Physics.Raycast` parameters. |
| `raycastWorldCanvases` | on | Hit-test world-space canvas graphics. |
| `worldCanvases` | empty | The canvases to drive. Empty = every world-space canvas in the loaded scenes, inactive ones included, collected on enable and on every scene load. Call `RefreshCanvases()` after instantiating one at runtime. Canvases under `DontDestroyOnLoad` are not found automatically: list them here. |
| `takeOverCanvasRaycasters` | on | Disable each driven canvas's `GraphicRaycaster` while the component is enabled (re-enabled on disable). Leave on: see below. |
| `ignoreReversedGraphics` | on | Skip graphics seen from behind. |

## How the canvases are driven

1. The stereo ray is intersected with each graphic's own plane, in world space, and tested
   against its rect. Nothing in this step uses the camera.
2. The hit point is given to uGUI as the screen position at which the canvas's **own event
   camera** sees that world point (`eventCamera.WorldToScreenPoint`). Everything in uGUI that
   maps a pointer back into the canvas (masks, alpha hit-testing, a `Slider` or `ScrollRect`
   following a drag) keeps using that camera and lands on the same world point.
3. The events go out through `ExecuteEvents` in the order `StandaloneInputModule` uses.

This is why `takeOverCanvasRaycasters` matters. An enabled `GraphicRaycaster` is registered with
the EventSystem, and the project's input module then raycasts the canvas with the raw mouse
position and Unity's ray: a second pointer, in the wrong place. The component switches the
raycaster off and keeps it only as the event's raycast module, which is where uGUI controls look
up their camera.

A canvas with no `GraphicRaycaster` gets one for that purpose. It is added disabled, stays
disabled whatever `takeOverCanvasRaycasters` says, and is removed again when the component is
disabled, so every canvas ends as it started.

Set the canvas's **Event Camera**. Without one (and without a `MainCamera`-tagged camera) hover
and click still work through the picking camera, and a warning is logged once, but a `Slider` or
`ScrollRect` has no camera to follow a drag with.

Hover follows Unity's rule: every object from the hit up to the root is entered, and moving
between two children of one handler does not exit and re-enter it. A click goes to the click
handler that was under the press if it is also the one under the release.

## Next to Unity's input module

The component does not replace a `StandaloneInputModule` or `InputSystemUIInputModule`. Screen-
space canvases stay theirs. Two things follow from running both.

- **Screen-space UI on top of the scene.** With `blockedByScreenSpaceUI` on, the component does
  not pick while the EventSystem reports the pointer over one of its objects, so a HUD button
  does not also click the 3D object behind it. This needs an input module on the EventSystem.
  Turn the option off if that module also raycasts the 3D scene (a `PhysicsRaycaster`, or
  `takeOverCanvasRaycasters` off), or the scene blocks itself. With it off, 3D picks are not
  blocked by screen-space UI.
- **Selection.** Unity's module sees a press on a world-space control as a press on nothing (it
  cannot hit that canvas any more) and deselects. This component then selects the control. A
  focused world-space `InputField` therefore gets `OnDeselect` followed by `OnSelect` on each
  click on itself. It keeps the focus, but code that reacts to `OnDeselect` (end-edit handlers)
  runs.

Step 2 assumes that the event camera's `WorldToScreenPoint` and `ScreenPointToRay` are inverses
of each other, which they are for any one camera.

## Input

- **Position.** `DisplayXRPicking.TryGetPointerPosition`: the mouse through whichever backend
  the project enabled, or the cursor over the provider's own window where Unity's is hidden (a
  Windows transparent-overlay app). The first touch replaces it while a finger is down.
- **Button and wheel.** The Input System package when it is enabled (`ENABLE_INPUT_SYSTEM`),
  else the legacy Input Manager. With both enabled the Input System is used.
- One pointer, left button. For another device, replace `TryGetPointer`. To point with a
  controller or stylus, skip the screen step and use its aim ray (see the Stylus Input sample).

## Limits

- Planar graphics only: a graphic is hit on its own rect plane. A bent or curved UI mesh is not
  followed.
- In a Windows transparent-overlay app (Unity's window hidden behind the overlay):
  - with only the legacy Input Manager, the position is right but the button is not seen (legacy
    input is dead there). Use the Input System, or `DisplayXRTransparentOverlay`'s own pointer
    events;
  - the scroll wheel never arrives: the mouse state the overlay injects into the Input System
    carries position and buttons, not scroll. Use `DisplayXRTransparentOverlay.ConsumeWheelDelta`;
  - touch is not supported: a touch position is in the hidden window's pixels, not the overlay
    window's, so the ray is wrong.
- `Camera.rect` sub-viewports are not applied (see the picking doc).
- One zone: the ray is taken through the primary 3D zone. A multi-zone app picks the zone
  itself and calls the `DisplayXRPicking` overloads that take a zone index.
