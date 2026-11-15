// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.XR;

namespace DisplayXR
{
    /// <summary>
    /// Which rig the controller poses are expressed in (the rig the views were located
    /// with; same values as the hand-tracking rig kind).
    /// </summary>
    public enum DisplayXRInputRigKind
    {
        /// <summary>No rig chained (runtime without <c>XR_DXR_view_rig</c>): metres, display plane.</summary>
        None = 0,
        /// <summary>The app's display-centric rig (<see cref="DisplayXRDisplay"/>).</summary>
        Display = 1,
        /// <summary>The app's camera-centric rig (<see cref="DisplayXRCamera"/>).</summary>
        Camera = 2,
        /// <summary>The default display rig: no app rig pushed yet (boot splash, rig-less scene).</summary>
        DefaultDisplay = 3,
    }

    /// <summary>
    /// Controllers and styluses from <b>any</b> DisplayXR input provider, as standard
    /// <see cref="InputDevice"/>s.
    ///
    /// The display provider owns the OpenXR session, so it also owns the session's
    /// actions: one action set suggested for <c>/interaction_profiles/khr/simple_controller</c>
    /// and — when the runtime advertises <c>XR_LOGITECH_mx_ink_stylus_interaction</c> —
    /// <c>/interaction_profiles/logitech/mx_ink_stylus_logitech</c>, synced every frame,
    /// surfaced through the <c>"DisplayXR Input"</c> <see cref="XRInputSubsystem"/>. Each hand
    /// the runtime binds a profile to becomes one device:
    /// <list type="bullet">
    /// <item><description><see cref="StylusDeviceName"/> — the stylus profile.</description></item>
    /// <item><description><see cref="ControllerDeviceName"/> — the simple controller profile.</description></item>
    /// </list>
    /// Find them with <see cref="InputDevices.GetDevicesWithCharacteristics"/>
    /// (<c>HeldInHand | Controller | TrackedDevice</c>, plus <c>Left</c>/<c>Right</c>) or
    /// <see cref="TryGetStylus"/>; read the standard <see cref="CommonUsages"/> plus
    /// <see cref="DisplayXRStylusUsages"/>.
    ///
    /// <b>Frame.</b> Poses are relative to the <b>rig camera's transform</b> (the active
    /// <see cref="DisplayXRDisplay"/> / <see cref="DisplayXRCamera"/>), as the runtime
    /// located them in the frame of the rig the views were located with: a stylus on a
    /// glass-registered origin comes back in rig units, a tip touching the glass on the
    /// pixel it touches (see <see cref="ApplyRigGain"/> for metric display-relative devices).
    /// Parent the pen visual under the rig camera, or use <see cref="TryGetWorldPose"/>.
    /// On a display-centric rig the glass is the rig camera's plane (z = 0); on a
    /// camera-centric rig the glass is the convergence plane in front of the camera.
    ///
    /// <b>Haptics.</b> <c>device.SendHapticImpulse(0, amplitude, durationSeconds)</c> →
    /// <c>xrApplyHapticFeedback</c> on that hand's <c>/output/haptic</c>.
    /// </summary>
    public static class DisplayXRInput
    {
        /// <summary>The input subsystem id (Runtime/UnitySubsystemsManifest.json "inputs").</summary>
        public const string SubsystemId = "DisplayXR Input";
        /// <summary><see cref="InputDevice.name"/> of a hand bound to the stylus profile.</summary>
        public const string StylusDeviceName = "DisplayXR Stylus";
        /// <summary><see cref="InputDevice.name"/> of a hand bound to <c>khr/simple_controller</c>.</summary>
        public const string ControllerDeviceName = "DisplayXR Controller";
        /// <summary><see cref="InputDevice.manufacturer"/> of both.</summary>
        public const string Manufacturer = "DisplayXR";

        /// <summary>
        /// Master switch, read when the display loader initializes (set it before XR init,
        /// e.g. from a <c>[RuntimeInitializeOnLoadMethod(BeforeSplashScreen)]</c>). Default
        /// true. <c>DISPLAYXR_INPUT=0</c> in the environment also disables it. Off = no
        /// input subsystem, no OpenXR actions: the session is exactly what it was without it.
        /// </summary>
        public static bool Enabled { get; set; } = true;

        static bool s_NativeMissing;
        static bool s_ApplyRigGain;

