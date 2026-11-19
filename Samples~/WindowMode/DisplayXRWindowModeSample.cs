// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Window Mode sample (#477): an F11 toggle between "full screen on the 3D panel" and a
// window, an optional fade to black across the change, and a windowed size and position
// that are remembered - put back on leaving full screen and at the next launch.
//
// WHY A SAMPLE: which key, whether to fade, what to remember and where the window goes are
// app policy. The plug-in exposes the primitives this file is built from and nothing more:
//   - DisplayXRProvider.MoveWindowToDisplay()        put the window on the 3D panel
//   - DisplayXRWindowSpaceUI.TryGetWindowPixelSize() the size the provider renders for
//   - DisplayXRModeSwitch                            the eased ramp
//   - the rigs' ipdFactor                            stereo strength
// Everything else is Unity's own Screen API. Fork it freely.
//
// PIXELS: the remembered size and position are read from Unity's Screen API and handed
// back to it, and to nothing else. The one step that needs the panel's real place on the
// desktop is MoveWindowToDisplay(), which takes no coordinates from this file and does the
// geometry natively in a per-monitor-DPI-aware scope. See DisplayXRWindowModeRules.cs.
//
// USAGE: add it to an empty GameObject in your first scene. See the README for what it
// does not do (transparent overlay apps, the editor, native Wayland).

using System.Collections;
using System.Collections.Generic;
using UnityEngine;
#if ENABLE_INPUT_SYSTEM
using UnityEngine.InputSystem;
#endif

namespace DisplayXR.Samples
{
    [AddComponentMenu("DisplayXR/Samples/Window Mode (F11 fullscreen toggle)")]
    [DisallowMultipleComponent]
    [DefaultExecutionOrder(100)] // LateUpdate after the rigs and the provider driver (the fade quad follows them)
    public class DisplayXRWindowModeSample : MonoBehaviour
    {
        /// <summary>The PlayerPrefs key the remembered window is stored under (one string).</summary>
        public const string PrefsKey = "DisplayXR.Samples.WindowMode";

        [Header("Toggle")]
        [Tooltip("Key that toggles between full screen and a window. Read through the Input " +
                 "System when it is enabled, else the legacy Input Manager. None = no key: call " +
                 "Toggle() / SetFullscreen() from your own input.")]
        public KeyCode toggleKey = KeyCode.F11;

        [Tooltip("Going full screen, first move the window onto the DisplayXR panel " +
                 "(DisplayXRProvider.MoveWindowToDisplay), so it fills the 3D display and not " +
                 "whichever monitor the window happened to be on. Off, or with a runtime that " +
                 "does not report the panel: full screen on the current monitor.")]
        public bool fullscreenOnPanel = true;

        [Header("Remembered window")]
        [Tooltip("Remember the windowed size and position (PlayerPrefs) and put them back on " +
                 "leaving full screen.")]
        public bool rememberWindow = true;

        [Tooltip("At launch, put the window back the way the last run left it: the remembered " +
                 "windowed size and position, or full screen (on the panel). Do not combine with " +
                 "a DisplayXRTargetDisplay that also moves the window at start.")]
        public bool restoreOnLaunch = true;

        [Tooltip("Windowed size used when nothing is remembered yet (the app started full screen).")]
        public Vector2Int defaultWindowedSize = new Vector2Int(1280, 720);

        [Header("Fade")]
        [Tooltip("Fade to black around the change: stereo strength (the rig's ipdFactor) eases " +
                 "to 0 while a black quad fades in, the mode changes, and both ease back once " +
                 "the window has stopped changing size.")]
        public bool fade = true;

        [Tooltip("Length of each half of the fade, in seconds (unscaled time).")]
        [Range(0f, 2f)]
        public float fadeSeconds = 0.25f;

        public ModeSwitchEasing fadeEasing = ModeSwitchEasing.SmoothStep;

        [Tooltip("How many frames the window must keep one size before the fade comes back. " +
                 "The provider reallocates its swapchain after the size has held for 6 frames.")]
        [Range(1, 60)]
        public int settleFrames = 10;

        [Tooltip("Longest wait for the mode change and the window to settle, in seconds. The " +
                 "fade comes back after this even if the window never settled.")]
        [Range(0.5f, 10f)]
        public float settleTimeout = 3f;

