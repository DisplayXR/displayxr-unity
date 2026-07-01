// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
using UnityEngine.XR.OpenXR;

namespace DisplayXR
{
    /// <summary>
    /// Submits a Unity UI Canvas as an OpenXR XrCompositionLayerLocal2DEXT
    /// composition layer (#439/#491) — the modern, mask-based 2D-over-3D path.
    /// The runtime composites this "glass over 3D": the woven 3D under the
    /// layer's pixel rect goes flat 2D (implicit mask) and the Canvas content is
    /// alpha-composited on top. This is what the native VK avatar demo uses for
    /// its speech bubble, and it replaces the legacy 2D-surround handoff
    /// (<see cref="DisplayXRSurround"/>) for in-canvas 2D content — sidestepping
    /// the surround path's stale-RenderTexture-pointer fragility (the bubble
    /// vanishing when Unity reallocated the surround RT): this path recreates
    /// its overlay swapchain whenever the RT changes.
    ///
    /// Like <see cref="DisplayXRWindowSpaceUI"/> this takes over the Canvas it's
    /// attached to and drives it as a private WorldSpace canvas rendered by a
    /// dedicated orthographic camera into an offscreen RenderTexture. Give it its
    /// own Canvas GameObject.
    ///
    /// Position is authored in fractional window coords [0..1]; the component
    /// converts to the client-window pixel rect the Local2D layer requires using
    /// the live panel pixel size. Scoped to the hooked path (built apps); the
    /// editor/standalone-preview path is a follow-up (no cross-device bridge here).
    /// </summary>
    [AddComponentMenu("DisplayXR/Local 2D")]
    [RequireComponent(typeof(Canvas))]
    [ExecuteAlways]
    public class DisplayXRLocal2D : MonoBehaviour
    {
        [Header("Window Position (fractional 0..1)")]

        [Tooltip("Left edge as fraction of window width.")]
        [Range(0f, 1f)] public float positionX = 0.35f;

        [Tooltip("Top edge as fraction of window height.")]
        [Range(0f, 1f)] public float positionY = 0.05f;

        [Tooltip("Width as fraction of window width.")]
        [Range(0f, 1f)] public float width = 0.3f;

        [Tooltip("Height as fraction of window height.")]
        [Range(0f, 1f)] public float height = 0.18f;

        [Header("Explicit pixel rect (overrides fractional)")]

        [Tooltip("When set, the dest rect is taken from ExplicitRect (client-window " +
                 "pixels, top-left) instead of the fractional position above. Use when " +
                 "the caller already computes an exact panel-pixel rect (e.g. a region " +
                 "editor). Set via SetExplicitRect each frame.")]
        public bool useExplicitRect;

        /// <summary>Dest rect in client-window pixels (top-left origin) when
        /// <see cref="useExplicitRect"/> is true.</summary>
        public RectInt ExplicitRect;

        /// <summary>Set the explicit dest rect (client-window pixels) and enable
        /// explicit-rect mode. Safe to call every frame.</summary>
        public void SetExplicitRect(int x, int y, int w, int h)
        {
            useExplicitRect = true;
            ExplicitRect = new RectInt(x, y, w, h);
        }

        [Header("Render Settings")]

        [Tooltip("Resolution of the overlay RenderTexture.")]
        public Vector2Int resolution = new Vector2Int(1024, 512);

        /// <summary>The RenderTexture capturing the Canvas content.</summary>
        public RenderTexture OverlayTexture { get; private set; }

        private static readonly Vector3 kCanvasWorldPos = new Vector3(0, 100000f, 0);
        private const int kPrivateLayer = 30;

        private Canvas m_Canvas;
        private RectTransform m_CanvasRect;
        private Camera m_OverlayCamera;

        private RenderMode m_OrigRenderMode;
        private Vector3 m_OrigCanvasPos;
        private Quaternion m_OrigCanvasRot;
        private Vector3 m_OrigCanvasScale;
        private int m_OrigCanvasLayer;
        private Camera m_OrigCanvasWorldCamera;
        private bool m_StateSaved;

        private int m_LastRectX = int.MinValue, m_LastRectY, m_LastRectW, m_LastRectH;

        // Provider mode (#166): the custom Display Provider owns a separate D3D12
        // device, so it exposes its own cross-device Local2D bridge (like wsui). We
        // Graphics.CopyTexture the overlay RT into it each frame instead of handing the
        // RT pointer to the hook-path displayxr_local2d_set_texture.
        private bool m_ProviderMode;
        private Texture2D m_BridgeTex;

