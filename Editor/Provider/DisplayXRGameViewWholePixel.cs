// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Reflection;
using UnityEditor;
using UnityEngine;
using DisplayXR;

namespace DisplayXR.Editor
{
    /// <summary>
    /// (#459) WHOLE-PIXEL Game view size — an opt-in experiment
    /// (env <c>DISPLAYXR_PROV_GV_WHOLEPIXEL=1</c>, read by
    /// <see cref="DisplayXRProviderDriver.WholePixelEnabled"/>; off by default, and the
    /// default placement is untouched).
    ///
    /// At a fractional Windows scale a point is not a whole number of pixels. A Free Aspect
    /// Game view's target is its area, which at 125 / 175 / 225 % is a fraction of a pixel
    /// wide; Unity allocates a whole-pixel render texture and draws it, point-filtered, into
    /// the fractional rect. One column is dropped (or doubled) where the error crosses half a
    /// pixel and everything past it sits one pixel off — on a lenticular panel, the other eye.
    /// A Fixed Resolution whose size is a whole number of points AND pixels is drawn texel
    /// for pixel at zoom 1. So, during Play, this selects such a size on the Game view the
    /// glue is bound to (the main play-mode view): the largest multiple of the step
    /// (<see cref="DisplayXRGameViewPlacement.WholePixelStepPt"/>: 175 % -> 4 pt = 7 px,
    /// 150 % -> 2 pt = 3 px, 125 % -> 4 pt = 5 px, 250 % -> 2 pt = 5 px) that fits the view
    /// area, as a temporary custom size named <see cref="SizeName"/>, and snaps the zoom to 1.
    /// Whole scales (100 / 200 / 300 %) need nothing and keep the user's size.
    ///
    /// What it restores, and when: the previous selection and zoom are put back and the custom
    /// size removed on leaving Play, before a domain reload (it is re-applied afterwards if Play
    /// continues), on quitting, when the pane moves to a whole scale, and when the main view
    /// changes. A size left behind by a crash is removed at the next editor load. If the user
    /// picks another size while it is on, that choice wins and the experiment stays off until
    /// the next Play. A failed apply is rolled back the same way. Unity keeps custom sizes in a
    /// per-user list shared by all projects and writes it only from its own size menu, so
    /// after removing ours the list is saved (best-effort): otherwise a save the user
    /// triggered while the experiment was on would keep our entry for every project.
    ///
    /// The 0.35 s resize debounce: the size follows the view AREA, which a splitter drag
    /// changes every frame; a new size is applied only once the area has held for
    /// <see cref="DisplayXRProviderDriver.GameViewSettleSeconds"/>, so a drag costs one
    /// Game-view size change, which the driver's own debounce then pushes once.
    ///
    /// GVFIT: the driver skips its row stretch on the view that carries this size and places
    /// the anchor as an unstretched letterbox with the pane's real left border (see
    /// <see cref="DisplayXRProviderDriver.WholePixelView"/>) — this experiment wins there.
    ///
    /// Everything here reaches into UnityEditor internals (GameViewSizes, GameViewSizeGroup,
    /// GameView.selectedSizeIndex / SnapZoom / viewInWindow / m_ZoomArea) by reflection,
    /// best-effort: the first failure logs one warning (once per editor session) and turns the
    /// experiment off until the next domain reload.
    /// No Win32 call is made here; the scale is the driver's measured pane scale.
    /// </summary>
#if UNITY_EDITOR_WIN
    [InitializeOnLoad]
#endif
    internal static class DisplayXRGameViewWholePixel
    {
#if UNITY_EDITOR_WIN
        internal const string SizeName = "DisplayXR whole-pixel";
        const string kSessPrevIndex = "DisplayXR.GameViewWholePixel.PrevIndex"; // survives domain reloads
        const string kSessPrevZoom  = "DisplayXR.GameViewWholePixel.PrevZoom";  // the zoom to put back (0 = none)
        const string kSessWarned    = "DisplayXR.Once.GameViewWholePixel";      // warn once per editor session
        const double kPollGap = 0.1;
        const float  kToolbarPt = 21f;   // EditorGUI.kWindowToolbarHeight, the viewInWindow fallback
        const BindingFlags kAny = BindingFlags.Instance | BindingFlags.Static |
                                  BindingFlags.Public | BindingFlags.NonPublic;

