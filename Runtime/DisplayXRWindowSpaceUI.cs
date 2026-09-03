// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;
using UnityEngine.Experimental.Rendering;

namespace DisplayXR
{
    /// <summary>
    /// Submits a Unity UI Canvas as an OpenXR XrCompositionLayerWindowSpaceDXR
    /// composition layer.
    ///
    /// IMPORTANT API: this component takes over the Canvas it's attached to and
    /// drives it as a private WorldSpace canvas. Don't share the Canvas with
    /// other UI logic that expects ScreenSpaceOverlay/Camera modes — give wsui
    /// its own Canvas GameObject. The canvas's child UI elements ARE rendered
    /// into the layer; everything else is preserved.
    ///
    /// The overlay is composited per-eye with horizontal disparity, rendered
    /// pre-interlace by the runtime's display processor.
    ///
    /// Why WorldSpace and not ScreenSpaceCamera? URP's RenderGraph has multiple
    /// failure modes when a Canvas in ScreenSpaceCamera mode targets a camera
    /// whose targetTexture is an offscreen RT (canvas-camera coupling races,
    /// Canvas-position-depends-on-camera-position cycles when the camera is
    /// parented under the canvas, RenderGraph dropping offscreen-only cameras
    /// from its loop). WorldSpace + dedicated camera with targetTexture is the
    /// canonical Unity recipe for "render UI to a texture" and works
    /// identically across BiRP / URP / HDRP.
    /// </summary>
    [AddComponentMenu("DisplayXR/Window Space UI")]
    [RequireComponent(typeof(Canvas))]
    [ExecuteAlways]
    public class DisplayXRWindowSpaceUI : MonoBehaviour
    {
        [Header("Window Position (fractional 0..1)")]

        [Tooltip("Left edge position as fraction of window width.")]
        [Range(0f, 1f)]
        public float positionX = 0.02f;

        [Tooltip("Top edge position as fraction of window height.")]
        [Range(0f, 1f)]
        public float positionY = 0.02f;

        [Tooltip("Width as fraction of window width.")]
        [Range(0f, 1f)]
        public float width = 0.3f;

        [Tooltip("Height as fraction of window height.")]
        [Range(0f, 1f)]
        public float height = 0.15f;

        [Header("Depth")]

        [Tooltip("Horizontal shift for stereo depth. 0 = at screen plane, " +
                 "positive = in front, negative = behind.")]
        [Range(-0.05f, 0.05f)]
        public float disparity;

        [Header("Render Settings")]

        [Tooltip("Resolution of the overlay RenderTexture.")]
        public Vector2Int resolution = new Vector2Int(1024, 1024);

        /// <summary>The RenderTexture used to capture the Canvas content.</summary>
        public RenderTexture OverlayTexture { get; private set; }

        // We park the WorldSpace canvas at this fixed position, far from any
        // scene content, so the dedicated camera looking at it sees nothing
        // else that might bleed into our RT.
        private static readonly Vector3 kCanvasWorldPos = new Vector3(0, 100000f, 0);
        // Dedicated layer: we put the canvas + children on this layer and give
        // ONLY our overlay camera that layer in its cullingMask. We pick a
        // mid-range layer that's typically unused (Unity reserves 0-7 for
        // built-ins; 8+ are user layers; 31 is sometimes used for ignore-raycast).
        // 30 has the lowest collision risk in practice — rarely used by host apps.
        private const int kPrivateLayer = 30;

        private Canvas m_Canvas;
        private RectTransform m_CanvasRect;
        private Camera m_OverlayCamera;
        // Windows D3D11/D3D12 provider path: a shared bridge texture (NT-handle)
        // opened on Unity's device. We Graphics.CopyTexture our OverlayTexture
        // into it each frame, then the plugin's native layer copies the
        // provider-side bridge to the composition swapchain image. Null on Mac
        // (Metal: unified device, direct path works) and when the provider isn't
        // active (native returns null).
        private Texture2D m_BridgeTex;
        // Native pointer m_BridgeTex wraps — a change means a new session. See TryAcquireBridge.
        private System.IntPtr m_BridgePtr = System.IntPtr.Zero;

