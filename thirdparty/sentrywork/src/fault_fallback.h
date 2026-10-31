#pragma once

// Linux and macOS only (fault_fallback_posix.cpp): see there.

namespace sentrywork::detail {

// Right after sentry_init, while Sentry's SIGSEGV and SIGILL handlers are the
// installed ones.
void CaptureSentrySignalHandlers();

// Once the runtime is set up, after its own exception handlers.
void InstallFaultFallback();

} // namespace sentrywork::detail
