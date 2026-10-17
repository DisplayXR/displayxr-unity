// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Hand Tracking sample: the transparent "Ghost Hand" (Ultraleap's GenericHand model,
// Apache-2.0 — see THIRD-PARTY-NOTICES.md) driven by the DisplayXR XR Hands subsystem.
//
// Why not XRHandSkeletonDriver: it writes XR Hands joint rotations straight into the
// bone transforms, so the bones' local axes must already BE the XR Hands joint frame
// (+Z along the bone, +Y out of the back of the hand). The GenericHand rig is a Maya
// skeleton whose bones run along -X, and Ultraleap's own binder (HandBinder, not
// available here) drives it with a constant per-hand rotation offset applied in
// WORLD space. This component reproduces that binding: the offsets are transcribed
// from the plugin's GhostHands.prefab, the bones are found by name once, and the
// right hand is the same left-hand model mirrored in X — exactly their construction.
// Joint rotations from the DisplayXR runtime are Leap's own (the provider passes
// LEAP_BONE.rotation through) mirrored into Unity's frame the same way Ultraleap's
// Unity SDK mirrors them, so the offsets apply unchanged.

using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.XR.Hands;
using DisplayXR.Hands;

namespace DisplayXR.Samples
{
    /// <summary>
    /// Shows the tracked hands as transparent, fresnel-rimmed ghost hands (default) or as
    /// the joint-sphere debug visual (<see cref="DisplayXRHandJointGizmos"/>, the
    /// fallback — also used automatically on HDRP, where the sample ships no shader).
    /// Drop it on any GameObject. The hand models are parented under the active rig
    /// camera (the display plane the joint poses are relative to) and scaled with
    /// <see cref="DisplayXRHandTracking.CurrentPoseScale"/>.
    /// </summary>
    public class DisplayXRGhostHand : MonoBehaviour
    {
        public enum Visual { GhostHand, JointSpheres }

        [Tooltip("Ghost Hand = Ultraleap's transparent hand model. Joint Spheres = the debug visual (DisplayXRHandJointGizmos).")]
        public Visual visual = Visual.GhostHand;

        [Tooltip("Transform the joints are relative to. Leave empty to follow DisplayXRRigManager.ActiveCamera (recommended).")]
        public Transform rigOverride;

        [Tooltip("Override the material (default: the sample's Ghost Hand material for the project's render pipeline).")]
        public Material materialOverride;

        [Tooltip("Also drive the four finger metacarpal bones. Off (Ultraleap's default): they keep the model's rest pose, which hides the runtime's inferred metacarpals.")]
        public bool driveMetacarpals;

        [Tooltip("Also move each driven bone to its joint position (stretches the model to the tracked hand). Off (Ultraleap's default): rotations only, the model keeps its proportions.")]
        public bool setBonePositions;

        [Tooltip("Show a status line (support, per-hand tracking, pose scale) in the top-left.")]
        public bool showStatus = true;

        // ---- Binding table (left-hand FBX bone names; the right hand is the same model mirrored) ----

        struct BoneBinding
        {
            public XRHandJointID joint;
            public string bone;
            public bool metacarpal; // kept at rest unless driveMetacarpals (the thumb's is always driven)
            public BoneBinding(XRHandJointID j, string b, bool m = false) { joint = j; bone = b; metacarpal = m; }
        }

        const string k_WristBone = "L_Wrist";

        static readonly BoneBinding[] s_Bindings =
        {
            // Thumb: Leap's "proximal" bone is the anatomical metacarpal and the runtime
            // maps it to ThumbMetacarpal, so L_thumb_meta is driven like Ultraleap drives it.
            new BoneBinding(XRHandJointID.ThumbMetacarpal,   "L_thumb_meta"),
            new BoneBinding(XRHandJointID.ThumbProximal,     "L_thumb_a"),
            new BoneBinding(XRHandJointID.ThumbDistal,       "L_thumb_b"),
            new BoneBinding(XRHandJointID.IndexMetacarpal,   "L_index_meta", true),
            new BoneBinding(XRHandJointID.IndexProximal,     "L_index_a"),
            new BoneBinding(XRHandJointID.IndexIntermediate, "L_index_b"),
            new BoneBinding(XRHandJointID.IndexDistal,       "L_index_c"),
            new BoneBinding(XRHandJointID.MiddleMetacarpal,  "L_middle_meta", true),
            new BoneBinding(XRHandJointID.MiddleProximal,    "L_middle_a"),
            new BoneBinding(XRHandJointID.MiddleIntermediate,"L_middle_b"),
            new BoneBinding(XRHandJointID.MiddleDistal,      "L_middle_c"),
            new BoneBinding(XRHandJointID.RingMetacarpal,    "L_ring_meta", true),
            new BoneBinding(XRHandJointID.RingProximal,      "L_ring_a"),
            new BoneBinding(XRHandJointID.RingIntermediate,  "L_ring_b"),
            new BoneBinding(XRHandJointID.RingDistal,        "L_ring_c"),
            new BoneBinding(XRHandJointID.LittleMetacarpal,  "L_pinky_meta", true),
            new BoneBinding(XRHandJointID.LittleProximal,    "L_pinky_a"),
            new BoneBinding(XRHandJointID.LittleIntermediate,"L_pinky_b"),
            new BoneBinding(XRHandJointID.LittleDistal,      "L_pinky_c"),
            // L_*_end (tips) and L_Palm are leaves: skinned, never driven.
        };

