# The crash reporter window on Linux, as a project of its own.
#
# RexCrash builds it as an ExternalProject, which keeps its SDL apart from the SDK's: the SDK builds SDL without
# the renderer, while this window needs SDL's renderer, with its software fallback, and the two can't share a
# CMake scope, as both define SDL3::SDL3-static. On Windows the window is ImGui over Direct3D 11 instead, built with
# the game.
#
#   REXCRASH_SENTRY_SOURCE  the sentry-native checkout the game's build uses (required)

cmake_minimum_required(VERSION 3.25)
project(rexcrashreporter C CXX)
include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(REXCRASH_SENTRY_SOURCE "" CACHE PATH "The sentry-native checkout the game's build uses")
if(NOT REXCRASH_SENTRY_SOURCE)
  message(FATAL_ERROR "rexcrashreporter: set REXCRASH_SENTRY_SOURCE to a sentry-native checkout")
endif()

# SDL 3.4.14, the SDK's version, with what a window needs and its renderer.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_RENDER ON CACHE BOOL "" FORCE)
FetchContent_Declare(sdl3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.14 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(sdl3)

# Sentry for sending what the player chose to send. The window catches no crashes, so no backend.
set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SENTRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SENTRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SENTRY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(SENTRY_BACKEND "none" CACHE STRING "" FORCE)
add_subdirectory(${REXCRASH_SENTRY_SOURCE} sentry EXCLUDE_FROM_ALL)
target_compile_options(sentry PRIVATE -w)

set(imgui ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/imgui)
add_executable(rexcrashreporter
  main.cpp ui_imgui.cpp ui_imgui_sdl.cpp
  ${imgui}/imgui.cpp ${imgui}/imgui_draw.cpp ${imgui}/imgui_tables.cpp ${imgui}/imgui_widgets.cpp
  ${imgui}/backends/imgui_impl_sdl3.cpp ${imgui}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(rexcrashreporter PRIVATE ${imgui} ${imgui}/backends)
target_compile_definitions(rexcrashreporter PRIVATE IMGUI_DISABLE_DEMO_WINDOWS)
target_link_libraries(rexcrashreporter PRIVATE sentry SDL3::SDL3-static)
