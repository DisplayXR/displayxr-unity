# Stereo media (`DisplayXRStereoMedia`)

A stereo photo or video stores the two eye pictures in one frame, side by side or one
above the other. `DisplayXRStereoMedia` shows such a frame so that each eye sees its own
half. This page covers what it supports, how it draws on each render pipeline, how colour
is handled, and what still has to be checked by eye.

- Component: `Runtime/DisplayXRStereoMedia.cs`
- Math (pure C#, tested in CI): `Runtime/DisplayXRStereoMediaMath.cs`, `Tests~/StereoMediaMath`
- Shader: `Runtime/Resources/DisplayXRStereoMedia.shader` (`DisplayXR/StereoMedia`)
- Inspector: `Editor/DisplayXRStereoMediaEditor.cs`
- Sample: `Samples~/StereoMedia`

## Using it

Add **DisplayXR > Stereo Media** to any GameObject and assign a **Texture** (a `Texture2D`
or a `RenderTexture`) or a **Video Player**.

```csharp
var media = gameObject.AddComponent<DisplayXRStereoMedia>();
media.texture = mySideBySidePhoto;
media.layout = DisplayXRStereoLayout.SideBySide;
media.presentation = DisplayXRStereoMedia.PresentationMode.FullScreen;
```

The component submits the picture each frame with `Graphics.RenderMesh`. It adds no
renderer and no child object, never moves or scales the GameObject it sits on, and
allocates nothing per frame. Disabling the component or deactivating its GameObject simply
stops the draw.

### Importing a stereo photo

Two texture import defaults hurt a stereo picture, and the inspector warns about both:

- **Non-Power of 2** other than *None* resizes the image: 3840×1080 becomes 4096×1024, which
  changes the picture's shape (and resamples it). Set it to *None*. If you cannot, state
  the true size in **Source Size** — only the shape is used.
- **Generate Mip Maps**: see *The seam* below. Turn it off for a picture shown at or near
  its own size.

## Layouts

| Layout | Left eye | Right eye |
|---|---|---|
| `SideBySide` | left half | right half |
| `SideBySideSwapped` | right half | left half |
| `TopBottom` | top half | bottom half |
| `TopBottomSwapped` | bottom half | top half |
| `Mono` | whole picture | whole picture |

**Half Resolution** is for media that squeezes each eye picture to half size along the
packing axis so the frame keeps a normal shape: half-width side-by-side (two 16:9 pictures
in 1920×1080) or half-height top-bottom. Each eye is then shown at the aspect of the whole
texture. With it off, each eye picture is taken at its stored shape (3840×1080 holds two
16:9 pictures). The inspector warns when the picture's shape does not look like the
setting; it is a hint, since a 4:3 or portrait source can trip it legitimately.

**Pixel Aspect** is the width / height of one stored pixel. `0` takes it from the Video
Player.

**Depth Shift** moves the whole picture in depth by shifting the two eye pictures apart,
in percent of the picture width: positive pushes it behind the screen, negative pulls it
towards the viewer. Both pictures are cropped by the same amount, so no edge is ever
sampled outside its half. It is a horizontal image translation, the same control stereo
players call convergence; it does not change the depth range inside the picture.

**Mono fallback.** Both eyes get the left picture when the session is not rendering
stereo — no session, or an active rendering mode with one view — and with **Force Mono**.
The view count of the active mode is the single test. A panel switched to 2D is covered by
it: the provider answers the hardware state change by requesting the matching one-view
rendering mode. The state is re-read on the provider's session, rendering-mode and
hardware-state events, not polled. Outside Play Mode the component always shows the left
picture.

## Presentation

### Quad

A quad in the scene. With **Placement = Zero Parallax Plane** it is put on the plane where
the rig adds no disparity, sized to cover the view, so the only disparity the viewer sees
is the picture's own:

| Rig | Plane | Size |
|---|---|---|
| `DisplayXRDisplay` | the rig transform (it is the virtual display) | `virtualDisplayHeight` tall, or the physical display height when that is 0 |
| `DisplayXRCamera` | `1 / invConvergenceDistance` in front of the camera | what the rig's authored vertical FOV spans at that distance |

Width is height × the aspect of the window the session composites into. Only public rig
data is read; the projection is the runtime's (`XR_DXR_view_rig`), and the plug-in computes
none of it. `DisplayXRStereoMedia.TryGetZeroParallaxPlane` exposes the same plane for your
own objects.

The component runs at execution order 9999, just before the provider driver (10000)
samples the rig pose it sends the runtime, so the quad is placed from the pose that is
rendered this frame rather than the previous one.

A camera-centric rig that converges at infinity (`invConvergenceDistance = 0`) has no such
plane; the component then logs once and falls back to its own transform and **Quad Size**,
as **Placement = Manual** does.

The quad is still a scene object: it is drawn for every camera, other objects can be in
front of or behind it, and it shifts against them as the viewer's head moves. That is the
difference from full screen.

### Full screen

Each eye's half goes straight to that eye's render target: the vertex shader writes clip
space directly, with no view and no projection matrix. So the rig adds no parallax, and
nothing moves with the head. **Order** decides when it is drawn:

- `Background` — after the opaque queue and the skybox, before transparents, on the far
  plane with a depth test. It replaces the skybox / clear colour, and scene geometry draws
  over it.
- `Foreground` — after transparents, ignoring depth.

Clip-space geometry would cover any camera that drew it, so it is drawn for exactly **one**
camera: **Rig Camera**, or the active rig camera (`DisplayXRRigManager.ActiveCamera`) when
that is empty. The camera is a parameter of the draw (`RenderParams.camera`), so the Scene
view, previews, reflection probes and a second Game camera rendering into a texture never
get it, and nothing is toggled between cameras. With no rig camera at all, nothing is
drawn.

### Fit

| Mode | Quad | Full screen |
|---|---|---|
| `Fit` | the quad shrinks to the picture's aspect inside the frame | bars where the view is not covered |
| `Fill` | frame covered, picture cropped about its centre | view covered, picture cropped |
| `Stretch` | frame covered, aspect not kept | view covered, aspect not kept |

In full-screen `Fit`, the bars show whatever is behind: the scene or clear colour.

## Per-pipeline mechanism

One shader serves all three pipelines. That is deliberate, and rests on these facts:

1. **No pipeline include.** It is written against `UnityCG.cginc` only, so it can live in
   `Runtime/Resources/` (which puts it in every build) without breaking a project that has
   no URP or HDRP. A shader that includes URP or HDRP libraries must never be there — see
   `CLAUDE.md`, the #130 revert. `DisplayXRCursorDepthPatch.shader` follows the same rule.
   The shader has exactly one pass.
2. **It is drawn by every pipeline.** A pass with no `LightMode` tag is `SRPDefaultUnlit`.
   URP draws it in its opaque and transparent passes; HDRP lists it in its forward pass
   names as "a fall back to legacy unlit rendering". `Sprites/Default`, which the boot
   splash uses on all three pipelines, is drawn the same way.
3. **The built-in stereo constants are set.** In single-pass instanced rendering
   `UnityCG.cginc` reads `unity_StereoMatrixVP[eye]` / `unity_StereoMatrixP[eye]`. URP
   (`UniversalCameraData.PushBuiltinShaderConstantsXR`) and HDRP
   (`HDCamera.PushBuiltinShaderConstantsXR`) both publish them through
   `XRBuiltinShaderConstants` for exactly this purpose, and both set `STEREO_INSTANCING_ON`,
   which `#pragma multi_compile_instancing` declares.

So both presentations use the same mechanism on every pipeline: one mesh submitted with
`Graphics.RenderMesh`, drawn in the pipeline's normal scene pass like any renderer. No
`OnRenderImage`, renderer feature or custom pass is involved, and nothing has to be set up
in the project.

| | Quad | Full screen |
|---|---|---|
| **BiRP** (multi-pass) | yes | yes |
| **URP** (single-pass instanced) | yes | yes |
| **HDRP** (single-pass instanced) | yes, but see the HDRP limits below | yes, but see the HDRP limits below |

### Render queue: never the opaque queue

Even an opaque quad is drawn at queue **2900** (`Transparent − 100`), with depth write on.
The reason is URP's depth priming (Depth Priming Mode *Forced*, or *Auto* when a prepass
is needed, for instance SSAO with depth-normals and no MSAA): `DrawObjectsPass` then draws
the opaque queue with `DepthState(false, CompareFunction.Equal)` against the depth
prepass. A shader with no `DepthOnly` pass is not in that prepass, so every one of its
fragments fails the test and an opaque-queue quad **is not drawn at all**.

