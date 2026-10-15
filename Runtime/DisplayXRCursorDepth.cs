// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using UnityEngine;
#if HAS_INPUT_SYSTEM && ENABLE_INPUT_SYSTEM
using UnityEngine.InputSystem;
#endif

namespace DisplayXR
{
    /// <summary>
    /// The DisplayXR <b>depth-aware cursor</b> (<c>XR_DXR_cursor_depth</c>, runtime ADR-046).
    ///
    /// <para>
    /// <b>The problem.</b> The OS cursor is flat: it sits on the glass, at zero disparity.
    /// Hover it over content that pops OUT of the display and the cursor is drawn on top of
    /// pixels whose stereo depth says they are in front of it - the eyes get two contradicting
    /// depth cues. The fix is a cursor drawn in stereo, just in front of the nearest content
    /// under it.
    /// </para>
    /// <para>
    /// <b>Who does what.</b> The DisplayXR <i>runtime</i> draws that cursor (it looks like the
    /// user's own OS cursor, at its usual size) and owns all of its behaviour: never behind
    /// the glass, a small margin in front of the content, smooth rise/sink, and back to the
    /// plain OS cursor whenever the content under it is on or behind the glass. The plug-in
    /// sends the runtime the pointer and the depth under it every frame, and hides the OS
    /// cursor exactly while the runtime reports it is drawing its own - so there is never a
    /// second cursor and never no cursor.
    /// </para>
    /// <para>
    /// <b>Opt-in, zero cost otherwise.</b> Nothing happens until <see cref="Enabled"/> is set
    /// (or a <see cref="DisplayXRDepthCursor"/> component is enabled in the scene). An older
    /// runtime simply keeps the plain OS cursor; see <see cref="Supported"/>.
    /// </para>
    /// <para>
    /// <b>Depth sources</b> (<see cref="Source"/>):
    /// <list type="bullet">
    /// <item><see cref="DepthSource.Auto"/> (default): Unity's own scene depth when the render
    /// pipeline exposes it, else the runtime's measured disparity.</item>
    /// <item><see cref="DepthSource.Depth"/>: Unity's scene depth only. A small patch of the
    /// camera depth texture around the cursor is read back asynchronously (never stalls) and
    /// unprojected. Exact on any surface, textured or not. URP: the plug-in turns on the
    /// camera's Depth Texture while the cursor is enabled. HDRP: always available. BiRP:
    /// deferred, or forward when the camera already renders a depth texture. Runtime spec v5,
    /// and a DisplayXR rig.</item>
    /// <item><see cref="DepthSource.Measured"/>: the runtime block-matches the two rendered
    /// views. No pipeline support needed; low-texture areas (sky, a flat face) read as "on the
    /// glass". Runtime spec v7.</item>
    /// <item><see cref="DepthSource.Physics"/>: raycast the cursor footprint against 3D physics
    /// colliders (or <see cref="CustomHitTest"/>). Runtime spec v5, and a DisplayXR rig.</item>
    /// </list>
    /// <see cref="SetHitPoint"/> / <see cref="ClearHitPoint"/> override any source for the frame
    /// they are called in.
    /// </para>
    /// </summary>
    public static class DisplayXRCursorDepth
    {
        /// <summary>Where the depth under the cursor comes from.</summary>
        public enum DepthSource
        {
            /// <summary>Scene depth when the pipeline exposes it, else measured disparity.</summary>
            Auto = 0,
            /// <summary>Unity's scene depth under the cursor (runtime spec v5+).</summary>
            Depth = 1,
            /// <summary>The runtime block-matches the rendered views (runtime spec v7+).</summary>
            Measured = 2,
            /// <summary>Raycast the cursor footprint against physics colliders (runtime spec v5+).</summary>
            Physics = 3,
        }

        /// <summary>What actually supplied the depth on the last frame.</summary>
        public enum ActiveDepthSource
        {
            /// <summary>Nothing requested (off, unsupported, pointer off the canvas).</summary>
            None,
            /// <summary>Unity's scene depth.</summary>
            Depth,
            /// <summary>The runtime's measured disparity.</summary>
            Measured,
            /// <summary>The physics raycast / custom hit test.</summary>
            Physics,
            /// <summary><see cref="SetHitPoint"/> / <see cref="ClearHitPoint"/>.</summary>
            Manual,
            /// <summary>The source could not answer this frame: the cursor stays on the glass.</summary>
            NoDepth,
        }

