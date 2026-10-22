/**
 * @file    gpu/backend/native_window.h
 * @brief   The game window, found from the platform rather than the SDK.
 */
#pragma once

#include <cstdint>

#include <plume_render_interface.h>

#if !defined(_WIN32)
struct SDL_Window;
#endif

namespace renut::gpu {

// Windows: the HWND of this process's game window (the SDK's SDL window).
// Linux: the SDL window itself (plume is built with PLUME_SDL_VULKAN_ENABLED;
// its surface, Wayland or X11, is made in native_window.cpp).
bool GetNativeRenderWindow(plume::RenderWindow &out);

// The game window's client area, in pixels.
bool GetRenderWindowPixelSize(uint32_t &width, uint32_t &height);

#if !defined(_WIN32)
// The SDK's SDL window (the app owns one), or nullptr before it exists.
SDL_Window *GetGameSdlWindow();
#endif

} // namespace renut::gpu
