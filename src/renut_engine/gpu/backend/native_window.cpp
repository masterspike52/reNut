/**
 * @file    gpu/backend/native_window.cpp
 */
#include "renut_engine/gpu/backend/native_window.h"

#include "renut_engine/gpu/log.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cstdlib>

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#endif

namespace renut::gpu {

#if defined(_WIN32)

namespace {

// The process's visible top-level window, preferring SDL's window class (the
// SDK creates one window, through SDL).
BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM param) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
    return TRUE;
  auto *found = reinterpret_cast<HWND *>(param);
  wchar_t class_name[32] = {};
  GetClassNameW(hwnd, class_name, 32);
  if (wcscmp(class_name, L"SDL_app") == 0) {
    *found = hwnd;
    return FALSE;
  }
  if (!*found)
    *found = hwnd;
  return TRUE;
}

HWND FindGameHwnd() {
  HWND hwnd = nullptr;
  EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&hwnd));
  return hwnd;
}

} // namespace

bool GetNativeRenderWindow(plume::RenderWindow &out) {
  out = static_cast<plume::RenderWindow>(FindGameHwnd());
  if (!out) {
    RGPU_ERROR("No game window yet");
    return false;
  }
  return true;
}

bool GetRenderWindowPixelSize(uint32_t &width, uint32_t &height) {
  const HWND hwnd = FindGameHwnd();
  RECT rect{};
  if (!hwnd || !GetClientRect(hwnd, &rect))
    return false;
  width = uint32_t(rect.right - rect.left);
  height = uint32_t(rect.bottom - rect.top);
  return true;
}

#else

namespace {

// Runs before main, so before the SDK initialises SDL. plume builds its Vulkan
// surface with VK_KHR_xlib_surface, which needs the X11 video driver (XWayland
// on Wayland sessions). An explicit user setting is left alone.
[[maybe_unused]] const bool kPreferX11 = [] {
  setenv("SDL_VIDEO_DRIVER", "x11", 0);
  return true;
}();

// The app owns one window.
SDL_Window *FindGameSdlWindow() {
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  SDL_Window *sdl_window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  return sdl_window;
}

} // namespace

bool GetNativeRenderWindow(plume::RenderWindow &out) {
  SDL_Window *sdl_window = FindGameSdlWindow();
  if (!sdl_window) {
    RGPU_ERROR("No SDL window for the Vulkan surface");
    return false;
  }

  const SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window);
  out.display = static_cast<Display *>(
      SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
  out.window = static_cast<Window>(
      SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  if (!out.display || !out.window) {
    RGPU_ERROR("SDL window has no X11 handles (video driver '{}'); the Vulkan "
               "surface needs SDL_VIDEO_DRIVER=x11",
               SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
    return false;
  }
  return true;
}

bool GetRenderWindowPixelSize(uint32_t &width, uint32_t &height) {
  SDL_Window *sdl_window = FindGameSdlWindow();
  int w = 0, h = 0;
  if (!sdl_window || !SDL_GetWindowSizeInPixels(sdl_window, &w, &h))
    return false;
  width = uint32_t(w);
  height = uint32_t(h);
  return true;
}

#endif

} // namespace renut::gpu