        /// <summary>What the cursor is doing right now.</summary>
        public enum CursorState
        {
            /// <summary><see cref="Enabled"/> is false: the plain OS cursor, nothing requested.</summary>
            Off,
            /// <summary>Enabled, but the runtime is too old for the chosen source (or the
            /// session is not running): the plain OS cursor.</summary>
            Unsupported,
            /// <summary>Enabled and supported, but nothing was requested this frame (pointer off
            /// the 3D canvas, cursor locked, no mouse).</summary>
            Idle,
            /// <summary>Requested; the content under the cursor is on or behind the glass, so the
            /// OS cursor IS the cursor.</summary>
            OnGlass,
            /// <summary>The runtime draws the stereo cursor in front of the glass; the OS cursor
            /// is hidden.</summary>
            Lifted,
        }

        /// <summary>
        /// Custom hit test for <see cref="DepthSource.Physics"/>: return true and the WORLD-space
        /// hit for one footprint ray, false for a miss. Called up to 20 times a frame.
        /// </summary>
        public delegate bool HitTest(Ray ray, out Vector3 hitPoint);

        /// <summary>Turn the depth-aware cursor on. Default off.</summary>
        public static bool Enabled { get; set; }

        /// <summary>Where the depth comes from. Default <see cref="DepthSource.Auto"/>.</summary>
        public static DepthSource Source = DepthSource.Auto;

        /// <summary>
        /// How far in front of the content the cursor floats, in eye baselines (about 65 mm
        /// on the glass per 1.0). 0 = the runtime default (0.015, about 1 mm). Leave it at 0
        /// unless you have a reason: every DisplayXR app shares the default.
        /// </summary>
        public static float Margin = 0f;

        /// <summary>
        /// Hide the OS cursor (<c>Cursor.visible</c>) while the runtime draws its own. Default
        /// true. Set false to manage it yourself from <see cref="DepthCursorShownChanged"/>.
        /// (Over the plug-in's own presentation window the native side always hides it - that
        /// window sets its cursor itself, so <c>Cursor.visible</c> cannot reach it.)
        /// </summary>
        public static bool ManageOsCursor = true;

        /// <summary>Layers the <see cref="DepthSource.Physics"/> raycast considers. Default: every
        /// layer except Ignore Raycast (= <c>Physics.DefaultRaycastLayers</c>, spelled out so this
        /// file compiles in a project that disabled the built-in Physics module).</summary>
        public static int RaycastLayerMask = ~(1 << 2);

        /// <summary>Maximum physics raycast distance, world units.</summary>
        public static float RaycastMaxDistance = 1000f;

        /// <summary>Custom hit test for <see cref="DepthSource.Physics"/> (replaces the physics
        /// raycast).</summary>
        public static HitTest CustomHitTest;

        /// <summary>Fired when the runtime starts (true) or stops (false) drawing the cursor.</summary>
        public static event Action<bool> DepthCursorShownChanged;

        /// <summary>The <c>XR_DXR_cursor_depth</c> version the runtime advertised; 0 = none
        /// (or below v5, which this plug-in cannot use).</summary>
        public static uint RuntimeSpecVersion { get; private set; }

        /// <summary>True when the running runtime supports the selected <see cref="Source"/>.
        /// (Auto needs v5; with v5/v6 it has no measured fallback.)</summary>
        public static bool Supported =>
            RuntimeSpecVersion >= (Source == DepthSource.Measured ? 7u : 5u);

        /// <summary>True while the runtime draws the stereo cursor (OS cursor hidden).</summary>
        public static bool WillDraw { get; private set; }

        /// <summary>Filtered cursor disparity in eye baselines (0 = on the glass, &lt; 0 = in
        /// front). Diagnostic.</summary>
        public static float Disparity { get; private set; }

        /// <summary>See <see cref="CursorState"/>.</summary>
        public static CursorState State { get; private set; }

        /// <summary>What supplied the depth on the last frame (Auto shows which branch it took).</summary>
        public static ActiveDepthSource ActiveSource { get; private set; }

