// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;


namespace DisplayXR.Editor
{
    // ================================================================
    // Camera discovery types
    // ================================================================

    public enum CameraCategory { DisplayRig, CameraRig, RegularCamera }

    public struct CameraEntry
    {
        public Camera camera;
        public CameraCategory category;
        public string displayName;
    }

    /// <summary>
    /// Manages a standalone OpenXR session for the editor preview window.
    /// Completely independent of Unity's XR subsystem — no play mode required.
    /// Lifecycle is tied to the preview window, not play/stop transitions.
    ///
    /// Frame loop (each EditorApplication.update tick):
    ///   poll_events → begin_frame → compute_stereo_views →
    ///   [render left/right cameras] → submit_frame
    /// </summary>
    [InitializeOnLoad]
    public static class DisplayXRPreviewSession
    {
        private static bool s_IsRunning;
        public static bool IsRunning => s_IsRunning;

        public static DisplayXRDisplayInfo DisplayInfo { get; private set; }
        public static bool IsEyeTracked { get; private set; }
        public static Vector3 LeftEyePosition { get; private set; }
        public static Vector3 RightEyePosition { get; private set; }
        /// <summary>
        /// The atlas RenderTexture containing the raw eye tile renders.
        /// Available when the SA session is running.
        /// </summary>
        public static RenderTexture AtlasTexture => s_AtlasRT;

        private static bool s_Polling;
        private static bool s_Stopping; // Guard against re-entrant ticks during teardown
        private static bool s_InPollEvents; // True while inside displayxr_standalone_poll_events
        private static bool s_DeferredStop; // Deferred stop when called re-entrantly from poll_events
        private static int s_FrameCount;

        // Rendering rig
        private static RenderTexture s_AtlasRT;
        private static Camera[] s_EyeCams;
        private static GameObject s_RigRoot;
        private static float s_NearZ = 0.3f;
        private static float s_FarZ = 1000f;

        // Current mode tiling info (cached from native)
        private static uint s_ViewCount = 2;
        private static uint s_TileColumns = 2;
        private static uint s_TileRows = 1;
        private static uint s_ViewWidth;
        private static uint s_ViewHeight;
        private static uint s_AtlasWidth;
        private static uint s_AtlasHeight;

        /// <summary>
        /// When true, entering play mode auto-starts the SA preview and disables Unity's XR loader.
        /// This avoids the Game View XR rendering crash during teardown.
        /// </summary>
        public static bool PlayModeIntegration { get; set; } = true;

        // Camera selection
        private const string kSelectedCameraIDKey = "DisplayXR_SA_SelectedCameraID";
        private static Camera s_SelectedSourceCamera;
        public static Camera SelectedCamera => s_SelectedSourceCamera;

        // SessionState survives domain reload (unlike static fields)
        private const string kPlayModeStartedKey = "DisplayXR_SA_StartedByPlayMode";
        private const string kXRWasEnabledKey = "DisplayXR_SA_XRWasEnabled";
        private const string kXRLoaderAssetPathKey = "DisplayXR_SA_XRLoaderAssetPath";

        static DisplayXRPreviewSession()
        {
            // Clean up standalone session on domain reload (script recompilation)
            AssemblyReloadEvents.beforeAssemblyReload += OnBeforeAssemblyReload;
            EditorApplication.quitting += OnEditorQuitting;
            EditorApplication.playModeStateChanged += OnPlayModeStateChanged;
        }