        static bool         s_active;          // our size is selected on s_view
        static EditorWindow s_view;
        static int          s_appliedWPx, s_appliedHPx;
        static float        s_pendW, s_pendH, s_pendScale;   // candidate area awaiting settle
        static double       s_pendSince = -1.0;
        static double       s_nextPoll;
        static bool         s_exitingPlay;
        static bool         s_userOverride;    // the user picked another size: off until the next Play
        static bool         s_disabled;        // reflection failed: off for the session
        static float        s_loggedIdleScale; // "nothing to do at this scale", once per scale

        static DisplayXRGameViewWholePixel()
        {
            EditorApplication.update += Poll;
            EditorApplication.playModeStateChanged += OnPlayModeChanged;
            AssemblyReloadEvents.beforeAssemblyReload += OnBeforeAssemblyReload;
            EditorApplication.quitting += OnQuitting;
            // A domain load never has a live activation (the reload restored it), so an
            // entry found now was left by a crash: remove it, whether or not the switch is set.
            EditorApplication.delayCall += () => { if (!s_active) Restore("left over from an earlier session", quiet: true); };
        }

        static void OnPlayModeChanged(PlayModeStateChange change)
        {
            switch (change)
            {
            case PlayModeStateChange.ExitingPlayMode:
                s_exitingPlay = true;
                Restore("leaving Play", quiet: !s_active);
                break;
            case PlayModeStateChange.EnteredEditMode:
                s_exitingPlay = false;
                s_userOverride = false;
                Restore("left Play", quiet: true);   // backstop
                break;
            case PlayModeStateChange.EnteredPlayMode:
                s_exitingPlay = false;
                s_userOverride = false;
                break;
            }
        }

        static void OnBeforeAssemblyReload() { if (s_active) Restore("domain reload"); }
        static void OnQuitting() { Restore("editor quitting", quiet: true); }

        static void Poll()
        {
            if (!DisplayXRProviderDriver.WholePixelEnabled()) return;
            double now = EditorApplication.timeSinceStartup;
            if (now < s_nextPoll) return;
            s_nextPoll = now + kPollGap;

            if (!EditorApplication.isPlaying || s_exitingPlay)
            {
                if (s_active) Restore("not playing");
                return;
            }
            if (s_disabled || s_userOverride) return;
            // The subsystem is restarting (dock change, re-host): hold what is selected.
            if (!DisplayXRProviderDriver.IsActive) return;
            var view = DisplayXRProviderDriver.MatchedGameView as EditorWindow;
            float scale = DisplayXRProviderDriver.MatchedPaneScale;
            if (view == null || !(scale > 0f)) return;

            try { Tick(view, scale, now); }
            catch (Exception e)
            {
                s_disabled = true;
                WarnOnce("could not set a whole-pixel Game view size (editor internals changed?), " +
                         "so the experiment is off for this session: " + Unwrap(e).Message);
                // Unconditionally: Apply sets s_active last, so a throw after AddCustomSize or
                // after the selection write (e.g. SnapZoom missing) leaves our size behind, and
                // selected, with GVFIT still stretching it.
                Restore("reflection failure", quiet: true);
            }
        }

