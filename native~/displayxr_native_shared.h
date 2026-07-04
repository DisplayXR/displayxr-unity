// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Companion header for displayxr_native_shared.cpp — the small pool of native
// glue re-homed out of the deleted OpenXR-hook TU that surviving provider-mode
// TUs still link against (the logger, plus the C# P/Invoke readback exports
// defined in the .cpp). Included by displayxr_native_shared.cpp,
// displayxr_window_space_ui.cpp, and displayxr_local2d.cpp.
//
// The former GraphicsBackend abstraction + s_real_*/s_next_gipa hook
// function-pointers lived here; they were removed in the Task-3 hook-backend
// cleanup (#166) — the provider owns graphics directly and never used them.

#pragma once

#include "displayxr_exports.h"
#include "displayxr_extensions.h"
#include "displayxr_shared_state.h"

#if defined(__APPLE__)
#include "displayxr_metal.h"
#elif defined(_WIN32)
#include <windows.h>   // Win32 logging + GetClientRect in displayxr_native_shared.cpp
#include "displayxr_win32.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// --- Logging helper (non-static so TUs in separate files can call it) ---
void displayxr_log(const char *fmt, ...);
