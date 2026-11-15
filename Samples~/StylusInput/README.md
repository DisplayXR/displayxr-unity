# Stylus Input sample

`DisplayXRStylusPointer` reads a stylus (or a simple controller) served by any
DisplayXR input provider through Unity's ordinary `UnityEngine.XR.InputDevice` API
and draws with it:

| Input | Effect |
|---|---|
| Tip pose | A small marker follows the pen tip |
| Aim pose | A white ray along the pen |
| Tip pressure on the glass | Draws a stroke; its width follows the pressure |
| Front cluster button (`primaryButton`) | Clears the strokes, short haptic pulse |
| Back cluster button (`secondaryButton`) | Longer haptic pulse |

A status line shows the device name, tracking, the rig frame, tip and middle-cluster
pressure and the dock state.

## Use

1. Open a scene with a DisplayXR rig (`DisplayXRDisplay` or `DisplayXRCamera`), e.g.
   the *Display Scene* sample.
2. Add `DisplayXRStylusPointer` to any GameObject and press Play.
3. A stylus shows up as **DisplayXR Stylus** when the runtime advertises
   `XR_LOGITECH_mx_ink_stylus_interaction` and an input provider binds that profile;
   any other provider's controller shows up as **DisplayXR Controller**
   (`khr/simple_controller`: select draws, menu does nothing here).

## How the pose reaches the scene

The device poses are relative to the **rig camera** in rig units, the same frame the
XR Hands joints use. `DisplayXRInput.TryGetWorldPose` composes them with the active
rig camera's transform. You can also parent a pen model under the rig camera and set
its `localPosition` / `localRotation` straight from `devicePosition` /
`deviceRotation` (or a `TrackedPoseDriver` with the *Rotation and Position*, *Local*
setting on a child of the camera).

See `docs~/architecture/controller-input.md` in the package for the full list of
features, the Input System layouts and the frame rules.