        [Header("Lifetime")]
        [Tooltip("Keep this GameObject across scene loads (DontDestroyOnLoad). Use a GameObject " +
                 "of its own: everything else on it is kept too. A second instance in a later " +
                 "scene removes itself.")]
        public bool persistAcrossScenes = true;

        [Tooltip("Log what was done. Worth leaving on: a toggle that is not supported here and " +
                 "one that silently failed look the same otherwise.")]
        public bool logResult = true;

        /// <summary>True from the key press until the fade has come back.</summary>
        public bool IsSwitching { get; private set; }

        /// <summary>True when the window is in any full-screen mode.</summary>
        public static bool IsFullscreen => Screen.fullScreenMode != FullScreenMode.Windowed;

        static DisplayXRWindowModeSample s_Instance;

        DisplayXRSavedWindow m_Saved;
        bool m_HasSaved;
        bool m_Duplicate;
        int m_Switches; // switches started so far; the launch restore stands down after the first
        bool m_WarnedUnsupported;

        // ---- lifetime ------------------------------------------------------------

        void Awake()
        {
            if (s_Instance != null && s_Instance != this)
            {
                // A later scene's copy: the first one is still alive and owns the window.
                m_Duplicate = true;
                Destroy(this);
                return;
            }
            s_Instance = this;
            if (persistAcrossScenes)
            {
                transform.SetParent(null);
                DontDestroyOnLoad(gameObject);
            }
            m_HasSaved = DisplayXRWindowModeRules.TryParse(PlayerPrefs.GetString(PrefsKey, ""), out m_Saved);
        }

        void Start()
        {
            if (m_Duplicate) return;
            if (restoreOnLaunch && m_HasSaved && Unsupported() == null)
                StartCoroutine(RestoreAtLaunch());
        }

        void OnDisable()
        {
            if (m_Duplicate) return;
            // A switch cut short (the object was disabled, the app is quitting): never
            // leave the rig flat or the screen black.
            StopAllCoroutines();
            EndFade();
            IsSwitching = false;
        }

        void OnDestroy()
        {
            if (s_Instance == this) s_Instance = null;
            if (m_Quad != null) Destroy(m_Quad);
            if (m_QuadMaterial != null) Destroy(m_QuadMaterial);
            if (m_QuadMesh != null) Destroy(m_QuadMesh);
        }

        void OnApplicationQuit()
        {
            if (m_Duplicate || !rememberWindow || Unsupported() != null) return;
            if (!IsSwitching) CaptureWindowed();
            m_Saved.fullscreen = IsFullscreen;
            Store();
        }

        void Update()
        {
            if (m_Duplicate) return;
            if (TogglePressed()) Toggle();
        }

        // ---- the app-facing calls --------------------------------------------------

        /// <summary>Full screen if windowed, windowed if full screen. Ignored while a switch runs.</summary>
        public void Toggle() { SetFullscreen(!IsFullscreen); }

        /// <summary>
        /// Go full screen (on the 3D panel, with <see cref="fullscreenOnPanel"/>) or back
        /// to the remembered window. Returns false when nothing was started: a switch is
        /// already running, the mode is already that, or window modes are not this
        /// component's to change here (see the log line).
        /// </summary>
        public bool SetFullscreen(bool fullscreen)
        {
            if (m_Duplicate || IsSwitching || !isActiveAndEnabled) return false;
            string why = Unsupported();
            if (why != null)
            {
                if (logResult && !m_WarnedUnsupported)
                    Debug.Log("[DisplayXR] WindowMode: not switching - " + why);
                m_WarnedUnsupported = true;
                return false;
            }
            if (fullscreen == IsFullscreen) return false;
            StartCoroutine(Switch(fullscreen));
            return true;
        }

