// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Events;
using UnityEngine.Rendering;
#if HAS_INPUT_SYSTEM
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.LowLevel;
#endif

namespace DisplayXR
{
    /// <summary>
    /// (issue #57) Opt-in transparent overlay mode for Windows and macOS
    /// standalone builds. Drives the avatar/desktop-overlay use case.
    ///
    /// Mechanism: the OpenXR session is opted into
    /// <c>XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND</c> so Unity emits per-pixel
    /// alpha to the swapchain. The DisplayXR runtime composes the desktop
    /// under each tile pre-weave and alpha-gates post-weave (D3D11/D3D12/
    /// Vulkan on Windows; alpha-native Metal on macOS), so anti-aliased
    /// silhouettes get true soft alpha. On Windows the overlay HWND is
    /// top-level + WS_EX_NOREDIRECTIONBITMAP so DWM composites it purely
    /// from the runtime's DComp visuals; on macOS the runtime's CAMetalLayer
    /// + Unity's NSWindow opacity flip do the same job.
    ///
    /// Click handling: Unity's standard input system (Mouse.current.position,
    /// OnMouseDown, EventSystem) does NOT work in Windows transparent mode
    /// because the cloaked Unity HWND is not OS-foreground (documented Unity
    /// limitation — see CLAUDE.md "Known Issues"). Use the
    /// <see cref="onPointerEnter"/>, <see cref="onPointerExit"/>,
    /// <see cref="onPointerDown"/>, <see cref="onPointerUp"/>,
    /// <see cref="onPointerClick"/> UnityEvents instead. They're driven by
    /// per-pixel Physics.Raycast against the renderers' colliders, where the
    /// ray is built from the polled cursor (Win32 GetCursorPos) by inverse-
    /// projecting through a cyclopean (midpoint-eye) Kooima view+projection.
    /// </summary>
    [AddComponentMenu("DisplayXR/Transparent Overlay")]
    [RequireComponent(typeof(Camera))]
    public class DisplayXRTransparentOverlay : MonoBehaviour
    {
        [Header("Window")]

        [Tooltip("Keep the window above all others (WS_EX_TOPMOST).")]
        public bool alwaysOnTop = true;

        [Tooltip("DORMANT / not viable for transparent apps. Binding the runtime " +
                 "to Unity's REAL main HWND can't give clean transparency — " +
                 "Unity's own opaque window swapchain composites behind the " +
                 "runtime's output (WS_EX_NOREDIRECTIONBITMAP can't be retrofitted " +
                 "onto Unity's window). The transparent path uses the off-screen " +
                 "overlay; the avatar windowing (B-toggle decoration, region " +
                 "click-through, right-drag move) is applied to THAT overlay. " +
                 "Leave off.")]
        public bool useSimpleWindow = false;

        // NOTE: Window-chrome UI policy (which key toggles decoration, resizes,
        // or quits) is intentionally NOT in this component. Those are the app's
        // choice. The plugin exposes only the primitives — displayxr_toggle_
        // window_decoration / set_window_decorated, displayxr_resize_overlay,
        // displayxr_get_overlay_size, displayxr_get_overlay_pointer, and
        // displayxr_consume_overlay_close_request — and the app binds whatever
        // UI it wants on top (see the demo's window controller for an example).

        // Runtime mirror of useSimpleWindow, resolved in OnEnable (true only on
        // a Windows build, non-editor, with useSimpleWindow set). LateUpdate and
        // OnDisable branch on this so the editor / non-Win paths stay inert.
        private bool m_SimpleWindow;

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

        // Per-pixel silhouette mask (Approach B+, #57). Each frame we
        // render the clickable renderers to a small R8 RenderTexture
        // with Hidden/DisplayXR/Silhouette (red=1 where any geometry
        // rasterizes), AsyncGPUReadback the result, and hand the bytes
        // to native — which RLEs to RECTs, ExtCreateRegion's, and
        // SetWindowRgn's. Outside the silhouette the OS routes input
        // natively to whatever desktop window is at the cursor
        // (including concavities like the gap between the tiger's
        // legs that an AABB swallows). Cheap: a 256x144 R8 readback
        // is ~36 KB per frame.
        //
        // Resolution kept low intentionally — the region only needs to
        // be silhouette-accurate, not feature-accurate. Higher values
        // increase readback cost and region complexity (more RECTs).
        private const int  HIT_MASK_WIDTH  = 256;
        private const int  HIT_MASK_HEIGHT = 144;
        private RenderTexture m_HitMaskRT;
        private RenderTexture m_HitMaskDilatedRT;
        private Material      m_SilhouetteMat;
        private Material      m_SilhouetteDilateMat;
        private CommandBuffer m_HitMaskCB;
        // Only one readback in flight at a time — Unity's
        // AsyncGPUReadback budget is a few outstanding requests, but
        // a single one keeps memory + GPU pressure deterministic.
        private bool m_HitMaskReadbackPending;
        // Captured at request time so the callback (which runs on a
        // later main-thread tick) uses the overlay size that was
        // current when we issued the readback, not whatever the
        // overlay has resized to since.
        private int  m_HitMaskPendingDstW;
        private int  m_HitMaskPendingDstH;

