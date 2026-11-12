// A function the project hooks itself (REX_HOOK_RAW) replaces the generated one outright, slot check and all,
// so mods' rex.hook / rex.hook_after on it never ran. These keep them running.
//
//   // the project's hook does its own work around the game's function; mods' hooks run around the original
//   REXFABRIC_HOOKABLE(sub_82000000);
//   REX_HOOK_RAW(sub_82000000) {
//     ...
//     REXFABRIC_ORIGINAL(sub_82000000)(ctx, base);
//     ...
//   }
//
//   // or mods' hooks run around the whole of the project's hook
//   REXFABRIC_HOOK_RAW(sub_82000000) { ... __imp__sub_82000000(ctx, base); ... }

#pragma once

#include <atomic>

#include <rexfabric/hook_slot.h>

// Once at file scope, before REXFABRIC_ORIGINAL(name).
#define REXFABRIC_HOOKABLE(name)                    \
  extern rexfabric::HookSlot rexfabric_slot_##name; \
  REX_EXTERN(__imp__##name)

// The game's own function, with mods' hooks on `name` around it: a before hook returning false skips it.
#define REXFABRIC_ORIGINAL(name)                                                           \
  [](PPCContext& ctx_, uint8_t* base_) {                                                   \
    if (rexfabric_slot_##name.chain.load(std::memory_order_relaxed)) [[unlikely]]          \
      return rexfabric::Dispatch(rexfabric_slot_##name, __imp__##name, ctx_, base_);       \
    __imp__##name(ctx_, base_);                                                            \
  }

// A project hook (as REX_HOOK_RAW) with mods' hooks on `name` around all of it: a before hook returning false
// skips the project's hook too.
#define REXFABRIC_HOOK_RAW(name)                                                         \
  extern rexfabric::HookSlot rexfabric_slot_##name;                                      \
  static void rexfabric_body_##name(PPCContext& ctx, uint8_t* base);                     \
  REX_HOOK_RAW(name) {                                                                   \
    if (rexfabric_slot_##name.chain.load(std::memory_order_relaxed)) [[unlikely]]        \
      return rexfabric::Dispatch(rexfabric_slot_##name, rexfabric_body_##name, ctx, base); \
    rexfabric_body_##name(ctx, base);                                                    \
  }                                                                                      \
  static void rexfabric_body_##name([[maybe_unused]] PPCContext& ctx, [[maybe_unused]] uint8_t* base)
