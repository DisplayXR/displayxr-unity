# Stereo Media Sample

Shows a side-by-side stereo picture so that each eye sees its own half, using the
`DisplayXRStereoMedia` component. No media file ships with the sample: the picture is
generated in code (`StereoMediaTestPattern`). Works on the Built-in, Universal and High
Definition pipelines.

## What's Included

- `StereoMediaSetup.cs` — builds the content when you press Play: a `DisplayXRStereoMedia`
  showing the test picture (or your video), and a small cube in front of the screen
- `StereoMediaTestPattern.cs` — the generated picture
- `Editor/StereoMediaSetupEditor.cs` — the **DisplayXR > Samples > Create Stereo Media Scene**
  menu item, and the **Create Scene Content** button

## Quick Start

1. Import this sample via Package Manager > DisplayXR > Samples > Stereo Media
2. **DisplayXR > Samples > Create Stereo Media Scene** — a new scene with a camera carrying
   a display-centric rig and a **Stereo Media** object
3. Enable **DisplayXR** in Project Settings > XR Plug-in Management and enter Play Mode

(Or add `StereoMediaSetup` to an object in a scene of your own. If the main camera has no
DisplayXR rig, the sample adds a display-centric one.)

## What You Should See

| | Left eye | Right eye |
|---|---|---|
| Background | red | blue |
| Letter (top left) | **L** | **R** |

If the left eye sees the blue **R** picture, the layout is swapped.

Three shapes, the same in both pictures except for where they are horizontally:

- **yellow circle** — behind the screen
- **white square**, the letter, the grid and the white frame — on the screen
- **green triangle** — in front of the screen

On the runtime's simulated side-by-side display (no 3D panel attached) the window shows
the two eye views next to each other: the red **L** picture in the left half, the blue
**R** picture in the right half. Each half must show ONE whole picture — a half that
itself shows a red part and a blue part means the eyes are not being separated.

While playing, select **Stereo Media** and change the `DisplayXRStereoMedia` settings live:

- **Presentation** — `Quad` puts the picture on the screen plane as a scene object: the
  orange cube overlaps it and shifts against it as you move your head. `Full Screen` sends
  each half straight to its eye: nothing moves with the head.
- **Full Screen > Order** — `Background` lets the cube draw over the picture, `Foreground`
  draws the picture over everything.
- **Fit** — `Fit` / `Fill` / `Stretch`
- **Depth Shift** — moves the whole picture behind (+) or in front of (−) the screen
- **Force Mono** — the left (red **L**) picture to both eyes

## Your Own Video

Set **Video Url** on `StereoMediaSetup` to a file path or URL of a stereo video, for
example `C:/Videos/clip_sbs.mp4`, and press Play. Then set:

- **Video Layout** — `Side By Side` (left eye in the left half) or `Top Bottom` (left eye on
  top); the `Swapped` variants if your file has the right eye first
- **Video Half Resolution** — on when each eye is squeezed to half size: a 1920×1080 or
  3840×2160 side-by-side file almost always is. Off for a file that is twice as wide (or
  tall) as one picture, such as 3840×1080. The inspector of `DisplayXRStereoMedia` warns
  when the shape does not look like the setting.

The sample adds a `VideoPlayer` in **API Only** mode and hands it to the component. In
your own scene you can instead assign any `VideoPlayer` to the component's **Video
Player** field; if it renders into a **Target Texture**, that texture is used. Which
codecs play is up to Unity's `VideoPlayer` on your platform. This needs Unity's built-in
**Video** module (enabled by default).

To use a stereo photo, import it as a texture (turn **Generate Mip Maps** off and set
**Non-Power of 2** to **None** to keep its exact pixels) and assign it to **Texture**.

## Keeping the Content

Save the scene, select **Stereo Media** and press **Create Scene Content**: the test
picture is written as a PNG next to the scene, a `DisplayXRStereoMedia` is added and
pointed at it, and the setup component is removed.

## Without Hardware

With no 3D panel the runtime falls back to its simulated display automatically; set
`SIM_DISPLAY_OUTPUT=sbs` to choose the side-by-side output described above.

More: `docs~/architecture/stereo-media.md` in the package.