        // Debug: dump the readback hit-mask/silhouette to %TEMP%\displayxr_hitmask.png
        // for inspection when DXR_DUMP_HIT_MASK=1. Throttled to ~every 15 readbacks.
        private static readonly bool s_DumpHitMask =
            System.Environment.GetEnvironmentVariable("DXR_DUMP_HIT_MASK") == "1";
        private int m_HitMaskDumpCounter;

        // Per-zone matrix scratch buffers for the multi-zone silhouette union (#166).
        private readonly float[] m_ZoneLV = new float[16], m_ZoneLP = new float[16];
        private readonly float[] m_ZoneRV = new float[16], m_ZoneRP = new float[16];

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
#if UNITY_STANDALONE_WIN || UNITY_STANDALONE_OSX
            // Set the shared-state flag the native win32/macOS overlay reads
            // (get_app_main_view → transparent_mode) so it builds the transparent
            // overlay and keeps it on-screen while Unity's real HWND is cloaked/
            // off-screen. Without this the overlay builds opaque/off-screen and the
            // woven content isn't visible.
            DisplayXRNative.displayxr_set_transparent_background(1);
            // Provider mode (#166): the custom Display Provider drives its own
            // session. Request a transparent background on the provider so it sets
            // ALPHA_BLEND + transparentBackgroundEnabled on ITS session (only takes
            // effect if the runtime advertises ALPHA_BLEND). This latches
            // DisplayXRProvider.s_TransparentBackgroundRequested (read by the splash
            // bootstrap) AND flips the provider's transparent flag.
            DisplayXRProvider.RequestTransparentBackground(true);
#endif
        }

        /// <summary>
        /// (#131) Ask for the transparent overlay to be BORN covering its
        /// monitor (instead of inheriting Unity's window size and being resized
        /// later). MUST be called BEFORE the OpenXR session is created — from
        /// the SAME [RuntimeInitializeOnLoadMethod(SubsystemRegistration)]
        /// bootstrap as RequestTransparentSession. The overlay is then created
        /// full-size, so a fullscreen 2D-surround app needs no post-creation
        /// resize (which recreates the swapchain = a startup flash/freeze) and
        /// Unity itself can stay WINDOWED (avoiding fullscreen independent-flip,
        /// which bypasses DWM alpha compositing). Called from OnEnable it is too
        /// late — the overlay already exists, and the app falls back to the
        /// resize. Windows only.
        /// </summary>
        public static void RequestFullscreenOverlay()
        {
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
            DisplayXRNative.displayxr_set_fullscreen_overlay_pref(1);
#endif
        }

