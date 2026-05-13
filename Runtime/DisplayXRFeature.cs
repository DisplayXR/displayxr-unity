// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.XR.Management;
using UnityEngine.XR.OpenXR;
using UnityEngine.XR.OpenXR.Features;
using UnityEngine.XR.OpenXR.NativeTypes;

#if UNITY_EDITOR
using UnityEditor;
using UnityEditor.XR.OpenXR.Features;
#endif

namespace DisplayXR
{
#if UNITY_EDITOR
    [OpenXRFeature(
        UiName = "DisplayXR",
        BuildTargetGroups = new[] {
            UnityEditor.BuildTargetGroup.Standalone
        },
        Company = "DisplayXR",
        Desc = "Enables stereo rendering on 3D light field displays via DisplayXR OpenXR runtime. " +
               "Provides Kooima asymmetric frustum projection, display-centric and camera-centric " +
               "stereo rig modes, and 2D UI overlay support.",
        DocumentationLink = "https://github.com/DisplayXR/displayxr-unity",
        OpenxrExtensionStrings = ExtensionStrings,
        Version = "0.1.0",
        FeatureId = FeatureId
    )]
#endif
    public class DisplayXRFeature : OpenXRFeature
    {
        public const string FeatureId = "com.displayxr.unity.feature";

        // Extensions requested from the runtime
        private const string ExtensionStrings =
            "XR_EXT_display_info " +
            "XR_EXT_win32_window_binding " +
            "XR_EXT_cocoa_window_binding";

        /// <summary>Singleton instance, set during OnInstanceCreate.</summary>
        public static DisplayXRFeature Instance { get; private set; }

        /// <summary>
        /// Force XR tracking origin to Device (head-relative, Y=0 at session
        /// start) rather than Floor. Unity 6 OpenXR defaults to Floor which is
        /// correct for VR (eye poses reported relative to floor ≈ user height).
        /// DisplayXR's native xrLocateViews hook returns LOCAL-space (head-
        /// relative) eye coords for its Kooima off-axis projection — under
        /// BiRP, Unity's stereo pipeline reads those directly and everything
        /// lines up. Under URP, the RenderGraph path picks up
        /// XRInputSubsystem's Floor-relative pose offset and adds it on top of
        /// the plugin's eye coords, shifting the rendered image by user height
        /// (~1.5–1.7m). Forcing Device mode prevents the mismatch.
        ///
        /// AfterSceneLoad: subsystems must already be running for
        /// SubsystemManager.GetSubsystems(XRInputSubsystem) to find them;
        /// OnSessionCreate is too early.
        ///
        /// Verified on Unity 6 + URP 17.0.4 + Windows D3D12 against
        /// displayxr-unity-test-2d-ui (the URP variant test repo).
        /// </summary>
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void ForceDeviceTrackingOrigin()
        {
            try
            {
                var subs = new System.Collections.Generic.List<UnityEngine.XR.XRInputSubsystem>();
                UnityEngine.SubsystemManager.GetSubsystems(subs);
                if (subs.Count == 0)
                {
                    // XR not active in this Player (e.g. running without
                    // OpenXR loader). Nothing to do.
                    return;
                }
                foreach (var sub in subs)
                {
                    var supported = sub.GetSupportedTrackingOriginModes();
                    var current = sub.GetTrackingOriginMode();
                    if (current == UnityEngine.XR.TrackingOriginModeFlags.Device) continue;
                    if ((supported & UnityEngine.XR.TrackingOriginModeFlags.Device) == 0)
                    {
                        Debug.LogWarning("[DisplayXR] XRInputSubsystem doesn't support Device " +
                                         $"tracking origin (supported={supported}); leaving as " + current);
                        continue;
                    }
                    bool ok = sub.TrySetTrackingOriginMode(UnityEngine.XR.TrackingOriginModeFlags.Device);
                    Debug.Log($"[DisplayXR] Tracking origin: {current} -> Device (ok={ok})");
                }
            }
            catch (System.Exception e)
            {
                Debug.LogWarning("[DisplayXR] ForceDeviceTrackingOrigin: " + e.Message);
            }
        }

