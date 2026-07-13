// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEditor;
using UnityEngine;
using DisplayXR;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Closing the provider's dedicated Play-Mode preview window (its X button or
    /// Alt+F4) stops Play Mode — a property of the plugin, so no project has to wire
    /// it. The preview window is the plugin's own window, so its close obviously
    /// means "stop the preview."
    ///
    /// The native dedicated-window WndProc (displayxr_win32.c `dedicated_wnd_proc`
    /// WM_CLOSE) swallows the close — it must NOT DestroyWindow the live weave
    /// target — and raises a close-request flag. Here we poll that flag each editor
    /// tick during Play Mode and exit Play Mode, which tears the window down cleanly
    /// via the subsystem's LifecycleStop.
    ///
    /// Built players keep app-owned quit policy: the app polls the SAME flag
    /// (`displayxr_consume_overlay_close_request`) and decides `Application.Quit`.
    /// App pollers early-return in the editor (`Application.isEditor`), so this
    /// editor-only poll is the sole consumer in Play Mode — no double-consume race.
    /// </summary>
    /// <remarks>
    /// Windows-editor-only: the dedicated weave window + its close-request flag are
    /// win32 concepts (`displayxr_consume_overlay_close_request` is inside the
    /// UNITY_STANDALONE_WIN block of DisplayXRNative). On macOS the runtime
    /// self-hosts its window (#204) — close policy there is a Phase 4 topic (#206).
    /// Without this gate the whole Editor assembly failed to compile on macOS.
    /// </remarks>
#if UNITY_EDITOR_WIN
    [InitializeOnLoad]
#endif
    internal static class DisplayXRPreviewClose
    {
#if UNITY_EDITOR_WIN
        static DisplayXRPreviewClose()
        {
            EditorApplication.update += Poll;
        }

        static void Poll()
        {
            // Only while the provider is actually driving a Play-Mode preview.
            if (!EditorApplication.isPlaying) return;
            if (!DisplayXRProviderDriver.IsActive) return;

            int closed;
            try
            {
                // Consumes (reads + clears) the native close-request flag.
                closed = DisplayXRNative.displayxr_consume_overlay_close_request();
            }
            catch (System.EntryPointNotFoundException)
            {
                return; // older native plugin without the export — nothing to do
            }

            if (closed != 0)
            {
                Debug.Log("[DisplayXR] Preview window closed — stopping Play Mode.");
                EditorApplication.isPlaying = false;
            }
        }
#endif // UNITY_EDITOR_WIN
    }
}
