// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Compiled only when com.unity.xr.hands >= 1.3.0 is installed (the asmdef's
// DISPLAYXR_XR_HANDS version define); without the package this whole assembly is
// skipped and the plugin stays display-only.

using System;
using Unity.Collections;
using UnityEngine;
using UnityEngine.XR.Hands;
using UnityEngine.XR.Hands.ProviderImplementation;

namespace DisplayXR.Hands
{
    /// <summary>
    /// The DisplayXR <see cref="XRHandSubsystem"/>: 26-joint hand tracking served by the
    /// DisplayXR runtime's <c>XR_EXT_hand_tracking</c> (an input-provider plug-in with
    /// hand roles — the Ultraleap provider today), surfaced through Unity's standard
    /// XR Hands API so <c>XRHandTrackingEvents</c>, <c>XRHandSkeletonDriver</c>,
    /// <c>XRHandMeshController</c> and any XR Hands consumer work unchanged.
    ///
    /// Created, started and stopped by <see cref="DisplayXRHandTracking"/> alongside the
    /// display subsystem; apps do not instantiate it. Joint poses are relative to the
    /// rig camera's transform (the display plane) — see the Hand Tracking doc page.
    /// </summary>
    public sealed class DisplayXRHandSubsystem : XRHandSubsystem
    {
        /// <summary>Descriptor id, matched by <see cref="DisplayXRHandTracking"/> at loader Initialize.</summary>
        public const string SubsystemId = "DisplayXR Hands";

        // Descriptor registration runs at SubsystemRegistration — before XR Plug-in
        // Management initializes any loader — exactly as the XR Hands provider guide
        // prescribes. The subsystem itself is created later, from the loader's stage.
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        static void RegisterDescriptor()
        {
            XRHandSubsystemDescriptor.Register(new XRHandSubsystemDescriptor.Cinfo
            {
                id = SubsystemId,
                providerType = typeof(DisplayXRHandSubsystemProvider),
                subsystemTypeOverride = typeof(DisplayXRHandSubsystem),
            });
        }
    }

    /// <summary>
    /// The provider behind <see cref="DisplayXRHandSubsystem"/>: one P/Invoke per hand per
    /// update into <c>displayxr_unity</c>, which locates the joints with
    /// <c>xrLocateHandJointsEXT</c> in the views' reference space at the views' predicted
    /// display time, then converts OpenXR's joint order and location flags into XR Hands'.
    /// </summary>
    public sealed class DisplayXRHandSubsystemProvider : XRHandSubsystemProvider
    {
        const int k_JointCount = DisplayXRHandNative.JointCount;
        const int k_Stride = DisplayXRHandNative.PoseStride;

        // OpenXR XrHandJointEXT order (index) -> XRHandJointID. The two orders differ only
        // at the head: OpenXR is palm(0), wrist(1); XR Hands is wrist, palm. Everything
        // from the thumb metacarpal on is the same sequence — metacarpal, proximal,
        // intermediate, distal, tip per finger (the thumb has no intermediate).
        static readonly XRHandJointID[] s_OpenXRToUnity =
        {
            XRHandJointID.Palm,              // XR_HAND_JOINT_PALM_EXT = 0
            XRHandJointID.Wrist,             // XR_HAND_JOINT_WRIST_EXT = 1
            XRHandJointID.ThumbMetacarpal,   // 2
            XRHandJointID.ThumbProximal,     // 3
            XRHandJointID.ThumbDistal,       // 4
            XRHandJointID.ThumbTip,          // 5
            XRHandJointID.IndexMetacarpal,   // 6
            XRHandJointID.IndexProximal,     // 7
            XRHandJointID.IndexIntermediate, // 8
            XRHandJointID.IndexDistal,       // 9
            XRHandJointID.IndexTip,          // 10
            XRHandJointID.MiddleMetacarpal,  // 11
            XRHandJointID.MiddleProximal,    // 12
            XRHandJointID.MiddleIntermediate,// 13
            XRHandJointID.MiddleDistal,      // 14
            XRHandJointID.MiddleTip,         // 15
            XRHandJointID.RingMetacarpal,    // 16
            XRHandJointID.RingProximal,      // 17
            XRHandJointID.RingIntermediate,  // 18
            XRHandJointID.RingDistal,        // 19
            XRHandJointID.RingTip,           // 20
            XRHandJointID.LittleMetacarpal,  // 21
            XRHandJointID.LittleProximal,    // 22
            XRHandJointID.LittleIntermediate,// 23
            XRHandJointID.LittleDistal,      // 24
            XRHandJointID.LittleTip,         // 25
        };

        /// <summary>OpenXR index of the wrist — XR Hands' root pose ("located at the wrist joint").</summary>
        const int k_OpenXRWrist = 1;

        // XrSpaceLocationFlags.
        const ulong k_OrientationValid   = 0x1;
        const ulong k_PositionValid      = 0x2;
        const ulong k_OrientationTracked = 0x4;
        const ulong k_PositionTracked    = 0x8;

