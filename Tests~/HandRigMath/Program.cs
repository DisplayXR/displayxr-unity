// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DisplayXRHandRigMath - the joint -> rig-camera mapping of the XR Hands subsystem.
// Hand-rolled (no test framework) so it also runs from a bare `csc` + the .NET runtime that ships
// inside the Unity editor. Exit code 0 = all passed.
//
// The reference is the runtime's rule for rig-scoped hand joints (XR_DXR_view_rig spec 4, §2): a
// joint maps exactly as an eye at that point would. RuntimeEye() below re-implements the shared
// core's eye mapping (displayxr-common dxr_view_math.c: display3d `es * p`, es = persp * vH / canvasH;
// camera3d `m2v * (p - (0, 0, nominalZ))`) in OpenXR coordinates, independently of the code under test,
// which works in Unity's frame (z mirrored).

using System;
using DisplayXR.Hands;

static class Program
{
    static int s_pass, s_fail;

    static void Check(bool ok, string what)
    {
        if (ok) s_pass++;
        else { s_fail++; Console.WriteLine("FAIL " + what); }
    }

    static bool Near(float a, float b, float eps = 1e-5f) => Math.Abs(a - b) <= eps;

    // The code under test: an OpenXR display-plane point (+z toward the viewer) through the
    // provider's Unity mirror (z -> -z), then the mapping. Returns the rig-camera-local point in
    // Unity's frame (+z in front of the camera).
    static void Map(DisplayXRHandRigMapping m, float ox, float oy, float oz, out float x, out float y, out float z)
    {
        x = ox; y = oy; z = -oz;
        m.Apply(ref x, ref y, ref z);
    }

    // Reference, in OpenXR: canvas rebase, then the core's eye mapping; mirrored to Unity at the end.
    static void RuntimeEye(bool camera, DisplayXRHandRigMath.Canvas c, float vh, float persp, float m2v,
                           float nominalZ, float ox, float oy, float oz, out float x, out float y, out float z)
    {
        float px = ox - c.centerX, py = oy - c.centerY, pz = oz;
        if (camera)
        {
            x = m2v * px; y = m2v * py; z = m2v * (pz - nominalZ);
        }
        else
        {
            float es = persp * (vh / c.heightM);
            x = es * px; y = es * py; z = es * pz;
        }
        z = -z; // OpenXR -> Unity
    }

