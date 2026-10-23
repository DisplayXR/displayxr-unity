// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;

namespace DisplayXR
{
    /// <summary>
    /// The decisions of the input fields that copy and paste through the plug-in
    /// (<c>DisplayXRClipboardInputField</c> for TextMeshPro, <c>DisplayXRClipboardLegacyInputField</c>
    /// for UGUI's legacy <c>InputField</c>): when they take Ctrl+C / Ctrl+X / Ctrl+V away from the base
    /// field, which keys those are, and what a copy puts on the clipboard. No UnityEngine types on
    /// purpose: it is unit-tested outside Unity (<c>Tests~/TextClipboard</c>, run in CI).
    /// </summary>
    internal static class DisplayXRTextClipboardRules
    {
        /// <summary>What a key-down is to the clipboard.</summary>
        public enum Key { None, Copy, Cut, Paste }

        // UnityEngine.EventModifiers and UnityEngine.KeyCode values (unchanged since Unity 4).
        public const int ModShift = 1, ModControl = 2, ModAlt = 4;
        public const int KeyC = 99, KeyV = 118, KeyX = 120, KeyInsert = 277;

        /// <summary><c>DisplayXRNative.LinuxClipboardStateSdlCut</c> (that constant is Linux-player only).</summary>
        public const int StateSdlCut = 4;

        /// <summary>
        /// How long a paste waits for the clipboard's text at most, in ms. The plug-in ends a read
        /// itself after about 3 s (<c>Failed</c>); this only backs that up.
        /// </summary>
        public const int PasteGiveUpMs = 5000;

        /// <summary>
        /// True when the field must copy and paste through the plug-in: SDL's data device was cut
        /// (file drops on, after the first focus loss), so <c>GUIUtility.systemCopyBuffer</c> reads stale
        /// text and a write to it can crash the player. <paramref name="clipboardState"/> is
        /// <c>DisplayXRNative.LinuxClipboardState()</c>; 0 (no plug-in clipboard, other platforms)
        /// leaves the base field alone.
        /// </summary>
        public static bool Routes(int clipboardState) => (clipboardState & StateSdlCut) != 0;

        /// <summary>
        /// What a key-down is, by exactly the base fields' own tests (Linux: Control is Ctrl): Control
        /// without Alt and Shift (other modifiers do not matter) with C (copy), X (cut) or V (paste).
        /// With <paramref name="insertKeys"/> (UGUI's legacy <c>InputField</c>) also Control+Insert
        /// (copy) and Shift+Insert without Control and Alt (paste).
        /// </summary>
        public static Key Classify(int modifiers, int keyCode, bool insertKeys)
        {
            bool ctrl = (modifiers & ModControl) != 0;
            bool shift = (modifiers & ModShift) != 0;
            bool alt = (modifiers & ModAlt) != 0;
            bool ctrlOnly = ctrl && !alt && !shift;
            switch (keyCode)
            {
                case KeyC: return ctrlOnly ? Key.Copy : Key.None;
                case KeyX: return ctrlOnly ? Key.Cut : Key.None;
                case KeyV: return ctrlOnly ? Key.Paste : Key.None;
                case KeyInsert:
                    if (!insertKeys)
                        return Key.None;
                    if (ctrlOnly)
                        return Key.Copy;
                    return shift && !ctrl && !alt ? Key.Paste : Key.None;
                default:
                    return Key.None;
            }
        }

        /// <summary>
        /// The text a copy or a cut puts on the clipboard, as the base fields do: the text between
        /// the two selection ends (either order; clamped to the text); "" when nothing is selected and
        /// always in a password field (the base fields then empty the clipboard too).
        /// </summary>
        public static string CopyText(string text, int anchor, int focus, bool password)
        {
            if (password || string.IsNullOrEmpty(text))
                return "";
            int a = Math.Max(0, Math.Min(Math.Min(anchor, focus), text.Length));
            int b = Math.Max(0, Math.Min(Math.Max(anchor, focus), text.Length));
            return text.Substring(a, b - a);
        }
    }
}
