// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// The Unity-facing half of controller / stylus input: an IUnityXRInput provider
// ("DisplayXR Input", Runtime/UnitySubsystemsManifest.json) that turns the OpenXR
// action state of displayxr_provider_input.cpp into UnityEngine.XR InputDevices.
//
// One device per hand (/user/hand/left, /user/hand/right), connected while the runtime
// binds an interaction profile to that hand and re-created (new device id) when the
// bound profile changes, because a device's feature list is fixed at connection:
//
//   "DisplayXR Stylus"      /interaction_profiles/logitech/mx_ink_stylus_logitech
//   "DisplayXR Controller"  /interaction_profiles/khr/simple_controller
//
// Feature lists (names = Unity usages; the order is the device state layout):
//   both:   IsTracked, TrackingState, DevicePosition, DeviceRotation, DeviceVelocity,
//           DeviceAngularVelocity (grip pose), PointerPosition, PointerRotation (aim)
//   stylus: TipPosition, TipRotation (tip pose), Trigger + TipPressure (tip force),
//           TriggerButton (tip force > 0), Grip + MiddlePressure (middle cluster
//           force), GripButton (middle force >= 0.5), PrimaryButton (front cluster
//           click), PrimaryDoubleTap, SecondaryButton (back cluster click),
//           SecondaryDoubleTap, Docked
//   simple: Trigger + TriggerButton (select click, as 0/1), MenuButton (menu click)
// Poses are relative to the rig camera in rig units (see displayxr_provider_input.h).
// Haptics: InputDevice.SendHapticImpulse(0, amplitude, duration) ->
// xrApplyHapticFeedback on the hand's /output/haptic.
//
// Every callback here runs on Unity's main thread.

#include "displayxr_provider_input.h"

#include "IUnityInterface.h"
#include "IUnityXRInput.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern "C" void
dxr_prov_file_log(const char *s);

