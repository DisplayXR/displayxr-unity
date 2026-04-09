// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;
#if HAS_INPUT_SYSTEM
using UnityEngine.InputSystem;
#endif

namespace DisplayXR
{
    /// <summary>
    /// Basic keyboard + mouse input for navigating around a DisplayXR rig.
    /// WASD = move horizontally, QE = move up/down, left-mouse drag = rotate.
    /// Scroll wheel = zoom (scale). Space = reset to initial pose.
    /// Attach to the same GameObject as DisplayXRDisplay or DisplayXRCamera.
    /// Works in Play Mode (including with the standalone preview via PlayModeIntegration).
    /// </summary>
    [AddComponentMenu("DisplayXR/Input Controller")]
    public class DisplayXRInputController : MonoBehaviour
    {
        [Header("Movement")]
        [Tooltip("Movement speed in meters per second.")]
        public float moveSpeed = 1.0f;

        [Tooltip("Mouse rotation sensitivity (radians per pixel).")]
        public float rotationSensitivity = 0.005f;

        [Tooltip("Scroll wheel zoom speed (scale factor per scroll tick).")]
        public float zoomSpeed = 0.1f;

        private float m_Yaw;
        private float m_Pitch;
        private bool m_Dragging;
        private Vector2 m_LastMousePos;

        private Vector3 m_InitialPosition;
        private float m_InitialYaw, m_InitialPitch;
        private Vector3 m_InitialScale;

        // Rig type detection for zoom behavior
        private Camera m_Camera;
        private bool m_IsCameraCentric;
        private float m_InitialFov;

        void Start()
        {
            Application.runInBackground = true;
#if HAS_INPUT_SYSTEM
            InputSystem.settings.backgroundBehavior = InputSettings.BackgroundBehavior.IgnoreFocus;
#if UNITY_EDITOR
            InputSystem.settings.editorInputBehaviorInPlayMode =
                InputSettings.EditorInputBehaviorInPlayMode.AllDeviceInputAlwaysGoesToGameView;
#endif
#endif

            Vector3 euler = transform.eulerAngles;
            m_Yaw = euler.y * Mathf.Deg2Rad;
            m_Pitch = euler.x * Mathf.Deg2Rad;
            if (m_Pitch > Mathf.PI) m_Pitch -= 2f * Mathf.PI;

            m_InitialPosition = transform.position;
            m_InitialYaw = m_Yaw;
            m_InitialPitch = m_Pitch;
            m_InitialScale = transform.localScale;

            // Cache camera reference (needed for active check and zoom)
            m_Camera = GetComponent<Camera>();
            m_IsCameraCentric = GetComponent<DisplayXRCamera>() != null;
            if (m_IsCameraCentric)
                m_InitialFov = m_Camera.fieldOfView;
        }

        // Rendering mode cycling
        private int m_CurrentRenderingMode = 1;
        private static int s_LastTabFrame = -1;

        void Update()
        {
            // Tab cycles cameras globally (only process once per frame)
            if (GetKeyDown(KeyCode.Tab) && Time.frameCount != s_LastTabFrame)
            {
                s_LastTabFrame = Time.frameCount;
                DisplayXRRigManager.CycleNext();
            }

            if (!IsActiveCamera())
            {
                // Clear drag state so we don't jump on reactivation
                m_Dragging = false;
                m_DragPending = false;
                return;
            }

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
            UpdateShellMouse();
#endif
            HandleMouseRotation();
            HandleKeyboardMovement();
            HandleScrollZoom();
            HandleReset();
            HandleQuit();
            HandleFullscreen();
            HandleModeCycle();
        }

        private bool IsActiveCamera()
        {
            var active = DisplayXRRigManager.ActiveCamera;
            return active == null || active == m_Camera;
        }

        private const float kDragThreshold = 3f; // pixels before drag starts
        private bool m_DragPending;
        private Vector2 m_DragStartPos;

