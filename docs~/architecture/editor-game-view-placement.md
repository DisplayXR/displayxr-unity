# Editor Game view: pixel-exact placement of the woven image

In the editor the runtime's woven image is shown **inside the docked Game view** (weave-to-texture,
the default since v2.8.0). A lenticular weave is phase-sensitive: the image must reach the panel
texel for pixel, on the columns and rows the weaver assumed. One pixel of horizontal error is the
other eye.

This page says what has to hold, what the plug-in does about it, how to read the numeric
diagnostic it logs, what the opt-in whole-pixel experiment changes, what the arithmetic predicts
is still wrong, and how to test all of it at the panel (issues #420, #459).

Code: `Runtime/Provider/DisplayXRProviderDriver.cs` (the glue: reflection into the Game view,
Win32 rects, latches, logging), `Runtime/Provider/DisplayXRGameViewPlacement.cs` (all the
arithmetic, pure C#), `Editor/Provider/DisplayXRGameViewWholePixel.cs` (the experiment),
`Tests~/GameViewPlacement` (CI runs `dotnet run --project "Tests~/GameViewPlacement" -c Release`).
In that suite the #420 checks are pinned to hardware measurements; the #459 checks are unit tests
of the arithmetic and model-consistency checks, **not** hardware pins, and say so.

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
  disable). Every frame it sets the zoom area's `m_Scale.y` to `h / (h - 0.5)` on every Game view,
  `h` being the texture height. The 0.5 is the half row of the 250 % dev panel; it is exact at
  150 % and 250 % and nowhere else (F1).
- **Columns (condition 1): nothing.** Exact wherever the borders are whole pixels (F2).
- **Canvas size (condition 3):** the glue drives the canvas to `Mathf.RoundToInt` of Unity's main
  target size - halves to **even** - not to the texture's own size (F4).
- **Anchor (condition 4):** `DisplayXRGameViewPlacement.Place`. Docked Free Aspect: X = pane left +
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

## The whole-pixel experiment (`DISPLAYXR_PROV_GV_WHOLEPIXEL=1`)

Off by default. With it set (before the editor starts), during Play, on the main Game view:

1. The step `q` is the smallest number of points that is **exactly** a whole number of pixels:
   `100 / gcd(percent, 100)`. 4 pt = 5 px at 125 %, 2 pt = 3 px at 150 %, 4 pt = 7 px at 175 %,
   4 pt = 9 px at 225 %, 2 pt = 5 px at 250 %; 10 pt at 110 %; 100 pt at a custom scale such as
   133 % (so the margin can be large there). Whole scales have `q = 1` and nothing happens.
2. Once the view area has held 0.35 s, a custom Fixed Resolution size named
   **"DisplayXR whole-pixel"** is created (or resized) to `floor(area / q) * q` points on each
   axis, selected, and the zoom snapped to 1. The image is at most one step smaller than the area
   and centred, with a grey margin.
3. On that view the driver **skips GVFIT** and places the anchor as an unstretched letterbox with
   the pane's real left border. The experiment wins there; other Game views and the default path
   are untouched.
4. The previous size and zoom are put back and the custom size removed when Play ends, before a
   domain reload (it comes back afterwards if Play continues), on quit, when the pane lands on a
   whole scale, when the main view changes, and when applying it fails part-way. Picking another
   size by hand turns the experiment off until the next Play. Any entry of that name left behind
   (a crash) is removed at the next editor load.
5. Unity keeps custom sizes in a per-user list shared by **all projects** and writes it only from
   its own size menu. After removing the entry the plug-in saves the list (best-effort), so a save
   the user triggered while the experiment was on does not leave "DisplayXR whole-pixel" in every
   project.

Log lines: `whole-pixel Game view: selected WxH px (... pt, steps of q pt = p px) at S % ...`,
`... resized to ...`, `... restored the previous size ...`, `GVFIT skipped on the whole-pixel Game view`.

## Findings

Each is an **arithmetic prediction, unverified on hardware**. None is changed in code in this
branch; the default placement is exactly what it was.

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

### Pass A - default behaviour (no switch)

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

### Pass B - the whole-pixel experiment

Close the editor, `set DISPLAYXR_PROV_GV_WHOLEPIXEL=1`, start it, same layouts.

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

For every scale x layout x pass: the `GameView 1:1:` line, `GameView glue size change:`,
`GameView glue matched pane:`, `#740 dock-border:`, any warning, what was seen, and the nudge.
With those numbers F1-F5 can each be confirmed or dropped without another session.

## Not verified without a panel

Everything in the recipe and every finding. In particular: that the whole-pixel size removes the
175 % seam in this plug-in's texture path (it was reported from another plug-in's editor weave on
the same Unity Game view); the experiment's reflection on Unity 2022.3 (member names, the texture
rounding rule and `RemoveCustomSize`'s index meaning were read from the Unity 6000.4.0f1 editor
assemblies only).
