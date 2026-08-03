// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Hybrid-laptop GPU preference: driver export hints, process-environment
// plumbing, and adapter classification (#242).
//
// Background (#240): the runtime suggests the adapter a client must bind its
// session device on, defaulting to EnumAdapterByGpuPreference(0,
// HIGH_PERFORMANCE) — always the dGPU on an Optimus box. Whenever Unity's own
// adapter pick diverges from that suggestion, the eye bridge becomes
// cross-adapter and presents BLACK (ADR-032 / #223, reproduced on HW). Runtime
// v2.2.4 added DXR_D3D_FORCE_GPU=igpu|dgpu|<index> to steer the suggestion;
// this TU is how the plugin drives it so the two always agree.

#if defined(_WIN32)
#include <windows.h>
#include <stdlib.h>
#include <stdint.h>
#include <dxgi1_4.h>

// NVIDIA Optimus and AMD PowerXpress drivers look for the symbols below in the
// process that's about to render and steer it toward the discrete GPU when
// they're set non-zero. The drivers officially scan the *main executable*, but
// some driver versions also scan loaded modules — exporting from this plugin
// DLL is best-effort and not load-bearing.
//
// These are link-time constants and therefore CANNOT express the "Integrated"
// choice (#242) — an app that asks for the iGPU is steered by the per-exe
// UserGpuPreferences entry the build processor writes, which overrides these.
// Left at 1 so the unconfigured/Auto default keeps today's dGPU bias.
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

// Which adapter Unity's graphics device is on (displayxr_display_provider.cpp —
// its device getters have internal linkage, so this is the bridge). Valid from
// UnityPluginLoad onward, i.e. well before the XR loader's Initialize().
extern "C" int dxr_prov_unity_adapter_luid(int32_t *out_high, uint32_t *out_low);

// Adapter class, matching the runtime's igpu/dgpu keywords.
enum { DXR_GPU_UNKNOWN = 0, DXR_GPU_INTEGRATED = 1, DXR_GPU_DISCRETE = 2 };

// Classify Unity's adapter the SAME way the runtime classifies igpu/dgpu
// (oxr_d3d.cpp env_forced_d3d_adapter): by dedicated VRAM across hardware
// adapters — least = integrated, most = discrete. Deliberately NOT
// EnumAdapterByGpuPreference: a per-app UserGpuPreferences entry overrides the
// preference argument and reorders that enumeration, so the two sides would
// disagree exactly when the app has been pinned. VRAM ordering is registry-proof.
//
// Returns DXR_GPU_UNKNOWN when there is only ONE hardware adapter — on a
// single-GPU box the adapters cannot diverge, so there is nothing to steer and
// the caller correctly leaves the runtime's default alone.
extern "C" __declspec(dllexport) int dxr_prov_unity_gpu_class(void)
{
	// Via temporaries: LUID::HighPart is LONG, which is a distinct type from
	// int32_t to the compiler even though both are 32 bits.
	int32_t high = 0;
	uint32_t low = 0;
	if (!dxr_prov_unity_adapter_luid(&high, &low)) return DXR_GPU_UNKNOWN;
	LUID unity = {};
	unity.HighPart = (LONG)high;
	unity.LowPart = (DWORD)low;
	if (unity.HighPart == 0 && unity.LowPart == 0) return DXR_GPU_UNKNOWN;

	IDXGIFactory1 *factory = NULL;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)) || !factory)
		return DXR_GPU_UNKNOWN;

	LUID lo_luid = {}, hi_luid = {};
	SIZE_T lo_vram = 0, hi_vram = 0;
	int hw_count = 0;
	IDXGIAdapter1 *ad = NULL;
	for (UINT i = 0; factory->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 desc = {};
		if (SUCCEEDED(ad->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
			if (hw_count == 0 || desc.DedicatedVideoMemory < lo_vram) {
				lo_vram = desc.DedicatedVideoMemory; lo_luid = desc.AdapterLuid;
			}
			if (hw_count == 0 || desc.DedicatedVideoMemory > hi_vram) {
				hi_vram = desc.DedicatedVideoMemory; hi_luid = desc.AdapterLuid;
			}
			hw_count++;
		}
		ad->Release();
		ad = NULL;
	}
	factory->Release();

	if (hw_count < 2) return DXR_GPU_UNKNOWN; // single-GPU: nothing can diverge
	if (unity.HighPart == lo_luid.HighPart && unity.LowPart == lo_luid.LowPart)
		return DXR_GPU_INTEGRATED;
	if (unity.HighPart == hi_luid.HighPart && unity.LowPart == hi_luid.LowPart)
		return DXR_GPU_DISCRETE;
	return DXR_GPU_UNKNOWN; // 3+ adapters and Unity is on a middle one
}

// Set a process environment variable so that a LATER-LOADED DLL's getenv() sees it.
//
// LOAD-BEARING (#242): the runtime reads DXR_D3D_FORCE_GPU with getenv()
// (oxr_d3d.cpp:58). On Windows getenv() reads the CRT's OWN cached environment
// table, which SetEnvironmentVariableW does NOT update — so C#'s
// Environment.SetEnvironmentVariable alone is silently ignored by the runtime.
// _putenv_s updates the CRT copy; SetEnvironmentVariableW updates the Win32
// block that a statically-linked-CRT DLL initializes from when it loads. Doing
// BOTH covers the runtime however it links, provided this runs before the
// OpenXR loader loads the runtime DLL (i.e. before xrCreateInstance).
//
// Returns 1 on success, 0 on failure.
extern "C" __declspec(dllexport) int dxr_prov_set_env(const char *name, const char *value)
{
	if (!name || !name[0]) return 0;
	if (!value) value = "";
	int ok = (_putenv_s(name, value) == 0);
	// Widen for the Win32 block (ASCII names/values only — these are our own keys).
	WCHAR wname[128], wvalue[256];
	if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 128) > 0 &&
	    MultiByteToWideChar(CP_UTF8, 0, value, -1, wvalue, 256) > 0) {
		if (!SetEnvironmentVariableW(wname, wvalue[0] ? wvalue : NULL)) ok = 0;
	}
	return ok ? 1 : 0;
}

#endif // _WIN32
