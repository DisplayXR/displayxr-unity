// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DisplayXRTextClipboardRules - the decisions of the input fields that copy and paste
// through the plug-in (DisplayXRClipboardInputField, DisplayXRClipboardLegacyInputField): route or
// leave the base field alone, which key-downs are clipboard keys (exactly the base fields' tests),
// and what a copy puts on the clipboard. Hand-rolled (no test framework) so it also runs from a bare
// `csc` + the .NET runtime that ships inside the Unity editor. Exit code 0 = all passed.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
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

    // ---- Runtime/TMP/com.displayxr.unity.tmp.asmdef: which TextMeshPro gets which copy of the event loop ----
    // DisplayXRClipboardInputField copies TMP_InputField.OnUpdateSelected. After Enter / Escape (EditState.Finish)
    // TextMeshPro 3.0.x goes on with the frame's other events, and every TextMeshPro from 3.2.0-pre.1 on (the 3.2 / 4.0
    // pre-releases a 2022.3 project can pick, and the one inside ugui 2.0+ in Unity 6) leaves the loop. The copy
    // follows DISPLAYXR_TMP_32, so that define must be on for exactly those versions.

    static string FindUp(string rel)
    {
        foreach (string start in new[] { Directory.GetCurrentDirectory(), AppContext.BaseDirectory })
        {
            for (DirectoryInfo d = new DirectoryInfo(start); d != null; d = d.Parent)
            {
                string p = Path.Combine(d.FullName, rel);
                if (File.Exists(p))
                    return p;
            }
        }
        return null;
    }

    // Semantic-version order, as Unity's package manager (pre-release identifiers: numeric ones as numbers).
    static int CompareVersion(string a, string b)
    {
        string[] ap = a.Split(new[] { '-' }, 2), bp = b.Split(new[] { '-' }, 2);
        string[] ac = ap[0].Split('.'), bc = bp[0].Split('.');
        for (int i = 0; i < 3; i++)
        {
            int c = int.Parse(ac[i]).CompareTo(int.Parse(bc[i]));
            if (c != 0)
                return c;
        }
        if (ap.Length == 1 || bp.Length == 1)
            return (ap.Length == 1 ? 1 : 0) - (bp.Length == 1 ? 1 : 0); // a release is newer than its pre-releases
        string[] ai = ap[1].Split('.'), bi = bp[1].Split('.');
        for (int i = 0; i < Math.Min(ai.Length, bi.Length); i++)
        {
            bool an = int.TryParse(ai[i], out int av), bn = int.TryParse(bi[i], out int bv);
            int c = an && bn ? av.CompareTo(bv) : an ? -1 : bn ? 1 : string.CompareOrdinal(ai[i], bi[i]);
            if (c != 0)
                return c;
        }
        return ai.Length.CompareTo(bi.Length);
    }

    static void ExpectDefines(List<string[]> defs, string ugui, string tmp, bool wantTmp, bool want32)
    {
        var on = new HashSet<string>();
        foreach (string[] d in defs)
        {
            string have = d[0] == "com.unity.ugui" ? ugui : d[0] == "com.unity.textmeshpro" ? tmp : null;
            if (have != null && CompareVersion(have, d[1]) >= 0)
                on.Add(d[2]);
        }
        Check(on.Contains("DISPLAYXR_TMP") == wantTmp && on.Contains("DISPLAYXR_TMP_32") == want32,
              $"ugui {ugui}, textmeshpro {tmp ?? "-"}: defines [{string.Join(", ", on)}], want DISPLAYXR_TMP={wantTmp}, " +
              $"DISPLAYXR_TMP_32={want32}");
    }

    static void TestTmpVersionDefines()
    {
        string path = FindUp(Path.Combine("Runtime", "TMP", "com.displayxr.unity.tmp.asmdef"));
        Check(path != null, "Runtime/TMP/com.displayxr.unity.tmp.asmdef found");
        if (path == null)
            return;
        var defs = new List<string[]>();
        foreach (Match m in Regex.Matches(File.ReadAllText(path),
                     "\\{\\s*\"name\":\\s*\"([^\"]+)\",\\s*\"expression\":\\s*\"([^\"]*)\",\\s*\"define\":\\s*\"([^\"]+)\"\\s*\\}"))
        {
            // Only the plain "at least this version" form is used (and modelled here).
            Check(Regex.IsMatch(m.Groups[2].Value, @"^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$"),
                  $"version define {m.Groups[3].Value} for {m.Groups[1].Value}: a plain minimum version");
            defs.Add(new[] { m.Groups[1].Value, m.Groups[2].Value, m.Groups[3].Value });
        }
        Check(defs.Count >= 3, $"{defs.Count} version defines read");
        Check(CompareVersion("3.2.0-pre.15", "3.2.0-pre.1") > 0 && CompareVersion("3.2.0-pre.1", "3.0.9") > 0 &&
              CompareVersion("3.2.0", "3.2.0-pre.15") > 0 && CompareVersion("4.0.0-pre.2", "3.2.0") > 0,
              "semantic-version order");
        // 2022.3 LTS: ugui 1.0.0 + the TextMeshPro package.
        ExpectDefines(defs, "1.0.0", "3.0.6", true, false);
        ExpectDefines(defs, "1.0.0", "3.0.9", true, false);
        ExpectDefines(defs, "1.0.0", "3.2.0-pre.1", true, true);
        ExpectDefines(defs, "1.0.0", "3.2.0-pre.15", true, true);
        ExpectDefines(defs, "1.0.0", "4.0.0-pre.2", true, true);
        // Unity 6: TextMeshPro inside ugui 2.0+.
        ExpectDefines(defs, "2.0.0", null, true, true);
        ExpectDefines(defs, "2.0.0", "3.2.0-pre.15", true, true);
        // No TextMeshPro at all: the assembly is not compiled.
        ExpectDefines(defs, "1.0.0", null, false, false);
    }

    static void ExpectChars(string text, int caret, int bytes, bool before, int want)
    {
        int got = DisplayXRTextClipboardRules.ImeCharsForBytes(text, caret, bytes, before);
        Check(got == want, $"ImeCharsForBytes(\"{text}\", {caret}, {bytes}, before={before}) = {got}, want {want}");
    }

    static void TestImeCharsForBytes()
    {
        // The IME's delete_surrounding_text counts UTF-8 bytes around the caret (#415).
        ExpectChars("abc", 3, 2, true, 2);
        ExpectChars("abc", 1, 1, false, 1);
        ExpectChars("abc", 0, 0, true, 0);
        ExpectChars("", 0, 3, true, 0);
        ExpectChars("\u00e9", 1, 2, true, 1);                  // 2-byte
        ExpectChars("\uac00\ub098\ub2e4", 3, 3, true, 1);     // Hangul: 3 bytes each
        ExpectChars("\uac00\ub098\ub2e4", 3, 6, true, 2);
        ExpectChars("\uac00\ub098\ub2e4", 1, 6, false, 2);
        ExpectChars("a\uac00", 2, 4, true, 2);
        ExpectChars("x\ud83d\ude00", 3, 4, true, 2);           // a surrogate pair is one 4-byte code point
        ExpectChars("\ud83d\ude00x", 0, 4, false, 2);
        ExpectChars("\ud83d\ude00x", 0, 5, false, 3);
        ExpectChars("ab", 2, 10, true, 2);                      // never past the text's start
        ExpectChars("ab", 0, 10, false, 2);                     // ... or end
        ExpectChars("ab", 5, 1, true, 1);                       // caret clamped to the text
        ExpectChars("ab", -3, 1, false, 1);
        ExpectChars("a\ud800b", 2, 3, true, 1);                 // a lone surrogate: its 3-byte replacement
    }

    static int Main()
    {
        TestConstants();
        TestRoutes();
        TestClassify();
        TestCopyText();
        TestImeCharsForBytes();
        TestTmpVersionDefines();
        Console.WriteLine($"{s_pass} passed, {s_fail} failed");
        return s_fail == 0 ? 0 : 1;
    }
}
