// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Mouse input router for DisplayXRWindowSpaceUI (#268).
//
// WHY YOU NEED THIS. `XrCompositionLayerWindowSpaceDXR` submits pixels and lets the
// runtime composite them; it does not carry input. DisplayXRWindowSpaceUI takes over
// your Canvas — switches it to WorldSpace, parks it at world (0, 100000, 0) on a
// private layer, and renders it with a hidden offscreen camera into a RenderTexture.
// After that the mouse position in the app window and the overlay camera's screen
// space are two different coordinate systems, so GraphicRaycaster never hits anything
// and your buttons and sliders are dead. This bridges the two:
//
//   1. Read the cursor in fractional window coords.
//   2. Hit-test the wsui layer's fractional rect.
//   3. Map the hit to canvas-pixel coords inside the OverlayTexture.
//   4. Raycast EVERY GraphicRaycaster under the wsui canvas (nested canvases included).
//   5. Synthesize PointerEventData and dispatch enter/exit, scroll, click and drag.
//
// NESTED CANVASES. Anything that brings its own Canvas under the wsui root — a file
// browser, a modal dialog, a dropdown's blocker — comes with its own GraphicRaycaster,
// and the root raycaster never sees those graphics. Two things go wrong at once: the
// nested graphics are never hit-tested (the dialog is dead to clicks), and a nested
// WorldSpace canvas does NOT inherit the root's worldCamera, so its raycaster falls back
// to Camera.main and projects with the wrong camera. The router collects every
// raycaster under the wsui, points every nested canvas at the overlay camera, and
// orders hits by sortingOrder (dialogs override-sort above the app UI) then depth.
//
// This ships as a SAMPLE, not a plugin component, because input policy is app-owned
// (same reasoning as Samples~/DefaultInputController). Fork it freely — if your input
// model isn't a mouse, steps 1 and 5 are the only parts you need to replace.
//
// USAGE: drop it on the same GameObject as your DisplayXRWindowSpaceUI, or anywhere
// above it in the hierarchy. Assign `windowSpaceUI` explicitly if the automatic
// GetComponentInChildren lookup can't find it.

using System.Collections.Generic;
using DisplayXR;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.InputSystem;
using UnityEngine.UI;

namespace DisplayXR.Samples
{
    [AddComponentMenu("DisplayXR/Samples/Window Space UI Mouse Router")]
    public class DisplayXRWsuiMouseRouter : MonoBehaviour
    {
        [Tooltip("The window-space UI to route input to. Left empty, the router searches " +
                 "this GameObject and its children each frame until it finds one.")]
        public DisplayXRWindowSpaceUI windowSpaceUI;

        private DisplayXRWindowSpaceUI m_Wsui;
        private EventSystem m_EventSystem;

        // Every GraphicRaycaster under the wsui canvas, re-collected each frame so
        // canvases spawned at runtime (dialogs, popups) are picked up without wiring.
        private readonly List<GraphicRaycaster> m_Raycasters = new List<GraphicRaycaster>();
        private readonly List<RaycastResult> m_Hits = new List<RaycastResult>();
        private readonly List<RaycastResult> m_TempHits = new List<RaycastResult>();

        private GameObject m_PressTarget;
        private GameObject m_Hovered;
        private PointerEventData m_PointerData;
        private Vector2 m_LastCanvasPos;
        private bool m_LeftDown;

        void OnEnable()
        {
            m_EventSystem = EventSystem.current;
            if (m_EventSystem == null)
            {
                // A bare EventSystem with NO input module. On projects using the Input
                // System Package, StandaloneInputModule.UpdateModule throws every frame
                // trying to read legacy UnityEngine.Input. This router synthesizes events
                // through ExecuteEvents directly, so it needs no input module at all — only
                // a non-null EventSystem.current for PointerEventData construction.
                var es = new GameObject("DisplayXR_EventSystem", typeof(EventSystem));
                m_EventSystem = es.GetComponent<EventSystem>();
            }
            else
            {
                var legacy = m_EventSystem.GetComponent<StandaloneInputModule>();
                if (legacy != null)
                {
                    Debug.Log("[wsui-router] Removing StandaloneInputModule from the existing " +
                              "EventSystem to silence Input-System exceptions.");
                    if (Application.isPlaying) Destroy(legacy); else DestroyImmediate(legacy);
                }
            }
            m_PointerData = new PointerEventData(m_EventSystem);
        }

