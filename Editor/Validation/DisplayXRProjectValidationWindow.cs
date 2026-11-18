// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using UnityEditor;
using UnityEngine;

namespace DisplayXR.Editor
{
    /// <summary>
    /// <b>DisplayXR &gt; Project Validation</b> (#462): every registered rule checked
    /// against the active build target, one row each, with a Fix button where a safe fix
    /// exists and an Ignore toggle that is stored with the project.
    /// <para>
    /// The window never opens by itself and never changes a setting by itself: a fix runs
    /// from its own button, and Fix All lists what it will change and asks first.
    /// </para>
    /// </summary>
    public sealed class DisplayXRProjectValidationWindow : EditorWindow
    {
        const string MenuPath = "DisplayXR/Project Validation";

        List<DisplayXRValidationEntry> m_Entries;
        int m_EvaluatedTarget;
        Vector2 m_Scroll;
        bool m_ShowPassed;
        bool m_ShowIgnored = true;

        static GUIStyle s_Wrap, s_WrapMini, s_Title;

        [MenuItem(MenuPath, priority = 0)]
        public static void Open()
        {
            var window = GetWindow<DisplayXRProjectValidationWindow>(false, "DisplayXR Validation");
            window.minSize = new Vector2(420f, 300f);
            window.Refresh();
            window.Show();
        }

        void OnEnable()
        {
            DisplayXRValidationRegistry.Changed += Invalidate;
            EditorApplication.playModeStateChanged += OnPlayModeChanged;
        }

        void OnDisable()
        {
            DisplayXRValidationRegistry.Changed -= Invalidate;
            EditorApplication.playModeStateChanged -= OnPlayModeChanged;
        }

        // Settings change behind the window's back (another settings page, a platform
        // switch): take a fresh look whenever the user comes back to it.
        void OnFocus() { Invalidate(); }

        void OnPlayModeChanged(PlayModeStateChange change) { Invalidate(); }

        void Invalidate()
        {
            m_Entries = null;
            Repaint();
        }

        void Refresh()
        {
            m_Entries = DisplayXRProjectValidation.Evaluate();
            m_EvaluatedTarget = (int)EditorUserBuildSettings.activeBuildTarget;
            DisplayXRProjectValidation.InvalidateCache();
        }

        void OnGUI()
        {
            EnsureStyles();
            if (m_Entries == null || m_EvaluatedTarget != (int)EditorUserBuildSettings.activeBuildTarget)
                Refresh();

            var summary = DisplayXRValidationEngine.Summarize(m_Entries);
            var plan = DisplayXRValidationEngine.FixAllPlan(m_Entries);

            // Fixes edit XR Plug-in Management and Player settings, which Unity does not
            // allow (or does not apply) while playing.
            bool canFix = !EditorApplication.isPlayingOrWillChangePlaymode;

            using (new EditorGUILayout.HorizontalScope(EditorStyles.toolbar))
            {
                GUILayout.Label("Build target: " + EditorUserBuildSettings.activeBuildTarget,
                                EditorStyles.toolbarButton);
                GUILayout.FlexibleSpace();
                m_ShowPassed = GUILayout.Toggle(m_ShowPassed, "Show passed", EditorStyles.toolbarButton);
                if (GUILayout.Button("Refresh", EditorStyles.toolbarButton))
                    Refresh();
                using (new EditorGUI.DisabledScope(!canFix || plan.Count == 0))
                {
                    if (GUILayout.Button("Fix All (" + plan.Count + ")", EditorStyles.toolbarButton))
                        FixAll(plan);
                }
                if (GUILayout.Button("Docs", EditorStyles.toolbarButton))
                    Application.OpenURL(DisplayXRProjectValidation.DocUrl);
            }

            EditorGUILayout.Space();
            EditorGUILayout.LabelField(summary.ToString(), EditorStyles.boldLabel);
            if (!canFix)
                EditorGUILayout.HelpBox("Fixes are unavailable in Play Mode.", MessageType.None);

            m_Scroll = EditorGUILayout.BeginScrollView(m_Scroll);

            // Open issues, worst first; the registry order is kept inside a severity.
            bool any = false;
            for (int severity = (int)DisplayXRValidationSeverity.Error; severity >= 0; severity--)
            {
                foreach (var entry in m_Entries)
                {
                    if (!entry.IsOpenIssue || (int)entry.Severity != severity) continue;
                    DrawEntry(entry, canFix);
                    any = true;
                }
            }
            if (!any)
                EditorGUILayout.HelpBox("Nothing to fix for " + EditorUserBuildSettings.activeBuildTarget + ".",
                                        MessageType.Info);

            if (summary.Ignored > 0)
            {
                EditorGUILayout.Space();
                m_ShowIgnored = EditorGUILayout.Foldout(m_ShowIgnored, "Ignored (" + summary.Ignored + ")", true);
                if (m_ShowIgnored)
                    foreach (var entry in m_Entries)
                        if (entry.Failed && entry.Ignored)
                            DrawEntry(entry, canFix);
            }

            if (m_ShowPassed)
            {
                EditorGUILayout.Space();
                EditorGUILayout.LabelField("Passed / not applicable", EditorStyles.boldLabel);
                foreach (var entry in m_Entries)
                    if (!entry.Failed)
                        DrawEntry(entry, canFix);
            }

            EditorGUILayout.EndScrollView();

            // DrawEntry may have asked for a refresh (after a fix or an ignore change);
            // the list must not change while it is being drawn.
            if (m_RefreshRequested)
            {
                m_RefreshRequested = false;
                Refresh();
                Repaint();
            }
        }