        private static void OnPlayModeStateChanged(PlayModeStateChange state)
        {
            if (!PlayModeIntegration) return;

            switch (state)
            {
                case PlayModeStateChange.ExitingEditMode:
                    // Disable Unity's XR loader so it doesn't create an OpenXR session
                    // that conflicts with the SA session + causes Game View crash.
                    bool xrWasEnabled = DisableXRLoader();
                    SessionState.SetBool(kXRWasEnabledKey, xrWasEnabled);
                    SessionState.SetBool(kPlayModeStartedKey, !IsRunning);
                    break;

                case PlayModeStateChange.EnteredPlayMode:
                    // Domain reload wiped statics — read from SessionState
                    if (SessionState.GetBool(kPlayModeStartedKey, false) && !IsRunning)
                        Start();
                    break;

                case PlayModeStateChange.ExitingPlayMode:
                    if (SessionState.GetBool(kPlayModeStartedKey, false) && IsRunning)
                    {
                        Stop();
                        SessionState.SetBool(kPlayModeStartedKey, false);
                    }
                    break;

                case PlayModeStateChange.EnteredEditMode:
                    if (SessionState.GetBool(kXRWasEnabledKey, false))
                    {
                        EnableXRLoader();
                        SessionState.SetBool(kXRWasEnabledKey, false);
                    }
                    var sceneView = SceneView.lastActiveSceneView;
                    if (sceneView != null)
                        sceneView.Focus();
                    break;
            }
        }

        private static bool DisableXRLoader()
        {
            var xrSettings = UnityEngine.XR.Management.XRGeneralSettings.Instance;
            if (xrSettings == null || xrSettings.Manager == null) return false;

            var loaders = xrSettings.Manager.activeLoaders;
            for (int i = 0; i < loaders.Count; i++)
            {
                if (loaders[i].GetType().Name.Contains("OpenXR"))
                {
                    string assetPath = AssetDatabase.GetAssetPath(loaders[i]);
                    if (!string.IsNullOrEmpty(assetPath))
                        SessionState.SetString(kXRLoaderAssetPathKey, assetPath);
                    xrSettings.Manager.TryRemoveLoader(loaders[i]);
                    return true;
                }
            }
            return false;
        }

        private static void EnableXRLoader()
        {
            var xrSettings = UnityEngine.XR.Management.XRGeneralSettings.Instance;
            if (xrSettings == null || xrSettings.Manager == null) return;

            foreach (var loader in xrSettings.Manager.activeLoaders)
                if (loader.GetType().Name.Contains("OpenXR")) return;

            string assetPath = SessionState.GetString(kXRLoaderAssetPathKey, "");
            if (!string.IsNullOrEmpty(assetPath))
            {
                var loader = AssetDatabase.LoadAssetAtPath<UnityEngine.XR.Management.XRLoader>(assetPath);
                if (loader != null)
                {
                    xrSettings.Manager.TryAddLoader(loader);
                    SessionState.EraseString(kXRLoaderAssetPathKey);
                    return;
                }
            }

            var allLoaders = Resources.FindObjectsOfTypeAll<UnityEngine.XR.Management.XRLoader>();
            foreach (var loader in allLoaders)
            {
                if (loader.GetType().Name.Contains("OpenXR"))
                {
                    xrSettings.Manager.TryAddLoader(loader);
                    return;
                }
            }
        }

        private static void FocusPreviewWindow()
        {
            // Find and focus the DisplayXR Preview window
            var windows = Resources.FindObjectsOfTypeAll<DisplayXRPreviewWindow>();
            if (windows.Length > 0)
            {
                windows[0].Focus();
            }
            else
            {
                // Open the preview window if not already open
                DisplayXRPreviewWindow.ShowWindow();
            }
        }

        /// <summary>
        /// Start the standalone preview session.
        /// Loads the DisplayXR runtime directly and creates an OpenXR session.
        /// </summary>
        public static bool Start()
        {
            if (IsRunning)
                return true;

            string runtimeJson = FindRuntimeJson();
            if (string.IsNullOrEmpty(runtimeJson))
            {
                Debug.LogError("[DisplayXR-SA] Cannot find DisplayXR runtime. " +
                    "Set XR_RUNTIME_JSON environment variable or install the runtime.");
                return false;
            }

            Debug.Log($"[DisplayXR-SA] Starting with runtime: {runtimeJson}");

            int result = DisplayXRNative.displayxr_standalone_start(runtimeJson);

            if (result != 0)
            {
                s_IsRunning = true;
                Debug.Log("[DisplayXR-SA] Standalone session started");
                StartPolling();
                RefreshDisplayInfo();
                CreateRenderRig();
                return true;
            }

            Debug.LogError("[DisplayXR-SA] Failed to start standalone session");
            return false;
        }

