# Editor Game view: pixel-exact placement of the woven image

In the editor the runtime's woven image is shown **inside the docked Game view** (weave-to-texture,
the default since v2.8.0). A lenticular weave is phase-sensitive: the image must reach the panel
texel for pixel, on the columns and rows the weaver assumed. One pixel of horizontal error is the
other eye.

This page says what has to hold, what the plug-in does about it, how the image's position is
**measured on screen** instead of computed, how to read the numeric diagnostics it logs, what the
whole-pixel size changes, what the arithmetic predicts is still wrong, and how to test all of it
at the panel (issues #420, #459).

Code: `Runtime/Provider/DisplayXRProviderDriver.cs` (the glue: reflection into the Game view,
Win32 rects, latches, logging), `Runtime/Provider/DisplayXRGameViewPlacement.cs` (the computed
placement, pure C#), `Runtime/Provider/DisplayXRGameViewCalibration.cs` (the calibration marker
and its decoding, pure C#), `Runtime/Provider/DisplayXRGameViewCalibrator.cs` (when to calibrate,
the screen read-back, the log line), `Editor/Provider/DisplayXRGameViewWholePixel.cs` (the
whole-pixel size), `native~/displayxr_xrprovider/displayxr_display_provider.cpp` (the marker, in
the native mirror blit) and `native~/displayxr_win32.c` (`displayxr_read_screen_strip`).
Tests: `Tests~/GameViewPlacement` and `Tests~/GameViewCalibration` (CI runs both with
`dotnet run --project ... -c Release`). In the placement suite the #420 checks are pinned to
hardware measurements; the #459 checks are unit tests of the arithmetic and model-consistency
checks, **not** hardware pins, and say so. The calibration suite decodes synthetic screens only.

## What has to hold

Four things, all in physical pixels of the pane's own monitor:

| # | Condition | If it fails |
|---|---|---|
| 1 | Unity draws the Game view's render texture into a rect **as wide as the texture** | a column is dropped or doubled; everything past it sits one pixel off: the eyes swap on one side of a vertical line |
| 2 | ...and **as high as the texture** | a row is dropped or doubled; through the slanted lens the interlace phase shifts on one side of a horizontal line (crosstalk, not a full swap) |
| 3 | The woven canvas the runtime renders is **the texture's size** | the native mirror blit stretches the canvas over the texture with a linear sampler: the interlace is resampled over the whole view |
| 4 | The invisible weave anchor sits on the screen pixel that shows the texture's **first column and row 0** | the whole view is uniformly out of phase (up to a full eye swap) |

How Unity sizes and draws it (read from the Unity 6000.4.0f1 editor assembly):

- The *target* size is the view area in pixels for Free Aspect (points x the view's scale, so it
  can be fractional), or the selected fixed size.
- The texture is the target rounded **half away from zero** on each axis
  (`PlayModeView.RenderView`).
- It is drawn into `targetInView`: the target size x the zoom area's scale, its x and (flipped) y
  rounded to whole points, point-filtered while the zoom is 1 or more.

So for a docked Free Aspect view (2 pt of side borders; 47 pt of tab, toolbar and bottom border):

| Windows scale | Area width | Area height | Texture vs area | Rows drawn under GVFIT (texture has `h`) |
|---|---|---|---|---|
| 100 / 200 / 300 % | whole | whole | equal | `h + 0.5` |
| 150 / 250 % | whole | x.5 | 0.5 row taller | `h` (exact) |
| 175 % | x.5 | x.75 | 0.5 column wider, 0.25 row taller | `h + 0.25` |
| 125 / 225 % | x.5 | x.25 | 0.5 column wider, 0.25 row shorter | `h + 0.75` |

This table is arithmetic. Unity computes points x scale in single precision, so a value that is
x.5 on paper can land a hair below it and round the other way. **The diagnostic below reads the
real texture and the real zoom, so its line is to be trusted where this table is not.**

## What the plug-in does today

- **Low Resolution Aspect Ratios off** on every Game view (`ForceGameViewFullResolution`), so the
  texture is physical pixels.
- **Rows (condition 2): GVFIT** (`ApplyGameViewFit`, default on, `DISPLAYXR_PROV_GV_FIT=off` to
  disable). Every frame it sets the zoom area's `m_Scale.y` to `h / (h - 0.5)` on every Game view
  **whose zoom is 1**, `h` being the texture height. The 0.5 is the half row of the 250 % dev
  panel; it is exact at 150 % and 250 % and nowhere else (F1). On a view at any other zoom it
  keeps `m_Scale.y` equal to `m_Scale.x` (see
  [A fixed-size or zoomed Game view](#a-fixed-size-or-zoomed-game-view)). What it wrote is put
  back (`RestoreGameViewFit`: `m_Scale.y = m_Scale.x` on every Game view) when Play ends, on
  dock / undock, before a domain reload, when the editor quits, and once at editor load.
- **Columns (condition 1): nothing.** Exact wherever the borders are whole pixels (F2).
- **Canvas size (condition 3):** the glue drives the canvas to `Mathf.RoundToInt` of Unity's main
  target size - halves to **even** - not to the texture's own size (F4).
- **Anchor (condition 4): measured, with the computed position as the fallback.** See
  [On-screen calibration](#on-screen-calibration-condition-4-measured). The computed position:
  `DisplayXRGameViewPlacement.Place`. Docked Free Aspect: X = pane left +
  half the pane-minus-texture slack (odd pixel to the left); Y = the row Unity's own rule puts the
  texture's row 0 on (`RtRow0Y`). Letterboxed: Unity's whole-point rounding of the centred rect.
  The 250 % reference (pane 1733x977 at (1017,293), texture 1728x860) gives **(1020,406)**,
  hardware-verified, pinned in the tests.
- **Which Game view:** the one Unity calls the main play-mode view
  (`PlayModeView.GetMainPlayModeView()`), falling back to the view whose width is closest to the
  main target size. With one Game view these are the same view. Unity's main view is the **last
  focused** play-mode view: with two Game views open, clicking the other one mid-Play makes it the
  main view, so the weave rebinds to it - and if its dock state differs (one docked, one floating)
  the re-host watcher restarts the display subsystem. GVFIT and the Low Resolution switch still
  apply to every Game view.
- **Which scale:** the pane's own points-to-pixels, measured as (pane width in physical px, read
  under per-monitor-v2 awareness) / (Unity's host width in points) and snapped to a whole percent.
  The global `EditorGUIUtility.pixelsPerPoint` is only valid inside `OnGUI`; the glue latches its
  first value above 1.1 and uses it only to find the pane and before a pane is matched.
  `GetDpiForWindow(pane)` is logged next to the measured scale as a cross-check and is not used
  in any arithmetic (it answers in the window's own DPI awareness, not the caller's).
- **Resizes:** a changed size is pushed to the runtime only after it has held 0.35 s.

## A fixed-size or zoomed Game view

Everything on this page assumes the Game view draws its texture **at zoom 1**. Two things break
that:

- a **Fixed Resolution larger than the view area** (4K UHD in a docked pane): Unity shrinks it
  to fit, zoom below 1, and will not let it be shown at 1x;
- the **Scale slider** off 1x, on any size including Free Aspect.

Then every texel is resampled on its way to the panel. No anchor position makes the weave
right, the calibration marker cannot be decoded, and GVFIT's row fit means nothing.

**What went wrong (hardware session, 175 %, 2026-11-18).** The view was on 3840x2160 fitted at
0.2786. GVFIT wrote its row stretch on top regardless of the zoom, giving the anisotropic zoom
`(0.2786, 1.0002)`: the picture drawn 3.6x too tall. The placement centred the *unscaled*
2160 px target and put the anchor at pane `+28,-700`; the weave was black; the marker flashed
three times and failed; and the stretched zoom stayed in the layout after Play. The same
override was in the 250 % logs with the Scale slider at 2x (`zoom=(2,1.00058)`).

**What the plug-in does now**, in docked (texture) mode. A floating Game view is covered by the
runtime's own window and is not affected.

1. **GVFIT is applied at zoom 1 only** (`DisplayXRGameViewPlacement.FitZoomY`). At any other
   zoom it writes `m_Scale.y = m_Scale.x`, which is Unity's own uniform zoom; that also undoes
   a stretch left by an earlier frame or by an older plug-in in a saved layout.
2. **The view is taken over for the Play session** (`DisplayXRGameViewWholePixel`, within a few
   tenths of a second of the glue binding to it): Scale set to 1x when the selected size fits
   the view at 1x; otherwise **Free Aspect at 1x**. One log line says so:
   `Game view: "4K UHD (3840x2160)" was shown at 0.28x, which cannot be 1:1 on the panel (no 3D) - switched to Free Aspect at Scale 1x for this Play session; your size and zoom come back when Play ends`.
   From there the normal path runs (calibration, the whole-pixel size where it applies). The
   user's size, zoom and scroll are put back when Play ends, before a domain reload, on quit,
   and when the main Game view changes - never during Play. `DISPLAYXR_PROV_GV_TAKEOVER=0`
   opts out.
3. **If it is not taken over** (opted out, changed by hand, editor internals not reachable) the
   glue **degrades visibly instead of going black**: the anchor is the scaled picture's rect,
   centred in the render area (`PlaceScaled`), never off the pane; the picture is shown as
   Unity scales it, without usable 3D; the marker is not shown at all; and **one warning** per
   Play, once the state has held for a second, names the cause and the remedy:
   `Game view cannot be shown 1:1: the Game view shows its 3840x2160 image scaled to 0.28x (...). Remedy: in the Game view's toolbar select Free Aspect and set Scale to 1x ...`.
   The `GameView 1:1:` line carries `[zoom is not 1: cannot be 1:1 - picture shown scaled, no calibration]`
   and raises none of its own warnings.
4. **An anchor that comes out off the pane, for any reason, is not used**
   (`KeepInsidePane`, 8 px of slack): the render area is, with the same warning naming the
   computed position.

**One owner of the size and zoom during Play** (`DisplayXRGameViewOwner`, pure C#, tested in
`Tests~/GameViewPlacement`). The takeover and the whole-pixel size used to be two mechanisms
that each read the view's selection. On hardware (175 %): with the whole-pixel size 1239x595
selected, a splitter drag made the pane narrower than the size; Unity fitted the size (zoom
below 1); the takeover saw "a fixed size that does not fit" and selected Free Aspect; the
whole-pixel side saw a selection that was not its own, logged `another Game view size was
picked`, switched itself off and put the pre-Play 4K size back in the middle of Play. Random
phase from then on. Now:

| Situation during Play | What happens |
|---|---|
| the selection is the one the plug-in made (Free Aspect, its own entry, or the user's size with the zoom set to 1) | it is the plug-in's; never read as a user pick |
| the held whole-pixel size stops fitting (the pane shrank) | re-fitted to the area **at once**, at every poll while the pane keeps shrinking (`... re-fitted without waiting`) |
| the pane grows, or changes without the size ceasing to fit | re-fitted once the area has held 0.35 s |
| the pane lands on a whole display scale, or the area is smaller than one step | the held size is given up for **Free Aspect** (1:1 there), not for the pre-Play size |
| the selection differs from what the plug-in holds, or its entry was deleted | a **user pick**: the view is left exactly as it is until Play ends (`... the plug-in leaves the Game view alone until Play ends`) |
| the zoom is not 1 on a size that fits | set back to 1, twice; the third time it is the user's |
| Play ends (also: domain reload, quit, the main view changes) | the pre-Play size, zoom and scroll come back, **whatever happened in between** |

After every size change the layout key of the calibration changes, so the marker is shown
and the anchor measured again once the layout has held 0.35 s. A resize therefore ends as:
follow, settle, re-fit, re-measure, re-anchor.

**The restore is verified.** Selecting a size and setting a zoom in one call does not stick
(read from the 6000.4.0f1 `GameView` / `ZoomableArea`): `SizeSelectionCallback` does nothing
when the index is already the selected one, which it is after our entry (the last) is removed
and Unity clamps the index to the new last entry; and the zoom area's limits (`hScaleMin` /
`vScaleMin`) are recomputed for the new size only in `ConfigureZoomArea`, so a zoom set before
that is held to the **old** size's minimum - 1x for the whole-pixel size. On hardware the log
said `restored ... zoom 0.279x` while the view showed 4K UHD at 1x. The restore now selects
the size, calls `GameView.UpdateZoomAreaAndParent`, sets zoom and scroll through
`ZoomableArea.SetTransform` (then the fields, if the read-back differs), and reads size, zoom
and scroll back every 0.1 s: three agreeing reads in a row end it with
`Game view: restored the previous size and zoom ... - leaving Play: read back size index 6 "4K UHD (3840x2160)", zoom 0.2786x, scroll (x,y)`;
a read that differs is re-applied; after 30 reads one warning states what the view really
shows. The line reports values read from the view, not the intended ones. Before a domain
reload or a quit nothing can run later, so there is one application and no read-back.

**GVFIT and a texture that is not the view's yet.** Right after a size change the view still
holds the old texture for a frame. GVFIT fits only a texture whose height is the view's
target height (`GameView.targetRenderSize`) within a pixel; until then the zoom stays uniform.

**Why take over rather than only warn.** A scaled view cannot show 3D at all, so leaving it
means every Play starts broken until the user finds the size menu. Taking over is safe because
of what it selects: **Free Aspect at zoom 1 is Unity's own default and always index 0**, so
even if the editor dies in Play and nothing is ever restored, the view is left in a valid,
built-in state, never on a foreign size. And what is to be restored is on disk before the first
change (below), so the next editor start puts it back.

**Crash safety: the record.** `Library/DisplayXR.GameViewTakeover.txt` (per project) holds the
user's size index, its name, the zoom, the scroll, and what was selected instead. It is written **before**
the first change and deleted after the restore. Every editor load (and domain reload) heals:

| Found at load | Done |
|---|---|
| a zoom with `y != x` (a row stretch saved with the layout) | `y = x` |
| a selected size index past the end of the list | Free Aspect |
| a record, and a single Game view | the recorded size (if the index still carries the recorded name, else Free Aspect), zoom and scroll |
| a record, several Game views | a view on our entry, or on the last entry where ours used to be: as above; the others are left as they are (which of them was ours is not known) |
| a size named "DisplayXR whole-pixel" with no record | views on it go to Free Aspect; the entry is removed |

**The persistence bug this replaces.** The previous selection was kept in `SessionState`,
which does not outlive the editor. The custom size exists only in memory (`AddCustomSize` /
`RemoveCustomSize` bump a change id; Unity writes the list from its own size menu). The
selected **index** is serialized with the window layout. So an editor killed, or closed while
in Play, with the whole-pixel size selected could start again with index 7 in a list of 7:
`GameView.EnsureSelectedSizeAreValid` clamps that to the **last** entry, which in the Standalone
group is **4K UHD 3840x2160** (read from the 6000.4.0f1 editor assembly), scaled to fit. The old
clean-up at load looked for our entry by name, did not find it, and returned. *The clamp and
the list order are read from Unity's code; that the layout was written with the stale index in
the session in question is inferred from the result, not observed.* A view that an older
plug-in left on 4K UHD cannot be told from one the user chose, so it is not changed at load;
it is taken over during Play like any other scaled view and comes back afterwards. Select
Free Aspect once by hand to be rid of it.

Not verified on hardware: all of this section (unit tests cover the decisions and the
placement; the reflection is checked against the 6000.4.0f1 assembly by reading it, not by
running it).

## On-screen calibration (condition 4, measured)

Computing where Unity draws the image does not survive a change of display scale. Measured on
hardware: at 175 % with the whole-pixel size selected (texture 1120x595, drawn 1:1) Unity centres
the image in a 1122.5 px area, which leaves 1.25 px on each side, and snaps it to **pane +1 px**;
the computed anchor was pane +2 px, and the 3D was inverted. Moving the anchor one pixel fixed
it. At 250 % the computed position was right. Each rounding rule held at one scale and broke at
the next, because Unity rounds fractional points differently by scale and layout. So the position
is measured.

**What happens.** In the Windows editor, with the Game view docked (texture mode):

1. The layout is watched: the matched pane and its monitor, the pane's size, the computed image
   rect inside the pane, the texture size, maximized or not, whole-pixel size or not, the scale.
   Where the pane sits on its monitor is not part of it (the image does not move inside its pane
   when the editor window moves).
2. When that has differed from the calibrated layout and held for 0.35 s (the same settle clock
   as the size push), the native mirror blit draws a **position-coded marker** instead of the
   woven image, into the same texture, through the same draw. It reaches the screen exactly
   where the weave does.
3. Once the marker has been drawn for two frames and 50 ms have passed, native code reads
   **1-px strips of the screen** (GDI `BitBlt` from the screen DC, inside a per-monitor-v2
   scope, physical px): a horizontal one a third (or two thirds) of the way down the image, then
   a vertical one at the column the horizontal strip just located. Each read costs about 15 ms
   (measured), so a clean strip ends the read; the second strip of each pair is only read when
   the first is not clean.
4. The strips are decoded. On success the marker comes down at once (typically well under
   0.2 s after it went up), the anchor moves to the measured origin, and one line is logged.
   The phase nudge (`dxr_phase_nudge.txt`) still applies on top.
5. If nothing decodes within 0.3 s the marker comes down, the **computed position stays**, one
   line says why, and the same layout is tried again after 2 s, three times in all.
   The computed position is corrected by a **remembered offset** when there is one: where the
   image lands in its pane is Unity's rounding of a centred rect, a function of the scale, the
   margins around the image (pane minus image, per axis), where the computed rect sits in the
   pane, and the kind of size (whole-pixel, maximized) - not of the image's absolute size or
   the pane's position. So `measured - computed` of an earlier successful calibration with
   exactly those inputs is applied (`... corrected by the offset measured earlier for the same
   scale, margins and kind of size (+2,-1 px)`). Any difference in them: the bare computed
   position. The image is 1:1 either way where the whole-pixel size is selected; only the
   anchor is at stake. *Arithmetic argument, not verified on hardware.*

The marker cannot stay up: native code takes it down by itself 0.5 s after it was requested,
whatever the managed side does (a domain reload, a stopped session).

**The marker.** Every texel is one of six saturated colours (each channel fully on or off: red,
green, blue, yellow, cyan, magenta), so a Gamma / Linear difference on the way to the screen
cannot turn one into another; a capture that arrives with red and blue exchanged is decoded too
and reported. A texel's colour is the code of its **column**, except inside two vertical bands
24 texels wide (near one third and two thirds of the width), where it is the code of its **row**.
The code comes in blocks of six texels: a magenta sync texel, then the block's index as five
base-5 digits. A block read off the screen therefore names its own texel, and
`screen position - texel` is the screen position of texel 0. Every block in the strip votes.

**The verdict, per axis.**

| Result | Meaning | What happens |
|---|---|---|
| `1:1` | all blocks agree and every pixel of the strip shows the colour its texel should have, over the whole width (height); the pixel beyond each end does not repeat the end texel | origin used |
| `NOT 1:1` | the marker is there, but a line is dropped or doubled: part of the image is at another origin within 4 px, or an end line is missing or doubled | majority origin used; at a fractional scale the whole-pixel size is selected (below) |
| unreadable | more than 4 px show neither the marker nor a shifted copy of it: another window is over the Game view, or the image is filtered | computed position, retry |
| no marker | fewer than 3 blocks: the marker is not on screen | computed position, retry |

A clean `1:1` read cannot be produced by something covering the strip, so when the two strips of
a pair disagree the clean one wins.

**The log line** (one per calibration):

```
[DisplayXR] GameView calibration: measured image origin pane +1,+110 (screen 1501,1327), computed pane +2,+110
  (measured - computed = -1,0 px) | W 1:1, H 1:1 | texture 1120x595 | 3 marker frames, 84 ms | the anchor now uses the measured origin
[DisplayXR] GameView calibration: fell back to the computed origin (pane +3,+113) - the horizontal strips are partly
  covered or filtered (212 px show neither the marker nor a shifted copy of it; another window over the Game view?) [attempt 1 of 3, retrying in 2 s]
```

A not-1:1 axis reads `W NOT 1:1 (from column 561 the image is -1 px off, 448 px differ)`.

**Where it does not run.** The marker is drawn by the D3D12 native mirror blit, so with D3D11,
or with `DISPLAYXR_PROV_NATIVE_MIRROR=0`, no marker appears, the line says
`the marker was not drawn`, and the computed position is used, as before. A floating Game view
(present mode) is not calibrated: the runtime presents its own window there. A Game view that
is not repainting (Play paused) shows no marker and falls back too (expected, not verified). `DISPLAYXR_PROV_GV_CALIBRATE=0` turns calibration off.
A Game view whose zoom is not 1 is never calibrated: no marker is shown while it is scaled
(see [A fixed-size or zoomed Game view](#a-fixed-size-or-zoomed-game-view)).

**What is on screen while the strips are read.** In docked texture mode the plug-in's anchor
window is an invisible layered child (alpha 0) and nothing of the plug-in presents, so the
screen shows Unity's own Game view. Overlays Unity draws over the image (the Stats panel, gizmo
labels) cover part of a strip; that is why each axis has two strips in different places.

**For other code.** `DisplayXRProviderDriver.TryGetGameViewImageRect(out clientX, out clientY,
out width, out height, out measured)` (internal) returns the image's top-left in the pane
window's client area and its size, in physical px, and whether that was measured or computed.

## The 1:1 diagnostic (always on in the editor, changes nothing pushed to the runtime)

Each time the numbers have held for 0.35 s and differ from the last line logged, one **info** line:

```
[DisplayXR] GameView 1:1: W drawn 1728.00 px for a 1728 px texture (1:1), H drawn 860.00 px for a 860 px texture (1:1)
  | texture=1728x860 glue computed 1728x860 (agree) | target=1728x859.5 px zoom=(1,1.0005817) scale=2.5 (pane dpi 240 = 2.5)
  canvas=1728x860 (= texture) | left border 1 pt = +3 px | anchor at pane +3,+113 | targetInView=(...)pt viewInWindow=(...)pt
```

| Field | Meaning | Good |
|---|---|---|
| `W drawn A px for a B px texture` | condition 1: target width x zoom x, against the real texture | `(1:1)` |
| `H drawn ...` | condition 2, with GVFIT's stretch in the zoom. A mismatch is tagged `predicted, unverified: F1` | `(1:1)` |
| `texture=WxH glue computed WxH` | the real texture against the size the glue derived from the target (F4) | `(agree)` |
| `canvas=WxH` | condition 3: what the glue drives the woven canvas to | `(= texture)` |
| `left border b pt = +n px` | the pane's real left border, from Unity (`EditorWindow.position.x - HostView.screenPosition.x`) | - |
| `anchor at pane +dx,+dy` | where the glue put the anchor, relative to the pane's top-left | Free Aspect: `dx == n` (F3) |
| `scale` / `pane dpi` | the measured pane scale and the window's DPI / 96 | equal |
| `targetInView` / `viewInWindow` | Unity's raw rects in points, for reference (not used for the verdict: they go through `pixelsPerPoint`) | - |
| `[whole-pixel size]` | the experiment's size is on this view | - |
| `[present mode ...]` | the Game view is floating: the runtime presents over the pane, so the line is informational and nothing is raised | - |

**What is a warning and what is not.** Warnings are raised only for conditions that are certain
from values read back from Unity, once per **editor session** (the flag is kept in `SessionState`,
so it does not repeat on every Play), and never in present mode:

| Console | When |
|---|---|
| warning `Game view is not drawn 1:1 horizontally ...` | the drawn width (target x zoom) is not the real texture's width |
| warning `GameView glue: the Game view's texture is WxH but the glue computed WxH ...` | F4: the two sizes differ |
| warning `... the woven canvas is driven to WxH but the Game view's texture is WxH ...` | the canvas differs from the texture for another reason (a clamp, a fit below 1) |
| warning `... the pane's measured scale (...) is not its window DPI (...)` | the two scale sources disagree |
| warning `GameView 1:1 check unavailable ...` / `PlayModeView.GetMainPlayModeView not found ...` | editor internals changed |
| **info only**, inside the `GameView 1:1:` line | a row mismatch (`H drawn ... predicted, unverified: F1`). At 100 / 200 / 300 % this is expected on every Play with GVFIT on; it stays a log line until hardware confirms F1 |

`DISPLAYXR_PROV_GVGEOM=1` adds the older, much longer `GVGEOM` geometry dump.

## The whole-pixel size (`DISPLAYXR_PROV_GV_WHOLEPIXEL`)

| `DISPLAYXR_PROV_GV_WHOLEPIXEL` | Behaviour |
|---|---|
| unset | **automatic**: off until a calibration *measures* an image that is not drawn 1:1 at a fractional scale; then on until the editor quits (so the next Play starts with it). Logged: `whole-pixel Game view: the measured image is not drawn 1:1 horizontally at 175 % - selecting the whole-pixel size ...` |
| `1` | on from the start at every fractional scale, measured or not (the former opt-in) |
| `0` | never; a not-1:1 measurement is only logged |

Selecting the size changes the layout, so a second calibration follows and measures the new
image. With calibration off (`DISPLAYXR_PROV_GV_CALIBRATE=0`, or where the marker is not drawn)
the automatic mode never turns on: use `=1`.

When it is on (the variable is read once, when the editor starts), during Play, on the main Game view:

1. The step `q` is the smallest number of points that is **exactly** a whole number of pixels:
   `100 / gcd(percent, 100)`. 4 pt = 5 px at 125 %, 2 pt = 3 px at 150 %, 4 pt = 7 px at 175 %,
   4 pt = 9 px at 225 %, 2 pt = 5 px at 250 %; 10 pt at 110 %; 100 pt at a custom scale such as
   133 % (so the margin can be large there). Whole scales have `q = 1` and nothing happens.
2. Once the view area has held 0.35 s, a custom Fixed Resolution size named
   **"DisplayXR whole-pixel"** is created (or resized) to `floor(area / q) * q` points on each
   axis, selected, and the zoom snapped to 1. The image is at most one step smaller than the area
   and centred, with a grey margin.
3. On that view the driver **skips GVFIT** and computes the anchor as an unstretched letterbox
   with the pane's real left border (the calibration then replaces that with the measured
   origin). The whole-pixel size wins there; other Game views and the default path are untouched.
4. The pre-Play size, zoom and scroll are put back and the custom size removed when Play ends,
   before a domain reload (it comes back afterwards if Play continues), on quit, when the main
   view changes, and when applying it fails part-way - not during Play. Picking another size
   by hand during Play leaves the view on that pick until Play ends. What is to be put back is kept in
   `Library/DisplayXR.GameViewTakeover.txt`, written before the size is selected, so an editor
   that is killed or closed in Play is healed at its next load (see
   [A fixed-size or zoomed Game view](#a-fixed-size-or-zoomed-game-view): the record, and the
   bug it replaces).
5. Unity keeps custom sizes in a per-user list shared by **all projects** and writes it only from
   its own size menu. After removing the entry the plug-in saves the list (best-effort), so a save
   the user triggered while the experiment was on does not leave "DisplayXR whole-pixel" in every
   project.

Log lines: `whole-pixel Game view: selected WxH px (... pt, steps of q pt = p px) at S % ...`,
`... resized to ...` (`... re-fitted without waiting` when the pane shrank under it),
`Game view: restored the previous size and zoom and removed "DisplayXR whole-pixel" - reason: read back size index n "name", zoom z, scroll (x,y)`,
`GVFIT skipped on the whole-pixel Game view`.

## Findings

Each is an **arithmetic prediction, unverified on hardware**. The computed placement they
describe is unchanged; since the calibration it is the fallback, and F3 and F5 (anchor errors)
are corrected by the measurement wherever it succeeds, F2 by the whole-pixel size it selects. A
calibration line is now direct evidence for or against each: `measured - computed` is the
anchor's error (F3, F5), `W NOT 1:1` / `H NOT 1:1` are F2 / F1.

**F1. GVFIT's 0.5 is the 250 % panel's half row.** *Arithmetic prediction, unverified on hardware.*
With a texture of `h` rows GVFIT draws `target x h / (h - 0.5)`:

| Scale | Area ends on | Texture | Drawn | |
|---|---|---|---|---|
| 150 / 250 % | x.5 | rounds up | `h` | exact |
| 100 / 200 / 300 % | whole | equal | `h + 0.5` | up to one row doubled (the 300 % captures already show "879 of 880 rows visible") |
| 175 % | x.75 | rounds up | `h + 0.25` | up to one doubled |
| 125 / 225 % | x.25 | rounds **down** | `h + 0.75` | up to one doubled |

The general form is `scale.y = texture rows / target rows`. `RtRow0Y` models the 0.5, so the two
must change together. On 6000.4 the texture is never the area rounded down from x.5, so GVFIT does
not overshoot by a whole row there.

**F2. Nothing fits the columns.** *Arithmetic prediction, unverified on hardware.* At
125 / 175 / 225 % two 1 pt side borders are x.5 px, the area is x.5 px wide, the texture rounds up,
and the rect is 0.5 px narrower than the texture (175 %: a 1927 px pane, 1923.5 px area, 1924 px
texture). Float noise may flip the rounding, making the rect 0.5 px *wider* instead; either way it
is not 1:1. The whole-pixel experiment is the proposed fix.

**F3. A pane docked at the editor window's left or right edge has one side border, not two.**
*Arithmetic prediction, unverified on hardware.* The Free Aspect anchor's X is half the slack,
`(slack + 1) / 2`. Between two neighbours at 250 %: slack 5, +3, correct (measured). At an edge:

| Scale | Edge slack | Anchor | Far left: texture, nudge | Far right: texture, nudge |
|---|---|---|---|---|
| 250 % | always 2 | +1 | +0, **-1** | +3, **+2** |
| 300 % | always 3 | +2 | +0, **-2** | +3, **+1** |

The letterbox path has the same assumption (`side = (host - draw) / 2`). The real border is
available from Unity and is what the whole-pixel path already uses; the 1:1 line prints it next to
the anchor's `dx`.

**F4. The glue's texture size is the rounded target, not the texture.** *The two rounding rules
are certain (read from the code and from Unity's assembly); the effect on the image is unverified
on hardware.* The glue uses `Mathf.RoundToInt(target)`, which rounds halves to even; Unity rounds
them away from zero. With exact halves they differ wherever the area ends on x.5 with an even
floor: 859.5 -> 860 both ways (the reference pane), but 860.5 -> 860 for the glue and 861 for
Unity. That is 275 of the 551 pane heights from 700 to 1250 px at 150 %, 276 at 250 %, and about
half the pane widths at 125 / 175 / 225 %. Where they differ the woven canvas is one row (column)
short of the texture, so the mirror blit resamples it, and the one-short `RtH` goes into `RtRow0Y`.
The diagnostic reports it (`texture=... glue computed ... (DISAGREE)`) and raises the F4 warning.

- *The fix, for a follow-up after the hardware session:* in `TryGetGameViewRenderRect`'s matched
  path, take `rtW` / `rtH` from the Game view's texture itself (`rt.width` / `rt.height`, the same
  texture `ApplyGameViewFit` and the diagnostic already read) instead of
  `Mathf.RoundToInt(size * sizeScale)`, keeping the rounded target only as the fallback while no
  texture exists.
- *Pinned test rows it would move:* none of the hardware rows (their halves all have an odd floor:
  1079.5 -> 1080, 859.5 -> 860). Two modelled rows of the replay block: "150% docked Free" and
  "150% docked 16:9 L/R" (target 610.5 px: `RtH` 610 -> 611, anchor y 228 -> 227). The Free Aspect
  sweep's inputs use the same half-to-even rounding and would be regenerated.

**F5. The code is inconsistent at a quarter pixel.** *Arithmetic prediction, unverified on
hardware.* A whole-point edge that falls on x.5 or x.75 px starts on the next pixel (measured at
250 %, 150 % and 175 %). At x.25 (125 % / 175 %) the X arithmetic (`LetterboxOffsets`, `BorderPx`)
rounds **down** while the Y arithmetic (`RtRow0Y`) and `LinesShown` take the next pixel **up**.
One of them is wrong there: in whole-pixel mode at 125 / 175 % expect a possible +-1 on one axis
only.

## Hardware test recipe

Needs a person at the panel. About 30 minutes for the 250 % pass, the same again per extra scale.

### Setup

1. DisplayXR runtime running, `XR_RUNTIME_JSON` set; a 3D panel; any sample project
   (`displayxr-unity-samples/samples/birp-multipass` is the plain one) with this package on a
   `file:` pin; **DisplayXR** enabled under XR Plug-in Management.
2. Use a scene with something clearly **in front of** the display plane and something clearly
   **behind** it, spread across the whole width of the view.
3. Start the editor from a normal (not elevated) terminal so it inherits the environment
   variables. Set them before starting; they are read once.
4. After changing the Windows scale (Settings > System > Display > Scale), sign out and in, then
   start the editor.
5. Keep `Editor.log` open (`%LOCALAPPDATA%\Unity\Editor\Editor.log`) and filter on `[DisplayXR]`.
   Warnings appear once per editor session: restart the editor between scales.
6. Keep a single Game view open (a second one becomes the bound view when clicked).

### What correct and wrong look like

- **Correct:** near things float in front of the glass, far things sit behind it, and moving your
  head sideways shows more of the side you move towards. Sharp, no doubling.
- **Eye-swapped (uniform):** depth is inside out everywhere - near things look sunk, edges fight,
  it is uncomfortable within seconds. Cause: the anchor (condition 4). The X nudge fixes it.
- **Eye-swapped on one side of a vertical line:** correct on the left part of the view, inside out
  on the right part (or the reverse), with the line near the middle. Cause: a dropped or doubled
  column (condition 1). The nudge only moves which side is right; it cannot fix both.
- **Ghosting on one side of a horizontal line:** one part of the view is clean, the part above or
  below it shows double edges. Cause: a dropped or doubled row (condition 2).
- **Soft, washed-out depth over the whole view:** the canvas is resampled (condition 3).

The **maximized** Game view (Shift+Space) and a **floating** one are the reference: if those are
right and the docked one is not, it is the docked placement.

### The phase nudge

`%TEMP%\dxr_phase_nudge.txt` holds an integer: pixels to shift the invisible anchor in X. It is
re-read about twice a second, docked mode only, and moves only the weave's phase, never the
picture. `%TEMP%\dxr_phase_nudge_y.txt` is the same for Y.

```powershell
Set-Content $env:TEMP\dxr_phase_nudge.txt 1     # try 1, -1, 2, -2, 3, -3
Set-Content $env:TEMP\dxr_phase_nudge.txt 0     # back to the computed anchor
Remove-Item $env:TEMP\dxr_phase_nudge*.txt      # when done - it persists across sessions
```

The log confirms each change: `docked phase nudge X = 1 px`. The value that makes a uniformly
wrong view right **is** the anchor's error in that layout; write it down. Start every layout at 0.

### Pass 0 - the calibration (do this first)

No switch. Press Play with the Game view docked.

1. About half a second after Play the Game view flashes a pattern of thin coloured stripes for a
   fraction of a second, then the image returns. The log has one `GameView calibration:` line.
2. `measured image origin ... computed ...`: note `measured - computed`. At 250 % in the
   reference layout expect `(agree)`. At 175 % expect `-1,0` or similar.
3. Look at the panel: correct depth with `dxr_phase_nudge.txt` at 0 (or absent). **A layout
   that needed a nudge before must not need one now.** If it still does, record the nudge next
   to the calibration line: that is an error of the measurement itself.
4. At 175 % / 125 % without the whole-pixel size: `W NOT 1:1 ...`, then
   `whole-pixel Game view: the measured image is not drawn 1:1 ... - selecting the whole-pixel size`,
   a second flash, and a second line with `W 1:1, H 1:1`.
5. Drag a splitter, release: one flash and one line about half a second later. Move the editor
   window: no flash. Maximize (Shift+Space) and restore: a flash each. Drag the editor to a
   monitor at another scale: a flash.
6. Put another window over part of the Game view and resize a splitter under it: the line
   reads `fell back to the computed origin ... partly covered ...`, up to three times 2 s
   apart; take the window away before the third and it measures.
7. Opt-outs: `DISPLAYXR_PROV_GV_CALIBRATE=0` (no flash, no line, computed anchor),
   `DISPLAYXR_PROV_GV_WHOLEPIXEL=0` (at 175 %: `... keeps the whole-pixel size off`).
8. With D3D11 as the editor's graphics API: `fell back ... the marker was not drawn`.

### Pass Z - a fixed-size or zoomed Game view

1. Free Aspect, drag the Scale slider to 2x, press Play. Expect, within a second:
   `Game view: "Free Aspect" was shown at 2x ... set Scale to 1x for this Play session`, the
   slider back at 1x, **one** marker flash, a normal `GameView calibration:` line, correct 3D.
   Stop Play: `Game view: restored the previous size and zoom ... read back size index 0 "Free Aspect", zoom 2x, scroll (...)`,
   the slider back at 2x, the same part of the picture in view, **not stretched**.
2. Select 4K UHD (3840x2160) in a docked pane (it shows scaled, about 0.28x), press Play.
   Expect `... "4K UHD (3840x2160)" was shown at 0.28x ... switched to Free Aspect at Scale 1x`,
   then as above. Stop Play: back on 4K UHD at the fitted zoom (the view looks exactly as it
   did before Play), `... read back size index 6 "4K UHD (3840x2160)", zoom 0.2786x ...`.
3. In Play, after the takeover, drag the Scale slider to 2x: it is set back to 1x; again: set
   back; a third time: `the zoom was changed again ... the plug-in leaves the Game view alone
   until Play ends`, the picture stays visible (scaled, no 3D, **not black**), no marker
   flashes, and one warning `Game view cannot be shown 1:1: ...` appears about a second later.
   Stop Play: the pre-Play size and zoom are back.
3b. Resizes (at 175 % / 125 %, whole-pixel size selected). Drag a splitter wider, release;
   narrower, release; several times, also the height, also in one long drag. After **each**:
   `whole-pixel Game view: resized to ...`, one marker flash, a `GameView calibration: measured ...
   W 1:1, H 1:1` line, correct 3D. Never `another Game view size was picked`, never
   `restored the previous size` before Play ends.
4. `DISPLAYXR_PROV_GV_TAKEOVER=0`, 4K UHD selected, Play: no takeover line; the scaled picture
   is visible; no marker flash; the one warning; `GameView 1:1:` ends
   `[zoom is not 1: cannot be 1:1 - picture shown scaled, no calibration]` with
   `zoom=(0.278...,0.278...)` (both the same). After Play the zoom is unchanged.
5. Crash safety. With step 2 running (Free Aspect taken over; at 175 % also the whole-pixel
   size selected), end the editor process from Task Manager. Check that
   `Library/DisplayXR.GameViewTakeover.txt` exists. Start the editor: the Game view is on 4K UHD
   at its fitted zoom, the size menu has no "DisplayXR whole-pixel", the file is gone, and the
   log has `Game view: restored the previous size and zoom ... - left over from an earlier session: read back size index 6 "4K UHD (3840x2160)" ...`.
   Repeat closing the editor normally while in Play.

### Pass A - default behaviour (no switch)

Run this pass with `DISPLAYXR_PROV_GV_CALIBRATE=0` to see the computed placement on its own.

For each scale, press Play and go through the layouts. In each, wait a second, read the newest
`GameView 1:1:` line and the newest `GameView glue size change:` line, look at the panel.

| Layout | How |
|---|---|
| Docked, centred | default layout: Game view between other panes on both sides |
| Docked, far left | drag the Game tab to the left edge of the editor window |
| Docked, far right | drag it to the right edge |
| Maximized | Shift+Space on the Game view |
| Floating | drag the Game tab out of the editor window (the log says `GameView dock state: UNDOCKED ... PRESENT mode`) |

Record per layout: the `1:1` line, any warning, correct / swapped / seam, and the nudge that fixes
a uniform swap.

**250 % (the dev panel's native scale) - what the arithmetic predicts:**

- Centred, reference layout: `W ... (1:1), H ... (1:1)`, `texture=1728x860 glue computed 1728x860
  (agree)`, `canvas=1728x860 (= texture)`, `left border 1 pt = +3 px`, `anchor at pane +3,+113`,
  no warning, correct with nudge 0; the size-change line ends `render=(1020,406 1728x860)`.
  **This must not have changed.**
- F3: far left `left border 0 pt = +0 px`, `anchor at pane +1`: expect nudge **-1**. Far right
  `left border 1 pt = +3 px`, `anchor at pane +1`: expect nudge **+2**.
- F4: drag a horizontal splitter to change the pane's height one pixel at a time (let go, wait a
  second each time). `H` stays `(1:1)` at every height. What should alternate, height by height,
  is `glue computed ... (agree)` / `(DISAGREE)` together with `canvas=`; the F4 warning appears at
  the first disagreement. Compare the picture at an agreeing and a disagreeing height: is the
  disagreeing one softer? Note both pane heights.

**175 % - what the arithmetic predicts:**

- Centred: the horizontal warning, `W drawn x.50 px for a ... px texture (up to 1 dropped)`, and a
  vertical line near the middle of the view with the eyes swapped on one side (F2).
  `H drawn ... (up to 1 doubled - predicted ...)` with `h + 0.25`.
- Maximized: `W ... (1:1)` (no side borders); `H` still `h + 0.25`.

**125 % or 225 %:** as 175 %, with `H` at `h + 0.75`.

**300 % or 200 %:** `W (1:1)`; `H drawn x.50 ... (up to 1 doubled - predicted, unverified: F1)` in
the info line and **no warning**. Look for a horizontal line with ghosting on one side: seeing it
confirms F1, not seeing it refutes it. F3 at 300 %: far left nudge **-2**, far right **+1**.

### Pass B - the whole-pixel size, forced

Close the editor, `set DISPLAYXR_PROV_GV_WHOLEPIXEL=1` (and `DISPLAYXR_PROV_GV_CALIBRATE=0` to
judge the computed anchor), start it, same layouts.

- At 175 % / 125 %, about half a second after Play: `whole-pixel Game view: selected ...`, then a
  `1:1` line with `(1:1)` twice, `(agree)`, `[whole-pixel size]`, and `GVFIT skipped ...`. The Game
  view's size menu shows "DisplayXR whole-pixel"; a thin grey margin (under 7 px at 175 %)
  surrounds the image. **The vertical seam of pass A must be gone.** If the whole view is now
  uniformly off, find the X nudge and the Y nudge separately and record them with the `1:1` line:
  F5 predicts at most +-1 on one axis.
- Drag a splitter for a few seconds: no log spam during the drag, one `resized to` line and one
  size change about half a second after letting go.
- Stop Play: `restored the previous size ...`; the size menu is back on what it was, the zoom
  slider is where it was, and the custom entry is gone. Check the same after a script recompile
  during Play (it goes and comes back) and after picking another size by hand during Play (it
  stays off). Open another Unity project afterwards: its Game view size list must not contain
  "DisplayXR whole-pixel".
- At 250 % the experiment also engages (2 pt = 5 px steps). The image shrinks by up to 4 px; it
  must look as right as pass A did.
- At 100 / 200 / 300 %: `nothing to do at ... %`, and nothing else changes.

### What to send back

For every scale x layout x pass: the `GameView calibration:` line(s), the `GameView 1:1:` line, `GameView glue size change:`,
`GameView glue matched pane:`, `#740 dock-border:`, any warning, what was seen, and the nudge.
With those numbers F1-F5 can each be confirmed or dropped without another session.

## Not verified without a panel

Everything in the recipe and every finding. In particular: that the whole-pixel size removes the
175 % seam in this plug-in's texture path (it was reported from another plug-in's editor weave on
the same Unity Game view); the whole-pixel reflection on Unity 2022.3 (member names, the texture
rounding rule and `RemoveCustomSize`'s index meaning were read from the Unity 6000.4.0f1 editor
assemblies only).

The calibration, as of the change that added it:

| | Status |
|---|---|
| Marker definition and decoding (origin, 1:1, dropped / doubled lines, covered, filtered, colour shifts) | unit-tested on synthetic screens (`Tests~/GameViewCalibration`) |
| `displayxr_read_screen_strip`: pixel order, orientation, physical coordinates from a DPI-unaware thread, clipping at the desktop edge | checked once on a development machine against a plain test window painted with the marker (1:1 and one dropped column both decoded at the window's true origin); not in CI |
| The HLSL of the marker | compiles (`fxc`, ps_5_0); **that it draws the same pattern as the C# definition is not verified** |
| The marker reaching the screen through Unity's Game view; timing; the measured origin being the right anchor on a panel | **not verified** - Pass 0 |
| A screen read while the Game view is on an HDR or colour-managed desktop | not verified (the classifier is relative to the brightest channel to allow for it) |
| D3D11 editor, Vulkan | falls back by design; the fallback line is not verified |
