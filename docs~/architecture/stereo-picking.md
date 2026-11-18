# Stereo-correct picking (`DisplayXRPicking`, #460)

`Camera.ScreenPointToRay` does not return the ray the viewer is pointing along on a DisplayXR
display. `DisplayXRPicking` does. This page explains why the stock ray misses, what the API
returns, and which pixel space it works in.

```csharp
if (DisplayXRPicking.TryGetPointerPosition(out Vector2 pointer) &&
    DisplayXRPicking.TryScreenPointToRay(pointer, out Ray ray) &&
    Physics.Raycast(ray, out RaycastHit hit))
{
    // hit is what the viewer sees under the cursor
}
```

For ready-made pointer events on colliders and world-space canvases, import the **Stereo
Picking (Pointer Events)** sample (`Samples~/StereoPicking`).

## Why the stock ray misses

The picture is drawn with two off-axis frusta. Each has its apex at one of the viewer's eyes and
passes through the four edges of the display (or of the 3D zone). The runtime computes them
every frame from the tracked eye positions (`XR_DXR_view_rig`), and the provider hands Unity the
resulting eye poses and full projection matrices.

`Camera.ScreenPointToRay` uses neither. It uses `Camera.projectionMatrix`, a symmetric frustum
built from the camera's field of view, and the camera transform. That is one fixed apex, and it
is not where the viewer's eyes are. It is not the cyclopean view either: the midpoint eye's
frustum is also off-axis and also moves with the head.

