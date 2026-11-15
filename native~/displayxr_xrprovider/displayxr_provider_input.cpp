// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// OpenXR action binding — see displayxr_provider_input.h for what is bound, the
// threading model, the frame the poses are expressed in, and the lifetime.

#include "displayxr_provider_input.h"
#include "displayxr_provider_hands.h" // DXR_PROV_HAND_RIG_*

#include <atomic>
#include <mutex>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

std::mutex g_mx;

DxrInputLogFn g_log = nullptr;
int g_app_enabled = 1; // DisplayXR_Input_SetEnabled; outlives the instance

XrInstance g_instance = XR_NULL_HANDLE;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_base = XR_NULL_HANDLE; // the provider's LOCAL space (the views' space)
int g_mx_ink = 0;                // the stylus profile's bindings were accepted
int g_simple = 0;                // the simple controller's bindings were accepted
int g_attached = 0;

XrPath g_hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH};
XrPath g_profile_simple = XR_NULL_PATH;
XrPath g_profile_mx_ink = XR_NULL_PATH;

XrActionSet g_set = XR_NULL_HANDLE;
enum Act
{
	A_GRIP_POSE,
	A_AIM_POSE,
	A_TIP_POSE,
	A_SELECT,
	A_MENU,
	A_TIP_FORCE,
	A_MIDDLE_FORCE,
	A_FRONT_CLICK,
	A_FRONT_DTAP,
	A_BACK_CLICK,
	A_BACK_DTAP,
	A_DOCKED,
	A_HAPTIC,
	A_COUNT
};
XrAction g_act[A_COUNT] = {};
XrSpace g_space[2][DXR_INPUT_POSE_COUNT] = {};

DxrInputHandState g_state[2] = {};
int g_profile_logged[2] = {-1, -1};
int g_sync_failed_logged = 0;
unsigned g_ticks = 0;

std::atomic<int64_t> g_frame_time{0};
std::atomic<int> g_profile_dirty{1};

// The rig frame of the latest primary view locate (graphics thread -> main
// thread).
std::mutex g_rig_mx;
DxrInputRigFrame g_rig = {{{0, 0, 0, 1}, {0, 0, 0}}, {0, 0, 0}, 1.0f, 0};

PFN_xrStringToPath pfn_string_to_path = nullptr;
PFN_xrCreateActionSet pfn_create_action_set = nullptr;
PFN_xrDestroyActionSet pfn_destroy_action_set = nullptr;
PFN_xrCreateAction pfn_create_action = nullptr;
PFN_xrSuggestInteractionProfileBindings pfn_suggest = nullptr;
PFN_xrAttachSessionActionSets pfn_attach = nullptr;
PFN_xrSyncActions pfn_sync = nullptr;
PFN_xrGetActionStateBoolean pfn_get_bool = nullptr;
PFN_xrGetActionStateFloat pfn_get_float = nullptr;
PFN_xrGetActionStatePose pfn_get_pose = nullptr;
PFN_xrCreateActionSpace pfn_create_action_space = nullptr;
PFN_xrDestroySpace pfn_destroy_space = nullptr;
PFN_xrLocateSpace pfn_locate_space = nullptr;
PFN_xrApplyHapticFeedback pfn_apply_haptic = nullptr;
PFN_xrStopHapticFeedback pfn_stop_haptic = nullptr;
PFN_xrGetCurrentInteractionProfile pfn_current_profile = nullptr;

#define INPUT_LOG(...)                                                                                                 \
	do {                                                                                                           \
		if (g_log)                                                                                             \
			g_log(__VA_ARGS__);                                                                            \
	} while (0)

const char *
hand_name(int hand)
{
	return hand == 0 ? "left" : "right";
}

const char *
kind_name(int kind)
{
	return kind == DXR_INPUT_KIND_STYLUS   ? "stylus (mx_ink_stylus_logitech)"
	       : kind == DXR_INPUT_KIND_SIMPLE ? "simple_controller"
	                                       : "none";
}

bool
env_disabled()
{
	const char *v = getenv("DISPLAYXR_INPUT");
	return v != nullptr && v[0] == '0' && v[1] == '\0';
}

// ---- Pose math (OpenXR frame) ----