        /// <summary>
        /// Off (default): poses as the runtime located them, re-expressed relative to the
        /// rig camera. The runtime already places a glass-registered stylus in the rig
        /// frame, glass scale included. On: also scale each pose about the rig's display
        /// plane by the hand-joint gain (display rig <c>perspectiveFactor · virtualDisplayHeight
        /// / canvas height</c>; camera rig the virtual screen height on the convergence plane
        /// over the canvas height). Only for a display-relative device the runtime returns
        /// metric; on a glass-registered stylus it scales the pen twice. Live.
        /// <c>DISPLAYXR_INPUT_RIG_GAIN=1</c> in the environment also turns it on.
        /// </summary>
        public static bool ApplyRigGain
        {
            get => s_ApplyRigGain;
            set
            {
                s_ApplyRigGain = value;
                if (s_NativeMissing) return;
                try { DisplayXRInputNative.DisplayXR_Input_SetRigGain(value ? 1 : 0); }
                catch (DllNotFoundException) { s_NativeMissing = true; }
                catch (EntryPointNotFoundException) { s_NativeMissing = true; }
            }
        }

        internal static bool IsEnabledByApp() =>
            Enabled && Environment.GetEnvironmentVariable("DISPLAYXR_INPUT") != "0";

        internal static void PushEnabledToNative(bool enabled)
        {
            if (s_NativeMissing) return;
            try
            {
                DisplayXRInputNative.DisplayXR_Input_SetEnabled(enabled ? 1 : 0);
                DisplayXRInputNative.DisplayXR_Input_SetRigGain(s_ApplyRigGain ? 1 : 0);
            }
            catch (DllNotFoundException) { s_NativeMissing = true; }
            catch (EntryPointNotFoundException) { s_NativeMissing = true; }
        }

        /// <summary>True while the action set is attached to a live session.</summary>
        public static bool IsActive => Query(DisplayXRInputNative.DisplayXR_Input_IsActive) != 0;

        /// <summary>
        /// True when the runtime advertises the stylus interaction profile extension and its
        /// bindings were accepted on the current session. False on a runtime without it — a
        /// stylus provider then shows up (if at all) as a <see cref="ControllerDeviceName"/>.
        /// </summary>
        public static bool IsStylusProfileAvailable =>
            Query(DisplayXRInputNative.DisplayXR_Input_IsStylusProfileAvailable) != 0;

        /// <summary>The rig the poses are currently expressed in.</summary>
        public static DisplayXRInputRigKind RigKind =>
            (DisplayXRInputRigKind)Query(DisplayXRInputNative.DisplayXR_Input_RigKind);

        static int Query(Func<int> f)
        {
            if (s_NativeMissing) return 0;
            try { return f(); }
            catch (DllNotFoundException) { s_NativeMissing = true; return 0; }
            catch (EntryPointNotFoundException) { s_NativeMissing = true; return 0; }
        }

        static readonly List<InputDevice> s_Devices = new List<InputDevice>();

        /// <summary>
        /// The first connected DisplayXR stylus (optionally only the one on
        /// <see cref="InputDeviceCharacteristics.Left"/> / <see cref="InputDeviceCharacteristics.Right"/>).
        /// </summary>
        public static bool TryGetStylus(out InputDevice device,
            InputDeviceCharacteristics hand = InputDeviceCharacteristics.None) =>
            TryGetByName(StylusDeviceName, hand, out device);

        /// <summary>The first connected DisplayXR simple controller (optionally for one hand).</summary>
        public static bool TryGetController(out InputDevice device,
            InputDeviceCharacteristics hand = InputDeviceCharacteristics.None) =>
            TryGetByName(ControllerDeviceName, hand, out device);

        static bool TryGetByName(string name, InputDeviceCharacteristics hand, out InputDevice device)
        {
            s_Devices.Clear();
            InputDevices.GetDevicesWithCharacteristics(
                InputDeviceCharacteristics.HeldInHand | InputDeviceCharacteristics.Controller | hand, s_Devices);
            foreach (var d in s_Devices)
            {
                if (d.isValid && d.name == name && d.manufacturer == Manufacturer)
                {
                    device = d;
                    return true;
                }
            }
            device = default;
            return false;
        }