        // Why the window's mode is not this component's to change here, or null when it is.
        static string Unsupported()
        {
            if (Application.isEditor)
                return "the editor's Game view is not a window the player's fullscreen mode applies to. " +
                       "Build a player.";
            var p = Application.platform;
            if (p != RuntimePlatform.WindowsPlayer && p != RuntimePlatform.OSXPlayer && p != RuntimePlatform.LinuxPlayer)
                return "window modes are a desktop feature.";
            try
            {
                // A workspace tile: the shell owns the window, the app has none to change.
                if (DisplayXRNative.displayxr_is_shell_mode() != 0)
                    return "the app runs as a workspace tile; the shell owns its window.";
            }
            catch (System.DllNotFoundException) { }
            catch (System.EntryPointNotFoundException) { }
            // A transparent overlay app: what is on screen is the plug-in's overlay window,
            // and Unity's own window is hidden and parked off screen. Screen.fullScreenMode
            // would change the hidden window and nothing the user sees. Size that app with
            // DisplayXRNative.displayxr_resize_overlay / displayxr_set_overlay_fullscreen.
            if (Object.FindAnyObjectByType<DisplayXRTransparentOverlay>() != null)
                return "this is a transparent overlay app: Unity's own window is hidden, and the overlay " +
                       "window is sized with DisplayXRNative.displayxr_resize_overlay.";
            return null;
        }

        // ---- the switch ------------------------------------------------------------

        IEnumerator Switch(bool toFullscreen)
        {
            IsSwitching = true;
            m_Switches++;
            if (fade) yield return FadeTo(0f);

            if (toFullscreen)
            {
                CaptureWindowed();
                bool onPanel = false;
                if (fullscreenOnPanel && DisplayXRProvider.IsRunning)
                {
                    // Still windowed here: the native move centres the window on the panel
                    // (and fits it), in physical pixels, without a number from this file.
                    onPanel = DisplayXRProvider.MoveWindowToDisplay();
                    // Unity learns which display its window is on from the window's own
                    // messages; give it two frames before asking.
                    yield return null;
                    yield return null;
                }
                DisplayInfo d = Screen.mainWindowDisplayInfo;
                int w = d.width > 0 ? d.width : Screen.currentResolution.width;
                int h = d.height > 0 ? d.height : Screen.currentResolution.height;
                Screen.SetResolution(w, h, FullScreenMode.FullScreenWindow);
                yield return WaitSettled(FullScreenMode.FullScreenWindow);
                if (logResult)
                    Debug.Log($"[DisplayXR] WindowMode: full screen {w}x{h} on '{d.name}'" +
                              (onPanel ? " (the DisplayXR panel)."
                               : fullscreenOnPanel ? " (not moved to the panel: no session, or the runtime " +
                                                     "does not report where the panel is)."
                               : "."));
            }
            else
            {
                yield return ApplyWindowed();
            }

            m_Saved.fullscreen = toFullscreen;
            Store();
            if (fade) yield return FadeTo(1f);
            EndFade();
            IsSwitching = false;
        }

        // Windowed at the remembered size, then back at the remembered position.
        IEnumerator ApplyWindowed()
        {
            int w = m_Saved.hasSize ? m_Saved.width : defaultWindowedSize.x;
            int h = m_Saved.hasSize ? m_Saved.height : defaultWindowedSize.y;
            if (!DisplayXRWindowModeRules.IsPlausibleSize(w, h)) { w = 1280; h = 720; }

            // Not Screen.fullScreen = false: that keeps the full-screen resolution, so the
            // "window" is as large as the monitor and looks like nothing happened.
            if (Screen.fullScreenMode != FullScreenMode.Windowed || Screen.width != w || Screen.height != h)
            {
                Screen.SetResolution(w, h, FullScreenMode.Windowed);
                yield return WaitSettled(FullScreenMode.Windowed);
            }

            string where = "left where the OS put it";
            if (rememberWindow && m_Saved.hasPosition && PositionSupported())
            {
                var layout = new List<DisplayInfo>();
                Screen.GetDisplayLayout(layout);
                var entries = new List<DisplayXRDisplayEntry>(layout.Count);
                foreach (var info in layout) entries.Add(new DisplayXRDisplayEntry(info.name, info.width, info.height));
                int index = DisplayXRWindowModeRules.FindDisplay(m_Saved, entries);
                if (index < 0)
                {
                    where = $"position not restored: display '{m_Saved.display}' " +
                            $"{m_Saved.displayWidth}x{m_Saved.displayHeight} is not connected";
                }
                else
                {
                    DisplayInfo target = layout[index];
                    AsyncOperation move = Screen.MoveMainWindowTo(in target, new Vector2Int(m_Saved.x, m_Saved.y));
                    if (move != null) yield return move;
                    yield return null;
                    yield return null;
                    // A move onto a display with another scale factor makes the OS resize
                    // the window (it keeps its logical size, not its pixel size). Put the
                    // remembered size back; the top-left corner stays.
                    if (Screen.width != w || Screen.height != h)
                    {
                        Screen.SetResolution(w, h, FullScreenMode.Windowed);
                        yield return WaitSettled(FullScreenMode.Windowed);
                    }
                    where = $"at ({m_Saved.x},{m_Saved.y}) on '{target.name}'";
                }
            }
            if (logResult)
                Debug.Log($"[DisplayXR] WindowMode: windowed {w}x{h}, {where}. Now {Screen.width}x{Screen.height} " +
                          $"at ({Screen.mainWindowPosition.x},{Screen.mainWindowPosition.y}) on " +
                          $"'{Screen.mainWindowDisplayInfo.name}'.");
        }

