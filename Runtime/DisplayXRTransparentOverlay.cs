// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Events;
using UnityEngine.Serialization;
#if HAS_INPUT_SYSTEM
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.LowLevel;
#endif

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
    /// Click handling: Unity's standard input system (Mouse.current.position,
    /// OnMouseDown, EventSystem) does NOT work in transparent mode because the
    /// cloaked Unity HWND is not OS-foreground (documented Unity limitation
    /// — see CLAUDE.md "Known Issues"). Use the <see cref="onPointerEnter"/>,
    /// <see cref="onPointerExit"/>, <see cref="onPointerDown"/>,
    /// <see cref="onPointerUp"/>, <see cref="onPointerClick"/> UnityEvents
    /// instead. They're driven by per-pixel Physics.Raycast against the
    /// renderers' colliders, where the ray is built from the polled cursor
    /// (Win32 GetCursorPos) by inverse-projecting through a cyclopean
    /// (midpoint-eye) Kooima view+projection — built by averaging the
    /// runtime's left and right eye matrices. The cyclopean ray lands on
    /// the cube where the user perceives it after stereo fusion of the two
    /// rendered eye images. Camera.ScreenPointToRay can't be used because
    /// Camera.projectionMatrix is the symmetric pre-Kooima projection —
    /// rays from it miss the collider where the cube actually appears on
    /// screen.
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
        [SerializeField, FormerlySerializedAs("chromaKeyColor")]
        private Color m_ChromaKeyColor = new Color(1f, 0f, 1f, 0f);

        /// <summary>
        /// Color rendered in transparent regions. Assignment at runtime is
        /// safe: the setter immediately re-pushes the new color to both the
        /// camera clear and the native overlay (LWA_COLORKEY + win32 binding),
        /// so all three sides — camera clear, layered-window key, runtime
        /// post-weave — stay in sync without toggling the component.
        ///
        /// If you change this from the Inspector at runtime, OnValidate
        /// re-applies. Pre-build / authoring-time changes are picked up by
        /// OnEnable as before.
        /// </summary>
        public Color chromaKeyColor
        {
            get => m_ChromaKeyColor;
            set
            {
                m_ChromaKeyColor = value;
                ApplyChromaKey();
            }
        }

        [Header("Window")]

        [Tooltip("Keep the window above all others (WS_EX_TOPMOST).")]
        public bool alwaysOnTop = true;

        [Header("Hit testing")]

        [Tooltip("Renderers (must have colliders) that drive the pointer events. " +
                 "Per-frame Physics.Raycast — using a Kooima-aware ray inverse-" +
                 "projected from the cursor through both eye matrices — decides " +
                 "which one is under the pointer. Clicks off the silhouettes " +
                 "fall through to the desktop. Empty = whole window blocks.")]
        public Renderer[] clickableRenderers;

        /// <summary>UnityEvent payload — the renderer the pointer is over.</summary>
        [System.Serializable]
        public class PointerEvent : UnityEvent<Renderer> {}

        // Initialized inline so .AddListener() works when the component is
        // added at runtime via AddComponent (Unity only auto-instantiates
        // serialized UnityEvent fields when deserializing scenes/prefabs).
        [Header("Pointer Events (transparent mode replacement for OnMouseDown)")]
        public PointerEvent onPointerEnter = new PointerEvent();
        public PointerEvent onPointerExit  = new PointerEvent();
        public PointerEvent onPointerDown  = new PointerEvent();
        public PointerEvent onPointerUp    = new PointerEvent();
        public PointerEvent onPointerClick = new PointerEvent();

        private Camera m_Camera;
        private DisplayXRFeature m_Feature;
        private CameraClearFlags m_SavedClearFlags;
        private Color m_SavedBackgroundColor;
        private bool m_SavedRestore;

        // Pointer state — only used in transparent build mode.
        private Renderer m_HoverRenderer;
        private Renderer m_PressedRenderer;
        private bool m_LeftWasDown;
        private bool m_RightWasDown;
        private Vector2 m_PrevPointerPos;
        private bool m_HasPrevPointerPos;
        private static int s_diagCounter;

        // Hit-test hysteresis: hold "captured" state for a short window
        // after a hit so per-triangle ray-tri intermittent misses don't
        // drop the user's click.
        private Renderer m_LastHitRenderer;
        private int m_LastHitFrame = int.MinValue;

        // Per-triangle hit-test for SkinnedMeshRenderers in clickableRenderers.
        // Each frame we BakeMesh() the current animated pose, cache the verts/
        // tris arrays, and test the cyclopean ray against each triangle
        // manually (transforming through smr.transform.localToWorldMatrix at
        // test time). Hits only the visible silhouette polygons — transparent
        // gaps inside the AABB (between legs, around hat tip) register as
        // misses and clicks fall through.
        //
        // Why manual instead of MeshCollider: MeshCollider on a SkinnedMesh
        // requires getting BakeMesh's frame-of-reference and the host
        // transform to align exactly, which empirically failed across rigs
        // (cm-source meshes, non-trivial bone scale chains) — collider bounds
        // either overshot by 100x or sat stale under bone animation. Manual
        // ray-triangle is foolproof: the math goes through SMR.localToWorld
        // directly, no transform-pairing guessing.
        //
        // Vendor-agnostic: no knowledge of chroma key or runtime transparency
        // encoding; we just check whether the cyclopean ray passes through
        // any rendered triangle.
        //
        // Cache is static so multi-rig scenes only bake once per SMR per frame.
        private class BakedHit
        {
            public Mesh mesh;
            public Vector3[] verts;
            public int[] tris;
            public int lastBakeFrame;
        }
        private static readonly Dictionary<SkinnedMeshRenderer, BakedHit> s_BakedHits
            = new Dictionary<SkinnedMeshRenderer, BakedHit>();

        /// <summary>Cursor position in window-client pixels (top-left origin).
        /// Only meaningful in standalone Windows build with transparent mode
        /// active. Updated each frame from native polling — works regardless
        /// of Unity input focus, unlike Mouse.current.position.</summary>
        public Vector2 PointerPosition { get; private set; }

        /// <summary>Cursor movement this frame, in window pixels. Same axes as
        /// <see cref="PointerPosition"/>. Use for drag-style interactions.</summary>
        public Vector2 PointerDelta { get; private set; }

        /// <summary>True while the left mouse button is held.</summary>
        public bool IsLeftPressed { get; private set; }

        /// <summary>True while the right mouse button is held.</summary>
        public bool IsRightPressed { get; private set; }

        /// <summary>The renderer currently under the pointer, or null if none.</summary>
        public Renderer HoverRenderer => m_HoverRenderer;

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
            m_Feature = DisplayXRFeature.Instance;

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

            // Belt-and-braces: keep the loop running while Unity is in the
            // background. Pointer events here come from native polling so they
            // don't actually depend on focus, but other components might.
            Application.runInBackground = true;

            uint colorRef = ColorRefFromColor(chromaKeyColor);
            DisplayXRNative.displayxr_set_transparent_overlay(
                1, colorRef, alwaysOnTop ? 1 : 0);
            // Default hit rect = whole window until LateUpdate refines it.
            DisplayXRNative.displayxr_set_overlay_hit_rect(
                0, 0, Screen.width, Screen.height);