        void OnDisable()
        {
            // Leave no Selectable stuck in its highlighted/pressed state, and release the
            // input claim so scene controllers resume.
            SetHovered(null);
            ReleaseIfDown();
            DisplayXRWindowSpaceUI.IsCursorOverInteractive = false;
        }

        void Update()
        {
            if (m_Wsui == null)
            {
                m_Wsui = windowSpaceUI != null
                    ? windowSpaceUI
                    : GetComponentInChildren<DisplayXRWindowSpaceUI>();
                if (m_Wsui == null) return;
                // A canvas authored without a raycaster gets one on the root so it is
                // hit-testable at all; nested canvases bring their own.
                if (m_Wsui.GetComponent<GraphicRaycaster>() == null)
                    m_Wsui.gameObject.AddComponent<GraphicRaycaster>();
            }

            // ---- 1. Cursor in fractional window coords -------------------------
            if (!TryGetWindowMouseFractional(out Vector2 windowFrac))
            {
                DisplayXRWindowSpaceUI.IsCursorOverInteractive = false;
                SetHovered(null);
                ReleaseIfDown();
                return;
            }

            // ---- 2. Hit-test the wsui layer rect (also fractional, top-left origin)
            if (windowFrac.x < m_Wsui.positionX || windowFrac.x > m_Wsui.positionX + m_Wsui.width ||
                windowFrac.y < m_Wsui.positionY || windowFrac.y > m_Wsui.positionY + m_Wsui.height)
            {
                DisplayXRWindowSpaceUI.IsCursorOverInteractive = m_PressTarget != null;
                SetHovered(null);
                ReleaseIfDown();
                return;
            }

            // ---- 3. Map to RT-pixel coords (== screen coords for OverlayCamera) -
            float panelFracX = (windowFrac.x - m_Wsui.positionX) / m_Wsui.width;
            float panelFracY = (windowFrac.y - m_Wsui.positionY) / m_Wsui.height;
            // No extra flip here: OverlayCamera's flipped up-vector already inverts
            // ScreenPointToRay's Y, so fracY 0 (top of the layer) maps to screenY 0.
            var canvasPos = new Vector2(panelFracX * m_Wsui.resolution.x,
                                        panelFracY * m_Wsui.resolution.y);

            // ---- 4. Raycast every raycaster under the wsui canvas ---------------
            m_PointerData.Reset();
            m_PointerData.position = canvasPos;
            m_PointerData.delta = canvasPos - m_LastCanvasPos;
            m_PointerData.scrollDelta = Vector2.zero;
            m_PointerData.button = PointerEventData.InputButton.Left;
            m_PointerData.pressPosition = m_LeftDown ? m_PointerData.pressPosition : canvasPos;

            RaycastAll();
            var hovered = m_Hits.Count > 0 ? m_Hits[0].gameObject : null;

            // Claim input so scene controllers pause (a slider drag must not also rotate
            // the camera) — but only over an ACTUAL graphic, or while a press is held.
            // The recommended 2D-scene recipe is a full-rect wsui (position 0,0 / size
            // 1,1), and "anywhere inside the layer rect" would then be the whole window,
            // blocking orbit/pan/zoom everywhere. The plugin's own doc comment on the flag
            // says "hovering or a press is held over a wsui-rendered UI element"; this
            // matches it.
            DisplayXRWindowSpaceUI.IsCursorOverInteractive = hovered != null || m_PressTarget != null;

            // pressEventCamera / enterEventCamera are read-only in Unity 6's UGUI — they
            // derive from pointerCurrentRaycast.module / pointerPressRaycast.module. Wiring
            // the raycast results is what makes consumers such as Slider.OnDrag's
            // ScreenPointToLocalPointInRectangle resolve OverlayCamera and project correctly.
            m_PointerData.pointerCurrentRaycast = m_Hits.Count > 0 ? m_Hits[0] : default(RaycastResult);

            // ---- 5a. Enter / exit — Selectable hover highlights, tooltips ---------
            SetHovered(hovered);

            // ---- 5b. Scroll wheel — ScrollRect lists, dropdowns ------------------
            var mouse = Mouse.current;
            if (hovered != null && mouse != null)
            {
                Vector2 scroll = mouse.scroll.ReadValue();
                if (scroll.sqrMagnitude > 0.001f)
                {
                    // Windows reports 120 per notch; UGUI's ScrollRect expects roughly
                    // 1 per notch (it multiplies by scrollSensitivity itself).
                    m_PointerData.scrollDelta = scroll / 120f;
                    ExecuteEvents.ExecuteHierarchy(hovered, m_PointerData, ExecuteEvents.scrollHandler);
                    m_PointerData.scrollDelta = Vector2.zero;
                }
            }

            // ---- 5c. Click / drag -------------------------------------------------
            bool nowDown = IsLeftDown();
            if (!m_LeftDown && nowDown && hovered != null)
            {
                m_PointerData.pointerPressRaycast = m_PointerData.pointerCurrentRaycast;
                m_PressTarget = ExecuteEvents.ExecuteHierarchy(
                    hovered, m_PointerData, ExecuteEvents.pointerDownHandler)
                    ?? ExecuteEvents.GetEventHandler<IPointerClickHandler>(hovered);
                ExecuteEvents.Execute(m_PressTarget, m_PointerData, ExecuteEvents.beginDragHandler);
                m_PointerData.pressPosition = canvasPos;
            }
            else if (m_LeftDown && nowDown && m_PressTarget != null)
            {
                ExecuteEvents.Execute(m_PressTarget, m_PointerData, ExecuteEvents.dragHandler);
            }
            else if (m_LeftDown && !nowDown)
            {
                ExecuteEvents.Execute(m_PressTarget, m_PointerData, ExecuteEvents.endDragHandler);
                ExecuteEvents.Execute(m_PressTarget, m_PointerData, ExecuteEvents.pointerUpHandler);
                // Click if the press target handles clicks AND the cursor is still either
                // over that hierarchy or somewhere inside the panel. Unity's stricter rule
                // (current raycast must resolve to the press target) is too brittle here,
                // where a pixel of jitter at release would silently drop the click.
                var clickHandler = ExecuteEvents.GetEventHandler<IPointerClickHandler>(m_PressTarget);
                bool overPressHierarchy = hovered != null &&
                    ExecuteEvents.GetEventHandler<IPointerClickHandler>(hovered) == clickHandler;
                if (clickHandler != null && (overPressHierarchy || m_Hits.Count > 0))
                    ExecuteEvents.Execute(clickHandler, m_PointerData, ExecuteEvents.pointerClickHandler);
                m_PressTarget = null;
            }

            m_LeftDown = nowDown;
            m_LastCanvasPos = canvasPos;
        }

