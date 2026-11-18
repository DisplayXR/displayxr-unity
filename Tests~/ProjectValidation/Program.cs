// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for project validation's pure C# half (#462): the rule registry, evaluation,
// the ignore-list file, and the decisions behind the built-in rules. Hand-rolled (no test
// framework) like the other Tests~ projects. Exit code 0 = all passed.
//
// What is NOT covered here, because it needs the Unity editor: reading the facts out of a
// project (XR Plug-in Management, Player Settings, the scenes), the fixes, and the window.

using System;
using System.Collections.Generic;
using System.IO;
using DisplayXR.Editor;

using Api = DisplayXR.Editor.DisplayXRGraphicsApiRules;
using Platforms = DisplayXR.Editor.DisplayXRValidationPlatforms;
using Result = DisplayXR.Editor.DisplayXRValidationResult;
using Severity = DisplayXR.Editor.DisplayXRValidationSeverity;
using Status = DisplayXR.Editor.DisplayXRValidationStatus;

static class Program
{
    static int s_pass, s_fail;

    static void Check(bool ok, string what)
    {
        if (ok) s_pass++;
        else { s_fail++; Console.WriteLine("FAIL " + what); }
    }

    static bool Throws<T>(Action a) where T : Exception
    {
        try { a(); } catch (T) { return true; } catch (Exception) { return false; }
        return false;
    }

    static DisplayXRValidationRule Rule(string id, Severity severity = Severity.Warning,
        Func<DisplayXRValidationContext, Result> check = null, int order = 0,
        Platforms platforms = Platforms.Supported)
    {
        return new DisplayXRValidationRule
        {
            Id = id, Title = "Title of " + id, Explanation = "Why " + id, Severity = severity,
            Order = order, Platforms = platforms,
            Check = check ?? (c => Result.Pass()),
        };
    }

    static DisplayXRValidationContext Ctx(Platforms p)
    {
        return new DisplayXRValidationContext { Platform = p };
    }

    static string Ids(IEnumerable<DisplayXRValidationRule> rules)
    {
        var ids = new List<string>();
        foreach (var r in rules) ids.Add(r.Id);
        return string.Join(",", ids);
    }

    // ---- registry ---------------------------------------------------------------

    static void TestRegistry()
    {
        DisplayXRValidationRegistry.Clear();
        int changed = 0;
        Action onChanged = () => changed++;
        DisplayXRValidationRegistry.Changed += onChanged;

        DisplayXRValidationRegistry.Register(Rule("b", order: 10));
        DisplayXRValidationRegistry.Register(Rule("a", order: 10));
        DisplayXRValidationRegistry.Register(Rule("first", order: 1));
        // Another assembly (the URP one) registering later, with a higher order.
        DisplayXRValidationRegistry.Register(Rule("urp-x", order: 200));
        Check(Ids(DisplayXRValidationRegistry.Rules) == "first,b,a,urp-x",
              "rules sort by Order, then registration order (got " + Ids(DisplayXRValidationRegistry.Rules) + ")");
        Check(changed == 4, "Changed fires once per registration");

        // Same id again replaces in place: no duplicate row, position kept.
        var replacement = Rule("b", Severity.Error, order: 10);
        DisplayXRValidationRegistry.Register(replacement);
        Check(DisplayXRValidationRegistry.Rules.Count == 4, "re-registering an id does not add a row");
        Check(ReferenceEquals(DisplayXRValidationRegistry.Find("b"), replacement), "re-registering replaces the rule");
        Check(Ids(DisplayXRValidationRegistry.Rules) == "first,b,a,urp-x", "a replaced rule keeps its place");

        Check(DisplayXRValidationRegistry.Unregister("a"), "Unregister returns true for a known id");
        Check(!DisplayXRValidationRegistry.Unregister("a"), "Unregister returns false for an unknown id");
        Check(DisplayXRValidationRegistry.Find("a") == null, "an unregistered rule is gone");

        // The snapshot is a copy: registering while iterating it is safe.
        var snapshot = DisplayXRValidationRegistry.Rules;
        DisplayXRValidationRegistry.Register(Rule("late"));
        Check(snapshot.Count == 3 && DisplayXRValidationRegistry.Rules.Count == 4, "Rules returns a snapshot");

        Check(Throws<ArgumentNullException>(() => DisplayXRValidationRegistry.Register(null)), "null rule is rejected");
        Check(Throws<ArgumentException>(() => DisplayXRValidationRegistry.Register(Rule("Bad Id"))), "malformed id is rejected");
        Check(Throws<ArgumentException>(() => DisplayXRValidationRegistry.Register(
            new DisplayXRValidationRule { Id = "no-title", Check = c => Result.Pass() })), "missing Title is rejected");
        Check(Throws<ArgumentException>(() => DisplayXRValidationRegistry.Register(
            new DisplayXRValidationRule { Id = "no-check", Title = "t" })), "missing Check is rejected");
        Check(DisplayXRValidationRegistry.Rules.Count == 4, "a rejected rule is not added");

        DisplayXRValidationRegistry.Changed -= onChanged;
        DisplayXRValidationRegistry.Clear();
    }