        private void HandleMouseRotation()
        {
            // Cancel any in-progress drag if input should be ignored
            // (e.g. preview window is being moved/resized).
            // Checked every frame, not just on mouseDown, because the
            // window interaction flag may arrive after the mouseDown.
            if (ShouldIgnoreInput())
            {
                m_Dragging = false;
                m_DragPending = false;
                return;
            }

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
            // Shell mode: Mouse.current.position is frozen in background.
            // Use Mouse.current.delta (Raw Input deltas via RIDEV_INPUTSINK)
            // and our native button tracker instead.
            if (IsShellMode() && Mouse.current != null)
            {
                if (ShellGetMouseButtonDown(0))
                    m_Dragging = true;
                if (ShellGetMouseButtonUp(0))
                    m_Dragging = false;
                if (m_Dragging)
                {
                    Vector2 delta = Mouse.current.delta.ReadValue();
                    m_Yaw += delta.x * rotationSensitivity;
                    m_Pitch -= delta.y * rotationSensitivity;
                    m_Pitch = Mathf.Clamp(m_Pitch, -1.4f, 1.4f);
                    transform.rotation = Quaternion.Euler(
                        m_Pitch * Mathf.Rad2Deg,
                        m_Yaw * Mathf.Rad2Deg,
                        0f);
                }
                return;
            }
#endif
            if (GetMouseButtonDown(0))
            {
                m_DragPending = true;
                m_DragStartPos = GetMousePosition();
                m_LastMousePos = m_DragStartPos;
            }
            if (GetMouseButtonUp(0))
            {
                m_Dragging = false;
                m_DragPending = false;
            }

            if (m_DragPending && !m_Dragging)
            {
                Vector2 pos = GetMousePosition();
                if ((pos - m_DragStartPos).sqrMagnitude > kDragThreshold * kDragThreshold)
                {
                    m_Dragging = true;
                    m_DragPending = false;
                    m_LastMousePos = pos;
                }
            }

            if (m_Dragging)
            {
                Vector2 pos = GetMousePosition();
                Vector2 delta = pos - m_LastMousePos;
                m_Yaw += delta.x * rotationSensitivity;
                m_Pitch -= delta.y * rotationSensitivity;
                m_Pitch = Mathf.Clamp(m_Pitch, -1.4f, 1.4f);
                m_LastMousePos = pos;

                transform.rotation = Quaternion.Euler(
                    m_Pitch * Mathf.Rad2Deg,
                    m_Yaw * Mathf.Rad2Deg,
                    0f);
            }
        }

        // --- Shell mode mouse input (bypasses Input System entirely) ---
        // Unity reads mouse buttons from legacy WM_LBUTTONDOWN which only goes
        // to the foreground window. In shell mode we read button state directly
        // from the native WM_INPUT tracker via P/Invoke.
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
        private static bool s_shellModeChecked;
        private static bool s_shellMode;
        private static int s_shellButtonsPrev;
        private static int s_shellButtonsCurr;

        private static bool IsShellMode()
        {
            if (!s_shellModeChecked)
            {
                s_shellMode = DisplayXRNative.displayxr_is_shell_mode() != 0;
                s_shellModeChecked = true;
            }
            return s_shellMode;
        }

        /// Call once per frame (from Update) to snapshot shell button state.
        private static void UpdateShellMouse()
        {
            if (!IsShellMode()) return;
            s_shellButtonsPrev = s_shellButtonsCurr;
            DisplayXRNative.displayxr_get_shell_mouse_state(
                out s_shellButtonsCurr, out _, out _);
        }

        private static bool ShellGetMouseButtonDown(int b)
        {
            int mask = 1 << b;
            return (s_shellButtonsCurr & mask) != 0 && (s_shellButtonsPrev & mask) == 0;
        }

        private static bool ShellGetMouseButtonUp(int b)
        {
            int mask = 1 << b;
            return (s_shellButtonsCurr & mask) == 0 && (s_shellButtonsPrev & mask) != 0;
        }

        private static bool ShellGetMouseButton(int b)
        {
            return (s_shellButtonsCurr & (1 << b)) != 0;
        }
#endif

        private void HandleKeyboardMovement()
        {
            // Compute direction vectors from stored yaw/pitch, NOT from transform.
            // In XR mode, Unity's XR subsystem overwrites the camera transform each
            // frame with the tracked eye pose — reading transform.forward/right/up
            // gives the XR-modified orientation, not the controller's intended one.
            Quaternion ori = Quaternion.Euler(m_Pitch * Mathf.Rad2Deg, m_Yaw * Mathf.Rad2Deg, 0f);
            Vector3 fwd = ori * Vector3.forward;
            Vector3 rt = ori * Vector3.right;
            Vector3 up = ori * Vector3.up;

            Vector3 move = Vector3.zero;
            if (GetKey(KeyCode.W)) move += fwd;
            if (GetKey(KeyCode.S)) move -= fwd;
            if (GetKey(KeyCode.D)) move += rt;
            if (GetKey(KeyCode.A)) move -= rt;
            if (GetKey(KeyCode.E)) move += up;
            if (GetKey(KeyCode.Q)) move -= up;

            if (move.sqrMagnitude > 0f)
                transform.position += move.normalized * moveSpeed * Time.deltaTime;
        }

        private void HandleScrollZoom()
        {
            if (ShouldIgnoreInput()) return;
            float scroll = GetScrollDelta();
            if (Mathf.Abs(scroll) < 0.001f) return;

            if (m_IsCameraCentric)
            {
                // Camera-centric: zoom by adjusting FOV
                m_Camera.fieldOfView = Mathf.Clamp(
                    m_Camera.fieldOfView - scroll * zoomSpeed * 10f, 5f, 120f);
            }
            else
            {
                // Display-centric: zoom by scaling transform
                float factor = 1f + scroll * zoomSpeed;
                factor = Mathf.Clamp(factor, 0.5f, 2f);
                transform.localScale *= factor;
            }
        }