        // Saved state, restored in OnDisable.
        private RenderMode m_OrigRenderMode;
        private Vector3 m_OrigCanvasPos;
        private Quaternion m_OrigCanvasRot;
        private Vector3 m_OrigCanvasScale;
        private int m_OrigCanvasLayer;
        private Camera m_OrigCanvasWorldCamera;
        private bool m_StateSaved;

        private float m_LastX, m_LastY, m_LastW, m_LastH, m_LastDisparity;

        /// <summary>
        /// Set true by app-side input routers while the cursor is hovering or
        /// a press is held over a wsui-rendered UI element. Scene input
        /// controllers (DisplayXRInputController, custom drag handlers, etc.)
        /// should consult this and skip mouse handling so a slider drag
        /// doesn't bleed into cube rotation. The plugin owns the flag so all
        /// routers and controllers can coordinate without referencing each
        /// other directly.
        /// </summary>
        public static bool IsCursorOverInteractive { get; set; }

        void OnEnable()
        {
            m_Canvas = GetComponent<Canvas>();
            m_CanvasRect = m_Canvas.GetComponent<RectTransform>();

            // Save state for restoration in OnDisable.
            m_OrigRenderMode = m_Canvas.renderMode;
            m_OrigCanvasPos = m_CanvasRect.position;
            m_OrigCanvasRot = m_CanvasRect.rotation;
            m_OrigCanvasScale = m_CanvasRect.localScale;
            m_OrigCanvasLayer = gameObject.layer;
            m_OrigCanvasWorldCamera = m_Canvas.worldCamera;
            m_StateSaved = true;

            // ---- RT with explicit depth-stencil format (URP RenderGraph requirement) ----
            var rtDesc = new RenderTextureDescriptor(resolution.x, resolution.y,
                GraphicsFormat.B8G8R8A8_UNorm, GraphicsFormat.D24_UNorm_S8_UInt)
            {
                msaaSamples = 1,
                useMipMap = false,
                autoGenerateMips = false,
            };
            OverlayTexture = new RenderTexture(rtDesc) { name = "DisplayXR_Overlay" };
            OverlayTexture.Create();

            // ---- Switch the canvas to WorldSpace + park it on the private layer ----
            m_Canvas.renderMode = RenderMode.WorldSpace;
            // worldCamera is assigned to the OverlayCamera below (after creation)
            // so GraphicRaycaster can project screen-cursor input onto the canvas.
            m_CanvasRect.position = kCanvasWorldPos;
            m_CanvasRect.rotation = Quaternion.identity;
            // Use the canvas's existing reference width as the scale baseline.
            // 1 world unit per UI unit at scale 1 → set scale so the RT
            // resolution maps cleanly. Use 1/100 like a typical WorldSpace UI
            // setup (1 cm per UI unit).
            m_CanvasRect.localScale = new Vector3(0.01f, 0.01f, 0.01f);
            // Canvas size in UI units = resolution (so 1 RT pixel = 1 UI unit).
            m_CanvasRect.sizeDelta = resolution;
            SetLayerRecursive(gameObject, kPrivateLayer);

            // ---- Dedicated camera: orthographic, points at canvas, renders ONLY the private layer ----
            var camGO = new GameObject("DisplayXR_OverlayCam");
            camGO.transform.SetParent(transform, false);
            camGO.hideFlags = HideFlags.HideAndDontSave;
            // Canvas plane normal is +Z; camera sits at +Z, looks toward -Z
            // to see the front face of the canvas. We deliberately invert
            // the camera's up vector so the rendered RT is Y-flipped at
            // capture time — this compensates for the bottom-left ↔ top-left
            // origin convention mismatch between Unity's render target
            // pipeline and the swapchain image our native blit feeds the
            // runtime compositor with. Without this the panel reads
            // upside-down in the runtime preview window.
            camGO.transform.position = kCanvasWorldPos + new Vector3(0, 0, 1);
            camGO.transform.rotation = Quaternion.LookRotation(Vector3.back, Vector3.down);

            m_OverlayCamera = camGO.AddComponent<Camera>();
            m_OverlayCamera.clearFlags = CameraClearFlags.SolidColor;
            m_OverlayCamera.backgroundColor = Color.clear;
            m_OverlayCamera.orthographic = true;
            // Ortho size = half-height in world units. Canvas height = resolution.y * 0.01
            // (because of localScale=0.01). So ortho size = resolution.y * 0.005 covers the canvas exactly.
            m_OverlayCamera.orthographicSize = resolution.y * 0.005f;
            m_OverlayCamera.aspect = (float)resolution.x / resolution.y;
            m_OverlayCamera.nearClipPlane = 0.01f;
            m_OverlayCamera.farClipPlane = 10f;
            m_OverlayCamera.targetTexture = OverlayTexture;
            m_OverlayCamera.cullingMask = 1 << kPrivateLayer;
            m_OverlayCamera.depth = -1000;
            // Render in Unity's normal camera loop (enabled), NOT via a manual
            // Camera.Render() in LateUpdate. Immediate-mode Camera.Render() is
            // incompatible with URP's RenderGraph (Unity 6): even for a camera
            // with a targetTexture it retrieves the editor GameView backbuffer off
            // the gfx worker thread ("Back buffer can only be retrieved on the gfx
            // worker thread!"), which blanks the whole XR mirror → pure-black docked
            // Game view (URP only; BiRP/HDRP have no RenderGraph, so the manual path
            // was fine there). An enabled offscreen camera renders every frame via
            // the pipeline loop and RenderGraph imports the RT, never the backbuffer.
            // Mirrors DisplayXRLocal2D (which was migrated off manual Render for the
            // canvas-rebuild race — same enabled=true resolution).
            m_OverlayCamera.enabled = true;

            // Wire OverlayCamera as the canvas's event camera. GraphicRaycaster
            // needs a camera reference to project screen-cursor input onto a
            // WorldSpace canvas — without it, raycasts return empty hits and
            // app-side input routers (e.g. DisplayXRWsuiMouseRouter) can't drive
            // sliders/buttons. Using OverlayCamera (which already views the
            // canvas full-frame) means cursor coords passed in RT-pixel space
            // map 1:1 to the rendered layout. The camera's flipped up-vector
            // (down) flips the projection's Y to match the runtime's top-left
            // texture origin, so callers should pass a Y-flipped cursor coord
            // to PointerEventData.position to compensate.
            m_Canvas.worldCamera = m_OverlayCamera;

            // ---- Tell native about our texture + initial layer descriptor ----
            DisplayXRNative.displayxr_window_space_ui_set_layer(
                positionX, positionY, width, height, disparity);
            DisplayXRNative.displayxr_window_space_ui_set_texture(
                OverlayTexture.GetNativeTexturePtr(), resolution.x, resolution.y);

            // ---- Windows-only: query the cross-device bridge ----
            // On Windows the provider session runs on its own D3D12 device;
            // a direct CopyResource from our RT to the swapchain image is
            // invalid because the two are on different devices. Native lazily
            // creates a SHARED (NT-handle) texture on the provider device +
            // opens it on Unity's device; we wrap it here as a Texture2D and
            // Graphics.CopyTexture our RT into it each frame in LateUpdate.
            // Returns null on Mac (unified device) and when the provider isn't
            // active — both paths use the direct unity_tex path in native.
            TryAcquireBridge();

            m_LastX = positionX; m_LastY = positionY;
            m_LastW = width;     m_LastH = height;
            m_LastDisparity = disparity;

            Debug.Log($"[DisplayXR] WindowSpaceUI enabled: {resolution.x}x{resolution.y} " +
                      $"(WorldSpace canvas, layer {kPrivateLayer}, dedicated camera)");
        }

