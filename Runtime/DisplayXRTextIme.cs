// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Text;
using UnityEngine;
using UnityEngine.EventSystems;

namespace DisplayXR
{
    /// <summary>
    /// IME composition for the DisplayXR input fields (<c>DisplayXRClipboardInputField</c>,
    /// <c>DisplayXRClipboardLegacyInputField</c>) in a native-Wayland Linux player.
    /// <para>Unity's player drops the IME's composition (zwp_text_input_v3 preedit) and hands a
    /// committed string to the Input System only, cut to its first character; UGUI and TextMeshPro
    /// fields never see it (#415). The main plug-in's SDL fixups (fix "ime",
    /// native~/displayxr_linux_sdl_fixups.c) record the protocol events instead. While a DisplayXR
    /// field has the keyboard focus it owns them: they no longer reach Unity, the composition shows
    /// in the field (the fields' own IME display, fed through a <see cref="BaseInput"/> override)
    /// and each commit is inserted once, in full.</para>
    /// <para>Everywhere else (X11 / XWayland, a Unity player build the fixups do not know,
    /// <c>DISPLAYXR_SDL_FIXUPS</c> without "ime", Windows, macOS, the editor) <see cref="Available"/>
    /// is false and the fields are exactly their base class. Typed text never reaches a log.</para>
    /// </summary>
    internal static class DisplayXRTextIme
    {
        /// <summary>A change the IME made, in the protocol's order: the composition goes, the text
        /// around the caret is deleted, the commit is inserted, the new composition shows.</summary>
        internal struct Change
        {
            public string Commit;        // null: none
            public string Composition;   // "": none
            public int DeleteBeforeBytes, DeleteAfterBytes;
        }

        const uint k_TextInput = 16, k_Owned = 32;

        static object s_Owner;
        static string s_Composition = "";
#pragma warning disable CS0414 // set and read on the Linux player only
        static bool s_NoLibrary;
#pragma warning restore CS0414
        static ImeInput s_Input;
        static BaseInput s_PreviousOverride;
        static BaseInputModule s_Module;
        static readonly byte[] s_Commit = new byte[4096], s_Preedit = new byte[4096];
        static readonly int[] s_Info = new int[6];

        /// <summary>The composition the owning field shows ("" when none).</summary>
        public static string Composition => s_Owner != null ? s_Composition : "";

        /// <summary>True in a native-Wayland Linux player once the plug-in's SDL fixups have seen an
        /// event of SDL's text input (a known Unity player build, the fix on).</summary>
        public static bool Available
        {
            get
            {
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
                if (s_NoLibrary)
                    return false;
                try
                {
                    return (State() & k_TextInput) != 0;
                }
                catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException)
                {
                    s_NoLibrary = true;
                    return false;
                }
#else
                return false;
#endif
            }
        }

        public static bool Owns(object field) => field != null && s_Owner == field;

        /// <summary>The focused field takes the IME. False when it is not available.</summary>
        public static bool Acquire(object field)
        {
            if (s_Owner == field)
                return true;
            if (!Available)
                return false;
            if (s_Owner != null)
                Release(s_Owner);
            if ((Own(1) & k_Owned) == 0)
                return false;
            s_Owner = field;
            s_Composition = "";
            InstallInput();
            return true;
        }

