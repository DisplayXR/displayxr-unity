// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Hand Tracking sample: draws each tracked hand's 26 joints as spheres plus finger
// bones, placed relative to the active DisplayXR rig camera, with a status line.
// Dependency-free beyond XR Hands itself — no XR Hands sample content needed. For
// skinned hand meshes use XRHandSkeletonDriver / XRHandMeshController instead (README).

using System.Collections.Generic;
using UnityEngine;
using UnityEngine.XR.Hands;
using DisplayXR.Hands;

namespace DisplayXR.Samples
{
    /// <summary>
    /// Visualizes the DisplayXR XR Hands subsystem. Drop it on any GameObject; it reads
    /// <c>XRHandSubsystem.leftHand/rightHand</c> every Update and positions its spheres
    /// in WORLD space by transforming the (rig-relative) joint poses through the active
    /// rig camera — the pattern any app should follow: the runtime's hands are relative
    /// to the display plane, and the rig camera's transform IS the display plane.
    /// </summary>
    public class DisplayXRHandJointGizmos : MonoBehaviour
    {
        [Tooltip("Transform the joints are relative to. Leave empty to follow DisplayXRRigManager.ActiveCamera (recommended).")]
        public Transform rigOverride;

        [Tooltip("Multiplies each joint's reported radius for the sphere size.")]
        public float radiusScale = 1f;

        [Tooltip("Sphere radius (rig units) for joints that report no radius.")]
        public float fallbackRadius = 0.008f;

        [Tooltip("Optional material for the spheres and bones (default: a plain unlit-looking material).")]
        public Material material;

        public bool drawBones = true;

        [Tooltip("Show a status line (support, per-hand tracking, pose scale) in the top-left.")]
        public bool showStatus = true;

        // Finger chains, wrist first — one LineRenderer each.
        static readonly XRHandJointID[][] s_Chains =
        {
            new[] { XRHandJointID.Wrist, XRHandJointID.ThumbMetacarpal, XRHandJointID.ThumbProximal, XRHandJointID.ThumbDistal, XRHandJointID.ThumbTip },
            new[] { XRHandJointID.Wrist, XRHandJointID.IndexMetacarpal, XRHandJointID.IndexProximal, XRHandJointID.IndexIntermediate, XRHandJointID.IndexDistal, XRHandJointID.IndexTip },
            new[] { XRHandJointID.Wrist, XRHandJointID.MiddleMetacarpal, XRHandJointID.MiddleProximal, XRHandJointID.MiddleIntermediate, XRHandJointID.MiddleDistal, XRHandJointID.MiddleTip },
            new[] { XRHandJointID.Wrist, XRHandJointID.RingMetacarpal, XRHandJointID.RingProximal, XRHandJointID.RingIntermediate, XRHandJointID.RingDistal, XRHandJointID.RingTip },
            new[] { XRHandJointID.Wrist, XRHandJointID.LittleMetacarpal, XRHandJointID.LittleProximal, XRHandJointID.LittleIntermediate, XRHandJointID.LittleDistal, XRHandJointID.LittleTip },
        };

        class HandVisual
        {
            public GameObject root;
            public Transform[] joints = new Transform[XRHandJointID.EndMarker.ToIndex()];
            public LineRenderer[] bones;
            public bool visible;
        }

        HandVisual m_Left, m_Right;
        XRHandSubsystem m_Subsystem;
        readonly List<XRHandSubsystem> m_Found = new List<XRHandSubsystem>();
        readonly Vector3[] m_Line = new Vector3[6];

        void Start()
        {
            m_Left = Build("Left Hand", new Color(0.3f, 0.7f, 1f));
            m_Right = Build("Right Hand", new Color(1f, 0.6f, 0.3f));
        }

