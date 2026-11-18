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
    /// Stereo-correct picking: screen point to ray, world point to screen point, through the
    /// views the viewer is actually looking through (#460).
    ///
    /// <para>
    /// <b>Why not <c>Camera.ScreenPointToRay</c>.</b> Under DisplayXR the picture is drawn with
    /// two off-axis frusta the runtime computes from the tracked eye positions.
    /// <c>Camera.projectionMatrix</c> and the camera transform know neither: a ray built from
    /// them starts at the wrong point and misses what the viewer sees, by more the further the
    /// object is from the display plane and the further the head is from the centre. These
    /// methods use the per-eye view and projection the provider renders with.
    /// </para>
    /// <para>
    /// <b>Which eye.</b> The default is the <b>cyclopean</b> view (the point midway between
    /// the eyes, <c>Camera.MonoOrStereoscopicEye.Mono</c>): a fused object is perceived there,
    /// so that is where a pointer aims. Pass <c>Left</c> / <c>Right</c> for one eye's own ray
    /// or screen position; the two screen positions of one world point differ by its on-screen
    /// disparity.
    /// </para>
    /// <para>
    /// <b>Pixel space.</b> <c>screenPoint</c> is in Unity screen pixels, origin bottom-left -
    /// what <c>Input.mousePosition</c> and <c>Mouse.current.position</c> report - measured
    /// against <see cref="TryGetScreenSize"/>. That is <c>Screen.width</c> x
    /// <c>Screen.height</c> wherever Unity's own window (or the editor Game view) is what the
    /// user looks at. On Windows it is not always: in a transparent-overlay app Unity's window
    /// is hidden and the picture is in the provider's window, of a different size, and Unity's
    /// mouse position does not follow the cursor. Take the pointer from
    /// <see cref="TryGetPointerPosition"/> and sizes from <see cref="TryGetScreenSize"/> and
    /// the same code is right in both cases. A 3D zone (<see cref="DisplayXRProvider.SetZoneRect"/>)
    /// is accounted for: the views frame the zone, so the point is mapped into its rect.
    /// The mapping is always against that whole screen (or zone): a camera's own
    /// <c>Camera.rect</c> sub-viewport is not applied.
    /// </para>
    /// <para>
    /// Every method returns false, without logging or throwing, while there is nothing to
    /// pick through: the provider is not running, no stereo views have been located yet, or
    /// the native plug-in is missing. Call from the main thread. Nothing here allocates.
    /// The views are the ones of the last rendered frame.
    /// </para>
    /// </summary>
    public static class DisplayXRPicking
    {
        /// <summary>True when the provider is running and has located stereo views, i.e. the
        /// methods below can answer.</summary>
        public static bool IsAvailable => TryFetchViews(0);

        // ---- Screen point -> ray ----------------------------------------------------------

        /// <summary>
        /// The cyclopean ray through <paramref name="screenPoint"/> for the active rig camera
        /// (<see cref="DisplayXRRigManager.ActiveCamera"/>), in world space. The origin is on
        /// the near plane, as with <c>Camera.ScreenPointToRay</c>.
        /// </summary>
        /// <param name="screenPoint">Unity screen pixels, origin bottom-left (see the class
        /// remarks for the pixel space).</param>
        public static bool TryScreenPointToRay(Vector2 screenPoint, out Ray ray)
        {
            return TryScreenPointToRay(screenPoint, null, Camera.MonoOrStereoscopicEye.Mono, 0, out ray);
        }

        /// <summary>
        /// The cyclopean ray through <paramref name="screenPoint"/> for
        /// <paramref name="camera"/>. Every camera the provider renders shares the located
        /// views; the camera only decides where they sit in the world when it is not the rig
        /// camera driving them (a second camera, or a camera without a DisplayXR rig).
        /// </summary>
        public static bool TryScreenPointToRay(Vector2 screenPoint, Camera camera, out Ray ray)
        {
            return TryScreenPointToRay(screenPoint, camera, Camera.MonoOrStereoscopicEye.Mono, 0, out ray);
        }

        /// <summary>
        /// The ray through <paramref name="screenPoint"/> for one eye
        /// (<c>Left</c> / <c>Right</c>), or the cyclopean one (<c>Mono</c>).
        /// </summary>
        /// <param name="camera">Null = the active rig camera.</param>
        public static bool TryScreenPointToRay(Vector2 screenPoint, Camera camera,
                                               Camera.MonoOrStereoscopicEye eye, out Ray ray)
        {
            return TryScreenPointToRay(screenPoint, camera, eye, 0, out ray);
        }

        /// <summary>
        /// As above, for one 3D zone of a multi-zone app. Zone 0 is the primary zone (or the
        /// whole window when the app set none); zone i &gt;= 1 is the extra zone set with
        /// <see cref="DisplayXRProvider.SetZone"/> index i. Each zone has its own views and
        /// its own rect, so a point is picked through the zone it lies in.
        /// </summary>
        public static bool TryScreenPointToRay(Vector2 screenPoint, Camera camera,
                                               Camera.MonoOrStereoscopicEye eye, int zone, out Ray ray)
        {
            ray = new Ray(Vector3.zero, Vector3.forward);
            if (!TrySelectView(eye, zone, out float[] view, out float[] proj)) return false;
            if (!TryGetFrame(camera, out Frame frame)) return false;
            if (!TryScreenToNdc(screenPoint, zone, out float nx, out float ny)) return false;
            if (!DisplayXRPickingMath.TryUnprojectRay(view, proj, nx, ny, out float ox, out float oy, out float oz,
                                                      out float dx, out float dy, out float dz))
                return false;
            ray = new Ray(frame.position + frame.rotation * new Vector3(ox, oy, oz),
                          frame.rotation * new Vector3(dx, dy, dz));
            return true;
        }

        // ---- World point -> screen point --------------------------------------------------

        /// <summary>
        /// Where the viewer sees <paramref name="world"/> on the screen, through the
        /// cyclopean view, for the active rig camera. x, y: Unity screen pixels, origin
        /// bottom-left, the same space as <c>screenPoint</c> above (they may lie outside the
        /// screen). z: the point's signed distance in front of the viewer along the view axis,
        /// in world units; negative = behind the viewer.
        /// <para>
        /// Returns false when the point is at or behind the viewer's eye plane: it has no
        /// place on the screen. z is still written, so the caller can tell that case from
        /// "not running" (z = 0 then).
        /// </para>
        /// </summary>
        public static bool TryWorldToScreenPoint(Vector3 world, out Vector3 screenPoint)
        {
            return TryWorldToScreenPoint(world, null, Camera.MonoOrStereoscopicEye.Mono, 0, out screenPoint);
        }

        /// <summary>
        /// As above for one eye (<c>Left</c> / <c>Right</c>) or the cyclopean view
        /// (<c>Mono</c>). The left and right x of one world point differ by its on-screen
        /// disparity: zero on the display plane, right minus left positive behind it and
        /// negative in front of it.
        /// </summary>
        /// <param name="camera">Null = the active rig camera.</param>
        public static bool TryWorldToScreenPoint(Vector3 world, Camera camera,
                                                 Camera.MonoOrStereoscopicEye eye, out Vector3 screenPoint)
        {
            return TryWorldToScreenPoint(world, camera, eye, 0, out screenPoint);
        }

        /// <summary>As above, for one 3D zone (see <see cref="TryScreenPointToRay(Vector2, Camera, Camera.MonoOrStereoscopicEye, int, out Ray)"/>).</summary>
        public static bool TryWorldToScreenPoint(Vector3 world, Camera camera,
                                                 Camera.MonoOrStereoscopicEye eye, int zone, out Vector3 screenPoint)
        {
            screenPoint = Vector3.zero;
            if (!TrySelectView(eye, zone, out float[] view, out float[] proj)) return false;
            if (!TryGetFrame(camera, out Frame frame)) return false;
            if (!TryGetMapping(zone, out Mapping m)) return false;
            world = Quaternion.Inverse(frame.rotation) * (world - frame.position);
            bool inFront = DisplayXRPickingMath.TryProject(view, proj, world.x, world.y, world.z,
                                                           out float nx, out float ny, out float _, out float depth);
            screenPoint.z = depth;
            if (!inFront) return false;
            DisplayXRPickingMath.NdcToViewportUV(nx, ny, out float u, out float v);
            if (!DisplayXRPickingMath.ViewportUVToScreen(u, v, m.screenW, m.screenH, m.clientW, m.clientH,
                                                         m.zoneValid, m.zoneX, m.zoneY, m.zoneW, m.zoneH,
                                                         out float sx, out float sy))
                return false;
            screenPoint.x = sx;
            screenPoint.y = sy;
            return true;
        }

        // ---- Screen point + distance -> world point ---------------------------------------

        /// <summary>
        /// The world point the viewer sees at <paramref name="screenPoint"/>,
        /// <paramref name="distance"/> world units in front of them, through the cyclopean
        /// view of the active rig camera. The distance is measured along the view axis - the
        /// z <see cref="TryWorldToScreenPoint(Vector3, out Vector3)"/> returns, and the
        /// convention of <c>Camera.ScreenToWorldPoint</c> - not along the ray.
        /// </summary>
        public static bool TryScreenToWorldPoint(Vector2 screenPoint, float distance, out Vector3 world)
        {
            return TryScreenToWorldPoint(screenPoint, distance, null, Camera.MonoOrStereoscopicEye.Mono, 0, out world);
        }

        /// <summary>As above for one eye or the cyclopean view.</summary>
        /// <param name="camera">Null = the active rig camera.</param>
        public static bool TryScreenToWorldPoint(Vector2 screenPoint, float distance, Camera camera,
                                                 Camera.MonoOrStereoscopicEye eye, out Vector3 world)
        {
            return TryScreenToWorldPoint(screenPoint, distance, camera, eye, 0, out world);
        }

        /// <summary>As above, for one 3D zone.</summary>
        public static bool TryScreenToWorldPoint(Vector2 screenPoint, float distance, Camera camera,
                                                 Camera.MonoOrStereoscopicEye eye, int zone, out Vector3 world)
        {
            world = Vector3.zero;
            if (!TrySelectView(eye, zone, out float[] view, out float[] proj)) return false;
            if (!TryGetFrame(camera, out Frame frame)) return false;
            if (!TryScreenToNdc(screenPoint, zone, out float nx, out float ny)) return false;
            if (!DisplayXRPickingMath.TryUnprojectPoint(view, proj, nx, ny, distance, out float x, out float y, out float z))
                return false;
            world = frame.position + frame.rotation * new Vector3(x, y, z);
            return true;
        }

        // ---- The pixel space --------------------------------------------------------------

        /// <summary>
        /// The size, in pixels, of the screen that <c>screenPoint</c> is measured against:
        /// the window the 3D picture is shown in. <c>Screen.width</c> x <c>Screen.height</c>
        /// when that is Unity's own window or the editor Game view; the provider's window
        /// when Unity's is hidden behind it (a Windows transparent-overlay app, or the Windows
        /// editor run with <c>DISPLAYXR_PROV_EXTERNAL_WINDOW=1</c>), where <c>Screen.*</c>
        /// describes a window nobody sees.
        /// </summary>
        public static bool TryGetScreenSize(out Vector2 size)
        {
            size = Vector2.zero;
            if (!TryGetScreenSize(out float w, out float h)) return false;
            size = new Vector2(w, h);
            return true;
        }

        /// <summary>
        /// The pointer, in the pixel space of <see cref="TryGetScreenSize"/> (origin
        /// bottom-left): the mouse through whichever input backend the project has enabled,
        /// or the cursor over the provider's window where Unity's own window is hidden and
        /// its mouse position does not follow the cursor. The position may lie outside the
        /// screen. Returns false when there is no pointer to read. It reads the mouse, not
        /// touches: on a touch-only device (Android without a mouse) it returns false, and a
        /// touch position read from Unity is already in this pixel space.
        /// </summary>
        public static bool TryGetPointerPosition(out Vector2 screenPoint)
        {
            screenPoint = Vector2.zero;
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
            if (UsesProviderWindow())
            {
                try
                {
                    // Client pixels of the provider's window, origin top-left. (#437) absence
                    // is the return value: a cursor left of or above the window is negative.
                    if (DisplayXRNative.displayxr_get_overlay_pointer(out int cx, out int cy, out int _) == 0)
                        return false;
                    DisplayXRNative.displayxr_get_overlay_size(out int ow, out int oh);
                    if (ow <= 0 || oh <= 0) return false;
                    screenPoint = new Vector2(cx, oh - cy);
                    return true;
                }
                catch (EntryPointNotFoundException) { return false; }
                catch (DllNotFoundException) { return false; }
            }
#endif
#if HAS_INPUT_SYSTEM && ENABLE_INPUT_SYSTEM
            var mouse = Mouse.current;
            if (mouse != null)
            {
                screenPoint = mouse.position.ReadValue();
                return true;
            }
#endif
#if ENABLE_LEGACY_INPUT_MANAGER
            if (Input.mousePresent)
            {
                Vector3 mp = Input.mousePosition;
                screenPoint = new Vector2(mp.x, mp.y);
                return true;
            }
#endif
            return false;
        }

        // ---- Shared with DisplayXRTransparentOverlay ----------------------------------------
        // The overlay hit-test works in its own window's client pixels (it knows that window
        // and its canvas sub-rect), so it enters below the pixel mapping. Same math.

        /// <summary>The primary views as the provider published them (OpenXR convention).
        /// False until a stereo pair has been located.</summary>
        internal static bool TryGetStereoMatrices(out Matrix4x4 leftView, out Matrix4x4 leftProj,
                                                  out Matrix4x4 rightView, out Matrix4x4 rightProj)
        {
            leftView = leftProj = rightView = rightProj = Matrix4x4.identity;
            if (!TryFetchPrimaryViews()) return false;
            leftView = ToMatrix(s_LV); leftProj = ToMatrix(s_LP);
            rightView = ToMatrix(s_RV); rightProj = ToMatrix(s_RP);
            return true;
        }

        /// <summary>See <see cref="DisplayXRPickingMath.Cyclopean"/>.</summary>
        internal static void BuildCyclopean(Matrix4x4 leftView, Matrix4x4 leftProj,
                                            Matrix4x4 rightView, Matrix4x4 rightProj,
                                            out Matrix4x4 cyclopeanView, out Matrix4x4 cyclopeanProj)
        {
            ToFloats(leftView, s_A); ToFloats(leftProj, s_B);
            ToFloats(rightView, s_C); ToFloats(rightProj, s_D);
            DisplayXRPickingMath.Cyclopean(s_A, s_B, s_C, s_D, s_E, s_F);
            cyclopeanView = ToMatrix(s_E);
            cyclopeanProj = ToMatrix(s_F);
        }

        /// <summary>World ray through a client pixel (top-left origin) of a viewport rect in
        /// the same pixels, through one view.</summary>
        internal static bool TryClientPixelToRay(float clientX, float clientY,
                                                 float vpX, float vpY, float vpW, float vpH,
                                                 Matrix4x4 view, Matrix4x4 proj, out Ray ray)
        {
            ray = new Ray(Vector3.zero, Vector3.forward);
            ToFloats(view, s_A); ToFloats(proj, s_B);
            DisplayXRPickingMath.ClientPixelToNdc(clientX, clientY, vpX, vpY, vpW, vpH, out float nx, out float ny);
            if (!DisplayXRPickingMath.TryUnprojectRay(s_A, s_B, nx, ny, out float ox, out float oy, out float oz,
                                                      out float dx, out float dy, out float dz))
                return false;
            ray = new Ray(new Vector3(ox, oy, oz), new Vector3(dx, dy, dz));
            return true;
        }

        /// <summary>World point to client pixels (top-left origin) of a window of the given
        /// size, through one view. False behind the eye or outside the near / far range.</summary>
        internal static bool TryWorldToClientPixel(Vector3 world, Matrix4x4 view, Matrix4x4 proj,
                                                   float clientW, float clientH, out Vector2 clientPx)
        {
            clientPx = Vector2.zero;
            ToFloats(view, s_A); ToFloats(proj, s_B);
            if (!DisplayXRPickingMath.TryProject(s_A, s_B, world.x, world.y, world.z,
                                                 out float nx, out float ny, out float nz, out float _))
                return false;
            if (nz < -1f || nz > 1f) return false;
            DisplayXRPickingMath.NdcToViewportUV(nx, ny, out float u, out float v);
            clientPx = new Vector2(u * clientW, v * clientH);
            return true;
        }

        /// <summary>Column-major float[16] to Matrix4x4.</summary>
        internal static Matrix4x4 ToMatrix(float[] m)
        {
            var mat = new Matrix4x4();
            mat.m00 = m[0];  mat.m10 = m[1];  mat.m20 = m[2];  mat.m30 = m[3];
            mat.m01 = m[4];  mat.m11 = m[5];  mat.m21 = m[6];  mat.m31 = m[7];
            mat.m02 = m[8];  mat.m12 = m[9];  mat.m22 = m[10]; mat.m32 = m[11];
            mat.m03 = m[12]; mat.m13 = m[13]; mat.m23 = m[14]; mat.m33 = m[15];
            return mat;
        }

        static void ToFloats(Matrix4x4 mat, float[] m)
        {
            m[0] = mat.m00;  m[1] = mat.m10;  m[2] = mat.m20;  m[3] = mat.m30;
            m[4] = mat.m01;  m[5] = mat.m11;  m[6] = mat.m21;  m[7] = mat.m31;
            m[8] = mat.m02;  m[9] = mat.m12;  m[10] = mat.m22; m[11] = mat.m32;
            m[12] = mat.m03; m[13] = mat.m13; m[14] = mat.m23; m[15] = mat.m33;
        }

        // ---- Views --------------------------------------------------------------------------

        // Main-thread scratch: the last fetched pair, its cyclopean view, and conversion buffers.
        static readonly float[] s_LV = new float[16], s_LP = new float[16];
        static readonly float[] s_RV = new float[16], s_RP = new float[16];
        static readonly float[] s_CV = new float[16], s_CP = new float[16];
        static readonly float[] s_A = new float[16], s_B = new float[16], s_C = new float[16];
        static readonly float[] s_D = new float[16], s_E = new float[16], s_F = new float[16];

        // One latch per entry point, so a native library that lacks an OPTIONAL export (an
        // older binary next to newer managed code) loses that feature only: it must never
        // take the primary views - and with them the overlay's hit-test - down with it. Each
        // stops a per-frame caller from throwing (and catching) on every call.
        static bool s_ViewsMissing, s_SessionMissing, s_ZoneViewsMissing;

        // The primary zone's pair into s_LV..s_RP, straight from the shared state the provider
        // publishes each frame. No session check: the overlay reads it this way, and the valid
        // flag only ever goes up once a pair has been located.
        static bool TryFetchPrimaryViews()
        {
            if (s_ViewsMissing) return false;
            try
            {
                DisplayXRNative.displayxr_get_stereo_matrices(s_LV, s_LP, s_RV, s_RP, out int valid);
                return valid != 0;
            }
            catch (DllNotFoundException) { s_ViewsMissing = true; return false; }
            catch (EntryPointNotFoundException) { s_ViewsMissing = true; return false; }
        }

        static bool IsSessionRunning()
        {
            if (s_SessionMissing) return false;
            try { return DisplayXRProvider.IsRunning; }
            catch (DllNotFoundException) { s_SessionMissing = true; return false; }
            catch (EntryPointNotFoundException) { s_SessionMissing = true; return false; }
        }

        // The pair of one zone into s_LV..s_RP, only while the session runs (the shared state
        // keeps the last session's matrices after it stops).
        static bool TryFetchViews(int zone)
        {
            if (zone < 0 || !IsSessionRunning()) return false;
            if (zone == 0) return TryFetchPrimaryViews();
            if (s_ZoneViewsMissing) return false;
            try
            {
                return DisplayXRProviderNative.dxr_prov_get_zone_stereo_matrices(
                           (uint)zone, s_LV, s_LP, s_RV, s_RP) != 0;
            }
            catch (DllNotFoundException) { s_ZoneViewsMissing = true; return false; }
            catch (EntryPointNotFoundException) { s_ZoneViewsMissing = true; return false; }
        }

        static bool TrySelectView(Camera.MonoOrStereoscopicEye eye, int zone, out float[] view, out float[] proj)
        {
            view = proj = null;
            if (!TryFetchViews(zone)) return false;
            switch (eye)
            {
                case Camera.MonoOrStereoscopicEye.Left: view = s_LV; proj = s_LP; break;
                case Camera.MonoOrStereoscopicEye.Right: view = s_RV; proj = s_RP; break;
                default:
                    DisplayXRPickingMath.Cyclopean(s_LV, s_LP, s_RV, s_RP, s_CV, s_CP);
                    view = s_CV; proj = s_CP;
                    break;
            }
            return true;
        }

        // ---- Where the located views sit in the world -------------------------------------
        //
        // The views are located in the runtime's space with the last pushed rig pose chained
        // in, and Unity composes every camera's eye poses as
        // camera.transform * (rigPose^-1 * view) - see set_eye_pose_rig_relative in the native
        // provider. So the views sit at camera.transform * rigPose^-1: the identity (up to
        // rounding) for a rig camera standing where it was when its rig pushed, and something
        // else for any other camera, for a camera with no rig at all (rig pose = identity),
        // and for a rig camera that moved after its rig stopped pushing (component disabled,
        // splash up) - the runtime keeps the pose it was last sent. There is deliberately no
        // "this is the rig camera, so identity" shortcut: it is wrong in that last case.

        struct Frame
        {
            public Vector3 position;
            public Quaternion rotation;
        }

        static bool TryGetFrame(Camera camera, out Frame frame)
        {
            frame = default(Frame);
            frame.rotation = Quaternion.identity;

            if (camera == null)
            {
                camera = DisplayXRRigManager.ActiveCamera;
                if (camera == null) camera = Camera.main;
                if (camera == null) return false; // no camera: nowhere to put the views
            }

            Transform t = camera.transform;
            Vector3 cp = t.position;
            Quaternion cq = t.rotation;
            Vector3 rp = Vector3.zero;
            Quaternion rq = Quaternion.identity;
            if (DisplayXRProvider.RigPlaneValid)
            {
                rp = DisplayXRProvider.RigPlanePos;
                rq = DisplayXRProvider.RigPlaneRotation;
            }
            DisplayXRPickingMath.FrameFromPoses(
                cp.x, cp.y, cp.z, cq.x, cq.y, cq.z, cq.w,
                rp.x, rp.y, rp.z, rq.x, rq.y, rq.z, rq.w,
                out float px, out float py, out float pz,
                out float qx, out float qy, out float qz, out float qw);
            frame.position = new Vector3(px, py, pz);
            frame.rotation = new Quaternion(qx, qy, qz, qw);
            return true;
        }

        // ---- Pixel mapping ------------------------------------------------------------------

        struct Mapping
        {
            public float screenW, screenH; // what screenPoint is measured against
            public float clientW, clientH; // the same area in the zone rect's pixels; 0 = same unit
            public bool zoneValid;
            public float zoneX, zoneY, zoneW, zoneH; // client pixels, top-left origin
        }

        static bool TryScreenToNdc(Vector2 screenPoint, int zone, out float ndcX, out float ndcY)
        {
            ndcX = ndcY = 0f;
            if (!TryGetMapping(zone, out Mapping m)) return false;
            if (!DisplayXRPickingMath.ScreenToViewportUV(screenPoint.x, screenPoint.y, m.screenW, m.screenH,
                                                         m.clientW, m.clientH,
                                                         m.zoneValid, m.zoneX, m.zoneY, m.zoneW, m.zoneH,
                                                         out float u, out float v))
                return false;
            DisplayXRPickingMath.ViewportUVToNdc(u, v, out ndcX, out ndcY);
            return true;
        }

        // The viewport the views of `zone` frame. The same rule the depth-aware cursor applies
        // natively (ps_cursor_chain): an APP-authored zone frames its rect; anything else -
        // no zone, or the editor's own Game-view zone - frames the whole screen.
        static bool TryGetMapping(int zone, out Mapping m)
        {
            m = default(Mapping);
            if (!TryGetScreenSize(out m.screenW, out m.screenH)) return false;
            GetClientSize(m.screenW, m.screenH, out m.clientW, out m.clientH);

            if (zone > 0)
            {
                // Extra zones are always app-authored.
                if (s_ZoneRectMissing) return false;
                try
                {
                    if (DisplayXRProviderNative.dxr_prov_get_zone_rect_px(
                            (uint)zone, out int ex, out int ey, out int ew, out int eh) == 0 || ew <= 0 || eh <= 0)
                        return false;
                    m.zoneValid = true;
                    m.zoneX = ex; m.zoneY = ey; m.zoneW = ew; m.zoneH = eh;
                    return true;
                }
                catch (EntryPointNotFoundException) { s_ZoneRectMissing = true; return false; }
                catch (DllNotFoundException) { s_ZoneRectMissing = true; return false; }
            }

            // The provider's own answer to "do the views frame an app zone, and which rect":
            // an app-authored zone that is live. Not a managed copy of what the app asked
            // for - that cannot follow a clear (native keeps the app's rect and re-applies it
            // on a session restart), nor a runtime that ignores zones.
            if (!s_AppZoneMissing)
            {
                try
                {
                    if (DisplayXRProviderNative.dxr_prov_get_app_zone_rect_px(
                            out int zx, out int zy, out int zw, out int zh) != 0 && zw > 0 && zh > 0)
                    {
                        m.zoneValid = true;
                        m.zoneX = zx; m.zoneY = zy; m.zoneW = zw; m.zoneH = zh;
                    }
                    return true;
                }
                // A native library built before #460 (the export is new): the provider cannot
                // say. Fall through to the rect the overlay's own hit-test uses.
                catch (EntryPointNotFoundException) { s_AppZoneMissing = true; }
                catch (DllNotFoundException) { s_AppZoneMissing = true; }
            }
            // Older native only. An app that zones its 3D publishes the same sub-rect here
            // for the click-through mask; the transparent overlay maps its cursor into it.
            if (!s_CanvasRectMissing)
            {
                try
                {
                    if (DisplayXRNative.displayxr_get_canvas_rect_px(
                            out int cx, out int cy, out uint cw, out uint ch) != 0 && cw > 0 && ch > 0)
                    {
                        m.zoneValid = true;
                        m.zoneX = cx; m.zoneY = cy; m.zoneW = cw; m.zoneH = ch;
                    }
                }
                catch (EntryPointNotFoundException) { s_CanvasRectMissing = true; }
                catch (DllNotFoundException) { s_CanvasRectMissing = true; }
            }
            return true;
        }

        static bool s_AppZoneMissing, s_CanvasRectMissing, s_ZoneRectMissing;

        static bool TryGetScreenSize(out float w, out float h)
        {
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
            if (UsesProviderWindow())
            {
                w = h = 0f;
                try
                {
                    DisplayXRNative.displayxr_get_overlay_size(out int ow, out int oh);
                    if (ow <= 0 || oh <= 0) return false;
                    w = ow; h = oh;
                    return true;
                }
                catch (EntryPointNotFoundException) { return false; }
                catch (DllNotFoundException) { return false; }
            }
#endif
            w = Screen.width;
            h = Screen.height;
            return w > 0f && h > 0f;
        }

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
        // Is the picture in the provider's own window rather than in Unity's?
        //  * A transparent-overlay player: Unity's window is cloaked and parked off-screen
        //    (DisplayXRTransparentOverlay applies that, and says so here).
        //  * The editor with DISPLAYXR_PROV_EXTERNAL_WINDOW=1: a separate output window. (By
        //    default the editor weaves into the Game view, which is Unity's own screen.)
        static bool UsesProviderWindow()
        {
#if UNITY_EDITOR_WIN
            return DisplayXRProviderDriver.IsActive && !DisplayXRProviderDriver.GameViewTextureModeEnabled();
#else
            return DisplayXRTransparentOverlay.OverlayWindowActive;
#endif
        }
#endif

        // The window's client size in the pixels zone rects are authored in, when that is not
        // Unity's screen unit: on the native-Wayland player Unity's screen and pointer are
        // logical pixels and the zone rect is device pixels. 0 = the same unit.
        static void GetClientSize(float screenW, float screenH, out float clientW, out float clientH)
        {
            clientW = clientH = 0f;
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
            if (s_LinuxUiScaleMissing) return;
            try
            {
                float s = DisplayXRNative.displayxr_linux_ui_scale();
                if (s > 0f && s != 1f)
                {
                    clientW = screenW * s;
                    clientH = screenH * s;
                }
            }
            catch (EntryPointNotFoundException) { s_LinuxUiScaleMissing = true; }
            catch (DllNotFoundException) { s_LinuxUiScaleMissing = true; }
#endif
        }

#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
        static bool s_LinuxUiScaleMissing;
#endif
    }
}
