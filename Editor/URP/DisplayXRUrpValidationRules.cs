// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Project validation, the URP-only rules (#462). They live in the URP editor
// sub-assembly (compiled only with URP 17+, see its asmdef) and add themselves to the
// same registry the core rules use - so a Built-in or HDRP project never compiles them
// and never shows them.
//
// Both rules are about transparent-overlay apps, and both first look for evidence that
// the project IS one. A normal opaque URP app sees them as "not applicable".

using UnityEditor;

using Platforms = DisplayXR.Editor.DisplayXRValidationPlatforms;
using Result = DisplayXR.Editor.DisplayXRValidationResult;
using Severity = DisplayXR.Editor.DisplayXRValidationSeverity;

namespace DisplayXR.Editor.URP
{
    static class DisplayXRUrpValidationRules
    {
        [InitializeOnLoadMethod]
        static void Register()
        {
            // Source: the plugin cannot set the Player Setting at run time, and without
            // it URP's HDR + 32-bit colour target is B10G11R11 - no alpha channel - so a
            // transparent overlay renders opaque black (#127; Samples~/URPBasicScene/README.md
            // "Transparent overlays on URP").
            DisplayXRValidationRegistry.Register(new DisplayXRValidationRule
            {
                Id = "urp-preserve-framebuffer-alpha",
                Title = "URP transparent overlay: Preserve Framebuffer Alpha",
                Explanation =
                    "A transparent overlay needs an alpha channel in URP's colour target. With " +
                    "Preserve Framebuffer Alpha off, URP may pick a format without one (HDR with " +
                    "32-bit colour uses B10G11R11) and the overlay renders opaque black. The " +
                    "plug-in cannot set it at run time.",
                Severity = Severity.Error,
                Platforms = Platforms.Standalone,
                Order = 200,
                DocUrl = DisplayXRProjectValidation.DocUrlFor("urp-preserve-framebuffer-alpha"),
                Check = CheckPreserveAlpha,
                Fix = c => PlayerSettings.preserveFramebufferAlpha = true,
                FixDescription = "Turn on Preserve Framebuffer Alpha (Player Settings > Other Settings)",
            });

            // Source: DisplayXRCamera / DisplayXRDisplay publish the clip globals under URP
            // and nothing else ("inert if the pass isn't wired"); the pass is what
            // DisplayXR > Setup URP Foreground Clip adds (DisplayXRUrpInstaller).
            DisplayXRValidationRegistry.Register(new DisplayXRValidationRule
            {
                Id = "urp-foreground-clip",
                Title = "URP foreground clip pass wired",
                Explanation =
                    "On URP a rig's Foreground Only Clip is done by a Full Screen Pass renderer " +
                    "feature running the DisplayXR/ForegroundClipURP shader. The rig only publishes " +
                    "the clip planes; without the pass nothing is clipped, and nothing warns.",
                Severity = Severity.Warning,
                Order = 210,
                DocUrl = DisplayXRProjectValidation.DocUrlFor("urp-foreground-clip"),
                Check = CheckForegroundClip,
                Fix = c => DisplayXRUrpInstaller.InstallForegroundClip(interactive: false),
                FixDescription = "Create Assets/DisplayXR/DXRForegroundClip.mat and add a Full Screen " +
                                 "Pass Renderer Feature to each URP renderer asset in use that lacks " +
                                 "it (the same as DisplayXR > Setup URP Foreground Clip)",
            });
        }

        static Result CheckPreserveAlpha(DisplayXRValidationContext c)
        {
            Result off;
            if (DisplayXRProjectValidation.LoaderNotEnabled(c, out off)) return off;
            if (!DisplayXRProjectValidation.IsUrpActive())
                return Result.NotApplicable("URP is not the active render pipeline.");
            string evidence;
            if (!DisplayXRProjectValidation.UsesTransparentOverlay(out evidence))
                return Result.NotApplicable("No transparent overlay found (Manifest Settings, open scenes).");
            return PlayerSettings.preserveFramebufferAlpha
                ? Result.Pass()
                : Result.Fail("This looks like a transparent overlay app (" + evidence +
                              "), and Preserve Framebuffer Alpha is off.");
        }

        static Result CheckForegroundClip(DisplayXRValidationContext c)
        {
            Result off;
            if (DisplayXRProjectValidation.LoaderNotEnabled(c, out off)) return off;
            if (!DisplayXRProjectValidation.IsUrpActive())
                return Result.NotApplicable("URP is not the active render pipeline.");
            string rig = DisplayXRProjectValidation.FindRigWithForegroundClip();
            if (rig == null)
                return Result.NotApplicable("No rig in the open scenes has Foreground Only Clip ticked.");

            // The renderer assets in use - the active pipeline asset's and each quality
            // level's, at the renderer index the rig's camera selects - not the first
            // renderer asset a project search happens to return.
            bool guessed; int resolved;
            var missing = DisplayXRUrpInstaller.RenderersMissingClip(out guessed, out resolved);
            if (resolved == 0)
                return Result.Fail("'" + rig + "' has Foreground Only Clip ticked, but the project has " +
                                   "no URP renderer asset to carry the clip pass.", noFix: true);
            if (missing.Count == 0)
                return Result.Pass();

            var names = new System.Collections.Generic.List<string>();
            foreach (var data in missing) names.Add(data.name);
            return Result.Fail(
                "'" + rig + "' has Foreground Only Clip ticked, but the clip pass is not on: " +
                string.Join(", ", names.ToArray()) + "." +
                (guessed
                    ? " (No URP pipeline asset is assigned in Graphics or Quality settings, so this is " +
                      "only the first URP renderer asset found in the project.)"
                    : " These are the renderers the rig's camera uses under the active pipeline asset " +
                      "and the quality levels' pipeline assets."));
        }
    }
}