        // Re-acquires across session restarts. The provider destroys the wsui bridge with
        // the session, and the editor's docked<->undocked switch restarts it mid-play, so
        // a non-null m_BridgeTex is NOT proof the bridge is live — it may wrap a DESTROYED
        // resource, into which Graphics.CopyTexture succeeds silently. Poll the pointer
        // (the getter is cheap: it early-returns while the session is down and its create
        // is idempotent) and re-wrap when it changes. Same fix as DisplayXRLocal2D.
        private void TryAcquireBridge()
        {
            try
            {
                System.IntPtr bridgePtr = System.IntPtr.Zero;
                uint bw = 0, bh = 0;
                if (DisplayXRProviderDriver.IsActive)
                {
                    // Custom display-provider mode: the provider owns a SEPARATE
                    // D3D12 device, so it exposes its own cross-device wsui bridge. (#166)
                    DisplayXRProviderNative.dxr_prov_get_wsui_bridge(
                        (uint)resolution.x, (uint)resolution.y,
                        out bridgePtr, out bw, out bh);
                }
                if (bridgePtr == System.IntPtr.Zero || bw == 0 || bh == 0)
                {
                    // Session down / not ready — drop a stale wrapper so the next live
                    // bridge is picked up cleanly instead of being copied into a dead one.
                    if (m_BridgeTex != null) ReleaseBridgeTex();
                    return;
                }
                if (m_BridgeTex != null && bridgePtr == m_BridgePtr) return; // unchanged

                if (m_BridgeTex != null) ReleaseBridgeTex();
                m_BridgeTex = Texture2D.CreateExternalTexture(
                    (int)bw, (int)bh, TextureFormat.BGRA32, false, true,
                    bridgePtr);
                m_BridgeTex.name = "DisplayXR_WsuiBridge";
                m_BridgePtr = bridgePtr;
                // A new pointer means a NEW SESSION. The layer descriptor lives in a native
                // file static and survives, but re-push it anyway so a restart can never
                // leave the runtime with a stale/never-seen layer, and invalidate the
                // change-detection cache below so LateUpdate re-pushes too.
                DisplayXRNative.displayxr_window_space_ui_set_texture(
                    OverlayTexture != null ? OverlayTexture.GetNativeTexturePtr() : System.IntPtr.Zero,
                    resolution.x, resolution.y);
                DisplayXRNative.displayxr_window_space_ui_set_layer(
                    positionX, positionY, width, height, disparity);
                m_LastX = positionX; m_LastY = positionY;
                m_LastW = width;     m_LastH = height;
                m_LastDisparity = disparity;
                Debug.Log($"[DisplayXR] wsui: bridge acquired {bw}x{bh} (provider={DisplayXRProviderDriver.IsActive})");
            }
            catch (System.EntryPointNotFoundException)
            {
                // Older plugin without the bridge API — non-Windows path or
                // pre-bridge build. Stay on the direct unity_tex path.
            }
        }

