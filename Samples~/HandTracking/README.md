# Hand Tracking (XR Hands)

Shows the DisplayXR plugin's **XR Hands subsystem**: the DisplayXR runtime's 26-joint
`XR_EXT_hand_tracking` (served by an input-provider plug-in with hand roles — an
Ultraleap / Leap Motion device today) surfaced through Unity's standard
`com.unity.xr.hands` API. The sample renders each tracked hand as a transparent
**Ghost Hand** — Ultraleap's GenericHand model with its HDR fresnel rim and hairline
outline (Apache-2.0, see `THIRD-PARTY-NOTICES.md`) — placed in front of the panel where
your real hands are, with a joint-sphere debug visual as the fallback.

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
   `DisplayXRGhostHand` to an empty GameObject (`Visual` = *Ghost Hand*; switch to
   *Joint Spheres* for the debug visual, which is also what HDRP projects get).
3. Press Play with the tracker connected. Hold your hands above the sensor: the status
   line reports `left TRACKED / right TRACKED` and the joints appear in front of the
   display. The native log (`%TEMP%\displayxr_prov_native.log` / Player.log) shows
   `XR_EXT_hand_tracking: AVAILABLE`, `hand tracking: AVAILABLE (... supportsHandTracking=1 ...)`,
   `left tracker created`, then `left hand first seen (wrist LOCAL x,y,z m)`.

## The Ghost Hand

Two components ship:

| Component | What it shows | Pipelines |
|---|---|---|
| `DisplayXRGhostHand` (default) | Ultraleap's **GenericHand** skinned model with the Ghost Hand look: transparent white, HDR fresnel rim (`_FresnelColor` 22.4× white @ α 0.098, `_FresnelPower` 0.629), white hairline outline, unlit, ZWrite on with a depth-prime pass so the hand self-occludes. | Built-in (`GhostHand.shader`, CG) in this sample; **URP** (`GhostHandURP.shader`, hand-written HLSL port — not Shader Graph) in the companion sample *Hand Tracking (URP Ghost Hand)*. Picked by `GraphicsSettings.currentRenderPipeline`; HDRP, and URP without the companion sample, fall back to spheres. |
| `DisplayXRHandJointGizmos` | 26 joint spheres + finger bones. Dependency-free debug visual. | any |

**How the model is driven.** XR Hands' `XRHandSkeletonDriver` writes joint rotations
straight into bone transforms, so it needs a rig whose bones already use the XR Hands
joint frame (+Z along the bone, +Y out of the back of the hand). GenericHand is a Maya
skeleton with bones along −X, which Ultraleap drives through their `HandBinder` with a
constant world-space rotation offset per hand — a component we do not ship. So
`DisplayXRGhostHand` reproduces that binding itself: a 19-row table binds
`XRHandJointID`s to bone **names** once (`L_Wrist`, `L_thumb_meta/a/b`,
`L_<finger>_meta/a/b/c`; tips and the palm are skinned leaves), the rotation offsets are
transcribed from the plugin's `GhostHands.prefab`, and the right hand is the same
left-hand model mirrored in X under the rig — exactly Ultraleap's construction. The
offsets apply unchanged because the DisplayXR runtime passes Leap's bone rotations
through to `XR_EXT_hand_tracking` and the plugin mirrors them into Unity's frame the
same way Ultraleap's SDK does. Like Ultraleap's default, only rotations are driven (the
model keeps its proportions; `setBonePositions` stretches it to the tracked joints) and
the four finger metacarpals stay at rest (`driveMetacarpals` opts in).

Materials: `Resources/DisplayXRGhostHand/Ghost Hand.mat` (Built-in, this sample) /
`Ghost Hand URP.mat` (the URP sample); tweak the fresnel / outline / lighting toggles there,
or assign `materialOverride`.

**URP projects: import both samples.** The URP shader and its material live in a separate
sample, *Hand Tracking (URP Ghost Hand)*, because importing a sample compiles every shader
in it, and the URP shader's `#include "Packages/com.unity.render-pipelines.universal/…"`
cannot resolve in a Built-in or HDRP project — it was a red Console error on import for
every non-URP user. Import it next to this sample (same `Resources/DisplayXRGhostHand/`
name, so `Resources.Load` finds it wherever it lands; its material also uses this sample's
`GenericHandTransparencyMap.png` by GUID). Without it, a URP project logs a warning and uses
the spheres. Don't import it into a non-URP project; if you do, its `PackageRequirements`
block skips the SubShader, so you get one warning, not an include error.

## Where the hands land — the rig camera is the origin

The runtime locates joints in the same reference space it locates the eyes in: relative to
the **display plane**, in metres. On the Unity side the display plane is the **rig camera's
transform** (the GameObject carrying `DisplayXRDisplay` or `DisplayXRCamera` — the pose the
provider sends the runtime). So joint poses from `XRHandSubsystem` are **relative to that
transform**, already scaled to the rig's world units (virtual display height vs. the
physical panel; see `DisplayXRHandTracking.ResolvePoseScale`).

Both visuals therefore go through `DisplayXRRigManager.ActiveCamera` — the Ghost Hand
roots are parented under it (so the rig's scale reaches the bone lengths and the right
hand's mirror plane turns with the rig), the spheres transform through it:

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
- The Ghost Hand ships a Built-in shader here and a URP shader in the companion
  *Hand Tracking (URP Ghost Hand)* sample; HDRP projects get the sphere visual.
- Not yet verified on hardware: the bone binding (mirror, offsets, thumb mapping) is
  transcribed from Ultraleap's prefab and reasoned from the conventions, so the first
  panel run should eyeball a natural rest pose, correct thumb side, and finger curl
  direction on both hands (see the doc page's validation list).