        static void Tick(EditorWindow view, float scale, double now)
        {
            int step = DisplayXRGameViewPlacement.WholePixelStepPt(scale);
            if (step <= 1)
            {
                if (s_active) Restore(step == 1 ? "the pane is at a whole display scale" : "the pane's scale is not a whole percent");
                if (scale != s_loggedIdleScale)
                {
                    s_loggedIdleScale = scale;
                    Debug.Log("[DisplayXR] whole-pixel Game view: nothing to do at " + Percent(scale) +
                              (step == 1 ? " (a point is a whole number of pixels; the selected size is kept)"
                                         : " (the measured scale is not a whole percent)"));
                }
                return;
            }

            if (s_active && s_view != view) Restore("the main Game view changed");

            if (s_active)
            {
                // The user's own pick wins: if our size is no longer the selected one (or was
                // deleted in the size menu), stand down until the next Play.
                object group = CurrentGroup(out Type groupType);
                int ours = FindEntry(group, groupType, out _);
                if (ours < 0 || GetSelectedIndex(view) != ours)
                {
                    s_userOverride = true;
                    Restore("another Game view size was picked - off until the next Play");
                    return;
                }
            }

            Vector2 area = ViewAreaPoints(view);
            if (s_pendSince < 0.0 || area.x != s_pendW || area.y != s_pendH || scale != s_pendScale)
            {
                s_pendW = area.x; s_pendH = area.y; s_pendScale = scale; s_pendSince = now;
                return;   // (re)start the settle clock
            }
            if (now - s_pendSince < DisplayXRProviderDriver.GameViewSettleSeconds) return;

            if (!DisplayXRGameViewPlacement.WholePixelSize(area.x, area.y, scale,
                                                           out int wPt, out int hPt, out int wPx, out int hPx))
            {
                if (s_active) Restore("the view area is smaller than one step");
                return;
            }
            if (s_active && wPx == s_appliedWPx && hPx == s_appliedHPx) return;
            Apply(view, wPx, hPx, wPt, hPt, step, scale, area);
        }

        static void Apply(EditorWindow view, int wPx, int hPx, int wPt, int hPt, int step, float scale, Vector2 area)
        {
            object group = CurrentGroup(out Type groupType);
            int index = FindEntry(group, groupType, out object entry);
            Type sizeType = EditorType("UnityEditor.GameViewSize");
            if (entry == null)
            {
                Type kind = EditorType("UnityEditor.GameViewSizeType");
                entry = Activator.CreateInstance(sizeType, kAny, null,
                    new object[] { Enum.Parse(kind, "FixedResolution"), wPx, hPx, SizeName }, null);
                Method(groupType, "AddCustomSize").Invoke(group, new[] { entry });
                // Found again by name: whether the group stores the instance or a copy, and what
                // its IndexOf returns for a custom size, has varied between editor versions.
                index = FindEntry(group, groupType, out entry);
                if (index < 0) throw new InvalidOperationException("the custom size did not appear in the size group");
            }
            else
            {
                PropertyInfo w = Property(sizeType, "width"), h = Property(sizeType, "height");
                if ((int)w.GetValue(entry, null) != wPx) w.SetValue(entry, wPx, null);
                if ((int)h.GetValue(entry, null) != hPx) h.SetValue(entry, hPx, null);
            }

            int current = GetSelectedIndex(view);
            if (!s_active && current != index)
            {
                SessionState.SetInt(kSessPrevIndex, current);
                // The user's zoom (x: GVFIT only ever writes y), put back by Restore.
                SessionState.SetFloat(kSessPrevZoom, TryGetZoomX(view, out float zoom) ? zoom : 0f);
            }
            if (current != index) Property(view.GetType(), "selectedSizeIndex").SetValue(view, index, null);
            // Zoom 1 on both axes (it also clears GVFIT's row stretch, which the driver stops
            // re-applying to this view from now on).
            Method(view.GetType(), "SnapZoom", typeof(float)).Invoke(view, new object[] { 1f });
            view.Repaint();

            bool first = !s_active;
            s_active = true; s_view = view; s_appliedWPx = wPx; s_appliedHPx = hPx;
            DisplayXRProviderDriver.WholePixelView = view;
            var inv = System.Globalization.CultureInfo.InvariantCulture;
            Debug.Log("[DisplayXR] whole-pixel Game view: " + (first ? "selected " : "resized to ") +
                      wPx + "x" + hPx + " px (" + wPt + "x" + hPt + " pt, steps of " + step + " pt = " +
                      Mathf.RoundToInt(step * scale) + " px) at " + Percent(scale) + " for a " +
                      area.x.ToString("0.###", inv) + "x" + area.y.ToString("0.###", inv) + " pt area; zoom 1" +
                      (first ? "; previous size index " + current : ""));
        }

