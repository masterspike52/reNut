#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/setting.h>
#include <rex/types.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <rexglue_guide/guide.h>
#include "renut_engine/app/renut_settings.h"
#include "renut_engine/online/online.h"


static bool CheatsAllowed() {
    return !renut::online::InSession() || renut::online::SessionAllowsCheats();
}

namespace {

// Turns a cheat back off when it is switched on while the session doesn't allow cheats.
rex::ScopeGuard GateCheat(rex::Setting<bool> &cheat) {
    return cheat.OnChange([&cheat](const bool &on) {
        if (on && !CheatsAllowed())
            (void)cheat.SetText(cheat.GetLayer(), "false");
    });
}

struct HookSettings final : rex::SettingOwner {
    HookSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
    // Name = "Showdown Town Vehicles"
    REX_SETTING(bool, change_vehicles_anywhere, false, "Enables Changing Vehicles Anywhere. Off in online multiplayer unless the host allows cheats.");
    // Name = "No Notes Spent"
    REX_SETTING(bool, no_notes_spent, false, "hook created by serenity. Off in online multiplayer unless the host allows cheats.");
    // Name = "Disable LOD"
    REX_SETTING(bool, disable_lod, false, "Disables LOD (Level of Detail) scaling");
    // Name = "Infinite Fuel and Ammo"
    REX_SETTING(bool, infinite_fuel_and_ammo, false, "fuel never decreases. Off in online multiplayer unless the host allows cheats.");
    // Name = "Infinite Health"
    REX_SETTING(bool, infinite_health, false, "health never decreases. Off in online multiplayer unless the host allows cheats.");
    // Name = "Infinite Parts"
    REX_SETTING(bool, infinite_parts, false, "vehicle parts never decreases. Off in online multiplayer unless the host allows cheats.");
    // Name = "Banjo Skins"
    REX_SETTING(std::string, banjo_skin, "default", "Banjo skin override", rex::Allowed("default", "robot", "tuxedo"));
    // Name = "No Out of Bounds Reset"
    REX_SETTING(bool, no_out_of_bounds_reset, false, "Vehicles drive and fly through Showdown Town's invisible walls and ceiling, and going past the edge of the world no longer resets Banjo with a jiggy wall. Off in online multiplayer unless the host allows cheats.");
    // Name = "Unlock All Parts"
    REX_SETTING(bool, unlock_all_parts, false, "Every vehicle part crate is unlocked in the garage, whether or not you've earned it. Takes effect on the next level load. Off in online multiplayer unless the host allows cheats.");
    // Name = "Max Acquired Parts Access"
    REX_SETTING(bool, max_acquired_parts_access, false, "Allows max acquired vehicle parts. Off in online multiplayer unless the host allows cheats.");

    rex::ScopeGuard gates[8] = {
        GateCheat(change_vehicles_anywhere), GateCheat(no_notes_spent),
        GateCheat(infinite_fuel_and_ammo),   GateCheat(infinite_health),
        GateCheat(infinite_parts),           GateCheat(no_out_of_bounds_reset),
        GateCheat(unlock_all_parts),         GateCheat(max_acquired_parts_access),
    };
};
HookSettings &g_hooks = *new HookSettings();  // never freed, registered by RenutApp::OnConfigure

}  // namespace




// True: the fuel/ammo use returns before it takes anything (return_on_true).
bool Infinite_fuel_and_ammo() {
    return g_hooks.infinite_fuel_and_ammo.Get() && CheatsAllowed();
}

void overworld_vehicles_hook(PPCRegister& r23, PPCRegister& r26) {
	(void)r23;
    if (g_hooks.change_vehicles_anywhere.Get() && CheatsAllowed()) {
		r26.u8 = 1;
    }
}


bool town_jet_thrust() {
    return g_hooks.change_vehicles_anywhere.Get() && CheatsAllowed();
}

static constexpr uint32_t kTownBoundaryMaterialFlags = 0x0C000000;
static constexpr uint32_t kBodyUserData = 0x0C;
static constexpr uint32_t kBodyFilterInfo = 0x2C;
static constexpr uint32_t kVehicleBodyLayer = 7;
static constexpr uint32_t kObjectWorld = 0x4C;
static constexpr uint32_t kWorldIsTown = 0x58;

static uint32_t GuestLoad32(uint32_t va) {
    return rex::byte_swap(*REX_RUNTIME()->GetMembase().TranslateVirtual<const uint32_t*>(va));
}