        IEnumerator RestoreAtLaunch()
        {
            // The window exists, but Unity may still be applying its own saved resolution.
            yield return null;
            yield return null;
            // Asked again: a transparent overlay component can be added by a setup script
            // in the first frame.
            if (Unsupported() != null) yield break;
            if (m_Saved.fullscreen)
            {
                if (fullscreenOnPanel)
                {
                    // The panel's place is known once the session runs, a frame or two into Play.
                    float waited = 0f;
                    while (!DisplayXRProvider.IsRunning && waited < 10f)
                    {
                        waited += Time.unscaledDeltaTime;
                        yield return null;
                    }
                }
                // The user (or the app) switched while this waited: theirs stands.
                if (m_Switches != 0 || IsSwitching) yield break;
                if (!IsFullscreen)
                {
                    SetFullscreen(true); // the ordinary switch: capture, move, full screen
                    yield break;
                }
                // Unity already opened full screen. The native move makes a borderless
                // full-screen window cover the panel. An exclusive one is left alone, as
                // DisplayXRTargetDisplay leaves it: it holds the display it is on.
                if (fullscreenOnPanel && DisplayXRProvider.IsRunning &&
                    Screen.fullScreenMode == FullScreenMode.FullScreenWindow)
                {
                    bool onPanel = DisplayXRProvider.MoveWindowToDisplay();
                    if (logResult)
                        Debug.Log("[DisplayXR] WindowMode: launch, full screen" +
                                  (onPanel ? " on the DisplayXR panel." : " (not moved to the panel)."));
                }
            }
            else if (!IsFullscreen && m_Switches == 0 && !IsSwitching)
            {
                IsSwitching = true;
                yield return ApplyWindowed();
                IsSwitching = false;
            }
        }

        // Wait until the window is in `target` mode and has kept one size for settleFrames
        // frames, or settleTimeout has passed. The size is the one the provider renders
        // for; it is only ever compared with itself, frame to frame.
        IEnumerator WaitSettled(FullScreenMode target)
        {
            var settle = new DisplayXRWindowModeRules.Settle();
            float deadline = Time.realtimeSinceStartup + settleTimeout;
            while (Time.realtimeSinceStartup < deadline)
            {
                yield return null;
                if (Screen.fullScreenMode != target) { settle.Reset(); continue; }
                float w, h;
                if (!DisplayXRWindowSpaceUI.TryGetWindowPixelSize(out w, out h)) { w = Screen.width; h = Screen.height; }
                if (settle.Tick(Mathf.RoundToInt(w), Mathf.RoundToInt(h), settleFrames)) yield break;
            }
            if (logResult)
                Debug.LogWarning($"[DisplayXR] WindowMode: the window did not settle in {target} within " +
                                 $"{settleTimeout:F1}s (now {Screen.fullScreenMode}, {Screen.width}x{Screen.height}).");
        }

        // ---- remembered window -----------------------------------------------------