        /// <summary>
        /// A device pose in WORLD space: the rig-camera-relative pose composed with the
        /// active rig camera's transform (its scale included, so a scale-as-zoom rig keeps
        /// the pen on the content). <paramref name="position"/> /
        /// <paramref name="rotation"/> pick the pose — <see cref="CommonUsages.devicePosition"/>
        /// (grip), <see cref="DisplayXRStylusUsages.pointerPosition"/> (aim) or
        /// <see cref="DisplayXRStylusUsages.tipPosition"/> (tip). False when there is no
        /// active rig camera or the device does not report that pose.
        /// </summary>
        public static bool TryGetWorldPose(InputDevice device, InputFeatureUsage<Vector3> position,
            InputFeatureUsage<Quaternion> rotation, out Pose world)
        {
            world = Pose.identity;
            var cam = DisplayXRRigManager.ActiveCamera;
            if (cam == null || !device.isValid) return false;
            if (!device.TryGetFeatureValue(position, out var p) || !device.TryGetFeatureValue(rotation, out var q))
                return false;
            var t = cam.transform;
            world = new Pose(t.TransformPoint(p), t.rotation * q);
            return true;
        }
    }

    /// <summary>
    /// The DisplayXR devices' features beyond <see cref="CommonUsages"/>. Standard usages
    /// they also carry: <c>isTracked</c>, <c>trackingState</c>, <c>devicePosition</c> /
    /// <c>deviceRotation</c> / <c>deviceVelocity</c> / <c>deviceAngularVelocity</c> (grip
    /// pose); stylus: <c>trigger</c> (tip force), <c>triggerButton</c> (tip force &gt; 0),
    /// <c>grip</c> (middle cluster force), <c>gripButton</c> (middle force ≥ 0.5),
    /// <c>primaryButton</c> (front cluster click), <c>secondaryButton</c> (back cluster click);
    /// simple controller: <c>trigger</c> / <c>triggerButton</c> (select), <c>menuButton</c>.
    /// </summary>
    public static class DisplayXRStylusUsages
    {
        /// <summary>Aim pose position (both device kinds).</summary>
        public static readonly InputFeatureUsage<Vector3> pointerPosition = new InputFeatureUsage<Vector3>("PointerPosition");
        /// <summary>Aim pose rotation (both device kinds).</summary>
        public static readonly InputFeatureUsage<Quaternion> pointerRotation = new InputFeatureUsage<Quaternion>("PointerRotation");
        /// <summary>Pen tip pose position (stylus).</summary>
        public static readonly InputFeatureUsage<Vector3> tipPosition = new InputFeatureUsage<Vector3>("TipPosition");
        /// <summary>Pen tip pose rotation (stylus).</summary>
        public static readonly InputFeatureUsage<Quaternion> tipRotation = new InputFeatureUsage<Quaternion>("TipRotation");
        /// <summary>Tip force 0..1 (stylus; same value as <c>trigger</c>).</summary>
        public static readonly InputFeatureUsage<float> tipPressure = new InputFeatureUsage<float>("TipPressure");
        /// <summary>Middle cluster force 0..1 (stylus; same value as <c>grip</c>).</summary>
        public static readonly InputFeatureUsage<float> middlePressure = new InputFeatureUsage<float>("MiddlePressure");
        /// <summary>Front cluster double tap (stylus).</summary>
        public static readonly InputFeatureUsage<bool> primaryDoubleTap = new InputFeatureUsage<bool>("PrimaryDoubleTap");
        /// <summary>Back cluster double tap (stylus).</summary>
        public static readonly InputFeatureUsage<bool> secondaryDoubleTap = new InputFeatureUsage<bool>("SecondaryDoubleTap");
        /// <summary>The stylus sits in its dock (stylus).</summary>
        public static readonly InputFeatureUsage<bool> docked = new InputFeatureUsage<bool>("Docked");
    }

    /// <summary>P/Invoke bindings to <c>native~/displayxr_xrprovider/displayxr_provider_input.{h,cpp}</c>.</summary>
    internal static class DisplayXRInputNative
    {
        const string LibName = "displayxr_unity";

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void DisplayXR_Input_SetEnabled(int enabled);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int DisplayXR_Input_IsActive();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int DisplayXR_Input_IsStylusProfileAvailable();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int DisplayXR_Input_GetHandKind(int hand);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int DisplayXR_Input_RigKind();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void DisplayXR_Input_SetRigGain(int enabled);
    }
}