        private void ReleaseBridgeTex()
        {
            if (m_BridgeTex != null)
            {
                if (Application.isPlaying) Destroy(m_BridgeTex); else DestroyImmediate(m_BridgeTex);
                m_BridgeTex = null;
            }
            m_BridgePtr = System.IntPtr.Zero;
        }

        void OnDisable()
        {
            DisplayXRNative.displayxr_window_space_ui_clear();

            // Restore the canvas's original mode + transform + layer.
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
                if (Application.isPlaying)
                    Destroy(m_OverlayCamera.gameObject);
                else
                    DestroyImmediate(m_OverlayCamera.gameObject);
            }

            if (OverlayTexture != null)
            {
                OverlayTexture.Release();
                if (Application.isPlaying)
                    Destroy(OverlayTexture);
                else
                    DestroyImmediate(OverlayTexture);
                OverlayTexture = null;
            }

            // Texture2D.CreateExternalTexture'd handles don't own the underlying native
            // resource — native keeps the bridge alive across enable/disable cycles. Just
            // drop our Unity-side wrapper (and the cached pointer, so a later enable
            // re-wraps rather than trusting a stale match).
            ReleaseBridgeTex();
        }

        void LateUpdate()
        {
            // Keep EVERYTHING under the canvas on the private layer, every frame.
            // OnEnable parks the hierarchy on kPrivateLayer once, but the overlay
            // camera culls to that layer alone — so anything Instantiated under the
            // canvas afterwards (list items, model tiles, a file dialog, a dropdown's
            // blocker) is born on its prefab's layer and silently culled: no error,
            // no warning, just absent. Any app with dynamic UI hits this. Writes only
            // where the layer differs, so the steady-state cost is a read-only walk.
            EnforcePrivateLayer();

            // Match the camera aspect to the live panel pixel aspect so UI
            // content stays at correct aspect when the host window resizes
            // OR when the wsui rect (positionX/Y/width/height) changes. The
            // camera renders into a fixed-size RT, but the runtime stretches
            // that RT into the panel rect — by setting camera.aspect to the
            // panel's pixel aspect, the camera "pre-distorts" its rendering
            // so the post-stretch result is aspect-correct in the panel.
            if (m_OverlayCamera != null && TryGetPanelPixelSize(out float pw, out float ph) &&
                pw > 0f && ph > 0f)
            {
                float panelAspect = pw / ph;
                // ortho size is half the canvas height in world units (canvas
                // sizeDelta.y * canvas.localScale.y / 2). Recompute each frame
                // — cheap, and supports inspector edits to resolution.y too.
                m_OverlayCamera.orthographicSize = resolution.y * 0.005f;
                m_OverlayCamera.aspect = panelAspect;

                // Dynamically resize the canvas's RectTransform to MATCH the
                // camera view. Without this the canvas stays its initial
                // (square) size and there's pillarboxing/letterboxing inside
                // the panel rect — the panel image only fills the canvas, not
                // the camera's full view. Since the runtime stretches the RT
                // into the panel rect, we want the canvas (and its panel
                // image) to fill the camera's view exactly so the resulting
                // panel content has no internal margins.
                if (m_CanvasRect != null)
                {
                    m_CanvasRect.sizeDelta = new Vector2(resolution.y * panelAspect, resolution.y);
                }
            }

            // Copy our RT into the shared cross-device bridge so the provider's
            // separate device can read it via the shared NT handle. The overlay
            // camera now renders every frame via the pipeline loop (enabled=true, see
            // OnEnable) — so in LateUpdate the RT holds the PREVIOUS frame's content:
            // a 1-frame latency that's imperceptible for the slow-changing panel UI,
            // and the price of dropping the RenderGraph-incompatible manual Render.
            // (Mirrors DisplayXRLocal2D, which copies its bridge the same way.)
            // No-op off Windows/provider mode (m_BridgeTex stays null).
            if (m_OverlayCamera != null && OverlayTexture != null)
            {
                // Unconditional — the provider session may come up after our OnEnable, and
                // TryAcquireBridge also detects a RESTART via the native pointer changing,
                // which a "== null" guard would hide.
                TryAcquireBridge();
                if (m_BridgeTex != null)
                {
                    Graphics.CopyTexture(OverlayTexture, m_BridgeTex);
                }
            }

            if (positionX != m_LastX || positionY != m_LastY ||
                width != m_LastW || height != m_LastH ||
                disparity != m_LastDisparity)
            {
                DisplayXRNative.displayxr_window_space_ui_set_layer(
                    positionX, positionY, width, height, disparity);
                m_LastX = positionX; m_LastY = positionY;
                m_LastW = width;     m_LastH = height;
                m_LastDisparity = disparity;
            }
        }

