// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Pointer events through the views the viewer actually looks through (#460).
//
// WHY YOU NEED THIS. Unity's own pointer path - PhysicsRaycaster, GraphicRaycaster on a
// world-space canvas, OnMouseDown - builds its ray with Camera.ScreenPointToRay. Under
// DisplayXR the picture is drawn with two off-axis frusta the runtime computes from the
// tracked eyes, and the camera's own projection knows nothing about them, so that ray
// misses what the viewer sees: a button floating behind the display highlights when the
// cursor is next to it, not on it, and the error moves with the head. This component
// casts DisplayXRPicking's ray instead and delivers the usual events:
//
//   1. Read the pointer (mouse or first touch) in the picking screen space.
//   2. DisplayXRPicking.TryScreenPointToRay -> the cyclopean world ray.
//   3. Hit-test 3D colliders (Physics.Raycast) and the graphics of world-space canvases.
//   4. Send enter / exit / down / up / click / drag / scroll to the nearest hit through
//      ExecuteEvents - any IPointer*Handler, Button, Toggle, Slider, ScrollRect - and
//      raise the UnityEvents below.
//
// WORLD-SPACE uGUI. The graphics are hit-tested in world space against the stereo ray,
// then the hit point is handed to uGUI as the screen position at which the canvas's own
// event camera sees that same world point. Everything downstream (masks, alpha hit-test,
// Slider and ScrollRect drags) keeps using the camera it always used and lands on the
// right spot. The canvas's GraphicRaycaster is switched off while this runs (see
// takeOverCanvasRaycasters), otherwise the EventSystem's input module drives the same
// canvas a second time with the wrong ray.
//
// This ships as a SAMPLE, not a plugin component, because input policy is app-owned (the
// same reasoning as Samples~/DefaultInputController and the Window Space UI Mouse
// Router). The plugin's part is DisplayXRPicking; fork this freely.
//
// USAGE: add it to any GameObject in the scene. Leave the fields at their defaults for
// "the active rig camera, every collider, every world-space canvas".

using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Events;
using UnityEngine.EventSystems;
using UnityEngine.UI;
#if ENABLE_INPUT_SYSTEM
using UnityEngine.InputSystem;
#endif

namespace DisplayXR.Samples
{
    [AddComponentMenu("DisplayXR/Samples/Pointer Events (Stereo Picking)")]
    public class DisplayXRPointerEvents : MonoBehaviour
    {
        [System.Serializable] public class GameObjectEvent : UnityEvent<GameObject> { }

        [Header("Ray")]
        [Tooltip("The camera whose picture is being pointed at. Empty = the active DisplayXR " +
                 "rig camera (DisplayXRRigManager.ActiveCamera), else Camera.main.")]
        public Camera pickingCamera;

        [Tooltip("With no DisplayXR session (the editor without a runtime, an unsupported " +
                 "platform) fall back to Camera.ScreenPointToRay so the scene stays usable.")]
        public bool fallBackToCameraRay = true;

        [Header("3D colliders")]
        public bool raycastPhysics = true;
        public LayerMask physicsLayers = Physics.DefaultRaycastLayers;
        public float maxDistance = 1000f;
        public QueryTriggerInteraction triggerInteraction = QueryTriggerInteraction.Ignore;

        [Header("World-space canvases")]
        public bool raycastWorldCanvases = true;

        [Tooltip("The canvases to drive. Empty = every world-space canvas in the scene, " +
                 "found on enable and then once a second (call RefreshCanvases after " +
                 "spawning one to pick it up at once).")]
        public Canvas[] worldCanvases;

        [Tooltip("Disable the GraphicRaycaster of each driven canvas while this component is " +
                 "enabled, so the EventSystem's input module does not ALSO drive it with " +
                 "Unity's own (wrong) ray. They are re-enabled on disable.")]
        public bool takeOverCanvasRaycasters = true;

        [Tooltip("Skip graphics seen from behind, like GraphicRaycaster.ignoreReversedGraphics.")]
        public bool ignoreReversedGraphics = true;

        [Header("Events (the argument is the object under the pointer)")]
        public GameObjectEvent onEnter = new GameObjectEvent();
        public GameObjectEvent onExit = new GameObjectEvent();
        public GameObjectEvent onDown = new GameObjectEvent();
        public GameObjectEvent onUp = new GameObjectEvent();
        public GameObjectEvent onClick = new GameObjectEvent();

