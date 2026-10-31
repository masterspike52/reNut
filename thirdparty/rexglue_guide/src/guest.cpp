#include "guest.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <dlfcn.h>
#endif

namespace rexglue_guide::guest {

Func Original(const char* name) {
  static std::mutex mutex;
  static std::unordered_map<std::string, Func> cache;
  std::lock_guard lock(mutex);
  auto it = cache.find(name);
  if (it != cache.end()) return it->second;
  Func found = nullptr;
#if defined(_WIN32)
  // The executable exports nothing, so the module that has the symbol is the
  // runtime DLL.
  HMODULE modules[512];
  DWORD bytes = 0;
  if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) {
    for (DWORD i = 0; i < bytes / sizeof(HMODULE) && !found; ++i) {
      found = reinterpret_cast<Func>(GetProcAddress(modules[i], name));
    }
  }
#else
  // The next definition after the executable's own (the override, as the
  // Guide is compiled into it): the runtime library's. Not by walking the
  // loaded objects with dl_iterate_phdr and dlopen-ing each: glibc takes its
  // loader locks in the opposite order there to a dlopen on another thread
  // (the Vulkan driver, SDL's audio, libcurl), and the two deadlock.
  found = reinterpret_cast<Func>(dlsym(RTLD_NEXT, name));
#endif
  cache[name] = found;
  return found;
}

}  // namespace rexglue_guide::guest
