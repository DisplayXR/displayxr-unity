# Hand Tracking — the XR Hands subsystem

> **Status: shipping in the next release (branch `feat/xr-hands-subsystem`).** Joints only
> (`XR_EXT_hand_tracking`); controller-style input is a follow-up (see *Limitations*).

## What it is

The DisplayXR runtime serves 26-joint hand tracking through the standard OpenXR
`XR_EXT_hand_tracking` extension, fed by an **input-provider plug-in** with hand roles —
the Ultraleap (Leap Motion) provider today (runtime
[ADR-034](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/adr/ADR-034-input-provider-plugins.md),
#825 Tier 2; discovery in
[`input-provider-discovery.md`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/runtime/input-provider-discovery.md)).
The plugin surfaces that to Unity as an **`XRHandSubsystem`** from the
[`com.unity.xr.hands`](https://docs.unity3d.com/Packages/com.unity.xr.hands@1.5/manual/index.html)
package, so every XR Hands consumer — `XRHandTrackingEvents`, `XRHandSkeletonDriver`,
`XRHandMeshController`, `XRFingerShape`, your own `updatedHands` subscribers — works
unchanged.

We are **not** on `com.unity.xr.openxr`: the provider loads the runtime itself
(`xrNegotiateLoaderRuntimeInterface`) and owns the `XrInstance`/`XrSession`, so Unity's
OpenXR hand-tracking feature cannot be used. The plugin ships its own provider instead,
modelled on it.

## Component map

| Piece | Where | Role |
|---|---|---|
| `displayxr_provider_hands.{h,cpp}` | `native~/displayxr_xrprovider/` | `XR_EXT_hand_tracking` binding: tracker lifetime, locate, frame conversion. Own TU, same shape as the MCP / desktop-control bindings. |
| Session hooks | `displayxr_provider_session.cpp` | Probes + enables the extension, chains `XrSystemHandTrackingPropertiesEXT` on `xrGetSystemProperties`, binds the hands TU after the LOCAL space exists, latches each frame's predicted display time, unbinds before `xrDestroySession`. |
| `DisplayXRCompanionSubsystems` | `Runtime/Provider/` (main assembly) | Lifecycle events the display loader raises (`Initialized`/`Started`/`Stopped`/`Deinitialized`) so a package-gated assembly can ride it without the main assembly referencing it. |
| `com.displayxr.unity.hands` | `Runtime/Hands/` | Package-gated assembly: `defineConstraints: [DISPLAYXR_XR_HANDS]` + a `versionDefines` on `com.unity.xr.hands >= 1.3.0` — the `Runtime/URP/` pattern. Absent package → the assembly is not compiled and nothing changes. |
| `DisplayXRHandSubsystem` / `…Provider` | `Runtime/Hands/` | The `XRHandSubsystem` + `XRHandSubsystemProvider`; registers the descriptor at `SubsystemRegistration`. |
| `DisplayXRHandTracking` | `Runtime/Hands/` | Creates/starts/stops the subsystem from the loader hooks, runs XR Hands' `SubsystemUpdater`, resolves the pose scale, exposes `Enabled` / `IsRunning` / `IsSupported`. |
| `DisplayXRHandNative` | `Runtime/Hands/` | The four P/Invokes. |
| Sample *Hand Tracking (XR Hands)* | `Samples~/HandTracking/` | Joint spheres + bones through the rig camera, status line, README with the skinned-mesh wiring. |

## How it works

```
XR Plug-in Management                      DisplayXR runtime (in-process)
─────────────────────                      ──────────────────────────────
DisplayXRDisplayLoader.Initialize
  ├─ CreateSubsystem(DisplayXR Display)
  └─ CompanionSubsystems.Initialized ──► DisplayXRHandTracking: descriptor.Create()
                                           + new SubsystemUpdater
Loader.Start
  ├─ StartSubsystem (display)
  ├─ driver up
  └─ CompanionSubsystems.Started ──────► subsystem.Start() → DisplayXR_HandTracking_Start
                                           (request remembered; trackers follow the session)
GfxStart (graphics thread)
  └─ dxr_prov_session_start ───────────► xrEnumerateInstanceExtensionProperties: XR_EXT_hand_tracking?
                                          xrCreateInstance(+XR_EXT_hand_tracking if advertised)
                                          dxr_hands_on_instance: resolve xrCreate/Destroy/LocateHandJointsEXT
                                          xrGetSystemProperties(+XrSystemHandTrackingPropertiesEXT)
                                          xrCreateSession, xrCreateReferenceSpace(LOCAL)
                                          dxr_hands_on_session_created → xrCreateHandTrackerEXT ×2
Every frame
  graphics: dxr_prov_begin_frame ──────► xrWaitFrame/xrBeginFrame; latch predictedDisplayTime (atomic)
  main: SubsystemUpdater (EarlyUpdate.XRUpdate, then before render)
        └─ provider.TryUpdateHands ─────► DisplayXR_HandTracking_Locate(hand, time=0) ×2
                                            xrLocateHandJointsEXT(baseSpace = LOCAL, time = latched)
                                            → 26 × {pose (Unity frame), radius, flags}, isActive
Loader.Stop / GfxStop
  ├─ CompanionSubsystems.Stopped ───────► updater.Stop(); subsystem.Stop() → _Stop (trackers destroyed)
  └─ dxr_prov_session_stop ────────────► dxr_hands_on_session_destroying (under the lock) → xrDestroySession
```

### Threading

The session lives on Unity's **graphics thread** (GfxStart/GfxStop, the frame loop); the
XR Hands subsystem updates on the **main thread**. OpenXR already makes
`xrLocateHandJointsEXT` safe against the frame loop; what the hands TU protects is the
**handles**: one mutex orders tracker create / locate / destroy against the session's
teardown, which takes the same lock to destroy the trackers *before* `xrDestroySession`.
A locate in flight therefore completes before the session goes; a locate arriving after
sees no tracker and returns 0. The predicted display time crosses threads through an
atomic written once per begun frame, so the joints are located at the **same time the
views were** — the "current frame" contract.

The app's request (`Start`/`Stop`) outlives the session: trackers are created lazily
once a session is up and re-created after a restart (editor dock/undock), so hand tracking
survives the provider restarts that bit earlier pushed-before-start state (v2.9.0).

### Joint order, frame, flags

- **Order.** OpenXR: palm 0, wrist 1, then per finger metacarpal → proximal →
  intermediate → distal → tip (thumb has no intermediate). XR Hands: wrist, palm, then the
  same sequence. The provider maps by name (`s_OpenXRToUnity`) and indexes with
  `XRHandJointID.ToIndex()`; the hand layout is all 26.
- **Frame.** Native converts OpenXR (right-handed, −Z forward) to Unity (left-handed, +Z
  forward) with the same mirror the eye poses take: position `(x, y, −z)`, quaternion
  `(−x, −y, z, w)` (≡ `(x, y, −z, −w)`). That also maps joint axes: OpenXR's −Z "along the
  bone" becomes Unity's +Z, +Y (back of the hand) stays +Y — XR Hands' convention.
- **Root pose** = the wrist (XR Hands: "located at the wrist joint").
- **Flags.** `XrSpaceLocationFlags` → `XRHandJointTrackingState`: both VALID bits → `Pose`;
  both TRACKED bits as well → `+HighFidelityPose`; radius > 0 → `Radius`. A hand whose
  `XrHandJointLocationsEXT.isActive` is false contributes **no** update flags, which is
  what makes `XRHand.isTracked` false (joints keep their last values, as XR Hands
  specifies).

## Where the hands land: origin and scale

The runtime locates joints in the provider's **LOCAL** reference space — the space the
views are located in — in **metres relative to the physical display**. Unlike the eye
poses, the joints are **not** made rig-relative in native: the located views bake the
app's rig pose in (`XrDisplayRigDXR`/`XrCameraRigDXR` carry it, and the display provider
subtracts it back so Unity does not apply it twice), whereas `xrLocateHandJointsEXT` never
sees that descriptor. LOCAL-space joints are therefore *already* relative to the display
plane — and on the Unity side the display plane **is the rig camera's transform** (the
pose the driver sends as `dxr_prov_set_display_pose`). Hence the one rule:

> **Joint poses are relative to the rig camera's transform.** Parent hand visuals under it
> (or `TransformPoint` through it). Moving/rotating/scaling the rig carries the hands,
> exactly as it carries the eyes.

**Scale.** A rig whose virtual display is not the physical size scales the eye positions
by `virtualDisplayHeight / physicalHeight` (display-centric) or by the FOV's span at the
convergence distance over the physical height (camera-centric: `2·tan(vFov/2) /
invConvergenceDistance / physicalHeight`). The hands must follow, or a fingertip touching
the glass would not reach the virtual display plane. `DisplayXRHandTracking.ResolvePoseScale`
derives that ratio from the active rig every update (1 for a rig-less camera, a rig at
infinity, or before display info arrives); `PoseScaleOverride` pins it. Scale-as-zoom (a
scaled rig *transform*) needs nothing extra: the driver folds `lossyScale` out of the
tunables, and parenting under the scaled camera applies it to the hands.

## Requirements

- `com.unity.xr.hands` ≥ 1.3.0 (written against the 1.5 API:
  `XRHandSubsystemDescriptor.Cinfo`, `XRHandSubsystemProvider.TryUpdateHands(UpdateType, ref Pose, NativeArray<XRHandJoint>, ref Pose, NativeArray<XRHandJoint>)`,
  `XRHandProviderUtility.CreateJoint(Handedness, XRHandJointTrackingState, XRHandJointID, Pose, float)`,
  `XRHandProviderUtility.SubsystemUpdater`).
