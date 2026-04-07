
# renut - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(renut LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT REXSDK_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/../rexglue-sdk/CMakeLists.txt")
    set(REXSDK_DIR "${CMAKE_SOURCE_DIR}/../rexglue-sdk" CACHE PATH "Path to rexglue-sdk source tree" FORCE)
endif()

if(REXSDK_DIR AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    add_compile_options(-mssse3)
endif()

include(generated/rexglue.cmake)

# Sources
set(RENUT_SOURCES
    src/main.cpp
    src/renut_engine/hooks.cpp
    src/renut_engine/StopNSwap.cpp
)

if(WIN32)
list(APPEND RENUT_SOURCES icon/app.rc)
    add_executable(renut WIN32 ${RENUT_SOURCES})
else()
    add_executable(renut ${RENUT_SOURCES})
endif()

rexglue_setup_target(renut)

# Symlink the source assets folder into the build output directory so the
# runtime VFS can find game data regardless of which build config is active.
add_custom_command(TARGET renut POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:renut>/assets"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:renut>/assets"
    COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE_DIR:renut>/renut.toml"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        "${CMAKE_SOURCE_DIR}/renut.toml"
        "$<TARGET_FILE_DIR:renut>/renut.toml"
    COMMENT "Symlinking assets and renut.toml -> build output"
)