    static void TestIds()
    {
        foreach (var good in new[] { "loader-enabled", "a", "9lives", "com.acme.rule-1", "urp-preserve-framebuffer-alpha" })
            Check(DisplayXRValidationRegistry.IsValidId(good), "valid id: " + good);
        foreach (var bad in new[] { "", null, "-lead", ".lead", "Upper", "has space", "under_score", "quote\"", "a/b",
                                    new string('a', 65) })
            Check(!DisplayXRValidationRegistry.IsValidId(bad), "invalid id: " + (bad ?? "(null)"));
        Check(DisplayXRValidationRegistry.IsValidId(new string('a', 64)), "64 characters is the longest valid id");
    }

    // ---- evaluation -------------------------------------------------------------

    static void TestEvaluate()
    {
        int androidRuns = 0;
        var rules = new List<DisplayXRValidationRule>
        {
            Rule("pass"),
            Rule("err", Severity.Error, c => Result.Fail("broken")),
            Rule("warn", Severity.Warning, c => Result.Fail("meh")),
            Rule("info-override", Severity.Error, c => Result.Fail("fyi", Severity.Info, noFix: true)),
            Rule("android-only", Severity.Error, c => { androidRuns++; return Result.Fail("x"); }, platforms: Platforms.Android),
            Rule("throws", Severity.Info, c => { throw new InvalidOperationException("boom"); }),
            Rule("na", Severity.Error, c => Result.NotApplicable("nothing to say")),
            Rule("ignored-err", Severity.Error, c => Result.Fail("known")),
        };
        foreach (var r in rules) r.Fix = c => { };
        var ignored = new HashSet<string> { "ignored-err", "pass" };

        var entries = DisplayXRValidationEngine.Evaluate(rules, Ctx(Platforms.Windows), ignored);
        var by = new Dictionary<string, DisplayXRValidationEntry>();
        foreach (var e in entries) by[e.Rule.Id] = e;

        Check(entries.Count == rules.Count, "one entry per rule, in order");
        Check(by["pass"].Result.Status == Status.Pass && !by["pass"].IsOpenIssue, "a pass is not an issue");
        Check(by["err"].IsOpenIssue && by["err"].Severity == Severity.Error && by["err"].CanFix, "a failure takes the rule's severity");
        Check(by["info-override"].Severity == Severity.Info, "a result can override the rule's severity");
        Check(!by["info-override"].CanFix, "NoFix hides the fix for that finding");
        Check(by["android-only"].Result.Status == Status.NotApplicable && androidRuns == 0,
              "a rule for another platform is not applicable and its check does not run");
        Check(by["throws"].Failed && by["throws"].Severity == Severity.Warning && !by["throws"].CanFix
              && by["throws"].Result.Message.Contains("boom"),
              "a check that throws becomes a warning naming the exception, with no fix");
        Check(by["na"].Result.Status == Status.NotApplicable && !by["na"].Failed, "NotApplicable is not a failure");
        Check(by["ignored-err"].Failed && by["ignored-err"].Ignored && !by["ignored-err"].IsOpenIssue,
              "an ignored failure is still a failure, but not an open issue");

        var s = DisplayXRValidationEngine.Summarize(entries);
        Check(s.Errors == 1 && s.Warnings == 2 && s.Infos == 1 && s.Ignored == 1 && s.Passed == 1 && s.NotApplicable == 2,
              $"summary counts (E{s.Errors} W{s.Warnings} I{s.Infos} ign{s.Ignored} pass{s.Passed} na{s.NotApplicable})");
        Check(s.OpenIssues == 4, "open issues = errors + warnings + notes");
        Check(s.ToString() == "1 error, 2 warnings, 1 note (1 ignored)", "summary text (got '" + s + "')");

        // On Android the Android-only rule runs.
        var android = DisplayXRValidationEngine.Evaluate(rules, Ctx(Platforms.Android), null);
        Check(androidRuns == 1 && android[4].Failed, "the platform filter lets a matching rule run");
        Check(!android[7].Ignored, "a null ignore list ignores nothing");

        Check(DisplayXRValidationEngine.Summarize(new List<DisplayXRValidationEntry>()).ToString() == "No open issues",
              "empty summary text");
        var onlyIgnored = DisplayXRValidationEngine.Evaluate(
            new[] { Rule("x", Severity.Error, c => Result.Fail("f")) }, Ctx(Platforms.Linux), new[] { "x" });
        Check(DisplayXRValidationEngine.Summarize(onlyIgnored).ToString() == "No open issues (1 ignored)",
              "summary text when everything open is ignored");
    }

