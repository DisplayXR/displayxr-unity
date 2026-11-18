// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Project validation, the data model (#462): what a rule is, the registry rules are added
// to, how a set of rules is evaluated, and the per-project ignore list.
//
// PURE C# - no UnityEngine / UnityEditor in this file. It is compiled twice: by Unity as
// part of the editor assembly, and by Tests~/ProjectValidation, which runs in CI without
// the editor. Unity types cross this boundary as plain ints (see
// DisplayXRValidationContext) and are turned back in DisplayXRProjectValidation.cs.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace DisplayXR.Editor
{
    /// <summary>How much a failed rule matters. Ordered: a higher value is worse.</summary>
    public enum DisplayXRValidationSeverity
    {
        /// <summary>Worth knowing; nothing is broken.</summary>
        Info = 0,
        /// <summary>Works, but not the way the project most likely wants.</summary>
        Warning = 1,
        /// <summary>The app will not render in 3D (or not start) on the target.</summary>
        Error = 2,
    }

    /// <summary>The outcome of one rule's check.</summary>
    public enum DisplayXRValidationStatus
    {
        Pass,
        Fail,
        /// <summary>The rule says nothing about this project / build target.</summary>
        NotApplicable,
    }

    /// <summary>
    /// The platform a rule is evaluated for. A context carries exactly one flag; a rule
    /// carries the set it applies to.
    /// </summary>
    [Flags]
    public enum DisplayXRValidationPlatforms
    {
        None = 0,
        Windows = 1,
        MacOS = 2,
        Linux = 4,
        Android = 8,
        /// <summary>A build target the provider does not exist for (WebGL, iOS, ...).</summary>
        Other = 16,
        Standalone = Windows | MacOS | Linux,
        /// <summary>Every target the DisplayXR provider ships for.</summary>
        Supported = Standalone | Android,
        All = Supported | Other,
    }

    /// <summary>What a rule is checked against.</summary>
    public sealed class DisplayXRValidationContext
    {
        /// <summary>The build target's platform - exactly one flag.</summary>
        public DisplayXRValidationPlatforms Platform;
        /// <summary><c>(int)UnityEditor.BuildTarget</c>. Use the <c>Target()</c> extension.</summary>
        public int BuildTargetId;
        /// <summary><c>(int)UnityEditor.BuildTargetGroup</c>. Use the <c>Group()</c> extension.</summary>
        public int BuildTargetGroupId;
        /// <summary>True when evaluated from the build hook, false from the window.</summary>
        public bool IsBuild;
    }

    /// <summary>What a rule's check returns.</summary>
    public struct DisplayXRValidationResult
    {
        public DisplayXRValidationStatus Status;
        /// <summary>What was found, in one or two sentences. May be null on a pass.</summary>
        public string Message;
        /// <summary>
        /// Set when this particular finding is more or less serious than the rule's
        /// default (a Vulkan-first Windows project is an Info under a rule that is
        /// otherwise an Error).
        /// </summary>
        public DisplayXRValidationSeverity? SeverityOverride;
        /// <summary>
        /// True when the rule has a fix but it does not apply to this finding - the
        /// window then shows no Fix button.
        /// </summary>
        public bool NoFix;

        public static DisplayXRValidationResult Pass(string message = null)
        {
            return new DisplayXRValidationResult { Status = DisplayXRValidationStatus.Pass, Message = message };
        }

        public static DisplayXRValidationResult NotApplicable(string message = null)
        {
            return new DisplayXRValidationResult { Status = DisplayXRValidationStatus.NotApplicable, Message = message };
        }

        public static DisplayXRValidationResult Fail(string message,
            DisplayXRValidationSeverity? severity = null, bool noFix = false)
        {
            return new DisplayXRValidationResult
            {
                Status = DisplayXRValidationStatus.Fail, Message = message,
                SeverityOverride = severity, NoFix = noFix,
            };
        }
    }

    /// <summary>
    /// One project-validation rule. Build one, then hand it to
    /// <see cref="DisplayXRValidationRegistry.Register"/> from an
    /// <c>[InitializeOnLoadMethod]</c>; any editor assembly may add rules.
    /// </summary>
    public sealed class DisplayXRValidationRule
    {
        /// <summary>
        /// Stable identifier: lower-case letters, digits, '-' and '.', starting with a
        /// letter or digit. It is what the ignore list stores and what the doc anchors on,
        /// so never rename one that has shipped.
        /// </summary>
        public string Id;
        /// <summary>Short name shown as the row's heading.</summary>
        public string Title;
        /// <summary>Why the rule exists and what happens when it fails.</summary>
        public string Explanation;
        /// <summary>Severity of a failure, unless the result overrides it.</summary>
        public DisplayXRValidationSeverity Severity = DisplayXRValidationSeverity.Warning;
        /// <summary>The build targets the rule applies to. Elsewhere it is NotApplicable.</summary>
        public DisplayXRValidationPlatforms Platforms = DisplayXRValidationPlatforms.Supported;
        /// <summary>Sort key in the window; rules with the same value keep registration order.</summary>
        public int Order;
        /// <summary>Where the rule is documented (optional).</summary>
        public string DocUrl;
        /// <summary>The check. Must not change anything.</summary>
        public Func<DisplayXRValidationContext, DisplayXRValidationResult> Check;
        /// <summary>
        /// Optional fix. Runs only from an explicit click (Fix, or Fix All after its
        /// confirmation). Leave null when no safe, reversible fix exists.
        /// </summary>
        public Action<DisplayXRValidationContext> Fix;
        /// <summary>What <see cref="Fix"/> changes, as one line. Listed by Fix All before it asks.</summary>
        public string FixDescription;
        /// <summary>Text on the button. Defaults to "Fix".</summary>
        public string FixLabel;
        /// <summary>
        /// True when <see cref="Fix"/> changes nothing itself - it only opens the page
        /// where the user makes the change. Such a fix is never part of Fix All.
        /// </summary>
        public bool FixOnlyNavigates;
        /// <summary>
        /// True when the fix is a matter of project policy rather than a repair (unticking
        /// another XR loader a project may keep on purpose, turning on Initialize XR on
        /// Startup for an app that may start XR itself). Its own Fix button stays; Fix All
        /// never runs it, whatever the finding's severity.
        /// </summary>
        public bool ExcludeFromFixAll;
    }

    /// <summary>A rule together with what its check found.</summary>
    public sealed class DisplayXRValidationEntry
    {
        public DisplayXRValidationRule Rule;
        public DisplayXRValidationResult Result;
        /// <summary>The result's override, else the rule's default.</summary>
        public DisplayXRValidationSeverity Severity;
        /// <summary>The project's ignore list names this rule.</summary>
        public bool Ignored;

        public bool Failed { get { return Result.Status == DisplayXRValidationStatus.Fail; } }
        /// <summary>A failure the user has not ignored.</summary>
        public bool IsOpenIssue { get { return Failed && !Ignored; } }
        /// <summary>A Fix button belongs on this row.</summary>
        public bool CanFix { get { return Failed && Rule.Fix != null && !Result.NoFix; } }
    }

    /// <summary>Counts over one evaluation. Ignored failures are counted apart.</summary>
    public struct DisplayXRValidationSummary
    {
        public int Errors, Warnings, Infos, Passed, NotApplicable, Ignored;

        public int OpenIssues { get { return Errors + Warnings + Infos; } }

        public override string ToString()
        {
            if (Errors == 0 && Warnings == 0 && Infos == 0)
                return Ignored > 0 ? "No open issues (" + Ignored + " ignored)" : "No open issues";
            var sb = new StringBuilder();
            Append(sb, Errors, "error");
            Append(sb, Warnings, "warning");
            Append(sb, Infos, "note");
            if (Ignored > 0) sb.Append(" (").Append(Ignored).Append(" ignored)");
            return sb.ToString();
        }

        static void Append(StringBuilder sb, int n, string noun)
        {
            if (n == 0) return;
            if (sb.Length > 0) sb.Append(", ");
            sb.Append(n).Append(' ').Append(noun);
            if (n != 1) sb.Append('s');
        }
    }

    /// <summary>
    /// The set of rules. Static and open: the package registers its own rules at editor
    /// load, the URP sub-assembly adds the URP-only ones, and an app or another package
    /// may add more the same way.
    /// </summary>
    public static class DisplayXRValidationRegistry
    {
        static readonly List<DisplayXRValidationRule> s_Rules = new List<DisplayXRValidationRule>();
        static readonly Dictionary<string, int> s_Sequence = new Dictionary<string, int>();
        static int s_NextSequence;

        /// <summary>Raised after a rule is added or removed (the window repaints on it).</summary>
        public static event Action Changed;

        /// <summary>
        /// Add a rule. A rule with the same id replaces the earlier one in place (so a
        /// second registration after a partial reload cannot produce two rows).
        /// </summary>
        /// <exception cref="ArgumentException">The id is malformed, or Title / Check is missing.</exception>
        public static void Register(DisplayXRValidationRule rule)
        {
            if (rule == null) throw new ArgumentNullException("rule");
            if (!IsValidId(rule.Id))
                throw new ArgumentException("DisplayXR validation rule id '" + rule.Id +
                    "' is not ^[a-z0-9][a-z0-9.-]{0,63}$");
            if (string.IsNullOrEmpty(rule.Title))
                throw new ArgumentException("DisplayXR validation rule '" + rule.Id + "' has no Title");
            if (rule.Check == null)
                throw new ArgumentException("DisplayXR validation rule '" + rule.Id + "' has no Check");

            int existing = s_Rules.FindIndex(r => r.Id == rule.Id);
            if (existing >= 0)
            {
                s_Rules[existing] = rule;
            }
            else
            {
                s_Sequence[rule.Id] = s_NextSequence++;
                s_Rules.Add(rule);
            }
            var changed = Changed;
            if (changed != null) changed();
        }

        /// <summary>Remove a rule. Returns false when no rule has that id.</summary>
        public static bool Unregister(string id)
        {
            int removed = s_Rules.RemoveAll(r => r.Id == id);
            if (removed == 0) return false;
            s_Sequence.Remove(id);
            var changed = Changed;
            if (changed != null) changed();
            return true;
        }

        /// <summary>The rule with this id, or null.</summary>
        public static DisplayXRValidationRule Find(string id)
        {
            return s_Rules.Find(r => r.Id == id);
        }

        /// <summary>A snapshot of every rule, by <c>Order</c> and then registration order.</summary>
        public static IReadOnlyList<DisplayXRValidationRule> Rules
        {
            get
            {
                var copy = new List<DisplayXRValidationRule>(s_Rules);
                copy.Sort((a, b) =>
                {
                    int c = a.Order.CompareTo(b.Order);
                    return c != 0 ? c : s_Sequence[a.Id].CompareTo(s_Sequence[b.Id]);
                });
                return copy;
            }
        }

        /// <summary>Lower-case letters, digits, '-' and '.'; 1..64 characters; no leading punctuation.</summary>
        public static bool IsValidId(string id)
        {
            if (string.IsNullOrEmpty(id) || id.Length > 64) return false;
            for (int i = 0; i < id.Length; i++)
            {
                char c = id[i];
                bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
                if (i == 0 ? !alnum : !(alnum || c == '-' || c == '.')) return false;
            }
            return true;
        }

        /// <summary>Tests only.</summary>
        internal static void Clear()
        {
            s_Rules.Clear();
            s_Sequence.Clear();
            s_NextSequence = 0;
        }
    }

    /// <summary>Runs rules and summarizes what they found. No side effects.</summary>
    public static class DisplayXRValidationEngine
    {
        /// <summary>
        /// Check every rule against <paramref name="context"/>. A rule whose platforms
        /// exclude the context's is NotApplicable without being run; a check that throws
        /// becomes a Warning naming the exception, so one bad rule cannot hide the rest.
        /// </summary>
        public static List<DisplayXRValidationEntry> Evaluate(
            IEnumerable<DisplayXRValidationRule> rules,
            DisplayXRValidationContext context,
            ICollection<string> ignoredIds)
        {
            var entries = new List<DisplayXRValidationEntry>();
            foreach (var rule in rules)
            {
                DisplayXRValidationResult result;
                if ((rule.Platforms & context.Platform) == 0)
                {
                    result = DisplayXRValidationResult.NotApplicable();
                }
                else
                {
                    try { result = rule.Check(context); }
                    catch (Exception e)
                    {
                        result = DisplayXRValidationResult.Fail(
                            "The check could not run: " + e.GetType().Name + ": " + e.Message,
                            DisplayXRValidationSeverity.Warning, noFix: true);
                    }
                }
                entries.Add(new DisplayXRValidationEntry
                {
                    Rule = rule,
                    Result = result,
                    Severity = result.SeverityOverride ?? rule.Severity,
                    Ignored = ignoredIds != null && ignoredIds.Contains(rule.Id),
                });
            }
            return entries;
        }

        public static DisplayXRValidationSummary Summarize(IEnumerable<DisplayXRValidationEntry> entries)
        {
            var s = new DisplayXRValidationSummary();
            foreach (var e in entries)
            {
                switch (e.Result.Status)
                {
                    case DisplayXRValidationStatus.Pass: s.Passed++; break;
                    case DisplayXRValidationStatus.NotApplicable: s.NotApplicable++; break;
                    default:
                        if (e.Ignored) s.Ignored++;
                        else if (e.Severity == DisplayXRValidationSeverity.Error) s.Errors++;
                        else if (e.Severity == DisplayXRValidationSeverity.Warning) s.Warnings++;
                        else s.Infos++;
                        break;
                }
            }
            return s;
        }

        /// <summary>
        /// What Fix All would run: open ERRORS whose fix changes something. Deliberately
        /// narrow - one click must not rewrite settings that are a choice rather than a
        /// fault. Left out: warnings and notes (each keeps its own Fix button), ignored
        /// rules, navigation-only fixes, and rules marked
        /// <see cref="DisplayXRValidationRule.ExcludeFromFixAll"/>.
        /// </summary>
        public static List<DisplayXRValidationEntry> FixAllPlan(IEnumerable<DisplayXRValidationEntry> entries)
        {
            var plan = new List<DisplayXRValidationEntry>();
            foreach (var e in entries)
                if (e.IsOpenIssue && e.CanFix && e.Severity == DisplayXRValidationSeverity.Error
                    && !e.Rule.FixOnlyNavigates && !e.Rule.ExcludeFromFixAll)
                    plan.Add(e);
            return plan;
        }

        /// <summary>
        /// What the build hook appends to each line: which target the finding is for and
        /// where the fix is. The window checks the ACTIVE build target, so when the build
        /// is for another one the hint says to switch first.
        /// </summary>
        public static string BuildHint(string builtTarget, string activeTarget)
        {
            return builtTarget == activeTarget
                ? "(Build target " + builtTarget + ". DisplayXR > Project Validation shows the fix; " +
                  "ignore the rule there to silence this.)"
                : "(Build target " + builtTarget + ". DisplayXR > Project Validation checks the active " +
                  "build target, which is " + activeTarget + ": switch the active build target to " +
                  builtTarget + " to see this finding and its fix there, or ignore the rule to silence this.)";
        }

        /// <summary>The text Fix All shows before it asks: one line per change.</summary>
        public static string DescribeFixAllPlan(IList<DisplayXRValidationEntry> plan)
        {
            var sb = new StringBuilder();
            for (int i = 0; i < plan.Count; i++)
            {
                var rule = plan[i].Rule;
                sb.Append("• ");
                sb.Append(string.IsNullOrEmpty(rule.FixDescription) ? rule.Title : rule.FixDescription);
                sb.Append('\n');
            }
            return sb.ToString();
        }

        /// <summary>
        /// What the build hook logs: one line per open Error. Warnings and notes stay in
        /// the window - a build log is not the place to nag.
        /// </summary>
        public static List<string> BuildMessages(IEnumerable<DisplayXRValidationEntry> entries)
        {
            var lines = new List<string>();
            foreach (var e in entries)
            {
                if (!e.IsOpenIssue || e.Severity != DisplayXRValidationSeverity.Error) continue;
                lines.Add(e.Rule.Title + ": " + (e.Result.Message ?? e.Rule.Explanation) +
                          " [" + e.Rule.Id + "]");
            }
            return lines;
        }
    }

    /// <summary>
    /// The rules a project has chosen to ignore. Project state, so it is a small text
    /// file under <c>ProjectSettings/</c> that goes into version control with the rest of
    /// the project's settings - never EditorPrefs, which are per machine and per user.
    /// <code>
    /// {
    ///   "version": 1,
    ///   "ignored": [
    ///     "runtime-installed"
    ///   ]
    /// }
    /// </code>
    /// Ids are written sorted, one per line, so two people ignoring different rules get a
    /// clean merge.
    /// </summary>
    public static class DisplayXRValidationIgnoreList
    {
        /// <summary>Relative to the project root (Unity's working directory).</summary>
        public const string DefaultPath = "ProjectSettings/DisplayXRProjectValidation.json";

        public const int FormatVersion = 1;

        public static string Serialize(IEnumerable<string> ids)
        {
            var sorted = new List<string>();
            foreach (var id in ids)
                if (DisplayXRValidationRegistry.IsValidId(id) && !sorted.Contains(id))
                    sorted.Add(id);
            sorted.Sort(string.CompareOrdinal);

            var sb = new StringBuilder();
            sb.Append("{\n  \"version\": ").Append(FormatVersion).Append(",\n  \"ignored\": [");
            for (int i = 0; i < sorted.Count; i++)
            {
                sb.Append(i == 0 ? "\n" : ",\n");
                sb.Append("    \"").Append(sorted[i]).Append('"');
            }
            sb.Append(sorted.Count == 0 ? "]\n}\n" : "\n  ]\n}\n");
            return sb.ToString();
        }

        /// <summary>
        /// Read the ids back. Tolerant: anything that is not a well-formed id is dropped,
        /// and a file that cannot be understood reads as "nothing ignored" - a damaged
        /// file must show the issues again, not hide them.
        /// </summary>
        public static HashSet<string> Parse(string json)
        {
            var ids = new HashSet<string>();
            if (string.IsNullOrEmpty(json)) return ids;
            int key = json.IndexOf("\"ignored\"", StringComparison.Ordinal);
            if (key < 0) return ids;
            int open = json.IndexOf('[', key);
            if (open < 0) return ids;
            int close = json.IndexOf(']', open);
            if (close < 0) return ids;

            int i = open + 1;
            while (i < close)
            {
                int start = json.IndexOf('"', i);
                if (start < 0 || start >= close) break;
                int end = json.IndexOf('"', start + 1);
                if (end < 0 || end > close) break;
                string id = json.Substring(start + 1, end - start - 1);
                if (DisplayXRValidationRegistry.IsValidId(id)) ids.Add(id);
                i = end + 1;
            }
            return ids;
        }

        /// <summary>Missing or unreadable file = nothing ignored.</summary>
        public static HashSet<string> Load(string path)
        {
            try
            {
                return File.Exists(path) ? Parse(File.ReadAllText(path)) : new HashSet<string>();
            }
            catch (IOException) { return new HashSet<string>(); }
            catch (UnauthorizedAccessException) { return new HashSet<string>(); }
        }

        /// <summary>
        /// Write the list. An empty list removes the file, so a project that ignores
        /// nothing carries no file at all.
        /// </summary>
        public static void Save(string path, ICollection<string> ids)
        {
            if (ids == null || ids.Count == 0)
            {
                if (File.Exists(path))
                {
                    // A read-only file (checked in under version control) must be refused the
                    // same way on every OS. Windows refuses the delete itself; POSIX would
                    // allow it, because deleting depends on the folder, not the file.
                    if ((File.GetAttributes(path) & FileAttributes.ReadOnly) != 0)
                        throw new UnauthorizedAccessException(
                            "The file is read-only: " + path);
                    File.Delete(path);
                }
                return;
            }
            string dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            File.WriteAllText(path, Serialize(ids), new UTF8Encoding(false));
        }

        /// <summary>
        /// Add or remove one id and write the file. If the write fails (a read-only file
        /// under version control, a locked folder) the set is put back as it was, so what
        /// is in memory never claims more than what is on disk; <paramref name="error"/>
        /// then says why. Returns true when the set and the file agree afterwards - also
        /// when nothing needed to change.
        /// </summary>
        public static bool TrySet(string path, ICollection<string> ids, string id, bool ignored, out string error)
        {
            error = null;
            if (!DisplayXRValidationRegistry.IsValidId(id))
            {
                error = "'" + id + "' is not a valid rule id";
                return false;
            }
            bool had = ids.Contains(id);
            if (had == ignored) return true;
            if (ignored) ids.Add(id); else ids.Remove(id);
            try
            {
                Save(path, ids);
                return true;
            }
            catch (Exception e)
            {
                if (ignored) ids.Remove(id); else ids.Add(id);
                error = e.Message;
                return false;
            }
        }
    }
}