        /// <summary>
        /// Stop the standalone preview session and release all resources.
        /// </summary>
        public static void Stop()
        {
            if (s_Stopping) return; // Prevent re-entrant teardown

            // If we're inside poll_events, the native session is still on the call stack.
            // Destroying it now would cause a use-after-free crash when poll_events returns.
            // Defer the actual teardown until poll_events finishes.
            if (s_InPollEvents)
            {
                s_DeferredStop = true;
                s_IsRunning = false;
                return;
            }

            DoStop();
        }

        private static void DoStop()
        {
            s_Stopping = true;
            s_IsRunning = false;

            StopPolling();
            DestroyRenderRig();

            try
            {
                DisplayXRNative.displayxr_standalone_stop();
            }
            catch (Exception e)
            {
                Debug.LogWarning($"[DisplayXR-SA] Exception during stop: {e.Message}");
            }

            DisplayInfo = default;
            s_DeferredStop = false;
            s_Stopping = false;
            Debug.Log("[DisplayXR-SA] Standalone session stopped");
        }

        private static void StartPolling()
        {
            if (s_Polling) return;
            s_Polling = true;
            EditorApplication.update += FrameTick;
        }

        private static void StopPolling()
        {
            if (!s_Polling) return;
            s_Polling = false;
            EditorApplication.update -= FrameTick;
        }

        private static void FrameTick()
        {
            if (s_Stopping || !IsRunning)
            {
                StopPolling();
                return;
            }

            // 1. Poll OpenXR events (session state changes)
            // NOTE: poll_events pumps macOS events, which can trigger Unity UI callbacks
            // (e.g. play button → ExitPlayMode → Stop). We guard against destroying
            // the session while it's still on the call stack.
            try
            {
                s_InPollEvents = true;
                DisplayXRNative.displayxr_standalone_poll_events();
                s_InPollEvents = false;
            }
            catch (Exception e)
            {
                s_InPollEvents = false;
                Debug.LogError($"[DisplayXR-SA] poll_events crashed: {e.Message}. Stopping session.");
                Stop();
                return;
            }

            // Handle deferred stop (Stop() was called re-entrantly during poll_events)
            if (s_DeferredStop)
            {
                DoStop();
                return;
            }

            // 2. Begin frame (xrWaitFrame + xrBeginFrame)
            int ok = DisplayXRNative.displayxr_standalone_begin_frame(out int shouldRender);
            if (ok == 0)
                return;

            if (shouldRender == 0)
            {
                DisplayXRNative.displayxr_standalone_end_frame_empty();
                return;
            }

            // 3. Push tunables + display pose from scene rig component
            PushRigParameters();

            // 4. Refresh mode info (may have changed via rendering mode switch)
            RefreshModeInfo();
            EnsureRigMatchesMode();

            // 5. Compute Kooima view/projection matrices for N views
            int nViews = (int)s_ViewCount;
            float[] viewMats = new float[nViews * 16];
            float[] projMats = new float[nViews * 16];

            DisplayXRNative.displayxr_standalone_compute_views(
                (uint)nViews, s_NearZ, s_FarZ,
                viewMats, projMats, out int matricesValid);

            if (matricesValid != 0 && s_EyeCams != null && s_AtlasRT != null)
            {
                s_FrameCount++;

                // Render each eye into its tile within the atlas
                for (int eye = 0; eye < nViews && eye < s_EyeCams.Length; eye++)
                {
                    if (s_EyeCams[eye] == null) continue;

                    uint tileX = (uint)eye % s_TileColumns;
                    uint tileY = (uint)eye / s_TileColumns;
                    float[] eyeView = new float[16];
                    float[] eyeProj = new float[16];
                    System.Array.Copy(viewMats, eye * 16, eyeView, 0, 16);
                    System.Array.Copy(projMats, eye * 16, eyeProj, 0, 16);

                    RenderEyeToAtlas(s_EyeCams[eye], s_AtlasRT, eyeView, eyeProj,
                        (int)(tileX * s_ViewWidth), (int)(tileY * s_ViewHeight),
                        (int)s_ViewWidth, (int)s_ViewHeight);
                }

                IntPtr atlasNative = s_AtlasRT.GetNativeTexturePtr();
                DisplayXRNative.displayxr_standalone_submit_frame_atlas(atlasNative);
            }
            else
            {
                s_FrameCount++;
                DisplayXRNative.displayxr_standalone_end_frame_empty();
            }

            // Refresh cached state for UI
            RefreshEyePositions();
        }

