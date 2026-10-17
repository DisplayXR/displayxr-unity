// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// XR_EXT_hand_tracking binding for the display provider: surfaces the runtime's
// 26-joint hand tracking (served by an input-provider plug-in with hand roles —
// the Ultraleap provider today; runtime ADR-034, #825 Tier 2) to Unity as an
// XR Hands subsystem (com.unity.xr.hands), see Runtime/Hands/.
//
// Threading. The session is created and torn down on Unity's GRAPHICS thread
// (GfxStart/GfxStop -> dxr_prov_session_start/stop) and xrWaitFrame/xrBeginFrame
// run there too. The XR Hands subsystem updates on the MAIN thread (Update and
// Application.onBeforeRender, via XRHandProviderUtility.SubsystemUpdater), so
// DisplayXR_HandTracking_Locate is a cross-thread call into a live session. OpenXR
// makes xrLocateHandJointsEXT itself safe against the frame loop; what this module
// guards is the HANDLES: one mutex orders tracker create / locate / destroy against
// the session's own teardown, so a locate can never run on a tracker whose session
// is mid-destroy. The predicted display time crosses from the graphics thread
// through an atomic (written once per begun frame, read per locate), so the joints
// are located at the same time the current frame's views were.
//
// Lifetime. The app's request (Start/Stop) outlives the session: trackers are
// created lazily on the first locate after a session is up and destroyed with the
// session, then re-created on the next one — so an editor dock/undock or any other
// provider restart does not silently lose hand tracking (the v2.9.0 lesson: state
// pushed before session start must survive a restart).
//
// Frame. Joints are located in the provider's LOCAL reference space — the same
// space the views are located in — and converted to Unity's frame (negate Z,
// flip the quaternion's X/Y; the same mirror the eye poses take). Unlike the eye
// poses they are NOT made rig-relative here: the located views bake the app's rig
// pose in (XrDisplayRigDXR/XrCameraRigDXR carry it), while xrLocateHandJointsEXT
// never sees that descriptor, so LOCAL-space joints are already expressed relative
// to the display plane, which is exactly what the rig camera's transform represents
// on the Unity side. Scale (virtual display height vs. the physical panel) is
// applied in C#, where the active rig's tunables live.

#pragma once

#include <stdint.h>
#include "displayxr_provider_session.h" // DISPLAYXR_EXPORT

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Provider-internal hooks (called from displayxr_provider_session.cpp) ----

typedef void (*DxrHandsLogFn)(const char *fmt, ...);

//! After xrCreateInstance: soft-resolve the three entry points. @p enabled says
//! whether XR_EXT_hand_tracking was enabled on the instance.
void dxr_hands_on_instance(XrInstance instance, PFN_xrGetInstanceProcAddr gipa, int enabled, DxrHandsLogFn log);
//! After xrCreateSession + the LOCAL reference space: remember the handles the
//! trackers hang off. @p system_supported = XrSystemHandTrackingPropertiesEXT
//! .supportsHandTracking (0 when the runtime has no hand source right now).
void dxr_hands_on_session_created(XrSession session, XrSpace base_space, int system_supported);
//! Before xrDestroySession: destroy the trackers and forget the session.
void dxr_hands_on_session_destroying(void);
//! After xrBeginFrame: the predicted display time the views were located at.
void dxr_hands_on_frame_begun(XrTime predicted_display_time);

// ---- C# surface (Runtime/Hands/DisplayXRHandNative.cs) ----

//! Number of joints per hand (XR_HAND_JOINT_COUNT_EXT); the array lengths below.
#define DISPLAYXR_HAND_JOINT_COUNT 26

//! 1 when a session is up, XR_EXT_hand_tracking is enabled on its instance AND the
//! system reports supportsHandTracking; 0 otherwise. A pure query, no side effects.
//! False before the session starts (it comes up on the graphics thread a frame or
//! two into Play), so poll it rather than deciding once.
DISPLAYXR_EXPORT int DisplayXR_HandTracking_IsSupported(void);

//! The app wants hand tracking: trackers are created (now, or lazily once a session
//! is up) and Locate starts answering. Remembered across session restarts. Returns
//! the IsSupported state at the time of the call — 0 only means "not yet".
DISPLAYXR_EXPORT int DisplayXR_HandTracking_Start(void);

//! The app no longer wants hand tracking: trackers are destroyed and Locate
//! returns 0 until the next Start.
DISPLAYXR_EXPORT void DisplayXR_HandTracking_Stop(void);

//! Locate one hand's 26 joints.
//! @param hand       0 = left, 1 = right (XR_HAND_LEFT_EXT / XR_HAND_RIGHT_EXT).
//! @param time       XrTime to locate at, or 0 = the current frame's predicted
//!                   display time (what the views were located at).
//! @param out_poses  26 x 7 floats, UNITY frame (left-handed, +Z forward):
//!                   [px, py, pz, qx, qy, qz, qw] per joint, in metres, relative to
//!                   the LOCAL origin (the display plane). OpenXR joint order
//!                   (palm = 0, wrist = 1, thumb metacarpal = 2 ... little tip = 25).
//! @param out_radii  26 floats, joint radius in metres (may be NULL).
//! @param out_flags  26 XrSpaceLocationFlags (may be NULL): bit 0 orientation valid,
//!                   bit 1 position valid, bit 2 orientation tracked, bit 3
//!                   position tracked.
//! @return 1 when the hand is actively tracked this call (XrHandJointLocationsEXT
//!         .isActive), 0 when it is not — or when hand tracking is unavailable, not
//!         started, no frame has begun yet, or the locate failed. The arrays are
//!         filled whenever the locate itself succeeded, even for an inactive hand.
DISPLAYXR_EXPORT int DisplayXR_HandTracking_Locate(int hand, int64_t time, float *out_poses, float *out_radii,
                                                   uint64_t *out_flags);

#ifdef __cplusplus
}
#endif
