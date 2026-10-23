// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;

namespace DisplayXR
{
    /// <summary>
    /// The decisions of the input fields that copy and paste through the plug-in
    /// (<c>DisplayXRClipboardInputField</c> for TextMeshPro, <c>DisplayXRClipboardLegacyInputField</c>
    /// for UGUI's legacy <c>InputField</c>): when they take Ctrl+C / Ctrl+X / Ctrl+V away from the base
    /// field, which keys those are, what a copy puts on the clipboard, and the IME's byte counts in
    /// UTF-16 chars (<see cref="ImeCharsForBytes"/>). No UnityEngine types on
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

        /// <summary>
        /// The IME's delete-around-the-caret (zwp_text_input_v3 delete_surrounding_text, #415) counts
        /// UTF-8 bytes; the fields' text is UTF-16. How many UTF-16 chars <paramref name="bytes"/> UTF-8
        /// bytes cover, walking from <paramref name="caret"/> (clamped to the text) backwards
        /// (<paramref name="before"/>) or forwards: a code point at a time (a surrogate pair is one, 4
        /// bytes; a lone surrogate counts as its 3-byte replacement), never past the text's ends.
        /// </summary>
        public static int ImeCharsForBytes(string text, int caret, int bytes, bool before)
        {
            if (string.IsNullOrEmpty(text) || bytes <= 0)
                return 0;
            int i = Math.Max(0, Math.Min(caret, text.Length)), chars = 0, done = 0;
            while (done < bytes && (before ? i > 0 : i < text.Length))
            {
                int len = 1, utf8;
                if (before && i >= 2 && char.IsLowSurrogate(text[i - 1]) && char.IsHighSurrogate(text[i - 2]))
                    len = 2;
                else if (!before && i + 1 < text.Length && char.IsHighSurrogate(text[i]) && char.IsLowSurrogate(text[i + 1]))
                    len = 2;
                if (len == 2)
                    utf8 = 4;
                else
                {
                    char c = text[before ? i - 1 : i];
                    utf8 = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
                }
                done += utf8;
                i += before ? -len : len;
                chars += len;
            }
            return chars;
        }
    }
}