        // Ultraleap HandBinder rotation offsets for this model, from GhostHands.prefab
        // (Euler degrees; applied as jointRotation * Euler(offset) in world space).
        // The finger and wrist offsets are identical per hand in that prefab.
        static readonly Vector3 k_OffsetLeft  = new Vector3(359.7198f, 267.04218f, 177.9738f);
        static readonly Vector3 k_OffsetRight = new Vector3(359.7198f, 92.95782f, 182.0262f);

        const string k_ResourcePath = "DisplayXRGhostHand/";

        class HandVisual
        {
            public bool isRight;
            public Transform root;      // mirrored (-1,1,1) for the right hand, scaled by the pose scale
            public Transform wrist;
            public Transform[] bones;   // indexed like s_Bindings; null when the name was not found
            public Quaternion offset;
            public bool visible;
        }

        HandVisual m_Left, m_Right;
        XRHandSubsystem m_Subsystem;
        readonly List<XRHandSubsystem> m_Found = new List<XRHandSubsystem>();
        DisplayXRHandJointGizmos m_Fallback;
        Transform m_ParentedTo;

        void Start()
        {
            bool hdrp = GraphicsSettings.currentRenderPipeline != null
                     && GraphicsSettings.currentRenderPipeline.GetType().FullName.Contains("HighDefinition");
            if (visual == Visual.JointSpheres || hdrp)
            {
                if (hdrp) Debug.Log("[DisplayXR] Ghost Hand: HDRP project — the sample ships Built-in and URP shaders only; using the joint-sphere visual");
                UseFallback();
                return;
            }

            var prefab = Resources.Load<GameObject>(k_ResourcePath + "GenericHand");
            var material = materialOverride != null ? materialOverride : LoadMaterialForPipeline();
            if (prefab == null || material == null)
            {
                Debug.LogWarning("[DisplayXR] Ghost Hand: model or material not found under Resources/" + k_ResourcePath +
                                 " — was the sample imported completely? Using the joint-sphere visual");
                UseFallback();
                return;
            }

            m_Left  = Build(prefab, material, false);
            m_Right = Build(prefab, material, true);
            if (m_Left == null || m_Right == null) { UseFallback(); return; }
        }

        void UseFallback()
        {
            m_Fallback = gameObject.GetComponent<DisplayXRHandJointGizmos>();
            if (m_Fallback == null) m_Fallback = gameObject.AddComponent<DisplayXRHandJointGizmos>();
            m_Fallback.rigOverride = rigOverride;
            m_Fallback.showStatus = showStatus;
            enabled = false;
        }

        static Material LoadMaterialForPipeline()
        {
            var rp = GraphicsSettings.currentRenderPipeline;
            bool urp = rp != null && rp.GetType().FullName.Contains("Universal");
            return Resources.Load<Material>(k_ResourcePath + (urp ? "Ghost Hand URP" : "Ghost Hand"));
        }

        HandVisual Build(GameObject prefab, Material material, bool isRight)
        {
            var hv = new HandVisual { isRight = isRight };
            var rootGo = new GameObject(isRight ? "Ghost Hand Right" : "Ghost Hand Left");
            hv.root = rootGo.transform;
            hv.root.SetParent(transform, false);

            var model = Instantiate(prefab, hv.root);
            model.name = "GenericHand";
            model.transform.localPosition = Vector3.zero;
            model.transform.localRotation = Quaternion.identity;
            model.transform.localScale = Vector3.one;

            foreach (var r in model.GetComponentsInChildren<Renderer>(true))
            {
                r.sharedMaterial = material;
                if (r is SkinnedMeshRenderer smr) smr.updateWhenOffscreen = true; // bones travel far from the bind root
            }

            hv.wrist = FindDeep(model.transform, k_WristBone);
            if (hv.wrist == null)
            {
                Debug.LogWarning("[DisplayXR] Ghost Hand: bone '" + k_WristBone + "' not found in the model; using the joint-sphere visual");
                Destroy(rootGo);
                return null;
            }
            hv.bones = new Transform[s_Bindings.Length];
            int missing = 0;
            for (int i = 0; i < s_Bindings.Length; i++)
            {
                hv.bones[i] = FindDeep(model.transform, s_Bindings[i].bone);
                if (hv.bones[i] == null) missing++;
            }
            if (missing > 0)
                Debug.LogWarning("[DisplayXR] Ghost Hand: " + missing + " bone(s) not found by name; those joints stay at rest");

            hv.offset = Quaternion.Euler(isRight ? k_OffsetRight : k_OffsetLeft);
            hv.root.gameObject.SetActive(false);
            return hv;
        }