#endif
        }

        // Hit-test runs in LateUpdate (not Update) so BakeMesh captures the
        // CURRENT frame's animated pose. Unity's animation step runs between
        // Update and LateUpdate; baking in Update would capture frame N-1's
        // pose while the renderer renders frame N — head triangles in the
        // collider drift behind the visible head as the animation plays.
        // Symptom that prompted the move: "head clicks work initially then
        // stop working as the tiger animates; hips clicks always work"
        // (head bone moves more than hips per frame, so its drift is bigger).
        void LateUpdate()
        {
#if UNITY_STANDALONE_WIN
            if (Application.isEditor || m_Camera == null)
                return;
            if (clickableRenderers == null || clickableRenderers.Length == 0)
                return;
            // Multi-rig gate: only the active rig drives hit_active.
            // With the per-triangle MeshCollider hit-test, even a sub-pixel
            // difference in the rigs' cyclopean rays can put one rig's ray
            // inside the silhouette and the other's just outside, producing
            // hit_active flap (1/0/1/0…) that makes clicks at silhouette
            // edges unpredictable — sometimes captured, sometimes forwarded.
            // The earlier (BoxCollider) version was tolerant enough that
            // both rigs agreed and we could skip the gate; per-triangle is
            // not. Same pattern as DisplayXRDisplay.LateUpdate /
            // DisplayXRCamera.LateUpdate.
            if (DisplayXRRigManager.ActiveCamera != null
                && DisplayXRRigManager.ActiveCamera != m_Camera)
                return;

            // Poll cursor + buttons via native (GetCursorPos + ScreenToClient
            // on the overlay HWND, plus s_vkey_state populated by raw input).
            // Bypasses Unity's New Input System entirely, which is broken for
            // cloaked windows — see component header comment.
            DisplayXRNative.displayxr_get_overlay_pointer(
                out int clientX, out int clientY, out int buttons);
            // Overlay's actual client size — needed for cursor → NDC. Cannot
            // use Screen.width/height because Unity's HWND is parked off-
            // screen with frozen dimensions; the overlay can be scroll-
            // resized independently.
            DisplayXRNative.displayxr_get_overlay_size(
                out int overlayW, out int overlayH);

            // Per-pixel hit test: build a CYCLOPEAN world-space ray from the
            // cursor pixel through the midpoint-eye Kooima projection, then
            // Physics.Raycast against the actual cube collider.
            //
            // Why cyclopean and not per-eye: the left and right eyes project
            // the cube to different screen positions (binocular disparity).
            // A user clicking the visible cube body clicks at its
            // STEREO-FUSED position, which is roughly the midpoint between
            // the two eye images. Per-eye rays land at one eye's projection
            // — offset from the fused position by ~half-disparity in the
            // direction of the parallax. Tried OR-of-both-eye-rays first;
            // it produced a hit region shifted off the cube in whichever
            // direction the parallax dictates. The cyclopean ray (built from
            // averaged view + projection matrices, equivalent to the Kooima
            // projection from the midpoint eye position) lands on the cube
            // where the user sees it.
            //
            // Camera.ScreenPointToRay can't be used because Camera.projectionMatrix
            // is the symmetric pre-Kooima projection — rays from it miss the
            // collider where the user perceives the cube on screen.
            // Update per-triangle MeshColliders for any SkinnedMeshRenderers
            // in clickableRenderers BEFORE the raycast, so hits reflect the
            // current animated silhouette (no AABB false-positives).
            UpdateBakedHitColliders();

            Renderer hitRenderer = null;
            if (clientX >= 0 && clientY >= 0
                && TryGetStereoMatrices(out Matrix4x4 leftView, out Matrix4x4 leftProj,
                                         out Matrix4x4 rightView, out Matrix4x4 rightProj))
            {
                BuildCyclopean(leftView, leftProj, rightView, rightProj,
                                out Matrix4x4 cycView, out Matrix4x4 cycProj);
                if (TryBuildEyeRay(clientX, clientY, overlayW, overlayH, cycView, cycProj, out Ray ray))
                {
                    float bestT = m_Camera.farClipPlane;
                    for (int i = 0; i < clickableRenderers.Length; i++)
                    {
                        var r = clickableRenderers[i];
                        if (r == null) continue;
                        if (r is SkinnedMeshRenderer smrR)
                        {
                            if (TryRayHitBakedSkinnedMesh(ray, bestT, smrR, out float t))
                            {
                                hitRenderer = r;
                                bestT = t;
                            }
                        }
                        else
                        {
                            if (Physics.Raycast(ray, out RaycastHit info, bestT)
                                && info.collider != null
                                && info.collider.transform.IsChildOf(r.transform))
                            {
                                hitRenderer = r;
                                bestT = info.distance;
                            }
                        }
                    }
                }
            }

            // Hysteresis: per-triangle ray-tri can give intermittent results
            // frame-to-frame even for a stationary cursor on dense geometry
            // (animation pose changes shift triangles a sub-pixel each
            // frame, ray either hits or slips between adjacent triangles
            // by luck). Once we've seen a hit, hold the captured state for
            // a short window so users don't lose clicks just because the
            // exact frame they pressed happened to land in a precision
            // miss. Trade-off: clickthrough takes up to STICKY_FRAMES
            // frames to re-engage after the cursor leaves a renderer.
            const int STICKY_FRAMES = 8;
            if (hitRenderer != null)
            {
                m_LastHitRenderer = hitRenderer;
                m_LastHitFrame = Time.frameCount;
            }
            else if (m_LastHitRenderer != null
                     && Time.frameCount - m_LastHitFrame <= STICKY_FRAMES)
            {
                // Recent hit — stay captured to absorb single-frame misses.
                hitRenderer = m_LastHitRenderer;
            }
            else
            {
                m_LastHitRenderer = null;
            }

            // Drive WM_NCHITTEST: only return HTCLIENT (overlay accepts the
            // click) when the cursor is on a transparent-region pixel.
            // Otherwise HTTRANSPARENT — clicks fall through.
            //
            // Mechanism: per-pixel test against the camera depth buffer.
            // Pixels with no geometry rendered have depth at the far plane
            // (0 reverse-Z, 1 forward-Z). Any other depth value means
            // something rendered there — opaque from the user's POV
            // regardless of how the runtime colors transparent pixels
            // (chroma-key, alpha=0, etc.). This is intentionally vendor-
            // agnostic: the plugin should never need to know the runtime's
            // transparency encoding (Leia DP handles it; OpenXR encodes it
            // differently elsewhere).
            //
            // Async readback to avoid stalling the GPU. 1–2 frame latency
            // is imperceptible for hit-testing (16–33 ms at 60 Hz). Falls
            // back to the collider raycast if depth isn't available yet
            // (first couple of frames, or if depthTextureMode wasn't set).
            DisplayXRNative.displayxr_set_overlay_hit_active(hitRenderer != null ? 1 : 0);

            // Expose pointer state for app code that wants to drive its own
            // drag/rotate/whatever (e.g. the test project's DragRotateCube).
            // PointerPosition / PointerDelta replace Mouse.current.position
            // / Mouse.current.delta — both of which are frozen for cloaked
            // HWNDs, the same Unity limitation that motivates this whole
            // polling architecture.
            Vector2 pos = (clientX >= 0 && clientY >= 0)
                ? new Vector2(clientX, clientY)
                : PointerPosition;
            PointerDelta = m_HasPrevPointerPos ? (pos - m_PrevPointerPos) : Vector2.zero;
            PointerPosition = pos;
            m_PrevPointerPos = pos;
            m_HasPrevPointerPos = true;
            IsLeftPressed  = (buttons & 1) != 0;
            IsRightPressed = (buttons & 2) != 0;

#if HAS_INPUT_SYSTEM
            // Inject the polled state into Unity's Mouse.current so any
            // standard input code keeps working in transparent mode:
            // OnMouseDown, IPointerClickHandler / EventSystem, anything
            // reading Input.mousePosition or Mouse.current.leftButton, etc.
            // Without this, only DisplayXRTransparentOverlay's own
            // onPointerXxx events fire — components like
            // DisplayXRInputController that rely on Mouse.current see frozen
            // state because the cloaked HWND can't observe raw input.
            // Unity's Mouse uses bottom-left screen origin; client coords
            // are top-left. Flip Y on both position and delta.
            if (Mouse.current != null && clientX >= 0 && clientY >= 0)
            {
                // Y-flip relative to the overlay's height (not Screen.height,
                // which is the off-screen Unity HWND's frozen size).
                float invY = Mathf.Max(1, overlayH) - clientY;
                ushort btn = 0;
                if ((buttons & 1) != 0) btn |= 1 << (int)MouseButton.Left;
                if ((buttons & 2) != 0) btn |= 1 << (int)MouseButton.Right;
                if ((buttons & 4) != 0) btn |= 1 << (int)MouseButton.Middle;
                var state = new MouseState
                {
                    position = new Vector2(clientX, invY),
                    delta    = new Vector2(PointerDelta.x, -PointerDelta.y),
                    buttons  = btn,
                };
                InputSystem.QueueStateEvent(Mouse.current, state);

                // Diagnostic — once per second, dump what we injected vs
                // what Mouse.current actually reads back. Lets us tell
                // whether QueueStateEvent is reaching the device or being
                // overwritten / dropped.
                if (++s_diagCounter >= 60)
                {
                    s_diagCounter = 0;
                    var mc = Mouse.current;
                    Debug.Log($"[DisplayXR.Inject:{m_Camera.name}] queued pos=({clientX},{invY:F0}) btn=0x{btn:X2} | " +
                              $"Mouse.current pos={mc.position.ReadValue()} " +
                              $"left.isPressed={mc.leftButton.isPressed} " +
                              $"left.wasPressedThisFrame={mc.leftButton.wasPressedThisFrame}");
                }
            }
            else if (++s_diagCounter >= 60)
            {
                s_diagCounter = 0;
                Debug.LogWarning("[DisplayXR.Inject] skipped: " +
                    (Mouse.current == null ? "Mouse.current is null" : $"clientX={clientX} clientY={clientY}"));
            }
#endif

            // Hover transitions
            if (hitRenderer != m_HoverRenderer)
            {
                if (m_HoverRenderer != null) onPointerExit?.Invoke(m_HoverRenderer);
                if (hitRenderer    != null) onPointerEnter?.Invoke(hitRenderer);
                m_HoverRenderer = hitRenderer;
            }

            // Button transitions — left = click semantics, right = drag handled
            // separately by the native overlay wndproc (WM_RBUTTONDOWN starts
            // capture-based window drag — issue #57 task 2). We still surface
            // right-button events for apps that want them.
            DispatchButton(buttons, 1, ref m_LeftWasDown, hitRenderer);
            DispatchButton(buttons, 2, ref m_RightWasDown, hitRenderer);

            // Coarse AABB hit rect — used by WM_NCHITTEST in the OPAQUE
            // WS_CHILD path as a fast reject before the per-pixel hit_active
            // check kicks in. (In transparent-overlay top-level mode the
            // native side returns HTCLIENT unconditionally, so this rect
            // is informational; we set it anyway for opaque-mode users.)
            if (TryGetStereoMatrices(out Matrix4x4 lv2, out Matrix4x4 lp2,
                                     out Matrix4x4 rv2, out Matrix4x4 rp2))
            {
                BuildCyclopean(lv2, lp2, rv2, rp2,
                               out Matrix4x4 cycView2, out Matrix4x4 cycProj2);
                if (TryGetUnionScreenRect(overlayW, overlayH, cycView2, cycProj2,
                                          out int rx, out int ry, out int rw, out int rh))
                {
                    DisplayXRNative.displayxr_set_overlay_hit_rect(rx, ry, rw, rh);
                }
            }
#endif
        }

        private void DispatchButton(int buttons, int mask, ref bool wasDown, Renderer hit)
        {
            bool isDown = (buttons & mask) != 0;
            if (isDown && !wasDown)
            {
                if (hit != null)
                {
                    if (mask == 1) m_PressedRenderer = hit;
                    onPointerDown?.Invoke(hit);
                }
            }
            else if (!isDown && wasDown)
            {
                if (mask == 1)
                {
                    if (m_PressedRenderer != null)
                    {
                        onPointerUp?.Invoke(m_PressedRenderer);
                        // Click only if release on same target as press.
                        if (hit == m_PressedRenderer)
                            onPointerClick?.Invoke(m_PressedRenderer);
                        m_PressedRenderer = null;
                    }
                }
                else if (hit != null)
                {
                    onPointerUp?.Invoke(hit);
                }
            }
            wasDown = isDown;
        }


        void OnDisable()
        {
#if UNITY_STANDALONE_WIN
            if (!Application.isEditor)
            {
                DisplayXRNative.displayxr_set_transparent_overlay(0, 0, 0);
                DisplayXRNative.displayxr_set_overlay_hit_active(0);
            }
#endif
            // Fire trailing exit so handlers can clean up.
            if (m_HoverRenderer != null)
            {
                onPointerExit?.Invoke(m_HoverRenderer);
                m_HoverRenderer = null;
            }
            m_PressedRenderer = null;
            m_LeftWasDown = false;
            m_RightWasDown = false;

            if (m_SavedRestore && m_Camera != null)
            {
                m_Camera.clearFlags      = m_SavedClearFlags;
                m_Camera.backgroundColor = m_SavedBackgroundColor;
                m_SavedRestore = false;
            }
        }

        /// <summary>
        /// Read + consume the overlay's accumulated mouse-wheel delta since
        /// the last call (Win32 raw units, 120 per notch; positive = wheel
        /// forward). Returns 0 in the editor or when no transparent overlay
        /// is active.
        ///
        /// The plugin no longer resizes the overlay window on wheel events
        /// (that was experimental in v1.2.0/v1.2.1 and removed in v1.2.2).
        /// Apps poll this value each frame and decide what to do — common
        /// patterns: drive a `DisplayXRDisplay` rig's `virtualDisplayHeight`
        /// to zoom in-window, scroll a 2D UI canvas, or rotate the avatar.
        /// </summary>
        public int ConsumeWheelDelta()
        {
#if UNITY_STANDALONE_WIN
            if (Application.isEditor)
                return 0;
            return DisplayXRNative.displayxr_consume_overlay_wheel_delta();
#else
            return 0;
#endif
        }

        // Re-apply the current chroma-key color to the camera clear and the
        // native overlay. Idempotent — safe to call any time. Early-outs
        // before OnEnable has run (m_SavedRestore is false): OnEnable will
        // pick up m_ChromaKeyColor itself and the explicit re-push isn't
        // needed.
        private void ApplyChromaKey()
        {
            if (!m_SavedRestore || m_Camera == null)
                return;

            m_Camera.backgroundColor = m_ChromaKeyColor;

#if UNITY_STANDALONE_WIN
            // Editor preview path skips the layered-window plumbing entirely
            // (see OnEnable); same gate here.
            if (Application.isEditor)
                return;
            if (!enabled || !gameObject.activeInHierarchy)
                return;
            DisplayXRNative.displayxr_set_transparent_overlay(
                1, ColorRefFromColor(m_ChromaKeyColor),
                alwaysOnTop ? 1 : 0);
#endif
        }

        // Inspector-driven changes go straight to the SerializeField backing
        // and bypass the property setter, so re-apply manually during Play.
        // Edit-mode changes fall through to the OnEnable path on next Play.
        void OnValidate()
        {
            if (Application.isPlaying)
                ApplyChromaKey();
        }

        // Pack a Unity Color into a Windows COLORREF (0x00BBGGRR).
        private static uint ColorRefFromColor(Color c)
        {
            uint r = (uint)Mathf.Clamp(Mathf.RoundToInt(c.r * 255f), 0, 255);
            uint g = (uint)Mathf.Clamp(Mathf.RoundToInt(c.g * 255f), 0, 255);
            uint b = (uint)Mathf.Clamp(Mathf.RoundToInt(c.b * 255f), 0, 255);
            return (b << 16) | (g << 8) | r;
        }

        // Manual ray-triangle test against the baked skinned mesh.
        // Walks all triangles transforming each through the SMR's current
        // localToWorldMatrix at test time. Returns the nearest hit distance
        // along the ray, or false if no triangle is hit within maxDist.
        // Möller–Trumbore intersection — branchless, no allocs.
        private static bool TryRayHitBakedSkinnedMesh(
            Ray ray, float maxDist, SkinnedMeshRenderer smr, out float hitT)
        {
            hitT = 0f;
            if (!s_BakedHits.TryGetValue(smr, out BakedHit entry)) return false;
            if (entry.verts == null || entry.tris == null) return false;
            var verts = entry.verts;
            var tris = entry.tris;
            // BakeMesh outputs verts already in world *units* (post-skinning,
            // with the rig's scale chain pre-applied) but in the SMR's local
            // rotated frame. So the right "bake-local → world" transform is
            // SMR.transform's position + rotation only, NO scale — applying
            // any of the lossyScales (SMR=180×, rootBone=1.8× on Mixamo)
            // double-scales and puts triangles way off the rendered tiger.
            // Verified empirically: row D in the [DisplayXR.BakedHit] diag
            // (SMR.pos+rot, no scale) reproduces renderer.bounds exactly,
            // while rootBone.lToW and SMR.lToW don't.
            var l2w = Matrix4x4.TRS(smr.transform.position,
                                    smr.transform.rotation,
                                    Vector3.one);
            Vector3 ro = ray.origin, rd = ray.direction;
            float closest = maxDist;
            bool any = false;
            for (int i = 0; i + 2 < tris.Length; i += 3)
            {
                Vector3 v0 = l2w.MultiplyPoint3x4(verts[tris[i]]);
                Vector3 v1 = l2w.MultiplyPoint3x4(verts[tris[i + 1]]);
                Vector3 v2 = l2w.MultiplyPoint3x4(verts[tris[i + 2]]);
                Vector3 e1 = v1 - v0, e2 = v2 - v0;
                Vector3 h = Vector3.Cross(rd, e2);
                float a = Vector3.Dot(e1, h);
                if (a > -1e-7f && a < 1e-7f) continue;
                float f = 1f / a;
                Vector3 s = ro - v0;
                float u = f * Vector3.Dot(s, h);
                if (u < 0f || u > 1f) continue;
                Vector3 q = Vector3.Cross(s, e1);
                float v = f * Vector3.Dot(rd, q);
                if (v < 0f || u + v > 1f) continue;
                float t = f * Vector3.Dot(e2, q);
                if (t > 1e-4f && t < closest)
                {
                    closest = t;
                    any = true;
                }
            }
            hitT = closest;
            return any;
        }

        // For each SkinnedMeshRenderer in clickableRenderers, BakeMesh the
        // current animated pose and cache the verts/tris arrays. The actual
        // hit test (TryRayHitBakedSkinnedMesh) walks the triangles and
        // transforms each through the SMR's localToWorldMatrix at test time,
        // so the visible silhouette is what's hit-tested — no MeshCollider
        // needed and no transform-pairing to get wrong. Transparent gaps
        // inside the AABB (between legs, around hat tip, AABB corners)
        // register as misses, which the native overlay routes via
        // WM_NCHITTEST = HTTRANSPARENT.
        //
        // BakeMesh costs ~1–3 ms per frame for a typical character mesh;
        // the verts[] copy is one extra alloc per frame (Mesh.vertices
        // returns a fresh array). Acceptable for a small clickable cast.
        //
        // No-op for non-skinned renderers (regular MeshRenderer): they keep
        // whatever collider the user attached and use Physics.Raycast.
        private void UpdateBakedHitColliders()
        {
            if (clickableRenderers == null) return;
            int frame = Time.frameCount;
            for (int i = 0; i < clickableRenderers.Length; i++)
            {
                var smr = clickableRenderers[i] as SkinnedMeshRenderer;
                if (smr == null) continue;

                if (!s_BakedHits.TryGetValue(smr, out BakedHit entry) || entry == null)
                {
                    var mesh = new Mesh { name = "DisplayXR_BakedHit_" + smr.name };
                    mesh.MarkDynamic();
                    entry = new BakedHit { mesh = mesh, lastBakeFrame = -1 };
                    s_BakedHits[smr] = entry;

                    // Force the SMR to always re-skin its mesh, regardless
                    // of visibility / focus. Default (false) makes Unity
                    // cache the skinned mesh state when it thinks the SMR
                    // is offscreen — which can include "app lost focus"
                    // — so the *bones* keep advancing via Animator while
                    // the *rendered* mesh stays stale. BakeMesh reads
                    // bone state directly, so we'd test against the
                    // current pose while the user clicks the stale
                    // rendered pose: head misses, hips (which barely
                    // moves) still hits.
                    if (!smr.updateWhenOffscreen) smr.updateWhenOffscreen = true;

                    // And force the Animator to always update bones.
                    // Some Animator culling modes (CullUpdateTransforms
                    // / CullCompletely) skip transform updates when the
                    // attached SMR is considered offscreen — same kind
                    // of stale-pose hazard.
                    var anim = smr.GetComponentInParent<Animator>();
                    if (anim != null) anim.cullingMode = AnimatorCullingMode.AlwaysAnimate;
                }

                // Multi-rig dedupe: only bake once per SMR per frame.
                if (entry.lastBakeFrame == frame) continue;

                smr.BakeMesh(entry.mesh);
                // Cache mesh data once when the topology stabilises; verts
                // change every frame so we always re-fetch them.
                entry.verts = entry.mesh.vertices;
                if (entry.tris == null || entry.tris.Length != entry.mesh.triangles.Length)
                    entry.tris = entry.mesh.triangles;
                entry.lastBakeFrame = frame;

            }
        }

        // Lazy-fetch the feature singleton — DisplayXRFeature.Instance may be
        // null at OnEnable time if our OnEnable runs before the OpenXR loader
        // initialises the feature.
        private bool TryGetStereoMatrices(out Matrix4x4 leftView, out Matrix4x4 leftProj,
                                           out Matrix4x4 rightView, out Matrix4x4 rightProj)
        {
            leftView = leftProj = rightView = rightProj = Matrix4x4.identity;
            if (m_Feature == null)
                m_Feature = DisplayXRFeature.Instance;
            if (m_Feature == null)
                return false;
            return m_Feature.GetStereoMatrices(out leftView, out leftProj,
                                                out rightView, out rightProj);
        }

        // Unproject a window-pixel cursor through one eye's view + projection
        // into a Unity-world-space Ray. Inverse path of ProjectThroughEye:
        // window px → NDC → inverse projection (eye-space, OpenXR convention)
        // → inverse view (Unity world). Origin = ray's near-plane point;
        // direction = far-near. Returns false if the projection is degenerate.
        private static bool TryBuildEyeRay(int clientX, int clientY,
                                            int overlayW, int overlayH,
                                            Matrix4x4 viewOpenXR, Matrix4x4 projOpenXR,
                                            out Ray ray)
        {
            ray = new Ray(Vector3.zero, Vector3.forward);
            // Use the overlay's actual client size (not Screen.*, which
            // reflects Unity's frozen off-screen HWND). The overlay rect
            // can change at runtime — without this, cursor → NDC drifts
            // and the ray misses the cube collider.
            float sw = Mathf.Max(1, overlayW);
            float sh = Mathf.Max(1, overlayH);
            // Window pixel (top-left origin) → NDC ([-1,1] with +Y up)
            float ndcX = 2f * clientX / sw - 1f;
            float ndcY = 1f - 2f * clientY / sh;

            Matrix4x4 projInv = projOpenXR.inverse;
            Vector4 nearClip = new Vector4(ndcX, ndcY, -1f, 1f);
            Vector4 farClip  = new Vector4(ndcX, ndcY,  1f, 1f);
            Vector4 nearEye = projInv * nearClip;
            Vector4 farEye  = projInv * farClip;
            if (Mathf.Abs(nearEye.w) < 1e-6f || Mathf.Abs(farEye.w) < 1e-6f)
                return false;
            Vector3 nearEye3 = new Vector3(nearEye.x / nearEye.w,
                                            nearEye.y / nearEye.w,
                                            nearEye.z / nearEye.w);
            Vector3 farEye3 = new Vector3(farEye.x / farEye.w,
                                           farEye.y / farEye.w,
                                           farEye.z / farEye.w);

            // viewUnity = FlipViewZ(viewOpenXR) maps Unity-world → OpenXR-eye-space.
            // Its inverse maps OpenXR-eye-space back to Unity-world.
            Matrix4x4 viewUnity = viewOpenXR;
            viewUnity.m02 = -viewUnity.m02;
            viewUnity.m12 = -viewUnity.m12;
            viewUnity.m22 = -viewUnity.m22;
            viewUnity.m32 = -viewUnity.m32;
            Matrix4x4 viewInv = viewUnity.inverse;
            Vector3 nearWorld = viewInv.MultiplyPoint(nearEye3);
            Vector3 farWorld  = viewInv.MultiplyPoint(farEye3);
            Vector3 dir = (farWorld - nearWorld);
            float len = dir.magnitude;
            if (len < 1e-4f)
                return false;
            ray = new Ray(nearWorld, dir / len);
            return true;
        }

        // Project a single world-space point through one eye's view + projection
        // into top-left-origin window pixels. Returns false if the point is
        // behind the eye or outside the depth clip range.
        //
        // The view matrix from the runtime is OpenXR convention (right-hand,
        // -Z forward); Unity world is +Z forward. FlipViewZ negates the
        // matrix's Z column, which is equivalent to negating the world Z
        // input — so the result equals applying the unflipped matrix to
        // (x, y, -z). We do that here so callers pass plain Unity-world
        // positions.
        private static bool ProjectThroughEye(Vector3 worldUnity,
                                               Matrix4x4 viewOpenXR, Matrix4x4 projOpenXR,
                                               float screenW, float screenH,
                                               out Vector2 windowPx)
        {
            windowPx = Vector2.zero;
            Matrix4x4 viewUnity = viewOpenXR;
            viewUnity.m02 = -viewUnity.m02;
            viewUnity.m12 = -viewUnity.m12;
            viewUnity.m22 = -viewUnity.m22;
            viewUnity.m32 = -viewUnity.m32;
            Vector4 worldH = new Vector4(worldUnity.x, worldUnity.y, worldUnity.z, 1f);
            Vector4 eye = viewUnity * worldH;
            Vector4 clip = projOpenXR * eye;
            if (clip.w <= 0.0001f) return false;
            float ndcX = clip.x / clip.w;
            float ndcY = clip.y / clip.w;
            float ndcZ = clip.z / clip.w;
            if (ndcZ < -1f || ndcZ > 1f) return false;
            // NDC ([-1,1] with +Y up) → window pixels (top-left origin, +Y down).
            windowPx.x = (ndcX + 1f) * 0.5f * screenW;
            windowPx.y = (1f - ndcY) * 0.5f * screenH;
            return true;
        }

        // Build cyclopean (midpoint-eye) view + projection from the left and
        // right eye matrices. The two eyes' view matrices share the same
        // rotation (both face the display) but have different translations
        // (one per eye position). Averaging the translation column gives the
        // midpoint eye's view matrix. The two eyes' Kooima projection
        // matrices share the same depth/aspect terms but have opposite
        // horizontal skew (left-eye frustum biased one way, right-eye the
        // other); element-wise averaging produces a symmetric frustum
        // identical to the Kooima projection that the midpoint eye would
        // build for itself.
        private static void BuildCyclopean(Matrix4x4 leftView, Matrix4x4 leftProj,
                                            Matrix4x4 rightView, Matrix4x4 rightProj,
                                            out Matrix4x4 cyclopeanView,
                                            out Matrix4x4 cyclopeanProj)
        {
            cyclopeanView = leftView;
            cyclopeanView.m03 = (leftView.m03 + rightView.m03) * 0.5f;
            cyclopeanView.m13 = (leftView.m13 + rightView.m13) * 0.5f;
            cyclopeanView.m23 = (leftView.m23 + rightView.m23) * 0.5f;

            cyclopeanProj = new Matrix4x4();
            for (int row = 0; row < 4; row++)
                for (int col = 0; col < 4; col++)
                    cyclopeanProj[row, col] =
                        (leftProj[row, col] + rightProj[row, col]) * 0.5f;
        }

        // Project a renderer's world-space AABB to a window-pixel rect via
        // cyclopean Kooima projection. 8 corners → take min/max.
        private bool TryProjectBoundsToScreen(Bounds b,
                                               int overlayW, int overlayH,
                                               Matrix4x4 cyclopeanView,
                                               Matrix4x4 cyclopeanProj,
                                               out int x, out int y, out int w, out int h)
        {
            x = y = w = h = 0;
            // See TryBuildEyeRay: overlay's actual client size, not Screen.*.
            float sw = Mathf.Max(1, overlayW);
            float sh = Mathf.Max(1, overlayH);
            Vector3 c = b.center;
            Vector3 e = b.extents;
            float minX = float.PositiveInfinity, minY = float.PositiveInfinity;
            float maxX = float.NegativeInfinity, maxY = float.NegativeInfinity;
            bool any = false;
            for (int k = 0; k < 8; k++)
            {
                Vector3 corner = c + new Vector3(
                    ((k & 1) == 0 ? -e.x : e.x),
                    ((k & 2) == 0 ? -e.y : e.y),
                    ((k & 4) == 0 ? -e.z : e.z));
                if (!ProjectThroughEye(corner, cyclopeanView, cyclopeanProj, sw, sh, out Vector2 px))
                    continue;
                if (px.x < minX) minX = px.x;
                if (px.y < minY) minY = px.y;
                if (px.x > maxX) maxX = px.x;
                if (px.y > maxY) maxY = px.y;
                any = true;
            }
            if (!any) return false;
            int xMin = Mathf.Clamp(Mathf.FloorToInt(minX), 0, (int)sw);
            int xMax = Mathf.Clamp(Mathf.CeilToInt(maxX),  0, (int)sw);
            int yMin = Mathf.Clamp(Mathf.FloorToInt(minY), 0, (int)sh);
            int yMax = Mathf.Clamp(Mathf.CeilToInt(maxY),  0, (int)sh);
            x = xMin; w = xMax - xMin;
            y = yMin; h = yMax - yMin;
            return w > 0 && h > 0;
        }

        // Union-rect across all clickableRenderers' projected AABBs — used by
        // LateUpdate to push a single coarse hit_rect to the native overlay.
        private bool TryGetUnionScreenRect(int overlayW, int overlayH,
                                            Matrix4x4 cyclopeanView,
                                            Matrix4x4 cyclopeanProj,
                                            out int x, out int y, out int w, out int h)
        {
            x = y = w = h = 0;
            int unionXMin = int.MaxValue, unionYMin = int.MaxValue;
            int unionXMax = int.MinValue, unionYMax = int.MinValue;
            bool any = false;
            for (int i = 0; i < clickableRenderers.Length; i++)
            {
                var r = clickableRenderers[i];
                if (r == null || !r.enabled || !r.gameObject.activeInHierarchy)
                    continue;
                if (!TryProjectBoundsToScreen(r.bounds, overlayW, overlayH,
                                               cyclopeanView, cyclopeanProj,
                                               out int rx, out int ry, out int rw, out int rh))
                    continue;
                if (rx < unionXMin) unionXMin = rx;
                if (ry < unionYMin) unionYMin = ry;
                if (rx + rw > unionXMax) unionXMax = rx + rw;
                if (ry + rh > unionYMax) unionYMax = ry + rh;
                any = true;
            }
            if (!any) return false;
            x = unionXMin; y = unionYMin;
            w = unionXMax - unionXMin;
            h = unionYMax - unionYMin;
            return w > 0 && h > 0;
        }
    }
}
