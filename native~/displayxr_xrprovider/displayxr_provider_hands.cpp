// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// XR_EXT_hand_tracking binding — see displayxr_provider_hands.h for the threading
// model, lifetime and frame conventions.

#include "displayxr_provider_hands.h"

#include <atomic>
#include <mutex>

#include <string.h>

namespace {

// One lock for everything below. Taken by the graphics thread (session hooks) and
// the main thread (C# calls). xrLocateHandJointsEXT runs UNDER it: a locate is a
// few microseconds, and holding the lock across it is what guarantees the session
// teardown (which takes the same lock before destroying the trackers) never pulls
// a tracker out from under a locate in flight.
std::mutex g_mx;

DxrHandsLogFn g_log = nullptr;
int g_ext_enabled = 0;      // XR_EXT_hand_tracking enabled on the instance + PFNs resolved
int g_system_supported = 0; // XrSystemHandTrackingPropertiesEXT.supportsHandTracking
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_space = XR_NULL_HANDLE; // the provider's LOCAL space — the views' space

int g_requested = 0;        // the app called Start (and not yet Stop); outlives the session
XrHandTrackerEXT g_tracker[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
int g_create_failed[2] = {0, 0};        // don't retry a failed create every frame
int g_locate_failed_logged[2] = {0, 0}; // one log line per hand per session
int g_first_active_logged[2] = {0, 0};  // one "hand seen" line per hand per session

// Written on the graphics thread once per begun frame, read on the main thread.
std::atomic<int64_t> g_frame_time{0};

PFN_xrCreateHandTrackerEXT pfn_create = nullptr;
PFN_xrDestroyHandTrackerEXT pfn_destroy = nullptr;
PFN_xrLocateHandJointsEXT pfn_locate = nullptr;

#define HANDS_LOG(...)                                                                                                 \
	do {                                                                                                               \
		if (g_log) g_log(__VA_ARGS__);                                                                                 \
	} while (0)

const char *
hand_name(int hand)
{
	return hand == 0 ? "left" : "right";
}

bool
supported_locked()
{
	return g_session != XR_NULL_HANDLE && g_space != XR_NULL_HANDLE && g_ext_enabled && g_system_supported &&
	       pfn_create && pfn_destroy && pfn_locate;
}

void
destroy_trackers_locked()
{
	for (int h = 0; h < 2; h++) {
		if (g_tracker[h] != XR_NULL_HANDLE && pfn_destroy) pfn_destroy(g_tracker[h]);
		g_tracker[h] = XR_NULL_HANDLE;
		g_create_failed[h] = 0;
		g_locate_failed_logged[h] = 0;
		g_first_active_logged[h] = 0;
	}
}

bool
ensure_tracker_locked(int hand)
{
	if (g_tracker[hand] != XR_NULL_HANDLE) return true;
	if (g_create_failed[hand] || !supported_locked()) return false;

	XrHandTrackerCreateInfoEXT ci = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
	ci.hand = hand == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
	ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
	XrHandTrackerEXT tracker = XR_NULL_HANDLE;
	XrResult r = pfn_create(g_session, &ci, &tracker);
	if (XR_FAILED(r) || tracker == XR_NULL_HANDLE) {
		g_create_failed[hand] = 1;
		HANDS_LOG("[DisplayXR-PROV] hand tracking: xrCreateHandTrackerEXT(%s) failed: %d\n", hand_name(hand), (int)r);
		return false;
	}
	g_tracker[hand] = tracker;
	HANDS_LOG("[DisplayXR-PROV] hand tracking: %s tracker created (%d joints, base space LOCAL)\n", hand_name(hand),
	          DISPLAYXR_HAND_JOINT_COUNT);
	return true;
}

} // namespace

extern "C" {

void
dxr_hands_on_instance(XrInstance instance, PFN_xrGetInstanceProcAddr gipa, int enabled, DxrHandsLogFn log)
{
	std::lock_guard<std::mutex> lk(g_mx);
	g_log = log;
	g_ext_enabled = 0;
	pfn_create = nullptr;
	pfn_destroy = nullptr;
	pfn_locate = nullptr;
	if (!enabled || !gipa || instance == XR_NULL_HANDLE) {
		HANDS_LOG("[DisplayXR-PROV] XR_EXT_hand_tracking: not advertised by this runtime — "
		          "XR Hands stays inactive (needs an input provider with hand roles)\n");
		return;
	}
	PFN_xrVoidFunction fn = nullptr;
	gipa(instance, "xrCreateHandTrackerEXT", &fn);
	pfn_create = (PFN_xrCreateHandTrackerEXT)fn;
	fn = nullptr;
	gipa(instance, "xrDestroyHandTrackerEXT", &fn);
	pfn_destroy = (PFN_xrDestroyHandTrackerEXT)fn;
	fn = nullptr;
	gipa(instance, "xrLocateHandJointsEXT", &fn);
	pfn_locate = (PFN_xrLocateHandJointsEXT)fn;
	if (!pfn_create || !pfn_destroy || !pfn_locate) {
		HANDS_LOG("[DisplayXR-PROV] XR_EXT_hand_tracking: enabled but entry points unresolved "
		          "(create=%p destroy=%p locate=%p) — XR Hands stays inactive\n",
		          (void *)pfn_create, (void *)pfn_destroy, (void *)pfn_locate);
		pfn_create = nullptr;
		pfn_destroy = nullptr;
		pfn_locate = nullptr;
		return;
	}
	g_ext_enabled = 1;
	HANDS_LOG("[DisplayXR-PROV] XR_EXT_hand_tracking: AVAILABLE (enabled, entry points resolved)\n");
}

void
dxr_hands_on_session_created(XrSession session, XrSpace base_space, int system_supported)
{
	std::lock_guard<std::mutex> lk(g_mx);
	g_session = session;
	g_space = base_space;
	g_system_supported = system_supported ? 1 : 0;
	HANDS_LOG("[DisplayXR-PROV] hand tracking: %s (extension %s, system supportsHandTracking=%d, "
	          "app requested=%d)\n",
	          supported_locked() ? "AVAILABLE" : "unavailable", g_ext_enabled ? "enabled" : "absent",
	          g_system_supported, g_requested);
	// The app may have asked before the session existed (XR Hands starts with the
	// loader, the session a frame or two later): create now so the first locate is
	// not the one paying for it.
	if (g_requested) {
		ensure_tracker_locked(0);
		ensure_tracker_locked(1);
	}
}

void
dxr_hands_on_session_destroying(void)
{
	std::lock_guard<std::mutex> lk(g_mx);
	destroy_trackers_locked();
	g_session = XR_NULL_HANDLE;
	g_space = XR_NULL_HANDLE;
	g_system_supported = 0;
	g_frame_time.store(0);
}

void
dxr_hands_on_frame_begun(XrTime predicted_display_time)
{
	g_frame_time.store((int64_t)predicted_display_time);
}

int
DisplayXR_HandTracking_IsSupported(void)
{
	std::lock_guard<std::mutex> lk(g_mx);
	return supported_locked() ? 1 : 0;
}

int
DisplayXR_HandTracking_Start(void)
{
	std::lock_guard<std::mutex> lk(g_mx);
	g_requested = 1;
	if (!supported_locked()) return 0; // remembered; trackers follow the next session
	ensure_tracker_locked(0);
	ensure_tracker_locked(1);
	return 1;
}

void
DisplayXR_HandTracking_Stop(void)
{
	std::lock_guard<std::mutex> lk(g_mx);
	g_requested = 0;
	destroy_trackers_locked();
}

int
DisplayXR_HandTracking_Locate(int hand, int64_t time, float *out_poses, float *out_radii, uint64_t *out_flags)
{
	if (hand < 0 || hand > 1 || out_poses == nullptr) return 0;
	std::lock_guard<std::mutex> lk(g_mx);
	if (!g_requested || !ensure_tracker_locked(hand)) return 0;

	const XrTime t = time > 0 ? (XrTime)time : (XrTime)g_frame_time.load();
	if (t <= 0) return 0; // no frame begun yet: nothing to locate against

	XrHandJointLocationEXT joints[DISPLAYXR_HAND_JOINT_COUNT];
	memset(joints, 0, sizeof(joints));
	XrHandJointLocationsEXT locs = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
	locs.jointCount = DISPLAYXR_HAND_JOINT_COUNT;
	locs.jointLocations = joints;

	XrHandJointsLocateInfoEXT li = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
	li.baseSpace = g_space;
	li.time = t;

	XrResult r = pfn_locate(g_tracker[hand], &li, &locs);
	if (XR_FAILED(r)) {
		if (!g_locate_failed_logged[hand]) {
			g_locate_failed_logged[hand] = 1;
			HANDS_LOG("[DisplayXR-PROV] hand tracking: xrLocateHandJointsEXT(%s) failed: %d (logged once)\n",
			          hand_name(hand), (int)r);
		}
		return 0;
	}

	for (int j = 0; j < DISPLAYXR_HAND_JOINT_COUNT; j++) {
		const XrPosef &p = joints[j].pose;
		float *o = out_poses + j * 7;
		// OpenXR (right-handed, -Z forward) -> Unity (left-handed, +Z forward): mirror
		// about the XY plane. Same convention as oxr_to_unity_pos/quat for the eyes
		// (position z -> -z; quaternion (x,y,z,w) -> (-x,-y,z,w), which equals
		// (x,y,-z,-w)). It also maps the joint axes: OpenXR's -Z "along the bone"
		// becomes Unity's +Z, +Y (back of the hand) stays +Y — XR Hands' convention.
		o[0] = p.position.x;
		o[1] = p.position.y;
		o[2] = -p.position.z;
		o[3] = -p.orientation.x;
		o[4] = -p.orientation.y;
		o[5] = p.orientation.z;
		o[6] = p.orientation.w;
		if (out_radii) out_radii[j] = joints[j].radius;
		if (out_flags) out_flags[j] = (uint64_t)joints[j].locationFlags;
	}

	const int active = locs.isActive == XR_TRUE ? 1 : 0;
	if (active && !g_first_active_logged[hand]) {
		g_first_active_logged[hand] = 1;
		const XrPosef &wrist = joints[XR_HAND_JOINT_WRIST_EXT].pose;
		HANDS_LOG("[DisplayXR-PROV] hand tracking: %s hand first seen (wrist LOCAL %.3f,%.3f,%.3f m; "
		          "flags=0x%llx)\n",
		          hand_name(hand), wrist.position.x, wrist.position.y, wrist.position.z,
		          (unsigned long long)joints[XR_HAND_JOINT_WRIST_EXT].locationFlags);
	}
	return active;
}

} // extern "C"
