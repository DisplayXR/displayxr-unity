// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Public access to the standalone preview window's input state. Intended
    /// for app-side input routers that drive UI rendered through a window-space
    /// composition layer (`DisplayXRWindowSpaceUI`) — Unity's `Input.mousePosition`
    /// reflects the editor Game view, not the runtime's preview NSWindow / HWND,
    /// so a router needs an OS-level reading of the preview cursor instead.
    ///
    /// Built apps don't need this — when Unity's OpenXR loader is active, the
    /// runtime composites into Unity's main window and `Input.mousePosition`
    /// is already in window-pixel coords.
    /// </summary>
    public static class DisplayXRPreviewInput
    {
        /// <summary>
        /// Cursor position inside the runtime preview window's content area
        /// as FRACTIONAL coords (0..1, top-left origin). Matches the
        /// fractional convention of `DisplayXRWindowSpaceUI.positionX/Y/
        /// width/height`, so app-side input routers can rect-test directly.
        /// </summary>
        /// <param name="fx">Fractional X if cursor is over the content area,
        /// -1 otherwise.</param>
        /// <param name="fy">Fractional Y if cursor is over the content area,
        /// -1 otherwise.</param>
        /// <returns>True if the cursor is over the content area (fx/fy
        /// valid), false otherwise.</returns>
        public static bool TryGetPreviewMousePosition(out float fx, out float fy)
        {
            try
            {
                int ok = DisplayXRNative.displayxr_standalone_get_preview_mouse_position(
                    out fx, out fy);
                return ok != 0;
            }
            catch (System.EntryPointNotFoundException)
            {
                fx = -1f;
                fy = -1f;
                return false;
            }
        }

        /// <summary>
        /// Mouse-button bitmask + accumulated wheel delta from the preview
        /// window. Wheel delta is consumed each call (atomic read+zero).
        /// </summary>
        /// <param name="buttons">Bit 0 = left, 1 = right, 2 = middle.</param>
        /// <param name="wheelDelta">Accumulated wheel delta (raw OS units —
        /// Win32 returns 120 per notch; Mac side currently returns 0).</param>
        public static void GetPreviewMouseState(out int buttons, out int wheelDelta)
        {
            try
            {
                DisplayXRNative.displayxr_standalone_get_preview_mouse_state(
                    out buttons, out wheelDelta);
            }
            catch (System.EntryPointNotFoundException)
            {
                buttons = 0;
                wheelDelta = 0;
            }
        }

        /// <summary>
        /// True if the given ASCII key (letters A–Z or digits 0–9) is
        /// currently held, polled from OS hardware state — works regardless
        /// of which window has keyboard focus. Pass uppercase for letters
        /// (e.g. 'V' = 0x56). Apps should track edges themselves; this is a
        /// raw "is held" reading. Returns false in built apps where Unity's
        /// Input System sees keyboard input directly.
        /// </summary>
        public static bool IsKeyPressed(int ascii)
        {
            try { return DisplayXRNative.displayxr_standalone_is_key_pressed(ascii) != 0; }
            catch (System.EntryPointNotFoundException) { return false; }
        }
    }
}