    static void Main()
    {
        const float H = 0.194f, W = 0.344f;   // a 15.6" 3D panel
        const uint PW = 3840, PH = 2160;
        const float N = 0.6f;

        // --- Canvas from the raw channel's panel rect ---------------------------------------------
        {
            Check(DisplayXRHandRigMath.TryCanvasFromPanelRect(0, 0, (int)PW, (int)PH, H, PW, PH, W, H, out var full),
                  "fullscreen canvas resolves");
            Check(Near(full.centerX, 0f) && Near(full.centerY, 0f) && Near(full.heightM, H), "fullscreen canvas is centred");

            // Top-left quadrant window: left of and ABOVE the panel centre (pixels run down, metres up).
            Check(DisplayXRHandRigMath.TryCanvasFromPanelRect(0, 0, (int)PW / 2, (int)PH / 2, H / 2, PW, PH, W, H, out var tl),
                  "quadrant canvas resolves");
            Check(Near(tl.centerX, -W / 4) && Near(tl.centerY, H / 4), $"top-left quadrant centre (-W/4, +H/4), got ({tl.centerX}, {tl.centerY})");

            Check(!DisplayXRHandRigMath.TryCanvasFromPanelRect(0, 0, 0, 10, H, PW, PH, W, H, out _), "zero-width rect rejected");
            Check(!DisplayXRHandRigMath.TryCanvasFromPanelRect(0, 0, 10, 10, 0f, PW, PH, W, H, out _), "zero canvas height rejected");
            Check(!DisplayXRHandRigMath.TryCanvasFromPanelRect(0, 0, 10, 10, H, 0, PH, W, H, out _), "zero panel pixels rejected");
        }

        var panel = DisplayXRHandRigMath.FullPanel(H);

        // --- Display-centric rig --------------------------------------------------------------
        {
            // Physical-size rig on a fullscreen canvas: identity (the hand is where it is).
            var m = DisplayXRHandRigMath.DisplayRig(panel, H, 1f);
            Map(m, 0.03f, -0.02f, 0.10f, out float x, out float y, out float z);
            Check(Near(x, 0.03f) && Near(y, -0.02f) && Near(z, -0.10f), "display rig, physical size: identity");

            // Arbitrary rig vs the runtime's core, on an off-centre window.
            DisplayXRHandRigMath.TryCanvasFromPanelRect(400, 300, 1920, 1080, H / 2, PW, PH, W, H, out var win);
            foreach (float vh in new[] { H, 0.5f, 2f })
            foreach (float persp in new[] { 1f, 0.7f })
            {
                var md = DisplayXRHandRigMath.DisplayRig(win, vh, persp);
                Map(md, 0.05f, 0.01f, 0.12f, out x, out y, out z);
                RuntimeEye(false, win, vh, persp, 1f, N, 0.05f, 0.01f, 0.12f, out float rx, out float ry, out float rz);
                Check(Near(x, rx) && Near(y, ry) && Near(z, rz), $"display rig vh={vh} persp={persp} matches the runtime core");
                Check(Near(md.scale, persp * vh / (H / 2)), "display rig scale = persp * vH / canvas height");
            }

            // A point on the glass at the canvas centre lands on the rig camera (the display plane).
            var mc = DisplayXRHandRigMath.DisplayRig(win, 0.5f, 1f);
            Map(mc, win.centerX, win.centerY, 0f, out x, out y, out z);
            Check(Near(x, 0f) && Near(y, 0f) && Near(z, 0f), "display rig: glass at the canvas centre -> rig origin");

            // Degenerate inputs never produce a zero/NaN scale.
            var bad = DisplayXRHandRigMath.DisplayRig(new DisplayXRHandRigMath.Canvas(), 0f, 0f);
            Check(Near(bad.scale, 1f), "display rig with no heights -> scale 1");
        }

        // --- Camera-centric rig -----------------------------------------------------------------
        {
            var m = DisplayXRHandRigMath.CameraRig(panel, N, 1f);

            // The partner report's case: a hand 10 cm in front of the glass. It used to come back
            // BEHIND the camera (z = -0.298 with the old convergence-plane scale); it must land in
            // front of it, at nominalZ - 0.10.
            Map(m, -0.05f, 0.02f, 0.10f, out float x, out float y, out float z);
            Check(z > 0f, $"camera rig: a hand in front of the panel is in front of the camera (z={z})");
            Check(Near(x, -0.05f) && Near(y, 0.02f) && Near(z, N - 0.10f), "camera rig: unscaled, offset by nominal z");

            // The glass lands nominalZ in front of the camera; the nominal viewer IS the camera.
            Map(m, 0f, 0f, 0f, out x, out y, out z);
            Check(Near(z, N), "camera rig: the glass is nominalZ in front of the camera");
            Map(m, 0f, 0f, N, out x, out y, out z);
            Check(Near(x, 0f) && Near(y, 0f) && Near(z, 0f), "camera rig: the nominal viewer maps to the camera");

            // Against the runtime's core, off-centre window, m2v != 1.
            DisplayXRHandRigMath.TryCanvasFromPanelRect(1200, 900, 1280, 720, H / 3, PW, PH, W, H, out var win);
            foreach (float m2v in new[] { 1f, 2.5f })
            {
                var mw = DisplayXRHandRigMath.CameraRig(win, N, m2v);
                Map(mw, 0.04f, -0.03f, 0.08f, out x, out y, out z);
                RuntimeEye(true, win, 0f, 1f, m2v, N, 0.04f, -0.03f, 0.08f, out float rx, out float ry, out float rz);
                Check(Near(x, rx) && Near(y, ry) && Near(z, rz), $"camera rig m2v={m2v} matches the runtime core");
            }

            // No nominal viewer in display info -> the shared 0.5 m fallback.
            var fb = DisplayXRHandRigMath.CameraRig(panel, 0f, 1f);
            Map(fb, 0f, 0f, 0f, out x, out y, out z);
            Check(Near(z, DisplayXRHandRigMath.NominalViewerZFallback), "camera rig: nominal z fallback");
            var fb2 = DisplayXRHandRigMath.CameraRig(panel, N, 0f);
            Check(Near(fb2.scale, 1f), "camera rig: metersToVirtual <= 0 -> 1");
        }

        // --- Camera-centric scene: the joints must be in the rig the views used ------------------
        {
            // Partner rerun of #446 (camera-centric scene, 2026-11-03): the views were located with
            // the provider's DEFAULT display rig while the boot splash held the app rig back, and the
            // hand locate chained the same. A wrist 0.145 m in front of the glass then comes back in
            // the display frame (OpenXR z = +0.145) - read as camera-relative it is BEHIND the camera.
            var disp = DisplayXRHandRigMath.DisplayRig(panel, H, 1f); // the default rig: vH = panel height
            Map(disp, -0.132f, -0.084f, 0.145f, out float dx, out float dy, out float dz);
            Check(dz < 0f, $"default display rig frame read under a camera rig: behind the camera (z={dz})");
            Check(!DisplayXRHandRigMath.JointRigFitsScene(DisplayXRHandRigKind.DefaultDisplay, DisplayXRHandRigKind.Camera),
                  "default-display-rig joints are held back under a camera rig");
            Check(!DisplayXRHandRigMath.JointRigFitsScene(DisplayXRHandRigKind.Display, DisplayXRHandRigKind.Camera),
                  "display-rig joints are held back under a camera rig");

            // Once the app's camera rig is the one chained, the same wrist lands in front of the camera,
            // nominalZ - 0.145 deep, exactly as the runtime maps it.
            Check(DisplayXRHandRigMath.JointRigFitsScene(DisplayXRHandRigKind.Camera, DisplayXRHandRigKind.Camera),
                  "camera-rig joints fit a camera rig");
            RuntimeEye(true, panel, 0f, 1f, 1f, N, -0.132f, -0.084f, 0.145f, out float cx, out float cy, out float cz);
            Check(cz > 0f && Near(cz, N - 0.145f), $"camera rig frame: the wrist is in front of the camera (z={cz})");
            var cam = DisplayXRHandRigMath.CameraRig(panel, N, 1f);
            Map(cam, -0.132f, -0.084f, 0.145f, out float fx, out float fy, out float fz);
            Check(Near(fx, cx) && Near(fy, cy) && Near(fz, cz), "C# fallback camera rig == runtime camera rig frame");

            // The other pairings.
            Check(DisplayXRHandRigMath.JointRigFitsScene(DisplayXRHandRigKind.Display, DisplayXRHandRigKind.Display),
                  "display-rig joints fit a display rig");
            Check(!DisplayXRHandRigMath.JointRigFitsScene(DisplayXRHandRigKind.Camera, DisplayXRHandRigKind.Display),
                  "camera-rig joints are held back under a display rig");
            Check(!DisplayXRHandRigMath.JointRigFitsScene(DisplayXRHandRigKind.DefaultDisplay, DisplayXRHandRigKind.Display),
                  "default-rig joints are held back under the app's display rig (wrong scale)");
            Check(DisplayXRHandRigMath.JointRigFitsScene(DisplayXRHandRigKind.DefaultDisplay, DisplayXRHandRigKind.None),
                  "default-rig joints fit a rig-less camera");
            Check(DisplayXRHandRigMath.JointRigFitsScene(DisplayXRHandRigKind.None, DisplayXRHandRigKind.Camera),
                  "display-plane metres always fit (C# maps them)");
        }

        // --- Identity ---------------------------------------------------------------------------
        {
            var id = DisplayXRHandRigMapping.Identity;
            Map(id, 0.1f, 0.2f, 0.3f, out float x, out float y, out float z);
            Check(Near(x, 0.1f) && Near(y, 0.2f) && Near(z, -0.3f), "identity mapping");
        }

        Console.WriteLine($"HandRigMath: {s_pass} passed, {s_fail} failed");
        Environment.Exit(s_fail == 0 ? 0 : 1);
    }
}