        /// <summary>Log one line per <see cref="State"/> / Auto-branch change. Default true.</summary>
        public static bool LogStateChanges = true;

        /// <summary>
        /// The nearest content point under the cursor this frame, in WORLD space. Overrides the
        /// selected <see cref="Source"/> for this frame only; call every frame from <c>Update</c>.
        /// Use the rays from <see cref="GetFootprintRays"/> so a nearer edge next to the
        /// hotspot still lifts the cursor. Needs runtime spec v5 and a DisplayXR rig.
        /// </summary>
        public static void SetHitPoint(Vector3 worldPoint)
        {
            s_ExplicitFrame = Time.frameCount;
            s_ExplicitHas = true;
            s_ExplicitPoint = worldPoint;
        }

        /// <summary>Nothing of the app's content is under the cursor this frame (overrides the
        /// selected source for this frame only).</summary>
        public static void ClearHitPoint()
        {
            s_ExplicitFrame = Time.frameCount;
            s_ExplicitHas = false;
        }

        /// <summary>
        /// The cursor FOOTPRINT rays in world space: from both eyes through the hotspot and a
        /// ring around the cursor body, built from the previous frame's views (the runtime's
        /// smoothing absorbs the one-frame lag). Returns the ray count written (0..20); 0 when
        /// the pointer is off the 3D canvas, the display is in 2D, or no DisplayXR rig is active.
        /// </summary>
        public static int GetFootprintRays(Ray[] buffer)
        {
            if (buffer == null || buffer.Length == 0) return 0;
            int n = FetchRays();
            if (n > buffer.Length) n = buffer.Length;
            for (int i = 0; i < n; i++) buffer[i] = s_Rays[i];
            return n;
        }

        // ---- Custom cursor sprite (#369) -------------------------------------------------

        /// <summary>
        /// Pass as <c>heightMeters</c> to <see cref="SetSprite"/> to draw the sprite at its
        /// texture's own pixel height on the panel (texture rows x the panel's pixel pitch) -
        /// about what a hardware cursor made from the same texture measures. This is what
        /// <see cref="SetCursor"/> uses.
        /// </summary>
        public const float SpriteHeightTexturePixels = -1f;

        /// <summary>
        /// Draw the lifted cursor with your own sprite instead of the default look (a copy of
        /// the OS cursor on Windows/macOS, the theme cursor on Linux X11, an arrow otherwise).
        /// Needs runtime spec v5, like the rest of the feature; an older runtime keeps the
        /// plain OS cursor. The sprite only shows where the cursor is lifted - on the glass
        /// the OS cursor is the cursor, so set your hardware cursor too (see
        /// <see cref="SetCursor"/>).
        /// <para>
        /// The texture is copied to the GPU once per call (any format, mips, compression or
        /// read/write setting; up to 1024 px a side), so call again after you change its
        /// pixels. Calling every frame with different textures animates the cursor at the
        /// cost of one small copy per frame. Alpha is <b>straight</b> (not premultiplied), as
        /// Unity textures are.
        /// </para>
        /// </summary>
        /// <param name="texture">The sprite. null = <see cref="ClearSprite"/>.</param>
        /// <param name="hotspotPx">The click point, in texture pixels from the TOP-left corner -
        /// the same convention as <c>Cursor.SetCursor</c>.</param>
        /// <param name="heightMeters">Height on the panel at the display plane, metres (the
        /// runtime keeps the apparent size constant as it lifts; the width follows the
        /// texture's aspect). 0 = the runtime's default (the OS cursor's size, 19 points);
        /// <see cref="SpriteHeightTexturePixels"/> = the texture's pixel height.</param>
        public static void SetSprite(Texture2D texture, Vector2 hotspotPx, float heightMeters = 0f)
        {
            if (texture == null) { ClearSprite(); return; }
            if (!DisplayXRCursorDepthMath.SpriteSizeOk(texture.width, texture.height))
            {
                Debug.LogWarning($"[DisplayXR] Depth cursor sprite '{texture.name}' is {texture.width}x{texture.height}; " +
                                 $"edges must be 1..{DisplayXRCursorDepthMath.SpriteMaxEdge} px. Keeping the previous look.");
                return;
            }
            s_SpriteTex = texture;
            s_SpriteHotspotPx = hotspotPx;
            s_SpriteHeight = heightMeters;
            s_SpriteGen++;
        }

