// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// OpenXR action binding for the display provider: surfaces the controllers and styluses
// of ANY DisplayXR input provider (runtime ADR-034 / ADR-054 input-provider plug-ins)
// to Unity as UnityEngine.XR InputDevices, through the IUnityXRInput provider in
// displayxr_input_provider.cpp.
//
// What it binds. One action set ("displayxr_input") with /user/hand/left and
// /user/hand/right subaction paths, suggested for two interaction profiles:
//   - /interaction_profiles/khr/simple_controller (always): grip + aim pose, select,
//     menu, haptic.
//   - /interaction_profiles/logitech/mx_ink_stylus_logitech (only when the runtime
//     advertises XR_LOGITECH_mx_ink_stylus_interaction, which is then enabled on the
//     instance): grip, aim and tip pose, tip force, middle cluster force, front and
//     back cluster click + double tap, docked, haptic. /input/system/click is a system
//     component and is deliberately not bound.
// Whichever profile the runtime binds a hand to (xrGetCurrentInteractionProfile)
// decides what kind of Unity device that hand is.
//
// Threading. Same model as the hands TU (displayxr_provider_hands.h): the session is
// created and torn down on Unity's GRAPHICS thread; Unity's input subsystem ticks on
// the MAIN thread. One mutex orders action-set / space create, sync / locate and
// destroy against the session's teardown, which takes it before xrDestroySession.
// xrSyncActions, xrGetActionState* and xrLocateSpace run under it on the main thread;
// OpenXR requires no external synchronisation for them against the frame loop. The
// predicted display time crosses threads through an atomic written once per begun
// frame, so the poses are located at the time the current frame's views were.
//
// Frame. Poses are located in the provider's LOCAL reference space (the views' space)
// and re-expressed RELATIVE TO THE RIG CAMERA: rel = inv(rig pose) o G. By default
// nothing is scaled: the runtime itself places action spaces in the frame of the rig
// the latest xrLocateViews chained (runtime input-provider-discovery.md 4b.4 / 4b.4a):
//   - a glass-registered device (XRT_TRACKING_TYPE_RIG_LOCAL_PHYSICAL, the frame a
//     stylus provider publishes in) comes back already in rig units, on the glass:
//     the canvas rebased and carried by the rig's glass mapping (display rig
//     virtualDisplayHeight / canvas h about the rig pose; camera rig its virtual
//     screen height on the convergence plane). A tip on the glass is on the pixel it
//     touches, in a window too. Scaling it again here would scale the pen twice.
//   - a display-relative device (RIG_LOCAL) comes back at P o (s L): on the rig's
//     plane P, canvas-relative, METRIC (no rig length scale);
//   - a stage-anchored device comes back as tracked.
//
// Opt-in rig gain (DisplayXR_Input_SetRigGain(1) / DISPLAYXR_INPUT_RIG_GAIN=1): for a
// display-relative device on a runtime that does not register it to the glass, take
// rel = inv(P) o G, scale it by the hand joints' metres-to-rig-units factor k about the
// plane and re-place it, relative to the rig camera:
//     display rig:           plane at the rig camera,
//                            k = perspectiveFactor * virtualDisplayHeight / canvas h
//     camera rig, conv > 0:  plane 1/conv in front of the camera (the glass),
//                            k = (2 tan(vFov/2) / conv) / canvas h
//     camera rig, conv = 0:  plane nominalViewerZ in front, k = metersToVirtual (1)
//   This is the mapping the runtime applies to rig-scoped hand joints. Never enable
//   it for a glass-registered stylus.
// The rig the frame is built from is a snapshot of the one the latest primary view
// locate chained (dxr_input_on_views_located, graphics thread), never the live
// tunables, so the poses are in the rig the views were located with.
//
// Lifetime. Everything here is per instance: the provider creates a new instance on
// every session start (and on the editor's dock/undock restart), and the action set,
// suggested bindings, spaces and attach are redone each time. The app's opt-out
// (DisplayXR_Input_SetEnabled(0) / DISPLAYXR_INPUT=0) is read when the instance is
// created: disabled = no action set is ever created, the session is exactly as before.

#pragma once

#include "displayxr_provider_session.h" // DISPLAYXR_EXPORT
#include <stdint.h>

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The MX Ink stylus interaction profile extension. Defined here because the
//! OpenXR headers this plug-in builds against (1.0.34) predate it.
#define DXR_INPUT_MX_INK_EXTENSION_NAME "XR_LOGITECH_mx_ink_stylus_interaction"

//! What a hand is bound to (dxr_input_hand_kind / DisplayXR_Input_GetHandKind).
#define DXR_INPUT_KIND_NONE 0   //!< no interaction profile bound: no Unity device
#define DXR_INPUT_KIND_SIMPLE 1 //!< /interaction_profiles/khr/simple_controller
#define DXR_INPUT_KIND_STYLUS 2 //!< /interaction_profiles/logitech/mx_ink_stylus_logitech

//! Pose slots.
#define DXR_INPUT_POSE_GRIP 0
#define DXR_INPUT_POSE_AIM 1
#define DXR_INPUT_POSE_TIP 2
#define DXR_INPUT_POSE_COUNT 3