        // The opt-in URP foreground clip (DisplayXR/ForegroundClipURP) is a built-in
        // FullScreenPassRendererFeature with NO XR-camera guard (unlike
        // KooimaProjectionFixFeature, which returns early on !cameraData.xr.enabled).
        // With a single default renderer it therefore also runs on THIS overlay
        // camera — clipping the bubble against the main rig's per-eye foreground far
        // (a different camera space), so the bubble vanishes when the 3D content
        // crosses the display plane (zoom-in / head-forward). The clip is enabled via
        // the _DXRForegroundFar global (z>0.5); we save+zero it for our render and
        // restore after, so the clip only ever applies to the projection/XR camera.
        // Inert in BiRP (RenderPipelineManager doesn't fire; the global is unset).
        private static readonly int s_ForegroundFarId = Shader.PropertyToID("_DXRForegroundFar");
        private Vector4 m_SavedForegroundFar;
        private bool m_CamRenderHooked;

        void OnEnable()
        {
            // Submitting an XrCompositionLayerLocal2DEXT when XR_EXT_local_3d_zone
            // isn't enabled makes the runtime reject the WHOLE xrEndFrame
            // (XR_ERROR_LAYER_INVALID) — black screen. Gate on it: if the runtime
            // doesn't support Local2D, stay inert (no RT, no submission) so the
            // rest of the scene renders normally.
            //
            // Provider mode (#166): Unity's OpenXR loader isn't active, so
            // OpenXRRuntime.IsExtensionEnabled is always false — gate on the provider
            // instead (it enables XR_EXT_local_3d_zone on its own instance). Without
            // this branch the component early-returns and the Canvas falls through to
            // the main eye texture (the "bubble pollutes the 3D" symptom).
            m_ProviderMode = DisplayXRProviderDriver.IsActive;
            if (!m_ProviderMode && !OpenXRRuntime.IsExtensionEnabled("XR_EXT_local_3d_zone"))
            {
                Debug.LogWarning("[DisplayXR] Local2D: XR_EXT_local_3d_zone not enabled — " +
                                 "bubble layer disabled (runtime lacks Local2D support).");
                return;
            }

            m_Canvas = GetComponent<Canvas>();
            m_CanvasRect = m_Canvas.GetComponent<RectTransform>();

            // A CanvasScaler fights the WorldSpace transform this component drives:
            // it rewrites canvas.scaleFactor / referencePixelsPerUnit from the live
            // screen size every frame. On a full-screen transparent overlay the
            // reported screen size can flap during setup/resize, intermittently
            // mis-scaling the content off the ortho camera's frustum → the RT
            // renders blank that frame (the "bubble shows up intermittently" bug).
            // We own the canvas transform fully, so neutralize any scaler.
            var scaler = GetComponent<UnityEngine.UI.CanvasScaler>();
            if (scaler != null && scaler.enabled)
            {
                scaler.enabled = false;
                Debug.Log("[DisplayXR] Local2D: disabled CanvasScaler (component drives the WorldSpace transform).");
            }

            m_OrigRenderMode = m_Canvas.renderMode;
            m_OrigCanvasPos = m_CanvasRect.position;
            m_OrigCanvasRot = m_CanvasRect.rotation;
            m_OrigCanvasScale = m_CanvasRect.localScale;
            m_OrigCanvasLayer = gameObject.layer;
            m_OrigCanvasWorldCamera = m_Canvas.worldCamera;
            m_StateSaved = true;

            // RT with explicit depth-stencil (URP RenderGraph requirement),
            // BGRA8 to match the overlay swapchain picker default on Windows.
            var rtDesc = new RenderTextureDescriptor(resolution.x, resolution.y,
                GraphicsFormat.B8G8R8A8_UNorm, GraphicsFormat.D24_UNorm_S8_UInt)
            {
                msaaSamples = 1,
                useMipMap = false,
                autoGenerateMips = false,
            };
            OverlayTexture = new RenderTexture(rtDesc) { name = "DisplayXR_Local2D" };
            OverlayTexture.Create();

            // WorldSpace canvas parked far away on a private layer.
            m_Canvas.renderMode = RenderMode.WorldSpace;
            m_CanvasRect.position = kCanvasWorldPos;
            m_CanvasRect.rotation = Quaternion.identity;
            m_CanvasRect.localScale = new Vector3(0.01f, 0.01f, 0.01f);
            m_CanvasRect.sizeDelta = resolution;
            SetLayerRecursive(gameObject, kPrivateLayer);

            // Dedicated ortho camera. Flipped up-vector Y-flips the RT to match
            // the swapchain image's top-left origin (same trick as wsui).
            var camGO = new GameObject("DisplayXR_Local2DCam");
            camGO.transform.SetParent(transform, false);
            camGO.hideFlags = HideFlags.HideAndDontSave;
            camGO.transform.position = kCanvasWorldPos + new Vector3(0, 0, 1);
            camGO.transform.rotation = Quaternion.LookRotation(Vector3.back, Vector3.down);

            m_OverlayCamera = camGO.AddComponent<Camera>();
            m_OverlayCamera.clearFlags = CameraClearFlags.SolidColor;
            m_OverlayCamera.backgroundColor = Color.clear;
            m_OverlayCamera.orthographic = true;
            m_OverlayCamera.orthographicSize = resolution.y * 0.005f;
            m_OverlayCamera.aspect = (float)resolution.x / resolution.y;
            m_OverlayCamera.nearClipPlane = 0.01f;
            m_OverlayCamera.farClipPlane = 10f;
            m_OverlayCamera.targetTexture = OverlayTexture;
            m_OverlayCamera.cullingMask = 1 << kPrivateLayer;
            m_OverlayCamera.depth = -1000;
            // Render in Unity's normal camera loop (enabled), NOT via a manual
            // Camera.Render() in LateUpdate. The manual path raced the canvas
            // rebuild: Unity rebuilds the UI batch in willRenderCanvases, which
            // runs AFTER LateUpdate, so a LateUpdate Render() intermittently
            // captured an empty canvas batch (camera clear landed, UI didn't —
            // the "bubble shows up intermittently" bug; confirmed by an opaque-
            // clear bisect: the clear was stable, the UI content was not). An
            // enabled offscreen camera with a targetTexture renders every frame
            // after the engine's canvas rebuild, so the UI is always present.
            m_OverlayCamera.enabled = true;

            m_Canvas.worldCamera = m_OverlayCamera;

            if (m_ProviderMode)
                TryAcquireBridge(); // provider owns its own cross-device Local2D bridge
            else
                DisplayXRNative.displayxr_local2d_set_texture(
                    OverlayTexture.GetNativeTexturePtr(), resolution.x, resolution.y);

            // Scope the URP foreground clip out of our overlay camera (see field doc).
            RenderPipelineManager.beginCameraRendering += OnBeginOverlayCamera;
            RenderPipelineManager.endCameraRendering += OnEndOverlayCamera;
            m_CamRenderHooked = true;

            Debug.Log($"[DisplayXR] Local2D enabled: {resolution.x}x{resolution.y}");
        }

