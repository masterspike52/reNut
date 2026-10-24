#pragma once
// Bionic has ucontext_t but not the functions, which the SDK's fibers use
// (src/core/fiber_posix.cpp). Defined in android_sdk_compat.cpp.
#include_next <ucontext.h>

#ifdef __cplusplus
extern "C" {
#endif
int getcontext(ucontext_t *ucp);
void makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...);
int swapcontext(ucontext_t *oucp, const ucontext_t *ucp);
#ifdef __cplusplus
}
#endif
