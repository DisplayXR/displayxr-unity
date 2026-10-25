// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.IO;
using UnityEditor.Android;
using UnityEngine;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Android (#418): makes the provider's Java helper survive R8 minify.
    ///
    /// <c>com.displayxr.unity.DisplayXRSurface</c> is reached only from native code
    /// (by name, through JNI), so a minified player strips it unless something keeps
    /// it. The keep rules live beside the class, in
    /// <c>Runtime/Plugins/Android/DisplayXR.androidlib/consumer-rules.pro</c>; this hook
    /// appends them to the generated <c>unityLibrary/proguard-unity.txt</c>, which that
    /// module already exports as its <c>consumerProguardFiles</c> — so they reach the
    /// launcher's R8 pass. Needed because Unity generates the Android Library's
    /// build.gradle without <c>consumerProguardFiles</c>, and R8 ignores
    /// <c>META-INF/proguard</c> rules from a Gradle subproject (both verified).
    ///
    /// Harmless when minify is off: the rules are then simply unused. Idempotent (a
    /// marker line guards against a second append into a reused Gradle project).
    /// </summary>
    public class DisplayXRAndroidProguardProcessor : IPostGenerateGradleAndroidProject
    {
        public int callbackOrder => 0;

        const string Marker = "# --- DisplayXR Unity provider keep rules (displayxr-unity#418) ---";

        // Used only if the androidlib copy is missing from the generated project.
        const string FallbackRules =
            "-keep class com.displayxr.unity.DisplayXRSurface { *; }\n" +
            "-keep class com.displayxr.unity.DisplayXRSurface$* { *; }\n";

        public void OnPostGenerateGradleAndroidProject(string path)
        {
            // `path` is the unityLibrary module.
            string target = Path.Combine(path, "proguard-unity.txt");
            if (!File.Exists(target))
            {
                Debug.LogWarning("[DisplayXR] proguard-unity.txt not found in the generated Gradle project (" +
                                 path + ") — the provider's R8 keep rules were not added. With Minify on, " +
                                 "add Runtime/Plugins/Android/DisplayXR.androidlib/consumer-rules.pro to " +
                                 "Assets/Plugins/Android/proguard-user.txt.");
                return;
            }
            string existing = File.ReadAllText(target);
            if (existing.Contains(Marker)) return;

            string src = Path.Combine(path, "DisplayXR.androidlib", "consumer-rules.pro");
            string rules = File.Exists(src) ? File.ReadAllText(src) : FallbackRules;
            File.AppendAllText(target, "\n" + Marker + "\n" + rules.TrimEnd() + "\n");
        }
    }
}