        /// <summary>The field gives the IME back, after putting <see cref="Composition"/> into its text
        /// or dropping it. The IME is reset; its own commit of that composition (an IME such as
        /// ibus-hangul flushes it on the reset) is dropped, so it never reaches the next field.</summary>
        public static void Release(object field)
        {
            if (s_Owner == null || s_Owner != field)
                return;
            s_Owner = null;
            s_Composition = "";
            RemoveInput();
            try
            {
                ReleaseNative(1);
            }
            catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException)
            {
                s_NoLibrary = true;
            }
        }

        /// <summary>The next change for the owning field, oldest first.</summary>
        public static bool Next(object field, out Change change)
        {
            change = default;
            if (s_Owner == null || s_Owner != field)
                return false;
            int r = NextNative(s_Commit, s_Commit.Length, s_Preedit, s_Preedit.Length, s_Info);
            if (r < 0)
            {
                // Longer than 4 KiB: read it with buffers of the size the library told.
                var c = new byte[Math.Max(1, s_Info[4] + 1)];
                var p = new byte[Math.Max(1, s_Info[5] + 1)];
                r = NextNative(c, c.Length, p, p.Length, s_Info);
                if (r != 1)
                    return false;
                change.Commit = s_Info[4] >= 0 ? Encoding.UTF8.GetString(c, 0, s_Info[4]) : null;
                change.Composition = s_Info[5] > 0 ? Encoding.UTF8.GetString(p, 0, s_Info[5]) : "";
            }
            else if (r == 1)
            {
                change.Commit = s_Info[4] >= 0 ? Encoding.UTF8.GetString(s_Commit, 0, s_Info[4]) : null;
                change.Composition = s_Info[5] > 0 ? Encoding.UTF8.GetString(s_Preedit, 0, s_Info[5]) : "";
            }
            else
            {
                return false;
            }
            change.DeleteBeforeBytes = s_Info[2];
            change.DeleteAfterBytes = s_Info[3];
            return true;
        }

        /// <summary>What the owning field shows as the composition (set by the field while it
        /// applies a <see cref="Change"/>, so the base class's caret arithmetic sees the right length).</summary>
        public static void SetComposition(object field, string composition)
        {
            if (s_Owner != null && s_Owner == field)
                s_Composition = composition ?? "";
        }

        /// <summary>How many UTF-16 chars the protocol's UTF-8 byte count covers, walking from
        /// <paramref name="caret"/> in <paramref name="text"/> backwards (<paramref name="before"/>)
        /// or forwards.</summary>
        public static int CharsForBytes(string text, int caret, int bytes, bool before)
        {
            int chars = 0, done = 0, i = Mathf.Clamp(caret, 0, text.Length);
            while (done < bytes && (before ? i > 0 : i < text.Length))
            {
                int len = 1;
                if (before && i >= 2 && char.IsLowSurrogate(text[i - 1]) && char.IsHighSurrogate(text[i - 2]))
                    len = 2;
                else if (!before && i + 1 < text.Length && char.IsHighSurrogate(text[i]) && char.IsLowSurrogate(text[i + 1]))
                    len = 2;
                done += Encoding.UTF8.GetByteCount(text.Substring(before ? i - len : i, len));
                i += before ? -len : len;
                chars += len;
            }
            return chars;
        }

        // The fields read the composition through the input module's BaseInput (their own IME
        // display code); this one answers with ours while a field owns the IME.
        sealed class ImeInput : BaseInput
        {
            public override string compositionString => s_Owner != null ? s_Composition : base.compositionString;
        }

        static void InstallInput()
        {
            BaseInputModule module = EventSystem.current ? EventSystem.current.currentInputModule : null;
            if (!module)
                return;
            if (!s_Input)
            {
                var go = new GameObject("DisplayXR IME") { hideFlags = HideFlags.HideAndDontSave };
                UnityEngine.Object.DontDestroyOnLoad(go);
                s_Input = go.AddComponent<ImeInput>();
            }
            if (module.inputOverride == s_Input)
                return;
            s_Module = module;
            s_PreviousOverride = module.inputOverride;
            module.inputOverride = s_Input;
        }

        static void RemoveInput()
        {
            if (s_Module && s_Module.inputOverride == s_Input)
                s_Module.inputOverride = s_PreviousOverride;
            s_Module = null;
            s_PreviousOverride = null;
        }

#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
        static uint State() => DisplayXRNative.displayxr_linux_ime_state();
        static uint Own(int own) => DisplayXRNative.displayxr_linux_ime_own(own);
        static uint ReleaseNative(int dropEcho) => DisplayXRNative.displayxr_linux_ime_release(dropEcho);
        static int NextNative(byte[] commit, int commitCap, byte[] preedit, int preeditCap, int[] info) =>
            DisplayXRNative.displayxr_linux_ime_next(commit, commitCap, preedit, preeditCap, info);
#else
        static uint State() => 0;
        static uint Own(int own) => 0;
        static uint ReleaseNative(int dropEcho) => 0;
        static int NextNative(byte[] commit, int commitCap, byte[] preedit, int preeditCap, int[] info) => 0;
#endif
    }
}