        // ================================================================
        // Render rig: two hidden cameras rendering to RenderTextures
        // ================================================================

        private static void CreateRenderRig()
        {
            // Query current mode tiling info from native
            RefreshModeInfo();

            // Get atlas swapchain size from runtime
            DisplayXRNative.displayxr_standalone_get_swapchain_size(out s_AtlasWidth, out s_AtlasHeight);
            if (s_AtlasWidth == 0 || s_AtlasHeight == 0)
            {
                s_AtlasWidth = DisplayInfo.displayPixelWidth * 2;
                s_AtlasHeight = DisplayInfo.displayPixelHeight;
            }
            if (s_AtlasWidth == 0 || s_AtlasHeight == 0)
            {
                s_AtlasWidth = 3840;
                s_AtlasHeight = 1080;
            }

            Debug.Log($"[DisplayXR-SA] Creating atlas render rig: {s_AtlasWidth}x{s_AtlasHeight} " +
                $"({s_ViewCount} views, {s_TileColumns}x{s_TileRows} tiles, {s_ViewWidth}x{s_ViewHeight} per view)");

            // Create single atlas RenderTexture
            s_AtlasRT = new RenderTexture((int)s_AtlasWidth, (int)s_AtlasHeight, 24, RenderTextureFormat.ARGB32);
            s_AtlasRT.name = "DisplayXR_SA_Atlas";
            s_AtlasRT.Create();

            // Create hidden camera rig with N eye cameras
            s_RigRoot = new GameObject("DisplayXR_SA_Rig");
            s_RigRoot.hideFlags = HideFlags.HideAndDontSave;

            s_EyeCams = new Camera[s_ViewCount];
            for (int i = 0; i < (int)s_ViewCount; i++)
            {
                var eyeGo = new GameObject($"Eye{i}");
                eyeGo.hideFlags = HideFlags.HideAndDontSave;
                eyeGo.transform.SetParent(s_RigRoot.transform);
                s_EyeCams[i] = eyeGo.AddComponent<Camera>();
                s_EyeCams[i].enabled = false; // We render manually via Camera.Render()
                s_EyeCams[i].targetTexture = s_AtlasRT;
            }

            // Restore camera selection (reads from SessionState) and clone settings
            RestoreSelection();
        }

        private static void DestroyRenderRig()
        {
            if (s_EyeCams != null)
            {
                foreach (var cam in s_EyeCams)
                    if (cam != null) UnityEngine.Object.DestroyImmediate(cam.gameObject);
            }
            if (s_RigRoot != null) UnityEngine.Object.DestroyImmediate(s_RigRoot);
            s_EyeCams = null;
            s_RigRoot = null;

            if (s_AtlasRT != null) { s_AtlasRT.Release(); UnityEngine.Object.DestroyImmediate(s_AtlasRT); }
            s_AtlasRT = null;
        }

