#include "library.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace rexfabric {

#if defined(_WIN32)

std::unique_ptr<Library> Library::Open(const std::filesystem::path& path, std::string& error) {
  HMODULE handle = LoadLibraryW(path.c_str());
  if (!handle) {
    error = "LoadLibrary failed with error " + std::to_string(GetLastError());
    return nullptr;
  }
  return std::unique_ptr<Library>(new Library(handle));
}

Library::~Library() { FreeLibrary(static_cast<HMODULE>(handle_)); }

void* Library::Symbol(const char* name) const { return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name)); }

#else

std::unique_ptr<Library> Library::Open(const std::filesystem::path& path, std::string& error) {
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    error = dlerror();
    return nullptr;
  }
  return std::unique_ptr<Library>(new Library(handle));
}

Library::~Library() { dlclose(handle_); }

void* Library::Symbol(const char* name) const { return dlsym(handle_, name); }

#endif

}  // namespace rexfabric
