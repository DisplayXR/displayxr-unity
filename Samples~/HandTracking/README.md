# Hand Tracking (XR Hands)

Shows the DisplayXR plugin's **XR Hands subsystem**: the DisplayXR runtime's 26-joint
`XR_EXT_hand_tracking` (served by an input-provider plug-in with hand roles — an
Ultraleap / Leap Motion device today) surfaced through Unity's standard
`com.unity.xr.hands` API. The sample draws each tracked hand as joint spheres plus
finger bones, placed in front of the panel where your real hands are.

## Requirements

- `com.unity.xr.hands` **1.3.0 or newer** in the project (Package Manager → Unity
  Registry → *XR Hands*). The plugin's hand subsystem and this sample are
  **version-defined on that package**: without it they compile out and the plugin
  stays display-only.
- A DisplayXR runtime whose `displayxr-cli selftest` / `info` lists an input provider
  with hand tracking (Ultraleap). On a runtime without one the subsystem still runs but
  both hands stay untracked — the sample's status line says so.
- **DisplayXR Display** enabled in Project Settings → XR Plug-in Management. The hand
  subsystem is created, started and stopped alongside it; nothing else to enable.

## How to use

1. Package Manager → DisplayXR → Samples → **Hand Tracking (XR Hands)** → Import.
2. Open any DisplayXR scene (e.g. the *Basic Scene* sample) and add
   `DisplayXRHandJointGizmos` to an empty GameObject.
3. Press Play with the tracker connected. Hold your hands above the sensor: the status
   line reports `left TRACKED / right TRACKED` and the joints appear in front of the
   display. The native log (`%TEMP%\displayxr_prov_native.log` / Player.log) shows
   `XR_EXT_hand_tracking: AVAILABLE`, `hand tracking: AVAILABLE (... supportsHandTracking=1 ...)`,
   `left tracker created`, then `left hand first seen (wrist LOCAL x,y,z m)`.

## Where the hands land — the rig camera is the origin

The runtime locates joints in the same reference space it locates the eyes in: relative to
the **display plane**, in metres. On the Unity side the display plane is the **rig camera's
transform** (the GameObject carrying `DisplayXRDisplay` or `DisplayXRCamera` — the pose the
provider sends the runtime). So joint poses from `XRHandSubsystem` are **relative to that
transform**, already scaled to the rig's world units (virtual display height vs. the
physical panel; see `DisplayXRHandTracking.ResolvePoseScale`).

This sample therefore transforms every pose through `DisplayXRRigManager.ActiveCamera`:

```csharp
Transform rig = DisplayXRRigManager.ActiveCamera.transform;
if (joint.TryGetPose(out Pose p))
{
    world.position = rig.TransformPoint(p.position);
    world.rotation = rig.rotation * p.rotation;
}
```

Moving, rotating or scaling the rig (WASD, scale-as-zoom) carries the hands with it, which
is exactly what the eyes do.

## Skinned hand meshes (`XRHandSkeletonDriver` / `XRHandMeshController`)

The XR Hands package ships a **HandVisualizer** sample with rigged left/right hand prefabs
driven by `XRHandTrackingEvents` + `XRHandSkeletonDriver` + `XRHandMeshController`. They
work unchanged against this subsystem (`XRHandTrackingEvents` finds the running
`XRHandSubsystem` by itself) with one placement rule:

- Parent the prefab — or whatever `XRHandSkeletonDriver.rootTransform`'s parent is — under
  the **rig camera**, so its local root pose (the wrist) is interpreted relative to the
  display plane. If your project uses an `XROrigin`, make the rig camera its camera and put
  the hand prefabs under the camera, not under the origin's floor offset.
- Do not add a `TrackedPoseDriver` to the rig camera: the provider already positions the
  per-eye views; the camera transform stays the app's.

## Notes and limits

- No controller / pinch / aim input yet: this is `XR_EXT_hand_tracking` joints only. XR
  Hands' *common pose* and gesture helpers that derive from joints
  (`XRHandTrackingEvents.jointsUpdated`, `XRFingerShape`) work; provider-supplied aim /
  pinch poses (`canSurfaceCommonPoseData`) are not implemented. Controller-style input
  (`IUnityXRInput`) is tracked as a follow-up issue in the plugin repo.
- Joints are located at the predicted display time of the frame being rendered (the same
  time as the views). Velocities are not reported.
- Hand tracking is Windows-first (the runtime's Ultraleap provider is Windows today); the
  subsystem compiles everywhere and simply reports *unsupported* elsewhere.