        /// <summary>Back to the default cursor look. The sprite's GPU copy is freed a few
        /// frames later.</summary>
        public static void ClearSprite()
        {
            if (s_SpriteTex == null && !s_SpritePushed) return;
            s_SpriteTex = null;
            s_SpriteGen++;
        }

        /// <summary>
        /// <c>UnityEngine.Cursor.SetCursor</c> that ALSO makes <paramref name="texture"/> the
        /// lifted cursor's sprite (sized at its pixel height), so the cursor keeps its look
        /// whether it is on the glass (the hardware/software cursor) or lifted (the runtime's
        /// stereo sprite). Use it in place of <c>Cursor.SetCursor</c>: Unity has no getter
        /// for the current cursor texture, so a plain <c>Cursor.SetCursor</c> call cannot be
        /// mirrored. null restores both to the default.
        /// <para>
        /// On Windows and macOS the runtime's default look already copies the live OS cursor,
        /// a hardware <c>CursorMode.Auto</c> cursor included, so there this mainly helps with
        /// <c>CursorMode.ForceSoftware</c> (which the OS never sees). On Linux the runtime
        /// draws the theme cursor, so a custom cursor needs this (or <see cref="SetSprite"/>).
        /// </para>
        /// </summary>
        public static void SetCursor(Texture2D texture, Vector2 hotspot, CursorMode cursorMode)
        {
            Cursor.SetCursor(texture, hotspot, cursorMode);
            if (texture == null) ClearSprite();
            else SetSprite(texture, hotspot, SpriteHeightTexturePixels);
        }

        /// <summary>The sprite set with <see cref="SetSprite"/>; null = the default look.</summary>
        public static Texture2D Sprite => s_SpriteTex;

        /// <summary>The sprite's hotspot, texture pixels from the top-left.</summary>
        public static Vector2 SpriteHotspot => s_SpriteHotspotPx;

        /// <summary>The sprite's requested height (see <see cref="SetSprite"/>).</summary>
        public static float SpriteHeightMeters => s_SpriteHeight;

        /// <summary>True while the runtime has the sprite (it was named on the last frame's
        /// draw request). False while it is still being uploaded, without a sprite, or when the
        /// runtime cannot take one.</summary>
        public static bool SpriteActive { get; private set; }

        static Texture2D s_SpriteTex;
        static Vector2 s_SpriteHotspotPx;
        static float s_SpriteHeight;
        static uint s_SpriteGen;          // bumped per SetSprite / ClearSprite
        static uint s_SpritePushedGen;    // last generation pushed to native
        static bool s_SpritePushed;       // native holds a sprite
        static uint s_SpriteBlittedGen;   // last generation copied into the RT
        static uint s_SpriteFilledGen;    // last generation copied into the native target
        static IntPtr s_SpriteFilledTarget;
        static RenderTexture s_SpriteRT;
        static bool s_SpriteRTsRGB;
        static Texture2D s_SpriteBridge;  // D3D: wrapper of the native copy target
        static IntPtr s_SpriteBridgePtr;
        static IntPtr s_RenderEventFunc;
        static bool s_SpriteEventMissingLogged;

        static void ReleaseSpriteBridge()
        {
            if (s_SpriteBridge != null) UnityEngine.Object.Destroy(s_SpriteBridge);
            s_SpriteBridge = null;
            s_SpriteBridgePtr = IntPtr.Zero;
        }

        static void PushSpriteCleared()
        {
            DisplayXRProviderNative.dxr_prov_cursor_set_sprite(IntPtr.Zero, 0, 0, 0, 0f, 0f, 0f, s_SpriteGen);
            s_SpritePushed = false;
            s_SpritePushedGen = s_SpriteGen;
            SpriteActive = false;
            ReleaseSpriteBridge();
            if (s_SpriteRT != null)
            {
                s_SpriteRT.Release();
                UnityEngine.Object.Destroy(s_SpriteRT);
                s_SpriteRT = null;
            }
        }

