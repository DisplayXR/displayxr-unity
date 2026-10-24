// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Android platform glue for the IUnityXRDisplay provider (#418). See
// displayxr_android.h for what lives here and why.

#if defined(__ANDROID__)

#include "displayxr_android.h"
#include "displayxr_exports.h"

#include <jni.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <sys/system_properties.h>
#include <errno.h>
#include <time.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "displayxr_extensions.h" // XR_DXR_android_surface_binding structs (#418 P3b)

#define DXR_ANDROID_LOG_TAG "DisplayXRUnity"

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

extern "C" void
dxr_android_log(const char *msg)
{
	if (!msg) return;
	__android_log_write(ANDROID_LOG_INFO, DXR_ANDROID_LOG_TAG, msg);
}

static void
and_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
and_log(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	dxr_android_log(buf);
}

// ---------------------------------------------------------------------------
// A/B switches
// ---------------------------------------------------------------------------

static int
and_flag_value_on(const char *v)
{
	return v && v[0] && strcmp(v, "0") != 0 && strcmp(v, "false") != 0;
}

extern "C" int
dxr_android_flag(const char *env, const char *prop)
{
	if (env) {
		const char *v = getenv(env);
		if (v) return and_flag_value_on(v);
	}
	if (prop) {
		char v[PROP_VALUE_MAX] = {0};
		if (__system_property_get(prop, v) > 0) return and_flag_value_on(v);
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Shell-mode stub
// ---------------------------------------------------------------------------

// There is no DisplayXR Shell on Android (the workspace/IPC tile session is a
// Windows product feature). The shared provider code queries this predicate
// unconditionally, so — exactly like the macOS and Linux stubs — answer 0.
DISPLAYXR_EXPORT int
displayxr_is_shell_mode(void)
{
	return 0;
}

// ---------------------------------------------------------------------------
// JavaVM + activity
// ---------------------------------------------------------------------------

static JavaVM *s_vm = nullptr;
static jobject s_activity = nullptr; // global ref, never released (process lifetime)
static jclass s_unity_player_cls = nullptr; // global ref, cached by JNI_OnLoad if visible
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;

// Unity's Android player calls a native plug-in's JNI_OnLoad when it loads the
// library, which is the documented way to obtain the JavaVM. Resolve the
// UnityPlayer class here too while we can: on a Java thread FindClass sees the
// app's class loader (an attached native thread later only sees the boot one).
// The activity itself is read later (currentActivity may not be set yet).
extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved)
{
	(void)reserved;
	s_vm = vm;
	JNIEnv *env = nullptr;
	if (vm && vm->GetEnv((void **)&env, JNI_VERSION_1_6) == JNI_OK && env) {
		jclass cls = env->FindClass("com/unity3d/player/UnityPlayer");
		if (cls && !env->ExceptionCheck()) {
			s_unity_player_cls = (jclass)env->NewGlobalRef(cls);
			env->DeleteLocalRef(cls);
		} else if (env->ExceptionCheck()) {
			env->ExceptionClear();
		}
	}
	and_log("[DisplayXR-AND] JNI_OnLoad: JavaVM=%p tid=%d UnityPlayer class %s", (void *)vm,
	        (int)gettid(), s_unity_player_cls ? "cached" : "not visible here (resolved later)");
	return JNI_VERSION_1_6;
}

// Fallback when JNI_OnLoad never fired (a loader that dlopen()s us directly).
// JNI_GetCreatedJavaVMs is exported by the runtime's libart / libnativehelper,
// but only stubbed in the NDK from API 31 — so resolve it at run time rather
// than adding a link dependency.
static JavaVM *
and_get_vm(void)
{
	if (s_vm) return s_vm;
	typedef jint (*PFN_GetCreatedJavaVMs)(JavaVM **, jsize, jsize *);
	PFN_GetCreatedJavaVMs get = (PFN_GetCreatedJavaVMs)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
	if (!get) {
		void *h = dlopen("libnativehelper.so", RTLD_NOW | RTLD_NOLOAD);
		if (!h) h = dlopen("libart.so", RTLD_NOW | RTLD_NOLOAD);
		if (h) get = (PFN_GetCreatedJavaVMs)dlsym(h, "JNI_GetCreatedJavaVMs");
	}
	if (get) {
		JavaVM *vm = nullptr;
		jsize n = 0;
		if (get(&vm, 1, &n) == JNI_OK && n > 0 && vm) {
			s_vm = vm;
			and_log("[DisplayXR-AND] JavaVM from JNI_GetCreatedJavaVMs (JNI_OnLoad did not run): %p",
			        (void *)vm);
		}
	}
	if (!s_vm) and_log("[DisplayXR-AND] ERROR: no JavaVM (JNI_OnLoad never ran, no fallback)");
	return s_vm;
}

static bool
and_clear_exc(JNIEnv *env)
{
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return true;
	}
	return false;
}

