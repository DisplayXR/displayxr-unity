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
//   4. Synthesize PointerEventData and dispatch click/drag to UI Selectables.
//
// This ships as a SAMPLE, not a plugin component, because input policy is app-owned
// (same reasoning as Samples~/DefaultInputController). Fork it freely — if your input
// model isn't a mouse, steps 1 and 4 are the only parts you need to replace.
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
        private GraphicRaycaster m_Raycaster;
        private EventSystem m_EventSystem;

        private GameObject m_PressTarget;
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

        void Update()
        {
            if (m_Wsui == null)
            {
                m_Wsui = windowSpaceUI != null
                    ? windowSpaceUI
                    : GetComponentInChildren<DisplayXRWindowSpaceUI>();
                if (m_Wsui == null) return;
            }
            if (m_Raycaster == null)
            {
                m_Raycaster = m_Wsui.GetComponent<GraphicRaycaster>()
                              ?? m_Wsui.gameObject.AddComponent<GraphicRaycaster>();
                // The wsui's OverlayCamera has up = Vector3.down and looks toward -Z to
                // Y-flip the rendered RT (the runtime's texture origin is top-left), which
                // makes Dot(camera.forward, canvas.forward) == -1. GraphicRaycaster's
                // ignoreReversedGraphics reads that as "back of the graphic faces the
                // camera" and skips every hit — so it must be off.
                m_Raycaster.ignoreReversedGraphics = false;
            }

            // ---- 1. Cursor in fractional window coords -------------------------
            if (!TryGetWindowMouseFractional(out Vector2 windowFrac))
            {
                DisplayXRWindowSpaceUI.IsCursorOverInteractive = false;
                ReleaseIfDown();
                return;
            }

            // ---- 2. Hit-test the wsui layer rect (also fractional, top-left origin)
            if (windowFrac.x < m_Wsui.positionX || windowFrac.x > m_Wsui.positionX + m_Wsui.width ||
                windowFrac.y < m_Wsui.positionY || windowFrac.y > m_Wsui.positionY + m_Wsui.height)
            {
                DisplayXRWindowSpaceUI.IsCursorOverInteractive = m_PressTarget != null;
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

            // ---- 4. Synthesize PointerEventData and dispatch --------------------
            m_PointerData.Reset();
            m_PointerData.position = canvasPos;
            m_PointerData.delta = canvasPos - m_LastCanvasPos;
            m_PointerData.scrollDelta = Vector2.zero;
            m_PointerData.button = PointerEventData.InputButton.Left;
            m_PointerData.pressPosition = m_LeftDown ? m_PointerData.pressPosition : canvasPos;

            var hits = new List<RaycastResult>();
            m_Raycaster.Raycast(m_PointerData, hits);
            var hovered = hits.Count > 0 ? hits[0].gameObject : null;

            // The cursor is inside the wsui rect: claim input so scene controllers pause
            // (a slider drag must not also rotate the camera). True even over empty space
            // inside the panel — that matches what a user means by "in the UI".
            DisplayXRWindowSpaceUI.IsCursorOverInteractive = true;

            // pressEventCamera / enterEventCamera are read-only in Unity 6's UGUI — they
            // derive from pointerCurrentRaycast.module / pointerPressRaycast.module. Wiring
            // the raycast results is what makes consumers such as Slider.OnDrag's
            // ScreenPointToLocalPointInRectangle resolve OverlayCamera and project correctly.
            m_PointerData.pointerCurrentRaycast = hits.Count > 0 ? hits[0] : default(RaycastResult);

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
                if (clickHandler != null && (overPressHierarchy || hits.Count > 0))
                    ExecuteEvents.Execute(clickHandler, m_PointerData, ExecuteEvents.pointerClickHandler);
                m_PressTarget = null;
            }

            m_LeftDown = nowDown;
            m_LastCanvasPos = canvasPos;
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
