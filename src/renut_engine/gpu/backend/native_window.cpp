#include "renut_engine/gpu/backend/native_window.h"

#include <atomic>

#include <rex/platform/native_window.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cstring>

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <plume_vulkan.h>
#endif

namespace renut::gpu {

namespace {

// The window SetNativeWindow took: an HWND on Windows, the SDL_Window elsewhere.
std::atomic<void *> g_window{nullptr};

} // namespace

void SetNativeWindow(const rex::NativeWindow &window) {
#if defined(_WIN32)
  g_window.store(window.type == rex::NativeWindow::Type::kWin32 ? window.window : nullptr,
                 std::memory_order_release);
#else
  g_window.store(window.sdl, std::memory_order_release);
#endif
}

#if defined(_WIN32)

namespace {

HWND FindGameHwnd() {
  return static_cast<HWND>(g_window.load(std::memory_order_acquire));
}

} // namespace

bool GetNativeRenderWindow(plume::RenderWindow &out) {
  out = static_cast<plume::RenderWindow>(FindGameHwnd());
  return out != nullptr;
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

SDL_Window *FindGameSdlWindow() {
  return static_cast<SDL_Window *>(g_window.load(std::memory_order_acquire));
}

} // namespace

bool GetNativeRenderWindow(plume::RenderWindow &out) {
  out = FindGameSdlWindow();
  return out != nullptr;
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

#if !defined(_WIN32)

// plume's Vulkan surface, made from the SDL window's Wayland or X11 handles
// (SDL's own surface needs SDL_WINDOW_VULKAN, which the SDK's window lacks).

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
    if (!info.display || !info.surface || !create)
      return false;
    result = create(instance, &info, nullptr, surface);
  } else {
    XlibSurfaceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    info.dpy = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    info.window = static_cast<unsigned long>(
        SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    auto create = reinterpret_cast<CreateSurfaceFunction>(
        vkGetInstanceProcAddr(instance, "vkCreateXlibSurfaceKHR"));
    if (!info.dpy || !info.window || !create)
      return false;
    result = create(instance, &info, nullptr, surface);
  }
  return result == VK_SUCCESS;
}

#endif