        private void HandleReset()
        {
            if (GetKeyDown(KeyCode.Space))
            {
                transform.position = m_InitialPosition;
                transform.localScale = m_InitialScale;
                m_Yaw = m_InitialYaw;
                m_Pitch = m_InitialPitch;
                transform.rotation = Quaternion.Euler(
                    m_Pitch * Mathf.Rad2Deg,
                    m_Yaw * Mathf.Rad2Deg,
                    0f);

                if (m_IsCameraCentric)
                    m_Camera.fieldOfView = m_InitialFov;
            }
        }

        private void HandleQuit()
        {
            if (GetKeyDown(KeyCode.Escape))
                Application.Quit();
        }

        private void HandleFullscreen()
        {
            if (GetKeyDown(KeyCode.F11))
                Screen.fullScreen = !Screen.fullScreen;
        }

        private void HandleModeCycle()
        {
            if (!GetKeyDown(KeyCode.V)) return;

            // Toggle 2D/3D via the non-standalone API (works in built apps)
            m_CurrentRenderingMode = m_CurrentRenderingMode == 0 ? 1 : 0;
            DisplayXRNative.displayxr_request_display_mode(m_CurrentRenderingMode);
            Debug.Log($"[DisplayXR] Display mode → {(m_CurrentRenderingMode == 0 ? "2D" : "3D")}");
        }

        private static bool ShouldIgnoreInput()
        {
            // Ignore input while the mouse cursor is over the preview window.
            try
            {
                if (DisplayXRNative.displayxr_standalone_window_is_interacting() != 0)
                    return true;
            }
            catch (System.EntryPointNotFoundException) { }

            // Runtime UI (Canvas/EventSystem)
            var es = UnityEngine.EventSystems.EventSystem.current;
            if (es != null && es.IsPointerOverGameObject())
                return true;

#if UNITY_EDITOR
            // Only capture mouse in Game View or Preview Window
            var focused = UnityEditor.EditorWindow.focusedWindow;
            if (focused == null) return true;
            string typeName = focused.GetType().Name;
            if (typeName != "GameView" && typeName != "DisplayXRPreviewWindow")
                return true;
#endif
            return false;
        }

        // --- Input abstraction (keyboard + mouse) ---

#if HAS_INPUT_SYSTEM
        private static bool GetKey(KeyCode k) =>
            Keyboard.current != null && Keyboard.current[ToKey(k)].isPressed;
        private static bool GetKeyDown(KeyCode k) =>
            Keyboard.current != null && Keyboard.current[ToKey(k)].wasPressedThisFrame;
        private static bool GetMouseButtonDown(int b)
        {
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
            if (IsShellMode()) return ShellGetMouseButtonDown(b);
#endif
            return Mouse.current != null && (b == 0 ? Mouse.current.leftButton.wasPressedThisFrame
                : b == 1 ? Mouse.current.rightButton.wasPressedThisFrame
                : Mouse.current.middleButton.wasPressedThisFrame);
        }
        private static bool GetMouseButtonUp(int b)
        {
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
            if (IsShellMode()) return ShellGetMouseButtonUp(b);
#endif
            return Mouse.current != null && (b == 0 ? Mouse.current.leftButton.wasReleasedThisFrame
                : b == 1 ? Mouse.current.rightButton.wasReleasedThisFrame
                : Mouse.current.middleButton.wasReleasedThisFrame);
        }
        private static Vector2 GetMousePosition() =>
            Mouse.current != null ? Mouse.current.position.ReadValue() : Vector2.zero;
        private static float GetScrollDelta() =>
            Mouse.current != null ? Mouse.current.scroll.ReadValue().y / 120f : 0f;

        private static Key ToKey(KeyCode k)
        {
            switch (k)
            {
                case KeyCode.W: return Key.W;
                case KeyCode.A: return Key.A;
                case KeyCode.S: return Key.S;
                case KeyCode.D: return Key.D;
                case KeyCode.Q: return Key.Q;
                case KeyCode.E: return Key.E;
                case KeyCode.V: return Key.V;
                case KeyCode.Space: return Key.Space;
                case KeyCode.Escape: return Key.Escape;
                case KeyCode.Tab: return Key.Tab;
                case KeyCode.F11: return Key.F11;
                default: return Key.None;
            }
        }
#else
        private static bool GetKey(KeyCode k) => Input.GetKey(k);
        private static bool GetKeyDown(KeyCode k) => Input.GetKeyDown(k);
        private static bool GetMouseButtonDown(int b) => Input.GetMouseButtonDown(b);
        private static bool GetMouseButtonUp(int b) => Input.GetMouseButtonUp(b);
        private static Vector2 GetMousePosition() => Input.mousePosition;
        private static float GetScrollDelta() => Input.mouseScrollDelta.y;
#endif
    }
}