        /// <summary>
        /// Per frame while the cursor is enabled: copy a changed sprite to the GPU, hand it to
        /// the provider, and fill the provider's copy target when it asks. Nothing at all
        /// without a sprite. See the native block comment above ps_cursor_sprite_submit.
        /// </summary>
        static void TickSprite()
        {
            Texture2D tex = s_SpriteTex;
            if (tex == null) // cleared, or the texture was destroyed
            {
                if (s_SpritePushed || s_SpritePushedGen != s_SpriteGen) PushSpriteCleared();
                return;
            }
            int w = tex.width, h = tex.height;
            // The bytes are sRGB-encoded colour for an sRGB texture, and for ANY texture in a
            // Gamma-space project (Unity never decodes there); linear values otherwise.
            bool srgb = QualitySettings.activeColorSpace == ColorSpace.Gamma || tex.isDataSRGB;

            // 1. Normalise into an RGBA8 RT: any format / mips / compression / readability.
            //    Y-flipped, so the rows land top-down, which is how the runtime samples the
            //    swapchain (Unity keeps texture rows bottom-up on every API).
            if (s_SpriteRT == null || s_SpriteRT.width != w || s_SpriteRT.height != h || s_SpriteRTsRGB != srgb)
            {
                if (s_SpriteRT != null) { s_SpriteRT.Release(); UnityEngine.Object.Destroy(s_SpriteRT); }
                s_SpriteRT = new RenderTexture(w, h, 0, RenderTextureFormat.ARGB32,
                                               srgb ? RenderTextureReadWrite.sRGB : RenderTextureReadWrite.Linear)
                {
                    name = "DisplayXR_CursorSprite",
                    useMipMap = false,
                    filterMode = FilterMode.Point,
                    hideFlags = HideFlags.HideAndDontSave,
                };
                s_SpriteRT.Create();
                s_SpriteRTsRGB = srgb;
                s_SpriteBlittedGen = 0;
            }
            if (s_SpriteBlittedGen != s_SpriteGen)
            {
                Graphics.Blit(tex, s_SpriteRT, new Vector2(1f, -1f), new Vector2(0f, 1f));
                s_SpriteBlittedGen = s_SpriteGen;
            }

            // 2. The configuration, on change.
            if (!s_SpritePushed || s_SpritePushedGen != s_SpriteGen)
            {
                DisplayXRCursorDepthMath.SpriteHotspotUV(s_SpriteHotspotPx.x, s_SpriteHotspotPx.y, w, h,
                                                         out float hu, out float hv);
                DisplayXRProviderNative.dxr_prov_cursor_set_sprite(s_SpriteRT.GetNativeTexturePtr(), (uint)w, (uint)h,
                                                                   srgb ? 1 : 0, hu, hv, s_SpriteHeight, s_SpriteGen);
                s_SpritePushed = true;
                s_SpritePushedGen = s_SpriteGen;
            }

            // 3. Fill the provider's copy target when the sprite changed, the target is new
            //    (first use, a size change, a session restart) or the provider asks (Vulkan).
            IntPtr target = DisplayXRProviderNative.dxr_prov_cursor_sprite_get_target(out uint tw, out uint th);
            if (target == IntPtr.Zero) { SpriteActive = false; return; } // not created yet, or no session
            bool need = s_SpriteFilledGen != s_SpriteGen || target != s_SpriteFilledTarget ||
                        DisplayXRProviderNative.dxr_prov_cursor_sprite_needs_fill() != 0;
            if (need)
            {
                if (s_RenderEventFunc == IntPtr.Zero)
                    s_RenderEventFunc = DisplayXRProviderNative.dxr_prov_get_render_event_func();
                if (s_RenderEventFunc == IntPtr.Zero)
                {
                    if (!s_SpriteEventMissingLogged)
                    {
                        s_SpriteEventMissingLogged = true;
                        Debug.LogWarning("[DisplayXR] Depth cursor sprite: native plug-in has no render event " +
                                         "(stale binary?) - keeping the default cursor look.");
                    }
                    return;
                }
                var api = SystemInfo.graphicsDeviceType;
                if (api == UnityEngine.Rendering.GraphicsDeviceType.Direct3D11 ||
                    api == UnityEngine.Rendering.GraphicsDeviceType.Direct3D12)
                {
                    // The native side made the target for the CURRENT sprite size; a stale
                    // target (resize in flight) is skipped until the new one appears.
                    if (tw != (uint)w || th != (uint)h) return;
                    if (s_SpriteBridge == null || s_SpriteBridgePtr != target)
                    {
                        ReleaseSpriteBridge();
                        // Same sRGB-ness as the RT, so Unity sees identical formats; the copy
                        // moves bytes either way.
                        s_SpriteBridge = Texture2D.CreateExternalTexture(w, h, TextureFormat.RGBA32, false, !srgb, target);
                        s_SpriteBridge.name = "DisplayXR_CursorSpriteBridge";
                        s_SpriteBridge.hideFlags = HideFlags.HideAndDontSave;
                        s_SpriteBridgePtr = target;
                    }
                    Graphics.CopyTexture(s_SpriteRT, 0, 0, s_SpriteBridge, 0, 0);
                }
                // In stream order after the copy: the provider uploads this generation at its
                // next submit (on Vulkan the event records the copy itself).
                GL.IssuePluginEvent(s_RenderEventFunc, DisplayXRProviderNative.kCursorSpriteFilledEvent0 +
                                    (int)(s_SpriteGen % DisplayXRProviderNative.kCursorSpriteGenSlots));
                s_SpriteFilledGen = s_SpriteGen;
                s_SpriteFilledTarget = target;
            }
            SpriteActive = DisplayXRProviderNative.dxr_prov_cursor_sprite_status() == 2;
        }

