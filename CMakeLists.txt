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
	src/renut_engine/game/time_of_day.cpp
	src/renut_engine/game/water_reflection.cpp
	src/renut_engine/game/mouse_camera.cpp
	src/renut_engine/game/keyboard_input.cpp
	src/renut_engine/game/custom_parts.cpp
	src/renut_engine/game/part_classes.cpp
	src/renut_engine/game/part_scripts.cpp
	src/renut_engine/game/part_api.cpp
	src/renut_engine/game/custom_worlds.cpp
	src/renut_engine/game/custom_npcs.cpp
	src/renut_engine/game/custom_shapes.cpp
	src/renut_engine/game/custom_materials.cpp
	src/renut_engine/game/sound_api.cpp
	src/renut_engine/game/menu_api.cpp
	src/renut_engine/game/mod_data.cpp
	src/renut_engine/game/renut_lua.cpp
	src/renut_engine/game/mod_bundles.cpp
	src/renut_engine/game/vehicle_limits.cpp
	src/renut_engine/game/status_dials.cpp
	src/renut_engine/game/notes.cpp
	src/renut_engine/game/cheats.cpp
	src/renut_engine/game/flag_editor.cpp
	src/renut_engine/game/walk_up_slopes.cpp
	src/renut_engine/game/challenge_replay.cpp
	src/renut_engine/game/stereo.cpp
	src/renut_engine/game/restored_parts.cpp
	src/renut_engine/game/banner.cpp
	src/renut_engine/game/vehicle_drivers.cpp
	src/renut_engine/game/startup_logo.cpp
	src/renut_engine/online/rexliveless.cpp
	src/renut_engine/online/net_api.cpp
	src/renut_engine/app/renut_settings.cpp
	src/renut_engine/app/xam_workspace.cpp
	src/renut_engine/app/crash_dump.cpp
	src/renut_engine/app/dlc_install.cpp
	src/renut_engine/app/game_setup.cpp
	src/renut_engine/app/save_headers.cpp
	src/renut_engine/app/updater.cpp
	src/renut_engine/app/workshop.cpp
	src/renut_engine/ui/launcher_menu.cpp
	src/renut_engine/mods/custom_parts.cpp
	src/renut_engine/mods/json.cpp
	src/renut_engine/mods/mod_device.cpp
	src/renut_engine/mods/mod_loader.cpp
	src/renut_engine/mods/nbpatch.cpp
	src/renut_engine/mods/sha256.cpp
	src/renut_engine/mods/xcompress.cpp
	src/renut_engine/mods/zip_reader.cpp
	src/hooks.cpp
	src/renut_engine/game/audio_client_guard.cpp
)

if(WIN32)
list(APPEND RENUT_SOURCES icon/app.rc)
    add_executable(renut WIN32 ${RENUT_SOURCES})
else()
    add_executable(renut ${RENUT_SOURCES})
endif()

# No Xenos GPU modules: reNut draws through its own renderer.
rexglue_setup_target(renut EXCLUDE rexgpu-xenos-null rexgpu-xenos-vulkan rexgpu-xenos-d3d12)

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
if(RENUT_RELEASE)
    target_compile_definitions(renut PRIVATE RENUT_RELEASE_BUILD)
endif()

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

if(NOT REXFABRIC_SDK_TEMPLATES AND rexglue_DIR)
    set(renut_sdk_root "${rexglue_DIR}")
    foreach(level RANGE 1 6)
        cmake_path(GET renut_sdk_root PARENT_PATH renut_sdk_root)
    endforeach()
    set(REXFABRIC_SDK_TEMPLATES "${renut_sdk_root}/resources/templates" CACHE PATH
        "The rexglue-sdk's resources/templates folder, matching the SDK the project builds with" FORCE)