        private static void RenderEyeToAtlas(Camera cam, RenderTexture atlas,
            float[] viewMat, float[] projMat,
            int vpX, int vpY, int vpW, int vpH)
        {
            Matrix4x4 view = ColumnMajorToMatrix4x4(viewMat);
            Matrix4x4 proj = ColumnMajorToMatrix4x4(projMat);

            // Convert view matrix from OpenXR convention (right-hand, -Z forward) to
            // Unity world convention (left-hand, +Z forward) by negating column 2.
            // Without this, the camera ends up on the wrong side of the display:
            // at +Z looking toward -Z instead of at -Z looking toward +Z.
            // This causes incorrect lighting (back-lit instead of front-lit).
            view.m02 = -view.m02;
            view.m12 = -view.m12;
            view.m22 = -view.m22;
            view.m32 = -view.m32;

            // Flip projection Y on macOS only: Metal RenderTextures are Y-inverted
            // and Unity doesn't auto-correct when projectionMatrix is set manually.
            // On D3D12/D3D11, the native memory layout matches without this flip.
#if UNITY_EDITOR_OSX
            proj.m10 = -proj.m10;
            proj.m11 = -proj.m11;
            proj.m12 = -proj.m12;
            proj.m13 = -proj.m13;
#endif

            // Reset matrices first so Unity recalculates internal lighting state
            // from the Transform (shadows, light culling, shader built-ins).
            cam.ResetWorldToCameraMatrix();
            cam.ResetProjectionMatrix();

            // Set the camera transform to match the view matrix. Unity's lighting
            // system reads from the Transform, not from worldToCameraMatrix.
            Matrix4x4 viewInv = view.inverse;
            cam.transform.position = new Vector3(viewInv.m03, viewInv.m13, viewInv.m23);
            Vector3 forward = new Vector3(-viewInv.m02, -viewInv.m12, -viewInv.m22);
            Vector3 up = new Vector3(viewInv.m01, viewInv.m11, viewInv.m21);
            cam.transform.rotation = Quaternion.LookRotation(forward, up);

            // Override with exact Kooima matrices for rendering
            cam.worldToCameraMatrix = view;
            cam.projectionMatrix = proj;
            cam.targetTexture = atlas;
            cam.pixelRect = new Rect(vpX, vpY, vpW, vpH);

            // The view matrix Z-flip reverses the determinant, flipping triangle
            // winding order. On macOS, the projection Y-flip reverses it again,
            // plus Metal's RT Y-inversion adds a third flip → need invertCulling.
            // On Windows D3D12, only the Z-flip applies → normal culling is correct.
#if UNITY_EDITOR_OSX
            GL.invertCulling = true;
#endif
            cam.Render();
#if UNITY_EDITOR_OSX
            GL.invertCulling = false;
#endif

            // Reset matrices after rendering so cam.fieldOfView returns the
            // original value on the next frame. Without this, the Y-flipped
            // projection (m11 < 0) causes fieldOfView to return a bogus value,
            // which corrupts halfTanVfov in PushRigParameters for camera-centric rigs.
            cam.ResetWorldToCameraMatrix();
            cam.ResetProjectionMatrix();
        }

        private static void RefreshModeInfo()
        {
            DisplayXRNative.displayxr_standalone_get_current_mode_info(
                out uint vc, out uint tc, out uint tr,
                out uint vw, out uint vh,
                out float _, out float _2,
                out int _3);

            if (vc > 0 && tc > 0 && tr > 0 && vw > 0 && vh > 0)
            {
                s_ViewCount = vc;
                s_TileColumns = tc;
                s_TileRows = tr;
                s_ViewWidth = vw;
                s_ViewHeight = vh;
            }
        }

        /// <summary>
        /// Ensure the eye camera array and atlas RT match the current mode.
        /// Called each frame to handle rendering mode switches.
        /// </summary>
        private static void EnsureRigMatchesMode()
        {
            if (s_EyeCams == null || s_AtlasRT == null) return;

            // Check if we need more/fewer cameras
            if (s_EyeCams.Length != (int)s_ViewCount)
            {
                // Rebuild cameras
                foreach (var cam in s_EyeCams)
                    if (cam != null) UnityEngine.Object.DestroyImmediate(cam.gameObject);

                s_EyeCams = new Camera[s_ViewCount];
                for (int i = 0; i < (int)s_ViewCount; i++)
                {
                    var eyeGo = new GameObject($"Eye{i}");
                    eyeGo.hideFlags = HideFlags.HideAndDontSave;
                    eyeGo.transform.SetParent(s_RigRoot.transform);
                    s_EyeCams[i] = eyeGo.AddComponent<Camera>();
                    s_EyeCams[i].enabled = false;
                    s_EyeCams[i].targetTexture = s_AtlasRT;
                }
                ApplyCameraSelection();
            }
        }