        void OnEnable()
        {
            m_Camera = GetComponent<Camera>();
            if (m_Camera == null)
                return;

            // Solid-color clear to fully transparent (alpha=0). The provider
            // session is in ALPHA_BLEND mode when transparency was requested,
            // so this alpha=0 reaches the swapchain unmodified; the runtime
            // composes the desktop under each tile before weaving and alpha-
            // gates post-weave so anti-aliased silhouettes blend cleanly.
            m_SavedClearFlags      = m_Camera.clearFlags;
            m_SavedBackgroundColor = m_Camera.backgroundColor;
            m_SavedRestore         = true;
            m_Camera.clearFlags      = CameraClearFlags.SolidColor;
            m_Camera.backgroundColor = new Color(0f, 0f, 0f, 0f);

#if UNITY_STANDALONE_WIN
            // Native restyle (overlay cloak/off-screen, or simple-window style
            // strip) is build-only: no top-level Unity HWND we want to mutate
            // from the editor.
            if (Application.isEditor)
                return;

            // Belt-and-braces: keep the loop running while Unity is in the
            // background. Pointer events here come from native polling so they
            // don't actually depend on focus, but other components might.
            Application.runInBackground = true;

            // Provider mode (#166): the transparent overlay machinery is SHARED with
            // the hook path — the overlay is already the unowned NOREDIRECTIONBITMAP
            // transparent window (transparent_background_requested drives
            // displayxr_get_app_main_view), and the runtime is bound to it by the
            // provider. We STILL need the Unity cloak + off-screen move below: without
            // it, Unity's suppressed (black) window shows behind the transparent
            // overlay (the "solid black background" symptom). The provider skips
            // provider-opaque overlay mode when transparent so parent_subclass_proc
            // doesn't drag the overlay off-screen following cloaked Unity.

            m_SimpleWindow = useSimpleWindow;
            if (m_SimpleWindow)
            {
                // Avatar-style: style Unity's REAL HWND borderless. Dormant / not
                // viable under the provider (see the useSimpleWindow tooltip) — the
                // provider binds its own window; this no-ops harmlessly if the real
                // HWND isn't the bound one.
                DisplayXRNative.displayxr_set_simple_window(1, alwaysOnTop ? 1 : 0);
            }
            else
            {
                DisplayXRNative.displayxr_set_transparent_overlay(
                    1, alwaysOnTop ? 1 : 0);
            }
            // Default hit rect = whole window until LateUpdate refines it.
            DisplayXRNative.displayxr_set_overlay_hit_rect(
                0, 0, Screen.width, Screen.height);
#elif UNITY_STANDALONE_OSX
            // (#85 Phase 1) Flip Unity's NSWindow opaque flag so the desktop
            // shows through alpha=0 regions. Runtime handles its own
            // CAMetalLayer + NSWindow via XR_EXT_cocoa_window_binding v5; the
            // Unity-owned NSWindow is the app's responsibility. Skip in the
            // editor unless we're in Play Mode — at edit time the game-view
            // hasn't been built and we'd mutate the wrong window.
            if (!Application.isEditor || Application.isPlaying)
            {
                Application.runInBackground = true;
                DisplayXRNative.displayxr_macos_configure_unity_nswindow(1);
            }
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

            int clientX, clientY, buttons, overlayW, overlayH;
            if (m_SimpleWindow)
            {
                // Simple-window mode: Unity IS the real on-screen foreground
                // window, so Mouse.current and Screen.* are live — no cloaked-
                // HWND polling needed. Read the cursor in top-left client px.
                GetRealWindowPointer(out clientX, out clientY, out buttons);
                overlayW = Screen.width;
                overlayH = Screen.height;
            }
            else
            {
                // Overlay mode: poll cursor + buttons via native (GetCursorPos
                // + ScreenToClient on the overlay HWND, plus s_vkey_state from
                // raw input). Bypasses Unity's New Input System, which is broken
                // for the cloaked off-screen Unity HWND — see header comment.
                DisplayXRNative.displayxr_get_overlay_pointer(
                    out clientX, out clientY, out buttons);
                // Overlay's actual client size — needed for cursor → NDC. Cannot
                // use Screen.width/height because Unity's HWND is parked off-
                // screen with frozen dimensions; the overlay can be scroll-
                // resized independently.
                DisplayXRNative.displayxr_get_overlay_size(
                    out overlayW, out overlayH);
            }

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
                GetStereoViewport(overlayW, overlayH,
                                  out int vpX, out int vpY, out int vpW, out int vpH);
                if (TryBuildEyeRay(clientX, clientY, vpX, vpY, vpW, vpH, cycView, cycProj, out Ray ray))
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
            // standard input code keeps working in (overlay) transparent mode:
            // OnMouseDown, IPointerClickHandler / EventSystem, anything
            // reading Input.mousePosition or Mouse.current.leftButton, etc.
            // Without this, only DisplayXRTransparentOverlay's own
            // onPointerXxx events fire — components like
            // DisplayXRInputController that rely on Mouse.current see frozen
            // state because the cloaked HWND can't observe raw input.
            // Unity's Mouse uses bottom-left screen origin; client coords
            // are top-left. Flip Y on both position and delta.
            //
            // SKIPPED in simple-window mode: Unity's real HWND is the on-screen
            // foreground window, so Mouse.current is already live — re-injecting
            // our polled copy would fight Unity's own raw input.
            if (!m_SimpleWindow && Mouse.current != null && clientX >= 0 && clientY >= 0)
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

            // Cube/avatar screen-space AABB. Drives the OS hit-test region
            // on the overlay HWND via displayxr_set_overlay_hit_rect:
            //  - Transparent mode (Approach B): native side calls
            //    SetWindowRgn(overlay, rect, TRUE). Outside the rect the
            //    OS treats our window as if it didn't exist, so input is
            //    routed natively to whatever desktop window is underneath
            //    (full cross-process fidelity: real DefWindowProc modal
            //    loops with proper GetKeyState, native cursor adaptation
            //    over resize edges, native menu activation, native hover
            //    highlights, taskbar previews, tooltips). Inside the rect
            //    the overlay catches the click and posts it to Unity,
            //    which runs its own per-pixel raycast (s_hit_active /
            //    onPointerClick) to decide whether to act.
            //  - Opaque WS_CHILD mode: WM_NCHITTEST uses the rect as a
            //    fast HTCLIENT-vs-HTTRANSPARENT discriminator.
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
                // Approach B+: render the silhouette PER EYE (union of
                // left+right) to a small RT, dilate, then
                // AsyncGPUReadback. Per-eye union covers what the
                // lenticular actually displays column-by-column; the
                // cyclopean midpoint by itself is narrower than the
                // visible silhouette on high-disparity foreground
                // geometry (e.g. hands), causing clipping.
                RenderHitMaskAndRequestReadback(
                    lv2, lp2, rv2, rp2,
                    overlayW, overlayH);
            }
#elif UNITY_STANDALONE_OSX
            // (#85) Mac cursor/button polling + cyclopean hit-test mirroring
            // the Win32 path. Unity uses a regular NSWindow here (no cloaked
            // HWND quirks), so Mouse.current is the right source. Drives
            // PointerPosition / PointerDelta / IsLeftPressed for HUD routing
            // AND fires onPointerEnter/Exit/Down/Up/Click events so app code
            // like DragRotateCube and the new MacRightDragMoveWindow gate
            // can know whether the cursor is on a clickable renderer.
            //
            // Skip in editor; gate to the active rig.
            if (Application.isEditor || m_Camera == null)
                return;
            if (DisplayXRRigManager.ActiveCamera != null
                && DisplayXRRigManager.ActiveCamera != m_Camera)
                return;
            #if HAS_INPUT_SYSTEM
            {
                var mouse = Mouse.current;
                if (mouse == null) goto MAC_HITTEST_DONE;
                Vector2 raw = mouse.position.ReadValue();
                // Mouse.current is bottom-left origin; PointerPosition is
                // window-client pixels with TOP-LEFT origin (matches Win32
                // GetCursorPos+ScreenToClient convention). Flip Y so HUD /
                // hit-test math is platform-agnostic.
                int clientXMac = Mathf.RoundToInt(raw.x);
                int clientYMac = Mathf.RoundToInt(Screen.height - raw.y);
                Vector2 macPos = new Vector2(clientXMac, clientYMac);

                PointerDelta = m_HasPrevPointerPos
                    ? (macPos - m_PrevPointerPos)
                    : Vector2.zero;
                m_PrevPointerPos = macPos;
                m_HasPrevPointerPos = true;
                PointerPosition = macPos;
                IsLeftPressed = mouse.leftButton.isPressed;
                IsRightPressed = mouse.rightButton.isPressed;
                int macButtons = (IsLeftPressed ? 1 : 0)
                               | (IsRightPressed ? 2 : 0)
                               | (mouse.middleButton.isPressed ? 4 : 0);

                // Accumulate wheel into Win32-unit accumulator (120 per notch)
                // so apps can poll via ConsumeWheelDelta() with platform-
                // identical semantics. Mac's scroll value is "ticks" with
                // 1.0 == one notch on a standard wheel — multiply by 120.
                float macWheelY = mouse.scroll.ReadValue().y;
                if (Mathf.Abs(macWheelY) > 0.001f)
                    m_MacWheelAccum += Mathf.RoundToInt(macWheelY * 120f);

                // Cyclopean hit-test (per-triangle for SMRs). Identical to the
                // Win32 path — same helpers, same hysteresis. Overlay size on
                // Mac is just the Unity window size (no off-screen-HWND quirk
                // to compensate for).
                if (clickableRenderers != null && clickableRenderers.Length > 0)
                {
                    int overlayWMac = Screen.width;
                    int overlayHMac = Screen.height;
                    UpdateBakedHitColliders();

                    Renderer hitRendererMac = null;
                    if (clientXMac >= 0 && clientYMac >= 0
                        && TryGetStereoMatrices(out Matrix4x4 lvM, out Matrix4x4 lpM,
                                                 out Matrix4x4 rvM, out Matrix4x4 rpM))
                    {
                        BuildCyclopean(lvM, lpM, rvM, rpM,
                                       out Matrix4x4 cvM, out Matrix4x4 cpM);
                        GetStereoViewport(overlayWMac, overlayHMac,
                                          out int vpXM, out int vpYM, out int vpWM, out int vpHM);
                        if (TryBuildEyeRay(clientXMac, clientYMac, vpXM, vpYM, vpWM, vpHM, cvM, cpM, out Ray rayM))
                        {
                            float bestTM = m_Camera.farClipPlane;
                            for (int i = 0; i < clickableRenderers.Length; i++)
                            {
                                var r = clickableRenderers[i];
                                if (r == null) continue;
                                if (r is SkinnedMeshRenderer smrM)
                                {
                                    if (TryRayHitBakedSkinnedMesh(rayM, bestTM, smrM, out float tM))
                                    {
                                        hitRendererMac = r;
                                        bestTM = tM;
                                    }
                                }
                                else
                                {
                                    if (Physics.Raycast(rayM, out RaycastHit infoM, bestTM)
                                        && infoM.collider != null
                                        && infoM.collider.transform.IsChildOf(r.transform))
                                    {
                                        hitRendererMac = r;
                                        bestTM = infoM.distance;
                                    }
                                }
                            }
                        }
                    }

                    // Same sticky-frame hysteresis as Win32.
                    const int STICKY_FRAMES_MAC = 8;
                    if (hitRendererMac != null)
                    {
                        m_LastHitRenderer = hitRendererMac;
                        m_LastHitFrame = Time.frameCount;
                    }
                    else if (m_LastHitRenderer != null
                             && Time.frameCount - m_LastHitFrame <= STICKY_FRAMES_MAC)
                    {
                        hitRendererMac = m_LastHitRenderer;
                    }
                    else
                    {
                        m_LastHitRenderer = null;
                    }

                    // Hover transitions
                    if (hitRendererMac != m_HoverRenderer)
                    {
                        if (m_HoverRenderer != null) onPointerExit?.Invoke(m_HoverRenderer);
                        if (hitRendererMac != null) onPointerEnter?.Invoke(hitRendererMac);
                        m_HoverRenderer = hitRendererMac;
                    }

                    // Button transitions (left = click, right = drag-handled-by-app)
                    DispatchButton(macButtons, 1, ref m_LeftWasDown, hitRendererMac);
                    DispatchButton(macButtons, 2, ref m_RightWasDown, hitRendererMac);
                }
            }
            MAC_HITTEST_DONE:;
            #endif
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


#if UNITY_STANDALONE_WIN
        // Simple-window mode: read the cursor + buttons from Unity's real
        // input (Mouse.current is live because the real HWND is foreground).
        // Returns top-left-origin client pixels to match the overlay path's
        // GetCursorPos+ScreenToClient convention. buttons: bit0=L,1=R,2=M.
        private void GetRealWindowPointer(out int clientX, out int clientY, out int buttons)
        {
            clientX = -1; clientY = -1; buttons = 0;
#if HAS_INPUT_SYSTEM
            var m = Mouse.current;
            if (m == null) return;
            Vector2 raw = m.position.ReadValue();        // bottom-left origin
            clientX = Mathf.RoundToInt(raw.x);
            clientY = Mathf.RoundToInt(Screen.height - raw.y); // → top-left origin
            if (m.leftButton.isPressed)   buttons |= 1;
            if (m.rightButton.isPressed)  buttons |= 2;
            if (m.middleButton.isPressed) buttons |= 4;
#else
            Vector3 mp = Input.mousePosition;
            clientX = Mathf.RoundToInt(mp.x);
            clientY = Mathf.RoundToInt(Screen.height - mp.y);
            if (Input.GetMouseButton(0)) buttons |= 1;
            if (Input.GetMouseButton(1)) buttons |= 2;
            if (Input.GetMouseButton(2)) buttons |= 4;
#endif
        }

#endif

