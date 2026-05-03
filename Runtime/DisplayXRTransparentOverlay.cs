// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;
using UnityEngine.Events;
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
    /// instead. They're driven by a per-frame Physics.Raycast at the polled
    /// cursor position (Win32 GetCursorPos, always works) against
    /// <see cref="clickableRenderers"/>' colliders.
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

        [Tooltip("Renderers (with colliders) that drive the pointer events. " +
                 "A per-frame Physics.Raycast at the cursor decides which one " +
                 "is under the pointer; clicks anywhere off the silhouettes " +
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

        void Update()
        {
#if UNITY_STANDALONE_WIN
            if (Application.isEditor || m_Camera == null)
                return;
            if (clickableRenderers == null || clickableRenderers.Length == 0)
                return;
            // No active-rig gate — gating to active killed clicks because
            // the active rig's mono ScreenPointToRay against the asymmetric
            // Kooima frustum doesn't reliably hit the cube collider where
            // the user perceives it (binocular stereo render places the
            // cube slightly off the mono projection's hit area). Both rigs
            // run; whichever one's raycast lands wins last-writer-wins,
            // which is reliable enough to catch clicks. Trade-off: in
            // multi-rig scenes the inactive rig's projection of the cube
            // creates a phantom hit zone (e.g. top-right of the window).
            // Cleanest fix is single-rig scenes for the avatar use case.

            // Poll cursor + buttons via native (GetCursorPos + ScreenToClient
            // on the overlay HWND, plus s_vkey_state populated by raw input).
            // Bypasses Unity's New Input System entirely, which is broken for
            // cloaked windows — see component header comment.
            DisplayXRNative.displayxr_get_overlay_pointer(
                out int clientX, out int clientY, out int buttons);

            Renderer hitRenderer = null;
            if (clientX >= 0 && clientY >= 0)
            {
                // Cursor in overlay-client coords (= Unity-client coords),
                // top-left origin, in window pixels (e.g. 0..Screen.width).
                // Camera.ScreenPointToRay expects camera-pixel coords with
                // bottom-left origin, in [0..Camera.pixelWidth] etc. The
                // camera's pixel range is the OpenXR eye render target size,
                // which is smaller than Screen on HiDPI (e.g. 1920×1080 on a
                // 4K display at 250% DPI where Screen reports 2498×2248). So:
                //   1. Y-flip from top-left to bottom-left.
                //   2. Scale by camPixel/Screen.
                float camW = Mathf.Max(1, m_Camera.pixelWidth);
                float camH = Mathf.Max(1, m_Camera.pixelHeight);
                float sw   = Mathf.Max(1, Screen.width);
                float sh   = Mathf.Max(1, Screen.height);
                Vector3 camPos = new Vector3(
                    clientX           * camW / sw,
                    (sh - clientY)    * camH / sh,
                    0f);
                Ray ray = m_Camera.ScreenPointToRay(camPos);
                if (Physics.Raycast(ray, out RaycastHit info, m_Camera.farClipPlane))
                {
                    for (int i = 0; i < clickableRenderers.Length; i++)
                    {
                        var r = clickableRenderers[i];
                        if (r != null && info.collider != null &&
                            info.collider.transform.IsChildOf(r.transform))
                        {
                            hitRenderer = r;
                            break;
                        }
                    }
                }
            }

            // Drive WM_NCHITTEST: only return HTCLIENT (overlay accepts the
            // click) when the cursor is on a clickable silhouette. Otherwise
            // HTTRANSPARENT — clicks fall through within the same thread.
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
                float invY = Mathf.Max(1, Screen.height) - clientY;
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

        void LateUpdate()
        {
#if UNITY_STANDALONE_WIN
            if (Application.isEditor || m_Camera == null)
                return;
            if (clickableRenderers == null || clickableRenderers.Length == 0)
                return;

            // No active-rig gate — same reason as in Update.

            // Coarse hit rect (cube AABB in window pixels) — used by
            // WM_NCHITTEST as a fast reject before the per-pixel hit_active
            // check from Update kicks in.
            if (TryGetScreenRect(out int x, out int y, out int w, out int h))
                DisplayXRNative.displayxr_set_overlay_hit_rect(x, y, w, h);
#endif
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
        //
        // Critical detail: Camera.WorldToScreenPoint returns coordinates in
        // the camera's pixelRect range (Camera.pixelWidth × Camera.pixelHeight),
        // which on OpenXR DisplayXR builds is the eye render-target size
        // (e.g. 1920×1080) — NOT Screen.width × Screen.height (e.g. 2498×2248
        // on a 4K display at 250% DPI). We must rescale to actual window
        // pixels before Y-flipping for Win32, otherwise the hit-rect lands in
        // only a fraction of the actual window — historically causing the
        // "right-drag only works in the bottom half of the window" symptom.
        private bool TryGetScreenRect(out int x, out int y, out int w, out int h)
        {
            x = y = w = h = 0;

            float minX = float.PositiveInfinity, minY = float.PositiveInfinity;
            float maxX = float.NegativeInfinity, maxY = float.NegativeInfinity;
            bool any = false;

            float camW = Mathf.Max(1, m_Camera.pixelWidth);
            float camH = Mathf.Max(1, m_Camera.pixelHeight);
            float sxScale = Screen.width  / camW;
            float syScale = Screen.height / camH;

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
                    float spx = sp.x * sxScale;
                    float spy = sp.y * syScale;
                    if (spx < minX) minX = spx;
                    if (spy < minY) minY = spy;
                    if (spx > maxX) maxX = spx;
                    if (spy > maxY) maxY = spy;
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