        private static Matrix4x4 ColumnMajorToMatrix4x4(float[] m)
        {
            var mat = new Matrix4x4();
            mat.m00 = m[0];  mat.m10 = m[1];  mat.m20 = m[2];  mat.m30 = m[3];
            mat.m01 = m[4];  mat.m11 = m[5];  mat.m21 = m[6];  mat.m31 = m[7];
            mat.m02 = m[8];  mat.m12 = m[9];  mat.m22 = m[10]; mat.m32 = m[11];
            mat.m03 = m[12]; mat.m13 = m[13]; mat.m23 = m[14]; mat.m33 = m[15];
            return mat;
        }

        // ================================================================
        // Camera discovery + selection
        // ================================================================

        public static CameraEntry[] DiscoverCameras()
        {
            var cameras = UnityEngine.Object.FindObjectsByType<Camera>(FindObjectsInactive.Exclude, FindObjectsSortMode.None);
            var entries = new List<CameraEntry>();

            foreach (var cam in cameras)
            {
                // Skip our own hidden render rig cameras
                if (cam.gameObject.hideFlags.HasFlag(HideFlags.HideAndDontSave))
                    continue;

                var entry = new CameraEntry { camera = cam };
                string name = cam.gameObject.name;

                if (cam.GetComponent<DisplayXRDisplay>() != null)
                {
                    entry.category = CameraCategory.DisplayRig;
                    entry.displayName = $"{name} (Display)";
                }
                else if (cam.GetComponent<DisplayXRCamera>() != null)
                {
                    entry.category = CameraCategory.CameraRig;
                    entry.displayName = $"{name} (Camera)";
                }
                else
                {
                    entry.category = CameraCategory.RegularCamera;
                    entry.displayName = name;
                }

                entries.Add(entry);
            }

            // Sort: DisplayXR rigs first, then regular cameras
            entries.Sort((a, b) =>
            {
                int catCmp = a.category.CompareTo(b.category);
                if (catCmp != 0) return catCmp;
                return string.Compare(a.displayName, b.displayName, StringComparison.Ordinal);
            });

            return entries.ToArray();
        }

        public static void SelectCamera(Camera cam)
        {
            s_SelectedSourceCamera = cam;
            if (cam != null)
                SessionState.SetString(kSelectedCameraIDKey, cam.GetEntityId().ToString());
            else
                SessionState.EraseString(kSelectedCameraIDKey);

            if (IsRunning)
                ApplyCameraSelection();

            // Sync the rig manager so rig gating and input controller agree
            DisplayXRRigManager.ActiveCamera = cam;
        }

        public static void RestoreSelection()
        {
            string savedID = SessionState.GetString(kSelectedCameraIDKey, "");
            if (!string.IsNullOrEmpty(savedID) && int.TryParse(savedID, out int id) && id != 0)
            {
#pragma warning disable CS0618 // EntityId int cast — no non-deprecated constructor available yet
                var obj = EditorUtility.EntityIdToObject((EntityId)id) as Camera;
#pragma warning restore CS0618
                if (obj != null)
                {
                    s_SelectedSourceCamera = obj;
                    ApplyCameraSelection();
                    return;
                }
            }

            // Fallback: first DisplayXR rig, then Camera.main, then any camera
            var entries = DiscoverCameras();
            if (entries.Length > 0)
            {
                // Prefer a DisplayXR rig (already sorted first)
                s_SelectedSourceCamera = entries[0].camera;
            }
            else
            {
                s_SelectedSourceCamera = Camera.main;
                if (s_SelectedSourceCamera == null)
                {
                    var allCams = Camera.allCameras;
                    if (allCams.Length > 0) s_SelectedSourceCamera = allCams[0];
                }
            }

            if (s_SelectedSourceCamera != null)
            {
                SessionState.SetString(kSelectedCameraIDKey, s_SelectedSourceCamera.GetEntityId().ToString());
                ApplyCameraSelection();
            }
        }

        private static void ApplyCameraSelection()
        {
            if (s_SelectedSourceCamera == null || s_EyeCams == null) return;
            DisplayXRRigManager.ActiveCamera = s_SelectedSourceCamera;
            CloneSourceCameraSettings(s_SelectedSourceCamera);
        }