        HandVisual Build(string name, Color color)
        {
            var hv = new HandVisual { root = new GameObject(name) };
            hv.root.transform.SetParent(transform, false);
            var mat = material != null ? material : new Material(Shader.Find("Sprites/Default")) { color = color };
            for (int i = 0; i < hv.joints.Length; i++)
            {
                var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
                Destroy(go.GetComponent<Collider>());
                go.name = XRHandJointIDUtility.FromIndex(i).ToString();
                go.transform.SetParent(hv.root.transform, false);
                go.GetComponent<Renderer>().sharedMaterial = mat;
                hv.joints[i] = go.transform;
            }
            hv.bones = new LineRenderer[s_Chains.Length];
            for (int c = 0; c < s_Chains.Length; c++)
            {
                var go = new GameObject("Bones " + c);
                go.transform.SetParent(hv.root.transform, false);
                var lr = go.AddComponent<LineRenderer>();
                lr.sharedMaterial = mat;
                lr.useWorldSpace = true;
                lr.positionCount = s_Chains[c].Length;
                lr.startWidth = lr.endWidth = 0.004f;
                hv.bones[c] = lr;
            }
            hv.root.SetActive(false);
            return hv;
        }

        void Update()
        {
            if (m_Subsystem == null || !m_Subsystem.running)
            {
                m_Subsystem = null;
                m_Found.Clear();
                SubsystemManager.GetSubsystems(m_Found);
                foreach (var s in m_Found)
                    if (s.running) { m_Subsystem = s; break; }
            }
            Transform rig = rigOverride != null ? rigOverride
                          : (DisplayXRRigManager.ActiveCamera != null ? DisplayXRRigManager.ActiveCamera.transform : null);
            Apply(m_Left, m_Subsystem != null ? m_Subsystem.leftHand : default, rig);
            Apply(m_Right, m_Subsystem != null ? m_Subsystem.rightHand : default, rig);
        }

        void Apply(HandVisual hv, XRHand hand, Transform rig)
        {
            bool show = m_Subsystem != null && hand.isTracked && rig != null;
            if (show != hv.visible) { hv.root.SetActive(show); hv.visible = show; }
            if (!show) return;

            for (int i = 0; i < hv.joints.Length; i++)
            {
                var joint = hand.GetJoint(XRHandJointIDUtility.FromIndex(i));
                var t = hv.joints[i];
                if (!joint.TryGetPose(out Pose pose)) { t.gameObject.SetActive(false); continue; }
                t.gameObject.SetActive(true);
                // Rig-relative -> world: the rig camera's transform is the display plane.
                t.position = rig.TransformPoint(pose.position);
                t.rotation = rig.rotation * pose.rotation;
                float r = joint.TryGetRadius(out float radius) && radius > 0f ? radius * radiusScale : fallbackRadius;
                t.localScale = Vector3.one * (2f * r);
            }
            if (!drawBones) { foreach (var lr in hv.bones) lr.enabled = false; return; }
            for (int c = 0; c < s_Chains.Length; c++)
            {
                var chain = s_Chains[c];
                var lr = hv.bones[c];
                lr.enabled = true;
                for (int k = 0; k < chain.Length; k++)
                    m_Line[k] = hv.joints[chain[k].ToIndex()].position;
                lr.positionCount = chain.Length;
                lr.SetPositions(m_Line);
            }
        }

        void OnGUI()
        {
            if (!showStatus) return;
            string status;
            if (m_Subsystem == null) status = "XR Hands subsystem: not running (is com.unity.xr.hands installed and DisplayXR Display enabled?)";
            else if (!DisplayXRHandTracking.IsSupported) status = "XR Hands subsystem running — runtime reports NO hand source (XR_EXT_hand_tracking unsupported or no input provider with hands)";
            else status = "Hands: left " + (m_Subsystem.leftHand.isTracked ? "TRACKED" : "-") +
                          "  right " + (m_Subsystem.rightHand.isTracked ? "TRACKED" : "-") +
                          "  (pose scale " + DisplayXRHandTracking.CurrentPoseScale.ToString("0.###") + ")";
            GUI.Label(new Rect(10, 10, Screen.width - 20, 24), status);
        }
    }
}
