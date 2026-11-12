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
