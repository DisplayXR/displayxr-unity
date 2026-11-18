# Auto-Convergence (camera-centric rig)

`DisplayXRAutoConvergence` (#461) keeps the zero-parallax "screen" plane of a
[`DisplayXRCamera`](kooima-pipeline.md) rig on the subject while the camera moves, and can
back the stereo strength off so the scene stays inside a comfort budget. With a free camera a
fixed `invConvergenceDistance` is wrong most of the time: too near and the whole scene sinks
behind the glass and goes flat, too far and everything pops out and strains.

It does nothing on a display-centric (`DisplayXRDisplay`) rig: there the screen plane is the
authored virtual display, which is the point of that rig.

## Using it

Add **DisplayXR > Auto Convergence** to the camera that carries the camera-centric rig. That is
all; the defaults are the tuned ones.

| Setting | Default | What it does |
|---|---|---|
| Mode | Auto | Where depth samples come from - see [Sources](#sources) |
| Drive Stereo Strength | on | Also lower the rig's IPD Factor when the caps would be exceeded. Never raises it |
| Min / Max Distance | 0.3 / 100 m | Limits of the screen plane, in rig units |
| Time Constant | 1.2 s | The slow follower. 1.0-1.5 reads as a drift; 0.6 visibly pumps |
| User Depth Scale | 1 | (0, 1]: scales both caps - a per-user "less depth" preference |
| Front / Behind Cap | 0.6 / 1.2 deg | Largest disparity in front of / behind the screen, as an angle seen from the display's nominal viewing distance |
| Raycast Layers | all but Ignore Raycast, UI | Raycast source only |
| Ignore Nearer Than | 0 (off) | Leave samples nearer than this out (first-person weapon, hands) |
| Advanced | | Sample period (0.15 s), percentile (80), blend (0.5), dead band (2 % of the budget), ray grid (9 x 7) |

From code:

```csharp
var ac = camera.GetComponent<DisplayXRAutoConvergence>();
ac.ResetConvergence();   // after a teleport, a cut, a scene load
float d = ac.ConvergenceDistance;   // also: TargetConvergenceDistance, StereoStrength,
                                    // NearDisparityPercent, FarDisparityPercent, IsHolding, Source
```

**Cuts are not auto-detected.** `ResetConvergence()` makes the next sample count immediately
and be followed over a few frames (a ramp, never a one-frame snap). Without it a teleport is
followed over the slow time constant, which looks like the scene swimming into place.

### What it writes, and what it leaves alone

- It writes only the rig's own fields, `invConvergenceDistance` and (with Drive Stereo Strength)
  `ipdFactor`, every frame from `LateUpdate`. They reach the runtime through the rig's normal
  per-frame push (`DisplayXRProviderDriver`), so nothing has to be re-armed after a session
  restart (dock / undock, Android resume).
- The **authored** values are remembered and restored when the component is disabled. A value
  somebody else writes into the rig while it runs (the inspector, an app slider) becomes the new
  authored value: for the IPD Factor that is the ceiling, for the convergence it is what gets
  restored.
- It only acts while its camera is `DisplayXRRigManager.ActiveCamera`, the provider session is
  running and the boot splash is over. Otherwise it holds.
- The runtime still owns the projection (`XR_DXR_view_rig`). The rear depth budget
  (`XR_DXR_depth_budget`) is not read, smoothed or written.
- No per-frame allocation: fixed buffers, an in-place sort, non-allocating physics queries.

With `foregroundOnlyClip` on, the clip plane follows the convergence plane, so auto-convergence
moves the clip too. That is rarely what a transparent overlay wants; use a fixed convergence or
`Target` mode there.

## Where the screen plane goes - and why not the mean

Putting the screen on the **mean** depth, or on the tallest bin of a depth histogram, was tried
on a real panel and rejected: both land on the big far plateau (sky, scenery, the back wall),
so most of the frame pops out in front of the glass. Stereographers do the opposite: the
subject sits at or just in front of the screen and the scenery falls behind it.

The statistic works in **nearness** `q = 1 / depth` (1/m), metric, in the rig's own units:

1. **Centre-weighted 80th percentile of nearness.** Each sample is weighted by a Gaussian
   centre prior (sigma 0.35 of the width and height) and the bottom rows are damped to 0.5
   (floors and props at the feet are near but rarely the subject). The 80th percentile means
   roughly a fifth of the weighted frame is in front of the screen.
2. **The dominant clump.** A 64-bin histogram of nearness, smoothed (sigma 1.5 bins); the
   highest peak that is not in the far 15 % of what the frame spans. Sky / far-plane samples
   are left out of this search altogether. A clump already being followed is kept unless
   another one scores 1.5x higher, so two comparable surfaces do not trade places.
3. **Screen plane = 0.5 x percentile + 0.5 x clump.** If only the far plateau has a peak, the
   percentile alone is used.

Why nearness and not depth: on-screen disparity is linear in `q`
(`d = k * ipdFactor * (q_screen - q)`), so a fixed histogram over `q` has uniform resolution in
exactly what the eye sees. Two depths that fall in the same bin differ by a fraction of a
percent of disparity - stereoscopically they are the same depth.

Nothing is re-normalised per frame. The histogram range is fixed (0 to 1 / Min Distance) and
every value is metric. Per-frame normalisation makes the same scene read differently when
something enters the frame (false cuts) and makes a depth budget impossible.

A sample is rejected - and the last target **held** - when fewer than 3 % of the samples have
real depth (a frame of sky), or the clump holds less than 6 % of the weighted mass.

### Stereo strength

With the same linear model, the disparity of the nearest and farthest content is known before
it is rendered. The 98th / 2nd percentile of nearness (unweighted - a comfort violation hurts
anywhere in the frame; sky counts as the far end) are held against the caps:

```
k        = 0.063 m / (2 tan(hfov / 2))                 view widths per (1/m)
front    = k * (q_near - q_screen)                     at strength 1
behind   = k * (q_screen - q_far)
strength = min(authored, capFront / front, capBehind / behind)
```

Strength is only ever reduced, never raised above the authored IPD Factor. It backs off about
three times faster than it returns.

The caps are **asymmetric** - more is tolerated behind the screen than in front; the defaults
split the budget 1/3 front, 2/3 behind. They are angles so that they mean the same on any
display; they become a fraction of the view width through the display's **nominal** viewing
distance and physical width (`XR_DXR_display_info`), with a 0.6 m / 0.344 m fallback. The live
head position is deliberately not used: a budget that follows the head makes the picture
breathe as the viewer leans.

The plug-in does not consume the runtime's `XR_DXR_stereo_comfort` bounds yet (see
[Limitations](#limitations)); the caps are the component's own fields.

### Over time

- Sampled every 0.15 s (about 7 Hz). The follower is far slower, so the one or two frames of
  readback latency do not matter.
- Median of the last three samples, then a dead band (2 % of the comfort budget, expressed in
  nearness), then a critically damped spring with the Time Constant - no overshoot from rest.
- The first sample, and the one after `ResetConvergence()`, are followed with a 0.06 s time
  constant for at most 0.4 s. When that fast phase ends its velocity is dropped: carried into
  the slow spring it overshoots the new plane and drifts back for seconds.

## Sources

| Mode | Samples | Notes |
|---|---|---|
| **Depth** | A 32 x 32 grid of the camera depth texture | Asynchronous GPU readback; never waits. Reads whatever wrote depth |
| **Raycast** | A grid of physics rays (9 x 7) through the rig's authored frustum | Needs colliders and the physics module. Honors Raycast Layers. A miss is sky |
| **Target** | A Transform; its Renderer bounds; or its `DisplayXRContentBounds` | The centre goes on the screen, the near / far faces size the strength |
| **Auto** | Depth where the pipeline provides it, else Raycast | Falls to rays when the depth hook reports it has no depth, or no grid arrives for 1 s; goes back when grids arrive |

**Depth, per pipeline.** The grid reuses the depth-aware cursor's readback
([cursor-depth.md](cursor-depth.md)) - the same 32 x 32 target, the same three pipeline hooks,
the same readback - pointed at the whole view for one frame instead of at the square under the
cursor. So it behaves exactly as the cursor's depth source does:

| Pipeline | Depth available |
|---|---|
| URP (17+) | Yes. The camera's Depth Texture is switched on while the component is enabled (restored afterwards) |
| HDRP (17+) | Yes. Not with an MSAA depth buffer |
| BiRP deferred | Yes (the depth is a by-product of the G-buffer) |
| BiRP forward | Only if the camera already renders a depth texture. **A depth pre-pass is never forced on the app** - Auto uses rays instead |
| Custom SRP | No - Auto uses rays |

Each grid is offset by a quarter texel in a four-sample rotation, so consecutive samples do not
read the same pixels. If the depth-aware cursor is also using scene depth, it skips the one
frame in which a grid is drawn (one in about nine at 60 fps), well inside its own freshness
window.

## Editor

- **Convergence as a distance.** The `DisplayXRCamera` inspector shows the convergence as
  metres as well, editable; type `infinity` (or clear the field) for parallel. It edits the same
  serialized inverse-distance field.
- **Comfort volume gizmo.** Both rigs draw the front (red) and behind (green) comfort limits
  either side of the screen plane, from the same caps (the defaults when there is no
  auto-convergence component). Toggle with **DisplayXR > Gizmos > Comfort Volume**. It is an
  authoring approximation - similar triangles from a 63 mm eye separation and the nominal
  viewing distance - not the runtime's projection.
- The component's inspector shows a live readout in Play Mode: source, delivered and target
  distance, strength, near / far disparity against the caps, sample counts.

## Tuning

- **Scene pumps / breathes** - raise Time Constant (1.5), or raise the dead band.
- **Subject sits too far out of the screen** - lower Percentile (70) or lower Front Cap.
- **Scene looks flat** - a cap is biting: check Stereo Strength in the readout; raise the caps
  or the User Depth Scale, or check that a near prop is not dominating (Ignore Nearer Than).
- **Screen plane sticks to a first-person weapon** - Raycast: take its layer out. Depth: set
  Ignore Nearer Than just beyond it.
- **Follows too slowly after a cut** - call `ResetConvergence()` at the cut.

## Limitations

- **Frame-edge (window) violations are not handled.** Content in front of the screen that is cut
  by the window edge is uncomfortable however small its disparity. The centre prior makes it
  less likely that such content sets the plane, but nothing detects or corrects it (no floating
  window). Unsolved on the runtime side too.
- **Transparents do not write depth.** Glass, particles, world-space UI and anything else in
  the transparent queue is invisible to the Depth source; the plane goes to whatever is behind
  it. Use Raycast (with colliders) or Target for such subjects.
- **The Depth source cannot filter by layer** - it reads the depth texture as rendered. Use
  Ignore Nearer Than, or Raycast.
- **32 x 32 samples.** Enough for percentiles of a frame (a 64-128 wide grid was the
  recommendation; the cursor's 32 x 32 target is shared so that no pipeline hook had to change).
  A subject thinner than a thirtieth of the frame can be missed in one sample; the jitter and
  the median cover that.
- **Depth is taken from the left eye**, corrected for the eye's offset along the camera axis.
- **Disparity is an estimate** (parallel-axis similar triangles, nominal 63 mm eye separation).
  It sizes the caps; it is not a measurement of the rendered views.
- **Raycast sees colliders, not pixels.** Geometry without colliders reads as sky.
- **`XR_DXR_stereo_comfort` is not bound.** The runtime can report the comfort zone for the
  current viewer (front / behind bounds, the user's depth preference). Consuming it needs a
  native binding: the extension enabled at instance creation when advertised,
  `XrStereoComfortDXR` chained on `XrViewState` at the provider's `xrLocateViews`, the result
  stored in the provider's shared state, and one getter exported to C# (plus the struct in
  `native~/displayxr_extensions.h`, which the drift check then covers). The component would then
  take `frontDisparityMeters / behindDisparityMeters` over the canvas width in place of its own
  caps, and `userDepthScale` in place of User Depth Scale. Use the nominal-distance figure, not
  the tracked one, for the reason above.

## Files

- `Runtime/DisplayXRAutoConvergence.cs` - the component
- `Runtime/DisplayXRAutoConvergenceMath.cs` - statistic, caps, follower (pure C#, no UnityEngine)
- `Runtime/DisplayXRCursorDepthPatch.cs` - the shared depth readback (`RequestGrid`, `Grid*`)
- `Editor/DisplayXRAutoConvergenceEditor.cs` - inspector, gizmo menu
- `Tests~/CursorDepthMath/AutoConvergenceTests.cs` - unit tests, run in CI with the cursor math
