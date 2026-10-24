# renut - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(renut LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(ANDROID)
    # The stock rexglue SDK builds for Android with what it expects from the
    # platform that bionic and the NDK do not have: headers in android/sdk_compat
    # (one force-included), defined in src/renut_engine/app/android_sdk_compat.cpp.
    # Its UI asks pkg-config for X11 and Wayland on every Linux build: none here.
    # It links pthread and rt, which bionic keeps in libc: empty archives.
    set(RENUT_SDK_COMPAT "${CMAKE_CURRENT_SOURCE_DIR}/android/sdk_compat")
    include_directories(BEFORE "${RENUT_SDK_COMPAT}")
    link_directories(BEFORE "${RENUT_SDK_COMPAT}/lib")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-include${RENUT_SDK_COMPAT}/renut_sdk_compat.h>")
    set(ENV{PKG_CONFIG_LIBDIR} "${RENUT_SDK_COMPAT}/pkgconfig")
endif()

include(generated/rexglue.cmake)

if(ANDROID)
    # librexruntime.so needs them as much as librenut.so.
    target_sources(rexcore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/renut_engine/app/android_sdk_compat.cpp")
    # Codegen runs the SDK's rexglue, which this build would make for Android:
    # its link writes a script running the host's instead
    # (REXGLUE_CODEGEN_EXECUTABLE, android/configure.sh).
    if(NOT REXGLUE_CODEGEN_EXECUTABLE)
        message(FATAL_ERROR "Set REXGLUE_CODEGEN_EXECUTABLE to a host build of rexglue")
    endif()
    set_property(TARGET rexglue PROPERTY RULE_LAUNCH_LINK
                 "${CMAKE_CURRENT_SOURCE_DIR}/android/host_rexglue.sh ${REXGLUE_CODEGEN_EXECUTABLE}")
endif()



set(RENUT_SOURCES
    src/main.cpp
    src/renut_engine/game/StopNSwap.cpp
	src/renut_engine/game/frameHooks.cpp
	src/renut_engine/perf/frame_stats.cpp
	src/renut_engine/game/frontend_debug_menu.cpp
	src/renut_engine/game/graphics_menu.cpp
	src/renut_engine/game/coop_menu.cpp
	src/renut_engine/game/env_map_pool.cpp
	src/renut_engine/game/level_pools.cpp
	src/renut_engine/game/controllers.cpp
	src/renut_engine/game/thread_slots.cpp
	src/renut_engine/game/lod.cpp
	src/renut_engine/game/field_of_view.cpp
	src/renut_engine/game/mouse_camera.cpp
	src/renut_engine/game/keyboard_input.cpp
	src/renut_engine/game/custom_parts.cpp
	src/renut_engine/game/part_classes.cpp
	src/renut_engine/game/part_scripts.cpp
	src/renut_engine/game/effect_log.cpp
	src/renut_engine/game/mod_bundles.cpp
	src/renut_engine/game/part_attach_log.cpp
	src/renut_engine/online/identity.cpp
	src/renut_engine/online/coop.cpp
	src/renut_engine/online/trace.cpp
	src/renut_engine/online/eos.cpp
	src/renut_engine/online/friends.cpp
	src/renut_engine/online/sessions.cpp
	src/renut_engine/online/qos.cpp
	src/renut_engine/online/transport.cpp
	src/renut_engine/app/crash_dump.cpp
	src/renut_engine/app/dlc_install.cpp
	src/renut_engine/mods/custom_parts.cpp
	src/renut_engine/mods/json.cpp
	src/renut_engine/mods/mod_device.cpp
	src/renut_engine/mods/mod_loader.cpp
	src/renut_engine/mods/nbpatch.cpp
	src/renut_engine/mods/sha256.cpp
	src/renut_engine/mods/xcompress.cpp
	src/renut_engine/mods/zip_reader.cpp
	src/hooks.cpp
)

if(WIN32)
list(APPEND RENUT_SOURCES icon/app.rc)
    add_executable(renut WIN32 ${RENUT_SOURCES})
elseif(ANDROID)
    # SDL's Java activity loads librenut.so and calls SDL_main (android/).
    add_library(renut SHARED ${RENUT_SOURCES})
else()
    add_executable(renut ${RENUT_SOURCES})
endif()

rexglue_setup_target(renut)

if(ANDROID)
    # reNut's SDL_main replaces the SDK's entry point (android_main.cpp).
    get_target_property(renut_sources renut SOURCES)
    list(FILTER renut_sources EXCLUDE REGEX "windowed_app_main_sdl\\.cpp$")
    set_target_properties(renut PROPERTIES SOURCES "${renut_sources}")
endif()

# Lua 5.4 (thirdparty/lua-5.4.7, lua.org, MIT) for mods' part scripts
# (src/renut_engine/game/part_scripts.cpp). Built as C++: script errors unwind
# with exceptions rather than longjmp across reNut's C++ frames.
set(RENUT_LUA_DIR "${CMAKE_SOURCE_DIR}/thirdparty/lua-5.4.7/src")
file(GLOB renut_lua_sources "${RENUT_LUA_DIR}/*.c")
list(REMOVE_ITEM renut_lua_sources "${RENUT_LUA_DIR}/lua.c" "${RENUT_LUA_DIR}/luac.c")
set_source_files_properties(${renut_lua_sources} PROPERTIES LANGUAGE CXX)
add_library(renut_lua STATIC ${renut_lua_sources})
target_include_directories(renut_lua PUBLIC "${RENUT_LUA_DIR}")
if(MSVC)
    target_compile_options(renut_lua PRIVATE /W0 /TP)
else()
    target_compile_options(renut_lua PRIVATE -w -x c++)
endif()
target_link_libraries(renut PRIVATE renut_lua)

if(ANDROID)
    # Custom Vulkan drivers (Turnip) for Adreno; its hook libraries go into the
    # APK next to librenut.so (android/build.sh).
    add_subdirectory(thirdparty/libadrenotools EXCLUDE_FROM_ALL)
    target_link_libraries(renut PRIVATE adrenotools)
    add_dependencies(renut main_hook file_redirect_hook gsl_alloc_hook)
    target_sources(renut PRIVATE src/renut_engine/app/android_main.cpp src/renut_engine/app/android_driver.cpp src/renut_engine/app/android_content.cpp src/renut_engine/app/xiso.cpp
        src/renut_engine/ui/touch_controls.cpp)
endif()

# Online play runs on Epic Online Services (src/renut_engine/online/eos.cpp):
# sign-in by Device ID and peer-to-peer traffic. The EOS SDK comes from the Epic
# Developer Portal; without it renut builds without online play. The product's
# IDs (Developer Portal: Product Settings) come from cmake/eos_credentials.cmake
# when it is there (kept out of version control: it holds the client secret),
# else from -D, and are built in.
include("${CMAKE_SOURCE_DIR}/cmake/eos_credentials.cmake" OPTIONAL)
set(RENUT_EOS_DIR "${CMAKE_SOURCE_DIR}/thirdparty/EOS-SDK-58105819-Release-v1.19.2.1/SDK" CACHE PATH
    "The EOS SDK's SDK folder (Include/eos_sdk.h inside)")
set(RENUT_EOS_PRODUCT_ID "" CACHE STRING "EOS product ID")
set(RENUT_EOS_SANDBOX_ID "" CACHE STRING "EOS sandbox ID")
set(RENUT_EOS_DEPLOYMENT_ID "" CACHE STRING "EOS deployment ID")
set(RENUT_EOS_CLIENT_ID "" CACHE STRING "EOS client ID (a client policy allowing Connect and P2P)")
set(RENUT_EOS_CLIENT_SECRET "" CACHE STRING "EOS client secret")
if(EXISTS "${RENUT_EOS_DIR}/Include/eos_sdk.h" AND NOT ANDROID)
    if(WIN32)
        set(renut_eos_lib "${RENUT_EOS_DIR}/Lib/EOSSDK-Win64-Shipping.lib")
        set(renut_eos_runtime "${RENUT_EOS_DIR}/Bin/EOSSDK-Win64-Shipping.dll")
    else()
        set(renut_eos_lib "${RENUT_EOS_DIR}/Bin/libEOSSDK-Linux-Shipping.so")
        set(renut_eos_runtime "${renut_eos_lib}")
    endif()
    target_include_directories(renut SYSTEM PRIVATE "${RENUT_EOS_DIR}/Include")
    target_link_libraries(renut PRIVATE "${renut_eos_lib}")
    target_compile_definitions(renut PRIVATE
        RENUT_EOS=1
        "RENUT_EOS_PRODUCT_ID=\"${RENUT_EOS_PRODUCT_ID}\""
        "RENUT_EOS_SANDBOX_ID=\"${RENUT_EOS_SANDBOX_ID}\""
        "RENUT_EOS_DEPLOYMENT_ID=\"${RENUT_EOS_DEPLOYMENT_ID}\""
        "RENUT_EOS_CLIENT_ID=\"${RENUT_EOS_CLIENT_ID}\""
        "RENUT_EOS_CLIENT_SECRET=\"${RENUT_EOS_CLIENT_SECRET}\"")
    add_custom_command(TARGET renut POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${renut_eos_runtime}" "$<TARGET_FILE_DIR:renut>"
        VERBATIM)
    if(RENUT_EOS_PRODUCT_ID STREQUAL "")
        message(STATUS "renut: EOS SDK found, but no RENUT_EOS_* product IDs: online play reports it")
    else()
        message(STATUS "renut: online play on EOS (${RENUT_EOS_DIR})")
    endif()
else()
    message(STATUS "renut: no EOS SDK at ${RENUT_EOS_DIR}: building without online play")
endif()

# Native renderer (plume D3D12 + Vulkan, XenosRecomp RENUT_RECOMP shader cache).
include(src/renut_engine/gpu/gpu.cmake)
renut_gpu_setup(renut)

# Linux builds also pack renut into an AppImage (cmake/appimage.cmake).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(cmake/appimage.cmake)
endif()
