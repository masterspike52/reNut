// Android: what the stock rexglue SDK expects from the platform that bionic
// does not have (declared in android/sdk_compat), so the SDK builds unmodified.
// Compiled into the SDK's rexcore (CMakeLists.txt): librexruntime.so needs
// these as much as librenut.so.

#include <android/api-level.h>
#include <ucontext.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <X11/Xlib-xcb.h>
#include <rex/filesystem.h>

namespace rex {
int32_t GetAndroidApiLevel() { return android_get_device_api_level(); }

// Game files are read through plain paths (all files access): no content://
// URI reaches the runtime.
int filesystem::OpenAndroidContentFileDescriptor(const std::string_view, const char *) { return -1; }
} // namespace rex

// --- ucontext, for the SDK's fibers (src/core/fiber_posix.cpp) ---------------
// Only what a cooperative switch has to keep, by the AAPCS64: x19-x30, sp, the
// resume address and d8-d15, kept in uc_mcontext (as raw storage: nothing else
// reads these contexts). No signal mask: fibers never change it.
// ponytail: makecontext takes no arguments (the SDK passes none); pass x0-x7
// from the va_list if that changes.

static_assert(offsetof(ucontext_t, uc_mcontext) == 176 && sizeof(mcontext_t) >= 176,
              "the offsets in the assembly below");
static_assert(offsetof(ucontext_t, uc_stack) + offsetof(stack_t, ss_sp) == 16 &&
              offsetof(ucontext_t, uc_stack) + offsetof(stack_t, ss_size) == 32);

// uc_mcontext + 0: x19-x30, +96: sp, +104: pc, +112: d8-d15
asm(R"(
    .text
    .p2align 2
    .globl getcontext
    .type getcontext, %function
getcontext:
    add x9, x0, #176
    stp x19, x20, [x9, #0]
    stp x21, x22, [x9, #16]
    stp x23, x24, [x9, #32]
    stp x25, x26, [x9, #48]
    stp x27, x28, [x9, #64]
    stp x29, x30, [x9, #80]
    mov x10, sp
    stp x10, x30, [x9, #96]
    stp d8, d9, [x9, #112]
    stp d10, d11, [x9, #128]
    stp d12, d13, [x9, #144]
    stp d14, d15, [x9, #160]
    mov x0, #0
    ret
    .size getcontext, . - getcontext

    .globl swapcontext
    .type swapcontext, %function
swapcontext:
    mov x11, x1
    add x9, x0, #176
    stp x19, x20, [x9, #0]
    stp x21, x22, [x9, #16]
    stp x23, x24, [x9, #32]
    stp x25, x26, [x9, #48]
    stp x27, x28, [x9, #64]
    stp x29, x30, [x9, #80]
    mov x10, sp
    stp x10, x30, [x9, #96]
    stp d8, d9, [x9, #112]
    stp d10, d11, [x9, #128]
    stp d12, d13, [x9, #144]
    stp d14, d15, [x9, #160]
    add x9, x11, #176
    ldp x19, x20, [x9, #0]
    ldp x21, x22, [x9, #16]
    ldp x23, x24, [x9, #32]
    ldp x25, x26, [x9, #48]
    ldp x27, x28, [x9, #64]
    ldp x29, x30, [x9, #80]
    ldp x10, x12, [x9, #96]
    mov sp, x10
    ldp d8, d9, [x9, #112]
    ldp d10, d11, [x9, #128]
    ldp d12, d13, [x9, #144]
    ldp d14, d15, [x9, #160]
    mov x0, #0
    br x12
    .size swapcontext, . - swapcontext
)");

// Where a fiber's entry function returns to: uc_link is never set by the SDK.
static void FiberReturned() { std::abort(); }

extern "C" void makecontext(ucontext_t *ucp, void (*func)(void), int, ...) {
    auto *saved = reinterpret_cast<uint64_t *>(&ucp->uc_mcontext);
    uintptr_t top = reinterpret_cast<uintptr_t>(ucp->uc_stack.ss_sp) + ucp->uc_stack.ss_size;
    saved[10] = 0;                                              // x29: end of the frame chain
    saved[11] = reinterpret_cast<uintptr_t>(&FiberReturned);    // x30
    saved[12] = top & ~uintptr_t(15);                           // sp
    saved[13] = reinterpret_cast<uintptr_t>(func);              // pc
}

// --- X11: compiled into the SDK's SDL window code, never called --------------

extern "C" {
xcb_connection_t *XGetXCBConnection(Display *) { return nullptr; }
xcb_get_geometry_cookie_t xcb_get_geometry(xcb_connection_t *, xcb_drawable_t) { return {}; }
xcb_get_geometry_reply_t *xcb_get_geometry_reply(xcb_connection_t *, xcb_get_geometry_cookie_t,
                                                 xcb_generic_error_t **) {
    return nullptr;
}
}
