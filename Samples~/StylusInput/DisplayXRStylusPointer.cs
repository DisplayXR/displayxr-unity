// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Collections.Generic;
using UnityEngine;
using UnityEngine.XR;

namespace DisplayXR.Samples
{
    /// <summary>
    /// Reads a DisplayXR stylus (or simple controller) through the standard
    /// <see cref="InputDevice"/> API and draws with it: a marker follows the pen tip, a
    /// ray follows the aim pose, pressing the tip on the glass draws a stroke whose width
    /// follows the tip pressure, the front button clears the strokes and the back button
    /// pulses the haptics. Put it on any GameObject in a scene with a DisplayXR rig.
    ///
    /// Everything here is ordinary Unity XR input; the only DisplayXR-specific pieces are
    /// <see cref="DisplayXRInput.TryGetStylus"/> (a name filter) and
    /// <see cref="DisplayXRInput.TryGetWorldPose"/> (rig camera transform ∘ device pose).
    /// </summary>
    public class DisplayXRStylusPointer : MonoBehaviour
    {
        [Tooltip("Tip pressure (0..1) above which the pen draws.")]
        public float drawThreshold = 0.05f;
        [Tooltip("Stroke width at full pressure, in world units.")]
        public float maxStrokeWidth = 0.01f;
        [Tooltip("Marker size, in world units.")]
        public float markerSize = 0.008f;
        [Tooltip("Length of the aim ray, in world units.")]
        public float rayLength = 0.3f;
        public Color strokeColor = new Color(0.2f, 0.8f, 1f);

        Transform m_Marker;
        LineRenderer m_Ray;
        LineRenderer m_Stroke;
        readonly List<LineRenderer> m_Strokes = new List<LineRenderer>();
        readonly List<float> m_Pressures = new List<float>();
        bool m_WasFront, m_WasBack;
        string m_Status = "";

        void Start()
        {
            m_Marker = GameObject.CreatePrimitive(PrimitiveType.Sphere).transform;
            Destroy(m_Marker.GetComponent<Collider>());
            m_Marker.SetParent(transform, false);
            m_Marker.localScale = Vector3.one * markerSize;
            m_Ray = NewLine("Aim Ray", Color.white, 0.001f);
        }

        LineRenderer NewLine(string name, Color c, float width)
        {
            var go = new GameObject(name);
            go.transform.SetParent(transform, false);
            var lr = go.AddComponent<LineRenderer>();
            lr.useWorldSpace = true;
            lr.positionCount = 0;
            lr.widthMultiplier = width;
            lr.material = new Material(Shader.Find("Sprites/Default"));
            lr.startColor = lr.endColor = c;
            return lr;
        }

        void Update()
        {
            bool stylus = DisplayXRInput.TryGetStylus(out var dev);
            if (!stylus && !DisplayXRInput.TryGetController(out dev))
            {
                m_Marker.gameObject.SetActive(false);
                m_Ray.positionCount = 0;
                m_Status = "no DisplayXR controller (input " + (DisplayXRInput.IsActive ? "active" : "inactive") +
                    ", stylus profile " + (DisplayXRInput.IsStylusProfileAvailable ? "available" : "not advertised") + ")";
                return;
            }

            dev.TryGetFeatureValue(CommonUsages.isTracked, out bool tracked);
            // The tip pose on a stylus; the grip pose on a simple controller.
            bool haveTip = stylus
                ? DisplayXRInput.TryGetWorldPose(dev, DisplayXRStylusUsages.tipPosition, DisplayXRStylusUsages.tipRotation, out var tip)
                : DisplayXRInput.TryGetWorldPose(dev, CommonUsages.devicePosition, CommonUsages.deviceRotation, out tip);
            bool haveAim = DisplayXRInput.TryGetWorldPose(dev, DisplayXRStylusUsages.pointerPosition,
                DisplayXRStylusUsages.pointerRotation, out var aim);

            m_Marker.gameObject.SetActive(tracked && haveTip);
            if (haveTip) m_Marker.position = tip.position;
            if (tracked && haveAim)
            {
                m_Ray.positionCount = 2;
                m_Ray.SetPosition(0, aim.position);
                m_Ray.SetPosition(1, aim.position + aim.rotation * Vector3.forward * rayLength);
            }
            else m_Ray.positionCount = 0;

            // Pressure: the stylus's tip force; a simple controller's select as 0/1.
            dev.TryGetFeatureValue(CommonUsages.trigger, out float pressure);
            if (tracked && haveTip && pressure > drawThreshold)
            {
                if (m_Stroke == null)
                {
                    m_Stroke = NewLine("Stroke", strokeColor, maxStrokeWidth);
                    m_Strokes.Add(m_Stroke);
                    m_Pressures.Clear();
                }
                int n = m_Stroke.positionCount;
                if (n == 0 || Vector3.Distance(m_Stroke.GetPosition(n - 1), tip.position) > maxStrokeWidth * 0.25f)
                {
                    m_Stroke.positionCount = n + 1;
                    m_Stroke.SetPosition(n, tip.position);
                    m_Pressures.Add(Mathf.Clamp01(pressure));
                    // Width follows the pressure along the stroke (keys over normalised length).
                    var keys = new Keyframe[m_Pressures.Count];
                    for (int i = 0; i < keys.Length; i++)
                        keys[i] = new Keyframe(keys.Length > 1 ? i / (float)(keys.Length - 1) : 0f, m_Pressures[i]);
                    m_Stroke.widthCurve = new AnimationCurve(keys);
                }
            }
            else m_Stroke = null;

            dev.TryGetFeatureValue(CommonUsages.primaryButton, out bool front);
            dev.TryGetFeatureValue(CommonUsages.secondaryButton, out bool back);
            if (front && !m_WasFront)
            {
                foreach (var s in m_Strokes) Destroy(s.gameObject);
                m_Strokes.Clear();
                m_Stroke = null;
                dev.SendHapticImpulse(0, 0.3f, 0.03f);
            }
            if (back && !m_WasBack) dev.SendHapticImpulse(0, 0.8f, 0.1f);
            m_WasFront = front;
            m_WasBack = back;

            dev.TryGetFeatureValue(DisplayXRStylusUsages.middlePressure, out float middle);
            dev.TryGetFeatureValue(DisplayXRStylusUsages.docked, out bool docked);
            m_Status = dev.name + (tracked ? " TRACKED" : " not tracked") + "  rig=" + DisplayXRInput.RigKind +
                "  tip=" + pressure.ToString("0.00") + (stylus ? "  middle=" + middle.ToString("0.00") +
                (docked ? "  DOCKED" : "") : "") + (front ? "  [front]" : "") + (back ? "  [back]" : "");
        }

        void OnGUI()
        {
            GUI.Label(new Rect(10, 10, 900, 24), "DisplayXR stylus: " + m_Status);
        }
    }
}
