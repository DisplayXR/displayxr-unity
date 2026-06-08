# SplashScreen sample

A DisplayXR-branded boot splash that renders the logo + "for Unity" subtitle on
the **zero-disparity plane** (the physical display surface) — razor-sharp, no
parallax — then loads your first scene.

**You don't need this sample to get the splash** — it's **on by default**. Every
DisplayXR app plays it at boot (and Unity's stock splash is turned off for
standalone builds) with no wiring. To opt out, tick **DisplayXR Manifest
Settings → Boot Splash → Disable DisplayXR Splash** (Window ▸ DisplayXR ▸
Manifest Settings).

Use this sample only when you want the splash as an **explicit scene** you
control — to customize the logo / timing / layout, or to place it at a specific
point in your scene flow rather than as the automatic boot overlay.

## What's in it

- `SplashScreen.unity` — one GameObject: a `Camera` + `DisplayXRDisplay` rig +
  `DisplayXRSplash`. The logo and subtitle artwork ship with the plugin
  (`Resources/displayxr_white`, `Resources/for_unity`) and load automatically.

## Use it

1. Opt out of the automatic boot splash (DisplayXR Manifest Settings ▸ Boot
   Splash ▸ Disable DisplayXR Splash) so you don't get both.
2. Import this sample (Package Manager ▸ DisplayXR ▸ Samples ▸ SplashScreen).
3. In **Build Settings**, put `SplashScreen` **first** (index 0) and your real
   first scene **second** (index 1).
4. Leave `DisplayXRSplash.Next Scene` empty to load the next build index
   (index 1), or set it to a specific scene name.

## Customize

On the `DisplayXRSplash` component:

- **Logo / Subtitle** — drop in your own white-on-transparent textures (leave
  empty to use the shipped DisplayXR artwork). Import them with **Non-Power-of-2
  = None** so their aspect isn't distorted.
- **Logo Height Fraction** — logo size as a fraction of display height.
- **Timing** — `Fade In` / `Hold` / `Fade Out` seconds.
- **Background** — clear color behind the artwork.
- **Overlay Mode** — leave **off** for this scene-based flow. (The auto-splash
  uses overlay mode internally to draw over an already-loaded scene.)

## Why it looks sharp

The `DisplayXRDisplay` rig's transform *is* the display surface, so the artwork
is placed at local z = 0 — exactly the zero-disparity plane — where both eyes
see it identically. The quads use oversized mesh bounds so Unity's frustum
culling (which uses the camera transform, sitting on that plane) never drops
them; the native Kooima eye projection then renders them at true zero disparity.
