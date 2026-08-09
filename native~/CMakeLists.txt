# Copyright 2024-2026, DisplayXR contributors
# SPDX-License-Identifier: Apache-2.0
#
# Build the displayxr_unity native plugin for OpenXR function interception.
# Produces: displayxr_unity.dll (Windows) or displayxr_unity.bundle (macOS)

cmake_minimum_required(VERSION 3.16)
project(displayxr_unity LANGUAGES CXX C)

if(APPLE)
    enable_language(OBJC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# --- OpenXR SDK headers ---
# Try to find the OpenXR SDK. If not found, download headers.
find_package(OpenXR QUIET)

if(NOT OpenXR_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        openxr_sdk
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.0.34
        GIT_SHALLOW TRUE
    )
    # Only fetch headers — don't build the full SDK (avoids jsoncpp/loader deps)
    FetchContent_GetProperties(openxr_sdk)
    if(NOT openxr_sdk_POPULATED)
        FetchContent_Populate(openxr_sdk)
    endif()
    set(OPENXR_INCLUDE_DIR "${openxr_sdk_SOURCE_DIR}/include")
else()
    get_target_property(OPENXR_INCLUDE_DIR OpenXR::Headers INTERFACE_INCLUDE_DIRECTORIES)
endif()

# --- displayxr::math removed (#396 W7) ---
# The plugin no longer computes Kooima locally — the DisplayXR runtime owns the
# view-rig math via XR_DXR_view_rig, and the plugin consumes render-ready
# XrView{pose,fov}. The former displayxr-common (display3d/camera3d) dependency
# is gone; do NOT re-add it (see the no-vendored-math drift guard).

# --- Which platforms build the Vulkan backend ---
# Windows (#247) and desktop Linux (#249). Apple is excluded on purpose: Metal is
# the macOS backend (#202/#204). Android is a future leg (#249 follow-on) and is
# deliberately NOT switched on here — its runtime is out-of-process, so the
# in-process bridge this backend implements is the wrong shape for it.
if(WIN32 OR (UNIX AND NOT APPLE AND NOT ANDROID))
    set(DXR_VULKAN_BACKEND ON)
else()
    set(DXR_VULKAN_BACKEND OFF)
endif()

# --- Vulkan headers (issue #124, #247, #249) ---
# Header-only. We compile against these with VK_NO_PROTOTYPES and resolve every
# entry point from the Vulkan loader at runtime (displayxr_vk_loader.cpp:
# vulkan-1.dll / libvulkan.so.1), so there is NO find_package(Vulkan), NO import
# library to link, and NO Vulkan SDK needed in CI — and the shipped binary keeps
# no hard dependency on the Vulkan loader, so it still loads on machines that
# have none.
if(DXR_VULKAN_BACKEND)
    find_package(VulkanHeaders QUIET)
    if(VulkanHeaders_FOUND)
        get_target_property(VULKAN_INCLUDE_DIR Vulkan::Headers INTERFACE_INCLUDE_DIRECTORIES)
    else()
        include(FetchContent)
        FetchContent_Declare(
            vulkan_headers
            GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
            GIT_TAG v1.3.280
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(vulkan_headers)
        if(NOT vulkan_headers_POPULATED)
            FetchContent_Populate(vulkan_headers)
        endif()
        set(VULKAN_INCLUDE_DIR "${vulkan_headers_SOURCE_DIR}/include")
    endif()
endif()

# --- Plugin sources ---
# Hook-removal pass: displayxr_hooks.cpp and the standalone (SA) session TUs are
# no longer compiled — the custom IUnityXRDisplay provider (displayxr_xrprovider/)
# is the shipping path. The handful of symbols surviving TUs still needed from the
# hook TU were re-homed into displayxr_native_shared.cpp. The SA files remain on
# disk (dormant quilt-render seed) but are not in SOURCES.
#
# Task 3 (#166): the 6 GraphicsBackend TUs (d3d11/d3d12/metal/vulkan/opengl/
# opengles) were dropped — they were only ever instantiated by the deleted hook,
# and the provider owns its D3D12 device + swapchains + wsui/Local2D layers
# directly (displayxr_xrprovider/displayxr_provider_session.cpp). The readback
# subsystem went with them (its only writers were the backends' window-binding
# injector). wsui/local2d retain only their C# P/Invoke setters (the provider
# reads the pending state); their dead hooked/standalone submission paths are gone.
set(SOURCES
    displayxr_native_shared.cpp
    displayxr_shared_state.cpp
    displayxr_vk_loader.cpp
    displayxr_window_space_ui.cpp
    displayxr_local2d.cpp
    displayxr_unity_plugin.cpp
    # Custom IUnityXRDisplay Display Provider (epic #166; macOS port #202).
    # Cross-platform TUs: D3D backends behind _WIN32, Metal behind __APPLE__.
    displayxr_xrprovider/displayxr_provider_session.cpp
    displayxr_xrprovider/displayxr_display_provider.cpp
    # XR SDK pre-init provider (#247): XRSDKPreInit export, compiled everywhere so
    # the boot.config xrsdk-pre-init-library entry never dangles; VK-only in effect.
    displayxr_xrprovider/displayxr_provider_preinit.cpp
)

set(HEADERS
    displayxr_exports.h
    displayxr_native_shared.h
    displayxr_shared_state.h
    displayxr_extensions.h
    displayxr_window_space_ui.h
    displayxr_local2d.h
)

# Apple-only: IOSurface helpers (Objective-C) + transparent-overlay NSWindow config
if(APPLE)
    list(APPEND SOURCES displayxr_metal.m displayxr_macos.mm
        displayxr_xrprovider/displayxr_provider_gfx_metal.mm)
    list(APPEND HEADERS displayxr_xrprovider/displayxr_provider_gfx_metal.h)
    list(APPEND HEADERS displayxr_metal.h)
endif()

# Windows-only: overlay child window + D3D11/DXGI + dGPU preference exports
# user32 is required for SetLayeredWindowAttributes / WS_EX_LAYERED
# (transparent overlay mode, issue #57).
list(APPEND HEADERS displayxr_xrprovider/displayxr_provider_session.h)

if(WIN32)
    list(APPEND SOURCES displayxr_win32.c displayxr_gpu_preference.cpp)
    list(APPEND HEADERS displayxr_win32.h)
    # dwmapi + advapi32: MSVC links them via #pragma comment(lib, ...) in the TUs,
    # but MinGW (the macOS cross-compile check) ignores that pragma — list them here.
    link_libraries(user32 d3d11 d3d12 dxgi dwmapi advapi32 d3dcompiler)
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    # Desktop-Linux platform glue (#249): the shell-mode stub the shared provider
    # code references unconditionally (macOS ships the same stub), plus the X11
    # helper that finds the player's own window so Linux can be a HANDLE app
    # (XR_DXR_xlib_window_binding) like Windows and macOS.
    list(APPEND SOURCES displayxr_linux.c)
endif()

# Provider Vulkan backend (#247 Windows, #249 Linux): enable2 session device +
# external-memory eye bridge to Unity's VkDevice. The TU is platform-parametric —
# the only per-OS part is the external-memory handle flavour (OPAQUE_WIN32 HANDLEs
# vs OPAQUE_FD file descriptors) — so it is listed once here rather than under a
# per-platform branch. Guarded internally by ENABLE_VULKAN, set below.
if(DXR_VULKAN_BACKEND)
    list(APPEND SOURCES displayxr_xrprovider/displayxr_provider_gfx_vulkan.cpp)
    list(APPEND HEADERS displayxr_vk_loader.h
        displayxr_xrprovider/displayxr_provider_gfx_vulkan.h)
endif()

# --- Build shared library ---
# Use MODULE on macOS so BUNDLE property works (creates .bundle directory)
if(APPLE)
    add_library(displayxr_unity MODULE ${SOURCES} ${HEADERS})
else()
    add_library(displayxr_unity SHARED ${SOURCES} ${HEADERS})
endif()

target_include_directories(displayxr_unity PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OPENXR_INCLUDE_DIR}
)

# Vendored Unity native-plugin interface headers (all platforms — the display
# provider TUs compile everywhere; #202).
target_include_directories(displayxr_unity PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/unity_pluginapi
)

# Vulkan headers (issue #124, #247, #249).
if(DXR_VULKAN_BACKEND)
    target_include_directories(displayxr_unity PRIVATE ${VULKAN_INCLUDE_DIR})
endif()

# Symbol visibility
if(MSVC)
    target_compile_definitions(displayxr_unity PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(displayxr_unity PRIVATE -fvisibility=hidden)
endif()

# Enable the Vulkan backend where DXR_VULKAN_BACKEND selected it (Windows #247,
# desktop Linux #249). Excludes Apple — Metal is the macOS backend (#202/#204).
if(DXR_VULKAN_BACKEND)
    target_compile_definitions(displayxr_unity PRIVATE ENABLE_VULKAN)
endif()

# --- Apple framework linking ---
if(APPLE)
    find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    target_link_libraries(displayxr_unity PRIVATE
        ${IOSURFACE_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
        ${APPKIT_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
    )
    # Enable ARC for Objective-C / Objective-C++ files
    set_source_files_properties(displayxr_metal.m displayxr_macos.mm
        displayxr_xrprovider/displayxr_provider_gfx_metal.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
endif()

# --- Install to Unity plugin directories ---
if(WIN32)
    # MinGW cross-compile: output stays in build dir (compile check only, not shipped)
    # MSVC (CI): output goes to Unity plugin directory
    if(MINGW)
        set(PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}")
        target_link_options(displayxr_unity PRIVATE -static-libgcc -static-libstdc++ -static -lpthread)
    else()
        set(PLUGIN_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Runtime/Plugins/Windows/x64")
    endif()
    # PREFIX "" prevents MinGW from adding "lib" prefix
    set_target_properties(displayxr_unity PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PLUGIN_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${PLUGIN_OUTPUT_DIR}"
    )
elseif(APPLE)
    set(PLUGIN_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Runtime/Plugins/macOS")
    set_target_properties(displayxr_unity PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.displayxr.unity.native"
    )
else()
    # Desktop Linux (#249). Unity's Linux plugin importer expects the SONAME-style
    # `lib` prefix, so — unlike Windows — we keep CMake's default naming and land
    # `libdisplayxr_unity.so` in the x86_64 plugin folder.
    set(PLUGIN_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Runtime/Plugins/Linux/x86_64")
    set_target_properties(displayxr_unity PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${PLUGIN_OUTPUT_DIR}"
    )
    # The plugin resolves Vulkan AND Xlib entry points through dlopen/dlsym
    # (displayxr_vk_loader.cpp, displayxr_linux.c), so it needs libdl but must NOT
    # link libvulkan or libX11 — that is what keeps the .so loadable on a box with
    # no Vulkan ICD and no X11.
    target_link_libraries(displayxr_unity PRIVATE ${CMAKE_DL_LIBS})

    # --no-undefined: make an unresolved symbol a LINK ERROR instead of a runtime
    # surprise. Learned the hard way (#249): a Linux shared library links happily
    # with dangling references and only dies when the loader binds them, so CI
    # reported green on a .so that could not load at all — `undefined symbol:
    # displayxr_is_shell_mode` on the target box, inside GfxStart. Windows (MSVC)
    # and macOS (two-level namespace) both reject this by default; Linux is the
    # odd one out, so ask for the same strictness explicitly.
    target_link_options(displayxr_unity PRIVATE -Wl,--no-undefined)
endif()

# --- Summary ---
message(STATUS "displayxr_unity: OpenXR headers from ${OPENXR_INCLUDE_DIR}")
message(STATUS "displayxr_unity: Output to ${PLUGIN_OUTPUT_DIR}")