        // Raycast every GraphicRaycaster under the wsui canvas into m_Hits, best hit first.
        private void RaycastAll()
        {
            m_Hits.Clear();
            m_Wsui.GetComponentsInChildren(false, m_Raycasters);
            Camera overlayCam = m_Wsui.GetComponent<Canvas>().worldCamera;
            for (int i = 0; i < m_Raycasters.Count; i++)
            {
                var rc = m_Raycasters[i];
                // The wsui's OverlayCamera has up = Vector3.down and looks toward -Z to
                // Y-flip the rendered RT (the runtime's texture origin is top-left), which
                // makes Dot(camera.forward, canvas.forward) == -1. GraphicRaycaster's
                // ignoreReversedGraphics reads that as "back of the graphic faces the
                // camera" and skips every hit — so it must be off, on EVERY raycaster
                // (a nested canvas arrives with Unity's default of true).
                rc.ignoreReversedGraphics = false;
                // A nested WorldSpace canvas does not inherit the root's worldCamera, and
                // GraphicRaycaster then falls back to Camera.main — the wrong projection.
                // Point every canvas under the wsui at the overlay camera.
                var c = rc.GetComponent<Canvas>();
                if (c != null && overlayCam != null && c.worldCamera != overlayCam)
                    c.worldCamera = overlayCam;

                m_TempHits.Clear();
                rc.Raycast(m_PointerData, m_TempHits);
                m_Hits.AddRange(m_TempHits);
            }
            // Cross-canvas ordering: higher sortingOrder wins (dialogs override-sort
            // above the app UI), then higher graphic depth within a canvas — the same
            // order GraphicRaycaster uses within one canvas.
            m_Hits.Sort(CompareHits);
        }

