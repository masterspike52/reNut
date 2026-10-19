#include <rex/ppc.h>
#include "renut_engine/app/renut_logging.h"
#include <rex/hook.h>
#include <rex/cvar.h>
#include <rex/system/kernel_state.h>


#if defined(_MSC_VER)
#include <stdlib.h>
#define REX_BSWAP32(x) _byteswap_ulong(x)
#else
#define REX_BSWAP32(x) __builtin_bswap32(x)
#endif

REX_EXTERN(gameFlagsSetFlag_82363ED0);

// The players' profiles (game flags among them), one per player: what
// sub_82243E58 returns and the game passes to gameFlagsSetFlag (e.g.
// sceneScript2GroupsExecute). The Stop 'n' Swop scan (sub_822C7EB0) uses
// player 1's at its object + 0x5C.
static constexpr uint32_t kPlayerProfiles = 0x82FAD9F0;

// Unlocks every Stop 'n' Swop crate as sub_822C7D08 does for the content it
// finds. The profile is read when the command runs: the scan's object, kept
// before, is freed once the scan ends.
static void StopNSwap()
{
    // The guest arena's host address changes from run to run: translate.
    auto* memory = REX_KERNEL_MEMORY();
    const uint32_t flags_ptr =
        REX_BSWAP32(*memory->TranslateVirtual<uint32_t*>(kPlayerProfiles));
    auto unlocked = [&](uint32_t offset) -> uint8_t& {
        return *memory->TranslateVirtual<uint8_t*>(flags_ptr + offset);
    };
    if (!flags_ptr) {
        RNUT_WARN("StopNSwap: no player profile yet (load a save first)");
        return;
    }

    unlocked(236) = 1;
    rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82363ED0, flags_ptr, 1, 0x10E, 1);
    rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82363ED0, flags_ptr, 0, 0x703, 1);

    unlocked(237) = 1;
    rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82363ED0, flags_ptr, 1, 0x110, 1);
    rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82363ED0, flags_ptr, 0, 0x705, 1);

    unlocked(238) = 1;
    rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82363ED0, flags_ptr, 1, 0x10D, 1);
    rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82363ED0, flags_ptr, 0, 0x702, 1);

    unlocked(239) = 1;
    rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82363ED0, flags_ptr, 1, 0x10C, 1);
    rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82363ED0, flags_ptr, 0, 0x701, 1);

    unlocked(240) = 1;
    rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82363ED0, flags_ptr, 1, 0x10B, 1);
    rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82363ED0, flags_ptr, 0, 0x700, 1);

    unlocked(241) = 1;
    rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82363ED0, flags_ptr, 1, 0x10F, 1);
    rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82363ED0, flags_ptr, 0, 0x704, 1);

    unlocked(242) = 1;
    rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82363ED0, flags_ptr, 1, 0x111, 1);
    rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82363ED0, flags_ptr, 0, 0x706, 1);
}


REXCVAR_DEFINE_COMMAND(StopNSwap, StopNSwap, "Nuts&Bolts/Events", "Triggers StopNSwop");

// config/renut_hooks.toml still attaches this at 0x822C7F0C; the command no
// longer needs the scan's object.
void StopNSwap_hook(PPCRegister& r22)
{
    (void)r22;
}