        /// <summary>The object under the pointer, or null.</summary>
        public GameObject Hovered { get { return m_Hovered; } }

        /// <summary>True when <see cref="HitPoint"/> / <see cref="HitDistance"/> describe a hit.</summary>
        public bool HasHit { get; private set; }

        /// <summary>World position of the hit under the pointer.</summary>
        public Vector3 HitPoint { get; private set; }

        /// <summary>Distance of the hit along <see cref="PointerRay"/>.</summary>
        public float HitDistance { get; private set; }

        /// <summary>The ray the last hit-test used. Valid while <see cref="HasRay"/>.</summary>
        public Ray PointerRay { get; private set; }

        public bool HasRay { get; private set; }

        /// <summary>True when the last ray came from DisplayXRPicking (false: the camera fallback).</summary>
        public bool RayIsStereoCorrect { get; private set; }

        // A driven canvas (root or nested) and what uGUI needs to know about it.
        struct CanvasEntry
        {
            public Canvas canvas;
            public GraphicRaycaster raycaster; // on the ROOT canvas: supplies the event camera
        }

        readonly List<CanvasEntry> m_Canvases = new List<CanvasEntry>();
        // The GraphicRaycasters switched off by takeOverCanvasRaycasters, to switch back on.
        readonly List<GraphicRaycaster> m_TakenOver = new List<GraphicRaycaster>();
        float m_NextDiscovery;

        EventSystem m_EventSystem;
        PointerEventData m_Data;
        GameObject m_Hovered;
        bool m_WasDown;
        // While a press that started on a canvas is held, the pointer keeps being reported
        // in that canvas's plane, so a Slider or ScrollRect drag follows it off the graphic.
        Transform m_PressPlane;
        Camera m_PressCamera;

        struct Hit
        {
            public GameObject go;
            public float distance;
            public Vector3 point, normal;
            public Vector2 screenPosition; // what PointerEventData.position must be for this hit
            public GraphicRaycaster module;
            public int sortingOrder, depth;
            public bool isGraphic;
        }

        void OnEnable()
        {
            m_EventSystem = EventSystem.current;
            if (m_EventSystem == null)
            {
                // A bare EventSystem with NO input module: this component dispatches through
                // ExecuteEvents itself and only needs one to build PointerEventData and to
                // hold the selection. (A StandaloneInputModule would throw every frame in a
                // project that uses the Input System package only.)
                var es = new GameObject("DisplayXR_EventSystem", typeof(EventSystem));
                m_EventSystem = es.GetComponent<EventSystem>();
            }
            m_Data = new PointerEventData(m_EventSystem);
            RefreshCanvases();
        }

        void OnDisable()
        {
            SetHovered(null);
            Release(null);
            HasRay = HasHit = false;
            for (int i = 0; i < m_TakenOver.Count; i++)
                if (m_TakenOver[i] != null) m_TakenOver[i].enabled = true;
            m_TakenOver.Clear();
            m_Canvases.Clear();
        }