        private static void CloneSourceCameraSettings(Camera source)
        {
            if (s_EyeCams == null) return;

            // The source camera may have been suppressed by GameViewOverlay
            // (cullingMask=0, clearFlags=SolidColor, bg=black). Save and
            // temporarily restore before cloning so eye cameras render the scene.
            int savedMask = source.cullingMask;
            CameraClearFlags savedClear = source.clearFlags;
            Color savedBg = source.backgroundColor;
            bool suppressed = savedMask == 0;
            if (suppressed)
            {
                source.cullingMask = -1; // Everything
                source.clearFlags = CameraClearFlags.Skybox;
            }

            foreach (var cam in s_EyeCams)
            {
                if (cam == null) continue;
                cam.CopyFrom(source);
                cam.enabled = false;
                cam.targetTexture = s_AtlasRT;
                cam.allowHDR = false;
            }

            // Restore suppression on the source camera
            if (suppressed)
            {
                source.cullingMask = savedMask;
                source.clearFlags = savedClear;
                source.backgroundColor = savedBg;
            }
        }

        public static CameraCategory GetSelectedCameraCategory()
        {
            if (s_SelectedSourceCamera == null) return CameraCategory.RegularCamera;
            if (s_SelectedSourceCamera.GetComponent<DisplayXRDisplay>() != null)
                return CameraCategory.DisplayRig;
            if (s_SelectedSourceCamera.GetComponent<DisplayXRCamera>() != null)
                return CameraCategory.CameraRig;
            return CameraCategory.RegularCamera;
        }

        // ================================================================
        // Rig parameter sync (tunables + display pose from scene component)
        // ================================================================

        private static void PushRigParameters()
        {
            // Ensure we have a selected camera
            if (s_SelectedSourceCamera == null)
                RestoreSelection();

            Camera cam = s_SelectedSourceCamera;

            // In play mode, sync with rig manager (Tab key may have changed active camera)
            if (Application.isPlaying)
            {
                var overrideCam = DisplayXRRigManager.ActiveCamera;
                if (overrideCam != null && overrideCam != cam)
                {
                    cam = overrideCam;
                    if (s_SelectedSourceCamera != cam)
                        SelectCamera(cam);
                }
                else if (cam != null)
                {
                    DisplayXRRigManager.ActiveCamera = cam;
                }
            }

            if (cam == null) return;

            var displayRig = cam.GetComponent<DisplayXRDisplay>();
            if (displayRig != null)
            {
                float vdh = displayRig.virtualDisplayHeight;
                if (vdh <= 0f && DisplayInfo.isValid)
                    vdh = DisplayInfo.displayHeightMeters;

                s_NearZ = cam.nearClipPlane;
                s_FarZ = cam.farClipPlane;

                DisplayXRNative.displayxr_standalone_set_tunables(
                    displayRig.ipdFactor,
                    displayRig.parallaxFactor,
                    displayRig.perspectiveFactor,
                    vdh,
                    0f, // invConvergenceDistance (display-centric = 0)
                    0f, // fovOverride (display-centric = 0)
                    s_NearZ, s_FarZ,
                    0); // cameraCentric = false

                Transform t = displayRig.transform;
                DisplayXRNative.displayxr_standalone_set_display_pose(
                    t.position.x, t.position.y, t.position.z,
                    t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w,
                    t.lossyScale.x, t.lossyScale.y, t.lossyScale.z,
                    1);
                return;
            }

            var cameraRig = cam.GetComponent<DisplayXRCamera>();
            if (cameraRig != null)
            {
                s_NearZ = cam.nearClipPlane;
                s_FarZ = cam.farClipPlane;
                float halfTanVfov = Mathf.Tan(cam.fieldOfView * 0.5f * Mathf.Deg2Rad);

                DisplayXRNative.displayxr_standalone_set_tunables(
                    cameraRig.ipdFactor,
                    cameraRig.parallaxFactor,
                    1.0f, // perspectiveFactor (camera-centric = 1)
                    0f,   // virtualDisplayHeight (camera-centric = 0)
                    cameraRig.invConvergenceDistance,
                    halfTanVfov,
                    s_NearZ, s_FarZ,
                    1); // cameraCentric = true

                Transform t = cameraRig.transform;
                DisplayXRNative.displayxr_standalone_set_display_pose(
                    t.position.x, t.position.y, t.position.z,
                    t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w,
                    1f, 1f, 1f, // camera-centric: no scale
                    1);
                return;
            }

            // Regular camera — camera-centric defaults (2D-equivalent)
            s_NearZ = cam.nearClipPlane;
            s_FarZ = cam.farClipPlane;
            float defaultHalfTanVfov = Mathf.Tan(cam.fieldOfView * 0.5f * Mathf.Deg2Rad);

            DisplayXRNative.displayxr_standalone_set_tunables(
                1.0f, // ipdFactor
                1.0f, // parallaxFactor
                1.0f, // perspectiveFactor
                0f,   // virtualDisplayHeight
                0f,   // invConvergenceDistance (infinite convergence → parallel)
                defaultHalfTanVfov,
                s_NearZ, s_FarZ,
                1);   // cameraCentric = true

            Transform ct = cam.transform;
            DisplayXRNative.displayxr_standalone_set_display_pose(
                ct.position.x, ct.position.y, ct.position.z,
                ct.rotation.x, ct.rotation.y, ct.rotation.z, ct.rotation.w,
                1f, 1f, 1f,
                1);
        }