        // Take the windowed size and position as they are now. Only while windowed, and
        // only values that pass the checks: a minimized window reports a position far off
        // every display, and that must not replace a good one.
        void CaptureWindowed()
        {
            if (!rememberWindow || Screen.fullScreenMode != FullScreenMode.Windowed) return;
            int w = Screen.width, h = Screen.height;
            if (!DisplayXRWindowModeRules.IsPlausibleSize(w, h)) return;

            if (PositionSupported())
            {
                DisplayInfo d = Screen.mainWindowDisplayInfo;
                Vector2Int pos = Screen.mainWindowPosition;
                if (!string.IsNullOrEmpty(d.name) &&
                    DisplayXRWindowModeRules.IsPositionReachable(pos.x, pos.y, w, d.width, d.height))
                {
                    var layout = new List<DisplayInfo>();
                    Screen.GetDisplayLayout(layout);
                    m_Saved.hasPosition = true;
                    m_Saved.x = pos.x; m_Saved.y = pos.y;
                    m_Saved.displayIndex = Mathf.Max(0, layout.FindIndex(i => i.Equals(d)));
                    m_Saved.display = d.name;
                    m_Saved.displayWidth = d.width; m_Saved.displayHeight = d.height;
                }
                // Else the position cannot be read right now (minimized, dragged off its
                // display): the last good one is kept, and DisplayXRWindowModeRules.Sanitize
                // drops it when it is not reachable with the new size.
            }
            m_Saved.hasSize = true;
            m_Saved.width = w; m_Saved.height = h;
            m_HasSaved = true;
        }

        void Store()
        {
            if (!rememberWindow) return;
            PlayerPrefs.SetString(PrefsKey, DisplayXRWindowModeRules.Serialize(m_Saved));
            PlayerPrefs.Save();
            m_HasSaved = true;
        }

        /// <summary>Forget the remembered window (the next launch starts as Unity opens it).</summary>
        public static void ForgetSavedWindow()
        {
            PlayerPrefs.DeleteKey(PrefsKey);
            if (s_Instance != null)
            {
                s_Instance.m_Saved = default(DisplayXRSavedWindow);
                s_Instance.m_HasSaved = false;
            }
        }

        // Whether this window can be asked where it is and told where to go. A native
        // Wayland client cannot place its own toplevel, so there the position is neither
        // saved nor restored; the size still is.
        static int s_PositionSupported = -1;
        static bool PositionSupported()
        {
            if (s_PositionSupported >= 0) return s_PositionSupported != 0;
            bool ok = true;
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
            try
            {
                // "wayland:<handle>" for a native-Wayland player, "x11:<hex>" for an X11 one.
                string handle = DisplayXRNative.LinuxWindowHandle();
                if (string.IsNullOrEmpty(handle)) return false; // not known (no session yet): ask again later
                ok = !handle.StartsWith("wayland:", System.StringComparison.Ordinal);
            }
            catch (System.DllNotFoundException) { }
            catch (System.EntryPointNotFoundException) { }
#endif
            s_PositionSupported = ok ? 1 : 0;
            return ok;
        }

        // ---- input -----------------------------------------------------------------

        bool TogglePressed()
        {
            if (toggleKey == KeyCode.None) return false;
#if ENABLE_INPUT_SYSTEM
            var keyboard = Keyboard.current;
            if (keyboard == null) return false;
            if (m_MappedFrom != toggleKey)
            {
                // KeyCode and Key share most names (F11, Space, A ...). One that has no
                // namesake (Return / Enter, Alpha1 / Digit1) is not guessed at.
                m_MappedFrom = toggleKey;
                if (!System.Enum.TryParse(toggleKey.ToString(), out m_MappedKey)) m_MappedKey = Key.None;
                if (m_MappedKey == Key.None && logResult)
                    Debug.LogWarning($"[DisplayXR] WindowMode: toggleKey {toggleKey} has no Input System key of the " +
                                     "same name - call Toggle() from your own binding.");
            }
            return m_MappedKey != Key.None && keyboard[m_MappedKey].wasPressedThisFrame;
#elif ENABLE_LEGACY_INPUT_MANAGER
            return Input.GetKeyDown(toggleKey);
#else
            return false;
#endif
        }

#if ENABLE_INPUT_SYSTEM
        KeyCode m_MappedFrom = KeyCode.None;
        Key m_MappedKey = Key.None;
#endif

