// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Hybrid-laptop GPU preference exports.
//
// NVIDIA Optimus and AMD PowerXpress drivers look for the symbols below in the
// process that's about to render and steer it toward the discrete GPU when
// they're set non-zero. The drivers officially scan the *main executable*, but
// some driver versions also scan loaded modules — exporting from this plugin
// DLL is best-effort and not load-bearing. The reliable path lives in
// Editor/DisplayXRGpuPreferenceBuildProcessor.cs, which writes a per-app
// UserGpuPreferences registry entry at build time.

#if defined(_WIN32)
#include <windows.h>

extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