        // ---------------------------------------------------------------------

        const int kMaxRays = 20;
        const int kNativeMeasured = 0, kNativeAppPoint = 1; // dxr_prov_cursor_configure source
        static readonly float[] s_RayBuf = new float[kMaxRays * 6];
        static readonly Ray[] s_Rays = new Ray[kMaxRays];
        static readonly float[] s_HitBuf = new float[kMaxRays * 3];
        static int s_ExplicitFrame = -1;
        static bool s_ExplicitHas;
        static Vector3 s_ExplicitPoint;
        static bool s_PushedEnabled;
        static int s_PushedNative = -1;
        static float s_PushedMargin = float.NaN;
        static bool s_WeHidCursor;
        static bool s_LastShown;
        static bool s_StateKnown;
        static CursorState s_LoggedState;
        static ActiveDepthSource s_LoggedSource = (ActiveDepthSource)(-1);
        static bool s_NativeMissing;

        static int FetchRays()
        {
            uint n;
            try { n = DisplayXRProviderNative.dxr_prov_cursor_get_footprint_rays(s_RayBuf, kMaxRays); }
            catch (Exception) { return 0; }
            if (n > kMaxRays) n = kMaxRays;
            for (int i = 0; i < n; i++)
            {
                int o = i * 6;
                s_Rays[i] = new Ray(new Vector3(s_RayBuf[o], s_RayBuf[o + 1], s_RayBuf[o + 2]),
                                    new Vector3(s_RayBuf[o + 3], s_RayBuf[o + 4], s_RayBuf[o + 5]));
            }
            return (int)n;
        }

        static bool TryGetPointer(out Vector2 pos)
        {
            pos = default;
            if (Cursor.lockState == CursorLockMode.Locked) return false; // FPS-style: no pointer
#if HAS_INPUT_SYSTEM && ENABLE_INPUT_SYSTEM
            var m = Mouse.current;
            if (m != null)
            {
                pos = m.position.ReadValue(); // bottom-left origin, screen px
                return true;
            }
#endif
#if ENABLE_LEGACY_INPUT_MANAGER
            if (Input.mousePresent)
            {
                Vector3 mp = Input.mousePosition;
                pos = new Vector2(mp.x, mp.y);
                return true;
            }
#endif
            return false;
        }

        /// <summary>Physics: raycast the footprint; returns the hit count written to s_HitBuf.</summary>
        static uint CastFootprint()
        {
            uint count = 0;
            int n = FetchRays();
            for (int i = 0; i < n; i++)
            {
                Vector3 p;
                bool hit = CustomHitTest != null ? CustomHitTest(s_Rays[i], out p) : PhysicsRaycast(s_Rays[i], out p);
                if (!hit) continue;
                s_HitBuf[count * 3 + 0] = p.x;
                s_HitBuf[count * 3 + 1] = p.y;
                s_HitBuf[count * 3 + 2] = p.z;
                count++;
            }
            return count;
        }