        /// <summary>
        /// Set by <see cref="DisplayXRTransparentOverlay.RequestTransparentSession"/>
        /// before the OpenXR session is created. Drives the
        /// XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND opt-in inside OnInstanceCreate
        /// so Unity's render path preserves alpha=0 in the swapchain image
        /// — required for the macOS Cocoa per-pixel transparency path (#85).
        /// </summary>
        internal static bool s_TransparentBackgroundRequested;

        /// <summary>Cached display info from the runtime.</summary>
        public DisplayXRDisplayInfo DisplayInfo { get; private set; }

        /// <summary>Whether eye tracking data is being received.</summary>
        public bool IsEyeTracked { get; private set; }

        /// <summary>Raw left eye position in display space (meters).</summary>
        public Vector3 LeftEyePosition { get; private set; }

        /// <summary>Raw right eye position in display space (meters).</summary>
        public Vector3 RightEyePosition { get; private set; }

        private bool m_HooksInstalled;

        // ====================================================================
        // OpenXR Feature lifecycle
        // ====================================================================

        /// <inheritdoc />
        protected override IntPtr HookGetInstanceProcAddr(IntPtr func)
        {
            // Install our native hook chain. The native plugin stores the next-in-chain
            // function pointer and returns our interceptor.
            IntPtr hooked = DisplayXRNative.displayxr_install_hooks(func);
            m_HooksInstalled = (hooked != IntPtr.Zero);

            if (!m_HooksInstalled)
            {
                Debug.LogError("[DisplayXR] Failed to install native OpenXR hooks");
                return func; // Pass through unmodified
            }

            Debug.Log("[DisplayXR] Native OpenXR hooks installed");
            return hooked;
        }

