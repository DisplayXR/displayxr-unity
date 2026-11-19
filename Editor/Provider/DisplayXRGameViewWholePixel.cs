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
    /// (#459) WHOLE-PIXEL Game view size. Gated by
    /// <see cref="DisplayXRProviderDriver.WholePixelEnabled"/>: automatic by default (it
    /// turns on, for the rest of the editor session, once the on-screen calibration
    /// MEASURES an image that is not drawn 1:1 at a fractional scale);
    /// <c>DISPLAYXR_PROV_GV_WHOLEPIXEL=1</c> turns it on from the start, <c>=0</c> keeps it
    /// off. While it is off nothing here runs and the default placement is untouched.
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
    /// (#459) TAKING OVER A GAME VIEW THAT CANNOT BE 1:1. Independently of the whole-pixel
    /// size: a docked Game view whose zoom is not 1 (a Fixed Resolution larger than the view,
    /// which Unity shrinks to fit; or the Scale slider) resamples every texel, so 3D cannot
    /// be right and the calibration marker cannot be read. During Play such a view is taken
    /// over: zoom snapped to 1 when its size fits the view at 1x, otherwise Free Aspect at
    /// zoom 1 (<see cref="DisplayXRGameViewPlacement.TakeoverFor"/>). The user's size and
    /// zoom come back with the same Restore as above. <c>DISPLAYXR_PROV_GV_TAKEOVER=0</c>
    /// opts out; the driver then shows the scaled picture and warns once. A size or zoom
    /// changed by hand afterwards wins until the next Play.
    ///
    /// CRASH SAFETY. What is to be put back (the user's size index, its name, the zoom) is
    /// written to <c>Library/DisplayXR.GameViewTakeover.txt</c> BEFORE the first change and
    /// deleted after the restore. It used to live in SessionState, which dies with the
    /// editor, while the custom size lives only in memory (Add / RemoveCustomSize bump a
    /// change id; Unity writes the list from its size menu) and the selected INDEX is
    /// serialized with the window layout. An editor killed or closed in Play could so start
    /// again with an index one past the end of the list, which GameView clamps to the LAST
    /// entry - "4K UHD" 3840x2160 in the Standalone group - scaled to fit: a foreign size
    /// the user never chose. Now every editor load heals: the stretch GVFIT may have left in
    /// a saved zoom is undone, a record found on disk is restored from, a selection on our
    /// own entry or past the end goes to Free Aspect, and our entry is removed.
    /// The takeover itself only ever selects Free Aspect at zoom 1, Unity's own default
    /// and always index 0, so even an unhealed crash cannot leave a wrong or foreign size.
    ///
    /// Everything here reaches into UnityEditor internals (GameViewSizes, GameViewSizeGroup,
    /// GameView.selectedSizeIndex / SizeSelectionCallback / SnapZoom / viewInWindow /
    /// m_ZoomArea / m_SelectedSizes) by reflection,
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
        const string kRecordFile    = "DisplayXR.GameViewTakeover.txt";         // in Library/: survives a killed editor
        const string kSessWarned    = "DisplayXR.Once.GameViewWholePixel";      // warn once per editor session
        const double kPollGap = 0.1;
        const float  kToolbarPt = 21f;   // EditorGUI.kWindowToolbarHeight, the viewInWindow fallback
        const BindingFlags kAny = BindingFlags.Instance | BindingFlags.Static |
                                  BindingFlags.Public | BindingFlags.NonPublic;

        static bool         s_active;          // our size is selected on s_view
        static bool         s_took;            // s_view was taken over (zoom 1 / Free Aspect) this Play
        static int          s_takeCount;       // takeovers this Play (a second one = it did not hold)
        static int          s_takeoverGate = -1;
        static int          s_healTries;
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
            // A domain load never has a live takeover (the reload restored it), so whatever
            // is found now was left by an editor that was killed or closed in Play.
            EditorApplication.delayCall += HealOnLoad;
        }

        static void OnPlayModeChanged(PlayModeStateChange change)
        {
            switch (change)
            {
            case PlayModeStateChange.ExitingPlayMode:
                s_exitingPlay = true;
                Restore("leaving Play", quiet: !(s_active || s_took));
                break;
            case PlayModeStateChange.EnteredEditMode:
                s_exitingPlay = false;
                s_userOverride = false;
                s_takeCount = 0;
                Restore("left Play", quiet: true);   // backstop
                break;
            case PlayModeStateChange.EnteredPlayMode:
                s_exitingPlay = false;
                s_userOverride = false;
                s_takeCount = 0;
                break;
            }
        }

        static void OnBeforeAssemblyReload() { Restore("domain reload", quiet: !(s_active || s_took)); }
        static void OnQuitting() { Restore("editor quitting", quiet: true); }

        /// <summary>False with <c>DISPLAYXR_PROV_GV_TAKEOVER=0</c>: a zoomed Game view is
        /// left as it is (the driver shows it scaled and warns).</summary>
        static bool TakeoverEnabled()
        {
            if (s_takeoverGate < 0)
                s_takeoverGate = Environment.GetEnvironmentVariable("DISPLAYXR_PROV_GV_TAKEOVER") == "0" ? 0 : 1;
            return s_takeoverGate == 1;
        }

        // Once per editor load (and per domain reload): undo what an earlier session left.
        static void HealOnLoad()
        {
            if (s_active || s_took) return;
            // 1. A zoom GVFIT stretched, saved with the layout (older plug-ins did not put it
            //    back, and a killed editor cannot).
            int fixedZoom = RestoreFit();
            if (fixedZoom > 0)
                Debug.Log("[DisplayXR] Game view: put Unity's uniform zoom back on " + fixedZoom +
                          " Game view(s) (a row stretch from an earlier Play was saved with the layout)");
            // 2. A selection past the end of the size list (our entry was selected when the
            //    editor died; the entry itself was never written to disk).
            try
            {
                object group = CurrentGroup(out Type groupType);
                int total = (int)Method(groupType, "GetTotalCount").Invoke(group, null);
                Type sizesType = EditorType("UnityEditor.GameViewSizes");
                int groupIndex = Convert.ToInt32(Property(sizesType, "currentGroupType")
                    .GetValue(Property(sizesType, "instance").GetValue(null, null), null));
                foreach (EditorWindow v in GameViews())
                {
                    FieldInfo raw = Field(v.GetType(), "m_SelectedSizes");
                    if (raw == null || !(raw.GetValue(v) is int[] sel)) continue;
                    if (groupIndex < 0 || groupIndex >= sel.Length || sel[groupIndex] < total) continue;
                    int was = sel[groupIndex];
                    SelectSize(v, 0);
                    v.Repaint();
                    Debug.Log("[DisplayXR] Game view: its selected size index (" + was + ") was past the end of the " +
                              "size list (" + total + " sizes) - selected Free Aspect");
                }
            }
            catch (Exception) { /* best-effort; Unity clamps such an index itself */ }
            // 3. A record of a takeover that was never restored, and / or our size entry.
            Restore("left over from an earlier session", quiet: true, heal: true);
        }

        static void Poll()
        {
            double now = EditorApplication.timeSinceStartup;
            if (now < s_nextPoll) return;
            s_nextPoll = now + kPollGap;

            if (!EditorApplication.isPlaying || s_exitingPlay)
            {
                if (s_active || s_took) Restore("not playing");
                return;
            }
            if (s_disabled || s_userOverride) return;
            // The subsystem is restarting (dock change, re-host): hold what is selected.
            if (!DisplayXRProviderDriver.IsActive) return;
            var view = DisplayXRProviderDriver.MatchedGameView as EditorWindow;
            float scale = DisplayXRProviderDriver.MatchedPaneScale;
            if (view == null || !(scale > 0f)) return;

            try
            {
                if ((s_active || s_took) && s_view != view) Restore("the main Game view changed");
                // A view that cannot be 1:1 is put right first; the whole-pixel size (if it is
                // on) follows on a later poll, from a view at zoom 1.
                if (TakeoverEnabled() && TakeoverTick(view, scale)) return;
                if (DisplayXRProviderDriver.WholePixelEnabled()) Tick(view, scale, now);
            }
            catch (Exception e)
            {
                s_disabled = true;
                WarnOnce("could not set the Game view's size or zoom (editor internals changed?), " +
                         "so it is left alone for this session: " + Unwrap(e).Message);
                // Unconditionally: Apply sets s_active last, so a throw after AddCustomSize or
                // after the selection write (e.g. SnapZoom missing) leaves our size behind, and
                // selected, with GVFIT still stretching it.
                Restore("reflection failure", quiet: true);
            }
        }

        // (#459) A docked Game view whose zoom is not 1 cannot show 3D: take it over for the
        // Play session. True when something was changed (the caller waits for the next poll).
        static bool TakeoverTick(EditorWindow view, float scale)
        {
            // Floating = present mode: the runtime's own window covers the view; nothing to do.
            if (!DisplayXRProviderDriver.LastAppliedDocked) return false;
            if (!DisplayXRProviderDriver.TryGetGameViewZoom(view, out Vector2 zoom)) return false;
            if (DisplayXRGameViewPlacement.IsZoomOne(zoom.x)) return false;

            if (s_takeCount >= 2)
            {
                // It did not hold, or the user zoomed again: their choice wins until the next
                // Play (the driver shows the scaled picture and warns).
                s_userOverride = true;
                Debug.Log("[DisplayXR] Game view: its zoom is " + zoom.x.ToString("0.##", Inv) +
                          "x again after it was set to 1x - leaving it alone until the next Play (no 3D while it is scaled)");
                return true;
            }

            int current = GetSelectedIndex(view);
            string name = SizeText(current, out float fit, view, scale);
            var what = DisplayXRGameViewPlacement.TakeoverFor(zoom.x, fit);
            if (what == DisplayXRGameViewPlacement.Takeover.None) return false;

            // Marked first, so that a failure part-way is rolled back on this view.
            s_took = true; s_view = view; s_takeCount++;
            Record rec = EnsureRecord(view, current, zoom.x, name);   // on disk BEFORE any change
            DisplayXRProviderDriver.RestoreGameViewFit();             // uniform zoom first
            if (what == DisplayXRGameViewPlacement.Takeover.FreeAspect && current != 0)
            {
                rec.FreeAspect = true;
                SaveRecord(rec);
                SelectSize(view, 0);
            }
            SnapZoom(view, 1f);
            view.Repaint();
            if (s_takeCount == 1)
                Debug.Log("[DisplayXR] Game view: \"" + name + "\" was shown at " + zoom.x.ToString("0.##", Inv) +
                          "x, which cannot be 1:1 on the panel (no 3D) - " +
                          (what == DisplayXRGameViewPlacement.Takeover.FreeAspect
                              ? "switched to Free Aspect at Scale 1x for this Play session"
                              : "set Scale to 1x for this Play session") +
                          "; your size and zoom come back when Play ends (DISPLAYXR_PROV_GV_TAKEOVER=0 opts out)");
            return true;
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
                Method(groupType, "AddCustomSize", sizeType).Invoke(group, new[] { entry });
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
                // What to put back (the user's size and zoom; x: GVFIT only ever writes y),
                // on disk before the selection changes. A record that already exists (the
                // view was taken over from a zoomed size first) keeps the user's original.
                Record rec = EnsureRecord(view, current,
                    DisplayXRProviderDriver.TryGetGameViewZoom(view, out Vector2 zoom) ? zoom.x : 0f,
                    SizeText(current, out _, view, scale));
                rec.OursIndex = index;
                SaveRecord(rec);
            }
            if (current != index) Property(view.GetType(), "selectedSizeIndex").SetValue(view, index, null);
            // Zoom 1 on both axes (it also clears GVFIT's row stretch, which the driver stops
            // re-applying to this view from now on).
            SnapZoom(view, 1f);
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

        /// <summary>Put the user's size and zoom back on every Game view this class changed,
        /// undo GVFIT's row stretch, remove our size and delete the record. Safe to call when
        /// nothing of ours exists. <paramref name="heal"/>: called at editor load, with no
        /// live state - the record on disk and the size list are all there is to go on.</summary>
        static void Restore(string reason, bool quiet = false, bool heal = false)
        {
            EditorWindow taken = s_view;
            s_active = false; s_took = false; s_view = null; s_appliedWPx = s_appliedHPx = 0; s_pendSince = -1.0;
            DisplayXRProviderDriver.WholePixelView = null;   // GVFIT and the default placement resume
            try
            {
                RestoreFit();   // Unity's uniform zoom first: SnapZoom below starts from it
                Record rec = LoadRecord();
                object group = CurrentGroup(out Type groupType);
                int index = FindEntry(group, groupType, out _);
                if (rec == null && index < 0) return;

                int total = (int)Method(groupType, "GetTotalCount").Invoke(group, null);
                Type sizesType = EditorType("UnityEditor.GameViewSizes");
                object sizes = Property(sizesType, "instance").GetValue(null, null);
                int groupIndex = Convert.ToInt32(Property(sizesType, "currentGroupType").GetValue(sizes, null));
                // The record is about one size group (the build target's); in another one its
                // index means nothing, so only our own entry is dealt with.
                bool recHere = rec != null && rec.Group == groupIndex;

                // Where the user's selection goes: the recorded index if it still names the
                // same size, Free Aspect otherwise. Resolved before our entry is removed.
                int prev = 0;
                if (recHere && rec.PrevIndex > 0 && rec.PrevIndex < total && rec.PrevIndex != index)
                {
                    string nameNow = SizeText(rec.PrevIndex, out _, null, 0f);
                    if (string.IsNullOrEmpty(rec.PrevName) || nameNow == rec.PrevName)
                        prev = rec.PrevIndex > index && index >= 0 ? rec.PrevIndex - 1 : rec.PrevIndex;
                }
                float prevZoom = recHere ? rec.PrevZoom : 0f;

                // Which views get it back. In a live session: the view that was taken over.
                // At editor load there is no such view: a view on our entry, or on the last
                // entry when ours (which was last) is gone and Unity clamped the index, is
                // certain; "Free Aspect because we selected it" is only certain when there is
                // a single Game view - with several, they stay on Free Aspect (Unity's
                // default: never a wrong or foreign size).
                var views = GameViews();
                if (heal && views.Count == 0 && rec != null)
                {
                    // No Game view is loaded yet: keep the record and look again shortly.
                    if (s_healTries++ < 50) EditorApplication.delayCall += HealOnLoad;
                    return;
                }
                var restoreSize = new System.Collections.Generic.List<EditorWindow>();
                var restoreZoom = new System.Collections.Generic.List<EditorWindow>();
                var shiftDown = new System.Collections.Generic.List<EditorWindow>();
                foreach (EditorWindow v in views)
                {
                    int sel = GetSelectedIndex(v);
                    bool mine = taken != null ? v == taken : views.Count == 1;
                    bool ours = index >= 0 && sel == index;
                    bool clamped = heal && recHere && index < 0 && rec.OursIndex >= total && sel == total - 1;
                    bool freed = recHere && rec.FreeAspect && sel == 0 && mine;
                    bool zoomOnly = recHere && !rec.FreeAspect && rec.OursIndex < 0 && sel == rec.PrevIndex && mine;
                    if (ours || clamped || freed) restoreSize.Add(v);
                    else if (zoomOnly) restoreZoom.Add(v);
                    else if (index >= 0 && sel > index) shiftDown.Add(v);
                }

                // Remove every entry of our name (a crash can leave one behind, and a later
                // session add another). RemoveCustomSize takes the TOTAL index (it converts
                // with TotalIndexToCustomIndex itself; read from the 6000.4 editor assembly). A
                // variant taking the index within the custom list is tried only if the entry is
                // still there, and the first call is guarded so that, under that other meaning,
                // it could only be out of range, never another custom size.
                bool removed = false;
                if (index >= 0)
                {
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
                    removed = true;
                }

                // The selections, now that the list is final (prev is an index into it).
                foreach (EditorWindow v in shiftDown)
                {
                    int sel = RawSelectedIndex(v, groupIndex);
                    if (sel > 0) Property(v.GetType(), "selectedSizeIndex").SetValue(v, sel - 1, null);
                    v.Repaint();
                }
                foreach (EditorWindow v in restoreSize)
                {
                    SelectSize(v, prev);
                    // The zoom the view had before (uniform; at zoom 1 GVFIT re-applies its
                    // row stretch on the next Play frame). Best-effort: the size matters more.
                    if (prevZoom > 0f) { try { SnapZoom(v, prevZoom); } catch (Exception) { } }
                    v.Repaint();
                }
                foreach (EditorWindow v in restoreZoom)
                {
                    if (prevZoom > 0f) { try { SnapZoom(v, prevZoom); } catch (Exception) { } }
                    v.Repaint();
                }
                DeleteRecord();

                // Add / RemoveCustomSize only bump a change id; the list is written to the
                // per-user, all-projects file by Unity's size menu. Write it now, without our
                // entry, in case such a save happened while it was in the list.
                if (removed)
                {
                    try { Method(sizesType, "SaveToHDD").Invoke(sizes, null); }
                    catch (Exception) { /* best-effort: the entry is gone from this editor either way */ }
                }
                bool did = removed || restoreSize.Count > 0 || restoreZoom.Count > 0;
                if (did && (!quiet || heal))
                    Debug.Log("[DisplayXR] Game view: restored the previous size (index " + prev +
                              (recHere && !string.IsNullOrEmpty(rec.PrevName) ? ", \"" + rec.PrevName + "\"" : "") + ")" +
                              (prevZoom > 0f ? " and zoom " + prevZoom.ToString("0.###", Inv) + "x" : "") +
                              " on " + (restoreSize.Count + restoreZoom.Count) + " Game view(s)" +
                              (removed ? " and removed \"" + SizeName + "\"" : "") + " - " + reason);
            }
            catch (Exception e)
            {
                if (!quiet || heal)
                    WarnOnce("could not put the Game view's size back (" + reason + "); select Free Aspect in the " +
                             "Game view's size menu and delete \"" + SizeName + "\" there if it is listed: " + Unwrap(e).Message);
            }
        }

        // Undo GVFIT's row stretch on every Game view (the driver's own restore) and repaint
        // the ones it changed. Never throws.
        static int RestoreFit()
        {
            int n = 0;
            try
            {
                n = DisplayXRProviderDriver.RestoreGameViewFit();
                if (n > 0) foreach (EditorWindow v in GameViews()) v.Repaint();
            }
            catch (Exception) { }
            return n;
        }

        // ---- the durable record (Library/, one per project) ----

        sealed class Record
        {
            public int    PrevIndex;          // the user's selected size (total index)
            public string PrevName = "";      // its display text, to recognise it again
            public float  PrevZoom;           // the user's zoom (0 = unknown)
            public int    Group;              // GameViewSizeGroupType it belongs to
            public bool   FreeAspect;         // we selected Free Aspect on the view
            public int    OursIndex = -1;     // total index our custom size was selected at (-1 = never)
        }
        static Record s_rec;
        static bool   s_recLoaded;
        static readonly System.Globalization.CultureInfo Inv = System.Globalization.CultureInfo.InvariantCulture;

        static string RecordPath() =>
            System.IO.Path.Combine(System.IO.Directory.GetCurrentDirectory(), "Library", kRecordFile);

        static Record LoadRecord()
        {
            if (s_recLoaded) return s_rec;
            s_recLoaded = true; s_rec = null;
            try
            {
                string path = RecordPath();
                if (!System.IO.File.Exists(path)) return null;
                var r = new Record();
                bool any = false;
                foreach (string line in System.IO.File.ReadAllLines(path))
                {
                    int eq = line.IndexOf('=');
                    if (eq <= 0) continue;
                    string k = line.Substring(0, eq), val = line.Substring(eq + 1);
                    switch (k)
                    {
                    case "prevIndex":  any |= int.TryParse(val, System.Globalization.NumberStyles.Integer, Inv, out r.PrevIndex); break;
                    case "prevName":   r.PrevName = val; break;
                    case "prevZoom":   float.TryParse(val, System.Globalization.NumberStyles.Float, Inv, out r.PrevZoom); break;
                    case "group":      int.TryParse(val, System.Globalization.NumberStyles.Integer, Inv, out r.Group); break;
                    case "freeAspect": r.FreeAspect = val == "1"; break;
                    case "oursIndex":  int.TryParse(val, System.Globalization.NumberStyles.Integer, Inv, out r.OursIndex); break;
                    }
                }
                if (any) s_rec = r;
                else System.IO.File.Delete(path);   // unreadable: nothing to restore from
            }
            catch (Exception) { s_rec = null; }
            return s_rec;
        }

        // Throws when the file cannot be written: the caller then changes nothing.
        static void SaveRecord(Record r)
        {
            string path = RecordPath();
            System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(path));
            System.IO.File.WriteAllText(path,
                "prevIndex=" + r.PrevIndex.ToString(Inv) + "\n" +
                "prevName=" + (r.PrevName ?? "").Replace("\n", " ").Replace("\r", " ") + "\n" +
                "prevZoom=" + r.PrevZoom.ToString("R", Inv) + "\n" +
                "group=" + r.Group.ToString(Inv) + "\n" +
                "freeAspect=" + (r.FreeAspect ? "1" : "0") + "\n" +
                "oursIndex=" + r.OursIndex.ToString(Inv) + "\n");
            s_rec = r; s_recLoaded = true;
        }

        static void DeleteRecord()
        {
            s_rec = null; s_recLoaded = true;
            try
            {
                string path = RecordPath();
                if (System.IO.File.Exists(path)) System.IO.File.Delete(path);
            }
            catch (Exception) { }
        }

        // The record of this takeover: the one already on disk (the user's ORIGINAL size and
        // zoom, from the first change of this Play), or a new one written now.
        static Record EnsureRecord(EditorWindow view, int currentIndex, float zoomX, string name)
        {
            Record rec = LoadRecord();
            if (rec != null) return rec;
            Type sizesType = EditorType("UnityEditor.GameViewSizes");
            object sizes = Property(sizesType, "instance").GetValue(null, null);
            rec = new Record
            {
                PrevIndex = currentIndex, PrevName = name ?? "", PrevZoom = zoomX,
                Group = Convert.ToInt32(Property(sizesType, "currentGroupType").GetValue(sizes, null)),
            };
            SaveRecord(rec);
            return rec;
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

        // A field by name up the hierarchy, or null.
        static FieldInfo Field(Type type, string name)
        {
            for (Type t = type; t != null; t = t.BaseType)
            {
                FieldInfo f = t.GetField(name, kAny | BindingFlags.DeclaredOnly);
                if (f != null) return f;
            }
            return null;
        }

        // GameView.m_SelectedSizes[groupIndex], raw (the property clamps it first); -1 if not found.
        static int RawSelectedIndex(EditorWindow view, int groupIndex)
        {
            FieldInfo f = Field(view.GetType(), "m_SelectedSizes");
            if (f != null && f.GetValue(view) is int[] a && groupIndex >= 0 && groupIndex < a.Length) return a[groupIndex];
            return -1;
        }

        static System.Collections.Generic.List<EditorWindow> GameViews()
        {
            var list = new System.Collections.Generic.List<EditorWindow>();
            foreach (UnityEngine.Object o in Resources.FindObjectsOfTypeAll(EditorType("UnityEditor.GameView")))
                if (o is EditorWindow w && w != null) list.Add(w);
            return list;
        }

        // Select a size the way the size menu does: GameView.SizeSelectionCallback(int, object)
        // also re-fits the zoom area and the render target size, which the bare
        // selectedSizeIndex setter does not. The setter is the fallback.
        static void SelectSize(EditorWindow view, int index)
        {
            MethodInfo cb = null;
            for (Type t = view.GetType(); t != null && cb == null; t = t.BaseType)
                cb = t.GetMethod("SizeSelectionCallback", kAny | BindingFlags.DeclaredOnly, null,
                                 new[] { typeof(int), typeof(object) }, null);
            if (cb != null) cb.Invoke(view, new object[] { index, null });
            else Property(view.GetType(), "selectedSizeIndex").SetValue(view, index, null);
        }

        // GameView.SnapZoom(float): a uniform zoom, as the Scale slider sets it.
        static void SnapZoom(EditorWindow view, float zoom) =>
            Method(view.GetType(), "SnapZoom", typeof(float)).Invoke(view, new object[] { zoom });

        // The display text of the size at a total index ("4K UHD (3840x2160)", "Free Aspect").
        // fit (only with a view): the largest zoom at which it still fits the view's area -
        // below 1 for a Fixed Resolution larger than the area, 1 for everything else.
        static string SizeText(int index, out float fit, EditorWindow view, float scale)
        {
            fit = 1f;
            object group = CurrentGroup(out Type groupType);
            int total = (int)Method(groupType, "GetTotalCount").Invoke(group, null);
            if (index < 0 || index >= total) return "";
            object size = Method(groupType, "GetGameViewSize", typeof(int)).Invoke(group, new object[] { index });
            if (size == null) return "";
            Type st = size.GetType();
            if (view != null && scale > 0f && Property(st, "sizeType").GetValue(size, null).ToString() == "FixedResolution")
            {
                int w = (int)Property(st, "width").GetValue(size, null), h = (int)Property(st, "height").GetValue(size, null);
                Vector2 area = ViewAreaPoints(view) * scale;
                if (w > 0 && h > 0) fit = Mathf.Min(area.x / w, area.y / h);
            }
            string text = Property(st, "displayText").GetValue(size, null) as string;
            return string.IsNullOrEmpty(text) ? "size " + index : text;
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

        static string Percent(float scale) =>
            (scale * 100f).ToString("0.#", System.Globalization.CultureInfo.InvariantCulture) + " %";

        static Exception Unwrap(Exception e) =>
            e is TargetInvocationException && e.InnerException != null ? e.InnerException : e;

        static void WarnOnce(string message)
        {
            // Once per editor session: statics do not survive the domain reload of each Play.
            if (SessionState.GetBool(kSessWarned, false)) return;
            SessionState.SetBool(kSessWarned, true);
            Debug.LogWarning("[DisplayXR] Game view size: " + message);
        }
#endif // UNITY_EDITOR_WIN
    }
}
