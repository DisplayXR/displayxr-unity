// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEditor;
using UnityEngine;
using DisplayXR;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Inspector for <see cref="DisplayXRStereoMedia"/>: the settings grouped by what they
    /// do, a preview of how the texture is split between the eyes, and a hint when the
    /// texture's shape does not look like the chosen layout.
    /// </summary>
    [CustomEditor(typeof(DisplayXRStereoMedia))]
    [CanEditMultipleObjects]
    public class DisplayXRStereoMediaEditor : UnityEditor.Editor
    {
        SerializedProperty m_Texture, m_VideoPlayer, m_PrepareVideo, m_Encoding, m_FlipVertical;
        SerializedProperty m_Layout, m_HalfResolution, m_PixelAspect, m_DepthShift, m_ForceMono;
        SerializedProperty m_Presentation, m_Fit, m_AlphaMode, m_Tint;
        SerializedProperty m_Placement, m_RigCamera, m_QuadSize;
        SerializedProperty m_FullScreenOrder, m_ViewAspect;

        static readonly Color k_LeftColor = new Color(1f, 0.35f, 0.3f);
        static readonly Color k_RightColor = new Color(0.3f, 0.6f, 1f);
        static GUIStyle s_Caption;

        void OnEnable()
        {
            if (target == null) return;
            m_Texture = serializedObject.FindProperty("texture");
            // Absent when the project has the Video module disabled.
            m_VideoPlayer = serializedObject.FindProperty("videoPlayer");
            m_PrepareVideo = serializedObject.FindProperty("prepareVideo");
            m_Encoding = serializedObject.FindProperty("encoding");
            m_FlipVertical = serializedObject.FindProperty("flipVertical");
            m_Layout = serializedObject.FindProperty("layout");
            m_HalfResolution = serializedObject.FindProperty("halfResolution");
            m_PixelAspect = serializedObject.FindProperty("pixelAspect");
            m_DepthShift = serializedObject.FindProperty("depthShiftPercent");
            m_ForceMono = serializedObject.FindProperty("forceMono");
            m_Presentation = serializedObject.FindProperty("presentation");
            m_Fit = serializedObject.FindProperty("fit");
            m_AlphaMode = serializedObject.FindProperty("alphaMode");
            m_Tint = serializedObject.FindProperty("tint");
            m_Placement = serializedObject.FindProperty("placement");
            m_RigCamera = serializedObject.FindProperty("rigCamera");
            m_QuadSize = serializedObject.FindProperty("quadSize");
            m_FullScreenOrder = serializedObject.FindProperty("fullScreenOrder");
            m_ViewAspect = serializedObject.FindProperty("viewAspect");
        }

        public override void OnInspectorGUI()
        {
            serializedObject.Update();
            var media = (DisplayXRStereoMedia)target;

            // The section headings come from the [Header] attributes on the fields.
            bool hasVideo = m_VideoPlayer != null && m_VideoPlayer.objectReferenceValue != null;
            using (new EditorGUI.DisabledScope(hasVideo))
                EditorGUILayout.PropertyField(m_Texture);
            if (m_VideoPlayer != null)
            {
                EditorGUILayout.PropertyField(m_VideoPlayer);
                if (hasVideo)
                {
                    EditorGUI.indentLevel++;
                    EditorGUILayout.PropertyField(m_PrepareVideo);
                    EditorGUI.indentLevel--;
                }
            }
            else
            {
                EditorGUILayout.HelpBox("Video sources need Unity's built-in Video module " +
                                        "(Package Manager > Built-in > Video), which is disabled in this project.",
                                        MessageType.None);
            }
            EditorGUILayout.PropertyField(m_Encoding);
            EditorGUILayout.PropertyField(m_FlipVertical);

            EditorGUILayout.PropertyField(m_Layout);
            bool mono = !m_Layout.hasMultipleDifferentValues &&
                        m_Layout.enumValueIndex == (int)DisplayXRStereoLayout.Mono;
            using (new EditorGUI.DisabledScope(mono))
            {
                EditorGUILayout.PropertyField(m_HalfResolution,
                    new GUIContent("Half Resolution (Squeezed)", m_HalfResolution.tooltip));
                EditorGUILayout.PropertyField(m_DepthShift, new GUIContent("Depth Shift (%)", m_DepthShift.tooltip));
            }
            EditorGUILayout.PropertyField(m_PixelAspect);
            EditorGUILayout.PropertyField(m_ForceMono);

            if (!serializedObject.isEditingMultipleObjects)
                DrawSplitPreview(media);

            EditorGUILayout.PropertyField(m_Presentation);
            EditorGUILayout.PropertyField(m_Fit);
            EditorGUILayout.PropertyField(m_AlphaMode);
            EditorGUILayout.PropertyField(m_Tint);

            bool mixed = m_Presentation.hasMultipleDifferentValues;
            bool fullScreen = m_Presentation.enumValueIndex == (int)DisplayXRStereoMedia.PresentationMode.FullScreen;
            if (mixed || !fullScreen)
            {
                EditorGUILayout.PropertyField(m_Placement);
                EditorGUILayout.PropertyField(m_RigCamera);
                bool manual = m_Placement.enumValueIndex == (int)DisplayXRStereoMedia.QuadPlacement.Manual;
                EditorGUILayout.PropertyField(m_QuadSize,
                    new GUIContent(manual ? "Quad Size" : "Quad Size (Fallback)", m_QuadSize.tooltip));
                if (!manual && !mixed)
                    EditorGUILayout.HelpBox(
                        "The quad is placed on the rig's zero-parallax plane and sized to it, so the rig adds no " +
                        "disparity and the picture's own disparity is the only one. This object's transform is not used.",
                        MessageType.None);
            }
            if (mixed || fullScreen)
            {
                EditorGUILayout.PropertyField(m_FullScreenOrder, new GUIContent("Order", m_FullScreenOrder.tooltip));
                EditorGUILayout.PropertyField(m_ViewAspect);
                if (!mixed)
                    EditorGUILayout.HelpBox(
                        "Each eye's half goes straight to that eye's render target: no added parallax, nothing moves " +
                        "with the head. Drawn for Game cameras only" +
                        (m_RigCamera.objectReferenceValue != null ? " (the Rig Camera assigned under Quad)." : "."),
                        MessageType.None);
            }

            serializedObject.ApplyModifiedProperties();

            if (Application.isPlaying && !serializedObject.isEditingMultipleObjects)
            {
                EditorGUILayout.Space();
                EditorGUILayout.LabelField("Runtime Status", EditorStyles.boldLabel);
                EditorGUILayout.LabelField("Showing", media.IsStereo ? "Stereo (one picture per eye)" : "Mono (left picture to both eyes)");
                Texture t = media.CurrentTexture;
                EditorGUILayout.LabelField("Texture", t != null ? $"{t.width} x {t.height}" : "none yet");
                if (media.PictureAspect > 0f)
                    EditorGUILayout.LabelField("Picture Aspect", media.PictureAspect.ToString("F3"));
            }
        }

        // The texture as stored, with each eye's part outlined, then the two parts as the
        // eyes will get them.
        void DrawSplitPreview(DisplayXRStereoMedia media)
        {
            Texture tex = PreviewTexture(media);
            if (tex == null || tex.width <= 0 || tex.height <= 0)
            {
                EditorGUILayout.HelpBox("Assign a texture" + (m_VideoPlayer != null ? " or a Video Player" : "") +
                                        " to preview the split.", MessageType.None);
                return;
            }

            var layout = (DisplayXRStereoLayout)m_Layout.enumValueIndex;
            bool half = m_HalfResolution.boolValue;
            bool flip = m_FlipVertical.boolValue;

            DrawHint(tex, layout, half);

            s_Caption ??= new GUIStyle(EditorStyles.miniLabel) { alignment = TextAnchor.MiddleCenter };

            EditorGUILayout.Space(2);
            float width = Mathf.Max(64f, EditorGUIUtility.currentViewWidth - 40f);

            // --- the whole texture, eye parts outlined ---
            float wholeAspect = (float)tex.width / tex.height;
            float wholeH = Mathf.Clamp(width / wholeAspect, 24f, 160f);
            float wholeW = Mathf.Min(width, wholeH * wholeAspect);
            Rect row = GUILayoutUtility.GetRect(width, wholeH);
            Rect whole = new Rect(row.x + (row.width - wholeW) * 0.5f, row.y, wholeW, wholeH);
            if (Event.current.type == EventType.Repaint)
            {
                // The picture upright: a flipped texture is shown the way the eyes get it.
                GUI.DrawTextureWithTexCoords(whole, tex, flip ? new Rect(0f, 1f, 1f, -1f) : new Rect(0f, 0f, 1f, 1f));
                if (layout != DisplayXRStereoLayout.Mono)
                {
                    Outline(whole, DisplayXRStereoMediaMath.EyeRect(layout, false), k_LeftColor, "L");
                    Outline(whole, DisplayXRStereoMediaMath.EyeRect(layout, true), k_RightColor, "R");
                }
            }

            // --- what each eye gets, at its displayed aspect ---
            float pixelAspect = m_PixelAspect.floatValue;
            float eyeAspect = DisplayXRStereoMediaMath.EyeAspect(tex.width, tex.height, layout, half, pixelAspect);
            if (eyeAspect <= 0f) return;
            var sol = DisplayXRStereoMediaMath.Solve(tex.width, tex.height, layout, half, pixelAspect,
                DisplayXRStereoFit.Stretch, 0f, m_DepthShift.floatValue * 0.01f, flip, !m_ForceMono.boolValue);

            float gap = 8f;
            float eyeW = (width - gap) * 0.5f;
            float eyeH = Mathf.Clamp(eyeW / sol.pictureAspect, 24f, 140f);
            eyeW = Mathf.Min(eyeW, eyeH * sol.pictureAspect);
            EditorGUILayout.Space(4);
            Rect eyes = GUILayoutUtility.GetRect(width, eyeH + 16f);
            float x0 = eyes.x + (eyes.width - (eyeW * 2f + gap)) * 0.5f;
            Rect left = new Rect(x0, eyes.y, eyeW, eyeH);
            Rect right = new Rect(x0 + eyeW + gap, eyes.y, eyeW, eyeH);
            if (Event.current.type == EventType.Repaint)
            {
                GUI.DrawTextureWithTexCoords(left, tex, ToRect(sol.left));
                GUI.DrawTextureWithTexCoords(right, tex, ToRect(sol.right));
                Frame(left, k_LeftColor);
                Frame(right, k_RightColor);
            }
            GUI.Label(new Rect(left.x, left.yMax, left.width, 16f), "Left eye", s_Caption);
            GUI.Label(new Rect(right.x, right.yMax, right.width, 16f),
                      m_ForceMono.boolValue && layout != DisplayXRStereoLayout.Mono ? "Right eye (mono: left picture)" : "Right eye",
                      s_Caption);

            EditorGUILayout.LabelField(" ",
                $"{tex.width} x {tex.height} texture, {eyeAspect:F2}:1 per eye" +
                (Application.isPlaying ? "" : "  (in Play, mono is also used when the session is not stereo)"),
                EditorStyles.miniLabel);
        }

        Texture PreviewTexture(DisplayXRStereoMedia media)
        {
            // In Play the component knows the live texture (the video's included).
            if (Application.isPlaying && media.CurrentTexture != null) return media.CurrentTexture;
            return m_Texture.objectReferenceValue as Texture;
        }

        void DrawHint(Texture tex, DisplayXRStereoLayout layout, bool half)
        {
            string axis = layout == DisplayXRStereoLayout.TopBottom || layout == DisplayXRStereoLayout.TopBottomSwapped
                ? "height" : "width";
            float eye = DisplayXRStereoMediaMath.EyeAspect(tex.width, tex.height, layout, half, 1f);
            string message = null;
            switch (DisplayXRStereoMediaMath.DiagnoseLayout(tex.width, tex.height, layout, half))
            {
                case DisplayXRStereoLayoutHint.LooksHalfResolution:
                    message = $"A {tex.width} x {tex.height} texture gives each eye a {eye:F2}:1 picture with this layout. " +
                              $"It looks like half-{axis} (squeezed) media: turn on Half Resolution.";
                    break;
                case DisplayXRStereoLayoutHint.LooksFullResolution:
                    message = $"A {tex.width} x {tex.height} texture gives each eye a {eye:F2}:1 picture with Half Resolution on. " +
                              $"It looks like full-{axis} media: turn Half Resolution off.";
                    break;
                case DisplayXRStereoLayoutHint.LooksSideBySide:
                    message = $"A {tex.width} x {tex.height} texture is very wide for one picture. It looks like a " +
                              "side-by-side pair: set Layout to Side By Side.";
                    break;
                case DisplayXRStereoLayoutHint.LooksTopBottom:
                    message = $"A {tex.width} x {tex.height} texture has the shape of two 16:9 pictures stacked. " +
                              "If it is a top-bottom pair, set Layout to Top Bottom.";
                    break;
            }
            if (message != null)
                EditorGUILayout.HelpBox(message, MessageType.Warning);
        }

        // An EyeMapping as the texCoords rect IMGUI takes (a negative height mirrors).
        static Rect ToRect(DisplayXRStereoMediaMath.EyeMapping m) =>
            new Rect(m.offsetX, m.offsetY, m.scaleX, m.scaleY);

        // imageRect is in image space (origin bottom-left); GUI rects have their origin top-left.
        static void Outline(Rect area, DisplayXRStereoMediaMath.UVRect imageRect, Color color, string label)
        {
            var r = new Rect(area.x + imageRect.x * area.width,
                             area.y + (1f - imageRect.y - imageRect.height) * area.height,
                             imageRect.width * area.width, imageRect.height * area.height);
            Frame(r, color);
            var badge = new Rect(r.x + 3f, r.y + 3f, 16f, 14f);
            EditorGUI.DrawRect(badge, color);
            GUI.Label(badge, label, s_Caption);
        }

        static void Frame(Rect r, Color color)
        {
            const float t = 2f;
            EditorGUI.DrawRect(new Rect(r.x, r.y, r.width, t), color);
            EditorGUI.DrawRect(new Rect(r.x, r.yMax - t, r.width, t), color);
            EditorGUI.DrawRect(new Rect(r.x, r.y, t, r.height), color);
            EditorGUI.DrawRect(new Rect(r.xMax - t, r.y, t, r.height), color);
        }
    }
}
