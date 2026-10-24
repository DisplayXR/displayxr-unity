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
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

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

// com.unity3d.player.UnityPlayer, resolved through the APP's class loader. A
// plain FindClass on a native thread that attached itself (Unity's render thread,
// say) only sees the boot class loader and fails, so fall back to
// ActivityThread.currentApplication().getClassLoader().loadClass(...): both
// ActivityThread and ClassLoader are framework classes FindClass always sees.
static jclass
and_find_unity_player(JNIEnv *env)
{
	if (s_unity_player_cls) return (jclass)env->NewLocalRef(s_unity_player_cls);
	jclass cls = env->FindClass("com/unity3d/player/UnityPlayer");
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
	jstring name = env->NewStringUTF("com.unity3d.player.UnityPlayer");
	jclass up = (jclass)env->CallObjectMethod(loader, load, name);
	env->DeleteLocalRef(name);
	env->DeleteLocalRef(loader);
	if (!up || and_clear_exc(env)) return nullptr;
	return up;
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

#endif // __ANDROID__
