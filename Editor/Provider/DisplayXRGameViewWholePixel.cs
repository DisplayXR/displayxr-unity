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
    /// What it restores, and when: the pre-Play size, zoom and scroll are put back and the
    /// custom size removed on leaving Play, before a domain reload (it is re-applied afterwards
    /// if Play continues), on quitting and when the main view changes - and at no other time:
    /// never in the middle of Play. If the user picks another size (or zooms, twice) during
    /// Play, that choice is left alone until Play ends; then the pre-Play state comes back
    /// all the same. A failed apply is rolled back. Unity keeps custom sizes in a per-user
    /// list shared by all projects and writes it only from its own size menu, so after
    /// removing ours the list is saved (best-effort): otherwise a save the user triggered
    /// while the size was on would keep our entry for every project.
    ///
    /// ONE OWNER. What is held on the view and what happens next is decided by
    /// <see cref="DisplayXRGameViewOwner"/> (pure C#, unit-tested): every selection made
    /// here is recorded there, so only a selection that differs from it is a user pick, and
    /// a held whole-pixel size that stops fitting (the pane shrank) is re-fitted at once.
    /// This class observes the view, carries the steps out and owns the restore.
    ///
    /// The 0.35 s resize debounce: the size follows the view AREA, which a splitter drag
    /// changes every frame; a new size is applied only once the area has held for
    /// <see cref="DisplayXRProviderDriver.GameViewSettleSeconds"/>, so a growing drag costs one
    /// Game-view size change, which the driver's own debounce then pushes once. A SHRINKING
    /// drag is followed at every poll: the held size no longer fits, Unity would show it
    /// scaled, and a scaled view has no 3D.
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
    /// zoom 1 (<see cref="DisplayXRGameViewPlacement.TakeoverFor"/>). The user's size, zoom
    /// and scroll come back with the same Restore as above. <c>DISPLAYXR_PROV_GV_TAKEOVER=0</c>
    /// opts out; the driver then shows the scaled picture and warns once.
    ///
    /// THE RESTORE IS VERIFIED. Setting a size and a zoom in one call does not stick by
    /// itself: GameView.SizeSelectionCallback does nothing when the index is already the
    /// (clamped) selected one, and the zoom area's limits (ZoomableArea.hScaleMin / vScaleMin)
    /// are only recomputed for the new size in GameView.ConfigureZoomArea, so a zoom set
    /// before that is clamped to the OLD size's minimum - 1x for the whole-pixel size, which
    /// showed a restored 4K UHD at 1x instead of 0.28x. So the restore selects the size, calls
    /// GameView.UpdateZoomAreaAndParent, sets zoom and scroll through ZoomableArea, and then
    /// reads all three back over the following editor frames, re-applying until they hold
    /// (or warning with the values actually read).
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
    /// GameView.selectedSizeIndex / SizeSelectionCallback / SnapZoom / UpdateZoomAreaAndParent /
    /// viewInWindow / m_ZoomArea / m_SelectedSizes, ZoomableArea.SetTransform) by reflection,
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

        static readonly DisplayXRGameViewOwner s_owner = new DisplayXRGameViewOwner
            { SettleSeconds = DisplayXRProviderDriver.GameViewSettleSeconds };
        static EditorWindow s_view;            // the view the owner holds (null = none)
        static int          s_takeoverGate = -1;
        static int          s_healTries;
        static double       s_nextPoll;
        static bool         s_exitingPlay;
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

        static bool Live => s_view != null || s_owner.Changed;

        static void OnPlayModeChanged(PlayModeStateChange change)
        {
            switch (change)
            {
            case PlayModeStateChange.ExitingPlayMode:
                s_exitingPlay = true;
                Restore("leaving Play", quiet: !Live);
                break;
            case PlayModeStateChange.EnteredEditMode:
                s_exitingPlay = false;
                Restore("left Play", quiet: true);   // backstop
                break;
            case PlayModeStateChange.ExitingEditMode:
                s_verify.Clear();                    // a new Play: the view is about to change again
                break;
            case PlayModeStateChange.EnteredPlayMode:
                s_exitingPlay = false;
                break;
            }
        }

        static void OnBeforeAssemblyReload() { Restore("domain reload", quiet: !Live); }
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
            if (Live) return;
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
                int groupIndex = CurrentGroupIndex();
                foreach (EditorWindow v in GameViews())
                {
                    int was = RawSelectedIndex(v, groupIndex);
                    if (was < total) continue;
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

            VerifyTick();
            if (!EditorApplication.isPlaying || s_exitingPlay)
            {
                if (Live) Restore("not playing");
                return;
            }
            if (s_disabled) return;
            // The subsystem is restarting (dock change, re-host): hold what is selected.
            if (!DisplayXRProviderDriver.IsActive) return;
            var view = DisplayXRProviderDriver.MatchedGameView as EditorWindow;
            float scale = DisplayXRProviderDriver.MatchedPaneScale;
            if (view == null || !(scale > 0f)) return;

            try
            {
                if (s_view != null && s_view != view) Restore("the main Game view changed");
                Tick(view, scale, now);
            }
            catch (Exception e)
            {
                s_disabled = true;
                WarnOnce("could not set the Game view's size or zoom (editor internals changed?), " +
                         "so it is left alone for this session: " + Unwrap(e).Message);
                // Whatever was changed before the failure (an entry added, a selection
                // written) is rolled back from the record.
                Restore("reflection failure", quiet: true);
            }
        }

        // One look at the view, one step of the owner, carried out.
        static void Tick(EditorWindow view, float scale, double now)
        {
            object group = CurrentGroup(out Type groupType);
            int selected = GetSelectedIndex(view);
            Vector2 area = ViewAreaPoints(view);
            bool haveZoom = DisplayXRProviderDriver.TryGetGameViewZoom(view, out Vector2 zoom);
            string name = SizeText(selected, out float fit, view, scale);
            bool wholePixelOn = DisplayXRProviderDriver.WholePixelEnabled();
            var obs = new DisplayXRGameViewOwner.Obs
            {
                Selected = selected,
                OursIndex = FindEntry(group, groupType, out _),
                ZoomX = haveZoom ? zoom.x : 0f,
                FitScale = fit,
                Scale = scale,
                AreaWPt = area.x, AreaHPt = area.y,
                WholePixelOn = wholePixelOn,
                // Floating = present mode: the runtime's own window covers the view, so a
                // zoomed view needs nothing there.
                TakeoverOn = TakeoverEnabled() && DisplayXRProviderDriver.LastAppliedDocked,
                Now = now,
            };
            DisplayXRGameViewOwner.Holds heldBefore = s_owner.Held;
            DisplayXRGameViewOwner.Step step = s_owner.Next(obs);

            if (step.Kind == DisplayXRGameViewOwner.Act.None)
            {
                if (wholePixelOn && s_owner.Held == DisplayXRGameViewOwner.Holds.Nothing && !s_owner.UserPicked &&
                    DisplayXRGameViewPlacement.WholePixelStepPt(scale) <= 1 && scale != s_loggedIdleScale)
                {
                    s_loggedIdleScale = scale;
                    int q = DisplayXRGameViewPlacement.WholePixelStepPt(scale);
                    Debug.Log("[DisplayXR] whole-pixel Game view: nothing to do at " + Percent(scale) +
                              (q == 1 ? " (a point is a whole number of pixels; the selected size is kept)"
                                      : " (the measured scale is not a whole percent)"));
                }
                return;
            }

            if (step.Kind == DisplayXRGameViewOwner.Act.StandDown)
            {
                // The user's choice: leave the view exactly as it is. The pre-Play size is NOT
                // put back now (it may be one that cannot be woven); it comes back when Play ends.
                DisplayXRProviderDriver.WholePixelView = null;
                Debug.Log("[DisplayXR] Game view: " + step.Why + " during Play - the plug-in leaves the Game view alone " +
                          "until Play ends (now \"" + name + "\" at " + (haveZoom ? zoom.x.ToString("0.###", Inv) + "x" : "?") +
                          "); the size, zoom and scroll from before Play come back then");
                return;
            }

            // A change: what is to come back is on disk BEFORE it, and the view is ours from
            // here on, so that a failure part-way is rolled back on this view.
            s_view = view;
            Record rec = EnsureRecord(view, selected, haveZoom ? zoom.x : 0f, name);
            DisplayXRProviderDriver.RestoreGameViewFit();   // Unity's uniform zoom first

            switch (step.Kind)
            {
            case DisplayXRGameViewOwner.Act.ZoomOne:
                RefreshZoomArea(view);
                SnapZoom(view, 1f);
                break;

            case DisplayXRGameViewOwner.Act.FreeAspect:
                if (!rec.FreeAspect) { rec.FreeAspect = true; SaveRecord(rec); }
                DisplayXRProviderDriver.WholePixelView = null;
                SelectSize(view, 0);
                RefreshZoomArea(view);
                SnapZoom(view, 1f);
                break;

            case DisplayXRGameViewOwner.Act.WholePixel:
            {
                int index = FindEntry(group, groupType, out object entry);
                Type sizeType = EditorType("UnityEditor.GameViewSize");
                if (entry == null)
                {
                    Type kind = EditorType("UnityEditor.GameViewSizeType");
                    entry = Activator.CreateInstance(sizeType, kAny, null,
                        new object[] { Enum.Parse(kind, "FixedResolution"), step.WPx, step.HPx, SizeName }, null);
                    Method(groupType, "AddCustomSize", sizeType).Invoke(group, new[] { entry });
                    // Found again by name: whether the group stores the instance or a copy, and what
                    // its IndexOf returns for a custom size, has varied between editor versions.
                    index = FindEntry(group, groupType, out entry);
                    if (index < 0) throw new InvalidOperationException("the custom size did not appear in the size group");
                }
                else
                {
                    PropertyInfo w = Property(sizeType, "width"), h = Property(sizeType, "height");
                    if ((int)w.GetValue(entry, null) != step.WPx) w.SetValue(entry, step.WPx, null);
                    if ((int)h.GetValue(entry, null) != step.HPx) h.SetValue(entry, step.HPx, null);
                }
                if (rec.OursIndex != index) { rec.OursIndex = index; SaveRecord(rec); }
                if (GetSelectedIndex(view) != index) Property(view.GetType(), "selectedSizeIndex").SetValue(view, index, null);
                // Zoom 1 on both axes, with the zoom limits of the size as it is NOW (it also
                // clears GVFIT's row stretch, which the driver does not apply to this view).
                RefreshZoomArea(view);
                SnapZoom(view, 1f);
                DisplayXRProviderDriver.WholePixelView = view;
                break;
            }
            }
            view.Repaint();
            int selectedAfter = GetSelectedIndex(view);
            s_owner.Applied(step, selectedAfter);

            switch (step.Kind)
            {
            case DisplayXRGameViewOwner.Act.WholePixel:
                Debug.Log("[DisplayXR] whole-pixel Game view: " +
                          (heldBefore == DisplayXRGameViewOwner.Holds.WholePixel ? "resized to " : "selected ") +
                          step.WPx + "x" + step.HPx + " px (" + step.WPt + "x" + step.HPt + " pt, steps of " +
                          DisplayXRGameViewPlacement.WholePixelStepPt(scale) + " pt = " +
                          Mathf.RoundToInt(DisplayXRGameViewPlacement.WholePixelStepPt(scale) * scale) + " px) at " + Percent(scale) +
                          " for a " + area.x.ToString("0.###", Inv) + "x" + area.y.ToString("0.###", Inv) + " pt area; zoom 1" +
                          (step.Refit ? "; " + step.Why + " (re-fitted without waiting)" : "") +
                          (heldBefore == DisplayXRGameViewOwner.Holds.WholePixel ? "" : "; previous size index " + selected));
                break;
            case DisplayXRGameViewOwner.Act.FreeAspect:
                Debug.Log("[DisplayXR] Game view: \"" + name + "\"" +
                          (haveZoom && !DisplayXRGameViewPlacement.IsZoomOne(zoom.x)
                              ? " was shown at " + zoom.x.ToString("0.##", Inv) + "x, which cannot be 1:1 on the panel (no 3D)" : "") +
                          " - switched to Free Aspect at Scale 1x for this Play session (" + step.Why +
                          "); your size, zoom and scroll come back when Play ends" +
                          (heldBefore == DisplayXRGameViewOwner.Holds.Nothing ? " (DISPLAYXR_PROV_GV_TAKEOVER=0 opts out)" : ""));
                break;
            case DisplayXRGameViewOwner.Act.ZoomOne:
                Debug.Log("[DisplayXR] Game view: \"" + name + "\" was shown at " + (haveZoom ? zoom.x.ToString("0.##", Inv) : "?") +
                          "x, which cannot be 1:1 on the panel (no 3D) - set Scale to 1x for this Play session; " +
                          "your zoom and scroll come back when Play ends" +
                          (heldBefore == DisplayXRGameViewOwner.Holds.Nothing ? " (DISPLAYXR_PROV_GV_TAKEOVER=0 opts out)" : ""));
                break;
            }
        }

        /// <summary>Put the pre-Play size, zoom and scroll back on the Game view this class
        /// changed, undo GVFIT's row stretch, remove our size and delete the record. Safe to
        /// call when nothing of ours exists. <paramref name="heal"/>: called at editor load,
        /// with no live state - the record on disk and the size list are all there is to go
        /// on. The result is read back over the next editor frames (VerifyTick).</summary>
        static void Restore(string reason, bool quiet = false, bool heal = false)
        {
            EditorWindow taken = s_view;
            s_owner.Reset(); s_view = null;
            DisplayXRProviderDriver.WholePixelView = null;   // GVFIT and the default placement resume
            try
            {
                RestoreFit();   // Unity's uniform zoom first
                Record rec = LoadRecord();
                object group = CurrentGroup(out Type groupType);
                int index = FindEntry(group, groupType, out _);
                if (rec == null && index < 0) return;

                int total = (int)Method(groupType, "GetTotalCount").Invoke(group, null);
                Type sizesType = EditorType("UnityEditor.GameViewSizes");
                object sizes = Property(sizesType, "instance").GetValue(null, null);
                int groupIndex = CurrentGroupIndex();
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

                // Which views get it back. In a live session: the view that was changed,
                // whatever is selected on it now (a size the user picked during Play gives
                // way to the pre-Play one when Play ends). At editor load there is no such
                // view: a single Game view is it; with several, a view on our entry, or on the
                // last entry when ours (which was last) is gone and Unity clamped the index,
                // is certain, and the others are left as they are (Free Aspect, if we
                // selected it: Unity's default, never a wrong or foreign size).
                var views = GameViews();
                if (heal && views.Count == 0 && rec != null)
                {
                    // No Game view is loaded yet: keep the record and look again shortly.
                    if (s_healTries++ < 50) EditorApplication.delayCall += HealOnLoad;
                    return;
                }
                var restore = new System.Collections.Generic.List<EditorWindow>();
                var shiftDown = new System.Collections.Generic.List<EditorWindow>();
                foreach (EditorWindow v in views)
                {
                    int sel = GetSelectedIndex(v);
                    bool mine = taken != null ? v == taken : views.Count == 1;
                    bool ours = index >= 0 && sel == index;
                    bool clamped = heal && recHere && index < 0 && rec.OursIndex >= total && sel == total - 1;
                    if ((recHere && mine) || ours || clamped) restore.Add(v);
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
                var want = new ViewState
                {
                    Index = prev,
                    Name = recHere ? rec.PrevName : "",
                    Zoom = recHere ? rec.PrevZoom : 0f,
                    HaveScroll = recHere && rec.HaveScroll,
                    Scroll = recHere ? new Vector2(rec.ScrollX, rec.ScrollY) : Vector2.zero,
                    Area = recHere ? new Vector2(rec.AreaW, rec.AreaH) : Vector2.zero,
                };
                foreach (EditorWindow v in restore)
                {
                    ApplyViewState(v, want);
                    // Read back over the next frames; not when the domain or the editor is
                    // about to go (nothing would run), where this one application has to do.
                    if (reason != "domain reload" && reason != "editor quitting")
                        s_verify.Add(new Pending { View = v, Want = want, Reason = reason, Removed = removed });
                    else if (!quiet)
                        Debug.Log("[DisplayXR] Game view: restored the previous size and zoom - " + reason + ": now " + Describe(v));
                }
                DeleteRecord();

                // Add / RemoveCustomSize only bump a change id; the list is written to the
                // per-user, all-projects file by Unity's size menu. Write it now, without our
                // entry, in case such a save happened while it was in the list.
                if (removed)
                {
                    try { Method(sizesType, "SaveToHDD").Invoke(sizes, null); }
                    catch (Exception) { /* best-effort: the entry is gone from this editor either way */ }
                    if (restore.Count == 0 && (!quiet || heal))
                        Debug.Log("[DisplayXR] Game view: removed \"" + SizeName + "\" - " + reason);
                }
            }
            catch (Exception e)
            {
                if (!quiet || heal)
                    WarnOnce("could not put the Game view's size back (" + reason + "); select Free Aspect in the " +
                             "Game view's size menu and delete \"" + SizeName + "\" there if it is listed: " + Unwrap(e).Message);
            }
        }

        // ---- applying a view state, and checking that it held ----

        struct ViewState
        {
            public int Index; public string Name;
            public float Zoom;              // 0 = leave the zoom alone
            public bool HaveScroll; public Vector2 Scroll;   // ZoomableArea.m_Translation
            public Vector2 Area;            // the view area the scroll was recorded in, points (zero = unknown)
        }

        sealed class Pending
        {
            public EditorWindow View; public ViewState Want; public string Reason; public bool Removed;
            public int Checks, Good, Reapplied;
        }
        static readonly System.Collections.Generic.List<Pending> s_verify = new System.Collections.Generic.List<Pending>();
        const int kVerifyGood = 3;      // consecutive checks (0.1 s apart, a repaint between) that must agree
        const int kVerifyMax = 30;      // then give up and say what the view really shows
        const float kScrollTolPt = 0.51f;

        // The recorded scroll is only one Unity will hold while the view area is the size it
        // was recorded in (DisplayXRGameViewPlacement.ScrollIsComparable): after a splitter
        // drag during Play, Unity re-centres / clamps it for the new area on every OnGUI.
        static bool ScrollApplies(EditorWindow v, in ViewState want)
        {
            if (!want.HaveScroll) return false;
            Vector2 now = ViewAreaPoints(v);
            return DisplayXRGameViewPlacement.ScrollIsComparable(want.Area.x, want.Area.y, now.x, now.y);
        }

        // Size, then the zoom area re-fitted to THAT size, then zoom and scroll. The order
        // matters: see "THE RESTORE IS VERIFIED" at the top.
        static void ApplyViewState(EditorWindow v, in ViewState want)
        {
            SelectSize(v, want.Index);
            if (GetSelectedIndex(v) != want.Index)
                Property(v.GetType(), "selectedSizeIndex").SetValue(v, want.Index, null);
            RefreshZoomArea(v);
            if (want.Zoom > 0f)
            {
                bool set = false;
                if (ScrollApplies(v, want))
                    set = DisplayXRProviderDriver.SetGameViewZoomState(v, want.Zoom, want.Scroll);
                if (!set) { try { SnapZoom(v, want.Zoom); } catch (Exception) { } }
            }
            v.Repaint();
        }

        static bool Matches(EditorWindow v, in ViewState want, out string why)
        {
            why = null;
            int sel = GetSelectedIndex(v);
            if (sel != want.Index) { why = "size index " + sel + " instead of " + want.Index; return false; }
            if (want.Zoom > 0f)
            {
                if (!DisplayXRProviderDriver.TryGetGameViewZoomState(v, out Vector2 zoom, out Vector2 scroll))
                { why = "the zoom cannot be read"; return false; }
                float tol = 1e-4f * Mathf.Max(1f, want.Zoom);
                if (Mathf.Abs(zoom.x - want.Zoom) > tol || Mathf.Abs(zoom.y - want.Zoom) > tol)
                { why = "zoom (" + zoom.x.ToString("0.####", Inv) + "," + zoom.y.ToString("0.####", Inv) + ") instead of " + want.Zoom.ToString("0.####", Inv); return false; }
                if (ScrollApplies(v, want) && (Mathf.Abs(scroll.x - want.Scroll.x) > kScrollTolPt || Mathf.Abs(scroll.y - want.Scroll.y) > kScrollTolPt))
                { why = "scroll (" + scroll.x.ToString("0.##", Inv) + "," + scroll.y.ToString("0.##", Inv) + ") instead of (" +
                        want.Scroll.x.ToString("0.##", Inv) + "," + want.Scroll.y.ToString("0.##", Inv) + ")"; return false; }
            }
            return true;
        }

        // "size index 6 "4K UHD (3840x2160)", zoom 0.2786x, scroll (12.5,-3)": what the view
        // shows NOW, read back from it.
        static string Describe(EditorWindow v)
        {
            try
            {
                int sel = GetSelectedIndex(v);
                string t = "size index " + sel + " \"" + SizeText(sel, out _, null, 0f) + "\"";
                if (DisplayXRProviderDriver.TryGetGameViewZoomState(v, out Vector2 zoom, out Vector2 scroll))
                    t += ", zoom " + zoom.x.ToString("0.####", Inv) + "x" +
                         (Mathf.Abs(zoom.y - zoom.x) > 1e-5f ? " (y " + zoom.y.ToString("0.####", Inv) + ")" : "") +
                         ", scroll (" + scroll.x.ToString("0.##", Inv) + "," + scroll.y.ToString("0.##", Inv) + ")";
                return t;
            }
            catch (Exception e) { return "unreadable (" + Unwrap(e).Message + ")"; }
        }

        // Every poll (0.1 s): read each restored view back; re-apply what did not hold.
        static void VerifyTick()
        {
            for (int i = s_verify.Count - 1; i >= 0; i--)
            {
                Pending p = s_verify[i];
                if (p.View == null) { s_verify.RemoveAt(i); continue; }
                try
                {
                    p.Checks++;
                    if (Matches(p.View, p.Want, out string why))
                    {
                        if (++p.Good < kVerifyGood) { p.View.Repaint(); continue; }
                        Debug.Log("[DisplayXR] Game view: restored the previous size and zoom" +
                                  (p.Removed ? " and removed \"" + SizeName + "\"" : "") + " - " + p.Reason +
                                  ": read back " + Describe(p.View) +
                                  (p.Reapplied > 0 ? " (it held after " + p.Reapplied + " re-application(s))" : "") +
                                  (p.Want.HaveScroll && !ScrollApplies(p.View, p.Want)
                                      ? " - the view area is " + ViewAreaPoints(p.View).x.ToString("0.##", Inv) + "x" +
                                        ViewAreaPoints(p.View).y.ToString("0.##", Inv) + " pt, it was " +
                                        p.Want.Area.x.ToString("0.##", Inv) + "x" + p.Want.Area.y.ToString("0.##", Inv) +
                                        " pt before Play, so the scroll from then (" + p.Want.Scroll.x.ToString("0.##", Inv) + "," +
                                        p.Want.Scroll.y.ToString("0.##", Inv) + ") does not apply and Unity placed it"
                                      : ""));
                        s_verify.RemoveAt(i);
                        continue;
                    }
                    p.Good = 0;
                    if (p.Checks >= kVerifyMax)
                    {
                        Debug.LogWarning("[DisplayXR] Game view: the size / zoom from before Play could not be put back (" + p.Reason +
                                         "): " + why + " after " + p.Reapplied + " re-applications. The view now shows " + Describe(p.View) +
                                         "; wanted size index " + p.Want.Index + " \"" + p.Want.Name + "\", zoom " +
                                         p.Want.Zoom.ToString("0.####", Inv) + "x" +
                                         (ScrollApplies(p.View, p.Want) ? ", scroll (" + p.Want.Scroll.x.ToString("0.##", Inv) + "," + p.Want.Scroll.y.ToString("0.##", Inv) + ")" : "") +
                                         ". Set it in the Game view's toolbar.");
                        s_verify.RemoveAt(i);
                        continue;
                    }
                    ApplyViewState(p.View, p.Want);
                    p.Reapplied++;
                }
                catch (Exception) { s_verify.RemoveAt(i); }
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
            public bool   HaveScroll;         // ScrollX / ScrollY are valid
            public float  ScrollX, ScrollY;   // the user's scroll (ZoomableArea.m_Translation, points)
            public float  AreaW, AreaH;       // the view area it was read in, points (0 = unknown)
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
                    case "area":
                    {
                        string[] wh = val.Split(',');
                        if (wh.Length != 2 || !float.TryParse(wh[0], System.Globalization.NumberStyles.Float, Inv, out r.AreaW)
                                           || !float.TryParse(wh[1], System.Globalization.NumberStyles.Float, Inv, out r.AreaH))
                            r.AreaW = r.AreaH = 0f;
                        break;
                    }
                    case "scroll":
                    {
                        string[] xy = val.Split(',');
                        r.HaveScroll = xy.Length == 2
                            && float.TryParse(xy[0], System.Globalization.NumberStyles.Float, Inv, out r.ScrollX)
                            && float.TryParse(xy[1], System.Globalization.NumberStyles.Float, Inv, out r.ScrollY);
                        break;
                    }
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
                "oursIndex=" + r.OursIndex.ToString(Inv) + "\n" +
                (r.HaveScroll ? "scroll=" + r.ScrollX.ToString("R", Inv) + "," + r.ScrollY.ToString("R", Inv) + "\n" : "") +
                (r.AreaW > 0f && r.AreaH > 0f ? "area=" + r.AreaW.ToString("R", Inv) + "," + r.AreaH.ToString("R", Inv) + "\n" : ""));
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
            rec = new Record
            {
                PrevIndex = currentIndex, PrevName = name ?? "", PrevZoom = zoomX, Group = CurrentGroupIndex(),
            };
            // The scroll is part of what the user sees (a zoomed-in view shows one corner).
            if (DisplayXRProviderDriver.TryGetGameViewZoomState(view, out _, out Vector2 scroll))
            {
                rec.HaveScroll = true; rec.ScrollX = scroll.x; rec.ScrollY = scroll.y;
                Vector2 area = ViewAreaPoints(view);
                rec.AreaW = area.x; rec.AreaH = area.y;
            }
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

        // GameViewSizes.instance.currentGroupType as an int (the index into m_SelectedSizes).
        static int CurrentGroupIndex()
        {
            Type sizesType = EditorType("UnityEditor.GameViewSizes");
            object sizes = Property(sizesType, "instance").GetValue(null, null);
            return Convert.ToInt32(Property(sizesType, "currentGroupType").GetValue(sizes, null));
        }

        // GameView.UpdateZoomAreaAndParent(): re-fits the zoom area to the size selected NOW
        // (ConfigureZoomArea: the area's rect, the content range and the min / max zoom) and
        // moves a zoom that sat on the old default to the new one. Without it the limits are
        // those of the size selected before, until the view's next OnGUI.
        static void RefreshZoomArea(EditorWindow view)
        {
            MethodInfo m = null;
            for (Type t = view.GetType(); t != null && m == null; t = t.BaseType)
                m = t.GetMethod("UpdateZoomAreaAndParent", kAny | BindingFlags.DeclaredOnly, null, Type.EmptyTypes, null);
            if (m == null)
                for (Type t = view.GetType(); t != null && m == null; t = t.BaseType)
                    m = t.GetMethod("ConfigureZoomArea", kAny | BindingFlags.DeclaredOnly, null, Type.EmptyTypes, null);
            if (m != null) m.Invoke(view, null);
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
