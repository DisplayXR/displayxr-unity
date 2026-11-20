// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Globalization;

namespace DisplayXR
{
    /// <summary>
    /// Pure rules of the render scale (<see cref="DisplayXRProvider.RenderScale"/>, #475):
    /// what counts as "set", the allowed range, which of the app's value and the settings
    /// asset's value wins, and the one-line file a build writes so a built player knows the
    /// settings asset's value. No UnityEngine types on purpose: it is unit-tested outside
    /// Unity (<c>Tests~/RenderScale</c>, run in CI).
    ///
    /// <para>The native side (<c>displayxr_provider_render_scale.cpp</c>) applies the same
    /// range and rounding to whatever reaches it, so a value that went through
    /// <see cref="Sanitize"/> is taken as is.</para>
    /// </summary>
    public static class DisplayXRRenderScaleMath
    {
        /// <summary>Smallest render scale: a quarter of the base resolution per axis.</summary>
        public const float Min = 0.25f;

        /// <summary>Largest render scale: twice the base resolution per axis (4x the pixels).</summary>
        public const float Max = 2f;

        /// <summary>The value that means "not set": Unity's own hint decides.</summary>
        public const float Unset = 0f;

        /// <summary>The player-data file a build writes (relative to the player's data folder).</summary>
        public const string PlayerSettingsFile = "DisplayXR/player_settings.json";

        const string k_Key = "render_scale";

        /// <summary>
        /// A raw value as a request: NaN, infinity and anything not above zero are
        /// <see cref="Unset"/>; the rest is rounded to 1/100 and clamped to
        /// [<see cref="Min"/>, <see cref="Max"/>].
        /// </summary>
        public static float Sanitize(float value)
        {
            if (float.IsNaN(value) || float.IsInfinity(value) || !(value > 0f)) return Unset;
            float v = (float)(Math.Floor(value * 100.0 + 0.5) / 100.0);
            if (v < Min) v = Min;
            if (v > Max) v = Max;
            return v;
        }

        /// <summary>True when <paramref name="value"/> is a request at all.</summary>
        public static bool IsSet(float value) => Sanitize(value) > 0f;

        /// <summary>
        /// The value pushed to the native provider: the app's (set from code) when it set
        /// one, else the settings asset's, else <see cref="Unset"/> - which leaves the
        /// decision to Unity's hint (<c>XRSettings.eyeTextureResolutionScale</c>, the URP
        /// asset's Render Scale).
        /// </summary>
        public static float ResolveOverride(float appValue, float settingsValue)
        {
            float app = Sanitize(appValue);
            return app > 0f ? app : Sanitize(settingsValue);
        }

        /// <summary>
        /// The content of <see cref="PlayerSettingsFile"/> for a settings value. Written with
        /// the invariant culture: a build machine with a decimal comma must not produce a
        /// file a player elsewhere cannot read.
        /// </summary>
        public static string ToPlayerSettingsJson(float settingsValue)
        {
            float v = Sanitize(settingsValue);
            return "{\n  \"" + k_Key + "\": " + v.ToString("0.##", CultureInfo.InvariantCulture) + "\n}\n";
        }

        /// <summary>
        /// Read the settings value back from <see cref="PlayerSettingsFile"/>'s content.
        /// False (and <see cref="Unset"/>) when the text is missing, has no such key, or the
        /// number does not parse. A present, valid 0 is true with <see cref="Unset"/>.
        /// </summary>
        public static bool TryParsePlayerSettingsJson(string json, out float settingsValue)
        {
            settingsValue = Unset;
            if (string.IsNullOrEmpty(json)) return false;
            int k = json.IndexOf("\"" + k_Key + "\"", StringComparison.Ordinal);
            if (k < 0) return false;
            int colon = json.IndexOf(':', k + k_Key.Length + 2);
            if (colon < 0) return false;
            int start = colon + 1;
            while (start < json.Length && char.IsWhiteSpace(json[start])) start++;
            int end = start;
            while (end < json.Length && "+-0123456789.eE".IndexOf(json[end]) >= 0) end++;
            if (end == start) return false;
            if (!float.TryParse(json.Substring(start, end - start), NumberStyles.Float,
                                CultureInfo.InvariantCulture, out float raw))
                return false;
            if (float.IsNaN(raw) || float.IsInfinity(raw) || raw < 0f) return false;
            settingsValue = Sanitize(raw);
            return true;
        }
    }
}