- A runtime that advertises `XR_EXT_hand_tracking` **and** reports `supportsHandTracking`
  — i.e. an input provider with hand roles is registered and present. `displayxr-cli
  selftest` / `info` list the input providers. Without one, the subsystem runs with both
  hands untracked; `DisplayXRHandTracking.IsSupported` is the discriminator.
- **DisplayXR Display** enabled in XR Plug-in Management. There is no separate toggle:
  the hand subsystem is a companion of the display subsystem. Opt out with
  `DisplayXRHandTracking.Enabled = false` before XR init, or `DISPLAYXR_HANDS=0`.

## Validation (Windows box, Ultraleap attached)

1. `displayxr-cli selftest` passes and lists the Ultraleap input provider with hand
   tracking; a native OpenXR hand app (or `sim_input` with `DXR_SIM_INPUT`) shows joints.
2. Install `com.unity.xr.hands` in the test project, import the *Hand Tracking (XR
   Hands)* sample, add `DisplayXRHandJointGizmos` to the Basic Scene, press Play.
3. Native log (`%TEMP%\displayxr_prov_native.log` / Player.log), in order:
   `XR_EXT_hand_tracking: advertised …` → `XR_EXT_hand_tracking: AVAILABLE (enabled, entry
   points resolved)` → `hand tracking: AVAILABLE (extension enabled, system
   supportsHandTracking=1, app requested=1)` → `left tracker created` / `right tracker
   created` → `left hand first seen (wrist LOCAL x,y,z m; flags=0xf)`.
4. Console: `[DisplayXR] Hands: XR Hands subsystem created …`; the sample's status line
   reads `left TRACKED / right TRACKED` with your hands above the sensor; spheres sit in
   front of the panel where the hands are, mirrored correctly (left is left), fingers
   curl the right way, and a fingertip on the glass lands on the display plane.
5. Dock/undock the Game view (session restart): tracking resumes without a Play restart.
6. Unplug the tracker mid-session: both hands go untracked (`isTracked` false), no errors;
   replug: tracking resumes (the runtime's role arbitration follows presence, ADR-034
   Amendment 3).
7. A project **without** `com.unity.xr.hands`: compiles clean, no `Runtime/Hands`
   assembly, behaviour byte-for-byte as before.

## Limitations

- Joints only. No provider-supplied aim/pinch/grip/poke poses (`canSurfaceCommonPoseData`
  is left at its default) and no velocities; XR Hands' joint-derived helpers still work.
- No controller input: the runtime's Ultraleap Tier 1 "hand as motion controller"
  (grip/aim actions, pinch = select) and gamepads need an `IUnityXRInput` provider in this
  plugin — a separate follow-up issue, deliberately out of this change.
- Located at the frame's predicted display time only (`time` is exposed on the C ABI but
  the subsystem passes 0 = current frame).
- Windows-first in practice (the Ultraleap provider is Windows today). The binding compiles
  on every platform and reports *unsupported* where the runtime serves no hands.
- Not hardware-verified at the time of writing: the native and in-editor runs above are the
  acceptance test on the Windows box.