        // ---- fade ------------------------------------------------------------------
        // strength 1 = the scene as authored, 0 = flat and black. The ramp is the plug-in's
        // DisplayXRModeSwitch, used for its easing: a request between two modes with the
        // same view count is a plain eased ramp from the current value to the target.

        readonly DisplayXRModeSwitch m_Ramp = new DisplayXRModeSwitch();
        float m_Strength = 1f;
        bool m_Fading;
        DisplayXRDisplay m_DisplayRig;
        DisplayXRCamera m_CameraRig;
        float m_AuthoredIpd = 1f;
        GameObject m_Quad;
        Material m_QuadMaterial;
        Mesh m_QuadMesh;
        bool m_QuadUnavailable;

        IEnumerator FadeTo(float target)
        {
            if (!m_Fading) BeginFade();
            m_Ramp.Configure(fadeSeconds, fadeEasing);
            m_Ramp.Request(0, 2, 0, 2, m_Strength, target);
            do
            {
                bool fire; uint mode;
                ApplyFade(m_Ramp.Update(Time.unscaledDeltaTime, out fire, out mode));
                yield return null;
            } while (m_Ramp.Active);
            ApplyFade(target);
        }

        void BeginFade()
        {
            m_Fading = true;
            m_DisplayRig = null; m_CameraRig = null;
            Camera cam = DisplayXRRigManager.ActiveCamera;
            if (cam == null) return;
            // DisplayXRAutoConvergence writes ipdFactor every frame and takes any other
            // value it finds there for the authored one: leave the strength to it.
            var auto = cam.GetComponent<DisplayXRAutoConvergence>();
            if (auto != null && auto.isActiveAndEnabled && auto.driveStereoStrength) return;
            m_DisplayRig = cam.GetComponent<DisplayXRDisplay>();
            if (m_DisplayRig != null) { m_AuthoredIpd = m_DisplayRig.ipdFactor; return; }
            m_CameraRig = cam.GetComponent<DisplayXRCamera>();
            if (m_CameraRig != null) m_AuthoredIpd = m_CameraRig.ipdFactor;
        }

        void ApplyFade(float strength)
        {
            m_Strength = Mathf.Clamp01(strength);
            if (m_DisplayRig != null) m_DisplayRig.ipdFactor = m_AuthoredIpd * m_Strength;
            else if (m_CameraRig != null) m_CameraRig.ipdFactor = m_AuthoredIpd * m_Strength;
            SetQuadAlpha(1f - m_Strength);
        }

        // Back to the authored state, whatever the ramp was doing.
        void EndFade()
        {
            if (!m_Fading) return;
            m_Fading = false;
            m_Strength = 1f;
            if (m_DisplayRig != null) m_DisplayRig.ipdFactor = m_AuthoredIpd;
            else if (m_CameraRig != null) m_CameraRig.ipdFactor = m_AuthoredIpd;
            m_DisplayRig = null; m_CameraRig = null;
            SetQuadAlpha(0f);
        }

        void SetQuadAlpha(float alpha)
        {
            if (alpha <= 0f)
            {
                if (m_Quad != null) m_Quad.SetActive(false);
                return;
            }
            if (m_Quad == null && !CreateQuad()) return;
            m_QuadMaterial.color = new Color(0f, 0f, 0f, alpha);
            m_Quad.SetActive(true);
            PlaceQuad();
        }