        static Transform FindDeep(Transform t, string name)
        {
            if (t.name == name) return t;
            for (int i = 0; i < t.childCount; i++)
            {
                var r = FindDeep(t.GetChild(i), name);
                if (r != null) return r;
            }
            return null;
        }

        void OnDisable()
        {
            if (m_Subsystem != null) m_Subsystem.updatedHands -= OnUpdatedHands;
            m_Subsystem = null;
        }

        void Update()
        {
            // (Re)acquire the running subsystem: it comes up with the display loader, a
            // frame or two into Play, and restarts across an editor dock/undock.
            if (m_Subsystem == null || !m_Subsystem.running)
            {
                if (m_Subsystem != null) m_Subsystem.updatedHands -= OnUpdatedHands;
                m_Subsystem = null;
                m_Found.Clear();
                SubsystemManager.GetSubsystems(m_Found);
                foreach (var s in m_Found)
                    if (s.running) { m_Subsystem = s; break; }
                if (m_Subsystem != null) m_Subsystem.updatedHands += OnUpdatedHands;
                if (m_Subsystem == null) { Show(m_Left, false); Show(m_Right, false); }
            }
        }

        // Fired after each TryUpdateHands — both the XRUpdate and the before-render one, so
        // the skinning picks up the before-render sample.
        void OnUpdatedHands(XRHandSubsystem subsystem, XRHandSubsystem.UpdateSuccessFlags flags, XRHandSubsystem.UpdateType updateType)
        {
            Transform rig = rigOverride != null ? rigOverride
                          : (DisplayXRRigManager.ActiveCamera != null ? DisplayXRRigManager.ActiveCamera.transform : null);
            if (rig == null) { Show(m_Left, false); Show(m_Right, false); return; }

            // The hand roots live UNDER the rig camera: the joint poses are relative to it,
            // its scale (scale-as-zoom) then applies to the model's bone lengths, and the
            // right hand's mirror plane rotates with it — which is what makes Ultraleap's
            // world-space rotation offsets valid under any rig orientation.
            if (m_ParentedTo != rig)
            {
                m_Left.root.SetParent(rig, false);
                m_Right.root.SetParent(rig, false);
                m_ParentedTo = rig;
            }
            float s = DisplayXRHandTracking.CurrentPoseScale;
            m_Left.root.localScale  = new Vector3(s, s, s);
            m_Right.root.localScale = new Vector3(-s, s, s);

            Apply(m_Left, subsystem.leftHand, rig);
            Apply(m_Right, subsystem.rightHand, rig);
        }

        void Apply(HandVisual hv, XRHand hand, Transform rig)
        {
            if (!hand.isTracked
             || !hand.GetJoint(XRHandJointID.Wrist).TryGetPose(out Pose wristPose)
             || !hand.GetJoint(XRHandJointID.Palm).TryGetPose(out Pose palmPose))
            {
                Show(hv, false);
                return;
            }
            Show(hv, true);

            // Wrist: positioned at the wrist joint, oriented by the PALM joint (Leap's hand
            // rotation, which is what HandBinder uses for the wrist bone) + the offset.
            hv.wrist.position = rig.TransformPoint(wristPose.position);
            hv.wrist.rotation = rig.rotation * palmPose.rotation * hv.offset;

            for (int i = 0; i < s_Bindings.Length; i++)
            {
                var bone = hv.bones[i];
                if (bone == null) continue;
                if (s_Bindings[i].metacarpal && !driveMetacarpals) continue;
                if (!hand.GetJoint(s_Bindings[i].joint).TryGetPose(out Pose p)) continue;
                if (setBonePositions) bone.position = rig.TransformPoint(p.position);
                bone.rotation = rig.rotation * p.rotation * hv.offset;
            }
        }

        static void Show(HandVisual hv, bool show)
        {
            if (hv == null || hv.visible == show) return;
            hv.root.gameObject.SetActive(show);
            hv.visible = show;
        }

        void OnGUI()
        {
            if (!showStatus || m_Fallback != null) return;
            GUI.Label(new Rect(10, 10, Screen.width - 20, 24), DisplayXRHandJointGizmos.StatusText(m_Subsystem));
        }
    }
}