        void OnDisable()
        {
#if UNITY_STANDALONE_WIN
            if (!Application.isEditor)
            {
                if (m_SimpleWindow)
                    DisplayXRNative.displayxr_set_simple_window(0, 0);
                else
                    DisplayXRNative.displayxr_set_transparent_overlay(0, 0);
                DisplayXRNative.displayxr_set_overlay_hit_active(0);
            }
            ReleaseHitMaskResources();
#elif UNITY_STANDALONE_OSX
            if (!Application.isEditor || Application.isPlaying)
            {
                DisplayXRNative.displayxr_macos_configure_unity_nswindow(0);
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
            if (Application.isEditor) return 0;
#if UNITY_STANDALONE_WIN
            return DisplayXRNative.displayxr_consume_overlay_wheel_delta();
#elif UNITY_STANDALONE_OSX
            // Mac: no native polling needed — Mouse.current.scroll is cleanly
            // delivered by Cocoa (no cloaked-HWND quirk). LateUpdate
            // accumulates into m_MacWheelAccum each frame; this call reads
            // and zeroes the accumulator, converting to Win32 raw units
            // (120 per notch) so apps see the same value on both platforms.
            int acc = m_MacWheelAccum;
            m_MacWheelAccum = 0;
            return acc;
#else
            return 0;
#endif
        }

#if UNITY_STANDALONE_OSX
        private int m_MacWheelAccum;
#endif

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

        private readonly float[] m_SmLV = new float[16], m_SmLP = new float[16],
                                 m_SmRV = new float[16], m_SmRP = new float[16];

        private bool TryGetStereoMatrices(out Matrix4x4 leftView, out Matrix4x4 leftProj,
                                           out Matrix4x4 rightView, out Matrix4x4 rightProj)
        {
            leftView = leftProj = rightView = rightProj = Matrix4x4.identity;

            // The provider publishes the per-eye matrices to the shared state each
            // frame — read them directly via the native getter so the cyclopean
            // hit-test (LMB drag on the silhouette) works.
            DisplayXRNative.displayxr_get_stereo_matrices(
                m_SmLV, m_SmLP, m_SmRV, m_SmRP, out int pvalid);
            if (pvalid == 0) return false;
            leftView = FloatsToMatrix(m_SmLV); leftProj = FloatsToMatrix(m_SmLP);
            rightView = FloatsToMatrix(m_SmRV); rightProj = FloatsToMatrix(m_SmRP);
            return true;
        }

        // Column-major float[16] → Unity Matrix4x4.
        private static Matrix4x4 FloatsToMatrix(float[] m)
        {
            var mat = new Matrix4x4();
            mat.m00 = m[0];  mat.m10 = m[1];  mat.m20 = m[2];  mat.m30 = m[3];
            mat.m01 = m[4];  mat.m11 = m[5];  mat.m21 = m[6];  mat.m31 = m[7];
            mat.m02 = m[8];  mat.m12 = m[9];  mat.m22 = m[10]; mat.m32 = m[11];
            mat.m03 = m[12]; mat.m13 = m[13]; mat.m23 = m[14]; mat.m33 = m[15];
            return mat;
        }



        // The stereo viewport (overlay client px) the Kooima projection from
        // displayxr_get_stereo_matrices is computed for. Defaults to the full
        // overlay; when a 3D canvas sub-rect is active (#131) the projection is
        // built for that sub-rect, so the raycast must map the cursor into it.
        private static void GetStereoViewport(int overlayW, int overlayH,
                                              out int vpX, out int vpY,
                                              out int vpW, out int vpH)
        {
            vpX = 0; vpY = 0; vpW = overlayW; vpH = overlayH;
            try
            {
                if (DisplayXRNative.displayxr_get_canvas_rect_px(
                        out int cx, out int cy, out uint cw, out uint ch) != 0
                    && cw > 0 && ch > 0)
                {
                    vpX = cx; vpY = cy; vpW = (int)cw; vpH = (int)ch;
                }
            }
            catch (System.EntryPointNotFoundException) { }
        }

        // Unproject a window-pixel cursor through one eye's view + projection
        // into a Unity-world-space Ray. Inverse path of ProjectThroughEye:
        // viewport px → NDC → inverse projection (eye-space, OpenXR convention)
        // → inverse view (Unity world). Origin = ray's near-plane point;
        // direction = far-near. Returns false if the projection is degenerate.
        private static bool TryBuildEyeRay(int clientX, int clientY,
                                            int vpX, int vpY, int vpW, int vpH,
                                            Matrix4x4 viewOpenXR, Matrix4x4 projOpenXR,
                                            out Ray ray)
        {
            ray = new Ray(Vector3.zero, Vector3.forward);
            // Map the cursor to NDC over the stereo VIEWPORT — the region the
            // Kooima projection is computed for. Normally that's the full
            // overlay client area, but when a 3D canvas sub-rect is active
            // (#131) the projection is built for that (smaller, offset)
            // sub-rect, so cursor → NDC must use it too; otherwise the ray
            // lands at the content's full-window position and misses the
            // shrunk-into-the-sub-rect collider. (overlayW/H is passed as the
            // viewport when no sub-rect is set.)
            float sw = Mathf.Max(1, vpW);
            float sh = Mathf.Max(1, vpH);
            // Viewport pixel (top-left origin) → NDC ([-1,1] with +Y up)
            float ndcX = 2f * (clientX - vpX) / sw - 1f;
            float ndcY = 1f - 2f * (clientY - vpY) / sh;

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

        // Convert an OpenXR-convention view matrix (right-handed
        // world, -Z forward) to a Unity-convention view matrix that
        // accepts Unity-world points (left-handed, +Z forward).
        //
        // Negating the third column flips the Z basis vector — when
        // the matrix is applied to (x_u, y_u, z_u, 1) it gives the
        // same eye-space result as applying the original to
        // (x_u, y_u, -z_u, 1), i.e. as if we first converted Unity
        // world coords to OpenXR world coords by negating Z.
        //
        // This is the same conversion the C# raycast helper
        // ProjectThroughEye performs. Used here for the silhouette
        // mask projection — the shader's input world-space points
        // come from unity_ObjectToWorld (Unity-world), so the view
        // matrix in our _DXRViewProj uniform has to accept Unity-
        // world input.
        private static Matrix4x4 ConvertOpenXRViewToUnity(Matrix4x4 v)
        {
            v.m02 = -v.m02;
            v.m12 = -v.m12;
            v.m22 = -v.m22;
            v.m32 = -v.m32;
            return v;
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

        // -------------------------------------------------------------
        // Per-pixel silhouette hit mask (Approach B+, issue #57)
        // -------------------------------------------------------------

        // Draw EVERY submesh of a renderer into the silhouette mask. The old
        // single DrawRenderer(r, mat, 0, 0) only rasterized submesh 0, so a mesh
        // with multiple material slots (e.g. body / face / limbs on separate
        // submeshes — common for character FBX imports) left the other submeshes
        // out of the mask → interior click-through holes over those parts (#131).
        // The silhouette shader writes solid 1 for any rasterized triangle, so
        // covering all submeshes makes the silhouette truly solid.
        void DrawSilhouetteAllSubmeshes(Renderer r)
        {
            Mesh mesh = null;
            if (r is SkinnedMeshRenderer smr) mesh = smr.sharedMesh;
            else { var mf = r.GetComponent<MeshFilter>(); if (mf != null) mesh = mf.sharedMesh; }

            int subCount = (mesh != null) ? Mathf.Max(1, mesh.subMeshCount) : 1;
            for (int s = 0; s < subCount; s++)
                m_HitMaskCB.DrawRenderer(r, m_SilhouetteMat, s, 0);
        }

        void EnsureHitMaskResources()
        {
            if (m_HitMaskRT == null)
            {
                m_HitMaskRT = new RenderTexture(HIT_MASK_WIDTH, HIT_MASK_HEIGHT,
                                                0, RenderTextureFormat.R8)
                {
                    filterMode  = FilterMode.Point,
                    useMipMap   = false,
                    autoGenerateMips = false,
                };
                m_HitMaskRT.Create();
            }
            if (m_HitMaskDilatedRT == null)
            {
                m_HitMaskDilatedRT = new RenderTexture(HIT_MASK_WIDTH, HIT_MASK_HEIGHT,
                                                       0, RenderTextureFormat.R8)
                {
                    filterMode  = FilterMode.Point,
                    useMipMap   = false,
                    autoGenerateMips = false,
                };
                m_HitMaskDilatedRT.Create();
            }
            if (m_SilhouetteMat == null)
            {
                Shader s = Shader.Find("Hidden/DisplayXR/Silhouette");
                if (s != null) m_SilhouetteMat = new Material(s) { hideFlags = HideFlags.HideAndDontSave };
            }
            if (m_SilhouetteDilateMat == null)
            {
                Shader s = Shader.Find("Hidden/DisplayXR/SilhouetteDilate");
                if (s != null) m_SilhouetteDilateMat = new Material(s) { hideFlags = HideFlags.HideAndDontSave };
            }
            if (m_HitMaskCB == null)
            {
                m_HitMaskCB = new CommandBuffer { name = "DisplayXR Hit Mask" };
            }
        }

        // Build a one-shot CommandBuffer that:
        //   1) Renders each clickable renderer ONCE PER EYE into
        //      m_HitMaskRT, using CommandBuffer.SetGlobalMatrix to
        //      push the per-eye view-projection as a custom property
        //      (_DXRViewProj) the silhouette shader consumes. Two
        //      passes accumulate into the same RT (no clear between);
        //      shader writes (1,0,0,1) unconditionally with no blend,
        //      so the result is the boolean union of the two
        //      silhouettes — what the lenticular actually displays.
        //   2) Blits through the 5x5 max-kernel dilation shader into
        //      m_HitMaskDilatedRT so AA-edge pixels and any sub-pixel
        //      mismatch fall inside the SetWindowRgn region.
        //   3) AsyncGPUReadback's the dilated RT.
        //
        // The custom _DXRViewProj uniform is critical: Unity's XR
        // render pipeline overrides UNITY_MATRIX_VP per stereo eye,
        // so CommandBuffer.SetViewProjectionMatrices is ineffective
        // here and DrawRenderer ends up rasterizing both passes with
        // the same matrices. _DXRViewProj is a property Unity doesn't
        // know about, so SetGlobalMatrix sticks across the draws.
        //
        // For N-view extension: add more (view, proj) pairs and call
        // SetGlobalMatrix + DrawRenderer for each. Architecture-friendly.
        //
        // Throttled to one outstanding readback at a time.
        void RenderHitMaskAndRequestReadback(
            Matrix4x4 leftView,  Matrix4x4 leftProj,
            Matrix4x4 rightView, Matrix4x4 rightProj,
            int overlayW, int overlayH)
        {
            if (clickableRenderers == null || clickableRenderers.Length == 0)
                return;

            EnsureHitMaskResources();
            if (m_SilhouetteMat == null) return;
            if (m_HitMaskReadbackPending) return;

            // Convert OpenXR-convention view matrices (right-handed,
            // -Z forward in world space) to Unity-convention (left-
            // handed, +Z forward) by negating the third COLUMN. The
            // shader's input world-space points come from
            // unity_ObjectToWorld (Unity-world), but the raw view
            // matrices from displayxr_get_stereo_matrices expect
            // OpenXR-world input. Without this conversion a Unity-world
            // point at +Z is interpreted as OpenXR's +Z (= behind the
            // camera), projects out of clip range, and gets clipped —
            // worst at high-Z foreground geometry like the tiger's
            // hands. This is the same conversion ProjectThroughEye
            // applies for the C# raycast (negating m02/m12/m22/m32 of
            // the view matrix).
            //
            // GL.GetGPUProjectionMatrix(proj, renderIntoTexture=FALSE)
            // does depth-range remap (OpenGL [-1,1] → D3D [0,1]) but
            // NO Y-flip. We don't want the Y-flip — we read the RT
            // back as CPU bytes and ship them to SetWindowRgn, where
            // row 0 = top of overlay (Win32 client coords). The Y-flip
            // would put row 0 at the bottom of the intended image.
            m_HitMaskCB.Clear();
            m_HitMaskCB.SetRenderTarget(m_HitMaskRT);
            m_HitMaskCB.ClearRenderTarget(true, true, Color.clear);

            // Build a full-window silhouette union. Single zone / hook = just the primary
            // eyes. Multi-zone (#166) = union EVERY zone's L+R silhouette (still full-window,
            // NO sub-rect) so the mask covers all zones' per-eye extents — each zone's
            // off-axis frustum gives its eyes a slightly different reach, and a primary-only
            // mask falls short on the other zones' outer eye. The NATIVE side
            // (displayxr_set_overlay_hit_mask) then stamps this union into each zone's rect;
            // because every zone projects to ~the same window spot, the union is only
            // marginally wider and lines up with each zone's woven content once stamped.
            int zoneCount = 1;
            if (DisplayXRProviderDriver.IsActive)
            {
                uint zc = DisplayXRProviderNative.dxr_prov_get_zone_count();
                if (zc > 1) zoneCount = (int)zc;
            }

            for (int z = 0; z < zoneCount; z++)
            {
                Matrix4x4 lView, lProj, rView, rProj;
                if (zoneCount > 1)
                {
                    if (DisplayXRProviderNative.dxr_prov_get_zone_stereo_matrices(
                            (uint)z, m_ZoneLV, m_ZoneLP, m_ZoneRV, m_ZoneRP) == 0)
                        continue;
                    lView = FloatsToMatrix(m_ZoneLV); lProj = FloatsToMatrix(m_ZoneLP);
                    rView = FloatsToMatrix(m_ZoneRV); rProj = FloatsToMatrix(m_ZoneRP);
                }
                else
                {
                    lView = leftView; lProj = leftProj; rView = rightView; rProj = rightProj;
                }

                Matrix4x4 lVP = GL.GetGPUProjectionMatrix(lProj, false) * ConvertOpenXRViewToUnity(lView);
                Matrix4x4 rVP = GL.GetGPUProjectionMatrix(rProj, false) * ConvertOpenXRViewToUnity(rView);

                // Left eye then right — accumulate the boolean union into the same RT.
                m_HitMaskCB.SetGlobalMatrix("_DXRViewProj", lVP);
                for (int i = 0; i < clickableRenderers.Length; i++)
                {
                    var r = clickableRenderers[i];
                    if (r == null || !r.enabled || !r.gameObject.activeInHierarchy) continue;
                    DrawSilhouetteAllSubmeshes(r);
                }
                m_HitMaskCB.SetGlobalMatrix("_DXRViewProj", rVP);
                for (int i = 0; i < clickableRenderers.Length; i++)
                {
                    var r = clickableRenderers[i];
                    if (r == null || !r.enabled || !r.gameObject.activeInHierarchy) continue;
                    DrawSilhouetteAllSubmeshes(r);
                }
            }

            // Dilate the union by ~2 mask pixels to absorb AA-edge
            // pixels at the silhouette boundary. Falls back to a
            // direct readback of m_HitMaskRT if the dilation shader
            // is unavailable.
            RenderTexture readbackRT;
            if (m_SilhouetteDilateMat != null && m_HitMaskDilatedRT != null)
            {
                m_HitMaskCB.Blit(m_HitMaskRT, m_HitMaskDilatedRT, m_SilhouetteDilateMat);
                readbackRT = m_HitMaskDilatedRT;
            }
            else
            {
                readbackRT = m_HitMaskRT;
            }

            Graphics.ExecuteCommandBuffer(m_HitMaskCB);

            m_HitMaskPendingDstW = overlayW;
            m_HitMaskPendingDstH = overlayH;
            m_HitMaskReadbackPending = true;
            AsyncGPUReadback.Request(readbackRT, 0, OnHitMaskReadback);
        }

        void OnHitMaskReadback(AsyncGPUReadbackRequest req)
        {
            m_HitMaskReadbackPending = false;
            if (req.hasError) return;
            if (m_HitMaskRT == null) return;

            var data = req.GetData<byte>();
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
            unsafe
            {
                IntPtr ptr = (IntPtr)Unity.Collections.LowLevel.Unsafe
                    .NativeArrayUnsafeUtility.GetUnsafeReadOnlyPtr(data);
                DisplayXRNative.displayxr_set_overlay_hit_mask(
                    ptr,
                    HIT_MASK_WIDTH, HIT_MASK_HEIGHT,
                    m_HitMaskPendingDstW, m_HitMaskPendingDstH);
            }
            if (s_DumpHitMask && (m_HitMaskDumpCounter++ % 15) == 0)
                DumpHitMaskPng(data);
#endif
        }

        // Debug: encode the R8 hit-mask readback to a grayscale PNG so the silhouette
        // coverage (which region SetWindowRgn keeps visible) can be inspected. The mask
        // is HIT_MASK_WIDTH×HIT_MASK_HEIGHT, one byte per pixel, row 0 = top.
        void DumpHitMaskPng(Unity.Collections.NativeArray<byte> data)
        {
            try
            {
                int n = HIT_MASK_WIDTH * HIT_MASK_HEIGHT;
                if (data.Length < n) return;
                var tex = new Texture2D(HIT_MASK_WIDTH, HIT_MASK_HEIGHT, TextureFormat.RGBA32, false);
                var px = new Color32[n];
                for (int i = 0; i < n; i++)
                {
                    byte v = data[i];
                    // Flip vertically: PNG row 0 = bottom, mask row 0 = top.
                    int x = i % HIT_MASK_WIDTH, y = i / HIT_MASK_WIDTH;
                    px[(HIT_MASK_HEIGHT - 1 - y) * HIT_MASK_WIDTH + x] = new Color32(v, v, v, 255);
                }
                tex.SetPixels32(px);
                tex.Apply(false);
                byte[] png = tex.EncodeToPNG();
                Destroy(tex);
                string path = System.IO.Path.Combine(
                    System.Environment.GetEnvironmentVariable("TEMP") ?? ".", "displayxr_hitmask.png");
                System.IO.File.WriteAllBytes(path, png);
                Debug.Log($"[DisplayXR] hit-mask dumped to {path} ({HIT_MASK_WIDTH}x{HIT_MASK_HEIGHT})");
            }
            catch (System.Exception e) { Debug.LogWarning($"[DisplayXR] hit-mask dump failed: {e.Message}"); }
        }

        void ReleaseHitMaskResources()
        {
            // Drop the readback flag so OnHitMaskReadback (which may
            // fire after OnDisable) silently no-ops instead of
            // dereferencing freed resources.
            m_HitMaskReadbackPending = false;
            if (m_HitMaskRT != null)
            {
                m_HitMaskRT.Release();
                Destroy(m_HitMaskRT);
                m_HitMaskRT = null;
            }
            if (m_HitMaskDilatedRT != null)
            {
                m_HitMaskDilatedRT.Release();
                Destroy(m_HitMaskDilatedRT);
                m_HitMaskDilatedRT = null;
            }
            if (m_SilhouetteMat != null)
            {
                Destroy(m_SilhouetteMat);
                m_SilhouetteMat = null;
            }
            if (m_SilhouetteDilateMat != null)
            {
                Destroy(m_SilhouetteDilateMat);
                m_SilhouetteDilateMat = null;
            }
            if (m_HitMaskCB != null)
            {
                m_HitMaskCB.Release();
                m_HitMaskCB = null;
            }
            // Tell native to drop the mask region (reverts to AABB-
            // region path for the next hit_rect push).
#if UNITY_STANDALONE_WIN
            if (!Application.isEditor)
                DisplayXRNative.displayxr_set_overlay_hit_mask(IntPtr.Zero, 0, 0, 0, 0);
#endif
        }
    }
}
