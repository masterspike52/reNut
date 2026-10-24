/**
 * @file    gpu/backend/native_window.cpp
 */
#include "renut_engine/gpu/backend/native_window.h"

#include "renut_engine/gpu/log.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cstring>

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <plume_vulkan.h>
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
#if defined(__ANDROID__)
  SDL_Window *sdl_window = FindGameSdlWindow();
  out = sdl_window ? static_cast<ANativeWindow *>(SDL_GetPointerProperty(
                         SDL_GetWindowProperties(sdl_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr))
                   : nullptr;
  if (!out) {
    RGPU_ERROR("SDL window has no ANativeWindow yet");
    return false;
  }
  return true;
#else
  out = FindGameSdlWindow();
  if (!out) {
    RGPU_ERROR("No SDL window for the Vulkan surface");
    return false;
  }
  return true;
#endif
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

SDL_Window *GetGameSdlWindow() {
  return FindGameSdlWindow();
}

#endif

} // namespace renut::gpu

#if !defined(_WIN32) && !defined(__ANDROID__)

// plume's Vulkan surface (its PLUME_SDL_VULKAN_ENABLED path, through
// gpu/backend/sdl2_vulkan/SDL_vulkan.h), on whichever video driver SDL chose:
// Wayland (VK_KHR_wayland_surface on the window's wl_display and wl_surface)
// or X11, XWayland included (VK_KHR_xlib_surface on its Display and Window).
// SDL_VIDEO_DRIVER=x11 or wayland picks one. Made here from the handles SDL
// gives every window: SDL's own surface needs SDL_WINDOW_VULKAN, which the
// SDK's window lacks.

namespace {

bool WaylandWindow() {
  const char *driver = SDL_GetCurrentVideoDriver();
  return driver && std::strcmp(driver, "wayland") == 0;
}

// VkWaylandSurfaceCreateInfoKHR and VkXlibSurfaceCreateInfoKHR, without the
// Wayland and Xlib headers: their handles come from SDL as pointers and an XID.
struct WaylandSurfaceCreateInfo {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
  void *display;  // wl_display *
  void *surface;  // wl_surface *
};
struct XlibSurfaceCreateInfo {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
  void *dpy;             // Display *
  unsigned long window;  // Window
};
using CreateSurfaceFunction = VkResult(VKAPI_PTR *)(VkInstance, const void *,
                                                    const VkAllocationCallbacks *, VkSurfaceKHR *);

constexpr const char *kWaylandExtensions[] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
constexpr const char *kXlibExtensions[] = {"VK_KHR_surface", "VK_KHR_xlib_surface"};

} // namespace

bool renut_SDL2_Vulkan_GetInstanceExtensions(SDL_Window *, unsigned int *count, const char **names) {
  if (!count)
    return false;
  const char *const *extensions = WaylandWindow() ? kWaylandExtensions : kXlibExtensions;
  constexpr unsigned int kCount = 2;
  if (names) {
    for (unsigned int i = 0; i < kCount && i < *count; ++i)
      names[i] = extensions[i];
  }
  *count = kCount;
  return true;
}

bool renut_SDL2_Vulkan_CreateSurface(SDL_Window *window, VkInstance instance, VkSurfaceKHR *surface) {
  if (!window || !instance || !surface)
    return false;
  const SDL_PropertiesID props = SDL_GetWindowProperties(window);
  VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  if (WaylandWindow()) {
    WaylandSurfaceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    info.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    info.surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    auto create = reinterpret_cast<CreateSurfaceFunction>(
        vkGetInstanceProcAddr(instance, "vkCreateWaylandSurfaceKHR"));
    if (!info.display || !info.surface || !create) {
      RGPU_ERROR("No Wayland surface for the window (display {}, surface {}, "
                 "vkCreateWaylandSurfaceKHR {})",
                 info.display != nullptr, info.surface != nullptr, create != nullptr);
      return false;
    }
    result = create(instance, &info, nullptr, surface);
    if (result == VK_SUCCESS)
      RGPU_INFO("Vulkan surface on Wayland");
  } else {
    XlibSurfaceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    info.dpy = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    info.window = static_cast<unsigned long>(
        SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    auto create = reinterpret_cast<CreateSurfaceFunction>(
        vkGetInstanceProcAddr(instance, "vkCreateXlibSurfaceKHR"));
    if (!info.dpy || !info.window || !create) {
      RGPU_ERROR("No X11 surface for the window (video driver '{}')",
                 SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
      return false;
    }
    result = create(instance, &info, nullptr, surface);
    if (result == VK_SUCCESS)
      RGPU_INFO("Vulkan surface on X11");
  }
  if (result != VK_SUCCESS) {
    RGPU_ERROR("Vulkan surface creation failed ({})", int(result));
    return false;
  }
  return true;
}

#endif
