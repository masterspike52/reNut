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
	src/renut_engine/game/cheats.cpp
	src/renut_engine/game/flag_editor.cpp
	src/renut_engine/game/walk_up_slopes.cpp
	src/renut_engine/game/stereo.cpp
	src/renut_engine/game/restored_parts.cpp
	src/renut_engine/game/banner.cpp
	src/renut_engine/game/vehicle_drivers.cpp
	src/renut_engine/game/startup_logo.cpp
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

# The build's name on the RexLiveless leaderboards and its Sentry release.
# Builds are "reNut dev" (renut@dev), which the server takes only from an
# admin's account; a release build (RENUT_RELEASE: the *-dist presets, which
# tools/release.ps1 builds after raising version.txt) is
# "reNut <version.txt>" (renut@<version>), which the server refuses below its
# minimum, keeping old releases off the leaderboards.
option(RENUT_RELEASE "A release build: versioned by version.txt (else a dev build)" OFF)
file(STRINGS "${CMAKE_SOURCE_DIR}/version.txt" RENUT_VERSION LIMIT_COUNT 1 REGEX "^[0-9]+(\\.[0-9]+)*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/version.txt")
if(RENUT_RELEASE)
    if(NOT RENUT_VERSION)
        message(FATAL_ERROR "version.txt holds no version (e.g. 1.2.3)")
    endif()
    set(RENUT_BUILD "reNut ${RENUT_VERSION}")
    set(RENUT_SENTRY_RELEASE "renut@${RENUT_VERSION}")
    set(RENUT_SENTRY_ENVIRONMENT "production")
else()
    set(RENUT_BUILD "reNut dev")
    set(RENUT_SENTRY_RELEASE "renut@dev")
    set(RENUT_SENTRY_ENVIRONMENT "development")
endif()
message(STATUS "reNut build: ${RENUT_BUILD}")
target_compile_definitions(renut PRIVATE "RENUT_BUILD=\"${RENUT_BUILD}\"")

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

include(thirdparty/rexglue_guide/CMakeLists.txt)
rexglue_guide_attach(renut)


include(thirdparty/sentrywork/CMakeLists.txt)
sentrywork_attach(renut
    DSN "https://953e4dacb8e759c2f49cc6dd5ba22a57@o4512343007952896.ingest.us.sentry.io/4512343096360960"
    RELEASE "${RENUT_SENTRY_RELEASE}" ENVIRONMENT "${RENUT_SENTRY_ENVIRONMENT}"
    ORG madlad-inc PROJECT renut)


include(src/renut_engine/gpu/gpu.cmake)
renut_gpu_setup(renut)


if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(cmake/appimage.cmake)
endif()