void
quat_rotate(const XrQuaternionf &q, const XrVector3f &v, XrVector3f *out)
{
	// v' = v + 2 w (q x v) + 2 q x (q x v)
	const float tx = 2.0f * (q.y * v.z - q.z * v.y);
	const float ty = 2.0f * (q.z * v.x - q.x * v.z);
	const float tz = 2.0f * (q.x * v.y - q.y * v.x);
	out->x = v.x + q.w * tx + (q.y * tz - q.z * ty);
	out->y = v.y + q.w * ty + (q.z * tx - q.x * tz);
	out->z = v.z + q.w * tz + (q.x * ty - q.y * tx);
}

XrQuaternionf
quat_conj(const XrQuaternionf &q)
{
	return XrQuaternionf{-q.x, -q.y, -q.z, q.w};
}

XrQuaternionf
quat_mul(const XrQuaternionf &a, const XrQuaternionf &b)
{
	return XrQuaternionf{
	    a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	    a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
	    a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}

// G (located in LOCAL) -> the rig frame (relative to the rig camera, rig
// units), then OpenXR (right-handed, -Z forward) -> Unity (left-handed, +Z
// forward): position and linear velocity (x, y, -z), quaternion (-x, -y, z, w),
// angular velocity - an axial vector, so the mirror flips its sign too - (-x,
// -y, z). Same convention as the eyes and the hand joints.
void
to_rig_unity(const DxrInputRigFrame &rig, const XrSpaceLocation &loc, const XrSpaceVelocity &vel, DxrInputPose *out)
{
	const XrQuaternionf inv_rq = quat_conj(rig.rig_pose.orientation);
	// The plane P = rig o plane_offset, in LOCAL.
	XrVector3f off_local;
	quat_rotate(rig.rig_pose.orientation, rig.plane_offset, &off_local);
	const XrVector3f plane_pos = {rig.rig_pose.position.x + off_local.x, rig.rig_pose.position.y + off_local.y,
	                              rig.rig_pose.position.z + off_local.z};
	// rel = inv(P) o G (P shares the rig's orientation).
	const XrVector3f d = {loc.pose.position.x - plane_pos.x, loc.pose.position.y - plane_pos.y,
	                      loc.pose.position.z - plane_pos.z};
	XrVector3f rel;
	quat_rotate(inv_rq, d, &rel);
	const float k = rig.scale > 0.0f ? rig.scale : 1.0f;
	const XrVector3f p = {rig.plane_offset.x + k * rel.x, rig.plane_offset.y + k * rel.y,
	                      rig.plane_offset.z + k * rel.z};
	const XrQuaternionf q = quat_mul(inv_rq, loc.pose.orientation);

	out->position[0] = p.x;
	out->position[1] = p.y;
	out->position[2] = -p.z;
	out->rotation[0] = -q.x;
	out->rotation[1] = -q.y;
	out->rotation[2] = q.z;
	out->rotation[3] = q.w;
	out->flags = (uint64_t)loc.locationFlags;

	XrVector3f lv = {0, 0, 0}, av = {0, 0, 0};
	if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
		quat_rotate(inv_rq, vel.linearVelocity, &lv);
		lv.x *= k;
		lv.y *= k;
		lv.z *= k;
	}
	if (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
		quat_rotate(inv_rq, vel.angularVelocity, &av);
	out->linear_velocity[0] = lv.x;
	out->linear_velocity[1] = lv.y;
	out->linear_velocity[2] = -lv.z;
	out->angular_velocity[0] = -av.x;
	out->angular_velocity[1] = -av.y;
	out->angular_velocity[2] = av.z;
	out->vel_flags = (uint64_t)vel.velocityFlags;
}

// ---- Instance setup ----

XrPath
path(const char *s)
{
	XrPath p = XR_NULL_PATH;
	if (pfn_string_to_path == nullptr || XR_FAILED(pfn_string_to_path(g_instance, s, &p)))
		return XR_NULL_PATH;
	return p;
}

bool
create_action(Act a, const char *name, const char *localized, XrActionType type)
{
	XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};
	strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
	strncpy(ci.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
	ci.actionType = type;
	ci.countSubactionPaths = 2;
	ci.subactionPaths = g_hand_path;
	XrResult r = pfn_create_action(g_set, &ci, &g_act[a]);
	if (XR_FAILED(r)) {
		INPUT_LOG("[DisplayXR-PROV] input: xrCreateAction(%s) failed: %d\n", name, (int)r);
		g_act[a] = XR_NULL_HANDLE;
		return false;
	}
	return true;
}

struct Binding
{
	Act act;
	const char *component; // appended to /user/hand/{left,right}
};

// Suggest @p n bindings for both hands under @p profile. Returns true when
// accepted.
bool
suggest(XrPath profile, const char *profile_name, const Binding *b, int n)
{
	XrActionSuggestedBinding sb[2 * 16];
	uint32_t count = 0;
	for (int h = 0; h < 2; h++) {
		for (int i = 0; i < n && count < 2 * 16; i++) {
			if (g_act[b[i].act] == XR_NULL_HANDLE)
				continue;
			char full[XR_MAX_PATH_LENGTH];
			snprintf(full, sizeof(full), "/user/hand/%s%s", h == 0 ? "left" : "right", b[i].component);
			XrPath p = path(full);
			if (p == XR_NULL_PATH)
				continue;
			sb[count].action = g_act[b[i].act];
			sb[count].binding = p;
			count++;
		}
	}
	XrInteractionProfileSuggestedBinding s = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	s.interactionProfile = profile;
	s.countSuggestedBindings = count;
	s.suggestedBindings = sb;
	XrResult r = pfn_suggest(g_instance, &s);
	INPUT_LOG("[DisplayXR-PROV] input: suggested %u bindings for %s: %s (%d)\n", count, profile_name,
	          XR_SUCCEEDED(r) ? "accepted" : "REJECTED", (int)r);
	return XR_SUCCEEDED(r);
}

void
destroy_locked()
{
	for (int h = 0; h < 2; h++) {
		for (int p = 0; p < DXR_INPUT_POSE_COUNT; p++) {
			if (g_space[h][p] != XR_NULL_HANDLE && pfn_destroy_space)
				pfn_destroy_space(g_space[h][p]);
			g_space[h][p] = XR_NULL_HANDLE;
		}
		g_state[h] = DxrInputHandState{};
		g_profile_logged[h] = -1;
	}
	// Destroying the set destroys its actions.
	if (g_set != XR_NULL_HANDLE && pfn_destroy_action_set)
		pfn_destroy_action_set(g_set);
	g_set = XR_NULL_HANDLE;
	memset(g_act, 0, sizeof(g_act));
	g_session = XR_NULL_HANDLE;
	g_base = XR_NULL_HANDLE;
	g_attached = 0;
	g_sync_failed_logged = 0;
	g_ticks = 0;
}

XrPath
current_profile_locked(int hand)
{
	if (pfn_current_profile == nullptr)
		return XR_NULL_PATH;
	XrInteractionProfileState st = {XR_TYPE_INTERACTION_PROFILE_STATE};
	if (XR_FAILED(pfn_current_profile(g_session, g_hand_path[hand], &st)))
		return XR_NULL_PATH;
	return st.interactionProfile;
}

int
read_bool_locked(Act a, int hand)
{
	if (g_act[a] == XR_NULL_HANDLE)
		return 0;
	XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
	gi.action = g_act[a];
	gi.subactionPath = g_hand_path[hand];
	XrActionStateBoolean s = {XR_TYPE_ACTION_STATE_BOOLEAN};
	if (XR_FAILED(pfn_get_bool(g_session, &gi, &s)) || !s.isActive)
		return 0;
	return s.currentState ? 1 : 0;
}

float
read_float_locked(Act a, int hand)
{
	if (g_act[a] == XR_NULL_HANDLE)
		return 0.0f;
	XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
	gi.action = g_act[a];
	gi.subactionPath = g_hand_path[hand];
	XrActionStateFloat s = {XR_TYPE_ACTION_STATE_FLOAT};
	if (XR_FAILED(pfn_get_float(g_session, &gi, &s)) || !s.isActive)
		return 0.0f;
	return s.currentState;
}

int
pose_active_locked(Act a, int hand)
{
	if (g_act[a] == XR_NULL_HANDLE)
		return 0;
	XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
	gi.action = g_act[a];
	gi.subactionPath = g_hand_path[hand];
	XrActionStatePose s = {XR_TYPE_ACTION_STATE_POSE};
	if (XR_FAILED(pfn_get_pose(g_session, &gi, &s)))
		return 0;
	return s.isActive ? 1 : 0;
}

void
locate_locked(int hand, XrTime t, const DxrInputRigFrame &rig)
{
	static const Act pose_act[DXR_INPUT_POSE_COUNT] = {A_GRIP_POSE, A_AIM_POSE, A_TIP_POSE};
	for (int p = 0; p < DXR_INPUT_POSE_COUNT; p++) {
		DxrInputPose &out = g_state[hand].pose[p];
		if (g_space[hand][p] == XR_NULL_HANDLE) {
			out = DxrInputPose{};
			continue;
		}
		XrSpaceVelocity vel = {XR_TYPE_SPACE_VELOCITY};
		XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
		loc.next = &vel;
		if (XR_FAILED(pfn_locate_space(g_space[hand][p], g_base, t, &loc))) {
			out.flags = 0;
			out.vel_flags = 0;
			out.active = 0;
			continue;
		}
		to_rig_unity(rig, loc, vel, &out);
		out.active = pose_active_locked(pose_act[p], hand);
	}
}

} // namespace

