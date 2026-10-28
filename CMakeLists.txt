# renut - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(renut LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)



set(RENUT_SOURCES
    src/main.cpp
    src/renut_engine/game/StopNSwap.cpp
	src/renut_engine/game/frameHooks.cpp
	src/renut_engine/perf/frame_stats.cpp
	src/renut_engine/game/frontend_debug_menu.cpp
	src/renut_engine/game/graphics_menu.cpp
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
	src/renut_engine/game/vehicle_limits.cpp
	src/renut_engine/game/status_dials.cpp
	src/renut_engine/game/notes.cpp
	src/renut_engine/game/stereo.cpp
	src/renut_engine/game/restored_parts.cpp
	src/renut_engine/game/banner.cpp
	src/renut_engine/game/vehicle_drivers.cpp
	src/renut_engine/online/trace.cpp
	src/renut_engine/online/rexliveless.cpp
	src/renut_engine/app/crash_dump.cpp
	src/renut_engine/app/dlc_install.cpp
	src/renut_engine/app/save_headers.cpp
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

# Linux only: the SDK's audio worker can run XAudio's render callback after
# the client is unregistered, which freezes Linux after the Microsoft logo.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND RENUT_SOURCES src/renut_engine/game/audio_client_guard.cpp)
endif()

if(WIN32)
list(APPEND RENUT_SOURCES icon/app.rc)
    add_executable(renut WIN32 ${RENUT_SOURCES})
else()
    add_executable(renut ${RENUT_SOURCES})
endif()

rexglue_setup_target(renut)

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

# miniaudio 0.11.21 (thirdparty/miniaudio, github.com/mackron/miniaudio,
# public domain / MIT-0) with stb_vorbis: the Stereo's music from the player's
# own files (src/renut_engine/game/stereo.cpp), on a sound device of its own.
enable_language(C)
add_library(renut_miniaudio STATIC "${CMAKE_SOURCE_DIR}/thirdparty/miniaudio/miniaudio_impl.c")
target_include_directories(renut_miniaudio PUBLIC "${CMAKE_SOURCE_DIR}/thirdparty/miniaudio")
if(MSVC)
    target_compile_options(renut_miniaudio PRIVATE /W0)
else()
    target_compile_options(renut_miniaudio PRIVATE -w)
endif()
if(UNIX)
    target_link_libraries(renut_miniaudio PRIVATE ${CMAKE_DL_LIBS} pthread m)
endif()
target_link_libraries(renut PRIVATE renut_miniaudio)

# Online play runs on RexLiveless through the Rexglue Guide
# (thirdparty/rexglue_guide): it answers the title's Xam / NetDll imports
# itself, so reNut's own EOS hooks (online/eos.cpp and friends) are left out.
include(thirdparty/rexglue_guide/CMakeLists.txt)
rexglue_guide_attach(renut)

# Native renderer (plume D3D12 + Vulkan, XenosRecomp RENUT_RECOMP shader cache).
include(src/renut_engine/gpu/gpu.cmake)
renut_gpu_setup(renut)

# Linux builds also pack renut into an AppImage (cmake/appimage.cmake).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(cmake/appimage.cmake)
endif()
