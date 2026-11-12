#include <rex/console_command.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/result.h>
#include <rex/types.h>

#include <string_view>

#include "renut_engine/app/renut_settings.h"

REX_EXTERN(__imp__rex_gameFlagsSetFlag);

// The players' profiles (game flags among them), as returned by sub_82212D18
// and passed to gameFlagsSetFlag.
static constexpr uint32_t kPlayerProfiles = 0x82E1B280;

// One unlock: the profile byte the game sets, then its two game flags.
struct Unlock {
    uint32_t offset;
    uint32_t flag;
    uint32_t event;
};

// The seven Stop 'n' Swop crates (profile + 260..266) that stopNSwapApplyUnlocks
// unlocks from a Banjo-Kazooie save, in its order.
static constexpr Unlock kUnlocks[] = {
    {260, 0x10E, 0x703}, {261, 0x110, 0x705}, {262, 0x10D, 0x702},
    {263, 0x10C, 0x701}, {264, 0x10B, 0x700}, {265, 0x10F, 0x704},
    {266, 0x111, 0x706},
};

// Unlocks every Stop 'n' Swop crate as stopNSwapApplyUnlocks does, using player 1's
// profile at the time the command runs.
static void StopNSwap()
{
    // The guest arena's host address changes from run to run: translate.
    const rex::memory::GuestMembase memory = REX_RUNTIME()->GetMembase();
    const uint32_t flags_ptr =
        rex::byte_swap(*memory.TranslateVirtual<uint32_t*>(kPlayerProfiles));
    // No player profile yet (no save loaded).
    if (!flags_ptr)
        return;

    // Off a guest thread (the console's) the game flags can't be set, only the bytes.
    const bool guest_thread = rex::ppc::GetBoundThread().ctx != nullptr;
    for (const Unlock& unlock : kUnlocks) {
        *memory.TranslateVirtual<uint8_t*>(flags_ptr + unlock.offset) = 1;
        if (!guest_thread)
            continue;
        rex::ppc::HostToGuestCall<void>(__imp__rex_gameFlagsSetFlag, flags_ptr, 1u, unlock.flag, 1u);
        rex::ppc::HostToGuestCall<void>(__imp__rex_gameFlagsSetFlag, flags_ptr, 0u, unlock.event, 1u);
    }
}

namespace {

struct StopNSwapSettings final : rex::SettingOwner {
    StopNSwapSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
    REX_COMMAND(StopNSwap,
        "Unlocks Stop 'n' Swop: the seven crates Banjo-Kazooie's Stop 'n' Swop items open.",
        [](std::string_view) {
            ::StopNSwap();
            return rex::Ok();
        });
};
[[maybe_unused]] StopNSwapSettings &g_stop_n_swap =*new StopNSwapSettings();  // never freed, registered by RenutApp::OnConfigure

}  // namespace

// config/renut_hooks.toml still attaches this at 0x822969D0; the command no
// longer needs the scan's object.
void StopNSwap_hook(PPCRegister& r22)
{
    (void)r22;
}
