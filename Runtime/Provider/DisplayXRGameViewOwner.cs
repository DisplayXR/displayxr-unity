// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;

namespace DisplayXR
{
    /// <summary>
    /// (#459) The ONE owner of the editor Game view's size and zoom during Play: what the
    /// plug-in holds on the view, what it does next, and whether a change was the user's.
    /// Pure C# (no UnityEngine), unit-tested in <c>Tests~/GameViewPlacement</c>; the editor
    /// side (<c>DisplayXRGameViewWholePixel</c>) observes the view, carries the steps out and
    /// owns the restore.
    ///
    /// Why one owner. There used to be two mechanisms side by side - "take a zoomed view
    /// over (Free Aspect at 1x)" and "select the whole-pixel size" - each reading the view's
    /// selection on its own. On hardware (175 %): the whole-pixel size 1239x595 was selected,
    /// the user dragged a splitter and the pane became narrower than the size; Unity fitted
    /// the size (zoom below 1); the takeover saw "a fixed size that does not fit" and selected
    /// Free Aspect; the whole-pixel side saw a selection that was not its own, called it a
    /// user pick, switched itself off and put the pre-Play 4K size back in the middle of
    /// Play. No 3D from then on. Here every selection the plug-in makes is recorded in
    /// <see cref="Held"/>, so only a selection that differs from what it holds is a user
    /// pick; a held whole-pixel size that stops fitting is RE-FITTED, at once; and nothing
    /// in here ever asks for the pre-Play size back - that happens when Play ends, only.
    /// </summary>
    internal sealed class DisplayXRGameViewOwner
    {
        /// <summary>What the plug-in currently holds on the view.</summary>
        public enum Holds
        {
            Nothing,        // untouched
            Zoom,           // the user's own size, zoom set to 1
            FreeAspect,     // Free Aspect (index 0) at zoom 1
            WholePixel,     // the plug-in's own Fixed Resolution entry at zoom 1
        }

        public enum Act
        {
            None,
            ZoomOne,        // set the zoom to 1, keep the selected size
            FreeAspect,     // select Free Aspect, zoom 1
            WholePixel,     // create / resize the plug-in's entry to WPx x HPx, select it, zoom 1
            StandDown,      // the user picked a size or zoomed: leave the view alone until Play ends
        }

        /// <summary>One look at the view.</summary>
        public struct Obs
        {
            public int Selected;        // the view's selected size (total index)
            public int OursIndex;       // total index of the plug-in's entry, -1 = not in the list
            public float ZoomX;         // the view's zoom (0 = unknown, treated as 1)
            public float FitScale;      // largest zoom at which the SELECTED size fits the area (below 1: it does not fit)
            public float Scale;         // the pane's points-to-px (1.75 at 175 %)
            public float AreaWPt, AreaHPt;  // the view area, points
            public bool WholePixelOn;   // the whole-pixel size is wanted (forced, or asked for by a calibration)
            public bool TakeoverOn;     // a zoomed view may be taken over (not opted out, docked)
            public double Now;          // seconds
        }

        public struct Step
        {
            public Act Kind;
            public int WPx, HPx, WPt, HPt;  // WholePixel only
            public bool Refit;              // WholePixel: the held size stopped fitting (no settle wait)
            public bool ZoomTry;            // counts against MaxZoomTries
            public string Why;
        }

        /// <summary>How often a zoom that is not 1 is put right before it is taken to be the
        /// user's wish (or something the plug-in cannot hold).</summary>
        public const int MaxZoomTries = 2;

        /// <summary>How long the view area must hold before a NEW whole-pixel size is fitted
        /// to it (a splitter drag then costs one size change). A held size that no longer
        /// fits is re-fitted without waiting.</summary>
        public double SettleSeconds = 0.35;

        public Holds Held { get; private set; }
        /// <summary>The user picked a size or zoom during Play: nothing more is done.</summary>
        public bool UserPicked { get; private set; }
        /// <summary>The plug-in changed the view this Play: the pre-Play state is owed back.</summary>
        public bool Changed { get; private set; }
        public int AppliedWPx { get; private set; }
        public int AppliedHPx { get; private set; }

        int _heldIndex;
        int _zoomTries;
        float _pendW, _pendH, _pendScale;
        double _pendSince = -1.0;

        /// <summary>Play ended (or the view is no longer ours): forget everything.</summary>
        public void Reset()
        {
            Held = Holds.Nothing; UserPicked = false; Changed = false;
            AppliedWPx = AppliedHPx = 0; _heldIndex = 0; _zoomTries = 0; _pendSince = -1.0;
        }

        static Step Do(Act kind, string why, bool zoomTry = false) =>
            new Step { Kind = kind, Why = why, ZoomTry = zoomTry };