        bool m_RefreshRequested;

        void DrawEntry(DisplayXRValidationEntry entry, bool canFix)
        {
            var rule = entry.Rule;
            using (new EditorGUILayout.VerticalScope(EditorStyles.helpBox))
            {
                using (new EditorGUILayout.HorizontalScope())
                {
                    GUILayout.Label(StatusIcon(entry), GUILayout.Width(20f), GUILayout.Height(18f));
                    GUILayout.Label(new GUIContent(rule.Title, rule.Id), s_Title);
                    GUILayout.FlexibleSpace();
                    GUILayout.Label(StatusText(entry), EditorStyles.miniLabel);
                }

                if (!string.IsNullOrEmpty(entry.Result.Message))
                    EditorGUILayout.LabelField(entry.Result.Message, s_Wrap);
                if (!string.IsNullOrEmpty(rule.Explanation))
                    EditorGUILayout.LabelField(rule.Explanation, s_WrapMini);

                using (new EditorGUILayout.HorizontalScope())
                {
                    GUILayout.Label(rule.Id, EditorStyles.miniLabel);
                    GUILayout.FlexibleSpace();

                    if (!string.IsNullOrEmpty(rule.DocUrl) &&
                        GUILayout.Button("Docs", EditorStyles.miniButton, GUILayout.Width(50f)))
                        Application.OpenURL(rule.DocUrl);

                    if (entry.Failed)
                    {
                        bool ignore = GUILayout.Toggle(entry.Ignored,
                            new GUIContent("Ignore", "Stop reporting this rule for this project. Stored in " +
                                           DisplayXRValidationIgnoreList.DefaultPath + "."),
                            EditorStyles.miniButton, GUILayout.Width(60f));
                        if (ignore != entry.Ignored)
                        {
                            DisplayXRProjectValidation.SetIgnored(rule.Id, ignore);
                            m_RefreshRequested = true;
                        }
                    }

                    if (entry.CanFix)
                    {
                        // A fix that only opens a page is harmless in Play Mode.
                        using (new EditorGUI.DisabledScope(!canFix && !rule.FixOnlyNavigates))
                        {
                            string label = string.IsNullOrEmpty(rule.FixLabel) ? "Fix" : rule.FixLabel;
                            var content = new GUIContent(label, rule.FixDescription);
                            if (GUILayout.Button(content, EditorStyles.miniButton,
                                                 GUILayout.MinWidth(60f), GUILayout.ExpandWidth(false)))
                            {
                                RunFix(entry);
                                m_RefreshRequested = true;
                            }
                        }
                    }
                }
            }
        }

        void RunFix(DisplayXRValidationEntry entry)
        {
            try
            {
                entry.Rule.Fix(DisplayXRProjectValidation.ActiveContext());
            }
            catch (Exception e)
            {
                Debug.LogWarning("[DisplayXR] Project validation: the fix for '" + entry.Rule.Id +
                                 "' failed: " + e.Message);
            }
        }

        void FixAll(List<DisplayXRValidationEntry> plan)
        {
            if (plan.Count == 0) return;
            string text =
                "This will change " + plan.Count + " project setting" + (plan.Count == 1 ? "" : "s") +
                " for " + EditorUserBuildSettings.activeBuildTarget + ":\n\n" +
                DisplayXRValidationEngine.DescribeFixAllPlan(plan) +
                "\nEach one is an ordinary Unity setting you can change back on its usual page.";
            if (!EditorUtility.DisplayDialog("DisplayXR - Fix All", text, "Apply", "Cancel"))
                GUIUtility.ExitGUI(); // throws: nothing is changed
            foreach (var entry in plan)
                RunFix(entry);
            Refresh();
            // A modal dialog ran inside OnGUI: the layout pass it interrupted is stale.
            GUIUtility.ExitGUI();
        }

        static GUIContent StatusIcon(DisplayXRValidationEntry entry)
        {
            string icon;
            if (!entry.Failed)
                icon = entry.Result.Status == DisplayXRValidationStatus.Pass ? "TestPassed" : "TestNormal";
            else if (entry.Ignored)
                icon = "TestIgnored";
            else if (entry.Severity == DisplayXRValidationSeverity.Error)
                icon = "console.erroricon.sml";
            else if (entry.Severity == DisplayXRValidationSeverity.Warning)
                icon = "console.warnicon.sml";
            else
                icon = "console.infoicon.sml";
            // IconContent logs an error for an unknown name; these exist in 2022.3 and Unity 6,
            // and a missing one must not break the row.
            var content = EditorGUIUtility.IconContent(icon);
            return content ?? GUIContent.none;
        }

        static string StatusText(DisplayXRValidationEntry entry)
        {
            switch (entry.Result.Status)
            {
                case DisplayXRValidationStatus.Pass: return "Passed";
                case DisplayXRValidationStatus.NotApplicable: return "Not applicable";
                default: return entry.Ignored ? entry.Severity + " (ignored)" : entry.Severity.ToString();
            }
        }

        static void EnsureStyles()
        {
            if (s_Wrap != null) return;
            s_Wrap = new GUIStyle(EditorStyles.label) { wordWrap = true };
            s_WrapMini = new GUIStyle(EditorStyles.miniLabel) { wordWrap = true };
            s_Title = new GUIStyle(EditorStyles.boldLabel);
        }
    }
}