With a display-centric rig the mismatch is as large as it can be. The rig camera's transform
**is the display plane** (the provider sends `camera.transform` to the runtime as the display
pose, and `DisplayXRDisplay` ignores the camera's field of view). So the stock ray starts at the
centre of the display, not at the viewer, and fans out at the camera's own field of view. It
agrees with the true ray only along the camera's forward axis, that is, at the centre of the
screen. Everywhere else it is wrong, **including for content that lies on the display plane**:
those points are at zero distance in front of the stock camera, which cannot place them at all.

Measured in built players (URP and BiRP, D3D12): the stock `Camera.ScreenPointToRay` misses
off-centre points on the display plane by 0.34 to 0.47 world units, and
`Camera.WorldToScreenPoint` is off by 24 to several thousand pixels. `DisplayXRPicking` in the
same players: world -> screen -> ray misses by less than 5e-6, and the drawn position of a
sphere matches `TryWorldToScreenPoint` per eye within 0.22 pixel.

The rest of this section is about the correct views, and why even a ray built from the right
kind of projection needs the eye positions.

Take a display plane at distance `d` from the viewer and a point `z` behind it (`z < 0`: in front
of it). Seen from an eye at lateral position `e`, a point at lateral position `x` appears on the
panel at

```
x_panel = e + (x - e) * d / (d + z)
```

Three things follow.

- **On the display plane (`z = 0`) every eye agrees**: `x_panel = x`. The left, right and
  cyclopean views put an on-plane point on the same pixel, wherever the head is. This is a
  statement about views whose frustum passes through the display's edges. It says nothing for
  the stock camera, whose frustum does not (see above).
- **Off the plane, the image moves with the eye** by `z / (d + z)` of the eye's movement. With
  the viewer 60 cm from the panel and an object 30 cm behind it, a head 15 cm off-centre moves
  the object's image 5 cm across the panel. A ray that does not know where the head is misses by
  that much. The error grows with `|z|` and with the head offset.
- **The two eyes disagree** by `ipd * z / (d + z)` (about 2 cm on the panel in the same
  example). That is the on-screen disparity. Neither eye's image is where the viewer perceives
  the object: the fused object is seen midway between them.

`Tests~/PickingMath` checks all three against the geometry (`TestDisplayPlaneAndDisparity`).

## The API

All methods are static on `DisplayXRPicking`, return `false` when there is nothing to pick
through (no session, no stereo views located yet, no native plug-in) and never throw or log.
They allocate nothing and are meant to be called every frame, from the main thread.

| Method | Returns |
|---|---|
| `TryScreenPointToRay(screenPoint, out ray)` | The cyclopean world ray through a screen point, for the active rig camera. The origin is on the near plane. |
| `TryScreenPointToRay(screenPoint, camera, out ray)` | The same for a given camera. |
| `TryScreenPointToRay(screenPoint, camera, eye, out ray)` | One eye's ray (`Left` / `Right`), or the cyclopean one (`Mono`). |
| `TryWorldToScreenPoint(world, out screenPoint)` | Where the viewer sees a world point: x, y in screen pixels, z = signed distance in front of the viewer. |
| `TryWorldToScreenPoint(world, camera, eye, out screenPoint)` | The same per eye. |
| `TryScreenToWorldPoint(screenPoint, distance, out world)` | The world point at a screen point, `distance` in front of the viewer. |
| `TryScreenToWorldPoint(screenPoint, distance, camera, eye, out world)` | The same per eye. |
| `TryGetPointerPosition(out screenPoint)` | The pointer in the picking pixel space (below). |
| `TryGetScreenSize(out size)` | The size of the picking pixel space. |
| `IsAvailable` | True when the methods above can answer. |

Each of the three conversions also has an overload with a trailing `int zone` for multi-zone
apps (see [Zones](#zones)). `eye` is Unity's own `Camera.MonoOrStereoscopicEye`.

`TryWorldToScreenPoint` returns `false` for a point at or behind the viewer's eye plane, which
has no position on the screen. `screenPoint.z` is written even then (it is negative), so the
caller can tell "behind the viewer" from "not running" (`z = 0`). A point in front of the viewer
but off the screen returns `true` with x, y outside the screen, like
`Camera.WorldToScreenPoint`.

`distance` and `screenPoint.z` are measured along the view axis (perpendicular to the display),
not along the ray. This is the convention of `Camera.ScreenToWorldPoint`, and it makes the two
methods inverses of each other.

## Cyclopean or per-eye

The default is the **cyclopean** view: the eye midway between the two, with the frustum that eye
would have. A fused object is perceived along that view, so a ray through it lands on the object
where the viewer sees it. A cursor drawn on the panel sits at zero disparity, and the cyclopean
ray is the one that passes from the midpoint of the eyes through that cursor.

The cyclopean matrices are the left view with its translation averaged with the right one's,
and the element-wise mean of the two projections. That is exactly the midpoint eye's own
off-axis frustum, for any two eye positions, a turned or tilted head included. Every term of a
projection that frames one fixed rectangle is linear in the eye position, so the mean of the two
projections is the projection of the mean position (`TestCyclopeanIsTheMidpointEye`,
`TestCyclopeanWithATurnedHead`).

Use a **per-eye** ray or screen point when the question is about one eye's image:

- the on-screen disparity of a point: `right.x - left.x` from two `TryWorldToScreenPoint` calls.
  It is zero on the display plane, positive behind it and negative in front of it;
- a mask or outline that must cover what either eye shows. `DisplayXRTransparentOverlay` builds
  its click-through silhouette from both eyes for this reason and hit-tests with the cyclopean
  ray;
- anything drawn per eye that must line up with that eye's picture.

A per-eye ray is not a better pointer. It lands half a disparity to one side of the fused object.

## Pixel space

`screenPoint` is in **Unity screen pixels, origin bottom-left**: what `Input.mousePosition` and
`Mouse.current.position` report. It is measured against `TryGetScreenSize`, which is the window
the 3D picture is shown in.

| Configuration | Picking screen | Pointer |
|---|---|---|
| Built player, opaque window (all platforms) | `Screen.width` x `Screen.height` | Unity's mouse |
| Editor, docked or floating Game view (default) | the Game view (`Screen.*` in Play Mode) | Unity's mouse |
| macOS / Linux transparent overlay | `Screen.*`: Unity's window is the visible one | Unity's mouse |
| Android | `Screen.*` | none from `TryGetPointerPosition` unless a mouse is attached (it reads the mouse, not touches). Read the touch yourself: its position is in the same pixels |
| **Windows transparent overlay** (`DisplayXRTransparentOverlay`, overlay mode) | the **provider's overlay window** | the cursor over that window |
| **Windows editor with `DISPLAYXR_PROV_EXTERNAL_WINDOW=1`** | the provider's output window | the cursor over that window |

The two bold rows are the trap. Unity's own window is hidden there, cloaked and parked
off-screen at whatever size Unity last saved, so `Screen.*` describes a window nobody sees and
Unity's mouse position does not follow the cursor. Code that uses `TryGetPointerPosition` and
`TryGetScreenSize` instead of `Input.mousePosition` and `Screen.*` is right in every row.

`DisplayXRTransparentOverlay` already injects its polled pointer into the Input System's
`Mouse.current` in overlay mode, in the overlay window's pixels. So `Mouse.current.position` is
also in the picking space there, although `Screen.*` is not its size.

No Win32 geometry call was added for this. The overlay window's size and pointer come from the
existing native exports (`displayxr_get_overlay_size`, `displayxr_get_overlay_pointer`). Those
are not DPI-pinned: they call `GetClientRect` and `GetCursorPos` + `ScreenToClient` on the
calling (managed) thread, so they answer in Unity's DPI awareness. That is safe here for one
reason only: both numbers come from the same thread and the same window, and the only thing done
with them is to divide one by the other. Do not combine either with a number from the pinned
native side (see the DPI rules in `CLAUDE.md`).

### Zones

When the app sets a 3D zone (`DisplayXRProvider.SetZoneRect`), the located views frame the
**zone**, not the window. A screen point is then mapped into the zone rect before it is
unprojected. A point outside the zone still maps to a ray, through the extension of the frustum,
although nothing is drawn there. This is the rule the depth-aware cursor applies natively
(`dxr_cursor::screen_to_canvas_uv`): an app-authored zone that is live frames its rect, and
anything else frames the whole screen. "Anything else" is no zone, a cleared zone, the zone the
provider makes for the editor Game view, and a runtime without `XR_DXR_display_zones`.

Whether that holds, and for which rect, is the provider's answer, not a managed copy of what the
app asked for: `dxr_prov_get_app_zone_rect_px` returns the app's rect only while an app zone is
live. A managed copy cannot follow the native state. `ClearZone` clears the live zone but the
provider keeps the app's rect and re-applies it when the session restarts.

The zone rect is in client pixels with a top-left origin. On a scaled native-Wayland output
Unity's screen and pointer are logical pixels while the rect is device pixels, and the mapping
scales by `displayxr_linux_ui_scale` as the overlay does. Elsewhere the two units are the same.

The export is new with this API. A native library built before it (a platform binary that has
not been rebuilt yet) lacks it. The managed side then treats that as "the provider cannot say",
keeps working, and uses the rect an app publishes with `displayxr_set_canvas_rect`, the one the
overlay's own hit-test maps the cursor into. Each optional native entry point has its own
"missing" latch, so a missing one never disables the primary views or the overlay's hit-test.

A camera's own `Camera.rect` sub-viewport is not applied. The mapping is always against the
whole picking screen, or the zone.

In a multi-zone app each zone has its own views and its own rect. Pass the zone index (0 =
primary, i >= 1 = the zone set with `DisplayXRProvider.SetZone(i, ...)`) to the `zone`
overloads, for the zone the point lies in.

## Cameras and rigs

The views are located by the runtime with the rig pose the provider last sent chained in. Unity
renders every camera with those same views, composed as
`camera.transform * (rigPose^-1 * view)` (see `set_eye_pose_rig_relative` in the native
provider). So the views sit at `camera.transform * rigPose^-1`, and the API places them there
before answering (`DisplayXRPickingMath.FrameFromPoses`, `TestFrameFromPoses`):

- for the active rig camera, standing where it was when its rig pushed, that is the identity.
  This is the default, and the case `DisplayXRTransparentOverlay` has always used;
- for any other camera, it is that camera's pose relative to the rig's;
- for a camera without a rig, the rig pose is the identity and the views sit at the camera;
- for a rig camera that moved after its rig stopped pushing (the component was disabled, or the
  splash is up), the runtime still has the last pose it was sent, and the views move with the
  camera. This is why there is no "it is the rig camera, so identity" shortcut.

The rig pose is forgotten when the session stops and at the start of every Play, so it never
leaks from one Play into the next with domain reload disabled. With no camera to place the views
by, the methods return `false`.

Unity's side of this, that the device anchor it composes the eye poses with is the camera's own
world transform, is taken from the native provider's comments and has not been measured for a
second camera (see [Checking it by hand](#checking-it-by-hand)).

## What it consumes

Nothing here computes a projection. `DisplayXRPicking` reads the per-eye view and projection
matrices the provider publishes each frame (`displayxr_get_stereo_matrices`, and
`dxr_prov_get_zone_stereo_matrices` for extra zones), the ones it hands Unity to render with.
`DisplayXRPickingMath` inverts and multiplies them. The off-axis math stays in the runtime.

The matrices are those of the **last rendered frame**. A ray built in `Update` is one frame
behind the head, the same lag as the depth-aware cursor's footprint rays.

Only the x and y rows of a projection decide where a point lands on the screen, and those are
the same in every graphics API. The reversed-Z remap of D3D, Vulkan and Metal touches the z row
only. So picking has no per-API case, unlike the depth-aware cursor's depth unprojection
(`TestGraphicsApiIndependent`).

## One implementation

`DisplayXRTransparentOverlay`'s hit-test used to carry this math privately. It now calls the
same code: `DisplayXRPickingMath` for the matrices, entered below the pixel mapping, because the
overlay works in its own window's client pixels and knows its canvas sub-rect.

| File | Role |
|---|---|
| `Runtime/DisplayXRPickingMath.cs` | Pure math on `float[16]`, no UnityEngine types, no allocation. Unit-tested. |
| `Runtime/DisplayXRPicking.cs` | The public API: fetches the views, resolves the pixel space and the camera frame. |
| `Runtime/DisplayXRTransparentOverlay.cs` | Calls the shared math for its cyclopean hit-test and screen rect. |
| `Samples~/StereoPicking/DisplayXRPointerEvents.cs` | Sample: pointer events on colliders and world-space canvases. |
| `Tests~/PickingMath` | `dotnet run --project Tests~/PickingMath -c Release`, run in CI. |

## Limits

- **2D display mode.** The provider publishes the matrices only while it locates a stereo pair.
  After a switch to 2D the last stereo pair stays in place, so the cyclopean ray is the one of
  the head position at the time of the switch. For a 2D scene the on-plane content is still hit
  correctly (every view agrees on the display plane).
- **Workspace (shell) tile.** Unity's window is minimised and the runtime composites the app
  into a tile. The pixel mapping above does not cover that case. Pointer input there comes from
  the shell.
- **`Camera.rect`.** A camera rendering into a sub-viewport is mapped against the whole screen.
- **More than two views.** The first two views of a multi-view mode are what is published.
- **First frames of a session.** The provider learns whether the runtime supports zones on its
  first zone frame. Until then a zoned app is mapped against the whole screen.

## Checking it by hand

On the simulated display (no 3D panel needed, the runtime falls back to it):

1. Open a scene with a `DisplayXRDisplay` rig and a few cubes with colliders: one on the display
   plane (at the rig camera's position along its forward axis), one about a display height
   behind it and one in front of it. Add `DisplayXRPointerEvents` and a script that tints the
   object passed to `onEnter` / `onExit`.
2. Press Play. Move the cursor over the edges of each cube. The tint must switch exactly at the
   drawn silhouette for all three cubes, measured against the midpoint of the two eye images
   where they are drawn apart.
3. For comparison, raycast `Camera.main.ScreenPointToRay(Input.mousePosition)` in a debug
   script and log what it hits. It is wrong for all three cubes, the on-plane one included:
   it agrees with the stereo ray only at the centre of the screen. In built URP and BiRP D3D12
   players it missed off-centre on-plane points by 0.34 to 0.47 world units. Do not use "the
   stock ray is right on the plane" as a sanity check. To check the plane property itself,
   compare the `Left`, `Right` and `Mono` results of `TryWorldToScreenPoint` for the on-plane
   cube: they must be the same pixel.
4. Draw `Debug.DrawRay` from both `TryScreenPointToRay(..., Left, ...)` and `(..., Right, ...)`.
   In the Scene view they start next to the two eye positions (on each eye's near plane) and
   cross on the display plane under the cursor.
5. The per-camera overload. Add a second camera with no rig, parented under a moved, rotated and
   scaled object. Compare `secondCamera.GetStereoViewMatrix(Left)` with the view the frame
   implies: the eye position from `TryScreenPointToRay(p, secondCamera, Left, out ray)` (the
   point all its rays converge on) must be the translation of the inverse of that matrix. This
   is the one assumption about Unity that nothing here has measured. Parent scale is the case
   most likely to differ: the frame is rigid and ignores it.
6. Zones. Call `SetZoneRect`, pick inside the zone, then `ClearZone` and pick again. Restart the
   session (dock and undock the Game view) and pick once more. The ray must follow what is drawn
   each time.
