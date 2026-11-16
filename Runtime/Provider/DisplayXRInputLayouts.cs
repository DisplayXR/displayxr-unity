// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Input System layouts for the DisplayXR input devices. Compiled only where the Input
// System's own XR support is (the XR module present, VR enabled) — the same guard the
// Input System puts on XRController.

#if HAS_INPUT_SYSTEM && DISPLAYXR_INPUT_SYSTEM_XR && (ENABLE_VR || UNITY_GAMECORE)
using UnityEngine;
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.Controls;
using UnityEngine.InputSystem.Layouts;
using UnityEngine.InputSystem.XR;

namespace DisplayXR
{
    /// <summary>
    /// Input System device for a hand the runtime binds to the stylus interaction profile
    /// (<see cref="DisplayXRInput.StylusDeviceName"/>). Bind actions to it as
    /// <c>&lt;DisplayXRStylus&gt;{RightHand}/tipPressure</c>, <c>…/primaryButton</c>,
    /// <c>…/devicePosition</c> etc., or through the generic <c>&lt;XRController&gt;</c> paths
    /// (<c>trigger</c>, <c>grip</c>, <c>primaryButton</c>, <c>secondaryButton</c>).
    ///
    /// The control set and state layout come from the native device definition: the
    /// Input System's XR layout builder generates a layout that extends this one, with
    /// one control per native feature (lower-cased feature name). This class only gives
    /// those controls typed properties; it declares none itself, so the two can never
    /// disagree on a state offset. A property is null when the feature is absent.
    /// Poses are relative to the rig camera (see <see cref="DisplayXRInput"/>).
    /// <c>SendImpulse(amplitude, duration)</c> drives the stylus haptics.
    /// </summary>
    [InputControlLayout(displayName = "DisplayXR Stylus", commonUsages = new[] { "LeftHand", "RightHand" })]
    public class DisplayXRStylus : XRControllerWithRumble
    {
        /// <summary>Aim pose position.</summary>
        public Vector3Control pointerPosition { get; private set; }
        /// <summary>Aim pose rotation.</summary>
        public QuaternionControl pointerRotation { get; private set; }
        /// <summary>Pen tip pose position.</summary>
        public Vector3Control tipPosition { get; private set; }
        /// <summary>Pen tip pose rotation.</summary>
        public QuaternionControl tipRotation { get; private set; }
        /// <summary>Tip force 0..1 (also <c>trigger</c>).</summary>
        public AxisControl tipPressure { get; private set; }
        /// <summary>Middle cluster force 0..1 (also <c>grip</c>).</summary>
        public AxisControl middlePressure { get; private set; }
        /// <summary>Tip force, as the generic trigger.</summary>
        public AxisControl trigger { get; private set; }
        /// <summary>Tip force &gt; 0.</summary>
        public ButtonControl triggerButton { get; private set; }
        /// <summary>Middle cluster force, as the generic grip.</summary>
        public AxisControl grip { get; private set; }
        /// <summary>Middle cluster force ≥ 0.5.</summary>
        public ButtonControl gripButton { get; private set; }
        /// <summary>Front cluster click.</summary>
        public ButtonControl primaryButton { get; private set; }
        /// <summary>Front cluster double tap.</summary>
        public ButtonControl primaryDoubleTap { get; private set; }
        /// <summary>Back cluster click.</summary>
        public ButtonControl secondaryButton { get; private set; }
        /// <summary>Back cluster double tap.</summary>
        public ButtonControl secondaryDoubleTap { get; private set; }
        /// <summary>The stylus sits in its dock.</summary>
        public ButtonControl docked { get; private set; }

        protected override void FinishSetup()
        {
            base.FinishSetup();
            pointerPosition = TryGetChildControl<Vector3Control>("pointerposition");
            pointerRotation = TryGetChildControl<QuaternionControl>("pointerrotation");
            tipPosition = TryGetChildControl<Vector3Control>("tipposition");
            tipRotation = TryGetChildControl<QuaternionControl>("tiprotation");
            tipPressure = TryGetChildControl<AxisControl>("tippressure");
            middlePressure = TryGetChildControl<AxisControl>("middlepressure");
            trigger = TryGetChildControl<AxisControl>("trigger");
            triggerButton = TryGetChildControl<ButtonControl>("triggerbutton");
            grip = TryGetChildControl<AxisControl>("grip");
            gripButton = TryGetChildControl<ButtonControl>("gripbutton");
            primaryButton = TryGetChildControl<ButtonControl>("primarybutton");
            primaryDoubleTap = TryGetChildControl<ButtonControl>("primarydoubletap");
            secondaryButton = TryGetChildControl<ButtonControl>("secondarybutton");
            secondaryDoubleTap = TryGetChildControl<ButtonControl>("secondarydoubletap");
            docked = TryGetChildControl<ButtonControl>("docked");
        }
    }

    /// <summary>
    /// Input System device for a hand bound to <c>khr/simple_controller</c>
    /// (<see cref="DisplayXRInput.ControllerDeviceName"/>): <c>trigger</c> /
    /// <c>triggerButton</c> (select), <c>menuButton</c>, grip and aim poses.
    /// </summary>
    [InputControlLayout(displayName = "DisplayXR Controller", commonUsages = new[] { "LeftHand", "RightHand" })]
    public class DisplayXRController : XRControllerWithRumble
    {
        /// <summary>Aim pose position.</summary>
        public Vector3Control pointerPosition { get; private set; }
        /// <summary>Aim pose rotation.</summary>
        public QuaternionControl pointerRotation { get; private set; }
        /// <summary>Select as 0/1.</summary>
        public AxisControl trigger { get; private set; }
        /// <summary>Select click.</summary>
        public ButtonControl triggerButton { get; private set; }
        /// <summary>Menu click.</summary>
        public ButtonControl menuButton { get; private set; }

        protected override void FinishSetup()
        {
            base.FinishSetup();
            pointerPosition = TryGetChildControl<Vector3Control>("pointerposition");
            pointerRotation = TryGetChildControl<QuaternionControl>("pointerrotation");
            trigger = TryGetChildControl<AxisControl>("trigger");
            triggerButton = TryGetChildControl<ButtonControl>("triggerbutton");
            menuButton = TryGetChildControl<ButtonControl>("menubutton");
        }
    }

    static class DisplayXRInputLayouts
    {
#if UNITY_EDITOR
        [UnityEditor.InitializeOnLoadMethod]
#endif
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
        static void Register()
        {
            InputSystem.RegisterLayout<DisplayXRStylus>(matches: new InputDeviceMatcher()
                .WithInterface(XRUtilities.InterfaceMatchAnyVersion)
                .WithManufacturer(DisplayXRInput.Manufacturer)
                .WithProduct(DisplayXRInput.StylusDeviceName));
            InputSystem.RegisterLayout<DisplayXRController>(matches: new InputDeviceMatcher()
                .WithInterface(XRUtilities.InterfaceMatchAnyVersion)
                .WithManufacturer(DisplayXRInput.Manufacturer)
                .WithProduct(DisplayXRInput.ControllerDeviceName));
        }
    }
}
#endif