namespace {

// Must match Runtime/UnitySubsystemsManifest.json ("name" + input "id").
const char *k_plugin_name = "DisplayXR";
const char *k_input_id = "DisplayXR Input";

const char *k_stylus_name = "DisplayXR Stylus";
const char *k_controller_name = "DisplayXR Controller";
const char *k_manufacturer = "DisplayXR";

IUnityXRInputInterface *s_input = nullptr;
UnitySubsystemHandle s_handle = nullptr;
bool s_running = false;
UnityXRInternalInputDeviceId s_next_id = 1;

// Feature indices are positional (the order AddFeature was called in), so a
// device definition and its state writer must agree: both are driven from this
// enum.
enum Feat
{
	F_IS_TRACKED,
	F_TRACKING_STATE,
	F_DEVICE_POSITION,
	F_DEVICE_ROTATION,
	F_DEVICE_VELOCITY,
	F_DEVICE_ANGULAR_VELOCITY,
	F_POINTER_POSITION,
	F_POINTER_ROTATION,
	// stylus
	F_TIP_POSITION,
	F_TIP_ROTATION,
	F_TRIGGER,
	F_TIP_PRESSURE,
	F_TRIGGER_BUTTON,
	F_GRIP,
	F_MIDDLE_PRESSURE,
	F_GRIP_BUTTON,
	F_PRIMARY_BUTTON,
	F_PRIMARY_DOUBLE_TAP,
	F_SECONDARY_BUTTON,
	F_SECONDARY_DOUBLE_TAP,
	F_DOCKED,
	// simple controller (reuses F_TRIGGER / F_TRIGGER_BUTTON)
	F_MENU_BUTTON,
	F_COUNT
};

struct Device
{
	UnityXRInternalInputDeviceId id = kUnityInvalidXRInternalInputDeviceId;
	int kind = DXR_INPUT_KIND_NONE;
	UnityXRInputFeatureIndex idx[F_COUNT];
};
Device s_dev[2];

void
log_line(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	dxr_prov_file_log(buf);
}

int
hand_of(UnityXRInternalInputDeviceId id)
{
	for (int h = 0; h < 2; h++)
		if (s_dev[h].kind != DXR_INPUT_KIND_NONE && s_dev[h].id == id)
			return h;
	return -1;
}

void
disconnect(int h)
{
	if (s_dev[h].kind != DXR_INPUT_KIND_NONE && s_input && s_handle)
		s_input->InputSubsystem_DeviceDisconnected(s_handle, s_dev[h].id);
	if (s_dev[h].kind != DXR_INPUT_KIND_NONE)
		log_line("[DisplayXR-PROV] input: %s device %u disconnected\n", h == 0 ? "left" : "right", s_dev[h].id);
	s_dev[h].kind = DXR_INPUT_KIND_NONE;
	s_dev[h].id = kUnityInvalidXRInternalInputDeviceId;
}

UnityXRInputFeatureIndex
add(UnityXRInputDeviceDefinition *def, const char *name, UnityXRInputFeatureType type, const char *usage)
{
	return usage ? s_input->DeviceDefinition_AddFeatureWithUsage(def, name, type, usage)
	             : s_input->DeviceDefinition_AddFeature(def, name, type);
}

// ---- IUnityXRInput provider ----

UnitySubsystemErrorCode UNITY_INTERFACE_API
Tick(UnitySubsystemHandle handle, void *, UnityXRInputUpdateType update_type)
{
	if (!s_running)
		return kUnitySubsystemErrorCodeSuccess;
	dxr_input_update(update_type == kUnityXRInputUpdateTypeBeforeRender ? 1 : 0);
	if (update_type != kUnityXRInputUpdateTypeDynamic)
		return kUnitySubsystemErrorCodeSuccess;
	for (int h = 0; h < 2; h++) {
		const int kind = dxr_input_get_hand_state(h, nullptr);
		if (kind == s_dev[h].kind)
			continue;
		disconnect(h);
		if (kind == DXR_INPUT_KIND_NONE)
			continue;
		s_dev[h].kind = kind;
		s_dev[h].id = s_next_id++;
		for (int f = 0; f < F_COUNT; f++)
			s_dev[h].idx[f] = kUnityInvalidXRInputFeatureIndex;
		s_input->InputSubsystem_DeviceConnected(handle, s_dev[h].id);
		log_line("[DisplayXR-PROV] input: %s device %u connected as \"%s\"\n", h == 0 ? "left" : "right",
		         s_dev[h].id, kind == DXR_INPUT_KIND_STYLUS ? k_stylus_name : k_controller_name);
	}
	return kUnitySubsystemErrorCodeSuccess;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
FillDeviceDefinition(UnitySubsystemHandle, void *, UnityXRInternalInputDeviceId id, UnityXRInputDeviceDefinition *def)
{
	const int h = hand_of(id);
	if (h < 0)
		return kUnitySubsystemErrorCodeInvalidArguments;
	Device &d = s_dev[h];
	const bool stylus = d.kind == DXR_INPUT_KIND_STYLUS;
	s_input->DeviceDefinition_SetName(def, stylus ? k_stylus_name : k_controller_name);
	s_input->DeviceDefinition_SetManufacturer(def, k_manufacturer);
	s_input->DeviceDefinition_SetCharacteristics(
	    def, (UnityXRInputDeviceCharacteristics)(kUnityXRInputDeviceCharacteristicsHeldInHand |
	                                             kUnityXRInputDeviceCharacteristicsTrackedDevice |
	                                             kUnityXRInputDeviceCharacteristicsController |
	                                             (h == 0 ? kUnityXRInputDeviceCharacteristicsLeft
	                                                     : kUnityXRInputDeviceCharacteristicsRight)));
	s_input->DeviceDefinition_SetCanQueryForDeviceStateAtTime(def, false);

	d.idx[F_IS_TRACKED] = add(def, "IsTracked", kUnityXRInputFeatureTypeBinary, kUnityXRInputFeatureUsageIsTracked);
	d.idx[F_TRACKING_STATE] =
	    add(def, "TrackingState", kUnityXRInputFeatureTypeDiscreteStates, kUnityXRInputFeatureUsageTrackingState);
	d.idx[F_DEVICE_POSITION] =
	    add(def, "DevicePosition", kUnityXRInputFeatureTypeAxis3D, kUnityXRInputFeatureUsageDevicePosition);
	d.idx[F_DEVICE_ROTATION] =
	    add(def, "DeviceRotation", kUnityXRInputFeatureTypeRotation, kUnityXRInputFeatureUsageDeviceRotation);
	d.idx[F_DEVICE_VELOCITY] =
	    add(def, "DeviceVelocity", kUnityXRInputFeatureTypeAxis3D, kUnityXRInputFeatureUsageDeviceVelocity);
	d.idx[F_DEVICE_ANGULAR_VELOCITY] = add(def, "DeviceAngularVelocity", kUnityXRInputFeatureTypeAxis3D,
	                                       kUnityXRInputFeatureUsageDeviceAngularVelocity);
	d.idx[F_POINTER_POSITION] = add(def, "PointerPosition", kUnityXRInputFeatureTypeAxis3D, "PointerPosition");
	d.idx[F_POINTER_ROTATION] = add(def, "PointerRotation", kUnityXRInputFeatureTypeRotation, "PointerRotation");
	if (stylus) {
		d.idx[F_TIP_POSITION] = add(def, "TipPosition", kUnityXRInputFeatureTypeAxis3D, "TipPosition");
		d.idx[F_TIP_ROTATION] = add(def, "TipRotation", kUnityXRInputFeatureTypeRotation, "TipRotation");
		d.idx[F_TRIGGER] =
		    add(def, "Trigger", kUnityXRInputFeatureTypeAxis1D, kUnityXRInputFeatureUsageTrigger);
		d.idx[F_TIP_PRESSURE] = add(def, "TipPressure", kUnityXRInputFeatureTypeAxis1D, "TipPressure");
		d.idx[F_TRIGGER_BUTTON] =
		    add(def, "TriggerButton", kUnityXRInputFeatureTypeBinary, kUnityXRInputFeatureUsageTriggerButton);
		d.idx[F_GRIP] = add(def, "Grip", kUnityXRInputFeatureTypeAxis1D, kUnityXRInputFeatureUsageGrip);
		d.idx[F_MIDDLE_PRESSURE] = add(def, "MiddlePressure", kUnityXRInputFeatureTypeAxis1D, "MiddlePressure");
		d.idx[F_GRIP_BUTTON] =
		    add(def, "GripButton", kUnityXRInputFeatureTypeBinary, kUnityXRInputFeatureUsageGripButton);
		d.idx[F_PRIMARY_BUTTON] =
		    add(def, "PrimaryButton", kUnityXRInputFeatureTypeBinary, kUnityXRInputFeatureUsagePrimaryButton);
		d.idx[F_PRIMARY_DOUBLE_TAP] =
		    add(def, "PrimaryDoubleTap", kUnityXRInputFeatureTypeBinary, "PrimaryDoubleTap");
		d.idx[F_SECONDARY_BUTTON] = add(def, "SecondaryButton", kUnityXRInputFeatureTypeBinary,
		                                kUnityXRInputFeatureUsageSecondaryButton);
		d.idx[F_SECONDARY_DOUBLE_TAP] =
		    add(def, "SecondaryDoubleTap", kUnityXRInputFeatureTypeBinary, "SecondaryDoubleTap");
		d.idx[F_DOCKED] = add(def, "Docked", kUnityXRInputFeatureTypeBinary, "Docked");
	} else {
		d.idx[F_TRIGGER] =
		    add(def, "Trigger", kUnityXRInputFeatureTypeAxis1D, kUnityXRInputFeatureUsageTrigger);
		d.idx[F_TRIGGER_BUTTON] =
		    add(def, "TriggerButton", kUnityXRInputFeatureTypeBinary, kUnityXRInputFeatureUsageTriggerButton);
		d.idx[F_MENU_BUTTON] =
		    add(def, "MenuButton", kUnityXRInputFeatureTypeBinary, kUnityXRInputFeatureUsageMenuButton);
	}
	return kUnitySubsystemErrorCodeSuccess;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
UpdateDeviceConfig(UnitySubsystemHandle, void *, UnityXRInternalInputDeviceId, UnityXRInputDeviceConfig *)
{
	return kUnitySubsystemErrorCodeSuccess;
}

void
set_bool(UnityXRInputDeviceState *st, UnityXRInputFeatureIndex i, int v)
{
	if (i != kUnityInvalidXRInputFeatureIndex)
		s_input->DeviceState_SetBinaryValue(st, i, v != 0);
}

void
set_float(UnityXRInputDeviceState *st, UnityXRInputFeatureIndex i, float v)
{
	if (i != kUnityInvalidXRInputFeatureIndex)
		s_input->DeviceState_SetAxis1DValue(st, i, v);
}

void
set_vec3(UnityXRInputDeviceState *st, UnityXRInputFeatureIndex i, const float *v)
{
	if (i == kUnityInvalidXRInputFeatureIndex)
		return;
	UnityXRVector3 u = {v[0], v[1], v[2]};
	s_input->DeviceState_SetAxis3DValue(st, i, u);
}

void
set_rot(UnityXRInputDeviceState *st, UnityXRInputFeatureIndex i, const float *q)
{
	if (i == kUnityInvalidXRInputFeatureIndex)
		return;
	UnityXRVector4 u = {q[0], q[1], q[2], q[3]};
	s_input->DeviceState_SetRotationValue(st, i, u);
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
UpdateDeviceState(
    UnitySubsystemHandle, void *, UnityXRInternalInputDeviceId id, UnityXRInputUpdateType, UnityXRInputDeviceState *st)
{
	const int h = hand_of(id);
	if (h < 0)
		return kUnitySubsystemErrorCodeInvalidArguments;
	const Device &d = s_dev[h];
	DxrInputHandState s;
	dxr_input_get_hand_state(h, &s);

	const DxrInputPose &grip = s.pose[DXR_INPUT_POSE_GRIP];
	const DxrInputPose &aim = s.pose[DXR_INPUT_POSE_AIM];
	const uint64_t pos_valid = XR_SPACE_LOCATION_POSITION_VALID_BIT;
	const uint64_t rot_valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	unsigned int ts = kUnityXRInputTrackingStateNone;
	if (grip.active && (grip.flags & pos_valid))
		ts |= kUnityXRInputTrackingStatePosition;
	if (grip.active && (grip.flags & rot_valid))
		ts |= kUnityXRInputTrackingStateRotation;
	if (grip.active && (grip.vel_flags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT))
		ts |= kUnityXRInputTrackingStateVelocity;
	if (grip.active && (grip.vel_flags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT))
		ts |= kUnityXRInputTrackingStateAngularVelocity;
	const int tracked = grip.active && (grip.flags & (pos_valid | rot_valid)) == (pos_valid | rot_valid);

	set_bool(st, d.idx[F_IS_TRACKED], tracked);
	if (d.idx[F_TRACKING_STATE] != kUnityInvalidXRInputFeatureIndex)
		s_input->DeviceState_SetDiscreteStateValue(st, d.idx[F_TRACKING_STATE], ts);
	set_vec3(st, d.idx[F_DEVICE_POSITION], grip.position);
	set_rot(st, d.idx[F_DEVICE_ROTATION], grip.rotation);
	set_vec3(st, d.idx[F_DEVICE_VELOCITY], grip.linear_velocity);
	set_vec3(st, d.idx[F_DEVICE_ANGULAR_VELOCITY], grip.angular_velocity);
	set_vec3(st, d.idx[F_POINTER_POSITION], aim.position);
	set_rot(st, d.idx[F_POINTER_ROTATION], aim.rotation);

	if (d.kind == DXR_INPUT_KIND_STYLUS) {
		const DxrInputPose &tip = s.pose[DXR_INPUT_POSE_TIP];
		set_vec3(st, d.idx[F_TIP_POSITION], tip.position);
		set_rot(st, d.idx[F_TIP_ROTATION], tip.rotation);
		set_float(st, d.idx[F_TRIGGER], s.tip_force);
		set_float(st, d.idx[F_TIP_PRESSURE], s.tip_force);
		set_bool(st, d.idx[F_TRIGGER_BUTTON], s.tip_force > 0.0f);
		set_float(st, d.idx[F_GRIP], s.middle_force);
		set_float(st, d.idx[F_MIDDLE_PRESSURE], s.middle_force);
		set_bool(st, d.idx[F_GRIP_BUTTON], s.middle_force >= 0.5f);
		set_bool(st, d.idx[F_PRIMARY_BUTTON], s.front_click);
		set_bool(st, d.idx[F_PRIMARY_DOUBLE_TAP], s.front_double_tap);
		set_bool(st, d.idx[F_SECONDARY_BUTTON], s.back_click);
		set_bool(st, d.idx[F_SECONDARY_DOUBLE_TAP], s.back_double_tap);
		set_bool(st, d.idx[F_DOCKED], s.docked);
	} else {
		set_float(st, d.idx[F_TRIGGER], s.select_click ? 1.0f : 0.0f);
		set_bool(st, d.idx[F_TRIGGER_BUTTON], s.select_click);
		set_bool(st, d.idx[F_MENU_BUTTON], s.menu_click);
	}
	return kUnitySubsystemErrorCodeSuccess;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
HandleEvent(UnitySubsystemHandle, void *, unsigned int, UnityXRInternalInputDeviceId, void *, unsigned int)
{
	return kUnitySubsystemErrorCodeSuccess;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
HandleRecenter(UnitySubsystemHandle, void *)
{
	// The poses are relative to the rig camera; the app owns recentring (it moves
	// the rig).
	return kUnitySubsystemErrorCodeSuccess;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
HandleHapticImpulse(
    UnitySubsystemHandle, void *, UnityXRInternalInputDeviceId id, int channel, float amplitude, float duration)
{
	const int h = hand_of(id);
	if (h < 0 || channel != 0)
		return kUnitySubsystemErrorCodeInvalidArguments;
	return dxr_input_haptic(h, amplitude, duration) ? kUnitySubsystemErrorCodeSuccess
	                                                : kUnitySubsystemErrorCodeFailure;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
HandleHapticBuffer(
    UnitySubsystemHandle, void *, UnityXRInternalInputDeviceId, int, unsigned int, const unsigned char *const)
{
	return kUnitySubsystemErrorCodeNotSupported;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
QueryHapticCapabilities(UnitySubsystemHandle, void *, UnityXRInternalInputDeviceId id, UnityXRHapticCapabilities *caps)
{
	if (hand_of(id) < 0 || caps == nullptr)
		return kUnitySubsystemErrorCodeInvalidArguments;
	memset(caps, 0, sizeof(*caps));
	caps->numChannels = 1;
	caps->supportsImpulse = true;
	caps->supportsBuffer = false;
	return kUnitySubsystemErrorCodeSuccess;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
HandleHapticStop(UnitySubsystemHandle, void *, UnityXRInternalInputDeviceId id)
{
	const int h = hand_of(id);
	if (h < 0)
		return kUnitySubsystemErrorCodeInvalidArguments;
	dxr_input_haptic_stop(h);
	return kUnitySubsystemErrorCodeSuccess;
}

// The poses are rig-camera-relative, which is a "device" origin: the app's
// camera (the rig) is the reference, nothing is floor-relative.
UnitySubsystemErrorCode UNITY_INTERFACE_API
QueryTrackingOriginMode(UnitySubsystemHandle, void *, UnityXRInputTrackingOriginModeFlags *mode)
{
	if (mode)
		*mode = kUnityXRInputTrackingOriginModeDevice;
	return kUnitySubsystemErrorCodeSuccess;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
QuerySupportedTrackingOriginModes(UnitySubsystemHandle, void *, UnityXRInputTrackingOriginModeFlags *modes)
{
	if (modes)
		*modes = kUnityXRInputTrackingOriginModeDevice;
	return kUnitySubsystemErrorCodeSuccess;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
HandleSetTrackingOriginMode(UnitySubsystemHandle, void *, UnityXRInputTrackingOriginModeFlags mode)
{
	return mode == kUnityXRInputTrackingOriginModeDevice ? kUnitySubsystemErrorCodeSuccess
	                                                     : kUnitySubsystemErrorCodeFailure;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
TryGetDeviceStateAtTime(
    UnitySubsystemHandle, void *, UnityXRTimeStamp, UnityXRInternalInputDeviceId, UnityXRInputDeviceState *)
{
	return kUnitySubsystemErrorCodeNotSupported;
}

// ---- Lifecycle (main thread) ----

UnitySubsystemErrorCode UNITY_INTERFACE_API
LifecycleInitialize(UnitySubsystemHandle handle, void *)
{
	s_handle = handle;
	if (!s_input)
		return kUnitySubsystemErrorCodeFailure;
	UnityXRInputProvider p;
	memset(&p, 0, sizeof(p));
	p.Tick = Tick;
	p.FillDeviceDefinition = FillDeviceDefinition;
	p.UpdateDeviceConfig = UpdateDeviceConfig;
	p.UpdateDeviceState = UpdateDeviceState;
	p.HandleEvent = HandleEvent;
	p.HandleRecenter = HandleRecenter;
	p.HandleHapticImpulse = HandleHapticImpulse;
	p.HandleHapticBuffer = HandleHapticBuffer;
	p.QueryHapticCapabilities = QueryHapticCapabilities;
	p.HandleHapticStop = HandleHapticStop;
	p.QueryTrackingOriginMode = QueryTrackingOriginMode;
	p.QuerySupportedTrackingOriginModes = QuerySupportedTrackingOriginModes;
	p.HandleSetTrackingOriginMode = HandleSetTrackingOriginMode;
	p.TryGetDeviceStateAtTime = TryGetDeviceStateAtTime;
	UnitySubsystemErrorCode rc = s_input->RegisterInputProvider(handle, &p);
	log_line("[DisplayXR-PROV] input subsystem: Initialize (%s)\n",
	         rc == kUnitySubsystemErrorCodeSuccess ? "provider registered" : "RegisterInputProvider FAILED");
	return rc;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
LifecycleStart(UnitySubsystemHandle handle, void *)
{
	s_handle = handle;
	s_running = true;
	log_line(
	    "[DisplayXR-PROV] input subsystem: Start (devices follow the "
	    "session's bound profiles)\n");
	return kUnitySubsystemErrorCodeSuccess;
}

void UNITY_INTERFACE_API
LifecycleStop(UnitySubsystemHandle, void *)
{
	s_running = false;
	for (int h = 0; h < 2; h++)
		disconnect(h);
	log_line("[DisplayXR-PROV] input subsystem: Stop\n");
}

void UNITY_INTERFACE_API
LifecycleShutdown(UnitySubsystemHandle, void *)
{
	s_running = false;
	for (int h = 0; h < 2; h++)
		disconnect(h);
	s_handle = nullptr;
}

} // namespace

extern "C" void
displayxr_register_xr_input_provider(IUnityInterfaces *ifaces)
{
	if (!ifaces)
		return;
	s_input = ifaces->Get<IUnityXRInputInterface>();
	if (!s_input) {
		log_line(
		    "[DisplayXR-PROV] IUnityXRInputInterface unavailable - no "
		    "controller input\n");
		return;
	}
	UnityLifecycleProvider lifecycle;
	memset(&lifecycle, 0, sizeof(lifecycle));
	lifecycle.Initialize = LifecycleInitialize;
	lifecycle.Start = LifecycleStart;
	lifecycle.Stop = LifecycleStop;
	lifecycle.Shutdown = LifecycleShutdown;
	UnitySubsystemErrorCode rc = s_input->RegisterLifecycleProvider(k_plugin_name, k_input_id, &lifecycle);
	log_line(rc == kUnitySubsystemErrorCodeSuccess ? "[DisplayXR-PROV] input: RegisterLifecycleProvider OK\n"
	                                               : "[DisplayXR-PROV] input: RegisterLifecycleProvider FAILED "
	                                                 "(manifest mismatch?)\n");
}