        /// <inheritdoc />
        protected override bool OnInstanceCreate(ulong xrInstance)
        {
            Instance = this;

            // Tell native code which typed swapchain format to substitute for
            // Unity's TYPELESS color swapchain (D3D11). Must happen before any
            // swapchain is created. Mismatch causes double-gamma in built apps
            // (Gamma project + sRGB swapchain) or wrong tint in Linear projects.
            var cs = QualitySettings.activeColorSpace;
            int useSRGB = cs == ColorSpace.Linear ? 1 : 0;
            DisplayXRNative.displayxr_set_use_srgb_swapchain(useSRGB);
            string fmtName = useSRGB == 1 ? "29 (UNORM_SRGB)" : "28 (UNORM)";
            Debug.Log($"[DisplayXR] Color space: {cs} → typed-sibling format = {fmtName}");

#if UNITY_EDITOR
            // Tell native code we're in editor mode — use IOSurface/shared texture
            // instead of auto-detecting the window and creating an overlay.
            DisplayXRNative.displayxr_set_editor_mode(1);

            // Register to switch focus away from Game View before teardown.
            // Game View's RenderToHMDOnly repaint cycle calls xrPollEvent through
            // freed dispatch trampolines during Deinitialize(), causing SIGSEGV.
            // Switching to Scene View stops the repaint cycle before it starts.
            EditorApplication.playModeStateChanged -= OnPlayModeStateChanged;
            EditorApplication.playModeStateChanged += OnPlayModeStateChanged;
#endif

            // Force multi-pass rendering. DisplayXRDisplay/DisplayXRCamera push
            // the Kooima asymmetric projection via Camera.SetStereoProjectionMatrix,
            // which Unity gates to MultiPass on every platform — in SPI it is
            // silently rejected ("Can't set custom eye projection matrix when
            // not in multipass mode") and the scene renders with a degenerate
            // frustum. Verified by experiment on Windows D3D12, issue #69.
            var settings = OpenXRSettings.Instance;
            if (settings != null && settings.renderMode != OpenXRSettings.RenderMode.MultiPass)
            {
                Debug.Log("[DisplayXR] Forcing MultiPass render mode (required for asymmetric frustum projection)");
                settings.renderMode = OpenXRSettings.RenderMode.MultiPass;

                // Also update the serialized backing field so ApplySettings()
                // (which runs after OnInstanceCreate) doesn't overwrite us.
                var field = typeof(OpenXRSettings).GetField("m_renderMode",
                    System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
                if (field != null)
                    field.SetValue(settings, OpenXRSettings.RenderMode.MultiPass);
            }

            // (#85) When the app requested transparent background, opt the
            // session into AlphaBlend so Unity's render path preserves alpha=0
            // in the swapchain image. Without this Unity writes alpha=1 in
            // transparent regions and the runtime's alpha-native sim_display
            // path sees an opaque atlas — visual transparency never reaches
            // the desktop. Mirror of the cocoa_window_binding bit on
            // xrCreateSession (native side).
            if (s_TransparentBackgroundRequested)
            {
                SetEnvironmentBlendMode(XrEnvironmentBlendMode.AlphaBlend);
                Debug.Log("[DisplayXR] EnvironmentBlendMode = AlphaBlend (transparent session)");
            }

            Debug.Log("[DisplayXR] OpenXR instance created");
            return true;
        }

        /// <inheritdoc />
        protected override void OnSystemChange(ulong xrSystem)
        {
            // Display info is extracted by our hooked xrGetSystemProperties.
            // Query it from the native plugin.
            RefreshDisplayInfo();

            // In editor Play Mode, the native hook chain creates its own window
            // and passes it to the runtime — no shared texture needed.

        }

        /// <inheritdoc />
        protected override void OnSessionCreate(ulong xrSession)
        {
            Debug.Log("[DisplayXR] OpenXR session created");

            // Suppress Unity's per-frame mirror render to the Game View / main
            // display. DisplayXR builds present through the runtime's overlay
            // HWND (opaque or transparent), so the mirror is wasted work that
            // re-renders Camera.main every frame for nothing visible. Standalone
            // editor preview sessions don't go through this code path, so the
            // separate DisplayXRGameViewOverlay path is unaffected.
            if (UnityEngine.XR.XRSettings.gameViewRenderMode != UnityEngine.XR.GameViewRenderMode.None)
            {
                Debug.Log("[DisplayXR] Disabling Game View mirror render (gameViewRenderMode = None)");
                UnityEngine.XR.XRSettings.gameViewRenderMode = UnityEngine.XR.GameViewRenderMode.None;
            }

            // Tracking origin is forced to Device by ForceDeviceTrackingOrigin()
            // below — that runs at AfterSceneLoad, after XR subsystems have
            // started. Doing it here in OnSessionCreate is too early:
            // SubsystemManager.GetSubsystems(XRInputSubsystem) returns empty
            // before the session is fully begun.

#if UNITY_STANDALONE_WIN
            // Shell mode: ensure Unity continues processing input and rendering
            // even when the shell's compositor window has foreground focus.
            if (DisplayXRNative.displayxr_is_shell_mode() != 0)
            {
                Application.runInBackground = true;
                Debug.Log("[DisplayXR] Shell mode: enabled runInBackground");
            }
#endif

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
            // D3D11 has a known Unity engine bug: TYPELESS swapchain textures (required
            // by OpenXR D3D11 spec) cause geometry corruption. D3D12 works correctly.
            if (UnityEngine.SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.Direct3D11)
            {
                Debug.LogWarning("[DisplayXR] D3D11 detected — known geometry corruption with TYPELESS " +
                    "swapchain textures (Unity engine bug). Switch to D3D12 in Player Settings > " +
                    "Other Settings > Graphics API for correct rendering.");
            }
#endif

            // Fallback: Unity's OnSystemChange is unreliable in some versions.
            // By session creation time, xrGetSystemProperties has definitely run
            // and our native hook has cached the display info + created the IOSurface.
            if (!DisplayInfo.isValid)
            {
                Debug.Log("[DisplayXR] OnSystemChange was not called — refreshing display info now");
                RefreshDisplayInfo();
            }

        }

        /// <inheritdoc />
        protected override void OnSessionDestroy(ulong xrSession)
        {
            Debug.Log("[DisplayXR] OnSessionDestroy BEGIN");

            // FIRST: kill native poll forwarding before anything else.
            // Unity's GameView repaints call ProcessOpenXRMessageLoop → xrPollEvent
            // continuously, even during teardown. This nulls the native function
            // pointer so our hook returns XR_EVENT_UNAVAILABLE immediately.
            try { DisplayXRNative.displayxr_stop_polling(); }
            catch (System.Exception e) { Debug.LogWarning($"[DisplayXR] stop_polling: {e.Message}"); }

            Debug.Log("[DisplayXR] OnSessionDestroy END");
        }

        /// <inheritdoc />
        protected override void OnInstanceDestroy(ulong xrInstance)
        {
            Debug.Log("[DisplayXR] OnInstanceDestroy BEGIN");
            Instance = null;
            m_HooksInstalled = false;
#if UNITY_EDITOR
            EditorApplication.playModeStateChanged -= OnPlayModeStateChanged;
#endif
            // Re-kill in case something reset the flags between OnSessionDestroy and here
            try { DisplayXRNative.displayxr_stop_polling(); }
            catch (System.Exception e) { Debug.LogWarning($"[DisplayXR] stop_polling: {e.Message}"); }
            Debug.Log("[DisplayXR] OnInstanceDestroy END");
        }

#if UNITY_EDITOR
        private static void OnPlayModeStateChanged(PlayModeStateChange state)
        {
            if (state == PlayModeStateChange.ExitingPlayMode)
            {
                // Kill native poll forwarding BEFORE Deinitialize() starts.
                // This is the primary crash prevention — once set, hooked_xrPollEvent
                // returns XR_EVENT_UNAVAILABLE without touching the runtime.
                Debug.Log("[DisplayXR] ExitingPlayMode: killing poll + closing Game Views");
                try { DisplayXRNative.displayxr_stop_polling(); }
                catch (System.Exception) { }

                // Close all Game View windows synchronously. Game View's
                // RenderToHMDOnly repaint cycle calls xrPollEvent through freed
                // dispatch trampolines during Deinitialize() → SIGSEGV.
                // Close() destroys the window and cancels all pending repaints.
                // Unity auto-recreates the Game View on next Play.
                CloseAllGameViews();

                // Fallback: focus Scene View in case CloseAllGameViews() missed any.
                var sceneView = UnityEditor.SceneView.lastActiveSceneView;
                if (sceneView != null)
                    sceneView.Focus();
            }
        }

        private static void CloseAllGameViews()
        {
            var gameViewType = typeof(UnityEditor.Editor).Assembly.GetType("UnityEditor.GameView");
            if (gameViewType == null) return;

            var gameViews = UnityEngine.Resources.FindObjectsOfTypeAll(gameViewType);
            foreach (var gv in gameViews)
            {
                var window = gv as EditorWindow;
                if (window != null)
                {
                    Debug.Log("[DisplayXR] Closing Game View to prevent teardown crash");
                    window.Close();
                }
            }
        }
#endif

        // ====================================================================
        // Validation
        // ====================================================================

#if UNITY_EDITOR
        /// <inheritdoc />
        protected override void GetValidationChecks(List<ValidationRule> rules, BuildTargetGroup targetGroup)
        {
            rules.Add(new ValidationRule(this)
            {
                message = "DisplayXR requires Multi-Pass render mode. " +
                          "Single-pass instanced is incompatible with Camera.SetStereoProjectionMatrix " +
                          "(MultiPass-only on every platform), which DisplayXR uses to push the Kooima " +
                          "asymmetric projection per eye.",
                error = true,
                checkPredicate = () =>
                {
                    var settings = OpenXRSettings.GetSettingsForBuildTargetGroup(targetGroup);
                    return settings != null && settings.renderMode == OpenXRSettings.RenderMode.MultiPass;
                },
                fixIt = () =>
                {
                    var settings = OpenXRSettings.GetSettingsForBuildTargetGroup(targetGroup);
                    if (settings != null)
                        settings.renderMode = OpenXRSettings.RenderMode.MultiPass;
                },
                fixItAutomatic = true,
            });
        }
#endif

        // ====================================================================
        // Public API
        // ====================================================================

        /// <summary>
        /// Set stereo rig tunables. Called each LateUpdate by DisplayXRDisplay or DisplayXRCamera.
        /// Thread-safe: double-buffered in the native plugin.
        /// </summary>
        public void SetTunables(DisplayXRTunables tunables)
        {
            if (!m_HooksInstalled) return;

            // For camera-centric mode, fovOverride is already half_tan_vfov
            // (C# computes Mathf.Tan(fov_rad * 0.5f) before setting it).
            // For display-centric mode, fovOverride is unused (0).
            DisplayXRNative.displayxr_set_tunables(
                tunables.ipdFactor,
                tunables.parallaxFactor,
                tunables.perspectiveFactor,
                tunables.virtualDisplayHeight,
                tunables.invConvergenceDistance,
                tunables.fovOverride,
                tunables.nearZ,
                tunables.farZ,
                tunables.cameraCentricMode ? 1 : 0,
                tunables.clipAtDisplayPlane ? 1 : 0);
        }

        /// <summary>
        /// Refresh display info from the native plugin cache.
        /// </summary>
        public void RefreshDisplayInfo()
        {
            if (!m_HooksInstalled) return;

            DisplayXRNative.displayxr_get_display_info(
                out float wm, out float hm,
                out uint pw, out uint ph,
                out float nx, out float ny, out float nz,
                out float sx, out float sy,
                out int valid);

            DisplayInfo = new DisplayXRDisplayInfo
            {
                displayWidthMeters = wm,
                displayHeightMeters = hm,
                displayPixelWidth = pw,
                displayPixelHeight = ph,
                nominalViewerX = nx,
                nominalViewerY = ny,
                nominalViewerZ = nz,
                recommendedViewScaleX = sx,
                recommendedViewScaleY = sy,
                isValid = valid != 0,
            };

            if (DisplayInfo.isValid)
            {
                Debug.Log($"[DisplayXR] Display: {pw}x{ph}px, " +
                          $"{wm * 100:F1}x{hm * 100:F1}cm, " +
                          $"nominal=({nx * 1000:F0},{ny * 1000:F0},{nz * 1000:F0})mm, " +
                          $"scale={sx:F2}x{sy:F2}");
            }
        }

        /// <summary>
        /// Refresh eye position data from native plugin. Call each frame if needed.
        /// </summary>
        public void RefreshEyePositions()
        {
            if (!m_HooksInstalled) return;

            DisplayXRNative.displayxr_get_eye_positions(
                out float lx, out float ly, out float lz,
                out float rx, out float ry, out float rz,
                out int tracked);

            LeftEyePosition = new Vector3(lx, ly, lz);
            RightEyePosition = new Vector3(rx, ry, rz);
            IsEyeTracked = tracked != 0;
        }

        // Reusable buffers for stereo matrix retrieval
        private float[] m_LeftView = new float[16];
        private float[] m_LeftProj = new float[16];
        private float[] m_RightView = new float[16];
        private float[] m_RightProj = new float[16];

        /// <summary>
        /// Get the Kooima stereo matrices computed by the native library.
        /// Returns true if valid matrices are available.
        /// Matrices are column-major, OpenXR/OpenGL convention (right-hand, -Z forward,
        /// Z clip range [-1,1]). Caller must convert to Unity convention.
        /// </summary>
        public bool GetStereoMatrices(out Matrix4x4 leftView, out Matrix4x4 leftProj,
                                       out Matrix4x4 rightView, out Matrix4x4 rightProj)
        {
            leftView = leftProj = rightView = rightProj = Matrix4x4.identity;
            if (!m_HooksInstalled) return false;

            DisplayXRNative.displayxr_get_stereo_matrices(
                m_LeftView, m_LeftProj, m_RightView, m_RightProj, out int valid);

            if (valid == 0) return false;

            leftView = FloatsToMatrix(m_LeftView);
            leftProj = FloatsToMatrix(m_LeftProj);
            rightView = FloatsToMatrix(m_RightView);
            rightProj = FloatsToMatrix(m_RightProj);
            return true;
        }

        private static Matrix4x4 FloatsToMatrix(float[] m)
        {
            // Column-major float[16] → Unity Matrix4x4 (also column-major)
            var mat = new Matrix4x4();
            mat.m00 = m[0];  mat.m10 = m[1];  mat.m20 = m[2];  mat.m30 = m[3];
            mat.m01 = m[4];  mat.m11 = m[5];  mat.m21 = m[6];  mat.m31 = m[7];
            mat.m02 = m[8];  mat.m12 = m[9];  mat.m22 = m[10]; mat.m32 = m[11];
            mat.m03 = m[12]; mat.m13 = m[13]; mat.m23 = m[14]; mat.m33 = m[15];
            return mat;
        }

        /// <summary>
        /// Set the viewport (window) size and screen position for window-relative
        /// Kooima projection. Screen position enables correct eye offset computation
        /// when the window is not centered on the display.
        /// </summary>
        public void SetViewportSize(int width, int height, int screenX, int screenY)
        {
            if (!m_HooksInstalled) return;
            DisplayXRNative.displayxr_set_viewport_size((uint)width, (uint)height, screenX, screenY);
        }

        /// <summary>
        /// Set the scene transform applied to raw eye positions before Kooima computation.
        /// This is the "parent camera pose" from the Unity scene hierarchy.
        /// Chain: raw eyes -> scene transform -> tunables -> Kooima.
        ///
        /// Use this to map raw LOCAL-space eye positions into your scene's coordinate
        /// frame, mirroring the test app's player transform (rotation, zoom, position offset).
        /// </summary>
        /// <param name="position">Translation offset in display space (meters).</param>
        /// <param name="orientation">Rotation quaternion applied to eye positions.</param>
        /// <param name="scale">Transform scale: spatial coordinates divided by this. (1,1,1) = no scaling.</param>
        /// <param name="enabled">Whether to apply the transform.</param>
        public void SetSceneTransform(Vector3 position, Quaternion orientation, Vector3 scale, bool enabled = true)
        {
            if (!m_HooksInstalled) return;

            DisplayXRNative.displayxr_set_scene_transform(
                position.x, position.y, position.z,
                orientation.x, orientation.y, orientation.z, orientation.w,
                scale.x, scale.y, scale.z,
                enabled ? 1 : 0);
        }

        /// <summary>
        /// Set the window handle for runtime rendering. Must be called before session creation.
        /// On Windows: pass the HWND. On macOS: pass the NSView pointer.
        /// </summary>
        public void SetWindowHandle(IntPtr handle)
        {
            if (!m_HooksInstalled) return;
            DisplayXRNative.displayxr_set_window_handle(handle);
        }

        /// <summary>
        /// Request 2D or 3D display mode via XR_EXT_display_info.
        /// </summary>
        /// <param name="mode3d">true for 3D, false for 2D.</param>
        /// <returns>true if the request succeeded.</returns>
        public bool RequestDisplayMode(bool mode3d)
        {
            if (!m_HooksInstalled) return false;
            return DisplayXRNative.displayxr_request_display_mode(mode3d ? 1 : 0) != 0;
        }

    }
}