        static bool PhysicsRaycast(Ray ray, out Vector3 point)
        {
#if DISPLAYXR_HAS_PHYSICS
            if (Physics.Raycast(ray, out RaycastHit h, RaycastMaxDistance, RaycastLayerMask,
                                QueryTriggerInteraction.Ignore))
            {
                point = h.point;
                return true;
            }
#endif
            point = default;
            return false;
        }

        static uint OnePoint(bool has, Vector3 p)
        {
            if (!has) return 0;
            s_HitBuf[0] = p.x;
            s_HitBuf[1] = p.y;
            s_HitBuf[2] = p.z;
            return 1;
        }

        /// <summary>
        /// Which source answers this frame, and (for app-point sources) the hits. Auto takes the
        /// depth branch while scene depth results keep arriving and the measured branch
        /// otherwise; it does not flip per frame at content edges, because a depth result
        /// covers the whole footprint ("nothing there" is an answer too, not a miss).
        /// </summary>
        static ActiveDepthSource Select(out uint hits)
        {
            hits = 0;
            if (s_ExplicitFrame == Time.frameCount)
            {
                hits = OnePoint(s_ExplicitHas, s_ExplicitPoint);
                return ActiveDepthSource.Manual;
            }
            switch (Source)
            {
                case DepthSource.Physics:
                    hits = CastFootprint();
                    return ActiveDepthSource.Physics;
                case DepthSource.Measured:
                    return RuntimeSpecVersion >= 7 ? ActiveDepthSource.Measured : ActiveDepthSource.NoDepth;
                case DepthSource.Depth:
                    if (!DisplayXRCursorDepthPatch.Fresh) return ActiveDepthSource.NoDepth;
                    hits = OnePoint(DisplayXRCursorDepthPatch.ResultHasContent, DisplayXRCursorDepthPatch.ResultPoint);
                    return ActiveDepthSource.Depth;
                default: // Auto
                    if (DisplayXRCursorDepthPatch.Fresh)
                    {
                        hits = OnePoint(DisplayXRCursorDepthPatch.ResultHasContent, DisplayXRCursorDepthPatch.ResultPoint);
                        return ActiveDepthSource.Depth;
                    }
                    // Depth works but this frame's answer is late: hold the branch (on-glass target,
                    // a slow sink for a frame or two) rather than flip to measured and back.
                    if (DisplayXRCursorDepthPatch.Working) return ActiveDepthSource.NoDepth;
                    return RuntimeSpecVersion >= 7 ? ActiveDepthSource.Measured : ActiveDepthSource.NoDepth;
            }
        }

        /// <summary>
        /// Per-frame pump, called by <c>DisplayXRProviderDriver.LateUpdate</c> while the
        /// session runs. Apps do not call this.
        /// </summary>
        internal static void Tick()
        {
            if (s_NativeMissing) return;
            try
            {
                TickInner();
            }
            catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException)
            {
                // An older native binary alongside newer managed code: degrade to the plain
                // OS cursor once, never throw per frame.
                s_NativeMissing = true;
                Debug.LogWarning("[DisplayXR] Depth-aware cursor unavailable: native plug-in has no " +
                                 "dxr_prov_cursor_* exports (stale binary?). " + e.Message);
                SetShown(false);
            }
        }