        // A black quad on the rig's zero-parallax plane, drawn over everything. A world
        // object, because it has to be in both eyes' pictures: OnGUI and a screen-space
        // canvas draw into Unity's own window, which the provider's window covers.
        // UI/Default is a built-in shader that every pipeline draws and that renders both
        // eyes in single-pass; its depth test is a material value, so the quad is not
        // hidden by content in front of the screen plane. Sprites/Default is the fallback
        // (depth-tested: content in front of the screen plane stays visible).
        bool CreateQuad()
        {
            if (m_QuadUnavailable) return false;
            Shader shader = Shader.Find("UI/Default");
            bool overEverything = shader != null;
            if (shader == null) shader = Shader.Find("Sprites/Default");
            if (shader == null)
            {
                m_QuadUnavailable = true;
                if (logResult)
                    Debug.LogWarning("[DisplayXR] WindowMode: neither UI/Default nor Sprites/Default is in the build " +
                                     "- fading the stereo strength only, without the black quad.");
                return false;
            }
            m_QuadMaterial = new Material(shader) { name = "DisplayXRWindowModeFade", hideFlags = HideFlags.DontSave };
            if (overEverything)
                m_QuadMaterial.SetFloat("unity_GUIZTestMode", (float)UnityEngine.Rendering.CompareFunction.Always);
            // Last of the transparent range: the highest queue every pipeline still draws.
            m_QuadMaterial.renderQueue = 3100;

            m_QuadMesh = new Mesh { name = "DisplayXRWindowModeFade", hideFlags = HideFlags.DontSave };
            m_QuadMesh.vertices = new[]
            {
                new Vector3(-1f, -1f, 0f), new Vector3(1f, -1f, 0f),
                new Vector3(1f, 1f, 0f), new Vector3(-1f, 1f, 0f),
            };
            m_QuadMesh.uv = new[] { new Vector2(0, 0), new Vector2(1, 0), new Vector2(1, 1), new Vector2(0, 1) };
            m_QuadMesh.colors = new[] { Color.white, Color.white, Color.white, Color.white };
            m_QuadMesh.triangles = new[] { 0, 2, 1, 0, 3, 2 }; // both shaders draw both faces (Cull Off)
            // Never culled: the camera's own frustum is not the one the eyes are drawn with.
            m_QuadMesh.bounds = new Bounds(Vector3.zero, Vector3.one * 1e6f);

            m_Quad = new GameObject("DisplayXRWindowModeFade") { hideFlags = HideFlags.DontSave };
            m_Quad.transform.SetParent(transform, false);
            m_Quad.AddComponent<MeshFilter>().sharedMesh = m_QuadMesh;
            var renderer = m_Quad.AddComponent<MeshRenderer>();
            renderer.sharedMaterial = m_QuadMaterial;
            renderer.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
            renderer.receiveShadows = false;
            m_Quad.SetActive(false);
            return true;
        }

        void LateUpdate()
        {
            if (m_Quad != null && m_Quad.activeSelf) PlaceQuad();
        }

        void PlaceQuad()
        {
            Camera cam = DisplayXRRigManager.ActiveCamera;
            if (cam == null) cam = Camera.main;
            if (cam == null || m_Quad == null) return;

            // A layer the camera draws.
            int layer = cam.gameObject.layer;
            if ((cam.cullingMask & (1 << layer)) == 0)
                for (int i = 0; i < 32; i++)
                    if ((cam.cullingMask & (1 << i)) != 0) { layer = i; break; }
            m_Quad.layer = layer;

            // On the plane the rig holds at zero parallax (always inside both eyes' frusta),
            // else a little in front of the camera. Far larger than the picture either way.
            Vector3 position, forward;
            float distance;
            Vector3 head;
            bool haveHead = DisplayXRProvider.TryGetViewerHead(out head);
            if (DisplayXRProvider.IsRunning && DisplayXRProvider.RigPlaneValid)
            {
                position = DisplayXRProvider.RigPlanePos;
                forward = DisplayXRProvider.RigPlaneForward;
                distance = haveHead ? (position - head).magnitude : 1f;
            }
            else
            {
                forward = cam.transform.forward;
                distance = Mathf.Sqrt(Mathf.Max(cam.nearClipPlane, 1e-3f) * Mathf.Max(cam.farClipPlane, 1e-2f));
                position = cam.transform.position + forward * distance;
            }
            if (forward.sqrMagnitude < 1e-8f) forward = cam.transform.forward;
            Transform t = m_Quad.transform;
            t.SetPositionAndRotation(position, Quaternion.LookRotation(forward, cam.transform.up));
            float half = Mathf.Max(distance, 1e-3f) * 200f;
            Vector3 parent = transform.lossyScale;
            t.localScale = new Vector3(half / Nonzero(parent.x), half / Nonzero(parent.y), 1f);
        }

        static float Nonzero(float v) { return Mathf.Abs(v) < 1e-6f ? 1f : v; }
    }
}