endif()
include(thirdparty/RexGlass/CMakeLists.txt)
rexglass_attach(renut)
include(thirdparty/RexFabric/CMakeLists.txt)
rexfabric_attach(renut WITH_GUIDE WITH_NODE_EDITOR)
rexfabric_template_fragment(codegen/pch_h BEFORE "#define REX_SHIM_HEAP" FILE ${CMAKE_SOURCE_DIR}/config/templates/pch_image_size.inja)
set(renut_asset_dir "${CMAKE_BINARY_DIR}/renut_assets")
foreach(picture launcher_background workshop_background launcher_logo launcher_gear_top
                launcher_gear_middle launcher_gear_corner)
    set(source "${CMAKE_SOURCE_DIR}/icon/${picture}.png")
    set(inc "${renut_asset_dir}/${picture}.png.inc")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    if(NOT EXISTS "${inc}" OR "${source}" IS_NEWER_THAN "${inc}")
        file(READ "${source}" hex HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
        file(WRITE "${inc}" "${hex}\n")
    endif()
endforeach()
target_include_directories(renut PRIVATE "${renut_asset_dir}")
include(thirdparty/RexCrash/CMakeLists.txt)
rexcrash_attach(renut
    DSN "https://953e4dacb8e759c2f49cc6dd5ba22a57@o4512343007952896.ingest.us.sentry.io/4512343096360960"
    RELEASE "${RENUT_SENTRY_RELEASE}")
target_compile_definitions(renut PRIVATE "RENUT_SENTRY_ENVIRONMENT=\"${RENUT_SENTRY_ENVIRONMENT}\"")
include(cmake/sentry_symbols.cmake)
renut_sentry_symbols(renut ORG madlad-inc PROJECT renut)


include(src/renut_engine/gpu/gpu.cmake)
renut_gpu_setup(renut)


set(RENUT_LAUNCHER_MUSIC "" CACHE FILEPATH
    "An already-made launcher music (launcher_music.ogg); empty: made from the game's assets with ffmpeg")
set(renut_music_inc "${renut_asset_dir}/launcher_music.ogg.inc")
if(RENUT_LAUNCHER_MUSIC)
    if(NOT EXISTS "${RENUT_LAUNCHER_MUSIC}")
        message(FATAL_ERROR "RENUT_LAUNCHER_MUSIC: no ${RENUT_LAUNCHER_MUSIC}")
    endif()
    add_custom_command(
        OUTPUT "${renut_music_inc}"
        COMMAND "${CMAKE_COMMAND}" "-DIN=${RENUT_LAUNCHER_MUSIC}" "-DOUT=${renut_music_inc}"
                -P "${CMAKE_SOURCE_DIR}/cmake/embed_string.cmake"
        DEPENDS "${RENUT_LAUNCHER_MUSIC}" "${CMAKE_SOURCE_DIR}/cmake/embed_string.cmake"
        COMMENT "Embedding the launcher music ${RENUT_LAUNCHER_MUSIC}"
        VERBATIM)
else()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(RENUT_FFMPEG ffmpeg DOC "ffmpeg, which makes the launcher music from the game's assets")
    if(NOT RENUT_FFMPEG)
        message(FATAL_ERROR "The launcher music is made from the game's assets with ffmpeg: install it "
                            "(winget install ffmpeg, or apt install ffmpeg), set RENUT_FFMPEG to it, or set "
                            "RENUT_LAUNCHER_MUSIC to a launcher_music.ogg another build made")
    endif()
    set(renut_music_bundle "${RENUT_ASSETS_DIR}/Bundle/50/685374")
    if(NOT EXISTS "${renut_music_bundle}")
        message(FATAL_ERROR "No ${renut_music_bundle} to make the launcher music from: set "
                            "RENUT_ASSETS_DIR to the game's assets directory")
    endif()
    set(renut_music "${renut_asset_dir}/launcher_music.ogg")
    add_custom_command(
        OUTPUT "${renut_music_inc}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/make_launcher_music.py"
                "${RENUT_ASSETS_DIR}" "${renut_music}" --ffmpeg "${RENUT_FFMPEG}"
        COMMAND "${CMAKE_COMMAND}" "-DIN=${renut_music}" "-DOUT=${renut_music_inc}"
                -P "${CMAKE_SOURCE_DIR}/cmake/embed_string.cmake"
        DEPENDS "${renut_music_bundle}" "${CMAKE_SOURCE_DIR}/tools/make_launcher_music.py"
                "${CMAKE_SOURCE_DIR}/cmake/embed_string.cmake"
        COMMENT "Making the launcher music from ${renut_music_bundle}"
        VERBATIM)
endif()
target_sources(renut PRIVATE "${renut_music_inc}")
set_source_files_properties("${renut_music_inc}" PROPERTIES HEADER_FILE_ONLY ON GENERATED ON)
set_source_files_properties(src/renut_engine/ui/launcher_menu.cpp PROPERTIES OBJECT_DEPENDS "${renut_music_inc}")

# The renut Lua API's definitions beside the executable (lua_api), which RexFabric's node editor makes nodes from.
add_custom_command(TARGET renut POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:renut>/lua_api"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/src/renut_engine/lua_api/renut.lua"
            "$<TARGET_FILE_DIR:renut>/lua_api/renut.lua"
    VERBATIM)


if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(cmake/appimage.cmake)
endif()
