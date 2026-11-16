// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit test of the controller / stylus action binding (displayxr_provider_input.cpp)
// against a fake runtime: the stylus profile is suggested only when its extension is
// enabled (and never the system component), simple_controller always; the opt-out
// creates no action set; the bound profile decides each hand's kind; action state is
// read per hand; poses come back relative to the rig camera, as located by default
// and at the rig's scale with the opt-in gain
// (display rig and camera rig); haptics reach xrApplyHapticFeedback clamped; teardown
// destroys every handle. No Unity, no real runtime.
//
//   c++ -std=c++17 -Wall -pthread -I native~/displayxr_xrprovider -I <openxr include>
//       native~/tests/input_binding_test.cpp
//       native~/displayxr_xrprovider/displayxr_provider_input.cpp
//   (one command; see the "Input binding tests" step in build-native.yml)

#include "displayxr_provider_input.h"
#include "displayxr_provider_hands.h" // DXR_PROV_HAND_RIG_*

#include <map>
#include <math.h>
#include <set>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c)                                                                                                       \
	do {                                                                                                           \
		if (!(c)) {                                                                                            \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                   \
			g_fail++;                                                                                      \
		}                                                                                                      \
	} while (0)
#define NEAR(a, b) CHECK(fabs((double)(a) - (double)(b)) < 1e-4)

// ---------------------------------------------------------------- fake runtime

struct FakeAction
{
	std::string name;
	XrActionType type;
};

struct Fake
{
	std::map<std::string, XrPath> paths;
	std::map<XrPath, std::string> names;
	int action_sets_created = 0;
	int action_sets_destroyed = 0;
	std::map<uint64_t, FakeAction> actions;                    // handle -> action
	std::map<std::string, std::vector<std::string>> suggested; // profile -> "action=path"
	int attach_calls = 0;
	int sync_calls = 0;
	int spaces_created = 0;
	int spaces_destroyed = 0;
	std::map<uint64_t, std::pair<uint64_t, XrPath>> spaces; // space -> (action, hand)
	XrPath current_profile[2] = {XR_NULL_PATH, XR_NULL_PATH};
	std::map<std::string, float> state;        // "action|hand" -> value
	std::map<std::string, XrPosef> pose_local; // "action|hand" -> pose in LOCAL
	XrPath haptic_hand = XR_NULL_PATH;
	XrHapticVibration haptic = {};
	int haptic_calls = 0;
	uint64_t next = 100;
} F;

static void
reset_fake()
{
	F = Fake{};
}

static XrPath
fake_path(const std::string &s)
{
	auto it = F.paths.find(s);
	if (it != F.paths.end())
		return it->second;
	XrPath p = (XrPath)(F.paths.size() + 1);
	F.paths[s] = p;
	F.names[p] = s;
	return p;
}

static int
hand_index(XrPath p)
{
	if (p == fake_path("/user/hand/left"))
		return 0;
	if (p == fake_path("/user/hand/right"))
		return 1;
	return -1;
}

static std::string
key(uint64_t action, XrPath hand)
{
	return F.actions[action].name + "|" + (hand_index(hand) == 0 ? "left" : "right");
}