Two ways out were considered:

- **Add `DepthOnly` / `DepthNormalsOnly` passes.** They would have to be written without
  URP's library, and `DepthNormalsOnly` has to match what URP expects in its normals
  target — world normals, octahedral-packed under a keyword in the deferred path, plus a
  second target when rendering layers are written. HDRP also has a `DepthOnly` pass name,
  with its own stencil and normal-buffer conventions, so the same pass would be picked up
  there with outputs HDRP does not expect. That is a lot of pipeline contract to reproduce
  blind in a shader that cannot be compiled or looked at here.
- **Stay out of the primed queue.** URP applies the `Equal` state only to its opaque pass
  (`m_IsOpaque`); its transparent pass draws `SRPDefaultUnlit` with the shader's own depth
  state. BiRP and HDRP likewise draw this queue with the shader's state. This is what the
  component does.

The queue numbers are chosen against all three pipelines: at or below 2500 is URP's opaque
(primed) range; 2501–2649 is HDRP's after-post-process opaque range; 2900–3100 is HDRP's
plain transparent range, which URP and BiRP also draw as transparents.

| What | Queue | Depth |
|---|---|---|
| Opaque quad | 2900 | test `LEqual`, write on |
| Quad with alpha | 3000 | test `LEqual`, write off |
| Full screen, Background | 2900 | far plane, test `LEqual`, write off |
| Full screen, Foreground | 3100 | test `Always`, write off |