// An app class (dotted name), resolved through the APP's class loader. A plain
// FindClass on a native thread that attached itself (Unity's render thread, say)
// only sees the boot class loader and fails, so fall back to
// ActivityThread.currentApplication().getClassLoader().loadClass(...): both
// ActivityThread and ClassLoader are framework classes FindClass always sees.
static jclass
and_load_app_class(JNIEnv *env, const char *slashed, const char *dotted)
{
	jclass cls = env->FindClass(slashed);
	if (cls && !and_clear_exc(env)) return cls;
	and_clear_exc(env);

	jclass at = env->FindClass("android/app/ActivityThread");
	if (!at || and_clear_exc(env)) return nullptr;
	jmethodID cur_app = env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;");
	if (!cur_app || and_clear_exc(env)) { env->DeleteLocalRef(at); return nullptr; }
	jobject app = env->CallStaticObjectMethod(at, cur_app);
	env->DeleteLocalRef(at);
	if (!app || and_clear_exc(env)) return nullptr;

	jclass ctx_cls = env->GetObjectClass(app);
	jmethodID get_cl = env->GetMethodID(ctx_cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
	env->DeleteLocalRef(ctx_cls);
	if (!get_cl || and_clear_exc(env)) { env->DeleteLocalRef(app); return nullptr; }
	jobject loader = env->CallObjectMethod(app, get_cl);
	env->DeleteLocalRef(app);
	if (!loader || and_clear_exc(env)) return nullptr;

	jclass cl_cls = env->FindClass("java/lang/ClassLoader");
	jmethodID load = cl_cls ? env->GetMethodID(cl_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;")
	                        : nullptr;
	if (cl_cls) env->DeleteLocalRef(cl_cls);
	if (!load || and_clear_exc(env)) { env->DeleteLocalRef(loader); return nullptr; }
	jstring name = env->NewStringUTF(dotted);
	jclass up = (jclass)env->CallObjectMethod(loader, load, name);
	env->DeleteLocalRef(name);
	env->DeleteLocalRef(loader);
	if (!up || and_clear_exc(env)) return nullptr;
	return up;
}

// com.unity3d.player.UnityPlayer (cached by JNI_OnLoad when it was visible there).
static jclass
and_find_unity_player(JNIEnv *env)
{
	if (s_unity_player_cls) return (jclass)env->NewLocalRef(s_unity_player_cls);
	return and_load_app_class(env, "com/unity3d/player/UnityPlayer", "com.unity3d.player.UnityPlayer");
}

// Attach (if needed) for the duration of one call. A thread we attached is
// detached again: ART aborts a native thread that exits while still attached.
struct AndEnv {
	JNIEnv *env = nullptr;
	bool attached = false;
	explicit AndEnv(JavaVM *vm)
	{
		if (!vm) return;
		if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) == JNI_OK) return;
		env = nullptr;
		if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
		else env = nullptr;
	}
	~AndEnv()
	{
		if (attached && s_vm) s_vm->DetachCurrentThread();
	}
};