static bool IsVehicleInTown(uint32_t body) {

    if (!body || (GuestLoad32(body + kBodyFilterInfo) & 0x3F) != kVehicleBodyLayer)
        return false;
    const uint32_t vehicle = GuestLoad32(body + kBodyUserData);
    const uint32_t world = vehicle ? GuestLoad32(vehicle + kObjectWorld) : 0;
    return world && GuestLoad32(world + kWorldIsTown) == 1;
}

bool town_boundary_pass(PPCRegister& r23, PPCRegister& r24, PPCRegister& r25, PPCRegister& r27) {
    if (!g_hooks.no_out_of_bounds_reset.Get() || !CheatsAllowed())
        return false;
    return ((r25.u32 & kTownBoundaryMaterialFlags) && IsVehicleInTown(r27.u32)) ||
           ((r23.u32 & kTownBoundaryMaterialFlags) && IsVehicleInTown(r24.u32));
}

bool keep_escaped_vehicle(PPCRegister& r30, PPCRegister& r31) {
    (void)r30;
    (void)r31;
    return g_hooks.no_out_of_bounds_reset.Get() && CheatsAllowed();
}

static constexpr uint32_t kConnectionAddress = 0x10;
static constexpr uint32_t kConnectionHasAddress = 0x14;

void title_server_cache_key(PPCRegister& r10, PPCRegister& r11) {
    const uint32_t connection = r11.u32;
    const bool has_address =
        *REX_RUNTIME()->GetMembase().TranslateVirtual<const uint8_t*>(connection + kConnectionHasAddress) != 0;
    r10.u32 = has_address ? GuestLoad32(connection + kConnectionAddress) : 0;
}

bool storage_upload(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7) {
    uint32_t result = 0;
    if (!rexglue_guide::StorageUploadFromMemory(r3.u32, r4.u32, r5.u32, r6.u32, r7.u32, &result))
        return false;
    r3.u64 = result;
    return true;
}

bool storage_download(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7,
                      PPCRegister& r8, PPCRegister& r9) {
    uint32_t result = 0;
    if (!rexglue_guide::StorageDownloadToMemory(r3.u32, r4.u32, r5.u32, r6.u32, r7.u32, r8.u32, r9.u32, &result))
        return false;
    r3.u64 = result;
    return true;
}

bool storage_enumerate(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7,
                       PPCRegister& r8, PPCRegister& r9) {
    uint32_t result = 0;
    if (!rexglue_guide::StorageEnumerate(r3.u32, r4.u32, r5.u32, r6.u32, r7.u32, r8.u32, r9.u32, &result))
        return false;
    r3.u64 = result;
    return true;
}

bool no_notes_spent() {
    if (g_hooks.no_notes_spent.Get() && CheatsAllowed()) {
        return true;
    }
    return false;
}

bool disable_lod() {
    return g_hooks.disable_lod.Get();
}

// A scene script's command on player 1's avatar (r27, world + 0xA44 by slot) with no marker (r11): skipped
// while there's no avatar, as in a match before the players spawn, instead of reading through null.
bool script_player_missing(PPCRegister& r11, PPCRegister& r27) {
    return r11.u32 == 0 && r27.u32 == 0;
}

bool Infinite_health() {
    if (g_hooks.infinite_health.Get() && CheatsAllowed()) {
        return true;
    }
    return false;
}

void Infinite_parts(PPCRegister& r11) {
    if (g_hooks.infinite_parts.Get() && CheatsAllowed()) {
        r11.u32 = 100;
    }
}

bool BanjoActorOverride(PPCRegister& r3, PPCRegister& r5) {
    const std::string skin = g_hooks.banjo_skin.Get();

    if (skin == "robot") {
        r3.u32 = 0x82148098;
        r5.u32 = 0;
        return false;
    }

    if (skin == "tuxedo") {
        r3.u32 = 0x821480AC;
        r5.u32 = 0;
        return false;
    }

    return true;
}

// True takes the unlocked branch for every part crate at 0x824FB648 when the garage's
// part list is rebuilt on level load (sub_824FB5D8); nothing is saved.
bool unlock_all_parts() {
    return g_hooks.unlock_all_parts.Get() && CheatsAllowed();
}

void Max_Acquired_Parts_Access_hook_1(PPCRegister& r11) {
    if (g_hooks.max_acquired_parts_access.Get() && CheatsAllowed()) {
        r11.u32 = 255;
    }
}

void Max_Acquired_Parts_Access_hook_2(PPCRegister& r10) {
    if (g_hooks.max_acquired_parts_access.Get() && CheatsAllowed()) {
        r10.u32 = 255;
    }
}

void Max_Acquired_Parts_Access_hook_3(PPCRegister& r11) {
    if (g_hooks.max_acquired_parts_access.Get() && CheatsAllowed()) {
        r11.u32 = 255;
    }
}
