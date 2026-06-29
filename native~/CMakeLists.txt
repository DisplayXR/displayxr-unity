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
set(SOURCES
    displayxr_hooks.cpp
    displayxr_d3d11_backend.cpp
    displayxr_d3d12_backend.cpp
    displayxr_metal_backend.cpp
    displayxr_vulkan_backend.cpp
    displayxr_opengl_backend.cpp
    displayxr_opengles_backend.cpp
    displayxr_shared_state.cpp
    displayxr_readback.cpp
    displayxr_standalone.cpp
    displayxr_standalone_d3d11.cpp
    displayxr_standalone_d3d12.cpp
    displayxr_standalone_metal_backend.cpp
    displayxr_standalone_vulkan.cpp
    displayxr_standalone_opengl.cpp
    displayxr_standalone_opengles.cpp
    displayxr_vk_loader.cpp
    displayxr_window_space_ui.cpp
    displayxr_local2d.cpp
)

set(HEADERS
    displayxr_hooks.h
    displayxr_hooks_internal.h
    displayxr_shared_state.h
    displayxr_readback.h
    displayxr_extensions.h
    displayxr_standalone.h
    displayxr_standalone_internal.h
    displayxr_window_space_ui.h
    displayxr_local2d.h
)

# Apple-only: IOSurface helpers (Objective-C) + transparent-overlay NSWindow config
if(APPLE)
    list(APPEND SOURCES displayxr_metal.m displayxr_standalone_metal.m displayxr_macos.mm)
    list(APPEND HEADERS displayxr_metal.h displayxr_standalone_metal.h)
endif()

# Windows-only: overlay child window + D3D11/DXGI + dGPU preference exports
# user32 is required for SetLayeredWindowAttributes / WS_EX_LAYERED
# (transparent overlay mode, issue #57).
if(WIN32)
    list(APPEND SOURCES displayxr_win32.c displayxr_gpu_preference.cpp displayxr_unity_plugin.cpp)
    list(APPEND HEADERS displayxr_win32.h displayxr_vk_loader.h)
    # Custom IUnityXRDisplay Display Provider (epic #166, M1 — Windows/D3D12).
    # Compiled into the same displayxr_unity DLL; registered from UnityPluginLoad.
    # Does not disturb the hook path or the editor standalone path.
    list(APPEND SOURCES
        displayxr_xrprovider/displayxr_provider_session.cpp
        displayxr_xrprovider/displayxr_display_provider.cpp)
    list(APPEND HEADERS displayxr_xrprovider/displayxr_provider_session.h)
    link_libraries(user32 d3d11 d3d12 dxgi)
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

# Vulkan headers + vendored Unity native-plugin interface headers (Windows
# standalone Vulkan backend, issue #124).
if(WIN32)
    target_include_directories(displayxr_unity PRIVATE
        ${VULKAN_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/unity_pluginapi
    )
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
    set_source_files_properties(displayxr_metal.m displayxr_standalone_metal.m displayxr_macos.mm PROPERTIES
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