        /// <summary>Put the previous selection back on every Game view that shows our size and
        /// remove the size. Safe to call when nothing of ours exists.</summary>
        static void Restore(string reason, bool quiet = false)
        {
            s_active = false; s_view = null; s_appliedWPx = s_appliedHPx = 0; s_pendSince = -1.0;
            DisplayXRProviderDriver.WholePixelView = null;   // GVFIT and the default placement resume
            try
            {
                object group = CurrentGroup(out Type groupType);
                int index = FindEntry(group, groupType, out _);
                if (index < 0) return;

                int prev = SessionState.GetInt(kSessPrevIndex, 0);
                float prevZoom = SessionState.GetFloat(kSessPrevZoom, 0f);
                SessionState.EraseInt(kSessPrevIndex);
                SessionState.EraseFloat(kSessPrevZoom);
                if (prev > index) prev--;                 // a size added after ours moves down one
                if (prev < 0 || prev == index) prev = 0;  // Free Aspect

                Type gvType = EditorType("UnityEditor.GameView");
                foreach (UnityEngine.Object o in Resources.FindObjectsOfTypeAll(gvType))
                {
                    var v = o as EditorWindow;
                    if (v == null) continue;
                    int sel = GetSelectedIndex(v);
                    PropertyInfo selProp = Property(v.GetType(), "selectedSizeIndex");
                    if (sel == index)
                    {
                        selProp.SetValue(v, prev, null);
                        // The zoom the view had before (uniform; GVFIT re-applies its row
                        // stretch on the next frame). Best-effort: the size matters more.
                        if (prevZoom > 0f)
                        {
                            try { Method(v.GetType(), "SnapZoom", typeof(float)).Invoke(v, new object[] { prevZoom }); }
                            catch (Exception) { }
                        }
                    }
                    else if (sel > index) selProp.SetValue(v, sel - 1, null);
                    v.Repaint();
                }

                // Remove every entry of our name (a crash can leave one behind, and a later
                // session add another). RemoveCustomSize takes the TOTAL index (it converts
                // with TotalIndexToCustomIndex itself; read from the 6000.4 editor assembly). A
                // variant taking the index within the custom list is tried only if the entry is
                // still there, and the first call is guarded so that, under that other meaning,
                // it could only be out of range, never another custom size.
                MethodInfo remove = Method(groupType, "RemoveCustomSize", typeof(int));
                for (int guard = 0; index >= 0 && guard < 8; guard++)
                {
                    int customIndex = (int)Method(groupType, "TotalIndexToCustomIndex", typeof(int)).Invoke(group, new object[] { index });
                    int customCount = (int)Method(groupType, "GetCustomCount").Invoke(group, null);
                    int before = (int)Method(groupType, "GetTotalCount").Invoke(group, null);
                    if (index >= customCount)
                    {
                        try { remove.Invoke(group, new object[] { index }); } catch (TargetInvocationException) { }
                    }
                    if ((int)Method(groupType, "GetTotalCount").Invoke(group, null) == before && customIndex >= 0)
                    {
                        try { remove.Invoke(group, new object[] { customIndex }); } catch (TargetInvocationException) { }
                    }
                    if ((int)Method(groupType, "GetTotalCount").Invoke(group, null) == before)
                        throw new InvalidOperationException("RemoveCustomSize did not remove it");
                    index = FindEntry(group, groupType, out _);
                }
                // Add / RemoveCustomSize only bump a change id; the list is written to the
                // per-user, all-projects file by Unity's size menu. Write it now, without our
                // entry, in case such a save happened while it was in the list.
                try
                {
                    Type sizesType = EditorType("UnityEditor.GameViewSizes");
                    object sizes = Property(sizesType, "instance").GetValue(null, null);
                    Method(sizesType, "SaveToHDD").Invoke(sizes, null);
                }
                catch (Exception) { /* best-effort: the entry is gone from this editor either way */ }
                if (!quiet)
                    Debug.Log("[DisplayXR] whole-pixel Game view: restored the previous size (index " + prev +
                              ") and removed \"" + SizeName + "\" - " + reason);
            }
            catch (Exception e)
            {
                if (!quiet)
                    WarnOnce("could not remove the \"" + SizeName + "\" Game view size (" + reason +
                             "); delete it from the Game view's size menu: " + Unwrap(e).Message);
            }
        }

        // ---- reflection helpers (throw on a missing member; callers catch) ----

        static Type EditorType(string fullName)
        {
            Type t = typeof(EditorWindow).Assembly.GetType(fullName);
            if (t == null) throw new MissingMemberException(fullName);
            return t;
        }

