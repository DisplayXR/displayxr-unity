# Editor Game view: pixel-exact placement of the woven image

In the editor the runtime's woven image is shown **inside the docked Game view** (weave-to-texture,
the default since v2.8.0). A lenticular weave is phase-sensitive: the image must reach the panel
texel for pixel, on the columns and rows the weaver assumed. One pixel of horizontal error is the
other eye.

This page says what has to hold, what the plug-in does about it, how to read the numeric
diagnostic it logs, what the opt-in whole-pixel experiment changes, what is still known to be
wrong, and how to test all of it at the panel (issues #420, #459).

Code: `Runtime/Provider/DisplayXRProviderDriver.cs` (the glue: reflection into the Game view,
Win32 rects, latches, logging), `Runtime/Provider/DisplayXRGameViewPlacement.cs` (all the
arithmetic, pure C#), `Editor/Provider/DisplayXRGameViewWholePixel.cs` (the experiment),
`Tests~/GameViewPlacement` (the arithmetic pinned to hardware measurements; CI runs
`dotnet run --project "Tests~/GameViewPlacement" -c Release`).

## What has to hold

Four things, all in physical pixels of the pane's own monitor:

| # | Condition | If it fails |
|---|---|---|
| 1 | Unity draws the Game view's render texture into a rect **as wide as the texture** | a column is dropped or doubled; everything past it sits one pixel off: the eyes swap on one side of a vertical line |
| 2 | ...and **as high as the texture** | a row is dropped or doubled; through the slanted lens the interlace phase shifts on one side of a horizontal line (crosstalk, not a full swap) |
| 3 | The woven canvas the runtime renders is **the texture's size** | the native mirror blit stretches the canvas over the texture with a linear sampler: the interlace is resampled over the whole view |
| 4 | The invisible weave anchor sits on the screen pixel that shows the texture's **first column and row 0** | the whole view is uniformly out of phase (up to a full eye swap) |

Unity draws the texture into `targetInView`: its *target* size (for Free Aspect the view area, which
can be a fraction of a pixel) times the zoom area's scale, with point filtering while the zoom is
1 or more. The texture itself is whole pixels. So conditions 1 and 2 come down to whether the
area is a whole number of pixels:

| Windows scale | 2 pt of side borders | 47 pt of docked chrome (tab + toolbar + bottom border) | Free Aspect, as Unity draws it |
|---|---|---|---|
| 100 / 200 / 300 % | whole px | whole px | W exact, H exact |
| 150 / 250 % | whole px (3, 5) | x.5 px (70.5, 117.5) | W exact, H half a row short |
| 125 / 175 / 225 % | x.5 px (2.5, 3.5, 4.5) | x.25 / x.75 px | W half a column short, H a quarter row off |

## What the plug-in does today

- **Low Resolution Aspect Ratios off** on every Game view (`ForceGameViewFullResolution`), so the
  texture is physical pixels.
- **Rows (condition 2): GVFIT** (`ApplyGameViewFit`, default on, `DISPLAYXR_PROV_GV_FIT=off` to
  disable). Every frame it sets the zoom area's `m_Scale.y` to `h / (h - 0.5)` on every Game view,
  `h` being the texture height. The 0.5 is the half row of the 250 % dev panel; it is exact at
  150 % and 250 % and nowhere else (see Findings).
- **Columns (condition 1): nothing.** Exact at 100 / 150 / 200 / 250 / 300 % because the borders
  are whole pixels there; half a column short at 125 / 175 / 225 %.
- **Canvas size (condition 3):** the glue drives the canvas to `Mathf.RoundToInt` of Unity's main
  target size (`GetMainPlayModeViewTargetSize`), not to the texture's own size.
- **Anchor (condition 4):** `DisplayXRGameViewPlacement.Place`. Docked Free Aspect: X = pane left +
  half the pane-minus-texture slack (odd pixel to the left); Y = the row Unity's own rule puts the
  texture's row 0 on (`RtRow0Y`). Letterboxed: Unity's whole-point rounding of the centred rect.
  The 250 % reference (pane 1733x977 at (1017,293), texture 1728x860) gives **(1020,406)**,
  hardware-verified, pinned in the tests.
- **Which Game view:** the one Unity calls the main play-mode view
  (`PlayModeView.GetMainPlayModeView()`), falling back to the view whose width is closest to the
  main target size. With one Game view these are the same view. GVFIT and the Low Resolution
  switch still apply to every Game view.
- **Which scale:** the pane's own points-to-pixels, measured as (pane width in physical px, read
  under per-monitor-v2 awareness) / (Unity's host width in points) and snapped to a whole percent.
  The global `EditorGUIUtility.pixelsPerPoint` is only valid inside `OnGUI`; the glue latches its
  first value above 1.1 and uses it only to find the pane and before a pane is matched.
  `GetDpiForWindow(pane)` is logged next to the measured scale as a cross-check and is not used
  in any arithmetic (it answers in the window's own DPI awareness, not the caller's).
- **Resizes:** a changed size is pushed to the runtime only after it has held 0.35 s.

## The 1:1 diagnostic (always on in the editor, changes nothing)

Each time the numbers have held for 0.35 s and differ from the last line logged:

```
[DisplayXR] GameView 1:1: W drawn 1728.00 px for a 1728 px texture (1:1), H drawn 860.00 px for a 860 px texture (1:1)
  | target=1728x859.5 px zoom=(1,1.0005817) scale=2.5 (pane dpi 240 = 2.5) canvas=1728x860 (= texture)
  | left border 1 pt = +3 px | anchor at pane +3,+113 | targetInView=(...)pt viewInWindow=(...)pt
```

| Field | Meaning | Good |
|---|---|---|
| `W drawn A px for a B px texture` | condition 1: target width x zoom x, against the texture | `(1:1)` |
| `H drawn ...` | condition 2, with GVFIT's stretch in the zoom | `(1:1)` |
| `canvas=WxH` | condition 3 | `(= texture)` |
| `left border b pt = +n px` | the pane's real left border, from Unity (`EditorWindow.position.x - HostView.screenPosition.x`) | - |
| `anchor at pane +dx,+dy` | where the glue put the anchor, relative to the pane's top-left | Free Aspect: `dx == n` |
| `scale` / `pane dpi` | the measured pane scale and the window's DPI / 96 | equal |
| `targetInView` / `viewInWindow` | Unity's raw rects in points, for reference (not used for the verdict: they go through `pixelsPerPoint`) | - |
| `[whole-pixel size]` | the experiment's size is on this view | - |
| `[present mode ...]` | the Game view is floating: the runtime presents over the pane, so the verdict is informational and no warning is raised | - |

One warning per kind per session:

- `Game view is not drawn 1:1 horizontally ...` - condition 1 (eye swap on one side of a line).
- `Game view is not drawn 1:1 vertically ...` - condition 2.
- `... the woven canvas is driven to WxH but the Game view's texture is WxH ...` - condition 3.
- `... the pane's measured scale (...) is not its window DPI (...)` - the two scale sources disagree.

`DISPLAYXR_PROV_GVGEOM=1` adds the older, much longer `GVGEOM` geometry dump.

## The whole-pixel experiment (`DISPLAYXR_PROV_GV_WHOLEPIXEL=1`)

Off by default. With it set (before the editor starts), during Play, on the main Game view:

1. The step `q` is the smallest number of points that is a whole number of pixels: 4 pt = 5 px at
   125 %, 2 pt = 3 px at 150 %, 4 pt = 7 px at 175 %, 4 pt = 9 px at 225 %, 2 pt = 5 px at 250 %.
   Whole scales have `q = 1` and nothing happens.
2. Once the view area has held 0.35 s, a custom Fixed Resolution size named
   **"DisplayXR whole-pixel"** is created (or resized) to `floor(area / q) * q` points on each
   axis, selected, and the zoom snapped to 1. The image is at most one step smaller than the area
   and centred, with a grey margin.
3. On that view the driver **skips GVFIT** and places the anchor as an unstretched letterbox with
   the pane's real left border. The experiment wins there; other Game views and the default path
   are untouched.
4. The previous size is selected again and the custom size removed when Play ends, before a domain
   reload (it comes back afterwards if Play continues), on quit, when the pane lands on a whole
   scale, and when the main view changes. Picking another size by hand turns the experiment off
   until the next Play. A size left behind by a crash is removed at the next editor load.

Log lines: `whole-pixel Game view: selected WxH px (... pt, steps of q pt = p px) at S % ...`,
`... resized to ...`, `... restored the previous size ...`, `GVFIT skipped on the whole-pixel Game view`.

## Findings (arithmetic; not changed in code)

**F1. GVFIT's 0.5 is the 250 % panel's half row.** It is right when the area ends on x.5 px and
the texture is that rounded up: 859.5 -> 860 rows, drawn 859.5 x 860/859.5 = 860. At a whole scale
the area is already whole: 880 rows are drawn 880 x 880/879.5 = 880.5 px high, over 881 device rows
of which the area shows 880 - one row doubled and one hidden (the 300 % captures already show
"879 of 880 rows visible"). At 125 / 175 % the area ends on x.25 / x.75 and a quarter row remains.
And if the texture is the area rounded *down* (860.5 -> 860), the stretch draws it a whole row too
tall. The general form is `scale.y = texture rows / target rows`. `RtRow0Y` models the 0.5, so the
two must change together.

**F2. Nothing fits the columns.** At 125 / 175 / 225 % two 1 pt side borders are x.5 px, so a
docked Free Aspect view is drawn half a column narrower than its texture (175 %: a 1927 px pane,
1923.5 px area, 1924 px texture). The whole-pixel experiment is the proposed fix.

**F3. A pane docked at the editor window's left or right edge has one side border, not two.**
The Free Aspect anchor's X is half the slack, `(slack + 1) / 2`. Between two neighbours at 250 %:
slack 5, +3, correct (measured). At the left edge the left border is absent: slack 2 or 3 gives
+1 or +2, the texture starts at +0. At the right edge the left border is there (2.5 px -> +3) but
the slack is still 2 or 3: +1 or +2. Either way 1-2 px off. At 300 % far left: pane 1845, texture
1842, anchor +2, texture at +0 (measured by screen capture on another plug-in). The letterbox path
has the same assumption (`side = (host - draw) / 2`). The real border is available from Unity and
is what the whole-pixel path already uses; the 1:1 line prints it next to the anchor's `dx`.

**F4. The canvas is the rounded target, not the texture.** `Mathf.RoundToInt` rounds halves to
even: 859.5 -> 860 but 860.5 -> 860, while the texture was measured as the area rounded up (#727).
Where they differ the canvas is one row short of the texture and the mirror blit resamples.
Reading the texture's own size would remove the guess. Unconfirmed: the `canvas=` field decides.

**F5. Quarter-pixel edges are unmeasured.** A whole-point edge that falls on x.5 or x.75 px starts
on the next pixel (measured at 250 %, 150 % and 175 %). For x.25 (125 % / 175 %) this code rounds
down; a rule of "always the next pixel" would say up. One pixel, and only a measurement decides.

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

**250 % (the dev panel's native scale) - expected:**

- Centred: `W ... (1:1), H ... (1:1)`, `canvas= ... (= texture)`, `left border 1 pt = +3 px`,
  `anchor at pane +3,...`, no warning, correct with nudge 0. In the reference layout the size-change
  line ends `render=(1020,406 1728x860)`. **This must not have changed.**
- Far left: `left border 0 pt = +0 px` but `anchor at pane +1` or `+2` -> F3. Expect to need a nudge
  of -1 or -2. Far right: `left border 1 pt = +3 px`, anchor `+1` or `+2`; expect +1 or +2.
- Any layout where `canvas=` says `NOT the texture's size` -> F4; note the pane height.
- Resize the pane's height by a pixel or two a few times (drag a splitter, let go, wait): `H` must
  stay `(1:1)` for every height if the texture is always the area rounded up; if it alternates,
  that is F1's last sentence.

**175 % (or 125 %) - expected:**

- Centred: the horizontal warning, `W drawn x.50 px for a ... px texture (up to 1 dropped)` (or
  doubled), and a vertical line near the middle of the view with the eyes swapped on one side.
  `H` reports a quarter row.
- Maximized: `W ... (1:1)` (no side borders); `H` still a quarter row.

**300 % or 200 % if available:** `W (1:1)`, `H drawn x.50 ... (up to 1 doubled)` and the vertical
warning (F1). Look for a horizontal line with ghosting on one side.

### Pass B - the whole-pixel experiment

Close the editor, `set DISPLAYXR_PROV_GV_WHOLEPIXEL=1`, start it, same layouts.

- At 175 % / 125 %, about half a second after Play: `whole-pixel Game view: selected ...`, then a
  `1:1` line with `(1:1)` twice, `[whole-pixel size]`, and `GVFIT skipped ...`. The Game view's
  size menu shows "DisplayXR whole-pixel"; a thin grey margin (under 7 px at 175 %) surrounds the
  image. **The vertical seam of pass A must be gone.** If the whole view is now uniformly swapped,
  find the nudge and record it with the `1:1` line - that is F5 (expect +1 or -1).
- Drag a splitter for a few seconds: no log spam during the drag, one `resized to` line and one
  size change about half a second after letting go.
- Stop Play: `restored the previous size ...`; the size menu is back on what it was (Free Aspect)
  and the custom entry is gone. Check the same after a script recompile during Play (it goes and
  comes back) and after picking another size by hand during Play (it stays off).
- At 250 % the experiment also engages (2 pt = 5 px steps). The image shrinks by up to 4 px; it
  must look as right as pass A did.
- At 100 / 200 / 300 %: `nothing to do at ... %`, and nothing else changes.

### What to send back

For every scale x layout x pass: the `GameView 1:1:` line, `GameView glue size change:`,
`GameView glue matched pane:`, `#740 dock-border:`, any warning, what was seen, and the nudge.
With those numbers F1-F5 can each be confirmed or dropped without another session.

## Not verified without a panel

Everything in the recipe. In particular: that the whole-pixel size removes the 175 % seam in this
plug-in's texture path (it was measured in another plug-in's editor weave on the same Unity Game
view); the anchor at quarter-pixel edges (F5); F3 and F4; the experiment's reflection on Unity
2022.3 (the member names and `RemoveCustomSize`'s index meaning were read from the Unity 6000.4
editor assemblies only).
