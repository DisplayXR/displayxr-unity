// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Android platform glue for the IUnityXRDisplay provider (#418).
//
// Everything that needs <jni.h>, <android/looper.h> or the Khronos Android loader
// lives in displayxr_android.cpp, so the shared provider TUs keep plain C entry
// points and never see JNI types:
//
//  - D1 runtime loading: the Khronos OpenXR loader for Android is LINKED (shipped
//    beside this library). It needs xrInitializeLoaderKHR(XrLoaderInitInfoAndroidKHR
//    {JavaVM, Context}) before anything else, and the runtime rejects an
//    xrCreateInstance that does not chain XrInstanceCreateInfoAndroidKHR. The
//    JavaVM comes from JNI_OnLoad (with a JNI_GetCreatedJavaVMs fallback), the
//    activity from com.unity3d.player.UnityPlayer.currentActivity.
//  - D4: the vendor display processor is created on the thread that calls
//    xrCreateSession and needs a prepared, pumped ALooper there.
//  - Logging: logcat (tag "DisplayXRUnity") instead of a /tmp file + stderr.
//
// Compiled on Android only (CMakeLists.txt). Desktop never includes this header.

#pragma once

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Write one log line to logcat under the "DisplayXRUnity" tag.
void dxr_android_log(const char *msg);

/// Initialise the Khronos Android loader once per process (xrInitializeLoaderKHR
/// with the JavaVM + the Unity activity) and return its xrGetInstanceProcAddr.
/// @return 1 on success (out_gipa set), 0 on failure (logged).
int dxr_android_loader_gipa(PFN_xrGetInstanceProcAddr *out_gipa);

/// The XrInstanceCreateInfoAndroidKHR to chain onto XrInstanceCreateInfo.next,
/// with its own `next` set to @p next. Valid until the next call; NULL if the
/// loader was never initialised (no JavaVM / activity).
const void *dxr_android_instance_create_info(const void *next);

/// Prepare an ALooper on the CALLING thread (the one about to call
/// xrCreateSession) and remember that thread for dxr_android_looper_poll. Logs
/// the thread id and whether the thread already had a looper (#418 D4).
void dxr_android_looper_prepare(void);

/// ALooper_pollOnce(0) when called on the thread prepare() ran on; a no-op (one
/// WARN) on any other thread. Called once per frame from the provider pump.
void dxr_android_looper_poll(void);

// --- Provider-owned weave surface (D3, #418 P3b) -----------------------------
// Java half: com.displayxr.unity.DisplayXRSurface (Runtime/Plugins/Android/
// DisplayXR.androidlib). See the section comment in displayxr_android.cpp.

/// Ask the UI thread to add the provider's SurfaceView over Unity's view (once per
/// process). @return 1 if the request was posted, 0 if the Java class is missing.
int dxr_android_surface_attach(void);

/// The XrAndroidSurfaceBindingCreateInfoDXR to chain at xrCreateSession (its `next`
/// set to @p next), waiting up to @p wait_ms for the surface to exist. NULL when
/// there is none — the session then runs hosted (the runtime's own SurfaceView).
const void *dxr_android_surface_binding(const void *next, int wait_ms);

/// After xrCreateSession: resolve the surface-binding entry points and, when the
/// session was created with the binding (@p bound), re-apply the current surface and
/// geometry. From here on the UI thread forwards every change to this session.
void dxr_android_on_session(XrSession session, PFN_xrGetInstanceProcAddr gipa, XrInstance instance,
                            int bound);

/// Before xrDestroySession: stop forwarding (the UI thread never touches a dead session).
void dxr_android_on_session_end(void);

/// The bound surface's buffer size. @return 0 while there is no surface.
int dxr_android_surface_size(uint32_t *w, uint32_t *h);

/// A/B switch read from the environment variable @p env (set in-process, e.g. by a
/// C# script at SubsystemRegistration) or, failing that, the system property
/// @p prop (`adb shell setprop debug.displayxr.<name> 1`, no rebuild). "0", "" and
/// "false" read as off. @return 1 when set and on.
int dxr_android_flag(const char *env, const char *prop);

#ifdef __cplusplus
}
#endif