What this costs is listed under Limits (*Not in the depth prepass*).

### Where the eye index comes from

The shader decides the eye in the **vertex** stage and passes that eye's UV mapping to the
fragment stage as interpolants. The fragment stage needs no eye index at all.

| Path | Source of the eye |
|---|---|
| **Single-pass instanced** (URP, HDRP; D3D11, D3D12, Metal, Vulkan) | The engine doubles the instance count; `UNITY_SETUP_INSTANCE_ID` sets `unity_StereoEyeIndex = instanceID & 1`, and `UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO` routes the instance to array slice 0 or 1. |
| **Multi-pass** (BiRP) | Either of two, OR-ed: the engine's per-camera `unity_StereoEyeIndex`, and `_DXRMediaScriptEye`, a global the component sets from `Camera.onPreRender` to 1 while `Camera.stereoActiveEye` is `Right`. |
| **No stereo** | Both are 0: the left picture. |

The second BiRP source exists because whether the engine sets `unity_StereoEyeIndex` to 1
for the right-eye pass *under this provider* (which issues one render pass per eye) could
not be settled from source. `Camera.stereoActiveEye` is the signal the BiRP per-eye
foreground clip (`DisplayXRForegroundClipBiRP`) already relies on under this provider — it
reads it in `OnRenderImage`; here it is read in `onPreRender`, before the eye's draws,
because the value has to be in place when the mesh is drawn. OR-ing them is safe: neither
can be 1 during a left-eye or non-stereo pass (a camera that does not render stereo
reports `Mono`, which sets 0), so if either mechanism works the right eye gets the right
picture. The subscription is made only when the Built-in pipeline is active; under URP and
HDRP the global is never set and stays 0. **This is still the first thing to verify on
hardware** — see Verification.

An odd index is the right eye, an even one the left, so an extra pair (another 3D zone, a
mode with more than two views) alternates.

### Picture orientation

`DisplayXRStereoMediaMath` works in *image space* (v = 1 is the top of the picture) and
hands the shader, per eye, a scale / offset that maps the quad's UV to texture UV. "Top
half" is therefore decided once, in tested C#, not per graphics API:

- A texture that follows Unity's convention — an imported texture, a render texture Unity
  rendered into, a Video Player's output — has v = 1 at the top on every API. Unity hides
  the D3D / Metal / Vulkan row order behind that convention.
- **Flip Vertical** is for a texture stored upside down (some native / external textures):
  the mapping becomes v → 1 − v, so the picture comes out upright *and* the top half is
  still the top half.