        /// <summary>Re-collect the world-space canvases to drive (after spawning or destroying one).</summary>
        public void RefreshCanvases()
        {
            m_NextDiscovery = Time.unscaledTime + 1f;
            if (!raycastWorldCanvases) return;

            // Resources.FindObjectsOfTypeAll is the one "find them all" call that is neither
            // missing on 2022.3 nor deprecated on Unity 6; it also returns prefab assets and
            // inactive objects, which the scene test below drops.
            Canvas[] found = worldCanvases != null && worldCanvases.Length > 0
                ? worldCanvases
                : Resources.FindObjectsOfTypeAll<Canvas>();
            for (int i = 0; i < found.Length; i++)
            {
                Canvas c = found[i];
                if (c == null || IndexOf(c) >= 0) continue;
                if (!c.gameObject.scene.IsValid() || !c.gameObject.activeInHierarchy) continue;
                Canvas root = c.rootCanvas;
                if (root == null || root.renderMode != RenderMode.WorldSpace) continue;
                // Canvases the plugin renders offscreen for a 2D layer are world-space only as
                // an implementation detail; they have their own input path (the wsui router).
                if (root.GetComponent<DisplayXRWindowSpaceUI>() != null ||
                    root.GetComponent<DisplayXRLocal2D>() != null)
                    continue;

                var entry = new CanvasEntry { canvas = c };
                // uGUI consumers (Slider.OnDrag, ...) find their camera through the event's
                // raycast module, so the root needs a GraphicRaycaster even though it is not
                // the one doing the hit-test.
                entry.raycaster = root.GetComponent<GraphicRaycaster>();
                if (entry.raycaster == null) entry.raycaster = root.gameObject.AddComponent<GraphicRaycaster>();
                if (takeOverCanvasRaycasters)
                {
                    // Every canvas with a raycaster, nested ones included, is a second pointer.
                    TakeOver(entry.raycaster);
                    TakeOver(c.GetComponent<GraphicRaycaster>());
                }
                m_Canvases.Add(entry);
            }
            // Forget destroyed canvases.
            for (int i = m_Canvases.Count - 1; i >= 0; i--)
                if (m_Canvases[i].canvas == null) m_Canvases.RemoveAt(i);
        }

        void TakeOver(GraphicRaycaster raycaster)
        {
            if (raycaster == null || !raycaster.enabled) return;
            raycaster.enabled = false;
            m_TakenOver.Add(raycaster);
        }

        int IndexOf(Canvas c)
        {
            for (int i = 0; i < m_Canvases.Count; i++)
                if (ReferenceEquals(m_Canvases[i].canvas, c)) return i;
            return -1;
        }

        void Update()
        {
            bool autoDiscover = worldCanvases == null || worldCanvases.Length == 0;
            if (raycastWorldCanvases && autoDiscover && Time.unscaledTime >= m_NextDiscovery)
                RefreshCanvases();

            // ---- 1. Pointer ---------------------------------------------------------------
            bool havePointer = TryGetPointer(out Vector2 pointer, out bool down, out Vector2 scroll);

            // A 2D HUD (window-space UI) under the cursor owns the pointer: do not also poke
            // the 3D scene behind it.
            if (DisplayXRWindowSpaceUI.IsCursorOverInteractive && !m_WasDown) havePointer = false;
            // Outside the window there is nothing to point at (a drag in progress carries on).
            if (havePointer && !m_WasDown && DisplayXRPicking.TryGetScreenSize(out Vector2 screen) &&
                (pointer.x < 0f || pointer.y < 0f || pointer.x >= screen.x || pointer.y >= screen.y))
                havePointer = false;

            // ---- 2. Ray -------------------------------------------------------------------
            Camera cam = ResolveCamera();
            Ray ray = default(Ray);
            HasRay = false;
            if (havePointer)
            {
                RayIsStereoCorrect = DisplayXRPicking.TryScreenPointToRay(pointer, cam, out ray);
                HasRay = RayIsStereoCorrect;
                if (!HasRay && fallBackToCameraRay && cam != null && !DisplayXRPicking.IsAvailable)
                {
                    ray = cam.ScreenPointToRay(pointer);
                    HasRay = true;
                }
            }
            if (HasRay) PointerRay = ray;

            // ---- 3. Hit-test --------------------------------------------------------------
            Hit hit = default(Hit);
            bool haveHit = HasRay && Raycast(ray, pointer, cam, out hit);
            HasHit = haveHit;
            if (haveHit) { HitPoint = hit.point; HitDistance = hit.distance; }

            // ---- 4. Events ----------------------------------------------------------------
            m_Data.Reset();
            m_Data.button = PointerEventData.InputButton.Left;
            Vector2 position = haveHit ? hit.screenPosition : pointer;
            // A held press on a canvas: keep reporting the pointer in that canvas's plane.
            if (m_WasDown && m_PressPlane != null && m_PressCamera != null && HasRay &&
                TryPlanePoint(ray, m_PressPlane, out Vector3 onPlane, out float _))
            {
                Vector3 sp = m_PressCamera.WorldToScreenPoint(onPlane);
                if (sp.z > 0f) position = new Vector2(sp.x, sp.y);
            }
            m_Data.delta = position - m_Data.position;
            m_Data.position = position;
            m_Data.scrollDelta = Vector2.zero;
            m_Data.pointerCurrentRaycast = haveHit ? ToRaycastResult(hit) : default(RaycastResult);

            GameObject hovered = haveHit ? hit.go : null;
            SetHovered(hovered);

            if (hovered != null && scroll.sqrMagnitude > 1e-6f)
            {
                m_Data.scrollDelta = scroll;
                ExecuteEvents.ExecuteHierarchy(hovered, m_Data, ExecuteEvents.scrollHandler);
                m_Data.scrollDelta = Vector2.zero;
            }

            down &= havePointer;
            if (down && !m_WasDown) Press(hovered, haveHit ? hit : default(Hit), haveHit);
            else if (down && m_WasDown) Drag();
            else if (!down && m_WasDown) Release(hovered);
            m_WasDown = down;
        }