        static void TickInner()
        {
            bool enabled = Enabled;
            if (enabled)
            {
                bool havePointer = TryGetPointer(out Vector2 p);
                DisplayXRProviderNative.dxr_prov_cursor_set_pointer(
                    havePointer ? 1 : 0, p.x, p.y, Screen.width, Screen.height);
            }

            // Scene depth: keep the patch pipeline running whenever Depth/Auto is selected
            // (it reads this frame's pointer, just pushed).
            bool wantDepth = enabled && RuntimeSpecVersion >= 5 &&
                             (Source == DepthSource.Auto || Source == DepthSource.Depth);
            DisplayXRCursorDepthPatch.Tick(wantDepth);

            ActiveDepthSource active = ActiveDepthSource.None;
            uint hits = 0;
            if (enabled) active = Select(out hits);
            int native = active == ActiveDepthSource.Measured ? kNativeMeasured : kNativeAppPoint;

            // Configuration on change only (the native side logs the first few changes).
            if (enabled != s_PushedEnabled || native != s_PushedNative || Margin != s_PushedMargin)
            {
                DisplayXRProviderNative.dxr_prov_cursor_configure(enabled ? 1 : 0, native, Margin);
                s_PushedEnabled = enabled;
                s_PushedNative = native;
                s_PushedMargin = Margin;
            }
            if (enabled && native == kNativeAppPoint) DisplayXRProviderNative.dxr_prov_cursor_set_hits(s_HitBuf, hits);
            if (enabled) TickSprite();
            else SpriteActive = false;

            DisplayXRProviderNative.dxr_prov_cursor_get_state(out var st);
            RuntimeSpecVersion = st.runtimeSpec;
            WillDraw = enabled && st.willDraw != 0;
            Disparity = st.disparity;
            if (!enabled) State = CursorState.Off;
            else if (!Supported) State = CursorState.Unsupported;
            else if (st.requested == 0) State = CursorState.Idle;
            else State = WillDraw ? CursorState.Lifted : CursorState.OnGlass;
            ActiveSource = st.requested != 0 ? active : ActiveDepthSource.None;

            SetShown(WillDraw);
            LogState();
        }

        static void SetShown(bool shown)
        {
            if (shown == s_LastShown) return;
            s_LastShown = shown;
            if (shown && ManageOsCursor)
            {
                // Hide only a cursor that is showing, and remember that WE hid it, so an
                // app that hides its own cursor never gets it shown back by us.
                if (Cursor.visible)
                {
                    Cursor.visible = false;
                    s_WeHidCursor = true;
                }
            }
            else if (!shown && s_WeHidCursor)
            {
                Cursor.visible = true;
                s_WeHidCursor = false;
            }
            DepthCursorShownChanged?.Invoke(shown);
        }

        static void LogState()
        {
            if (!LogStateChanges) return;
            // Lifted <-> OnGlass flips with every hover; log only the coarse states, plus which
            // branch Auto took (that changes only when scene depth appears / disappears).
            CursorState coarse = State == CursorState.Lifted ? CursorState.OnGlass : State;
            ActiveDepthSource src = ActiveSource == ActiveDepthSource.None ? s_LoggedSource : ActiveSource;
            if (s_StateKnown && s_LoggedState == coarse && s_LoggedSource == src) return;
            s_StateKnown = true;
            s_LoggedState = coarse;
            s_LoggedSource = src;
            if (coarse == CursorState.Unsupported)
                Debug.Log($"[DisplayXR] Depth-aware cursor: runtime XR_DXR_cursor_depth spec v{RuntimeSpecVersion} " +
                          $"does not support source {Source} (needs v{(Source == DepthSource.Measured ? 7 : 5)}) " +
                          "- keeping the plain OS cursor.");
            else
                Debug.Log($"[DisplayXR] Depth-aware cursor: {coarse}, source {Source} -> {src} " +
                          $"(runtime spec v{RuntimeSpecVersion})");
        }

        /// <summary>Session stopped: show the OS cursor again and forget the runtime's answers.
        /// The app's opt-in (<see cref="Enabled"/>, <see cref="Source"/>) survives restarts.</summary>
        internal static void Reset()
        {
            SetShown(false);
            WillDraw = false;
            Disparity = 0f;
            ActiveSource = ActiveDepthSource.None;
            State = Enabled ? CursorState.Unsupported : CursorState.Off;
            s_StateKnown = false;
            s_PushedNative = -1; // re-push the config to the next session
            // The provider destroyed the sprite swapchain + target with the session; its
            // configuration survives, so the next session only needs a fresh fill.
            s_SpriteFilledGen = 0;
            s_SpriteFilledTarget = IntPtr.Zero;
            ReleaseSpriteBridge();
            SpriteActive = false;
            DisplayXRCursorDepthPatch.Reset();
        }
    }
}
