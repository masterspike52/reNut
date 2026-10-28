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
#include <link.h>
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
  // Shared objects only: the executable's own definition is the override.
  std::pair<const char*, Func*> lookup{name, &found};
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) -> int {
        auto* args = static_cast<std::pair<const char*, Func*>*>(data);
        if (!info->dlpi_name || !*info->dlpi_name) return 0;
        void* handle = dlopen(info->dlpi_name, RTLD_LAZY | RTLD_NOLOAD);
        if (!handle) return 0;
        *args->second = reinterpret_cast<Func>(dlsym(handle, args->first));
        dlclose(handle);
        return *args->second ? 1 : 0;
      },
      &lookup);
#endif
  cache[name] = found;
  return found;
}

}  // namespace rexglue_guide::guest
