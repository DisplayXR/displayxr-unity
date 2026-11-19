// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DisplayXRRenderScaleMath - the managed rules of the render scale (#475).
// Hand-rolled (no test framework), like the other Tests~ projects. Exit code 0 = all passed.
//
// The native twin (native~/tests/render_scale_test.cpp) covers the sizes; what is pinned
// here is what only the managed side decides: which of the app's value and the settings
// asset's value is pushed, and that the file a build writes for a player reads back the
// same on a machine with a different number format.

using System;
using System.Globalization;
using System.Threading;
using DisplayXR;

static class Program
{
    static int s_pass, s_fail;

    static void Check(bool ok, string what)
    {
        if (ok) s_pass++;
        else { s_fail++; Console.WriteLine("FAIL " + what); }
    }

    static bool Near(float a, float b) => Math.Abs(a - b) <= 1e-6f;

    static void TestSanitize()
    {
        Check(DisplayXRRenderScaleMath.Sanitize(float.NaN) == 0f, "NaN is unset");
        Check(DisplayXRRenderScaleMath.Sanitize(float.PositiveInfinity) == 0f, "+inf is unset");
        Check(DisplayXRRenderScaleMath.Sanitize(float.NegativeInfinity) == 0f, "-inf is unset");
        Check(DisplayXRRenderScaleMath.Sanitize(0f) == 0f, "0 is unset");
        Check(DisplayXRRenderScaleMath.Sanitize(-0.5f) == 0f, "negative is unset");
        Check(DisplayXRRenderScaleMath.Sanitize(1f) == 1f, "1 stays 1");
        Check(DisplayXRRenderScaleMath.Sanitize(0.5f) == 0.5f, "0.5 stays");
        Check(DisplayXRRenderScaleMath.Sanitize(1.5f) == 1.5f, "1.5 stays");
        Check(DisplayXRRenderScaleMath.Sanitize(0.01f) == DisplayXRRenderScaleMath.Min, "below the range clamps up");
        Check(DisplayXRRenderScaleMath.Sanitize(8f) == DisplayXRRenderScaleMath.Max, "above the range clamps down");
        Check(DisplayXRRenderScaleMath.Sanitize(DisplayXRRenderScaleMath.Min) == 0.25f, "min is 0.25");
        Check(DisplayXRRenderScaleMath.Sanitize(DisplayXRRenderScaleMath.Max) == 2f, "max is 2");
        // Rounded to 1/100, so slider / float noise around 1 is exactly 1 (the identity).
        Check(DisplayXRRenderScaleMath.Sanitize(1.004f) == 1f, "1.004 -> 1");
        Check(DisplayXRRenderScaleMath.Sanitize(0.996f) == 1f, "0.996 -> 1");
        Check(Near(DisplayXRRenderScaleMath.Sanitize(0.757f), 0.76f), "0.757 -> 0.76");
        Check(Near(DisplayXRRenderScaleMath.Sanitize(0.504f), 0.5f), "0.504 -> 0.5");
        // Idempotent: what was sanitized once is taken as is.
        foreach (float v in new[] { 0.25f, 0.33f, 0.5f, 0.77f, 1f, 1.25f, 1.99f, 2f })
            Check(DisplayXRRenderScaleMath.Sanitize(DisplayXRRenderScaleMath.Sanitize(v)) ==
                  DisplayXRRenderScaleMath.Sanitize(v), "idempotent at " + v.ToString(CultureInfo.InvariantCulture));

        Check(!DisplayXRRenderScaleMath.IsSet(0f), "0 is not set");
        Check(!DisplayXRRenderScaleMath.IsSet(float.NaN), "NaN is not set");
        Check(DisplayXRRenderScaleMath.IsSet(0.1f), "0.1 is set (and will clamp)");
        Check(DisplayXRRenderScaleMath.IsSet(1f), "1 is set - an explicit 1 overrides Unity's knob");
    }

    static void TestResolveOverride()
    {
        Check(DisplayXRRenderScaleMath.ResolveOverride(0f, 0f) == 0f, "nothing set -> unset (Unity decides)");
        Check(DisplayXRRenderScaleMath.ResolveOverride(0f, 0.75f) == 0.75f, "settings alone");
        Check(DisplayXRRenderScaleMath.ResolveOverride(0.5f, 0f) == 0.5f, "app alone");
        Check(DisplayXRRenderScaleMath.ResolveOverride(0.5f, 1.5f) == 0.5f, "app wins over settings");
        Check(DisplayXRRenderScaleMath.ResolveOverride(1f, 0.5f) == 1f, "an explicit app 1 wins over settings");
        Check(DisplayXRRenderScaleMath.ResolveOverride(float.NaN, 0.75f) == 0.75f, "a NaN app value is no app value");
        Check(DisplayXRRenderScaleMath.ResolveOverride(-1f, 0.75f) == 0.75f, "a cleared app value falls back");
        Check(DisplayXRRenderScaleMath.ResolveOverride(9f, 0.75f) == 2f, "the app value is clamped, not dropped");
        Check(DisplayXRRenderScaleMath.ResolveOverride(0f, 9f) == 2f, "the settings value is clamped too");
        Check(DisplayXRRenderScaleMath.ResolveOverride(0f, float.NaN) == 0f, "a NaN settings value is unset");
    }

