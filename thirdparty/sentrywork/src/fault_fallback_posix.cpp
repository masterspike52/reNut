// The runtime's SIGSEGV and SIGILL handler (rex/exception_handler.h) is
// installed after Sentry's and replaces it: guest MMIO and memory watches
// fault on purpose, and those faults must not reach Sentry as crashes. But
// when none of the runtime's handlers claims a fault it returns to the
// faulting instruction without passing it on, so a real crash faults again
// forever and Sentry never sees it.
//
// So SentryWork adds the last of the runtime's handlers: reaching it means
// nothing claimed the fault. It puts Sentry's signal handlers back and
// declines; the instruction faults again, now into Sentry, with the crashing
// thread's real context.
//
// (On Windows the runtime uses a vectored handler, which runs before Sentry's
// unhandled exception filter and needs none of this.)

#include "fault_fallback.h"

#include <rex/exception_handler.h>

#include <signal.h>

namespace sentrywork::detail {

namespace {

struct sigaction g_sentry_segv;
struct sigaction g_sentry_ill;
bool g_captured = false;

bool OnUnclaimedFault(rex::arch::Exception *, void *) {
  sigaction(SIGSEGV, &g_sentry_segv, nullptr);
  sigaction(SIGILL, &g_sentry_ill, nullptr);
  return false;
}

} // namespace

void CaptureSentrySignalHandlers() {
  g_captured = sigaction(SIGSEGV, nullptr, &g_sentry_segv) == 0 &&
               sigaction(SIGILL, nullptr, &g_sentry_ill) == 0;
}

void InstallFaultFallback() {
  static bool installed = false;
  if (g_captured && !installed) {
    installed = true;
    rex::arch::ExceptionHandler::Install(OnUnclaimedFault, nullptr);
  }
}

} // namespace sentrywork::detail
