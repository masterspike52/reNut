#pragma once

#include <cstdint>

#include <plume_render_interface.h>

#if !defined(_WIN32)
struct SDL_Window;
#endif

namespace rex {
struct NativeWindow;
} // namespace rex

namespace renut::gpu {

// StartRenderer, UI thread: the SDK's window (IUISystem::GetNativeWindow), which
// everything below answers from.
void SetNativeWindow(const rex::NativeWindow &window);

// The game window: its HWND on Windows, the SDL window itself on Linux (whose
// Vulkan surface is made in native_window.cpp).
bool GetNativeRenderWindow(plume::RenderWindow &out);

// The game window's client area, in pixels.
bool GetRenderWindowPixelSize(uint32_t &width, uint32_t &height);

#if !defined(_WIN32)
// The SDK's SDL window, or nullptr before StartRenderer took it.
SDL_Window *GetGameSdlWindow();
#endif

} // namespace renut::gpu