// The Unity activity as a global ref (cached once found).
static jobject
and_get_activity(void)
{
	if (s_activity) return s_activity;
	JavaVM *vm = and_get_vm();
	if (!vm) return nullptr;
	AndEnv e(vm);
	if (!e.env) {
		and_log("[DisplayXR-AND] ERROR: could not get a JNIEnv for this thread");
		return nullptr;
	}
	JNIEnv *env = e.env;
	jclass up = and_find_unity_player(env);
	if (!up) {
		and_log("[DisplayXR-AND] ERROR: com.unity3d.player.UnityPlayer not found");
		return nullptr;
	}
	jfieldID fid = env->GetStaticFieldID(up, "currentActivity", "Landroid/app/Activity;");
	if (!fid || and_clear_exc(env)) {
		env->DeleteLocalRef(up);
		and_log("[DisplayXR-AND] ERROR: UnityPlayer.currentActivity field not found");
		return nullptr;
	}
	jobject act = env->GetStaticObjectField(up, fid);
	env->DeleteLocalRef(up);
	if (!act || and_clear_exc(env)) {
		and_log("[DisplayXR-AND] ERROR: UnityPlayer.currentActivity is null");
		return nullptr;
	}
	s_activity = env->NewGlobalRef(act);
	env->DeleteLocalRef(act);
	and_log("[DisplayXR-AND] Unity activity: %p (tid=%d)", (void *)s_activity, (int)gettid());
	return s_activity;
}

// ---------------------------------------------------------------------------
// Khronos Android loader (D1)
// ---------------------------------------------------------------------------

static int s_loader_ok = 0;
static XrInstanceCreateInfoAndroidKHR s_ici_android;

extern "C" int
dxr_android_loader_gipa(PFN_xrGetInstanceProcAddr *out_gipa)
{
	if (!out_gipa) return 0;
	pthread_mutex_lock(&s_mx);
	int ok = s_loader_ok;
	if (!ok) {
		JavaVM *vm = and_get_vm();
		jobject act = and_get_activity();
		if (vm && act) {
			PFN_xrInitializeLoaderKHR init = nullptr;
			XrResult r = xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
			                                   (PFN_xrVoidFunction *)&init);
			if (XR_SUCCEEDED(r) && init) {
				XrLoaderInitInfoAndroidKHR li = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
				li.applicationVM = vm;
				li.applicationContext = act;
				r = init((const XrLoaderInitInfoBaseHeaderKHR *)&li);
				and_log("[DisplayXR-AND] xrInitializeLoaderKHR: %d", (int)r);
				ok = XR_SUCCEEDED(r) ? 1 : 0;
			} else {
				and_log("[DisplayXR-AND] ERROR: xrInitializeLoaderKHR unresolved (%d)", (int)r);
			}
		}
		s_loader_ok = ok;
	}
	pthread_mutex_unlock(&s_mx);
	if (!ok) return 0;
	*out_gipa = xrGetInstanceProcAddr; // the linked loader's
	return 1;
}

extern "C" const void *
dxr_android_instance_create_info(const void *next)
{
	if (!s_loader_ok || !s_vm || !s_activity) return nullptr;
	memset(&s_ici_android, 0, sizeof(s_ici_android));
	s_ici_android.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	s_ici_android.next = next;
	s_ici_android.applicationVM = s_vm;
	s_ici_android.applicationActivity = s_activity;
	return &s_ici_android;
}

// ---------------------------------------------------------------------------
// Session-thread ALooper (D4)
// ---------------------------------------------------------------------------

static pid_t s_looper_tid = 0;
static int s_looper_warned = 0;

extern "C" void
dxr_android_looper_prepare(void)
{
	ALooper *had = ALooper_forThread();
	ALooper *l = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
	s_looper_tid = gettid();
	s_looper_warned = 0;
	char name[32] = {0};
	pthread_getname_np(pthread_self(), name, sizeof(name));
	and_log("[DisplayXR-AND] session thread tid=%d name='%s': ALooper %s (%p)", (int)s_looper_tid, name,
	        had ? "already present" : "prepared by the provider", (void *)l);
}

