// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// (issue #57) Opt-in chroma-key transparent overlay mode for Windows
    /// standalone builds. Drives the avatar/desktop-overlay use case.
    ///
    /// Background: the Leia weaver writes opaque RGB only and the DisplayXR
    /// D3D11 compositor uses DXGI_ALPHA_MODE_IGNORE, so per-pixel alpha
    /// doesn't survive end-to-end. Instead, this component renders a magic
    /// color (default magenta) in transparent regions of both eye views and
    /// asks the native plugin to flip the parent HWND to
    /// WS_POPUP | WS_EX_LAYERED with LWA_COLORKEY so DWM punches those pixels
    /// through to the desktop.
    ///
    /// Click-through outside <see cref="clickableRenderers"/> is provided by a
    /// rectangular WM_NCHITTEST gate (see displayxr_set_overlay_hit_rect).
    ///
    /// Windows only. No-op in the editor preview window and on macOS.
    /// </summary>
    [AddComponentMenu("DisplayXR/Transparent Overlay (Windows)")]
    [RequireComponent(typeof(Camera))]
    public class DisplayXRTransparentOverlay : MonoBehaviour
    {
        [Header("Chroma key")]

        [Tooltip("Color rendered in transparent regions. Plugin clears the " +
                 "camera to this RGB; the runtime's post-weave shader pass " +
                 "(spec v5, runtime-pvt #191) writes alpha=0 for matching " +
                 "pixels before Present so DComp/DWM blends per-pixel. " +
                 "Magenta (1,0,1) is the standard pick — pure primary, no " +
                 "sRGB round-trip drift, unlikely to appear in real content.")]
        public Color chromaKeyColor = new Color(1f, 0f, 1f, 0f);

        [Header("Window")]

        [Tooltip("Keep the window above all others (WS_EX_TOPMOST).")]
        public bool alwaysOnTop = true;

        [Header("Hit testing")]

        [Tooltip("Renderers whose screen-space bounds are clickable. Clicks " +
                 "outside the union bounding box fall through to whatever " +
                 "is behind the window. Empty = whole window stays clickable.")]
        public Renderer[] clickableRenderers;

        private Camera m_Camera;
        private CameraClearFlags m_SavedClearFlags;
        private Color m_SavedBackgroundColor;
        private bool m_SavedRestore;

        /// <summary>
        /// Request the runtime's transparent-background mode for the next
        /// OpenXR session. MUST be called BEFORE the OpenXR session is
        /// created — practically, from a static
        /// [RuntimeInitializeOnLoadMethod(SubsystemRegistration)] in the
        /// app's bootstrap code. Calling it from this component's OnEnable
        /// is too late: the session is already up by then.
        ///
        /// Example:
        /// <code>
        /// [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        /// static void EnableTransparentSession() {
        ///     DisplayXRTransparentOverlay.RequestTransparentSession();
        /// }
        /// </code>
        /// </summary>
        public static void RequestTransparentSession()
        {
#if UNITY_STANDALONE_WIN
            DisplayXRNative.displayxr_set_transparent_background(1);
#endif
        }

        /// <summary>
        /// Set the chroma-key color the runtime should convert to alpha=0 in
        /// its post-weave pass (spec v5 / runtime-pvt #191). Same timing
        /// constraint as <see cref="RequestTransparentSession"/> — must be
        /// called BEFORE the OpenXR session is created.
        /// </summary>
        /// <param name="color">RGBA. Alpha is ignored (chroma key is RGB-only).
        /// Pass the same color as the camera clear so the plugin and runtime agree
        /// on what counts as a transparent pixel.</param>
        public static void RequestChromaKey(Color color)
        {
#if UNITY_STANDALONE_WIN
            DisplayXRNative.displayxr_set_transparent_chroma_key(ColorRefFromColor(color));
#endif
        }

        void OnEnable()
        {
            m_Camera = GetComponent<Camera>();
            if (m_Camera == null)
                return;

            // Always switch the camera to solid-color clear so transparent
            // regions get the chroma key — even in the editor preview, where
            // the layered-window path doesn't run. That makes the preview
            // visually represent what the build will look like.
            m_SavedClearFlags      = m_Camera.clearFlags;
            m_SavedBackgroundColor = m_Camera.backgroundColor;
            m_SavedRestore         = true;
            m_Camera.clearFlags      = CameraClearFlags.SolidColor;
            m_Camera.backgroundColor = chromaKeyColor;

#if UNITY_STANDALONE_WIN
            // Layered-window mode is build-only: no top-level Unity HWND we
            // want to mutate from the editor, and the preview window isn't
            // the right target.
            if (Application.isEditor)
                return;

            uint colorRef = ColorRefFromColor(chromaKeyColor);
            DisplayXRNative.displayxr_set_transparent_overlay(
                1, colorRef, alwaysOnTop ? 1 : 0);
            // Default hit rect = whole window until LateUpdate refines it.
            DisplayXRNative.displayxr_set_overlay_hit_rect(
                0, 0, Screen.width, Screen.height);
#endif
        }

        void LateUpdate()
        {
#if UNITY_STANDALONE_WIN
            if (Application.isEditor || m_Camera == null)
                return;
            if (clickableRenderers == null || clickableRenderers.Length == 0)
                return;

            if (!TryGetScreenRect(out int x, out int y, out int w, out int h))
                return;

            DisplayXRNative.displayxr_set_overlay_hit_rect(x, y, w, h);
#endif
        }

        void OnDisable()
        {
#if UNITY_STANDALONE_WIN
            if (!Application.isEditor)
                DisplayXRNative.displayxr_set_transparent_overlay(0, 0, 0);
#endif
            if (m_SavedRestore && m_Camera != null)
            {
                m_Camera.clearFlags      = m_SavedClearFlags;
                m_Camera.backgroundColor = m_SavedBackgroundColor;
                m_SavedRestore = false;
            }
        }

        // Pack a Unity Color into a Windows COLORREF (0x00BBGGRR).
        private static uint ColorRefFromColor(Color c)
        {
            uint r = (uint)Mathf.Clamp(Mathf.RoundToInt(c.r * 255f), 0, 255);
            uint g = (uint)Mathf.Clamp(Mathf.RoundToInt(c.g * 255f), 0, 255);
            uint b = (uint)Mathf.Clamp(Mathf.RoundToInt(c.b * 255f), 0, 255);
            return (b << 16) | (g << 8) | r;
        }

        // Project the union of clickableRenderers' world-space bounds into
        // top-left-origin screen-space pixels. Returns false when fully
        // off-screen.
        private bool TryGetScreenRect(out int x, out int y, out int w, out int h)
        {
            x = y = w = h = 0;

            float minX = float.PositiveInfinity, minY = float.PositiveInfinity;
            float maxX = float.NegativeInfinity, maxY = float.NegativeInfinity;
            bool any = false;

            for (int i = 0; i < clickableRenderers.Length; i++)
            {
                var r = clickableRenderers[i];
                if (r == null || !r.enabled || !r.gameObject.activeInHierarchy)
                    continue;

                Bounds b = r.bounds;
                Vector3 c = b.center;
                Vector3 e = b.extents;

                // 8 corners of the AABB → screen space, take min/max.
                for (int k = 0; k < 8; k++)
                {
                    Vector3 corner = c + new Vector3(
                        ((k & 1) == 0 ? -e.x : e.x),
                        ((k & 2) == 0 ? -e.y : e.y),
                        ((k & 4) == 0 ? -e.z : e.z));
                    Vector3 sp = m_Camera.WorldToScreenPoint(corner);
                    if (sp.z <= 0f)
                        continue; // behind camera
                    if (sp.x < minX) minX = sp.x;
                    if (sp.y < minY) minY = sp.y;
                    if (sp.x > maxX) maxX = sp.x;
                    if (sp.y > maxY) maxY = sp.y;
                    any = true;
                }
            }

            if (!any)
                return false;

            // Unity screen origin is bottom-left; Win32 client origin is
            // top-left. Flip Y.
            int sw = Screen.width;
            int sh = Screen.height;

            int xMin = Mathf.Clamp(Mathf.FloorToInt(minX), 0, sw);
            int xMax = Mathf.Clamp(Mathf.CeilToInt(maxX),  0, sw);
            int yMinUnity = Mathf.Clamp(Mathf.FloorToInt(minY), 0, sh);
            int yMaxUnity = Mathf.Clamp(Mathf.CeilToInt(maxY),  0, sh);

            x = xMin;
            w = xMax - xMin;
            y = sh - yMaxUnity;
            h = yMaxUnity - yMinUnity;

            return w > 0 && h > 0;
        }
    }
}
