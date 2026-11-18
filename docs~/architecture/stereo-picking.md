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
is not where the viewer's eyes are.

Take a display plane at distance `d` from the viewer and a point `z` behind it (`z < 0`: in front
of it). Seen from an eye at lateral position `e`, a point at lateral position `x` appears on the
panel at

```
x_panel = e + (x - e) * d / (d + z)
```

Three things follow.

- **On the display plane (`z = 0`) every eye agrees**: `x_panel = x`. A ray built from any
  projection that frames the display hits the right spot there. This is why picking can look
  correct in a scene whose content sits on the plane.
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
and the element-wise mean of the two projections. For two eyes at the same distance from the
display that is exactly the midpoint eye's off-axis frustum (`TestCyclopeanIsTheMidpointEye`).
When the head is turned so that one eye is nearer the panel than the other, it is an
approximation of it.

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
| Android | `Screen.*` | touch, in the same pixels |
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
existing native exports (`displayxr_get_overlay_size`, `displayxr_get_overlay_pointer`), which
run on the per-monitor-DPI-aware native side. Only a point divided by a size from the same
source ever reaches managed code, so there is no DPI space to mix up (see the DPI rules in
`CLAUDE.md`).

### Zones

When the app sets a 3D zone (`DisplayXRProvider.SetZoneRect`), the located views frame the
**zone**, not the window. A screen point is then mapped into the zone rect before it is
unprojected. A point outside the zone still maps to a ray, through the extension of the frustum,
although nothing is drawn there. This is the rule the depth-aware cursor applies natively
(`dxr_cursor::screen_to_canvas_uv`): an app-authored zone frames its rect, and anything else,
including the zone the provider makes for the editor Game view, frames the whole screen.

The zone rect is in client pixels with a top-left origin. On a scaled native-Wayland output
Unity's screen and pointer are logical pixels while the rect is device pixels, and the mapping
scales by `displayxr_linux_ui_scale` as the overlay does. Elsewhere the two units are the same.

An app that sets the zone through the native export directly, without `SetZoneRect`, is covered
when it also publishes the rect with `displayxr_set_canvas_rect`, the rect the overlay's own
hit-test uses.

In a multi-zone app each zone has its own views and its own rect. Pass the zone index (0 =
primary, i >= 1 = the zone set with `DisplayXRProvider.SetZone(i, ...)`) to the `zone`
overloads, for the zone the point lies in.

## Cameras and rigs

The views are located by the runtime with the active rig's pose chained in, so for the camera
whose `DisplayXRDisplay` / `DisplayXRCamera` rig is active they are already in Unity world
space. That is the default, and the case `DisplayXRTransparentOverlay` has always used.

Unity renders every camera with the same located views, composed as
`camera.transform * (rigPose^-1 * view)` (see `set_eye_pose_rig_relative` in the native
provider). So for a camera other than the active rig's, and for a camera without a rig (where
the rig pose is the identity), the `camera` overloads place the views at
`camera.transform * rigPose^-1` before answering. With no rig and no camera to place them by,
the methods return `false`.

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
- **A zone the runtime ignores.** The zone rect is the one the app set. Against a runtime
  without `XR_DXR_display_zones` the views frame the whole window while the mapping still uses
  the rect.
- **More than two views.** The first two views of a multi-view mode are what is published.
- **Turned head.** See [Cyclopean or per-eye](#cyclopean-or-per-eye).

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
   script and log what it hits. The on-plane cube still switches at its edge. The other two
   switch away from their silhouettes, by more the further they are from the plane.
4. Draw `Debug.DrawRay` from both `TryScreenPointToRay(..., Left, ...)` and `(..., Right, ...)`.
   In the Scene view they start next to the two eye positions (on each eye's near plane) and
   cross on the display plane under the cursor.