        private static int CompareHits(RaycastResult a, RaycastResult b)
        {
            if (a.sortingOrder != b.sortingOrder)
                return b.sortingOrder.CompareTo(a.sortingOrder);
            return b.depth.CompareTo(a.depth);
        }

        private void SetHovered(GameObject hovered)
        {
            if (m_Hovered == hovered)
                return;
            if (m_Hovered != null && m_PointerData != null)
                ExecuteEvents.ExecuteHierarchy(m_Hovered, m_PointerData, ExecuteEvents.pointerExitHandler);
            m_Hovered = hovered;
            if (m_Hovered != null && m_PointerData != null)
            {
                m_PointerData.pointerEnter = m_Hovered;
                ExecuteEvents.ExecuteHierarchy(m_Hovered, m_PointerData, ExecuteEvents.pointerEnterHandler);
            }
        }

        // Where the cursor is read from depends on where the woven output actually IS.
        //
        //  * Built player, and the editor's default weave-to-texture path (v2.8.0+): the
        //    output is inside Unity's own window / Game view, so Unity's Input System
        //    tracks the cursor over it normally.
        //  * Editor with DISPLAYXR_PROV_EXTERNAL_WINDOW=1: the output is a separate
        //    dedicated window that Unity's input never sees, so the cursor has to come
        //    from the overlay window's own WndProc tracker.
        //
        // Getting this backwards is silent — the panel simply never responds — so the
        // branch is on the same flag the provider itself uses, not on UNITY_EDITOR alone.
        static bool UseNativeOverlayPointer()
        {
#if UNITY_EDITOR
            return !DisplayXRProviderDriver.GameViewTextureModeEnabled();
#else
            return false;
#endif
        }

        private bool TryGetWindowMouseFractional(out Vector2 frac)
        {
            if (UseNativeOverlayPointer())
            {
                try
                {
                    DisplayXRNative.displayxr_get_overlay_pointer(out int cx, out int cy, out int _);
                    DisplayXRNative.displayxr_get_overlay_size(out int ow, out int oh);
                    if (ow > 0 && oh > 0 && cx >= 0 && cy >= 0 && cx < ow && cy < oh)
                    {
                        frac = new Vector2((float)cx / ow, (float)cy / oh);
                        return true;
                    }
                }
                catch (System.EntryPointNotFoundException) { /* older native binary */ }
                frac = Vector2.zero;
                return false;
            }

            // Input System Package: legacy UnityEngine.Input returns zeros when
            // activeInputHandler is "Input System Package (New)". Mouse.current.position is
            // bottom-left origin; wsui rects are top-left, hence the 1 - y.
            var mouse = Mouse.current;
            if (mouse == null || Screen.width <= 0 || Screen.height <= 0)
            {
                frac = Vector2.zero;
                return false;
            }
            var pos = mouse.position.ReadValue();
            if (pos.x < 0 || pos.x >= Screen.width || pos.y < 0 || pos.y >= Screen.height)
            {
                frac = Vector2.zero;
                return false;
            }
            frac = new Vector2(pos.x / Screen.width, 1f - pos.y / Screen.height);
            return true;
        }

        private bool IsLeftDown()
        {
            if (UseNativeOverlayPointer())
            {
                try
                {
                    DisplayXRNative.displayxr_get_overlay_pointer(out int _, out int _, out int buttons);
                    return (buttons & 0x1) != 0;
                }
                catch (System.EntryPointNotFoundException) { return false; }
            }
            var mouse = Mouse.current;
            return mouse != null && mouse.leftButton.isPressed;
        }

        private void ReleaseIfDown()
        {
            if (m_LeftDown && m_PressTarget != null)
            {
                ExecuteEvents.Execute(m_PressTarget, m_PointerData, ExecuteEvents.endDragHandler);
                ExecuteEvents.Execute(m_PressTarget, m_PointerData, ExecuteEvents.pointerUpHandler);
                m_PressTarget = null;
            }
            m_LeftDown = false;
        }
    }
}