- The shader also mirrors v when Unity itself marks the texture as flipped
  (`_MainTex_TexelSize.y < 0` on a UV-starts-at-top API). It does so after the eye's
  rectangle is applied, so the halves do not swap.

In full-screen mode the quad's "up" must be the scene's "up". Whether clip-space +Y is up
depends on the API and on whether the pass renders into a texture. The shader takes the
answer from the sign of the Y scale of the projection matrix of the pass it is drawn in
(`UNITY_MATRIX_P[1][1]`), which Unity negates exactly when it flips — the same matrix the
scene's own geometry is drawn with, so the two cannot disagree.

### The seam

A linear filter tap reaches half a texel to either side, so sampling right at the edge of
an eye's half would blend in the other eye's edge column. Half a texel **of the mip level
in use**: a texel of mip *n* is 2ⁿ base texels wide, so the tap reaches 0.5 · 2ⁿ base
texels. A fixed half-base-texel guard is therefore only enough at mip 0; at mip *n* it
leaves the other eye's texel a weight of (2ⁿ − 1) / 2ⁿ⁺¹ — a quarter at mip 1, three
eighths at mip 2 — which shows as a column of the wrong eye along the inner edge of a
minified picture.

The shader computes the mip level per pixel from the UV derivatives (clamped to the
texture's last level), rounds it up — trilinear filtering blends two levels and the
coarser decides — and keeps each eye 0.5 · 2^level base texels inside its own half
(`DisplayXRStereoMediaMath.SeamInsetTexels`, tested for levels 0–3). It samples with the
derivatives of the unclamped coordinate, so the mip level does not drop at the clamp.

The price is that the picture's own edge texels are repeated across that inset, about half
a displayed pixel wide. A texture without mipmaps is always sampled at level 0 and pays the
minimum, half a texel. Anisotropic filtering spreads its taps along the long axis of the
footprint; the inset is derived from that long axis, so it covers them, but this has not
been looked at on hardware.

## Colour

Photos and video hold sRGB-encoded values. What has to happen to them depends on the
project's colour space and on the texture's *format* (runtime ADR-044: an `_SRGB` format
means the bytes are encoded and the GPU decodes on sampling; a UNORM format is sampled as
it is):

| Project | Texture format | **Encoding = SRGB** (default) | Encoding = Linear |
|---|---|---|---|
| Linear | sRGB | nothing (the sampler decodes) | shader re-encodes (undoes the sampler's decode) |
| Linear | UNORM | **shader decodes** | nothing |
| Gamma | either | nothing | shader encodes |

The second row is the one that bites: a non-sRGB `RenderTexture` that a Video Player
decodes into, in a Linear project, holds encoded bytes the GPU will not decode, and looks
washed out with an ordinary unlit shader. The component decodes it in the shader instead.
The decision is `DisplayXRStereoMediaMath.ColorTransform`, from the texture's format
(`GraphicsFormatUtility.IsSRGBFormat(texture.graphicsFormat)` — the format itself, since
that is what decides whether the sampler decodes) and `QualitySettings.activeColorSpace`;
the conversions use the exact piecewise sRGB curve.

**Alpha.** `Opaque` (default) ignores the texture's alpha and writes 1. `Premultiplied`
and `Straight` say what the alpha channel means; the picture is then blended
`One, OneMinusSrcAlpha` and alpha is written to the target, which is what an alpha-native
transparent overlay needs. **Tint** multiplies the result (its alpha fades a non-opaque
picture).

## Video

Assign a `VideoPlayer` to **Video Player**:

- If its **Render Mode** is *Render Texture* and it has a **Target Texture**, that texture
  is shown, and the player is left alone.
- Otherwise the component switches it to *API Only* (so the player does not draw the whole
  packed frame itself) and shows `VideoPlayer.texture` once the player is prepared. With
  **Prepare Video** on it calls `Prepare()` when enabled.

The texture, its size and the pixel aspect are read every frame (plain property reads), so
a new clip or URL is picked up; looping and seeking keep the same texture. Nothing is
drawn until a texture exists.

`VideoPlayer` lives in Unity's built-in **Video** module. The plug-in does not depend on
it: the field and the code are compiled only when the module is enabled
(`DISPLAYXR_HAS_VIDEO`, a `versionDefines` entry on `com.unity.modules.video`, like the
Physics module).

## Limits

- **Not in the depth prepass** (all pipelines, a consequence of the queue choice above).
  The quad is not in URP's camera depth texture or HDRP's depth buffer when effects that
  read them run, so: SSAO, depth of field and other depth-based effects treat it as not
  there; the depth-aware cursor's Depth source sees what is behind it; a transparent
  object in a queue *below* 2900 that is in front of the quad is drawn over by it. It
  still depth-tests against the scene and writes depth itself, so ordinary opaque and
  transparent (queue 3000) objects sort correctly against it.
- **HDRP: temporal anti-aliasing.** The shader has no depth-prepass and no motion-vector
  pass, so TAA (and anything else that reprojects) treats the picture's pixels as the
  background behind them. Under a head-tracked camera that can smear the picture. Use a
  non-temporal anti-aliasing mode on that camera.
- **HDRP: fog.** The picture is drawn after HDRP's opaque fog pass and the shader does not
  evaluate fog itself, so fog between the viewer and the picture is not applied to it
  (in the opaque queue it would instead have been fogged at the depth of whatever is
  behind it, since it is not in the depth buffer). Expect it to look unfogged in a foggy
  scene.
- **HDRP: colour.** The picture is drawn in HDRP's scene pass, before post-processing, so
  tonemapping and any other post effect apply to it, and its values are taken as already
  exposed. For faithful colour use a volume without tonemapping on that camera. An
  after-post-process custom pass would avoid this; it is not shipped, because it could not
  be verified on hardware when this was written.
- **HDRP without a session: full-screen orientation unknown.** With a session, HDRP's
  built-in stereo projection is published with "render into texture" set, which matches
  HDRP's own matrices. Without one (the non-XR path) the shader reads `unity_MatrixVP`'s
  projection as the engine set it for the camera, and whether its Y sign matches the
  target HDRP is then rendering into could not be determined from source. If a full-screen
  picture is upside down on HDRP with no session, that is the cause. Quad presentation is
  not affected (it uses the same matrix as every other built-in-shader object).
- **Post-processing in general.** The same applies to URP post-processing and to the rig's
  FXAA (**Post-Process AA**), which softens the picture slightly; turn it off for a
  pixel-exact viewer.
- **Foreground-only clip.** A rig with `foregroundOnlyClip` on (transparent-overlay apps)
  clips everything at or behind the display plane, which is exactly where a
  zero-parallax quad and a full-screen picture are. Do not combine them.
- **Forced multi-pass on URP / HDRP** (`DISPLAYXR_FORCE_MULTIPASS=1`, a development
  override) is not supported. Nothing in either pipeline's source sets a per-eye index
  for built-in shaders on that path, so expect the left picture in both eyes. Untested.
- **`Graphics.RenderMesh` under single-pass instancing.** It queues an ordinary renderer
  for the frame, and stereo instancing is applied by the engine when the pipeline draws
  renderers, so it is expected to behave exactly as the boot splash's mesh renderers do.
  That happens in engine code, not in C# that could be read, so it is on the verification
  list rather than proven.
- **Edit-mode preview.** Outside Play the draw is submitted once per camera render (from
  `Camera.onPreCull` on BiRP, `RenderPipelineManager.beginCameraRendering` on URP / HDRP)
  so it survives repaints. It is a convenience and has not been checked on HDRP; Play Mode
  submits from `LateUpdate` and does not depend on it.
- **Windowed display-centric rigs with `virtualDisplayHeight = 0`.** The plane height is
  taken as the physical display height, which is right for a full-screen window. In a
  smaller window set `virtualDisplayHeight`, or use **Placement = Manual**.
- **A scaled rig.** The plane follows the rig's gizmo: defined in the rig's local units
  and scaled by its transform. Only scale 1 has been reasoned through end to end.
- **Display zones** (`XR_DXR_display_zones`). Full screen fills every 3D zone's view. On
  BiRP, which eye an extra zone's passes report is not known.
- **More than two views.** Only two pictures exist; odd views get the right one.
- **Batching.** The shader is not SRP Batcher compatible (it is one draw).

## Verification

The math is covered by `Tests~/StereoMediaMath` (`dotnet run --project Tests~/StereoMediaMath -c Release`,
run in CI): every layout × eye, the swapped variants, half resolution, the fit modes, the
depth shift, flipped textures, the seam inset per mip level and the colour decision.

The shader cannot be tested in CI. Check these by eye with the **Stereo Media** sample on
the runtime's simulated side-by-side display (`SIM_DISPLAY_OUTPUT=sbs`, no 3D panel), where
the window shows the left eye's view in its left half and the right eye's in its right
half. The sample's left picture is red with an **L**, its right picture blue with an **R**.
Repeat on BiRP, URP and HDRP, and per graphics API in use.

1. **BiRP multi-pass eye index — the open question.** On a Built-in pipeline project: the
   right half of the window must show the blue **R** picture. If both halves show the red
   **L** picture, neither eye source works under this provider (`unity_StereoEyeIndex` is
   not 1 in the right-eye pass *and* `Camera.onPreRender` does not report
   `stereoActiveEye == Right` per eye). To tell the two apart, log
   `Camera.stereoActiveEye` from a `Camera.onPreRender` handler: `Left` then `Right` each
   frame means the script source works. If the project uses extra 3D zones, check each
   zone's two halves as well.
2. **Eye separation.** Left half of the window: the red **L** picture, whole. Right half:
   the blue **R** picture, whole. A half that shows both a red and a blue part means the
   eye index is not reaching the shader. On URP and HDRP this also proves that
   `Graphics.RenderMesh` is drawn into both array slices.
3. **Not swapped.** Red **L** left, blue **R** right — not the other way round.
4. **Upright.** The letters are at the top left and read correctly; the white frame runs
   round all four edges. Check Quad and Full Screen separately (full screen has its own
   up/down rule), and on HDRP also with no session running.
5. **URP depth priming.** In the URP renderer set **Depth Priming Mode** to *Forced*: the
   opaque quad is still drawn. Then *Auto* with SSAO (Depth Normals) enabled: still drawn.
6. **Top-bottom.** Set the sample's **Pattern Layout** to `TopBottom`: same result as 2–4.
7. **Swapped layouts.** `SideBySideSwapped` / `TopBottomSwapped` patterns still give red
   **L** to the left eye (the generator swaps the packing, the component swaps it back).
8. **Depth.** In both halves the square, the letter and the grid are at the same place.
   The yellow circle is further right in the right half; the green triangle further left.
9. **Mono fallback.** **Force Mono**: both halves show the red **L** picture. Same when
   the session switches to a 2D rendering mode (`DisplayXRSceneMode` set to 2D).
10. **Zero parallax, quad.** With **Depth Shift** 0 the white frame is at the same place in
    both halves, and stays so as the simulated head moves — including while the rig itself
    is moved by a script every frame (no one-frame lag between the frame and the view).
11. **Full screen.** The picture covers the view (`Fit`: with bars), and does not move at
    all as the head moves, while the orange cube does. `Background`: the cube is drawn over
    the picture. `Foreground`: the picture hides the cube.
12. **One camera only.** In Full Screen the Scene view is not covered by the picture, and
    neither is a second Game camera rendering into a `RenderTexture`.
13. **Show / hide.** Call `SetActive(false)` then `SetActive(true)` on the GameObject: the
    picture disappears and comes back, with no error in the console and no stray object in
    the hierarchy.
14. **Fit.** `Fit` shows the whole frame with bars, `Fill` crops, `Stretch` distorts; the
    shapes stay round / square in `Fit` and `Fill`.
15. **Seam.** No blue line on the right edge of the red picture, no red line on the left
    edge of the blue one. Repeat with **Create Scene Content**'s PNG re-imported with
    **Generate Mip Maps** on and the quad made small (**Placement = Manual**, a quad a
    quarter of the view): still no line.
16. **Non-power-of-two import.** Re-import that PNG with **Non-Power of 2** = *ToNearest*:
    the inspector warns, and **Set Source Size** restores round circles.
17. **Colour.** In a Linear and in a Gamma project the red and blue look the same, and
    match the PNG viewed in an image viewer. Then play a video into a non-sRGB render
    texture in a Linear project: not washed out.
18. **HDRP.** Expect the tonemapping, TAA and fog differences described under Limits;
    everything else as above.