        Camera ResolveCamera()
        {
            if (pickingCamera != null) return pickingCamera;
            Camera c = DisplayXRRigManager.ActiveCamera;
            return c != null ? c : Camera.main;
        }

        // ---- Hit-testing ------------------------------------------------------------------

        bool Raycast(Ray ray, Vector2 pointer, Camera cam, out Hit best)
        {
            best = default(Hit);
            bool any = false;

            if (raycastPhysics &&
                Physics.Raycast(ray, out RaycastHit ph, maxDistance, physicsLayers, triggerInteraction))
            {
                best.go = ph.collider.gameObject;
                best.distance = ph.distance;
                best.point = ph.point;
                best.normal = ph.normal;
                best.screenPosition = pointer;
                any = true;
            }

            if (!raycastWorldCanvases) return any;
            for (int i = 0; i < m_Canvases.Count; i++)
            {
                CanvasEntry e = m_Canvases[i];
                if (e.canvas == null || !e.canvas.isActiveAndEnabled) continue;
                // The camera uGUI will use for this canvas: the hit is reported to uGUI as the
                // screen position at which THAT camera sees the hit point.
                Camera eventCam = e.raycaster != null ? e.raycaster.eventCamera : cam;
                if (eventCam == null) continue;

                IList<Graphic> graphics = GraphicRegistry.GetRaycastableGraphicsForCanvas(e.canvas);
                for (int g = 0; g < graphics.Count; g++)
                {
                    Graphic graphic = graphics[g];
                    // depth -1 = not drawn by the canvas (same test as GraphicRaycaster).
                    if (graphic == null || !graphic.raycastTarget || graphic.depth == -1 ||
                        graphic.canvasRenderer.cull)
                        continue;
                    RectTransform rt = graphic.rectTransform;
                    if (ignoreReversedGraphics && Vector3.Dot(ray.direction, rt.forward) <= 0f) continue;
                    if (!TryPlanePoint(ray, rt, out Vector3 p, out float t) || t > maxDistance) continue;
                    Vector2 local = rt.InverseTransformPoint(p);
                    if (!rt.rect.Contains(local)) continue;

                    Vector3 sp = eventCam.WorldToScreenPoint(p);
                    if (sp.z <= 0f) continue;
                    var sp2 = new Vector2(sp.x, sp.y);
                    // Masks, CanvasGroups and alpha hit-testing, exactly as uGUI applies them.
                    if (!graphic.Raycast(sp2, eventCam)) continue;

                    int order = e.canvas.sortingOrder;
                    if (any && !IsInFront(t, order, graphic.depth, best)) continue;
                    best.go = graphic.gameObject;
                    best.distance = t;
                    best.point = p;
                    best.normal = -rt.forward;
                    best.screenPosition = sp2;
                    best.module = e.raycaster;
                    best.sortingOrder = order;
                    best.depth = graphic.depth;
                    best.isGraphic = true;
                    any = true;
                }
            }
            return any;
        }

        // Nearer wins. Graphics of one canvas are coplanar, so within a hair the canvas order
        // decides (sortingOrder, then the graphic's depth), as it does on screen.
        static bool IsInFront(float t, int sortingOrder, int depth, Hit best)
        {
            float eps = 1e-4f * Mathf.Max(1f, best.distance);
            if (t < best.distance - eps) return true;
            if (t > best.distance + eps) return false;
            if (!best.isGraphic) return true; // UI lying on a collider's surface
            if (sortingOrder != best.sortingOrder) return sortingOrder > best.sortingOrder;
            return depth > best.depth;
        }