    static void TestFixAllAndBuildMessages()
    {
        var fixable = Rule("fixable", Severity.Error, c => Result.Fail("no loader"));
        fixable.Fix = c => { };
        fixable.FixDescription = "Enable the loader";
        var noDescription = Rule("no-description", Severity.Warning, c => Result.Fail("w"));
        noDescription.Fix = c => { };
        var navigates = Rule("navigates", Severity.Info, c => Result.Fail("go look"));
        navigates.Fix = c => { };
        navigates.FixOnlyNavigates = true;
        var noFix = Rule("no-fix", Severity.Error, c => Result.Fail("cannot be fixed here"));
        var ignoredRule = Rule("skipped", Severity.Error, c => Result.Fail("known"));
        ignoredRule.Fix = c => { };
        var passing = Rule("passing");
        passing.Fix = c => { };
        var noFixForFinding = Rule("finding-without-fix", Severity.Error, c => Result.Fail("info", Severity.Info, noFix: true));
        noFixForFinding.Fix = c => { };
        var nullMessage = Rule("null-message", Severity.Error, c => Result.Fail(null));
        // An Error with a fix, but no description of it: Fix All falls back to the title.
        var errorNoDescription = Rule("error-plain", Severity.Error, c => Result.Fail("e2"));
        errorNoDescription.Fix = c => { };
        // Policy, not repair: an Error (another XR loader ahead of DisplayXR) that a project
        // may still have on purpose. Its own Fix button stays; Fix All must not touch it.
        var policy = Rule("policy", Severity.Warning, c => Result.Fail("other loader first", Severity.Error));
        policy.Fix = c => { };
        policy.ExcludeFromFixAll = true;
        // A Warning-by-default rule whose finding is an Error IS a repair.
        var escalated = Rule("escalated", Severity.Warning, c => Result.Fail("worse than usual", Severity.Error));
        escalated.Fix = c => { };
        escalated.FixDescription = "Repair the escalated thing";
        // An Error-by-default rule whose finding is only a Warning is not.
        var softened = Rule("softened", Severity.Error, c => Result.Fail("milder than usual", Severity.Warning));
        softened.Fix = c => { };

        var entries = DisplayXRValidationEngine.Evaluate(
            new[] { fixable, noDescription, navigates, noFix, ignoredRule, passing, noFixForFinding, nullMessage,
                    errorNoDescription, policy, escalated, softened },
            Ctx(Platforms.Windows), new[] { "skipped" });

        var plan = DisplayXRValidationEngine.FixAllPlan(entries);
        var planIds = new List<string>();
        foreach (var e in plan) planIds.Add(e.Rule.Id);
        Check(string.Join(",", planIds) == "fixable,error-plain,escalated",
              "Fix All = open ERRORS with a real fix; not warnings/notes, ignored, passing, navigation-only, " +
              "NoFix or ExcludeFromFixAll (got " + string.Join(",", planIds) + ")");
        foreach (var e in plan)
            Check(e.Severity == Severity.Error && !e.Rule.ExcludeFromFixAll && !e.Ignored,
                  "every Fix All entry is an open, non-policy Error: " + e.Rule.Id);
        var byId = new Dictionary<string, DisplayXRValidationEntry>();
        foreach (var e in entries) byId[e.Rule.Id] = e;
        Check(byId["policy"].CanFix && byId["no-description"].CanFix && byId["softened"].CanFix,
              "what Fix All leaves out keeps its own Fix button");

        // The dialog lists exactly the plan: one line per entry, nothing else.
        string text = DisplayXRValidationEngine.DescribeFixAllPlan(plan);
        Check(text == "• Enable the loader\n• Title of error-plain\n• Repair the escalated thing\n",
              "Fix All lists one line per change, falling back to the title (got " + text.Replace("\n", "|") + ")");
        Check(text.Split('\n').Length - 1 == plan.Count, "one line per planned change");
        Check(DisplayXRValidationEngine.FixAllPlan(new List<DisplayXRValidationEntry>()).Count == 0, "empty plan");

        var lines = DisplayXRValidationEngine.BuildMessages(entries);
        Check(lines.Count == 6, "the build hook reports open Errors only (got " + lines.Count + ")");
        Check(string.Join("|", lines).Contains("[policy]"),
              "ExcludeFromFixAll is about Fix All only: the Error is still reported at build time");
        Check(!string.Join("|", lines).Contains("[softened]"), "a finding softened to a Warning is not reported at build time");
        Check(lines.Count > 0 && lines[0] == "Title of fixable: no loader [fixable]", "build line = title: message [id]");
        Check(lines.Count > 1 && lines[1] == "Title of no-fix: cannot be fixed here [no-fix]",
              "an Error without a fix is still reported");
        Check(lines.Count > 2 && lines[2] == "Title of null-message: Why null-message [null-message]",
              "a finding without a message falls back to the explanation");
        foreach (var line in lines)
            Check(!line.Contains("skipped") && !line.Contains("no-description") && !line.Contains("finding-without-fix"),
                  "ignored rules, warnings and overridden-to-Info findings stay out of the build log");
    }

    static void TestBuildHint()
    {
        string same = DisplayXRValidationEngine.BuildHint("StandaloneWindows64", "StandaloneWindows64");
        Check(same.Contains("Build target StandaloneWindows64") && !same.Contains("switch"),
              "building the active target: the hint names it and does not ask for a switch");
        string other = DisplayXRValidationEngine.BuildHint("Android", "StandaloneWindows64");
        Check(other.Contains("Build target Android") && other.Contains("switch the active build target to Android")
              && other.Contains("which is StandaloneWindows64"),
              "building another target: the hint says the window shows the active one and to switch (got " + other + ")");
        Check(same.Contains("ignore the rule") && other.Contains("ignore the rule"), "both hints say how to silence the line");
    }