extern "C" {

uint32_t
dxr_input_add_instance_extensions(const char **names, uint32_t count, int advertised_mx_ink)
{
	if (!g_app_enabled || env_disabled())
		return count;
	// An interaction-profile extension: enabling it only makes its profile path
	// legal for xrSuggestInteractionProfileBindings. Gated on the runtime
	// advertising it, so a runtime without the profile still creates the
	// instance.
	if (advertised_mx_ink)
		names[count++] = DXR_INPUT_MX_INK_EXTENSION_NAME;
	return count;
}

void
dxr_input_on_instance(XrInstance instance, PFN_xrGetInstanceProcAddr gipa, int mx_ink_enabled, DxrInputLogFn log)
{
	std::lock_guard<std::mutex> lk(g_mx);
	g_log = log;
	destroy_locked();
	g_instance = XR_NULL_HANDLE;
	g_mx_ink = 0;
	g_simple = 0;
	g_profile_dirty.store(1);
	if (!g_app_enabled || env_disabled()) {
		INPUT_LOG(
		    "[DisplayXR-PROV] input: disabled (DisplayXRInput.Enabled / "
		    "DISPLAYXR_INPUT=0) - no action set\n");
		return;
	}
	if (instance == XR_NULL_HANDLE || gipa == nullptr)
		return;
	g_instance = instance;

#define RESOLVE(var, name)                                                                                             \
	do {                                                                                                           \
		PFN_xrVoidFunction fn = nullptr;                                                                       \
		gipa(instance, name, &fn);                                                                             \
		var = (decltype(var))fn;                                                                               \
	} while (0)
	RESOLVE(pfn_string_to_path, "xrStringToPath");
	RESOLVE(pfn_create_action_set, "xrCreateActionSet");
	RESOLVE(pfn_destroy_action_set, "xrDestroyActionSet");
	RESOLVE(pfn_create_action, "xrCreateAction");
	RESOLVE(pfn_suggest, "xrSuggestInteractionProfileBindings");
	RESOLVE(pfn_attach, "xrAttachSessionActionSets");
	RESOLVE(pfn_sync, "xrSyncActions");
	RESOLVE(pfn_get_bool, "xrGetActionStateBoolean");
	RESOLVE(pfn_get_float, "xrGetActionStateFloat");
	RESOLVE(pfn_get_pose, "xrGetActionStatePose");
	RESOLVE(pfn_create_action_space, "xrCreateActionSpace");
	RESOLVE(pfn_destroy_space, "xrDestroySpace");
	RESOLVE(pfn_locate_space, "xrLocateSpace");
	RESOLVE(pfn_apply_haptic, "xrApplyHapticFeedback");
	RESOLVE(pfn_stop_haptic, "xrStopHapticFeedback");
	RESOLVE(pfn_current_profile, "xrGetCurrentInteractionProfile");
#undef RESOLVE
	if (!pfn_string_to_path || !pfn_create_action_set || !pfn_destroy_action_set || !pfn_create_action ||
	    !pfn_suggest || !pfn_attach || !pfn_sync || !pfn_get_bool || !pfn_get_float || !pfn_get_pose ||
	    !pfn_create_action_space || !pfn_destroy_space || !pfn_locate_space || !pfn_current_profile) {
		INPUT_LOG(
		    "[DisplayXR-PROV] input: core action entry points unresolved - "
		    "controllers stay inactive\n");
		g_instance = XR_NULL_HANDLE;
		return;
	}

	g_hand_path[0] = path("/user/hand/left");
	g_hand_path[1] = path("/user/hand/right");
	g_profile_simple = path("/interaction_profiles/khr/simple_controller");
	g_profile_mx_ink =
	    mx_ink_enabled ? path("/interaction_profiles/logitech/mx_ink_stylus_logitech") : XR_NULL_PATH;

	XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
	strncpy(asci.actionSetName, "displayxr_input", XR_MAX_ACTION_SET_NAME_SIZE - 1);
	strncpy(asci.localizedActionSetName, "DisplayXR Input", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
	asci.priority = 0;
	XrResult r = pfn_create_action_set(instance, &asci, &g_set);
	if (XR_FAILED(r)) {
		INPUT_LOG(
		    "[DisplayXR-PROV] input: xrCreateActionSet failed: %d - "
		    "controllers stay inactive\n",
		    (int)r);
		g_set = XR_NULL_HANDLE;
		g_instance = XR_NULL_HANDLE;
		return;
	}
	create_action(A_GRIP_POSE, "grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT);
	create_action(A_AIM_POSE, "aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT);
	create_action(A_SELECT, "select", "Select", XR_ACTION_TYPE_BOOLEAN_INPUT);
	create_action(A_MENU, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
	create_action(A_HAPTIC, "haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);
	if (g_profile_mx_ink != XR_NULL_PATH) {
		create_action(A_TIP_POSE, "tip_pose", "Tip Pose", XR_ACTION_TYPE_POSE_INPUT);
		create_action(A_TIP_FORCE, "tip_force", "Tip Force", XR_ACTION_TYPE_FLOAT_INPUT);
		create_action(A_MIDDLE_FORCE, "middle_force", "Middle Force", XR_ACTION_TYPE_FLOAT_INPUT);
		create_action(A_FRONT_CLICK, "front_click", "Front Click", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create_action(A_FRONT_DTAP, "front_double_tap", "Front Double Tap", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create_action(A_BACK_CLICK, "back_click", "Back Click", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create_action(A_BACK_DTAP, "back_double_tap", "Back Double Tap", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create_action(A_DOCKED, "docked", "Docked", XR_ACTION_TYPE_BOOLEAN_INPUT);
	}

	static const Binding simple[] = {
	    {A_GRIP_POSE, "/input/grip/pose"}, {A_AIM_POSE, "/input/aim/pose"}, {A_SELECT, "/input/select/click"},
	    {A_MENU, "/input/menu/click"},     {A_HAPTIC, "/output/haptic"},
	};
	g_simple = suggest(g_profile_simple, "khr/simple_controller", simple, (int)(sizeof(simple) / sizeof(simple[0])))
	               ? 1
	               : 0;
	if (g_profile_mx_ink != XR_NULL_PATH) {
		static const Binding mx_ink[] = {
		    {A_GRIP_POSE, "/input/grip/pose"},
		    {A_AIM_POSE, "/input/aim/pose"},
		    {A_TIP_POSE, "/input/tip_logitech/pose"},
		    {A_TIP_FORCE, "/input/tip_logitech/force"},
		    {A_MIDDLE_FORCE, "/input/cluster_middle_logitech/force"},
		    {A_FRONT_CLICK, "/input/cluster_front_logitech/click"},
		    {A_FRONT_DTAP, "/input/cluster_front_logitech/double_tap_logitech"},
		    {A_BACK_CLICK, "/input/cluster_back_logitech/click"},
		    {A_BACK_DTAP, "/input/cluster_back_logitech/double_tap_logitech"},
		    {A_DOCKED, "/input/dock_logitech/docked_logitech"},
		    {A_HAPTIC, "/output/haptic"},
		};
		g_mx_ink = suggest(g_profile_mx_ink, "logitech/mx_ink_stylus_logitech", mx_ink,
		                   (int)(sizeof(mx_ink) / sizeof(mx_ink[0])))
		               ? 1
		               : 0;
	} else {
		INPUT_LOG(
		    "[DisplayXR-PROV] input: %s not advertised - stylus profile not "
		    "suggested "
		    "(simple_controller only)\n",
		    DXR_INPUT_MX_INK_EXTENSION_NAME);
	}
}

void
dxr_input_on_session_created(XrSession session, XrSpace base_space)
{
	std::lock_guard<std::mutex> lk(g_mx);
	if (g_set == XR_NULL_HANDLE || session == XR_NULL_HANDLE || base_space == XR_NULL_HANDLE)
		return;
	g_session = session;
	g_base = base_space;
	static const Act pose_act[DXR_INPUT_POSE_COUNT] = {A_GRIP_POSE, A_AIM_POSE, A_TIP_POSE};
	for (int h = 0; h < 2; h++) {
		for (int p = 0; p < DXR_INPUT_POSE_COUNT; p++) {
			if (g_act[pose_act[p]] == XR_NULL_HANDLE)
				continue;
			XrActionSpaceCreateInfo ci = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
			ci.action = g_act[pose_act[p]];
			ci.subactionPath = g_hand_path[h];
			ci.poseInActionSpace.orientation = XrQuaternionf{0, 0, 0, 1};
			XrResult r = pfn_create_action_space(session, &ci, &g_space[h][p]);
			if (XR_FAILED(r)) {
				INPUT_LOG(
				    "[DisplayXR-PROV] input: xrCreateActionSpace(%s pose %d) "
				    "failed: %d\n",
				    hand_name(h), p, (int)r);
				g_space[h][p] = XR_NULL_HANDLE;
			}
		}
	}
	XrSessionActionSetsAttachInfo ai = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	ai.countActionSets = 1;
	ai.actionSets = &g_set;
	XrResult r = pfn_attach(session, &ai);
	g_attached = XR_SUCCEEDED(r) ? 1 : 0;
	g_profile_dirty.store(1);
	INPUT_LOG(
	    "[DisplayXR-PROV] input: action set %s (%d; profiles: "
	    "simple_controller %s, stylus %s)\n",
	    g_attached ? "ATTACHED" : "attach FAILED", (int)r, g_simple ? "yes" : "no", g_mx_ink ? "yes" : "no");
}

void
dxr_input_on_session_destroying(void)
{
	std::lock_guard<std::mutex> lk(g_mx);
	destroy_locked();
	g_instance = XR_NULL_HANDLE;
	g_mx_ink = 0;
	g_simple = 0;
	g_frame_time.store(0);
	std::lock_guard<std::mutex> rk(g_rig_mx);
	g_rig = DxrInputRigFrame{{{0, 0, 0, 1}, {0, 0, 0}}, {0, 0, 0}, 1.0f, 0};
}

void
dxr_input_on_frame_begun(XrTime predicted_display_time)
{
	g_frame_time.store((int64_t)predicted_display_time);
}

void
dxr_input_on_views_located(const DxrInputRigFrame *frame)
{
	if (frame == nullptr)
		return;
	std::lock_guard<std::mutex> rk(g_rig_mx);
	g_rig = *frame;
}

void
dxr_input_on_interaction_profile_changed(void)
{
	g_profile_dirty.store(1);
}

int
dxr_input_update(int before_render)
{
	std::lock_guard<std::mutex> lk(g_mx);
	if (!g_attached || g_session == XR_NULL_HANDLE)
		return 0;
	const XrTime t = (XrTime)g_frame_time.load();
	if (t <= 0)
		return 0; // no frame begun yet: the session is not running

	DxrInputRigFrame rig;
	{
		std::lock_guard<std::mutex> rk(g_rig_mx);
		rig = g_rig;
	}

	if (!before_render) {
		XrActiveActionSet active = {g_set, XR_NULL_PATH};
		XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
		si.countActiveActionSets = 1;
		si.activeActionSets = &active;
		XrResult r = pfn_sync(g_session, &si);
		if (XR_FAILED(r) && !g_sync_failed_logged) {
			g_sync_failed_logged = 1;
			INPUT_LOG("[DisplayXR-PROV] input: xrSyncActions failed: %d (logged once)\n", (int)r);
		}

		// The current profile follows the runtime's binding, which a hot-plug or a
		// role arbitration change moves (it signals
		// XrEventDataInteractionProfileChanged). Re-query on the event, and every
		// 30 updates as a safety net.
		const int dirty = g_profile_dirty.exchange(0);
		if (dirty || (g_ticks++ % 30u) == 0u) {
			for (int h = 0; h < 2; h++) {
				const XrPath prof = current_profile_locked(h);
				int kind = DXR_INPUT_KIND_NONE;
				if (prof != XR_NULL_PATH && prof == g_profile_mx_ink)
					kind = DXR_INPUT_KIND_STYLUS;
				else if (prof != XR_NULL_PATH && prof == g_profile_simple)
					kind = DXR_INPUT_KIND_SIMPLE;
				g_state[h].kind = kind;
				if (g_profile_logged[h] != kind) {
					g_profile_logged[h] = kind;
					INPUT_LOG("[DisplayXR-PROV] input: %s hand -> %s\n", hand_name(h),
					          kind_name(kind));
				}
			}
		}

		for (int h = 0; h < 2; h++) {
			DxrInputHandState &s = g_state[h];
			s.select_click = read_bool_locked(A_SELECT, h);
			s.menu_click = read_bool_locked(A_MENU, h);
			s.tip_force = read_float_locked(A_TIP_FORCE, h);
			s.middle_force = read_float_locked(A_MIDDLE_FORCE, h);
			s.front_click = read_bool_locked(A_FRONT_CLICK, h);
			s.front_double_tap = read_bool_locked(A_FRONT_DTAP, h);
			s.back_click = read_bool_locked(A_BACK_CLICK, h);
			s.back_double_tap = read_bool_locked(A_BACK_DTAP, h);
			s.docked = read_bool_locked(A_DOCKED, h);
		}
	}

	for (int h = 0; h < 2; h++)
		locate_locked(h, t, rig);
	return 1;
}

int
dxr_input_get_hand_state(int hand, DxrInputHandState *out)
{
	if (hand < 0 || hand > 1)
		return DXR_INPUT_KIND_NONE;
	std::lock_guard<std::mutex> lk(g_mx);
	if (out)
		*out = g_state[hand];
	return g_state[hand].kind;
}

int
dxr_input_haptic(int hand, float amplitude, float duration_s)
{
	if (hand < 0 || hand > 1)
		return 0;
	std::lock_guard<std::mutex> lk(g_mx);
	if (!g_attached || g_act[A_HAPTIC] == XR_NULL_HANDLE || pfn_apply_haptic == nullptr)
		return 0;
	XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};
	hi.action = g_act[A_HAPTIC];
	hi.subactionPath = g_hand_path[hand];
	XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
	v.amplitude = amplitude < 0.0f ? 0.0f : (amplitude > 1.0f ? 1.0f : amplitude);
	v.duration = duration_s > 0.0f ? (XrDuration)((double)duration_s * 1e9 + 0.5) : XR_MIN_HAPTIC_DURATION;
	v.frequency = XR_FREQUENCY_UNSPECIFIED;
	return XR_SUCCEEDED(pfn_apply_haptic(g_session, &hi, (const XrHapticBaseHeader *)&v)) ? 1 : 0;
}

void
dxr_input_haptic_stop(int hand)
{
	if (hand < 0 || hand > 1)
		return;
	std::lock_guard<std::mutex> lk(g_mx);
	if (!g_attached || g_act[A_HAPTIC] == XR_NULL_HANDLE || pfn_stop_haptic == nullptr)
		return;
	XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};
	hi.action = g_act[A_HAPTIC];
	hi.subactionPath = g_hand_path[hand];
	pfn_stop_haptic(g_session, &hi);
}

void
DisplayXR_Input_SetEnabled(int enabled)
{
	std::lock_guard<std::mutex> lk(g_mx);
	g_app_enabled = enabled ? 1 : 0;
}

int
DisplayXR_Input_IsActive(void)
{
	std::lock_guard<std::mutex> lk(g_mx);
	return g_attached;
}

int
DisplayXR_Input_IsStylusProfileAvailable(void)
{
	std::lock_guard<std::mutex> lk(g_mx);
	return g_mx_ink;
}

int
DisplayXR_Input_GetHandKind(int hand)
{
	if (hand < 0 || hand > 1)
		return DXR_INPUT_KIND_NONE;
	std::lock_guard<std::mutex> lk(g_mx);
	return g_state[hand].kind;
}

int
DisplayXR_Input_RigKind(void)
{
	std::lock_guard<std::mutex> rk(g_rig_mx);
	return g_rig.kind;
}

} // extern "C"
