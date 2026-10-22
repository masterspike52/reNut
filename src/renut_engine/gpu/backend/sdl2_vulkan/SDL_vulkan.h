/**
 * @file    gpu/backend/sdl2_vulkan/SDL_vulkan.h
 * @brief   The SDL2 Vulkan API plume's PLUME_SDL_VULKAN_ENABLED path calls, for
 *          reNut's SDL3 (Linux).
 *
 * plume (read-only) makes its Linux Vulkan surface either from Xlib handles,
 * which only works on X11 (or XWayland), or, with PLUME_SDL_VULKAN_ENABLED,
 * through SDL, written against SDL2: it includes <SDL_vulkan.h> from the
 * SDL2::SDL2 target and calls SDL_Vulkan_CreateSurface(window, instance,
 * &surface) and SDL_Vulkan_GetInstanceExtensions(window, &count, names). On
 * Linux gpu.cmake builds plume that way with this directory as SDL2::SDL2's
 * includes: these two go to reNut's (native_window.cpp), which make the
 * surface on the SDK's window, Wayland or X11, whichever video driver SDL
 * chose. SDL's own SDL_Vulkan_CreateSurface would refuse it: the SDK makes
 * its window without SDL_WINDOW_VULKAN.
 */
#pragma once

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>

#ifdef SDL_bool
#undef SDL_bool
#endif
#ifdef SDL_TRUE
#undef SDL_TRUE
#endif
#ifdef SDL_FALSE
#undef SDL_FALSE
#endif
typedef bool SDL_bool;
#define SDL_TRUE true
#define SDL_FALSE false

// Makes the Vulkan surface of 'window' (VK_KHR_wayland_surface or
// VK_KHR_xlib_surface, by SDL's video driver).
bool renut_SDL2_Vulkan_CreateSurface(SDL_Window *window, VkInstance instance, VkSurfaceKHR *surface);
// The instance extensions that surface needs ('names' nullptr: only the count).
bool renut_SDL2_Vulkan_GetInstanceExtensions(SDL_Window *window, unsigned int *count, const char **names);

#define SDL_Vulkan_CreateSurface renut_SDL2_Vulkan_CreateSurface
#define SDL_Vulkan_GetInstanceExtensions renut_SDL2_Vulkan_GetInstanceExtensions