        // Disable the foreground clip for the overlay camera's render, restore after,
        // so the clip never reaches the bubble (it must only clip the projection/XR
        // camera). cam-identity gated so other cameras are untouched.
        void OnBeginOverlayCamera(ScriptableRenderContext ctx, Camera cam)
        {
            if (cam != m_OverlayCamera) return;
            m_SavedForegroundFar = Shader.GetGlobalVector(s_ForegroundFarId);
            if (m_SavedForegroundFar.z > 0.5f)
                Shader.SetGlobalVector(s_ForegroundFarId, Vector4.zero);
        }

        void OnEndOverlayCamera(ScriptableRenderContext ctx, Camera cam)
        {
            if (cam != m_OverlayCamera) return;
            if (m_SavedForegroundFar.z > 0.5f)
                Shader.SetGlobalVector(s_ForegroundFarId, m_SavedForegroundFar);
        }

        // Provider mode: acquire the provider's cross-device Local2D bridge and wrap it
        // as an external Texture2D we Graphics.CopyTexture the overlay RT into. Lazy —
        // the session may come up after OnEnable. Mirrors DisplayXRWindowSpaceUI.
        private void TryAcquireBridge()
        {
            if (m_BridgeTex != null) return;
            try
            {
                DisplayXRProviderNative.dxr_prov_get_local2d_bridge(
                    (uint)resolution.x, (uint)resolution.y,
                    out System.IntPtr bridgePtr, out uint bw, out uint bh);
                if (bridgePtr != System.IntPtr.Zero && bw > 0 && bh > 0)
                {
                    m_BridgeTex = Texture2D.CreateExternalTexture(
                        (int)bw, (int)bh, TextureFormat.BGRA32, false, true, bridgePtr);
                    m_BridgeTex.name = "DisplayXR_Local2DBridge";
                    Debug.Log($"[DisplayXR] Local2D: provider bridge acquired {bw}x{bh}");
                }
            }
            catch (System.EntryPointNotFoundException) { }
        }