        static bool TryPlanePoint(Ray ray, Transform plane, out Vector3 point, out float t)
        {
            point = default(Vector3);
            t = 0f;
            Vector3 n = plane.forward;
            float denom = Vector3.Dot(ray.direction, n);
            if (Mathf.Abs(denom) < 1e-6f) return false;
            t = Vector3.Dot(plane.position - ray.origin, n) / denom;
            if (t < 0f) return false;
            point = ray.origin + ray.direction * t;
            return true;
        }

        static RaycastResult ToRaycastResult(Hit h)
        {
            return new RaycastResult
            {
                gameObject = h.go,
                module = h.module,
                distance = h.distance,
                worldPosition = h.point,
                worldNormal = h.normal,
                screenPosition = h.screenPosition,
                sortingOrder = h.sortingOrder,
                depth = h.depth,
                index = 0,
            };
        }

        // ---- Event dispatch (the StandaloneInputModule sequence, one left button) ---------

        void SetHovered(GameObject hovered)
        {
            if (ReferenceEquals(m_Hovered, hovered)) return;
            if (m_Hovered != null)
            {
                ExecuteEvents.ExecuteHierarchy(m_Hovered, m_Data, ExecuteEvents.pointerExitHandler);
                onExit.Invoke(m_Hovered);
            }
            m_Hovered = hovered;
            m_Data.pointerEnter = hovered;
            if (hovered != null)
            {
                ExecuteEvents.ExecuteHierarchy(hovered, m_Data, ExecuteEvents.pointerEnterHandler);
                onEnter.Invoke(hovered);
            }
        }

        void Press(GameObject hovered, Hit hit, bool haveHit)
        {
            m_Data.eligibleForClick = true;
            m_Data.delta = Vector2.zero;
            m_Data.dragging = false;
            m_Data.useDragThreshold = true;
            m_Data.pressPosition = m_Data.position;
            m_Data.pointerPressRaycast = m_Data.pointerCurrentRaycast;
            m_PressPlane = null;
            m_PressCamera = null;

            // Clicking elsewhere deselects (an input field loses focus), as in Unity's modules.
            GameObject select = hovered != null ? ExecuteEvents.GetEventHandler<ISelectHandler>(hovered) : null;
            if (m_EventSystem != null && !ReferenceEquals(select, m_EventSystem.currentSelectedGameObject))
                m_EventSystem.SetSelectedGameObject(select, m_Data);

            if (hovered == null) return;
            if (haveHit && hit.isGraphic && hit.module != null)
            {
                m_PressPlane = hovered.transform;
                m_PressCamera = hit.module.eventCamera;
            }

            GameObject pressed = ExecuteEvents.ExecuteHierarchy(hovered, m_Data, ExecuteEvents.pointerDownHandler);
            if (pressed == null) pressed = ExecuteEvents.GetEventHandler<IPointerClickHandler>(hovered);
            m_Data.pointerPress = pressed;
            m_Data.rawPointerPress = hovered;
            m_Data.clickCount = 1;
            m_Data.clickTime = Time.unscaledTime;
            m_Data.pointerDrag = ExecuteEvents.GetEventHandler<IDragHandler>(hovered);
            if (m_Data.pointerDrag != null)
                ExecuteEvents.Execute(m_Data.pointerDrag, m_Data, ExecuteEvents.initializePotentialDrag);
            onDown.Invoke(hovered);
        }

        void Drag()
        {
            if (m_Data.pointerDrag == null) return;
            if (!m_Data.dragging)
            {
                float threshold = m_EventSystem != null ? m_EventSystem.pixelDragThreshold : 10;
                if (m_Data.useDragThreshold &&
                    (m_Data.pressPosition - m_Data.position).sqrMagnitude < threshold * threshold)
                    return;
                ExecuteEvents.Execute(m_Data.pointerDrag, m_Data, ExecuteEvents.beginDragHandler);
                m_Data.dragging = true;
            }
            // A drag that is not the pressed object's own cancels the press (a ScrollRect
            // dragged by one of its buttons does not click the button).
            if (m_Data.pointerPress != null && !ReferenceEquals(m_Data.pointerPress, m_Data.pointerDrag))
            {
                ExecuteEvents.Execute(m_Data.pointerPress, m_Data, ExecuteEvents.pointerUpHandler);
                m_Data.eligibleForClick = false;
                m_Data.pointerPress = null;
                m_Data.rawPointerPress = null;
            }
            ExecuteEvents.Execute(m_Data.pointerDrag, m_Data, ExecuteEvents.dragHandler);
        }

