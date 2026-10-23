// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DisplayXRTextClipboardRules - the decisions of the input fields that copy and paste
// through the plug-in (DisplayXRClipboardInputField, DisplayXRClipboardLegacyInputField): route or
// leave the base field alone, which key-downs are clipboard keys (exactly the base fields' tests),
// and what a copy puts on the clipboard. Hand-rolled (no test framework) so it also runs from a bare
// `csc` + the .NET runtime that ships inside the Unity editor. Exit code 0 = all passed.

using System;
using DisplayXR;
using K = DisplayXR.DisplayXRTextClipboardRules.Key;

static class Program
{
    static int s_pass, s_fail;

    // UnityEngine.EventModifiers beyond the three the rules test.
    const int Command = 8, Numeric = 16, CapsLock = 32, FunctionKey = 64;
    // A few other UnityEngine.KeyCode values.
    const int KeyA = 97, KeyBackspace = 8, KeyDelete = 127, KeyReturn = 13, KeyLeftArrow = 276;

    const int Ctrl = DisplayXRTextClipboardRules.ModControl;
    const int Shift = DisplayXRTextClipboardRules.ModShift;
    const int Alt = DisplayXRTextClipboardRules.ModAlt;
    const int C = DisplayXRTextClipboardRules.KeyC, X = DisplayXRTextClipboardRules.KeyX;
    const int V = DisplayXRTextClipboardRules.KeyV, Ins = DisplayXRTextClipboardRules.KeyInsert;

    static void Check(bool ok, string what)
    {
        if (ok) s_pass++;
        else { s_fail++; Console.WriteLine("FAIL " + what); }
    }

    static void Expect(int mods, int key, bool insertKeys, K want)
    {
        K got = DisplayXRTextClipboardRules.Classify(mods, key, insertKeys);
        Check(got == want, $"Classify(mods={mods}, key={key}, insertKeys={insertKeys}) = {got}, want {want}");
    }

    static void ExpectCopy(string text, int anchor, int focus, bool password, string want)
    {
        string got = DisplayXRTextClipboardRules.CopyText(text, anchor, focus, password);
        Check(got == want, $"CopyText(\"{text}\", {anchor}, {focus}, password={password}) = \"{got}\", want \"{want}\"");
    }

    static void TestConstants()
    {
        // The Unity enum values the components cast to int (they cannot be referenced here).
        Check(Shift == 1 && Ctrl == 2 && Alt == 4, "EventModifiers Shift 1, Control 2, Alt 4");
        Check(C == 'c' && V == 'v' && X == 'x', "KeyCode C, V, X are their lower-case letters");
        Check(Ins == 277, "KeyCode.Insert 277");
        Check(DisplayXRTextClipboardRules.StateSdlCut == 4, "LinuxClipboardStateSdlCut 4");
        Check(DisplayXRTextClipboardRules.PasteGiveUpMs > 3000, "the paste backstop is later than the plug-in's 3 s");
    }

    static void TestRoutes()
    {
        const int Available = 1, Focused = 2, Cut = 4, Owned = 8;
        Check(!DisplayXRTextClipboardRules.Routes(0), "state 0 (other platforms, X11, older plug-in): base field");
        Check(!DisplayXRTextClipboardRules.Routes(Available), "available, not cut: base field");
        Check(!DisplayXRTextClipboardRules.Routes(Available | Focused), "available + focused, not cut: base field");
        Check(!DisplayXRTextClipboardRules.Routes(Available | Focused | Owned), "the plug-in's text, not cut: base field");
        Check(DisplayXRTextClipboardRules.Routes(Cut), "cut: through the plug-in");
        Check(DisplayXRTextClipboardRules.Routes(Available | Focused | Cut), "cut + focused: through the plug-in");
        Check(DisplayXRTextClipboardRules.Routes(Cut | Owned), "cut, not focused: still never systemCopyBuffer");
        Check(DisplayXRTextClipboardRules.Routes(Available | Focused | Cut | Owned), "all bits: through the plug-in");
    }

    static void TestClassify()
    {
        foreach (bool ins in new[] { false, true })
        {
            Expect(Ctrl, C, ins, K.Copy);
            Expect(Ctrl, X, ins, K.Cut);
            Expect(Ctrl, V, ins, K.Paste);
            // Modifiers the base fields ignore.
            Expect(Ctrl | CapsLock, C, ins, K.Copy);
            Expect(Ctrl | Numeric, V, ins, K.Paste);
            Expect(Ctrl | FunctionKey, X, ins, K.Cut);
            Expect(Ctrl | Command, C, ins, K.Copy); // Linux: Command is the Super key, not tested by the base
            // Not "Control only": the base types or ignores them, so they stay with the base.
            Expect(0, C, ins, K.None);
            Expect(Shift, V, ins, K.None);
            Expect(Ctrl | Shift, C, ins, K.None);
            Expect(Ctrl | Alt, V, ins, K.None);
            Expect(Ctrl | Shift | Alt, X, ins, K.None);
            Expect(Alt, X, ins, K.None);
            Expect(Command, C, ins, K.None);
            // Other keys.
            Expect(Ctrl, KeyA, ins, K.None);
            Expect(Ctrl, KeyBackspace, ins, K.None);
            Expect(0, KeyReturn, ins, K.None);
            Expect(Ctrl, KeyLeftArrow, ins, K.None);
            Expect(Shift, KeyDelete, ins, K.None); // neither base field cuts on Shift+Delete
        }
        // Insert: the legacy InputField only (TMP_InputField has no Insert shortcuts).
        Expect(Ctrl, Ins, false, K.None);
        Expect(Shift, Ins, false, K.None);
        Expect(Ctrl, Ins, true, K.Copy);
        Expect(Ctrl | CapsLock, Ins, true, K.Copy);
        Expect(Shift, Ins, true, K.Paste);
        Expect(Shift | Numeric, Ins, true, K.Paste);
        Expect(0, Ins, true, K.None);
        Expect(Ctrl | Shift, Ins, true, K.None);
        Expect(Shift | Alt, Ins, true, K.None);
        Expect(Ctrl | Alt, Ins, true, K.None);
        Expect(Alt, Ins, true, K.None);
    }

    static void TestCopyText()
    {
        ExpectCopy("hello world", 0, 5, false, "hello");
        ExpectCopy("hello world", 5, 0, false, "hello");  // selected backwards
        ExpectCopy("hello world", 0, 11, false, "hello world");
        ExpectCopy("hello world", 6, 6, false, "");       // no selection: the base empties the clipboard
        ExpectCopy("hello world", 0, 5, true, "");        // password: never the text
        ExpectCopy("hello world", 0, 11, true, "");
        ExpectCopy("", 0, 0, false, "");
        ExpectCopy(null, 0, 3, false, "");
        // UTF-16 code units, as the base fields' string positions.
        ExpectCopy("한글 文字 abc", 0, 2, false, "한글");
        ExpectCopy("한글 文字 abc", 3, 5, false, "文字");
        ExpectCopy("a😀b", 1, 3, false, "😀");
        // Clamped instead of throwing.
        ExpectCopy("abc", 1, 10, false, "bc");
        ExpectCopy("abc", -4, 2, false, "ab");
        ExpectCopy("abc", 7, 9, false, "");
    }

    static int Main()
    {
        TestConstants();
        TestRoutes();
        TestClassify();
        TestCopyText();
        Console.WriteLine($"{s_pass} passed, {s_fail} failed");
        return s_fail == 0 ? 0 : 1;
    }
}