    // ---- ignore list ------------------------------------------------------------

    static void TestIgnoreList()
    {
        string json = DisplayXRValidationIgnoreList.Serialize(new[] { "runtime-installed", "a-rule", "runtime-installed" });
        Check(json == "{\n  \"version\": 1,\n  \"ignored\": [\n    \"a-rule\",\n    \"runtime-installed\"\n  ]\n}\n",
              "serialized sorted, de-duplicated, one id per line");
        Check(DisplayXRValidationIgnoreList.Serialize(new string[0]) == "{\n  \"version\": 1,\n  \"ignored\": []\n}\n",
              "empty list serializes to an empty array");

        var back = DisplayXRValidationIgnoreList.Parse(json);
        Check(back.Count == 2 && back.Contains("a-rule") && back.Contains("runtime-installed"), "round trip");
        Check(DisplayXRValidationIgnoreList.Parse(DisplayXRValidationIgnoreList.Serialize(new string[0])).Count == 0,
              "empty round trip");

        // Hand-edited / merged files: any whitespace, CRLF, single line, extra keys.
        var loose = DisplayXRValidationIgnoreList.Parse(
            "{\r\n\"note\":\"x\",\r\n \"ignored\" : [ \"one\" ,\"two\"\r\n,\t\"three\" ],\r\n \"version\":1 }");
        Check(loose.Count == 3 && loose.Contains("three"), "tolerates whitespace, CRLF and unknown keys");
        // Strings after the array are not ids.
        var trailing = DisplayXRValidationIgnoreList.Parse("{\"ignored\":[\"one\"],\"other\":[\"not-an-id-here\"]}");
        Check(trailing.Count == 1 && trailing.Contains("one"), "reads only the ignored array");

        // Damaged input must show the issues again, never throw and never hide.
        foreach (var bad in new[] { null, "", "not json", "{", "{\"ignored\":", "{\"ignored\":[\"unterminated", "{\"ignored\":[\"a\"",
                                    "{\"version\":1}" })
            Check(DisplayXRValidationIgnoreList.Parse(bad).Count == 0, "damaged file reads as nothing ignored: " + (bad ?? "(null)"));
        Check(DisplayXRValidationIgnoreList.Parse("{\"ignored\":[\"ok\",\"Not Valid\",\"\"]}").Count == 1,
              "malformed ids are dropped, good ones kept");
        Check(!DisplayXRValidationIgnoreList.Serialize(new[] { "Bad Id", "fine" }).Contains("Bad"),
              "malformed ids are never written");

        string dir = Path.Combine(Path.GetTempPath(), "dxr-validation-" + Guid.NewGuid().ToString("N"));
        string path = Path.Combine(dir, "ProjectSettings", "DisplayXRProjectValidation.json");
        try
        {
            Check(DisplayXRValidationIgnoreList.Load(path).Count == 0, "missing file = nothing ignored");
            DisplayXRValidationIgnoreList.Save(path, new[] { "b", "a" });
            Check(File.Exists(path), "Save creates the folder and the file");
            byte[] bytes = File.ReadAllBytes(path);
            Check(bytes.Length > 3 && !(bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF), "written without a BOM");
            Check(Array.IndexOf(bytes, (byte)'\r') < 0, "written with LF line endings");
            var loaded = DisplayXRValidationIgnoreList.Load(path);
            Check(loaded.Count == 2 && loaded.Contains("a") && loaded.Contains("b"), "Save / Load round trip");
            DisplayXRValidationIgnoreList.Save(path, new string[0]);
            Check(!File.Exists(path), "saving an empty list removes the file");
            DisplayXRValidationIgnoreList.Save(path, null);
            Check(!File.Exists(path), "saving an empty list twice is fine");
        }
        finally
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
        }
        TestIgnoreListTrySet();
        Check(DisplayXRValidationIgnoreList.DefaultPath == "ProjectSettings/DisplayXRProjectValidation.json",
              "the ignore list is project state under ProjectSettings/");
    }

    // A failed write must leave the in-memory list as it was: what the window shows as
    // ignored must be what is on disk.
    static void TestIgnoreListTrySet()
    {
        string dir = Path.Combine(Path.GetTempPath(), "dxr-validation-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            string path = Path.Combine(dir, "list.json");
            var ids = new HashSet<string>();
            string error;

            Check(DisplayXRValidationIgnoreList.TrySet(path, ids, "a", true, out error) && error == null
                  && ids.Contains("a") && DisplayXRValidationIgnoreList.Load(path).Contains("a"),
                  "TrySet adds and writes");
            DateTime stamp = File.GetLastWriteTimeUtc(path);
            Check(DisplayXRValidationIgnoreList.TrySet(path, ids, "a", true, out error) && ids.Count == 1
                  && File.GetLastWriteTimeUtc(path) == stamp,
                  "TrySet with nothing to change succeeds without rewriting the file");
            Check(!DisplayXRValidationIgnoreList.TrySet(path, ids, "Not Valid", true, out error) && error != null
                  && ids.Count == 1, "TrySet rejects a malformed id and changes nothing");

            // A path that cannot be written on any OS: its parent "folder" is a file.
            string blocker = Path.Combine(dir, "blocker");
            File.WriteAllText(blocker, "x");
            string unwritable = Path.Combine(blocker, "list.json");

            var kept = new HashSet<string> { "kept" };
            Check(!DisplayXRValidationIgnoreList.TrySet(unwritable, kept, "new", true, out error),
                  "TrySet reports a failed write");
            Check(!string.IsNullOrEmpty(error), "a failed write gives the reason");
            Check(kept.Count == 1 && kept.Contains("kept") && !kept.Contains("new"),
                  "a failed ADD is rolled back in memory");

            // Removing: with one id left the list is still written (non-empty), so it fails too.
            var two = new HashSet<string> { "one", "two" };
            Check(!DisplayXRValidationIgnoreList.TrySet(unwritable, two, "one", false, out error)
                  && two.Count == 2 && two.Contains("one"),
                  "a failed REMOVE is rolled back in memory");

            // A read-only file, the version-control case. (Skipped where the OS lets the
            // owner write anyway, e.g. running as root.)
            DisplayXRValidationIgnoreList.Save(path, new[] { "a" });
            File.SetAttributes(path, FileAttributes.ReadOnly);
            bool reallyReadOnly;
            try { File.AppendAllText(path, ""); reallyReadOnly = false; }
            catch (UnauthorizedAccessException) { reallyReadOnly = true; }
            if (reallyReadOnly)
            {
                var ro = DisplayXRValidationIgnoreList.Load(path);
                Check(!DisplayXRValidationIgnoreList.TrySet(path, ro, "b", true, out error)
                      && !ro.Contains("b") && ro.Contains("a"),
                      "read-only file: adding fails and is rolled back");
                Check(!DisplayXRValidationIgnoreList.TrySet(path, ro, "a", false, out error)
                      && ro.Contains("a") && File.Exists(path),
                      "read-only file: removing the last id (a delete) fails and is rolled back");
                var onDisk = DisplayXRValidationIgnoreList.Load(path);
                Check(onDisk.Count == 1 && onDisk.Contains("a"), "the read-only file is unchanged");
            }
            else
            {
                Console.WriteLine("note: read-only files are writable here, skipped the read-only checks");
            }
            File.SetAttributes(path, FileAttributes.Normal);
        }
        finally
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
        }
    }

    // ---- graphics APIs ----------------------------------------------------------
    // Pinned to the platform x API table in DisplayXRDisplayLoader.IsSinglePassEligible.

    static Api.Verdict Classify(Platforms p, params string[] apis)
    {
        return Api.Classify(p, apis);
    }

    static string Recommend(Platforms p, params string[] apis)
    {
        return string.Join(",", Api.Recommend(p, apis));
    }

    static void TestGraphicsApis()
    {
        Check(Classify(Platforms.Windows, "Direct3D11") == Api.Verdict.Ok, "Windows D3D11 ok");
        Check(Classify(Platforms.Windows, "Direct3D12", "Direct3D11") == Api.Verdict.Ok, "Windows D3D12 ok");
        Check(Classify(Platforms.Windows, "Vulkan", "Direct3D12") == Api.Verdict.WindowsVulkanFallsBackToD3D12,
              "Windows Vulkan first = the deliberate D3D12 fallback, not an error");
        Check(Classify(Platforms.Windows, "Direct3D11", "Vulkan") == Api.Verdict.Ok, "Windows Vulkan behind D3D11 is just a fallback");
        Check(Classify(Platforms.Windows, "OpenGLCore", "Direct3D11") == Api.Verdict.FirstUnsupported, "Windows OpenGL first");
        Check(Classify(Platforms.Windows, "Direct3D11", "OpenGLCore") == Api.Verdict.Ok, "Windows: only the first API is judged");

        Check(Classify(Platforms.MacOS, "Metal") == Api.Verdict.Ok, "macOS Metal ok");
        Check(Classify(Platforms.MacOS, "OpenGLCore", "Metal") == Api.Verdict.FirstUnsupported, "macOS OpenGL first");
        Check(Classify(Platforms.MacOS, "Vulkan") == Api.Verdict.FirstUnsupported, "macOS has no Vulkan backend");

        Check(Classify(Platforms.Linux, "Vulkan") == Api.Verdict.Ok, "Linux Vulkan ok");
        Check(Classify(Platforms.Linux, "Vulkan", "OpenGLCore") == Api.Verdict.Ok, "Linux Vulkan first, OpenGL behind");
        Check(Classify(Platforms.Linux, "OpenGLCore", "Vulkan") == Api.Verdict.FirstUnsupported, "Linux OpenGL first");

        Check(Classify(Platforms.Android, "Vulkan") == Api.Verdict.Ok, "Android Vulkan only ok");
        Check(Classify(Platforms.Android, "Vulkan", "OpenGLES3") == Api.Verdict.AndroidHasNonVulkanFallback,
              "Android: a non-Vulkan fallback is flagged");
        Check(Classify(Platforms.Android, "OpenGLES3", "Vulkan") == Api.Verdict.FirstUnsupported, "Android GLES first");

        Check(Classify(Platforms.Other, "WebGPU") == Api.Verdict.Unknown, "an unsupported build target is not judged");
        Check(Classify(Platforms.Windows) == Api.Verdict.Unknown, "an empty list is not judged");
        Check(Api.Classify(Platforms.Windows, null) == Api.Verdict.Unknown, "a null list is not judged");

        // What the Fix writes.
        Check(Recommend(Platforms.Android, "OpenGLES3", "Vulkan") == "Vulkan", "Android fix: Vulkan only");
        Check(Recommend(Platforms.Android, "Vulkan", "OpenGLES3") == "Vulkan", "Android fix drops the fallback");
        Check(Recommend(Platforms.Linux, "OpenGLCore", "Vulkan") == "Vulkan,OpenGLCore", "Linux fix: Vulkan to the front, list kept");
        Check(Recommend(Platforms.Linux, "OpenGLCore") == "Vulkan,OpenGLCore", "Linux fix adds Vulkan when it is absent");
        Check(Recommend(Platforms.Windows, "OpenGLCore", "Direct3D12", "Direct3D11") == "Direct3D12,OpenGLCore,Direct3D11",
              "Windows fix promotes the project's own first supported API");
        Check(Recommend(Platforms.Windows, "OpenGLCore") == "Direct3D11,OpenGLCore", "Windows fix adds D3D11 when nothing is supported");
        Check(Recommend(Platforms.MacOS, "OpenGLCore") == "Metal,OpenGLCore", "macOS fix adds Metal");
        Check(Recommend(Platforms.Other, "WebGPU") == "WebGPU", "no recommendation for an unsupported target");

        // Every recommendation must itself pass (on Windows it may still read as the Vulkan
        // note, when Vulkan was the project's own first supported API).
        foreach (var p in new[] { Platforms.Windows, Platforms.MacOS, Platforms.Linux, Platforms.Android })
            foreach (var start in new[] { new[] { "OpenGLCore" }, new[] { "OpenGLES3", "Vulkan" }, new[] { "Direct3D11" }, new[] { "Metal" } })
            {
                var v = Api.Classify(p, Api.Recommend(p, start));
                Check(v == Api.Verdict.Ok || (p == Platforms.Windows && v == Api.Verdict.WindowsVulkanFallsBackToD3D12),
                      "the fix's list passes the check: " + p + " from " + string.Join(",", start) + " -> " + v);
            }
    }

    // ---- XR loaders -------------------------------------------------------------

    static void TestLoaders()
    {
        const string dxr = DisplayXRLoaderRules.DisplayXRLoaderType;
        const string oxr = DisplayXRLoaderRules.UnityOpenXRLoaderType;
        Check(dxr == "DisplayXR.DisplayXRDisplayLoader", "loader type name matches Runtime/Provider/DisplayXRDisplayLoader.cs");

        Check(DisplayXRLoaderRules.Classify(new string[0]) == DisplayXRLoaderRules.Verdict.NotEnabled, "no loaders");
        Check(DisplayXRLoaderRules.Classify(null) == DisplayXRLoaderRules.Verdict.NotEnabled, "null list");
        Check(DisplayXRLoaderRules.Classify(new[] { oxr }) == DisplayXRLoaderRules.Verdict.NotEnabled, "only another loader");
        Check(DisplayXRLoaderRules.Classify(new[] { dxr }) == DisplayXRLoaderRules.Verdict.Sole, "DisplayXR alone");
        Check(DisplayXRLoaderRules.Classify(new[] { oxr, dxr }) == DisplayXRLoaderRules.Verdict.OtherLoaderFirst,
              "another loader tried first");
        Check(DisplayXRLoaderRules.Classify(new[] { dxr, oxr }) == DisplayXRLoaderRules.Verdict.OtherLoaderAfter,
              "another loader tried after");
        Check(DisplayXRLoaderRules.Classify(new[] { "A.B", dxr, oxr }) == DisplayXRLoaderRules.Verdict.OtherLoaderFirst,
              "one ahead and one behind counts as ahead");

        var others = DisplayXRLoaderRules.Others(new[] { "A.B", dxr, oxr });
        Check(others.Count == 2 && others[0] == "A.B" && others[1] == oxr, "Others keeps order and drops DisplayXR");
        Check(DisplayXRLoaderRules.Others(null).Count == 0, "Others of null");
        Check(DisplayXRLoaderRules.FriendlyName(oxr).StartsWith("OpenXR"), "Unity's OpenXR loader is named as such");
        Check(DisplayXRLoaderRules.FriendlyName("Some.Vendor.FooLoader") == "FooLoader", "other loaders by class name");
        Check(DisplayXRLoaderRules.FriendlyName("NoNamespace") == "NoNamespace", "a type without a namespace");

        // "Not enabled" is never an Error - an Error would reach the build log, and a
        // target left without the loader is a choice.
        Check(DisplayXRLoaderRules.NotEnabledSeverity(true) == Severity.Info,
              "loader off here, on for another target: a note");
        Check(DisplayXRLoaderRules.NotEnabledSeverity(false) == Severity.Warning,
              "loader on nowhere: a warning");
        foreach (var elsewhere in new[] { true, false })
        {
            var rule = Rule("loader-enabled", Severity.Warning,
                            c => Result.Fail("not ticked", DisplayXRLoaderRules.NotEnabledSeverity(elsewhere)));
            rule.Fix = c => { };
            var found = DisplayXRValidationEngine.Evaluate(new[] { rule }, Ctx(Platforms.Android), null);
            Check(DisplayXRValidationEngine.BuildMessages(found).Count == 0,
                  "a loader that is not enabled is never written to the build log (elsewhere=" + elsewhere + ")");
            Check(DisplayXRValidationEngine.FixAllPlan(found).Count == 0,
                  "and Fix All never enables it (elsewhere=" + elsewhere + ")");
            Check(found[0].CanFix, "its own Fix button stays (elsewhere=" + elsewhere + ")");
        }

        // The constant must name the real class. The runtime source is next door.
        string root = FindRepoRoot();
        if (root != null)
        {
            string loader = File.ReadAllText(Path.Combine(root, "Runtime", "Provider", "DisplayXRDisplayLoader.cs"));
            Check(loader.Contains("namespace DisplayXR\n") || loader.Contains("namespace DisplayXR\r\n"),
                  "DisplayXRDisplayLoader is in namespace DisplayXR");
            Check(loader.Contains("class DisplayXRDisplayLoader"), "DisplayXRDisplayLoader is still the loader's class name");
        }
    }

    // ---- menu multi-selection ---------------------------------------------------
    // Unity calls a GameObject/ menu item once per selected object from the Hierarchy's
    // context menu (context = that object), once without context from the menu bar.

    static void TestMenuRules()
    {
        Check(DisplayXRMenuRules.ActsOnThisInvocation(false, -1), "menu bar: the one invocation acts");
        Check(DisplayXRMenuRules.CoversWholeSelection(false, -1), "menu bar: over the whole selection");

        // [Camera, Cube] or N non-cameras, right-clicked: N invocations, one acts.
        foreach (int n in new[] { 1, 2, 5 })
        {
            int acting = 0;
            for (int i = 0; i < n; i++)
            {
                if (DisplayXRMenuRules.ActsOnThisInvocation(true, i)) acting++;
                Check(DisplayXRMenuRules.CoversWholeSelection(true, i), "a selected context covers the whole selection");
            }
            Check(acting == 1, "exactly one of " + n + " per-object invocations acts (got " + acting + ")");
        }

        // A context that is not in the selection: act for that object alone.
        Check(DisplayXRMenuRules.ActsOnThisInvocation(true, -1), "context outside the selection acts");
        Check(!DisplayXRMenuRules.CoversWholeSelection(true, -1), "context outside the selection: only that object");
    }

    // ---- native plug-in ---------------------------------------------------------

    static void TestNativePlugin()
    {
        Check(DisplayXRNativePluginRules.RequiredFiles(Platforms.Windows).Length == 1
              && DisplayXRNativePluginRules.RequiredFiles(Platforms.Windows)[0].EndsWith("Windows/x64/displayxr_unity.dll"),
              "Windows needs the x64 DLL");
        Check(DisplayXRNativePluginRules.RequiredFiles(Platforms.MacOS)[0].EndsWith(".bundle"), "macOS needs the bundle");
        Check(DisplayXRNativePluginRules.RequiredFiles(Platforms.Linux).Length == 2, "Linux needs the plugin and its Wayland library");
        Check(DisplayXRNativePluginRules.RequiredFiles(Platforms.Android).Length == 3, "Android needs the plugin, the loader and the layer");
        Check(DisplayXRNativePluginRules.RequiredFiles(Platforms.Other).Length == 0, "nothing ships for other targets");

        // Every required file's .meta must be in the tree: the binaries may be absent (the
        // Linux / Android ones are CI-built), their .meta files are always committed. This
        // is what catches a renamed or added library that the rule was not told about.
        string root = FindRepoRoot();
        if (root != null)
        {
            foreach (var p in new[] { Platforms.Windows, Platforms.MacOS, Platforms.Linux, Platforms.Android })
                foreach (var rel in DisplayXRNativePluginRules.RequiredFiles(p))
                    Check(File.Exists(Path.Combine(root, rel + ".meta")), "committed .meta for " + rel);
        }
        else
        {
            Console.WriteLine("note: repo root not found, skipped the checks against the source tree");
        }

        Check(!DisplayXRNativePluginRules.IsCiBuiltOnly(Platforms.Windows) && !DisplayXRNativePluginRules.IsCiBuiltOnly(Platforms.MacOS),
              "Windows and macOS binaries are committed");
        Check(DisplayXRNativePluginRules.IsCiBuiltOnly(Platforms.Linux) && DisplayXRNativePluginRules.IsCiBuiltOnly(Platforms.Android),
              "Linux and Android binaries are CI-built");

        Check(DisplayXRNativePluginRules.MissingSeverity(Platforms.Linux, true) == Severity.Info, "source checkout, Linux: expected");
        Check(DisplayXRNativePluginRules.MissingSeverity(Platforms.Android, true) == Severity.Info, "source checkout, Android: expected");
        Check(DisplayXRNativePluginRules.MissingSeverity(Platforms.Linux, false) == Severity.Error, "published install, Linux: wrong ref");
        Check(DisplayXRNativePluginRules.MissingSeverity(Platforms.Android, false) == Severity.Error, "published install, Android: wrong ref");
        Check(DisplayXRNativePluginRules.MissingSeverity(Platforms.Windows, true) == Severity.Error,
              "the Windows DLL is committed: missing is an error even in a checkout");
        Check(DisplayXRNativePluginRules.MissingSeverity(Platforms.MacOS, true) == Severity.Error, "the macOS bundle is committed: same");
    }

    static string FindRepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "package.json")) &&
                Directory.Exists(Path.Combine(dir.FullName, "Runtime", "Plugins")))
                return dir.FullName;
            dir = dir.Parent;
        }
        return null;
    }

    // ---- Android ----------------------------------------------------------------

    static void TestAndroid()
    {
        const uint v7 = DisplayXRAndroidRules.ArchARMv7, a64 = DisplayXRAndroidRules.ArchARM64;
        Check(v7 == 1u && a64 == 2u, "bit values match UnityEditor.AndroidArchitecture");
        Check(DisplayXRAndroidRules.ClassifyArchitectures(a64) == DisplayXRAndroidRules.ArchVerdict.Arm64Only, "ARM64 only");
        Check(DisplayXRAndroidRules.ClassifyArchitectures(v7 | a64) == DisplayXRAndroidRules.ArchVerdict.Arm64AndOthers, "ARMv7 + ARM64");
        Check(DisplayXRAndroidRules.ClassifyArchitectures(a64 | 8u) == DisplayXRAndroidRules.ArchVerdict.Arm64AndOthers, "ARM64 + x86-64");
        Check(DisplayXRAndroidRules.ClassifyArchitectures(v7) == DisplayXRAndroidRules.ArchVerdict.NoArm64, "ARMv7 only");
        Check(DisplayXRAndroidRules.ClassifyArchitectures(0u) == DisplayXRAndroidRules.ArchVerdict.NoArm64, "none");
        Check(DisplayXRAndroidRules.ClassifyArchitectures(0xffffffffu) == DisplayXRAndroidRules.ArchVerdict.Arm64AndOthers, "All");
        Check(DisplayXRAndroidRules.MinApiLevel == 29, "API 29, as native~/build-android.sh builds");

        string root = FindRepoRoot();
        if (root != null)
        {
            string script = File.ReadAllText(Path.Combine(root, "native~", "build-android.sh"));
            Check(script.Contains("ANDROID_PLATFORM:-" + DisplayXRAndroidRules.MinApiLevel + "}"),
                  "native~/build-android.sh still defaults to the API level the rule names");
        }
    }

    // ---- versions ---------------------------------------------------------------

    static void TestVersions()
    {
        Check(DisplayXRVersionRules.AtLeast("17.0.0", 17, 0, 0), "equal");
        Check(DisplayXRVersionRules.AtLeast("17.0.3", 17, 0, 0), "newer patch");
        Check(DisplayXRVersionRules.AtLeast("17.1.0", 17, 0, 5), "newer minor beats patch");
        Check(DisplayXRVersionRules.AtLeast("18.0.0", 17, 9, 9), "newer major beats the rest");
        Check(!DisplayXRVersionRules.AtLeast("14.0.11", 17, 0, 0), "URP 14 (Unity 2022.3) is older than 17");
        Check(!DisplayXRVersionRules.AtLeast("16.9.9", 17, 0, 0), "older major");
        Check(!DisplayXRVersionRules.AtLeast("1.2.9", 1, 3, 0), "XR Hands 1.2.x is too old");
        Check(DisplayXRVersionRules.AtLeast("1.3.0", 1, 3, 0), "XR Hands 1.3.0");
        Check(DisplayXRVersionRules.AtLeast("1.5.1", 1, 3, 0), "XR Hands 1.5.1");
        Check(DisplayXRVersionRules.AtLeast("1.10.0", 1, 3, 0), "numeric, not lexical: 1.10 > 1.3");
        Check(DisplayXRVersionRules.AtLeast("17.0.0-pre.2", 17, 0, 0), "a pre-release tag is ignored");
        Check(DisplayXRVersionRules.AtLeast("1.3.0+build.5", 1, 3, 0), "build metadata is ignored");
        Check(DisplayXRVersionRules.AtLeast("17", 17, 0, 0) && DisplayXRVersionRules.AtLeast("17.1", 17, 1, 0), "short versions");
        foreach (var bad in new[] { null, "", "abc", "1.x.0", "1.2.3.4", "-1.0.0", "1..0" })
            Check(!DisplayXRVersionRules.AtLeast(bad, 0, 0, 0), "unparseable version is never 'at least': " + (bad ?? "(null)"));
    }

    static int Main()
    {
        TestRegistry();
        TestIds();
        TestEvaluate();
        TestFixAllAndBuildMessages();
        TestBuildHint();
        TestIgnoreList();
        TestGraphicsApis();
        TestLoaders();
        TestMenuRules();
        TestNativePlugin();
        TestAndroid();
        TestVersions();
        Console.WriteLine($"project_validation (C#): {s_pass} passed, {s_fail} failed");
        return s_fail == 0 ? 0 : 1;
    }
}
