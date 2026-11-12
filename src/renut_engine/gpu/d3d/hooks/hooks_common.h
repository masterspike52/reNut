#pragma once

#include <rex/ppc/hook.h>

// Intentional no-op with no log output.
#define RENUT_NOOP(subroutine)                                                 \
  extern "C" REX_FUNC(subroutine) {                                            \
    (void)ctx;                                                                 \
    (void)base;                                                                \
  }

// RENUT_NOOP that returns a constant u32 in r3, for callers checking the result.
#define RENUT_NOOP_RETURN(subroutine, value)                                   \
  extern "C" REX_FUNC(subroutine) {                                            \
    (void)base;                                                                \
    ctx.r3.u64 = (value);                                                      \
  }
