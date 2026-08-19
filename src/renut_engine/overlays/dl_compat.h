#pragma once

// Minimal dlopen/dlsym/dlclose shim so the F5 debug hub's cross-plugin
// symbol lookups (native-shader tab, A/B benchmark tab -- see
// debug_hub_overlay.h and ab_benchmark_overlay.h) work on Windows too,
// instead of compiling the whole debug hub out of Windows builds entirely
// (previously done in renut_app.h; see that file's git history).
//
// Both call sites only ever want a handle to an ALREADY-loaded module (the
// GPU plugin is resident because the graphics system came from it) -- never
// a real load. On POSIX that's RTLD_NOLOAD; on Windows the equivalent is
// GetModuleHandle, which (unlike LoadLibrary) does NOT increment the
// module's reference count. That asymmetry is why RenutDlClose() is a no-op
// on Windows: calling FreeLibrary on a GetModuleHandle-obtained handle would
// wrongly decrement a refcount this code never incremented, risking
// unloading a plugin still in use elsewhere. POSIX dlclose() after a
// successful RTLD_NOLOAD dlopen is safe and balanced (every successful
// dlopen call, NOLOAD included, increments the refcount once), so that side
// keeps calling the real dlclose().

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

inline void* RenutDlOpenNoLoad(const char* name) {
    return reinterpret_cast<void*>(GetModuleHandleA(name));
}

inline void* RenutDlOpenSelf() {
    return reinterpret_cast<void*>(GetModuleHandleA(nullptr));
}

inline void* RenutDlSym(void* handle, const char* name) {
    return handle ? reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name)) : nullptr;
}

inline void RenutDlClose(void*) {}

#else

#include <dlfcn.h>

inline void* RenutDlOpenNoLoad(const char* name) {
    return dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
}

inline void* RenutDlOpenSelf() {
    return dlopen(nullptr, RTLD_LAZY);
}

inline void* RenutDlSym(void* handle, const char* name) {
    return handle ? dlsym(handle, name) : nullptr;
}

inline void RenutDlClose(void* handle) {
    if (handle) dlclose(handle);
}

#endif
