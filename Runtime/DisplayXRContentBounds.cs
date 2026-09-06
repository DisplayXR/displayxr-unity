// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Collections.Generic;
using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Tells the runtime <b>where this app's content is on screen</b>, so the rear depth
    /// budget (<see cref="DisplayXRDepthBudget"/>, <c>XR_DXR_depth_budget</c> v2, issue
    /// #318) is decided from the desktop <i>behind the content</i> rather than from the
    /// whole window.
    ///
    /// <para>
    /// <b>Why it matters.</b> The runtime opens the rear clip only when the background
    /// behind the app carries no horizontal depth cue. Without this component it judges
    /// everything under the window — so an empty Notepad's own menu and status bars
    /// (measured cue 0.93 on the panel run that motivated v2) keep the budget shut even
    /// when the avatar is in the opposite corner and nowhere near them. The app is the
    /// only party that knows where its geometry lands, so it says.
    /// </para>
    /// <para>
    /// <b>Usage.</b> Drop it on the content root — the avatar, the model, whatever
    /// occupies the rear volume. Each frame it unions the world-space
    /// <see cref="Renderer.bounds"/> of its own renderers and its children's and hands
    /// that box to the provider, which projects it through every eye and reports the
    /// union. Leave <see cref="renderers"/> empty for that automatic collection, or fill
    /// it in to pin the set explicitly. Nothing else to wire.
    /// </para>
    /// <para>
    /// <b>It is a hint, never a requirement.</b> No value can fail a frame: a box that
    /// projects behind an eye, or clamps away off-canvas, falls back to the whole canvas
    /// — the conservative answer, which is exactly the behaviour of an app that never
    /// reports bounds at all. Do not add ROI logic of your own on top; the dilation and
    /// the verdict are the runtime's.
    /// </para>
    /// <para>
    /// <b>List only what occupies the rear volume.</b> The reported region is a
    /// <i>rectangle</i> — the union of the projected box over all eyes — so every extra
    /// renderer widens the patch of desktop the runtime judges. Two cases cost you the
    /// whole benefit:
    /// <list type="bullet">
    /// <item><description><b>Anything reaching behind the eye.</b> A ground plane or
    /// skybox has AABB corners behind the camera, which cannot be projected, so the
    /// provider conservatively reports the <i>whole canvas</i> — back to judging
    /// everything. Measured on the panel: adding a floor quad to the list took a
    /// narrowed region straight to <c>roi=0,0,202,320 (whole preview)</c>.</description></item>
    /// <item><description><b>Geometry that is not the subject.</b> A backdrop that fills
    /// the window makes the region the window. Report the avatar, the model, the thing
    /// whose back you want to see — not its scenery.</description></item>
    /// </list>
    /// A rect is the granularity the extension offers today (<c>XrContentBoundsDXR</c>
    /// carries an <c>XrRect2Df</c>), so it cannot express a silhouette; a finer channel
    /// is under discussion for a future spec version.
    /// </para>
    /// </summary>
    [AddComponentMenu("DisplayXR/DisplayXR Content Bounds")]
    [DisallowMultipleComponent]
    public class DisplayXRContentBounds : MonoBehaviour
    {
        [Tooltip("Renderers whose world bounds define the content box. Leave EMPTY to " +
                 "collect every Renderer under this GameObject automatically (the usual " +
                 "case — drop this on the avatar root and forget it).")]
        public Renderer[] renderers;

        [Tooltip("Include disabled renderers in the automatic collection. Off by " +
                 "default: a hidden mesh occupies no pixels, so counting it would " +
                 "widen the measured region for nothing.")]
        public bool includeInactiveRenderers = false;

        [Tooltip("Seconds between automatic re-scans for new/removed child renderers. " +
                 "The bounds themselves update EVERY frame from the cached list — this " +
                 "is only how often the list is rebuilt, because GetComponentsInChildren " +
                 "allocates. 0 = rescan every frame (only worth it if the hierarchy " +
                 "changes constantly). Call RefreshRenderers() after spawning content.")]
        [Min(0f)]
        public float rendererRescanInterval = 1.0f;

        [Tooltip("Use the explicit box below (LOCAL space, transformed by this " +
                 "transform) instead of the renderer bounds. For content the renderer " +
                 "bounds don't describe — a particle system, a procedural mesh — or to " +
                 "pin the region while debugging.")]
        public bool useManualBounds = false;

        [Tooltip("Manual content box in this transform's LOCAL space (center + size).")]
        public Bounds manualBounds = new Bounds(Vector3.zero, Vector3.one);

        [Tooltip("Ask skinned meshes for their REAL per-frame bounds instead of the " +
                 "static import-time box.\n\n" +
                 "Unity's default SkinnedMeshRenderer.bounds is the box baked at import " +
                 "to cover the WHOLE animation set — for a humanoid that is arms-out, " +
                 "which stays arms-out no matter what the avatar is doing. Reporting " +
                 "that hands the runtime a region far wider than the character, so busy " +
                 "desktop nowhere near the silhouette closes the clip. Setting " +
                 "updateWhenOffscreen makes Unity compute the actual skinned bounds each " +
                 "frame.\n\n" +
                 "Cost: the real bounds are computed every frame and the renderer is no " +
                 "longer frustum-culled. For the handful of avatars a transparent " +
                 "overlay shows, that is the right trade; turn it off for a scene with " +
                 "many skinned meshes that are usually off screen.")]
        public bool tightSkinnedBounds = true;

        [Tooltip("Extra dilation to ask the runtime for, in canvas-normalised units " +
                 "(0.05 = 5% of the canvas), ON TOP of the runtime's own default " +
                 "margin. 0 = the runtime default alone, which is the right answer " +
                 "almost always — the runtime already reads the band AROUND the " +
                 "silhouette, not just under it.")]
        [Range(0f, 0.5f)]
        public float extraMarginNormalized = 0f;

        /// <summary>The world-space box reported to the runtime on the last frame. Empty
        /// (size zero) before the first push. Exposed for HUDs and gizmo debugging.</summary>
        public Bounds LastReportedBounds { get; private set; }

        /// <summary>True if the last frame actually pushed a box (something to report,
        /// and the provider session is up).</summary>
        public bool LastReportValid { get; private set; }

        // Only one component may own the channel — native holds a single AABB, so two
        // pushers would alternate and the runtime would see the region flicker between
        // them. First one enabled wins; the rest warn once and stay inert.
        private static DisplayXRContentBounds s_Owner;

        private readonly List<Renderer> m_Cache = new List<Renderer>();
        private float m_NextRescanTime;
        private bool m_WarnedNotOwner;
        // Skinned meshes whose updateWhenOffscreen we turned on, with what it was, so
        // OnDisable puts the scene back exactly as it was found.
        private readonly List<SkinnedMeshRenderer> m_Tightened = new List<SkinnedMeshRenderer>();

        void OnEnable()
        {
            if (s_Owner == null)
            {
                s_Owner = this;
            }
            else if (s_Owner != this && !m_WarnedNotOwner)
            {
                m_WarnedNotOwner = true;
                Debug.LogWarning(
                    "[DisplayXR] More than one DisplayXRContentBounds is enabled. The " +
                    $"runtime tracks a single content region, so '{s_Owner.name}' keeps " +
                    $"it and '{name}' is inert. Put one component on a shared root that " +
                    "encloses all the content instead.", this);
            }
            RefreshRenderers();
        }

        void OnDisable()
        {
            if (s_Owner == this)
            {
                s_Owner = null;
                // Stop chaining: the runtime falls back to the whole canvas within a
                // second on its own, but saying so immediately keeps a torn-down rig
                // from leaving a stale region behind.
                PushDisable();
            }
            LastReportValid = false;
            RestoreSkinnedBounds();
        }

        /// <summary>
        /// Rebuild the cached renderer list now. Call after spawning or swapping content
        /// under this transform if you have set <see cref="rendererRescanInterval"/> to
        /// something long.
        /// </summary>
        public void RefreshRenderers()
        {
            m_NextRescanTime = Time.unscaledTime + rendererRescanInterval;
            if (renderers == null || renderers.Length == 0)
            {
                m_Cache.Clear();
                GetComponentsInChildren(includeInactiveRenderers, m_Cache);
            }
            ApplyTightSkinnedBounds();
        }

        // Make SkinnedMeshRenderer.bounds mean "where this character actually is".
        // Unity's default is the box baked at import to cover the whole animation set —
        // arms-out for a humanoid — and it never changes as the avatar moves, so the
        // reported region stays a T-pose-wide rectangle around a character standing with
        // its arms down. Measured on the panel: 81% of the window width for an avatar
        // occupying far less. updateWhenOffscreen makes Unity skin the real bounds each
        // frame; the previous value is remembered so OnDisable can restore it.
        private void ApplyTightSkinnedBounds()
        {
            if (!tightSkinnedBounds) return;
            var list = (renderers != null && renderers.Length > 0) ? (IList<Renderer>)renderers : m_Cache;
            for (int i = 0; i < list.Count; i++)
            {
                if (!(list[i] is SkinnedMeshRenderer smr)) continue;
                if (smr.updateWhenOffscreen || m_Tightened.Contains(smr)) continue;
                smr.updateWhenOffscreen = true;
                m_Tightened.Add(smr);
            }
        }

        private void RestoreSkinnedBounds()
        {
            for (int i = 0; i < m_Tightened.Count; i++)
                if (m_Tightened[i] != null) m_Tightened[i].updateWhenOffscreen = false;
            m_Tightened.Clear();
        }

        // LateUpdate, not Update: the content has finished moving for the frame, so the
        // box matches what will actually be rendered.
        void LateUpdate()
        {
            LastReportValid = false;
            if (s_Owner != this) return;
            if (!DisplayXRProviderDriver.IsActive) return;

            if (renderers == null || renderers.Length == 0)
            {
                if (rendererRescanInterval <= 0f || Time.unscaledTime >= m_NextRescanTime)
                    RefreshRenderers();
            }

            if (!TryComputeBounds(out Bounds b))
            {
                // Nothing visible this frame (all renderers off, empty hierarchy). Stop
                // reporting rather than freezing on the last box: the runtime then reads
                // the whole canvas, which is the conservative answer.
                PushDisable();
                return;
            }

            LastReportedBounds = b;
            LastReportValid = true;
            Vector3 min = b.min, max = b.max;
            DisplayXRProviderNative.dxr_prov_set_content_bounds(
                min.x, min.y, min.z, max.x, max.y, max.z, extraMarginNormalized, 1);
        }

        private bool TryComputeBounds(out Bounds bounds)
        {
            if (useManualBounds)
            {
                // Local -> world. TransformPoint on the 8 corners rather than
                // Bounds.Transform-style scaling, so a rotated transform still yields a
                // world AABB that encloses the box.
                Vector3 c = manualBounds.center, e = manualBounds.extents;
                bounds = new Bounds(transform.TransformPoint(c), Vector3.zero);
                for (int i = 0; i < 8; i++)
                {
                    bounds.Encapsulate(transform.TransformPoint(c + new Vector3(
                        (i & 1) == 0 ? -e.x : e.x,
                        (i & 2) == 0 ? -e.y : e.y,
                        (i & 4) == 0 ? -e.z : e.z)));
                }
                return true;
            }

            var list = (renderers != null && renderers.Length > 0) ? null : m_Cache;
            bool any = false;
            bounds = default;

            if (list == null)
            {
                for (int i = 0; i < renderers.Length; i++)
                {
                    var r = renderers[i];
                    if (r == null || (!includeInactiveRenderers && !IsDrawing(r))) continue;
                    if (!any) { bounds = r.bounds; any = true; }
                    else bounds.Encapsulate(r.bounds);
                }
            }
            else
            {
                for (int i = 0; i < list.Count; i++)
                {
                    var r = list[i];
                    // A destroyed renderer leaves a null slot until the next rescan.
                    if (r == null || (!includeInactiveRenderers && !IsDrawing(r))) continue;
                    if (!any) { bounds = r.bounds; any = true; }
                    else bounds.Encapsulate(r.bounds);
                }
            }
            return any;
        }

        // A renderer draws only if the component is enabled AND its GameObject is
        // active in the hierarchy — `enabled` alone stays true on a deactivated object,
        // which would widen the reported region around geometry nobody can see.
        private static bool IsDrawing(Renderer r) => r.enabled && r.gameObject.activeInHierarchy;

        private void PushDisable()
        {
            DisplayXRProviderNative.dxr_prov_set_content_bounds(0f, 0f, 0f, 0f, 0f, 0f, 0f, 0);
        }

#if UNITY_EDITOR
        void OnDrawGizmosSelected()
        {
            Gizmos.color = new Color(0.2f, 0.9f, 1f, 0.9f);
            if (Application.isPlaying && LastReportValid)
            {
                Gizmos.DrawWireCube(LastReportedBounds.center, LastReportedBounds.size);
                return;
            }
            if (TryComputeBounds(out Bounds b))
                Gizmos.DrawWireCube(b.center, b.size);
        }
#endif
    }
}
