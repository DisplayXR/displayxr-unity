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
# view-rig math via XR_EXT_view_rig, and the plugin consumes render-ready
# XrView{pose,fov}. The former displayxr-common (display3d/camera3d) dependency
# is gone; do NOT re-add it (see the no-vendored-math drift guard).

# --- Vulkan headers (Windows standalone backend, issue #124) ---
# Header-only. We compile against these with VK_NO_PROTOTYPES and resolve every
# entry point from vulkan-1.dll at runtime (displayxr_vk_loader.cpp), so there
# is NO find_package(Vulkan), NO vulkan-1.lib link, and NO Vulkan SDK needed in
# CI — and the shipped DLL keeps no hard dependency on vulkan-1.dll.
if(WIN32)
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
    list(APPEND HEADERS displayxr_win32.h displayxr_vk_loader.h)
    # dwmapi + advapi32: MSVC links them via #pragma comment(lib, ...) in the TUs,
    # but MinGW (the macOS cross-compile check) ignores that pragma — list them here.
    link_libraries(user32 d3d11 d3d12 dxgi dwmapi advapi32)
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

# Vulkan headers (Windows standalone Vulkan backend, issue #124).
if(WIN32)
    target_include_directories(displayxr_unity PRIVATE ${VULKAN_INCLUDE_DIR})
endif()

# Symbol visibility
if(MSVC)
    target_compile_definitions(displayxr_unity PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(displayxr_unity PRIVATE -fvisibility=hidden)
endif()

# Enable Vulkan backend on Windows. The stub in displayxr_vulkan_backend.cpp
# is auto-active on Android/Linux but gated behind ENABLE_VULKAN on Windows
# so opt-in is explicit. Needed for #122 (test whether Vulkan unlocks MSAA
# on the XR eye RT). Excludes Apple — Vulkan on macOS is out of scope for
# this plugin.
if(WIN32)
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
endif()

# --- Summary ---
message(STATUS "displayxr_unity: OpenXR headers from ${OPENXR_INCLUDE_DIR}")
message(STATUS "displayxr_unity: Output to ${PLUGIN_OUTPUT_DIR}")