        void Release(GameObject hovered)
        {
            if (m_Data == null) return;
            GameObject raw = m_Data.rawPointerPress;
            if (m_Data.pointerPress != null)
                ExecuteEvents.Execute(m_Data.pointerPress, m_Data, ExecuteEvents.pointerUpHandler);
            GameObject clickHandler = hovered != null
                ? ExecuteEvents.GetEventHandler<IPointerClickHandler>(hovered) : null;
            bool clicked = m_Data.pointerPress != null && m_Data.eligibleForClick &&
                           ReferenceEquals(m_Data.pointerPress, clickHandler);
            if (clicked)
                ExecuteEvents.Execute(m_Data.pointerPress, m_Data, ExecuteEvents.pointerClickHandler);
            else if (m_Data.pointerDrag != null && m_Data.dragging && hovered != null)
                ExecuteEvents.ExecuteHierarchy(hovered, m_Data, ExecuteEvents.dropHandler);

            if (m_Data.pointerDrag != null && m_Data.dragging)
                ExecuteEvents.Execute(m_Data.pointerDrag, m_Data, ExecuteEvents.endDragHandler);

            if (m_WasDown && raw != null) onUp.Invoke(raw);
            // The UnityEvent click needs no handler on the object: released over what was pressed.
            if (m_WasDown && raw != null && ReferenceEquals(raw, hovered) && m_Data.eligibleForClick)
                onClick.Invoke(raw);

            m_Data.eligibleForClick = false;
            m_Data.pointerPress = null;
            m_Data.rawPointerPress = null;
            m_Data.dragging = false;
            m_Data.pointerDrag = null;
            m_PressPlane = null;
            m_PressCamera = null;
            m_WasDown = false;
        }

        // ---- Input ------------------------------------------------------------------------
        //
        // Position: DisplayXRPicking.TryGetPointerPosition reads the mouse through whichever
        // backend the project enabled, and the cursor over the provider's own window where
        // Unity's is hidden (a Windows transparent-overlay app). The first touch replaces it
        // while a finger is down (Android): touch positions are Unity screen pixels too.
        // Button and wheel: the Input System when it is enabled, else the legacy Input Manager.
        // Scroll is returned in uGUI's units (about 1 per notch).

        static bool TryGetPointer(out Vector2 position, out bool down, out Vector2 scroll)
        {
            position = Vector2.zero; down = false; scroll = Vector2.zero;
#if ENABLE_INPUT_SYSTEM
            var ts = Touchscreen.current;
            if (ts != null)
            {
                var touch = ts.primaryTouch;
                if (touch.phase.ReadValue() != UnityEngine.InputSystem.TouchPhase.None)
                {
                    // A finger that lifted this frame still reports its last position, with
                    // the press released, so the release lands where the press was.
                    position = touch.position.ReadValue();
                    down = touch.press.isPressed;
                    return true;
                }
            }
            if (!DisplayXRPicking.TryGetPointerPosition(out position)) return false;
            var mouse = Mouse.current;
            if (mouse != null)
            {
                down = mouse.leftButton.isPressed;
                // Windows reports 120 per notch.
                scroll = mouse.scroll.ReadValue() / 120f;
            }
            return true;
#elif ENABLE_LEGACY_INPUT_MANAGER
            if (Input.touchCount > 0)
            {
                Touch touch = Input.GetTouch(0);
                position = touch.position;
                down = touch.phase != UnityEngine.TouchPhase.Ended && touch.phase != UnityEngine.TouchPhase.Canceled;
                return true;
            }
            if (!DisplayXRPicking.TryGetPointerPosition(out position)) return false;
            down = Input.GetMouseButton(0);
            scroll = Input.mouseScrollDelta;
            return true;
#else
            return false;
#endif
        }
    }
}
