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

The component draws through a hidden child object it owns, so it never moves or scales the
GameObject it sits on, and it allocates nothing per frame.

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
16:9 pictures). The inspector warns when the texture's shape does not look like the
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

A camera-centric rig that converges at infinity (`invConvergenceDistance = 0`) has no such
plane; the component then logs once and falls back to its own transform and **Quad Size**,
as **Placement = Manual** does.

The quad is still a scene object: other objects can be in front of or behind it, and it
shifts against them as the viewer's head moves. That is the difference from full screen.

### Full screen

Each eye's half goes straight to that eye's render target: the vertex shader writes clip
space directly, with no view and no projection matrix. So the rig adds no parallax, and
nothing moves with the head. **Order** decides when it is drawn:

- `Background` — after the opaque queue and the skybox, before transparents, on the far
  plane with a depth test. It replaces the skybox / clear colour, and scene geometry draws
  over it.
- `Foreground` — after transparents, ignoring depth.

It is drawn for Game cameras only (never the Scene view, previews or reflection probes),
and only for **Rig Camera** when one is assigned.

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

So both presentations use the same mechanism on every pipeline: a mesh renderer with this
material, drawn in the pipeline's normal scene pass. No `OnRenderImage`, renderer feature
or custom pass is involved, and nothing has to be set up in the project.

| | Quad | Full screen |
|---|---|---|
| **BiRP** (multi-pass) | yes | yes |
| **URP** (single-pass instanced) | yes | yes |
| **HDRP** (single-pass instanced) | yes, but see the HDRP limits below | yes, but see the HDRP limits below |

### Where the eye index comes from

The shader reads `unity_StereoEyeIndex` in the **vertex** stage, where it is defined on
every path, and passes the chosen eye's UV mapping to the fragment stage as interpolants.
The fragment stage needs no eye index at all.

- **Single-pass instanced (URP, HDRP; D3D11, D3D12, Metal, Vulkan).** The engine doubles
  the instance count; `UNITY_SETUP_INSTANCE_ID` sets `unity_StereoEyeIndex = instanceID & 1`,
  and `UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO` routes the instance to array slice 0 or 1.
- **Multi-pass (BiRP).** `unity_StereoEyeIndex` is a per-camera constant the engine sets
  for each eye's pass.
- **No stereo.** The same constant is 0: the left picture.

A view index above 1 (a future mode with more than two views) gets the right picture.

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

Sampling is clamped half a texel inside the eye's own half, so bilinear filtering never
blends the two pictures at the seam.

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
The decision is `DisplayXRStereoMediaMath.ColorTransform`, from `Texture.isDataSRGB` and
`QualitySettings.activeColorSpace`; the conversions use the exact piecewise sRGB curve.

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

- **HDRP colour.** The picture is drawn in HDRP's scene pass, before post-processing, so
  tonemapping and any other post effect apply to it, and its values are taken as already
  exposed. For faithful colour use a volume without tonemapping on that camera. An
  after-post-process custom pass would avoid this; it is not shipped, because it could not
  be verified on hardware when this was written.
- **Post-processing in general.** The same applies to URP post-processing and to the rig's
  FXAA (**Post-Process AA**), which softens the picture slightly; turn it off for a
  pixel-exact viewer.
- **Foreground-only clip.** A rig with `foregroundOnlyClip` on (transparent-overlay apps)
  clips everything at or behind the display plane, which is exactly where a
  zero-parallax quad and a full-screen picture are. Do not combine them.
- **Forced multi-pass on URP / HDRP** (`DISPLAYXR_FORCE_MULTIPASS=1`, a development
  override) is not supported. Nothing in either pipeline's source sets a per-eye index
  for built-in shaders on that path, so expect the left picture in both eyes. Untested.
- **Windowed display-centric rigs with `virtualDisplayHeight = 0`.** The plane height is
  taken as the physical display height, which is right for a full-screen window. In a
  smaller window set `virtualDisplayHeight`, or use **Placement = Manual**.
- **A scaled rig.** The plane follows the rig's gizmo: defined in the rig's local units
  and scaled by its transform. Only scale 1 has been reasoned through end to end.
- **Display zones** (`XR_DXR_display_zones`). Full screen fills every 3D zone's view.
- **More than two views.** Only two pictures exist; views after the first get the right
  one.
- **Batching.** The shader is not SRP Batcher compatible (it is one draw).

## Verification

The math is covered by `Tests~/StereoMediaMath` (`dotnet run --project Tests~/StereoMediaMath -c Release`,
run in CI): every layout × eye, the swapped variants, half resolution, the fit modes, the
depth shift, flipped textures, the seam clamp and the colour decision.

The shader cannot be tested in CI. Check these by eye with the **Stereo Media** sample on
the runtime's simulated side-by-side display (`SIM_DISPLAY_OUTPUT=sbs`, no 3D panel), where
the window shows the left eye's view in its left half and the right eye's in its right
half. The sample's left picture is red with an **L**, its right picture blue with an **R**.
Repeat on BiRP, URP and HDRP, and per graphics API in use.

1. **Eye separation.** Left half of the window: the red **L** picture, whole. Right half:
   the blue **R** picture, whole. A half that shows both a red and a blue part means the
   eye index is not reaching the shader.
2. **Not swapped.** Red **L** left, blue **R** right — not the other way round.
3. **Upright.** The letters are at the top left and read correctly; the white frame runs
   round all four edges. Check Quad and Full Screen separately (full screen has its own
   up/down rule).
4. **Top-bottom.** Set the sample's **Pattern Layout** to `TopBottom`: same result as 1–3.
5. **Swapped layouts.** `SideBySideSwapped` / `TopBottomSwapped` patterns still give red
   **L** to the left eye (the generator swaps the packing, the component swaps it back).
6. **Depth.** In both halves the square, the letter and the grid are at the same place.
   The yellow circle is further right in the right half; the green triangle further left.
7. **Mono fallback.** **Force Mono**: both halves show the red **L** picture. Same when
   the session switches to a 2D rendering mode (`DisplayXRSceneMode` set to 2D).
8. **Zero parallax, quad.** With **Depth Shift** 0 the white frame is at the same place in
   both halves, and stays so as the simulated head moves.
9. **Full screen.** The picture covers the view (`Fit`: with bars), and does not move at
   all as the head moves, while the orange cube does. `Background`: the cube is drawn over
   the picture. `Foreground`: the picture hides the cube.
10. **Fit.** `Fit` shows the whole frame with bars, `Fill` crops, `Stretch` distorts; the
    shapes stay round / square in `Fit` and `Fill`.
11. **Seam.** No blue line on the right edge of the red picture, no red line on the left
    edge of the blue one.
12. **Colour.** In a Linear and in a Gamma project the red and blue look the same, and
    match the PNG that **Create Scene Content** writes, viewed in an image viewer. Then
    play a video into a non-sRGB render texture in a Linear project: not washed out.
13. **Scene view.** In Full Screen the Scene view is not covered by the picture.
14. **HDRP.** Expect the tonemapping difference described under Limits; everything else as
    above.