static XRAPI_ATTR XrResult XRAPI_CALL
f_string_to_path(XrInstance, const char *s, XrPath *out)
{
	*out = fake_path(s);
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_create_action_set(XrInstance, const XrActionSetCreateInfo *ci, XrActionSet *out)
{
	CHECK(strcmp(ci->actionSetName, "displayxr_input") == 0);
	F.action_sets_created++;
	*out = (XrActionSet)(uintptr_t)(F.next++);
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_destroy_action_set(XrActionSet)
{
	F.action_sets_destroyed++;
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_create_action(XrActionSet, const XrActionCreateInfo *ci, XrAction *out)
{
	CHECK(ci->countSubactionPaths == 2);
	uint64_t h = F.next++;
	F.actions[h] = FakeAction{ci->actionName, ci->actionType};
	*out = (XrAction)(uintptr_t)h;
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_suggest(XrInstance, const XrInteractionProfileSuggestedBinding *s)
{
	auto &v = F.suggested[F.names[s->interactionProfile]];
	for (uint32_t i = 0; i < s->countSuggestedBindings; i++)
		v.push_back(F.actions[(uint64_t)(uintptr_t)s->suggestedBindings[i].action].name + "=" +
		            F.names[s->suggestedBindings[i].binding]);
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_attach(XrSession, const XrSessionActionSetsAttachInfo *ai)
{
	CHECK(ai->countActionSets == 1);
	F.attach_calls++;
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_sync(XrSession, const XrActionsSyncInfo *si)
{
	CHECK(si->countActiveActionSets == 1);
	F.sync_calls++;
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_get_bool(XrSession, const XrActionStateGetInfo *gi, XrActionStateBoolean *s)
{
	auto it = F.state.find(key((uint64_t)(uintptr_t)gi->action, gi->subactionPath));
	s->isActive = it != F.state.end();
	s->currentState = (s->isActive && it->second != 0.0f) ? XR_TRUE : XR_FALSE;
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_get_float(XrSession, const XrActionStateGetInfo *gi, XrActionStateFloat *s)
{
	auto it = F.state.find(key((uint64_t)(uintptr_t)gi->action, gi->subactionPath));
	s->isActive = it != F.state.end();
	s->currentState = s->isActive ? it->second : 0.0f;
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_get_pose(XrSession, const XrActionStateGetInfo *gi, XrActionStatePose *s)
{
	s->isActive = F.pose_local.count(key((uint64_t)(uintptr_t)gi->action, gi->subactionPath)) ? XR_TRUE : XR_FALSE;
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_create_action_space(XrSession, const XrActionSpaceCreateInfo *ci, XrSpace *out)
{
	uint64_t h = F.next++;
	F.spaces[h] = {(uint64_t)(uintptr_t)ci->action, ci->subactionPath};
	F.spaces_created++;
	*out = (XrSpace)(uintptr_t)h;
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_destroy_space(XrSpace)
{
	F.spaces_destroyed++;
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_locate_space(XrSpace space, XrSpace, XrTime, XrSpaceLocation *loc)
{
	auto sp = F.spaces[(uint64_t)(uintptr_t)space];
	auto it = F.pose_local.find(key(sp.first, sp.second));
	if (it == F.pose_local.end()) {
		loc->locationFlags = 0;
		return XR_SUCCESS;
	}
	loc->pose = it->second;
	loc->locationFlags = 0xF;
	XrSpaceVelocity *v = (XrSpaceVelocity *)loc->next;
	if (v && v->type == XR_TYPE_SPACE_VELOCITY) {
		v->velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
		v->linearVelocity = XrVector3f{0.0f, 0.0f, 1.0f};
	}
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_apply_haptic(XrSession, const XrHapticActionInfo *hi, const XrHapticBaseHeader *h)
{
	F.haptic_calls++;
	F.haptic_hand = hi->subactionPath;
	F.haptic = *(const XrHapticVibration *)h;
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_stop_haptic(XrSession, const XrHapticActionInfo *)
{
	return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL
f_current_profile(XrSession, XrPath user, XrInteractionProfileState *st)
{
	int h = hand_index(user);
	st->interactionProfile = h >= 0 ? F.current_profile[h] : XR_NULL_PATH;
	return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL
fake_gipa(XrInstance, const char *name, PFN_xrVoidFunction *fn)
{
	static const std::map<std::string, PFN_xrVoidFunction> table = {
	    {"xrStringToPath", (PFN_xrVoidFunction)f_string_to_path},
	    {"xrCreateActionSet", (PFN_xrVoidFunction)f_create_action_set},
	    {"xrDestroyActionSet", (PFN_xrVoidFunction)f_destroy_action_set},
	    {"xrCreateAction", (PFN_xrVoidFunction)f_create_action},
	    {"xrSuggestInteractionProfileBindings", (PFN_xrVoidFunction)f_suggest},
	    {"xrAttachSessionActionSets", (PFN_xrVoidFunction)f_attach},
	    {"xrSyncActions", (PFN_xrVoidFunction)f_sync},
	    {"xrGetActionStateBoolean", (PFN_xrVoidFunction)f_get_bool},
	    {"xrGetActionStateFloat", (PFN_xrVoidFunction)f_get_float},
	    {"xrGetActionStatePose", (PFN_xrVoidFunction)f_get_pose},
	    {"xrCreateActionSpace", (PFN_xrVoidFunction)f_create_action_space},
	    {"xrDestroySpace", (PFN_xrVoidFunction)f_destroy_space},
	    {"xrLocateSpace", (PFN_xrVoidFunction)f_locate_space},
	    {"xrApplyHapticFeedback", (PFN_xrVoidFunction)f_apply_haptic},
	    {"xrStopHapticFeedback", (PFN_xrVoidFunction)f_stop_haptic},
	    {"xrGetCurrentInteractionProfile", (PFN_xrVoidFunction)f_current_profile},
	};
	auto it = table.find(name);
	*fn = it == table.end() ? nullptr : it->second;
	return it == table.end() ? XR_ERROR_FUNCTION_UNSUPPORTED : XR_SUCCESS;
}

static void
quiet_log(const char *, ...)
{}

static const XrInstance kInstance = (XrInstance)(uintptr_t)1;
static const XrSession kSession = (XrSession)(uintptr_t)2;
static const XrSpace kLocal = (XrSpace)(uintptr_t)3;

static bool
has(const std::vector<std::string> &v, const std::string &s)
{
	for (auto &x : v)
		if (x == s)
			return true;
	return false;
}

// ---------------------------------------------------------------- tests

static void
test_extensions_and_opt_out()
{
	const char *names[4];
	DisplayXR_Input_SetEnabled(1);
	CHECK(dxr_input_add_instance_extensions(names, 0, 0) == 0);
	CHECK(dxr_input_add_instance_extensions(names, 0, 1) == 1);
	CHECK(strcmp(names[0], "XR_LOGITECH_mx_ink_stylus_interaction") == 0);

	DisplayXR_Input_SetEnabled(0);
	CHECK(dxr_input_add_instance_extensions(names, 0, 1) == 0);
	reset_fake();
	dxr_input_on_instance(kInstance, fake_gipa, 0, quiet_log);
	CHECK(F.action_sets_created == 0);
	dxr_input_on_session_created(kSession, kLocal);
	CHECK(F.attach_calls == 0);
	CHECK(DisplayXR_Input_IsActive() == 0);
	dxr_input_on_session_destroying();
	DisplayXR_Input_SetEnabled(1);
}

static void
test_simple_only_without_stylus_extension()
{
	reset_fake();
	dxr_input_on_instance(kInstance, fake_gipa, 0, quiet_log);
	CHECK(F.action_sets_created == 1);
	CHECK(F.suggested.count("/interaction_profiles/khr/simple_controller") == 1);
	CHECK(F.suggested.count("/interaction_profiles/logitech/mx_ink_stylus_logitech") == 0);
	CHECK(F.paths.count("/interaction_profiles/logitech/mx_ink_stylus_logitech") == 0);
	auto &s = F.suggested["/interaction_profiles/khr/simple_controller"];
	CHECK(s.size() == 10);
	CHECK(has(s, "select=/user/hand/left/input/select/click"));
	CHECK(has(s, "aim_pose=/user/hand/right/input/aim/pose"));
	CHECK(has(s, "haptic=/user/hand/right/output/haptic"));
	dxr_input_on_session_created(kSession, kLocal);
	CHECK(F.spaces_created == 4); // grip + aim per hand, no tip
	CHECK(DisplayXR_Input_IsStylusProfileAvailable() == 0);
	dxr_input_on_session_destroying();
	CHECK(F.spaces_destroyed == 4);
	CHECK(F.action_sets_destroyed == 1);
}

static void
test_stylus_profile_and_state()
{
	reset_fake();
	dxr_input_on_instance(kInstance, fake_gipa, 1, quiet_log);
	auto &m = F.suggested["/interaction_profiles/logitech/mx_ink_stylus_logitech"];
	CHECK(m.size() == 22);
	CHECK(has(m, "tip_force=/user/hand/right/input/tip_logitech/force"));
	CHECK(has(m, "middle_force=/user/hand/right/input/cluster_middle_logitech/force"));
	CHECK(has(m, "front_click=/user/hand/right/input/cluster_front_logitech/click"));
	CHECK(has(m,
	          "front_double_tap=/user/hand/left/input/cluster_front_logitech/"
	          "double_tap_logitech"));
	CHECK(has(m, "back_click=/user/hand/left/input/cluster_back_logitech/click"));
	CHECK(has(m,
	          "back_double_tap=/user/hand/right/input/cluster_back_logitech/"
	          "double_tap_logitech"));
	CHECK(has(m, "docked=/user/hand/right/input/dock_logitech/docked_logitech"));
	CHECK(has(m, "tip_pose=/user/hand/right/input/tip_logitech/pose"));
	CHECK(has(m, "grip_pose=/user/hand/left/input/grip/pose"));
	CHECK(has(m, "haptic=/user/hand/left/output/haptic"));
	for (auto &b : m)
		CHECK(b.find("/input/system/click") == std::string::npos);

	dxr_input_on_session_created(kSession, kLocal);
	CHECK(F.attach_calls == 1);
	CHECK(F.spaces_created == 6);
	CHECK(DisplayXR_Input_IsActive() == 1);
	CHECK(DisplayXR_Input_IsStylusProfileAvailable() == 1);

	// No frame begun yet: the session is not running, nothing is synced.
	CHECK(dxr_input_update(0) == 0);
	CHECK(F.sync_calls == 0);

	F.current_profile[1] = fake_path("/interaction_profiles/logitech/mx_ink_stylus_logitech");
	F.current_profile[0] = fake_path("/interaction_profiles/khr/simple_controller");
	F.state["tip_force|right"] = 0.75f;
	F.state["middle_force|right"] = 0.25f;
	F.state["front_click|right"] = 1.0f;
	F.state["back_double_tap|right"] = 1.0f;
	F.state["docked|right"] = 0.0f;
	F.state["select|left"] = 1.0f;

	// A display rig at (1, 2, 3), no rotation, 2 rig units per canvas metre.
	DxrInputRigFrame rig = {};
	rig.rig_pose = XrPosef{{0, 0, 0, 1}, {1, 2, 3}};
	rig.scale = 2.0f;
	rig.kind = DXR_PROV_HAND_RIG_DISPLAY;
	dxr_input_on_views_located(&rig);
	F.pose_local["tip_pose|right"] = XrPosef{{0, 0, 0, 1}, {1.1f, 2.0f, 3.05f}};
	dxr_input_on_frame_begun(1000);

	// Default (no rig gain): as located, only relative to the rig camera. The
	// runtime already put a glass-registered pen in the rig frame; scaling it
	// here would scale it twice. rel = (0.1, 0, 0.05) -> Unity (0.1, 0, -0.05);
	// velocity (0,0,1) -> Unity (0,0,-1).
	CHECK(dxr_input_update(0) == 1);
	CHECK(F.sync_calls == 1);
	{
		DxrInputHandState d;
		dxr_input_get_hand_state(1, &d);
		NEAR(d.pose[DXR_INPUT_POSE_TIP].position[0], 0.1f);
		NEAR(d.pose[DXR_INPUT_POSE_TIP].position[1], 0.0f);
		NEAR(d.pose[DXR_INPUT_POSE_TIP].position[2], -0.05f);
		NEAR(d.pose[DXR_INPUT_POSE_TIP].linear_velocity[2], -1.0f);
	}

	// Opt-in rig gain: the hand-joint mapping about the plane.
	DisplayXR_Input_SetRigGain(1);
	CHECK(dxr_input_update(1) == 1); // before-render: relocate, no sync
	CHECK(F.sync_calls == 1);
	DxrInputHandState r, l;
	CHECK(dxr_input_get_hand_state(1, &r) == DXR_INPUT_KIND_STYLUS);
	CHECK(dxr_input_get_hand_state(0, &l) == DXR_INPUT_KIND_SIMPLE);
	CHECK(DisplayXR_Input_GetHandKind(1) == DXR_INPUT_KIND_STYLUS);
	NEAR(r.tip_force, 0.75f);
	NEAR(r.middle_force, 0.25f);
	CHECK(r.front_click == 1 && r.back_click == 0);
	CHECK(r.back_double_tap == 1 && r.front_double_tap == 0);
	CHECK(r.docked == 0);
	CHECK(l.select_click == 1 && l.menu_click == 0);
	CHECK(DisplayXR_Input_RigKind() == DXR_PROV_HAND_RIG_DISPLAY);

	// Display rig: rel = (0.1, 0, 0.05) m on the canvas -> x2 -> OpenXR (0.2, 0,
	// 0.1)
	// -> Unity (0.2, 0, -0.1); velocity (0,0,1) -> x2 -> Unity (0,0,-2).
	const DxrInputPose &tip = r.pose[DXR_INPUT_POSE_TIP];
	CHECK(tip.active == 1);
	CHECK(tip.flags == 0xF);
	NEAR(tip.position[0], 0.2f);
	NEAR(tip.position[1], 0.0f);
	NEAR(tip.position[2], -0.1f);
	NEAR(tip.rotation[3], 1.0f);
	NEAR(tip.linear_velocity[2], -2.0f);
	CHECK(r.pose[DXR_INPUT_POSE_GRIP].active == 0);
	CHECK(r.pose[DXR_INPUT_POSE_GRIP].flags == 0);

	// Camera rig yawed +90 deg about +Y at (0, 1, 0); glass 2 in front (OpenXR -Z
	// of the camera), 0.5 rig units per canvas metre. A pose 0.2 m to the
	// camera's right of the glass centre: the camera's +X is LOCAL -Z after the
	// yaw; the glass centre is the camera's -Z = LOCAL -X, i.e. at (-2, 1, 0).
	// Pose = (-2, 1, -0.2) in LOCAL.
	const float s = sqrtf(0.5f);
	rig.rig_pose = XrPosef{{0, s, 0, s}, {0, 1, 0}};
	rig.plane_offset = XrVector3f{0, 0, -2};
	rig.scale = 0.5f;
	rig.kind = DXR_PROV_HAND_RIG_CAMERA;
	dxr_input_on_views_located(&rig);
	F.pose_local["tip_pose|right"] = XrPosef{{0, s, 0, s}, {-2.0f, 1.0f, -0.2f}};
	CHECK(dxr_input_update(1) == 1); // before-render: relocate, no sync
	CHECK(F.sync_calls == 1);
	dxr_input_get_hand_state(1, &r);
	// Rig frame (OpenXR): plane (0,0,-2) + 0.5 * (0.2, 0, 0) = (0.1, 0, -2) ->
	// Unity (0.1, 0, 2); the pose shares the rig's orientation -> identity
	// relative rotation.
	NEAR(r.pose[DXR_INPUT_POSE_TIP].position[0], 0.1f);
	NEAR(r.pose[DXR_INPUT_POSE_TIP].position[1], 0.0f);
	NEAR(r.pose[DXR_INPUT_POSE_TIP].position[2], 2.0f);
	NEAR(fabsf(r.pose[DXR_INPUT_POSE_TIP].rotation[3]), 1.0f);

	// The same camera rig without the gain: relative to the camera, unscaled:
	// OpenXR (0.2, 0, -2) -> Unity (0.2, 0, 2).
	DisplayXR_Input_SetRigGain(0);
	CHECK(dxr_input_update(1) == 1);
	dxr_input_get_hand_state(1, &r);
	NEAR(r.pose[DXR_INPUT_POSE_TIP].position[0], 0.2f);
	NEAR(r.pose[DXR_INPUT_POSE_TIP].position[1], 0.0f);
	NEAR(r.pose[DXR_INPUT_POSE_TIP].position[2], 2.0f);

	// Haptics: clamped amplitude, seconds -> nanoseconds, on the right subaction
	// path.
	CHECK(dxr_input_haptic(1, 1.5f, 0.02f) == 1);
	CHECK(F.haptic_calls == 1);
	CHECK(F.haptic_hand == fake_path("/user/hand/right"));
	NEAR(F.haptic.amplitude, 1.0f);
	CHECK(F.haptic.duration == 20000000);
	CHECK(F.haptic.frequency == XR_FREQUENCY_UNSPECIFIED);
	CHECK(dxr_input_haptic(0, 0.5f, 0.0f) == 1);
	CHECK(F.haptic.duration == XR_MIN_HAPTIC_DURATION);

	// The hand loses its profile: kind NONE after the next (event-driven)
	// re-query.
	F.current_profile[1] = XR_NULL_PATH;
	dxr_input_on_interaction_profile_changed();
	dxr_input_update(0);
	CHECK(dxr_input_get_hand_state(1, nullptr) == DXR_INPUT_KIND_NONE);

	dxr_input_on_session_destroying();
	CHECK(F.spaces_destroyed == 6);
	CHECK(F.action_sets_destroyed == 1);
	CHECK(DisplayXR_Input_IsActive() == 0);
	CHECK(dxr_input_update(0) == 0);
	CHECK(dxr_input_haptic(1, 1.0f, 0.1f) == 0);
}

int
main()
{
	test_extensions_and_opt_out();
	test_simple_only_without_stylus_extension();
	test_stylus_profile_and_state();
	if (g_fail) {
		fprintf(stderr, "input_binding_test: %d FAILED\n", g_fail);
		return 1;
	}
	printf("input_binding_test: all passed\n");
	return 0;
}
