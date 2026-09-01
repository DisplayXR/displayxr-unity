// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;

namespace DisplayXR.Editor
{
    /// <summary>
    /// (#265) Edit-mode Game-view framing preview for the display-centric rig.
    ///
    /// <para>
    /// <b>The problem.</b> <see cref="DisplayXRDisplay"/> treats its transform as the virtual
    /// <i>display plane</i> — the runtime puts the eyes in front of it and projects through
    /// it. Outside Play there is no provider and no Kooima, so Unity renders a plain
    /// perspective camera sitting <i>on</i> that plane: the Game view shows the scene from
    /// the wrong place (typically near-clipping straight through the content), and the
    /// framing only snaps to what you authored once you press Play.
    /// </para>
    ///
    /// <para>
    /// <b>Why this does not simply move the camera.</b> The obvious fix — an inspector button
    /// that backs the camera out — is wrong for this rig, and quietly so: the transform IS
    /// the display plane, so baking a pull-back into it moves the virtual display itself and
    /// changes what the scene means in Play.
    /// </para>
    ///
    /// <para>
    /// <b>And it does not move the transform even temporarily.</b> Displacing it around the
    /// render and restoring it afterwards would work visually, but it mutates a serialized
    /// object during rendering: it can mark the scene dirty (a phantom unsaved-changes
    /// asterisk on a scene nobody edited), it fights a drag in progress, and any begin
    /// without a matching end strands the rig somewhere else. Instead the camera's
    /// <see cref="Camera.worldToCameraMatrix"/> is overridden for the render and reset after
    /// — the view moves, the scene data never does.
    /// </para>
    ///
    /// <para>
    /// The offset is <see cref="DisplayXRDisplay.TryGetVirtualDisplayPullback"/> — the same
    /// single implementation the runtime 2D fallback (#256) uses, so the preview cannot drift
    /// away from the real behavior.
    /// </para>
    ///
    /// <para>
    /// This is a <b>framing</b> preview, not a stereo preview. Play Mode remains the only
    /// place stereo is real (there is deliberately no edit-mode preview system — see
    /// ADR-002/003, superseded by the #166 provider migration). It answers "am I pointing at
    /// the right thing", not "how does the depth feel".
    /// </para>
    ///
    /// Toggle from the rig's inspector, or DisplayXR &gt; Edit-Mode Framing Preview.
    /// </summary>
    [InitializeOnLoad]
    internal static class DisplayXRDisplayFramingPreview
    {
        const string kPrefKey = "DisplayXR.EditModeFramingPreview";
        const string kMenu    = "DisplayXR/Edit-Mode Framing Preview";

        /// <summary>Whether the preview is active. Editor-local (EditorPrefs), never scene data.</summary>
        public static bool Enabled
        {
            get => EditorPrefs.GetBool(kPrefKey, true);
            set { EditorPrefs.SetBool(kPrefKey, value); SceneView.RepaintAll(); }
        }

        // Exactly one camera can be mid-override at a time (cameras render serially), but the
        // restore is keyed on the camera anyway so a mismatched begin/end can never strand an
        // override on the wrong camera.
        static Camera s_overriddenCam;

        static DisplayXRDisplayFramingPreview()
        {
            Camera.onPreCull     -= OnPreCull;      // BiRP
            Camera.onPreCull     += OnPreCull;
            Camera.onPostRender  -= OnPostRender;
            Camera.onPostRender  += OnPostRender;
            RenderPipelineManager.beginCameraRendering -= OnBeginSRP;   // URP / HDRP
            RenderPipelineManager.beginCameraRendering += OnBeginSRP;
            RenderPipelineManager.endCameraRendering   -= OnEndSRP;
            RenderPipelineManager.endCameraRendering   += OnEndSRP;
            EditorApplication.playModeStateChanged     -= OnPlayModeChanged;
            EditorApplication.playModeStateChanged     += OnPlayModeChanged;
        }

        [MenuItem(kMenu)]
        static void ToggleMenu() => Enabled = !Enabled;

        [MenuItem(kMenu, validate = true)]
        static bool ToggleMenuValidate()
        {
            Menu.SetChecked(kMenu, Enabled);
            return true;
        }

        // Entering Play tears down edit-mode rendering mid-flight; make sure nothing is left
        // displaced if a begin fired without its matching end.
        static void OnPlayModeChanged(PlayModeStateChange _) => Restore();

        static void OnBeginSRP(ScriptableRenderContext _, Camera cam) => OnPreCull(cam);
        static void OnEndSRP(ScriptableRenderContext _, Camera cam)   => OnPostRender(cam);

        static void OnPreCull(Camera cam)
        {
            // Play Mode is the real thing — never touch it. Also skip the Scene view and any
            // other camera: only the rig's OWN camera is repositioned, so the Scene view
            // keeps showing the true authored layout (with the display-plane gizmos) while
            // the Game view shows the framing.
            if (Application.isPlaying || !Enabled || cam == null) return;
            if (cam.cameraType != CameraType.Game) return;
            if (s_overriddenCam != null) return;                // already overridden; be re-entrant-safe

            var rig = cam.GetComponent<DisplayXRDisplay>();
            if (rig == null || !rig.enabled || !rig.isActiveAndEnabled) return;
            if (rig.bootSplashOverlay) return;                  // owns its own framing
            if (!rig.TryGetVirtualDisplayPullback(cam, out float d, out _)) return;

            var xf = rig.transform;
            // Position only. Rotation is already correct (the display plane's forward is the
            // viewing direction) and the FOV is the camera's own, which is what `d` was
            // derived from — changing either would make the preview disagree with the 2D
            // fallback it mirrors.
            Vector3 eye = xf.position - xf.forward * d;

            // Unity's view space is right-handed with -Z forward, so the camera-to-world
            // matrix carries a Z flip; worldToCameraMatrix is its inverse.
            var camToWorld = Matrix4x4.TRS(eye, xf.rotation, new Vector3(1f, 1f, -1f));
            cam.worldToCameraMatrix = camToWorld.inverse;
            // Cull from the SAME viewpoint. Without this Unity keeps culling from the
            // transform's real position, which sits closer to the content than the preview
            // eye — so geometry that the pulled-back frustum newly includes gets culled and
            // pops in only on Play.
            cam.cullingMatrix = cam.projectionMatrix * cam.worldToCameraMatrix;

            s_overriddenCam = cam;
        }

        static void OnPostRender(Camera cam)
        {
            if (s_overriddenCam == null || cam != s_overriddenCam) return;
            Restore();
        }

        static void Restore()
        {
            if (s_overriddenCam != null)
            {
                s_overriddenCam.ResetWorldToCameraMatrix();
                s_overriddenCam.ResetCullingMatrix();
            }
            s_overriddenCam = null;
        }
    }
}