        // ================================================================
        // Display info / eye tracking / shared texture refresh
        // ================================================================

        private static void RefreshDisplayInfo()
        {
            DisplayXRNative.displayxr_standalone_get_display_info(
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
        }

        private static void RefreshEyePositions()
        {
            DisplayXRNative.displayxr_standalone_get_eye_positions(
                out float lx, out float ly, out float lz,
                out float rx, out float ry, out float rz,
                out int tracked);

            LeftEyePosition = new Vector3(lx, ly, lz);
            RightEyePosition = new Vector3(rx, ry, rz);
            IsEyeTracked = tracked != 0;
        }

        private static string FindRuntimeJson()
        {
            // 1. XR_RUNTIME_JSON environment variable (highest priority on all platforms)
            string envPath = Environment.GetEnvironmentVariable("XR_RUNTIME_JSON");
            if (!string.IsNullOrEmpty(envPath) && File.Exists(envPath))
                return envPath;

#if UNITY_EDITOR_WIN
            // 2. Windows: read ActiveRuntime from the OpenXR registry (set by the installer)
            //    Key: HKLM\Software\Khronos\OpenXR\1  Value: ActiveRuntime
            //    The installer writes this with SetRegView 64, so we read the 64-bit hive.
            try
            {
                using (var hklm = Microsoft.Win32.RegistryKey.OpenBaseKey(
                    Microsoft.Win32.RegistryHive.LocalMachine,
                    Microsoft.Win32.RegistryView.Registry64))
                using (var key = hklm.OpenSubKey(@"Software\Khronos\OpenXR\1"))
                {
                    if (key != null)
                    {
                        string activeRuntime = key.GetValue("ActiveRuntime") as string;
                        if (!string.IsNullOrEmpty(activeRuntime) && File.Exists(activeRuntime))
                            return activeRuntime;
                    }
                }
            }
            catch (System.Exception e)
            {
                Debug.LogWarning($"[DisplayXR-SA] Failed to read OpenXR registry: {e.Message}");
            }
#elif UNITY_EDITOR_OSX
            // 2. macOS: well-known paths
            string[] searchPaths = new[]
            {
                // Local development build
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                    "Documents/GitHub/openxr-3d-display/_package/DisplayXR-macOS/openxr_displayxr.json"),
                // System-wide active runtime
                "/etc/xdg/openxr/1/active_runtime.json",
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                    ".config/openxr/1/active_runtime.json"),
            };

            foreach (string path in searchPaths)
            {
                if (File.Exists(path))
                    return path;
            }
#endif
            return null;
        }

        private static void OnBeforeAssemblyReload()
        {
            if (IsRunning)
            {
                Debug.Log("[DisplayXR-SA] Domain reload: stopping standalone session");
                Stop();
            }
        }

        private static void OnEditorQuitting()
        {
            if (IsRunning)
                Stop();
        }
    }
}