        void OnDisable()
        {
            if (m_BridgeTex != null)
            {
                if (Application.isPlaying) Destroy(m_BridgeTex); else DestroyImmediate(m_BridgeTex);
                m_BridgeTex = null;
            }

            if (m_CamRenderHooked)
            {
                RenderPipelineManager.beginCameraRendering -= OnBeginOverlayCamera;
                RenderPipelineManager.endCameraRendering -= OnEndOverlayCamera;
                m_CamRenderHooked = false;
            }

            if (m_ProviderMode)
                DisplayXRProviderNative.dxr_prov_set_local2d_rect(0, 0, 0, 0);
            else
                DisplayXRNative.displayxr_local2d_clear();

            if (m_StateSaved && m_Canvas != null)
            {
                m_Canvas.renderMode = m_OrigRenderMode;
                m_Canvas.worldCamera = m_OrigCanvasWorldCamera;
                if (m_CanvasRect != null)
                {
                    m_CanvasRect.position = m_OrigCanvasPos;
                    m_CanvasRect.rotation = m_OrigCanvasRot;
                    m_CanvasRect.localScale = m_OrigCanvasScale;
                }
                SetLayerRecursive(gameObject, m_OrigCanvasLayer);
                m_StateSaved = false;
            }

            if (m_OverlayCamera != null)
            {
                if (Application.isPlaying) Destroy(m_OverlayCamera.gameObject);
                else DestroyImmediate(m_OverlayCamera.gameObject);
            }

            if (OverlayTexture != null)
            {
                OverlayTexture.Release();
                if (Application.isPlaying) Destroy(OverlayTexture);
                else DestroyImmediate(OverlayTexture);
                OverlayTexture = null;
            }
        }

        void LateUpdate()
        {
            if (m_OverlayCamera == null || OverlayTexture == null) return;

            // Keep the camera aspect at the live pixel-rect aspect so the bubble
            // isn't stretched when the panel resizes.
            if (TryGetPanelPixelSize(out int px, out int py, out int pw, out int ph) &&
                pw > 0 && ph > 0)
            {
                float aspect = (float)pw / ph;
                m_OverlayCamera.orthographicSize = resolution.y * 0.005f;
                m_OverlayCamera.aspect = aspect;
                if (m_CanvasRect != null)
                    m_CanvasRect.sizeDelta = new Vector2(resolution.y * aspect, resolution.y);

                if (px != m_LastRectX || py != m_LastRectY ||
                    pw != m_LastRectW || ph != m_LastRectH)
                {
                    if (m_ProviderMode)
                        DisplayXRProviderNative.dxr_prov_set_local2d_rect(px, py, pw, ph);
                    else
                        DisplayXRNative.displayxr_local2d_set_rect(px, py, pw, ph);
                    m_LastRectX = px; m_LastRectY = py;
                    m_LastRectW = pw; m_LastRectH = ph;
                }
            }

            // Provider mode: mirror the overlay RT into the provider's cross-device
            // bridge each frame (the enabled camera rendered it in Unity's loop). The
            // provider copies the bridge into its Local2D swapchain at submit. 1-frame
            // latency is invisible for a HUD/bubble.
            if (m_ProviderMode)
            {
                if (m_BridgeTex == null) TryAcquireBridge();
                if (m_BridgeTex != null && OverlayTexture != null)
                    Graphics.CopyTexture(OverlayTexture, m_BridgeTex);
            }
            // No manual Render() here — the camera is enabled and renders in
            // Unity's loop after the canvas rebuild (see OnEnable). This is the
            // fix for the intermittent-blank UI content.
        }

        // Convert the fractional rect to client-window pixels using the live
        // panel size (built-app: the runtime composites into Unity's window, so
        // Screen.* is the panel).
        private bool TryGetPanelPixelSize(out int x, out int y, out int w, out int h)
        {
            // Explicit-rect mode: the caller supplies an exact client-window pixel
            // rect (e.g. a region editor that already knows the panel size). This
            // is authoritative — no Screen.* scaling, which can disagree with the
            // runtime's actual panel size for a full-screen overlay.
            if (useExplicitRect)
            {
                x = ExplicitRect.x; y = ExplicitRect.y;
                w = ExplicitRect.width; h = ExplicitRect.height;
                return w > 0 && h > 0;
            }

            x = y = w = h = 0;
            if (Screen.width <= 0 || Screen.height <= 0) return false;
            float sw = Screen.width, sh = Screen.height;
            x = Mathf.RoundToInt(Mathf.Clamp01(positionX) * sw);
            y = Mathf.RoundToInt(Mathf.Clamp01(positionY) * sh);
            w = Mathf.RoundToInt(Mathf.Clamp01(width) * sw);
            h = Mathf.RoundToInt(Mathf.Clamp01(height) * sh);
            return w > 0 && h > 0;
        }

        private static void SetLayerRecursive(GameObject go, int layer)
        {
            go.layer = layer;
            foreach (Transform child in go.transform)
                SetLayerRecursive(child.gameObject, layer);
        }
    }
}