//! One located pose, UNITY frame (left-handed, +Z forward), relative to the rig
//! camera (as located, or with the opt-in rig gain). flags = XrSpaceLocationFlags (1 orient valid, 2 pos
//! valid, 4 orient tracked, 8 pos tracked); vel_flags = XrSpaceVelocityFlags (1
//! linear, 2 angular).
typedef struct DxrInputPose
{
	float position[3];
	float rotation[4]; //!< x, y, z, w
	float linear_velocity[3];
	float angular_velocity[3];
	uint64_t flags;
	uint64_t vel_flags;
	int active; //!< the pose action is active (bound and the device is present)
} DxrInputPose;

//! The state of one hand after the latest update (main thread).
typedef struct DxrInputHandState
{
	int kind; //!< DXR_INPUT_KIND_*
	DxrInputPose pose[DXR_INPUT_POSE_COUNT];
	// Stylus (KIND_STYLUS)
	float tip_force;    //!< /input/tip_logitech/force, 0..1
	float middle_force; //!< /input/cluster_middle_logitech/force, 0..1
	int front_click;
	int front_double_tap;
	int back_click;
	int back_double_tap;
	int docked;
	// Simple controller (KIND_SIMPLE)
	int select_click;
	int menu_click;
} DxrInputHandState;

//! The rig frame the latest primary xrLocateViews was made in (OpenXR frame,
//! LOCAL).
typedef struct DxrInputRigFrame
{
	XrPosef rig_pose;        //!< the pose chained on the view rig (identity when none)
	XrVector3f plane_offset; //!< the rig's display plane origin, in the rig frame
	                         //!< (rig units)
	float scale;             //!< canvas metres -> rig units at that plane
	int kind;                //!< DXR_PROV_HAND_RIG_* (displayxr_provider_hands.h)
} DxrInputRigFrame;

// ---- Provider-internal hooks (called from displayxr_provider_session.cpp)
// ----

typedef void (*DxrInputLogFn)(const char *fmt, ...);

//! Before xrCreateInstance: append the extensions this TU wants to @p names
//! (room for one more) if the runtime advertises them. @p advertised_mx_ink:
//! the runtime lists XR_LOGITECH_mx_ink_stylus_interaction. Returns the new
//! count.
uint32_t
dxr_input_add_instance_extensions(const char **names, uint32_t count, int advertised_mx_ink);
//! After xrCreateInstance: resolve entry points, create the action set +
//! actions and suggest the bindings. @p mx_ink_enabled = the extension was
//! enabled on the instance.
void
dxr_input_on_instance(XrInstance instance, PFN_xrGetInstanceProcAddr gipa, int mx_ink_enabled, DxrInputLogFn log);
//! After xrCreateSession + the LOCAL reference space: create the action spaces
//! and attach the action set.
void
dxr_input_on_session_created(XrSession session, XrSpace base_space);
//! Before xrDestroySession: destroy the spaces and the action set, forget
//! everything.
void
dxr_input_on_session_destroying(void);
//! After xrBeginFrame: the predicted display time the views are located at.
void
dxr_input_on_frame_begun(XrTime predicted_display_time);
//! After the primary xrLocateViews: the rig frame those views were located in.
void
dxr_input_on_views_located(const DxrInputRigFrame *frame);
//! XrEventDataInteractionProfileChanged arrived (graphics thread): re-query the
//! current interaction profiles on the next update.
void
dxr_input_on_interaction_profile_changed(void);

// ---- Unity input provider hooks (displayxr_input_provider.cpp, main thread)
// ----

//! Sync (when @p before_render is 0) and locate (always). Returns 1 when an
//! update ran.
int
dxr_input_update(int before_render);
//! A copy of one hand's latest state (hand 0 = left, 1 = right). Returns its
//! kind.
int
dxr_input_get_hand_state(int hand, DxrInputHandState *out);
//! xrApplyHapticFeedback on that hand: @p amplitude 0..1, @p duration_s seconds
//! (0 = the runtime's minimum pulse). Returns 1 when the runtime accepted it.
int
dxr_input_haptic(int hand, float amplitude, float duration_s);
//! xrStopHapticFeedback on that hand.
void
dxr_input_haptic_stop(int hand);

// ---- C# surface (Runtime/Provider/DisplayXRInputNative.cs) ----

//! The app's switch, read at the next instance creation (default 1;
//! DISPLAYXR_INPUT=0 in the environment also disables). 0 = no action set, no
//! bindings, no sync.
DISPLAYXR_EXPORT void
DisplayXR_Input_SetEnabled(int enabled);
//! 1 when the action set is attached to a live session.
DISPLAYXR_EXPORT int
DisplayXR_Input_IsActive(void);
//! 1 when the runtime advertised XR_LOGITECH_mx_ink_stylus_interaction and it
//! was enabled + its bindings were accepted on the current instance.
DISPLAYXR_EXPORT int
DisplayXR_Input_IsStylusProfileAvailable(void);
//! DXR_INPUT_KIND_* of a hand (0 left, 1 right) after the latest update.
DISPLAYXR_EXPORT int
DisplayXR_Input_GetHandKind(int hand);
//! DXR_PROV_HAND_RIG_* of the rig frame the poses are expressed in.
DISPLAYXR_EXPORT int
DisplayXR_Input_RigKind(void);
//! 1 = also apply the rig's metres -> rig units gain about the rig's display
//! plane (the hand-joint mapping); 0 (default) = poses as located, relative to
//! the rig camera. Live. DISPLAYXR_INPUT_RIG_GAIN=1 in the environment also turns
//! it on (read at instance creation).
DISPLAYXR_EXPORT void
DisplayXR_Input_SetRigGain(int enabled);

#ifdef __cplusplus
}
#endif