        static MethodInfo Method(Type type, string name, params Type[] args)
        {
            for (Type t = type; t != null; t = t.BaseType)
            {
                MethodInfo m = t.GetMethod(name, kAny | BindingFlags.DeclaredOnly, null, args, null);
                if (m != null) return m;
            }
            throw new MissingMethodException(type.Name, name);
        }

        static PropertyInfo Property(Type type, string name)
        {
            for (Type t = type; t != null; t = t.BaseType)
            {
                PropertyInfo p = t.GetProperty(name, kAny | BindingFlags.DeclaredOnly);
                if (p != null) return p;
            }
            throw new MissingMemberException(type.Name, name);
        }

        // GameViewSizes.instance.currentGroup (the size list of the active build target group).
        static object CurrentGroup(out Type groupType)
        {
            Type sizesType = EditorType("UnityEditor.GameViewSizes");
            object sizes = Property(sizesType, "instance").GetValue(null, null);
            object group = Property(sizesType, "currentGroup").GetValue(sizes, null);
            if (group == null) throw new InvalidOperationException("GameViewSizes.currentGroup is null");
            groupType = group.GetType();
            return group;
        }

        // Our custom size's TOTAL index (built-in sizes first) and the entry, or -1 / null.
        static int FindEntry(object group, Type groupType, out object entry)
        {
            int total = (int)Method(groupType, "GetTotalCount").Invoke(group, null);
            MethodInfo get = Method(groupType, "GetGameViewSize", typeof(int));
            MethodInfo isCustom = Method(groupType, "IsCustomSize", typeof(int));
            PropertyInfo baseText = null;
            for (int i = 0; i < total; i++)
            {
                if (!(bool)isCustom.Invoke(group, new object[] { i })) continue;
                object s = get.Invoke(group, new object[] { i });
                if (s == null) continue;
                if (baseText == null) baseText = Property(s.GetType(), "baseText");
                if ((string)baseText.GetValue(s, null) == SizeName) { entry = s; return i; }
            }
            entry = null;
            return -1;
        }

        static int GetSelectedIndex(EditorWindow view) =>
            (int)Property(view.GetType(), "selectedSizeIndex").GetValue(view, null);

        // GameView.viewInWindow: the area below the toolbar, in points (EditorWindow.position
        // has the dock borders removed, so this needs no scale).
        static Vector2 ViewAreaPoints(EditorWindow view)
        {
            try
            {
                if (Property(view.GetType(), "viewInWindow").GetValue(view, null) is Rect r && r.width > 1f && r.height > 1f)
                    return new Vector2(r.width, r.height);
            }
            catch (MissingMemberException) { }
            Rect p = view.position;
            return new Vector2(p.width, p.height - kToolbarPt);
        }

        // GameView.m_ZoomArea.m_Scale.x: the view's zoom. False when the fields are not found.
        static bool TryGetZoomX(EditorWindow view, out float zoom)
        {
            zoom = 0f;
            try
            {
                FieldInfo areaField = null;
                for (Type t = view.GetType(); t != null && areaField == null; t = t.BaseType)
                    areaField = t.GetField("m_ZoomArea", kAny | BindingFlags.DeclaredOnly);
                object area = areaField != null ? areaField.GetValue(view) : null;
                if (area == null) return false;
                FieldInfo scaleField = null;
                for (Type t = area.GetType(); t != null && scaleField == null; t = t.BaseType)
                    scaleField = t.GetField("m_Scale", kAny | BindingFlags.DeclaredOnly);
                if (scaleField == null || !(scaleField.GetValue(area) is Vector2 sc)) return false;
                zoom = sc.x;
                return zoom > 0f;
            }
            catch (Exception) { return false; }
        }

        static string Percent(float scale) =>
            (scale * 100f).ToString("0.#", System.Globalization.CultureInfo.InvariantCulture) + " %";

        static Exception Unwrap(Exception e) =>
            e is TargetInvocationException && e.InnerException != null ? e.InnerException : e;

        static void WarnOnce(string message)
        {
            // Once per editor session: statics do not survive the domain reload of each Play.
            if (SessionState.GetBool(kSessWarned, false)) return;
            SessionState.SetBool(kSessWarned, true);
            Debug.LogWarning("[DisplayXR] whole-pixel Game view: " + message);
        }
#endif // UNITY_EDITOR_WIN
    }
}
