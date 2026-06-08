// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace DisplayXR.Editor
{
    /// <summary>
    /// The DisplayXR boot splash is on by default, so for standalone builds we
    /// turn Unity's stock splash off (otherwise both would play). This is done
    /// transiently per build — saved in preprocess, restored in postprocess —
    /// so the developer's Player Settings are never persistently changed.
    ///
    /// Skipped when the project opts out via the DISPLAYXR_NO_SPLASH define
    /// (DisplayXR Manifest Settings ▸ Boot Splash ▸ Disable), which also
    /// compiles out the runtime spawner.
    /// </summary>
    public class DisplayXRSplashBuildProcessor
        : IPreprocessBuildWithReport, IPostprocessBuildWithReport
    {
        public int callbackOrder => 0;

        const string NoSplashDefine = "DISPLAYXR_NO_SPLASH";

        static bool s_disabledThisBuild;
        static bool s_savedShow;

        public void OnPreprocessBuild(BuildReport report)
        {
            s_disabledThisBuild = false;

            if (report.summary.platformGroup != BuildTargetGroup.Standalone)
                return;

            if (OptedOut(report.summary.platformGroup))
                return;

            if (!PlayerSettings.SplashScreen.show)
                return; // already off — nothing to do

            s_savedShow = PlayerSettings.SplashScreen.show;
            PlayerSettings.SplashScreen.show = false;
            s_disabledThisBuild = true;
            Debug.Log("[DisplayXR] Boot splash: disabled Unity's stock splash for this " +
                      "build (the DisplayXR splash plays instead). Opt out via DisplayXR " +
                      "Manifest Settings ▸ Boot Splash ▸ Disable.");
        }

        public void OnPostprocessBuild(BuildReport report)
        {
            if (!s_disabledThisBuild)
                return;
            PlayerSettings.SplashScreen.show = s_savedShow;
            s_disabledThisBuild = false;
        }

        static bool OptedOut(BuildTargetGroup group)
        {
            var nbt = NamedBuildTarget.FromBuildTargetGroup(group);
            var defines = PlayerSettings.GetScriptingDefineSymbols(nbt);
            foreach (var d in defines.Split(';'))
                if (d == NoSplashDefine) return true;
            return false;
        }
    }
}