        private bool TryGetPanelPixelSize(out float pw, out float ph)
        {
            // Built-app / Play Mode: the runtime composites into Unity's main
            // window, so Screen.* is meaningful.
            if (Screen.width > 0 && Screen.height > 0)
            {
                pw = Screen.width * Mathf.Clamp01(width);
                ph = Screen.height * Mathf.Clamp01(height);
                return true;
            }
            pw = ph = 0f;
            return false;
        }

        private static void SetLayerRecursive(GameObject go, int layer)
        {
            go.layer = layer;
            foreach (Transform child in go.transform)
                SetLayerRecursive(child.gameObject, layer);
        }

        private bool m_LoggedStrayLayer;

        // Re-park runtime-spawned descendants on the private layer. Only writes where
        // the layer actually differs (a pure read otherwise), and logs the first time it
        // has to fix anything so the culling trap is visible in the console instead of
        // presenting as "my UI didn't appear".
        private void EnforcePrivateLayer()
        {
            if (!m_StateSaved) return; // not taken over (OnEnable hasn't run / OnDisable restored)
            int fixedCount = EnforceLayerRecursive(transform, kPrivateLayer);
            if (fixedCount > 0 && !m_LoggedStrayLayer)
            {
                m_LoggedStrayLayer = true;
                Debug.Log($"[DisplayXR] wsui: moved {fixedCount} runtime-spawned object(s) under " +
                          $"'{name}' to the private layer {kPrivateLayer} (they would otherwise be " +
                          "culled by the overlay camera).");
            }
        }

        private static int EnforceLayerRecursive(Transform t, int layer)
        {
            int fixedCount = 0;
            var go = t.gameObject;
            if (go.layer != layer)
            {
                go.layer = layer;
                fixedCount++;
            }
            for (int i = 0; i < t.childCount; i++)
                fixedCount += EnforceLayerRecursive(t.GetChild(i), layer);
            return fixedCount;
        }
    }
}
