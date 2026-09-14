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
    src/renut_engine/hooks.cpp
    src/renut_engine/StopNSwap.cpp
	src/renut_engine/frameHooks.cpp
    #src/renut_engine/fixes/mullhwucrash.cpp
	src/renut_engine/cvar_menu.cpp
	src/renut_engine/FPS.cpp
	src/renut_engine/shader_dump.cpp
	src/renut_engine/texture_tools.cpp
	src/renut_engine/frontend_debug_menu.cpp
	src/renut_engine/townsfolk.cpp
	src/renut_engine/mnk_controls.cpp
)

if(WIN32)
list(APPEND RENUT_SOURCES icon/app.rc)
    add_executable(renut WIN32 ${RENUT_SOURCES})
else()
    add_executable(renut ${RENUT_SOURCES})
endif()

rexglue_setup_target(renut)
