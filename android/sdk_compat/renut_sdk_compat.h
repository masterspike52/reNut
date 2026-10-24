#pragma once
// Force-included into every C++ file of the Android build (CMakeLists.txt), so
// the stock rexglue SDK builds unmodified: what it expects from the platform
// that bionic and the NDK's libc++ do not have. Definitions in
// src/renut_engine/app/android_sdk_compat.cpp.

#include <pthread.h>

#include <cerrno>
#include <chrono>
#include <cstdint>

// libc++ has no clock_cast; rex/chrono/chrono.h brings the same fallback for
// Apple only.
namespace std::chrono {
template <class, class>
struct clock_time_conversion {};

template <class DestClock, class SourceClock, class Duration>
auto clock_cast(const time_point<SourceClock, Duration> &t) {
    return clock_time_conversion<DestClock, SourceClock>{}(t);
}
} // namespace std::chrono

// Bionic has no robust mutexes, which the SDK's waits use to recover a lock
// whose owner died (src/core/threading_posix.cpp): refused, so they stay plain
// mutexes, which never report EOWNERDEAD.
#ifndef PTHREAD_MUTEX_ROBUST
#define PTHREAD_MUTEX_ROBUST 1
inline int pthread_mutexattr_setrobust(pthread_mutexattr_t *, int) { return ENOTSUP; }
inline int pthread_mutex_consistent(pthread_mutex_t *) { return 0; }
#endif

// From rex/main_android.h, which memory_posix.cpp uses without including it.
namespace rex {
int32_t GetAndroidApiLevel();
}