    static void TestPlayerSettingsFile()
    {
        Check(DisplayXRRenderScaleMath.PlayerSettingsFile == "DisplayXR/player_settings.json", "file name is the contract");

        float v;
        // Round trip, including "not set".
        foreach (float s in new[] { 0f, 0.25f, 0.5f, 0.75f, 1f, 1.25f, 1.33f, 2f })
        {
            string json = DisplayXRRenderScaleMath.ToPlayerSettingsJson(s);
            Check(DisplayXRRenderScaleMath.TryParsePlayerSettingsJson(json, out v) && Near(v, s),
                  "round trip " + s.ToString(CultureInfo.InvariantCulture));
        }
        Check(DisplayXRRenderScaleMath.ToPlayerSettingsJson(0.75f) == "{\n  \"render_scale\": 0.75\n}\n", "exact text");
        Check(DisplayXRRenderScaleMath.ToPlayerSettingsJson(0f) == "{\n  \"render_scale\": 0\n}\n", "unset is written as 0");
        Check(DisplayXRRenderScaleMath.ToPlayerSettingsJson(float.NaN) == "{\n  \"render_scale\": 0\n}\n", "NaN is written as 0");
        Check(DisplayXRRenderScaleMath.ToPlayerSettingsJson(7f) == "{\n  \"render_scale\": 2\n}\n", "out of range is written clamped");

        // A build machine or a player with a decimal COMMA must not change the file.
        CultureInfo saved = Thread.CurrentThread.CurrentCulture;
        try
        {
            Thread.CurrentThread.CurrentCulture = new CultureInfo("de-DE");
            string json = DisplayXRRenderScaleMath.ToPlayerSettingsJson(0.75f);
            Check(json == "{\n  \"render_scale\": 0.75\n}\n", "written with a decimal point under de-DE");
            Check(DisplayXRRenderScaleMath.TryParsePlayerSettingsJson(json, out v) && Near(v, 0.75f), "read under de-DE");
            Thread.CurrentThread.CurrentCulture = new CultureInfo("fr-FR");
            Check(DisplayXRRenderScaleMath.TryParsePlayerSettingsJson("{\"render_scale\": 1.5}", out v) && Near(v, 1.5f),
                  "read under fr-FR");
        }
        finally { Thread.CurrentThread.CurrentCulture = saved; }

        // Tolerant of layout and of other keys around it.
        Check(DisplayXRRenderScaleMath.TryParsePlayerSettingsJson("{\"render_scale\":0.5}", out v) && v == 0.5f, "compact");
        Check(DisplayXRRenderScaleMath.TryParsePlayerSettingsJson("{\r\n\t\"other\": 3,\r\n\t\"render_scale\" :   1.25 ,\r\n\t\"more\": true\r\n}", out v)
              && v == 1.25f, "CRLF, tabs, other keys");
        Check(DisplayXRRenderScaleMath.TryParsePlayerSettingsJson("{\"render_scale\": 5e-1}", out v) && v == 0.5f, "exponent");
        Check(DisplayXRRenderScaleMath.TryParsePlayerSettingsJson("{\"render_scale\": 40}", out v) && v == 2f, "clamped on read");

        // Anything else is "nothing stored", never a wrong number.
        Check(!DisplayXRRenderScaleMath.TryParsePlayerSettingsJson(null, out v) && v == 0f, "null");
        Check(!DisplayXRRenderScaleMath.TryParsePlayerSettingsJson("", out v) && v == 0f, "empty");
        Check(!DisplayXRRenderScaleMath.TryParsePlayerSettingsJson("{}", out v) && v == 0f, "no key");
        Check(!DisplayXRRenderScaleMath.TryParsePlayerSettingsJson("{\"render_scale\": \"big\"}", out v) && v == 0f, "a string");
        Check(!DisplayXRRenderScaleMath.TryParsePlayerSettingsJson("{\"render_scale\": }", out v) && v == 0f, "no value");
        Check(!DisplayXRRenderScaleMath.TryParsePlayerSettingsJson("{\"render_scale\"", out v) && v == 0f, "truncated");
        Check(!DisplayXRRenderScaleMath.TryParsePlayerSettingsJson("{\"render_scale\": -1}", out v) && v == 0f, "negative");
        Check(!DisplayXRRenderScaleMath.TryParsePlayerSettingsJson("{\"render_scale\": 1.2.3}", out v) && v == 0f, "malformed number");
        Check(!DisplayXRRenderScaleMath.TryParsePlayerSettingsJson("{\"render_scales\": 0.5}", out v) && v == 0f, "a different key");
    }

    static int Main()
    {
        TestSanitize();
        TestResolveOverride();
        TestPlayerSettingsFile();
        Console.WriteLine($"render_scale (C#): {s_pass} passed, {s_fail} failed");
        return s_fail == 0 ? 0 : 1;
    }
}
