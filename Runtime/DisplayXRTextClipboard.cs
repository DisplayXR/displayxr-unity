// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections;
using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// The plug-in's clipboard as the DisplayXR input fields use it
    /// (<c>DisplayXRClipboardInputField</c>, <c>DisplayXRClipboardLegacyInputField</c>). Linux player
    /// only: everywhere else (Windows, macOS, the editor) <see cref="Routes"/> is false and the fields
    /// are exactly their base class. Clipboard text never reaches a log.
    /// </summary>
    internal static class DisplayXRTextClipboard
    {
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
        static bool s_NoPlugin;
#endif

        /// <summary>
        /// True when Ctrl+C / Ctrl+X / Ctrl+V must go through the plug-in, never through
        /// <c>GUIUtility.systemCopyBuffer</c>: SDL's data device was cut
        /// (<see cref="DisplayXRTextClipboardRules.Routes"/>). Cheap; the fields ask on every
        /// <c>OnUpdateSelected</c>.
        /// </summary>
        public static bool Routes
        {
            get
            {
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
                if (s_NoPlugin)
                    return false;
                try
                {
                    return DisplayXRTextClipboardRules.Routes(DisplayXRNative.LinuxClipboardState());
                }
                catch (DllNotFoundException)
                {
                    s_NoPlugin = true; // no plug-in: file drops were never on, nothing was cut
                    return false;
                }
#else
                return false;
#endif
            }
        }

        /// <summary>
        /// Puts <paramref name="text"/> on the clipboard through the plug-in. False, with one warning
        /// naming the result (never the text), when it was not done.
        /// </summary>
        public static bool Write(string text, string who)
        {
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
            DisplayXRNative.LinuxClipboardResult r = DisplayXRNative.SetLinuxClipboardText(text);
            if (r == DisplayXRNative.LinuxClipboardResult.Done)
                return true;
            Debug.LogWarning($"[DisplayXR] {who}: the copy did not reach the clipboard ({r})");
#endif
            return false;
        }

        /// <summary>
        /// A coroutine that reads the clipboard's text through the plug-in without blocking a frame:
        /// it asks once per frame until the text is in, then calls <paramref name="done"/> with it
        /// ("" when the clipboard holds no text). When the read fails (the plug-in ends one after about
        /// 3 s; <see cref="DisplayXRTextClipboardRules.PasteGiveUpMs"/> backs that up) it logs one line
        /// naming the result and calls <paramref name="done"/> with null.
        /// </summary>
        public static IEnumerator Read(Action<string> done, string who)
        {
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
            float giveUp = Time.realtimeSinceStartup + DisplayXRTextClipboardRules.PasteGiveUpMs / 1000f;
            for (;;)
            {
                // Timeout 0: only ever poll, on whichever thread the plug-in takes this call for.
                DisplayXRNative.LinuxClipboardResult r = DisplayXRNative.LinuxClipboardText(out string text, 0);
                if (r == DisplayXRNative.LinuxClipboardResult.Done)
                {
                    done(text ?? "");
                    yield break;
                }
                bool pending = r == DisplayXRNative.LinuxClipboardResult.Pending;
                if (!pending || Time.realtimeSinceStartup > giveUp)
                {
                    Debug.Log($"[DisplayXR] {who}: nothing pasted, the clipboard read ended with " +
                              (pending ? $"no text in {DisplayXRTextClipboardRules.PasteGiveUpMs} ms" : r.ToString()));
                    done(null);
                    yield break;
                }
                yield return null;
            }
#else
            done(null);
            yield break;
#endif
        }
    }
}