extern "C" void
dxr_android_looper_poll(void)
{
	if (s_looper_tid == 0) return;
	if (gettid() != s_looper_tid) {
		if (!s_looper_warned) {
			s_looper_warned = 1;
			and_log("[DisplayXR-AND] WARN: provider pump on tid=%d, but the session thread is tid=%d — "
			        "its ALooper is not pumped from here (#418 D4)",
			        (int)gettid(), (int)s_looper_tid);
		}
		return;
	}
	ALooper_pollOnce(0, nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Provider-owned weave surface (D3, #418 P3b)
// ---------------------------------------------------------------------------
//
// The Java half is com.displayxr.unity.DisplayXRSurface (DisplayXR.androidlib). It
// owns a SurfaceView stacked above Unity's and calls the natives below from the UI
// thread. This half keeps the facts it reports (the window, its size, the last
// geometry) and forwards each to the runtime the moment it changes — or, when no
// session exists yet, re-applies all of them authoritatively the moment one does
// (dxr_android_on_session): a session restart must never depend on Java re-sending
// something it already sent once.
//
// LOCKING. s_surf_mx guards the window, the geometry and the session handle + entry
// points. The UI thread holds it across its xrSet*/xrSubmit* call so the render
// thread cannot destroy the session under it; the render thread only takes it to
// install / clear the handle and never calls into the runtime while holding it.

static pthread_mutex_t s_surf_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_surf_cv = PTHREAD_COND_INITIALIZER;
static ANativeWindow *s_win = nullptr;    // our own reference (ANativeWindow_fromSurface)
static int32_t s_win_w = 0, s_win_h = 0;  // buffer size (surfaceChanged)
static unsigned s_win_publishes = 0;
static XrAndroidWindowGeometryDXR s_geom; // last sample from the Choreographer poll
static int s_geom_valid = 0;
static XrSession s_surf_session = XR_NULL_HANDLE;
static PFN_xrSetAndroidSurfaceDXR s_pfn_set_surface = nullptr;
static PFN_xrSetAndroidWindowGeometryDXR s_pfn_set_geometry = nullptr;
static PFN_xrSubmitAndroidInputEventDXR s_pfn_submit_input = nullptr;
static int s_surf_attach_requested = 0;
static unsigned long s_touch_events = 0;
static XrAndroidSurfaceBindingCreateInfoDXR s_binding;

// Caller holds s_surf_mx and has checked s_surf_session / s_pfn_set_surface.
static void
surf_publish_locked(const char *why)
{
	XrAndroidSurfaceBindingCreateInfoDXR b = {};
	b.type = XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR;
	b.nativeWindow = (struct ANativeWindow *)s_win;
	b.screenOffsetX = s_geom_valid ? s_geom.windowRect.offset.x : 0;
	b.screenOffsetY = s_geom_valid ? s_geom.windowRect.offset.y : 0;
	b.transparentBackgroundEnabled = XR_FALSE;
	XrResult r = s_pfn_set_surface(s_surf_session, s_win ? &b : nullptr);
	and_log("[DisplayXR-AND] xrSetAndroidSurfaceDXR(%p %dx%d) [%s] -> %d", (void *)s_win, (int)s_win_w,
	        (int)s_win_h, why, (int)r);
}

static void
surf_push_geometry_locked(const char *why)
{
	XrResult r = s_pfn_set_geometry(s_surf_session, &s_geom);
	and_log("[DisplayXR-AND] xrSetAndroidWindowGeometryDXR rect=%d,%d %dx%d panel=%dx%d display=%d [%s] -> %d",
	        s_geom.windowRect.offset.x, s_geom.windowRect.offset.y, s_geom.windowRect.extent.width,
	        s_geom.windowRect.extent.height, s_geom.panelExtent.width, s_geom.panelExtent.height,
	        s_geom.displayId, why, (int)r);
}

static void JNICALL
jni_surface_created(JNIEnv *env, jclass, jobject surface, jint w, jint h)
{
	ANativeWindow *win = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
	pthread_mutex_lock(&s_surf_mx);
	if (s_win) ANativeWindow_release(s_win);
	s_win = win;
	s_win_w = w;
	s_win_h = h;
	s_win_publishes++;
	and_log("[DisplayXR-AND] surfaceCreated: window=%p %dx%d (#%u)", (void *)win, (int)w, (int)h, s_win_publishes);
	if (s_surf_session != XR_NULL_HANDLE && s_pfn_set_surface) surf_publish_locked("surfaceCreated");
	pthread_cond_broadcast(&s_surf_cv);
	pthread_mutex_unlock(&s_surf_mx);
}

static void JNICALL
jni_surface_changed(JNIEnv *, jclass, jint w, jint h)
{
	pthread_mutex_lock(&s_surf_mx);
	if (w != s_win_w || h != s_win_h)
		and_log("[DisplayXR-AND] surfaceChanged: %dx%d -> %dx%d", (int)s_win_w, (int)s_win_h, (int)w, (int)h);
	s_win_w = w;
	s_win_h = h;
	pthread_mutex_unlock(&s_surf_mx);
}

static void JNICALL
jni_surface_destroyed(JNIEnv *, jclass)
{
	pthread_mutex_lock(&s_surf_mx);
	and_log("[DisplayXR-AND] surfaceDestroyed: window=%p", (void *)s_win);
	// Tell the runtime BEFORE the window dies (we are inside surfaceDestroyed, the
	// last moment it is guaranteed alive): it drops its VkSurfaceKHR and pauses the
	// display processor instead of presenting into a dead window.
	ANativeWindow *dying = s_win;
	s_win = nullptr;
	s_win_w = s_win_h = 0;
	if (s_surf_session != XR_NULL_HANDLE && s_pfn_set_surface) surf_publish_locked("surfaceDestroyed");
	if (dying) ANativeWindow_release(dying);
	pthread_mutex_unlock(&s_surf_mx);
}

static void JNICALL
jni_window_geometry(JNIEnv *, jclass, jint x, jint y, jint w, jint h, jint pw, jint ph, jint display_id)
{
	pthread_mutex_lock(&s_surf_mx);
	memset(&s_geom, 0, sizeof(s_geom));
	s_geom.type = XR_TYPE_ANDROID_WINDOW_GEOMETRY_DXR;
	s_geom.windowRect.offset.x = x;
	s_geom.windowRect.offset.y = y;
	s_geom.windowRect.extent.width = w;
	s_geom.windowRect.extent.height = h;
	s_geom.panelExtent.width = pw;
	s_geom.panelExtent.height = ph;
	s_geom.displayId = display_id;
	s_geom_valid = 1;
	if (s_surf_session != XR_NULL_HANDLE && s_pfn_set_geometry) surf_push_geometry_locked("changed");
	else and_log("[DisplayXR-AND] window geometry %d,%d %dx%d panel=%dx%d (no session yet: held)", (int)x,
	             (int)y, (int)w, (int)h, (int)pw, (int)ph);
	pthread_mutex_unlock(&s_surf_mx);
}

static void JNICALL
jni_touch(JNIEnv *, jclass, jint action, jint pointer_id, jint pointer_count, jfloat x, jfloat y, jint sw,
          jint sh, jlong t_ns)
{
	pthread_mutex_lock(&s_surf_mx);
	XrResult r = XR_ERROR_SESSION_NOT_RUNNING;
	if (s_surf_session != XR_NULL_HANDLE && s_pfn_submit_input) {
		XrAndroidInputEventDXR e = {};
		e.type = XR_TYPE_ANDROID_INPUT_EVENT_DXR;
		e.action = (XrAndroidInputActionDXR)action;
		e.pointerId = (uint32_t)pointer_id;
		e.pointerCount = (uint32_t)pointer_count;
		e.position.x = x;
		e.position.y = y;
		e.surfaceExtent.width = sw;
		e.surfaceExtent.height = sh;
		e.eventTimeNs = (int64_t)t_ns;
		r = s_pfn_submit_input(s_surf_session, &e);
	}
	const bool submitted = s_surf_session != XR_NULL_HANDLE && s_pfn_submit_input != nullptr;
	unsigned long n = ++s_touch_events;
	pthread_mutex_unlock(&s_surf_mx);
	// Lifecycle-rate logging: every DOWN/UP/CANCEL is a gesture edge; MOVEs only
	// as a running count. A runtime below surface-binding spec v3 has no
	// xrSubmitAndroidInputEventDXR: say so once, then stay quiet.
	static int s_no_submit_logged = 0;
	if (!submitted) {
		if (!s_no_submit_logged) {
			s_no_submit_logged = 1;
			and_log("[DisplayXR-AND] touch: runtime has no xrSubmitAndroidInputEventDXR (surface binding spec "
			        "< 3, runtime < v2.47.1) — touches still reach Unity, the runtime's touch provider just "
			        "does not see them");
		}
		return;
	}
	if (action != XR_ANDROID_INPUT_ACTION_MOVE_DXR || (n % 120) == 0)
		and_log("[DisplayXR-AND] touch -> xrSubmitAndroidInputEventDXR action=%d id=%d count=%d (%.0f,%.0f) "
		        "in %dx%d -> %d (events=%lu)",
		        (int)action, (int)pointer_id, (int)pointer_count, x, y, (int)sw, (int)sh, (int)r, n);
}

static void JNICALL
jni_lifecycle(JNIEnv *, jclass, jint resumed)
{
	and_log("[DisplayXR-AND] activity %s", resumed ? "RESUMED" : "PAUSED");
}

extern "C" int
dxr_android_surface_attach(void)
{
	if (s_surf_attach_requested) return 1;
	JavaVM *vm = and_get_vm();
	jobject act = and_get_activity();
	if (!vm || !act) return 0;
	AndEnv e(vm);
	if (!e.env) return 0;
	JNIEnv *env = e.env;
	jclass cls = and_load_app_class(env, "com/displayxr/unity/DisplayXRSurface", "com.displayxr.unity.DisplayXRSurface");
	if (!cls) {
		and_log("[DisplayXR-AND] DisplayXRSurface class not found (DisplayXR.androidlib not in the build?) — "
		        "the runtime will host its own surface");
		return 0;
	}
	static const JNINativeMethod methods[] = {
	    {(char *)"nativeSurfaceCreated", (char *)"(Landroid/view/Surface;II)V", (void *)jni_surface_created},
	    {(char *)"nativeSurfaceChanged", (char *)"(II)V", (void *)jni_surface_changed},
	    {(char *)"nativeSurfaceDestroyed", (char *)"()V", (void *)jni_surface_destroyed},
	    {(char *)"nativeWindowGeometry", (char *)"(IIIIIII)V", (void *)jni_window_geometry},
	    {(char *)"nativeTouch", (char *)"(IIIFFIIJ)V", (void *)jni_touch},
	    {(char *)"nativeLifecycle", (char *)"(I)V", (void *)jni_lifecycle},
	};
	if (env->RegisterNatives(cls, methods, (jint)(sizeof(methods) / sizeof(methods[0]))) != JNI_OK ||
	    and_clear_exc(env)) {
		env->DeleteLocalRef(cls);
		and_log("[DisplayXR-AND] DisplayXRSurface: RegisterNatives failed");
		return 0;
	}
	jmethodID attach = env->GetStaticMethodID(cls, "attach", "(Landroid/app/Activity;)V");
	if (!attach || and_clear_exc(env)) {
		env->DeleteLocalRef(cls);
		and_log("[DisplayXR-AND] DisplayXRSurface.attach(Activity) not found");
		return 0;
	}
	env->CallStaticVoidMethod(cls, attach, act);
	bool exc = and_clear_exc(env);
	env->DeleteLocalRef(cls);
	if (exc) {
		and_log("[DisplayXR-AND] DisplayXRSurface.attach threw");
		return 0;
	}
	s_surf_attach_requested = 1;
	and_log("[DisplayXR-AND] DisplayXRSurface.attach posted to the UI thread (tid=%d)", (int)gettid());
	return 1;
}

extern "C" const void *
dxr_android_surface_binding(const void *next, int wait_ms)
{
	if (!s_surf_attach_requested) return nullptr;
	pthread_mutex_lock(&s_surf_mx);
	if (!s_win && wait_ms > 0) {
		struct timespec dl;
		clock_gettime(CLOCK_REALTIME, &dl);
		dl.tv_sec += wait_ms / 1000;
		dl.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
		if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
		while (!s_win) {
			if (pthread_cond_timedwait(&s_surf_cv, &s_surf_mx, &dl) == ETIMEDOUT) break;
		}
	}
	const void *out = nullptr;
	if (s_win) {
		memset(&s_binding, 0, sizeof(s_binding));
		s_binding.type = XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR;
		s_binding.next = next;
		s_binding.nativeWindow = (struct ANativeWindow *)s_win;
		s_binding.screenOffsetX = s_geom_valid ? s_geom.windowRect.offset.x : 0;
		s_binding.screenOffsetY = s_geom_valid ? s_geom.windowRect.offset.y : 0;
		s_binding.transparentBackgroundEnabled = XR_FALSE;
		out = &s_binding;
		and_log("[DisplayXR-AND] binding the provider SurfaceView at xrCreateSession: window=%p %dx%d offset=%d,%d",
		        (void *)s_win, (int)s_win_w, (int)s_win_h, s_binding.screenOffsetX, s_binding.screenOffsetY);
	} else {
		and_log("[DisplayXR-AND] WARN: provider SurfaceView not created within %d ms — no surface binding, the "
		        "runtime hosts its own surface",
		        wait_ms);
	}
	pthread_mutex_unlock(&s_surf_mx);
	return out;
}

extern "C" void
dxr_android_on_session(XrSession session, PFN_xrGetInstanceProcAddr gipa, XrInstance instance, int bound)
{
	PFN_xrVoidFunction f1 = nullptr, f2 = nullptr, f3 = nullptr;
	if (bound && gipa) {
		gipa(instance, "xrSetAndroidSurfaceDXR", &f1);
		gipa(instance, "xrSetAndroidWindowGeometryDXR", &f2);
		gipa(instance, "xrSubmitAndroidInputEventDXR", &f3);
	}
	pthread_mutex_lock(&s_surf_mx);
	s_surf_session = bound ? session : XR_NULL_HANDLE;
	s_pfn_set_surface = (PFN_xrSetAndroidSurfaceDXR)f1;
	s_pfn_set_geometry = (PFN_xrSetAndroidWindowGeometryDXR)f2;
	s_pfn_submit_input = (PFN_xrSubmitAndroidInputEventDXR)f3;
	if (s_surf_session != XR_NULL_HANDLE) {
		and_log("[DisplayXR-AND] session %p bound to the provider surface (set_surface=%p set_geometry=%p "
		        "submit_input=%p) — re-applying the surface + geometry authoritatively",
		        (void *)session, (void *)f1, (void *)f2, (void *)f3);
		// Authoritative re-apply on EVERY session start: the window may have been
		// destroyed or replaced between the binding fetch and now, and a restarted
		// session has never seen the geometry Java pushed (and de-duplicated) before.
		if (s_pfn_set_surface) surf_publish_locked("session start");
		if (s_pfn_set_geometry && s_geom_valid) surf_push_geometry_locked("session start");
	}
	pthread_mutex_unlock(&s_surf_mx);
}

extern "C" void
dxr_android_on_session_end(void)
{
	pthread_mutex_lock(&s_surf_mx);
	if (s_surf_session != XR_NULL_HANDLE)
		and_log("[DisplayXR-AND] session %p ending: surface/geometry/touch calls stop", (void *)s_surf_session);
	s_surf_session = XR_NULL_HANDLE;
	s_pfn_set_surface = nullptr;
	s_pfn_set_geometry = nullptr;
	s_pfn_submit_input = nullptr;
	pthread_mutex_unlock(&s_surf_mx);
}

extern "C" int
dxr_android_surface_size(uint32_t *w, uint32_t *h)
{
	pthread_mutex_lock(&s_surf_mx);
	int ok = (s_win && s_win_w > 0 && s_win_h > 0) ? 1 : 0;
	if (ok) {
		if (w) *w = (uint32_t)s_win_w;
		if (h) *h = (uint32_t)s_win_h;
	}
	pthread_mutex_unlock(&s_surf_mx);
	return ok;
}

#endif // __ANDROID__