        /// <summary>What to do with the view as it is now. Changes no state except the
        /// settle clock and <see cref="UserPicked"/>; a step that was carried out is
        /// confirmed with <see cref="Applied"/>.</summary>
        public Step Next(in Obs o)
        {
            if (UserPicked) return Do(Act.None, null);

            // 1. Is the selection still the one the plug-in made? If not, the user picked.
            if (Held != Holds.Nothing)
            {
                int expect = Held == Holds.WholePixel ? o.OursIndex : _heldIndex;
                if (expect < 0 || o.Selected != expect)
                {
                    UserPicked = true;
                    return Do(Act.StandDown, expect < 0 ? "the plug-in's size was deleted from the size menu"
                                                        : "another Game view size was picked");
                }
            }

            // 2. A zoom that is not 1 cannot be 1:1.
            if (DisplayXRGameViewPlacement.IsScaledView(o.ZoomX))
            {
                bool fits = o.FitScale >= 1f - DisplayXRGameViewPlacement.ZoomOneTol;
                if (Held == Holds.WholePixel && !fits)
                {
                    // The pane shrank under the held size and Unity fitted it. Not a user
                    // action and not a reason to give the size up: fit it again, now.
                    if (DisplayXRGameViewPlacement.WholePixelSize(o.AreaWPt, o.AreaHPt, o.Scale,
                                                                  out int rwPt, out int rhPt, out int rwPx, out int rhPx))
                        return new Step { Kind = Act.WholePixel, WPx = rwPx, HPx = rhPx, WPt = rwPt, HPt = rhPt,
                                          Refit = true, Why = "the view became smaller than the whole-pixel size" };
                    return Do(Act.FreeAspect, "the view area is smaller than one whole-pixel step");
                }
                if (!o.TakeoverOn) return Do(Act.None, null);
                if (_zoomTries >= MaxZoomTries)
                {
                    UserPicked = true;
                    return Do(Act.StandDown, "the zoom was changed again after it was set to 1x");
                }
                return DisplayXRGameViewPlacement.TakeoverFor(o.ZoomX, o.FitScale) == DisplayXRGameViewPlacement.Takeover.FreeAspect
                    ? Do(Act.FreeAspect, "the selected size does not fit the view at 1x", zoomTry: true)
                    : Do(Act.ZoomOne, "the zoom is not 1", zoomTry: true);
            }

            // 3. Zoom 1: the whole-pixel size, where it is wanted and the scale needs one.
            if (!o.WholePixelOn) return Do(Act.None, null);
            int step = DisplayXRGameViewPlacement.WholePixelStepPt(o.Scale);
            if (step <= 1)
                return Held == Holds.WholePixel
                    ? Do(Act.FreeAspect, step == 1 ? "the pane is at a whole display scale" : "the pane's scale is not a whole percent")
                    : Do(Act.None, null);

            if (_pendSince < 0.0 || o.AreaWPt != _pendW || o.AreaHPt != _pendH || o.Scale != _pendScale)
            {
                _pendW = o.AreaWPt; _pendH = o.AreaHPt; _pendScale = o.Scale; _pendSince = o.Now;
                return Do(Act.None, null);   // (re)start the settle clock
            }
            if (o.Now - _pendSince < SettleSeconds) return Do(Act.None, null);

            if (!DisplayXRGameViewPlacement.WholePixelSize(o.AreaWPt, o.AreaHPt, o.Scale,
                                                           out int wPt, out int hPt, out int wPx, out int hPx))
                return Held == Holds.WholePixel ? Do(Act.FreeAspect, "the view area is smaller than one whole-pixel step")
                                                : Do(Act.None, null);
            if (Held == Holds.WholePixel && wPx == AppliedWPx && hPx == AppliedHPx) return Do(Act.None, null);
            return new Step { Kind = Act.WholePixel, WPx = wPx, HPx = hPx, WPt = wPt, HPt = hPt, Why = "the view area settled" };
        }

        /// <summary>The step was carried out; <paramref name="selectedAfter"/> is the view's
        /// selected index now. From here on that selection is the plug-in's own.</summary>
        public void Applied(in Step s, int selectedAfter)
        {
            if (s.ZoomTry) _zoomTries++;
            switch (s.Kind)
            {
            case Act.ZoomOne:
                if (Held == Holds.Nothing) { Held = Holds.Zoom; _heldIndex = selectedAfter; }
                Changed = true;
                break;
            case Act.FreeAspect:
                Held = Holds.FreeAspect; _heldIndex = selectedAfter; AppliedWPx = AppliedHPx = 0;
                Changed = true;
                break;
            case Act.WholePixel:
                Held = Holds.WholePixel; _heldIndex = selectedAfter; AppliedWPx = s.WPx; AppliedHPx = s.HPx;
                Changed = true;
                break;
            }
        }
    }
}