        // Scratch buffers reused every update — no per-frame allocation.
        readonly float[] m_Poses = new float[k_JointCount * k_Stride];
        readonly float[] m_Radii = new float[k_JointCount];
        readonly ulong[] m_Flags = new ulong[k_JointCount];

        // Latched once the native plugin turns out to be absent (unsupported platform) so
        // the subsystem goes quiet instead of throwing twice per frame.
        bool m_NativeMissing;

        public override void Start()
        {
            if (m_NativeMissing) return;
            try { DisplayXRHandNative.DisplayXR_HandTracking_Start(); }
            catch (DllNotFoundException) { m_NativeMissing = true; }
            catch (EntryPointNotFoundException) { m_NativeMissing = true; }
        }

        public override void Stop()
        {
            if (m_NativeMissing) return;
            try { DisplayXRHandNative.DisplayXR_HandTracking_Stop(); }
            catch (DllNotFoundException) { m_NativeMissing = true; }
            catch (EntryPointNotFoundException) { m_NativeMissing = true; }
        }

        public override void Destroy() => Stop();

        public override void GetHandLayout(NativeArray<bool> handJointsInLayout)
        {
            for (int i = 0; i < handJointsInLayout.Length; i++)
                handJointsInLayout[i] = false;
            // XR_EXT_hand_tracking's default joint set is the full 26 — every XR Hands joint.
            foreach (var id in s_OpenXRToUnity)
                handJointsInLayout[id.ToIndex()] = true;
        }

        public override XRHandSubsystem.UpdateSuccessFlags TryUpdateHands(
            XRHandSubsystem.UpdateType updateType,
            ref Pose leftHandRootPose, NativeArray<XRHandJoint> leftHandJoints,
            ref Pose rightHandRootPose, NativeArray<XRHandJoint> rightHandJoints)
        {
            var flags = XRHandSubsystem.UpdateSuccessFlags.None;
            if (m_NativeMissing) return flags;

            // Metres -> the rig's world units (virtual display height vs. the physical
            // panel). Resolved once per update, on the main thread, from the active rig.
            float scale = DisplayXRHandTracking.ResolvePoseScale();

            if (UpdateHand(Handedness.Left, 0, scale, ref leftHandRootPose, leftHandJoints))
                flags |= XRHandSubsystem.UpdateSuccessFlags.LeftHandRootPose
                       | XRHandSubsystem.UpdateSuccessFlags.LeftHandJoints;
            if (UpdateHand(Handedness.Right, 1, scale, ref rightHandRootPose, rightHandJoints))
                flags |= XRHandSubsystem.UpdateSuccessFlags.RightHandRootPose
                       | XRHandSubsystem.UpdateSuccessFlags.RightHandJoints;
            return flags;
        }

        bool UpdateHand(Handedness handedness, int hand, float scale,
                        ref Pose rootPose, NativeArray<XRHandJoint> joints)
        {
            int active;
            try
            {
                // time 0 = the predicted display time of the frame the views were
                // located at (the native side latches it per xrBeginFrame).
                active = DisplayXRHandNative.DisplayXR_HandTracking_Locate(hand, 0, m_Poses, m_Radii, m_Flags);
            }
            catch (DllNotFoundException) { m_NativeMissing = true; return false; }
            catch (EntryPointNotFoundException) { m_NativeMissing = true; return false; }

            // Not tracked (or not available): leave the arrays alone and report no update
            // for this hand, which is what flips XRHand.isTracked to false.
            if (active == 0) return false;

            for (int j = 0; j < k_JointCount; j++)
            {
                int o = j * k_Stride;
                // Already in Unity's frame (native mirrors Z and the quaternion's X/Y, the
                // same conversion the eye poses take); only the scale is applied here.
                var pose = new Pose(
                    new Vector3(m_Poses[o] * scale, m_Poses[o + 1] * scale, m_Poses[o + 2] * scale),
                    new Quaternion(m_Poses[o + 3], m_Poses[o + 4], m_Poses[o + 5], m_Poses[o + 6]));

                ulong f = m_Flags[j];
                var state = XRHandJointTrackingState.None;
                if ((f & k_PositionValid) != 0 && (f & k_OrientationValid) != 0)
                {
                    state |= XRHandJointTrackingState.Pose;
                    // Both TRACKED bits = measured, not inferred/extrapolated — XR Hands'
                    // "high fidelity" distinction.
                    if ((f & k_PositionTracked) != 0 && (f & k_OrientationTracked) != 0)
                        state |= XRHandJointTrackingState.HighFidelityPose;
                }
                float radius = m_Radii[j] * scale;
                if (radius > 0f) state |= XRHandJointTrackingState.Radius;

                var id = s_OpenXRToUnity[j];
                joints[id.ToIndex()] = XRHandProviderUtility.CreateJoint(handedness, state, id, pose, radius);
                if (j == k_OpenXRWrist) rootPose = pose;
            }
            return true;
        }
    }
}
