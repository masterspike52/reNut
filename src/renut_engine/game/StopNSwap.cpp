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

REX_EXTERN(gameFlagsSetFlag_82332728);

// The players' profiles (game flags among them), one per player: what
// sub_82212D18 returns and the game passes to gameFlagsSetFlag (e.g.
// sceneScript2GroupsExecute). The Stop 'n' Swop scan (sub_82296968) uses
// player 1's at its object + 0x7C.
static constexpr uint32_t kPlayerProfiles = 0x82E1B280;

// One unlock: the profile byte the game sets, then its two game flags.
struct Unlock {
    uint32_t offset;
    uint32_t flag;
    uint32_t event;
};

// What sub_82296630 does for the content it finds, in its order: the seven
// crates (+ 260..266) and the seven unlocks the title update added
// (+ 267..273).
static constexpr Unlock kUnlocks[] = {
    {260, 0x10E, 0x703}, {261, 0x110, 0x705}, {262, 0x10D, 0x702},
    {263, 0x10C, 0x701}, {264, 0x10B, 0x700}, {265, 0x10F, 0x704},
    {266, 0x111, 0x706}, {267, 125, 1261},    {268, 126, 1262},
    {269, 127, 1263},    {270, 128, 1264},    {271, 129, 1265},
    {272, 130, 1266},    {273, 131, 1267},
};

// Unlocks every Stop 'n' Swop item as sub_82296630 does. The profile is read
// when the command runs: the scan's object, kept before, is freed once the
// scan ends.
static void StopNSwap()
{
    // The guest arena's host address changes from run to run: translate.
    auto* memory = REX_KERNEL_MEMORY();
    const uint32_t flags_ptr =
        REX_BSWAP32(*memory->TranslateVirtual<uint32_t*>(kPlayerProfiles));
    if (!flags_ptr) {
        RNUT_WARN("StopNSwap: no player profile yet (load a save first)");
        return;
    }

    for (const Unlock& unlock : kUnlocks) {
        *memory->TranslateVirtual<uint8_t*>(flags_ptr + unlock.offset) = 1;
        rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82332728, flags_ptr, 1, unlock.flag, 1);
        rex::ppc::GuestToHostFunction<void>(__imp__rex_gameFlagsSetFlag_82332728, flags_ptr, 0, unlock.event, 1);
    }
}


REXCVAR_DEFINE_COMMAND(StopNSwap, StopNSwap, "Nuts&Bolts/Events", "Triggers StopNSwop");

// config/renut_hooks.toml still attaches this at 0x822969D0; the command no
// longer needs the scan's object.
void StopNSwap_hook(PPCRegister& r22)
{
    (void)r22;
}
