#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include <cstdint>
#include <string_view>

#include "renut_engine/online/online.h"

// Cheats are off in online multiplayer unless the session's host
// allows them (renut_host_cheats, online/session_cheats.cpp): while the game
// has a session (renut::online::InSession) they cannot be turned on, and
// those already on do nothing.
static bool CheatsAllowed() {
    return !renut::online::InSession() || renut::online::SessionAllowsCheats();
}

// A cheat's new value: turning one off is always allowed, turning one on only
// outside multiplayer or in a session whose host allows cheats.
static bool CheatValueAllowed(std::string_view value) {
    return CheatsAllowed() || (value != "true" && value != "1");
}

// Name = "Showdown Town Vehicles"
REXCVAR_DEFINE_BOOL(change_vehicles_anywhere, false, "Nuts&Bolts/Cheats", "Enables Changing Vehicles Anywhere. Off in online multiplayer unless the host allows cheats.")
.validator(CheatValueAllowed);
// Name = "No Notes Spent"
REXCVAR_DEFINE_BOOL(no_notes_spent, false, "Nuts&Bolts/Cheats", "hook created by serenity. Off in online multiplayer unless the host allows cheats.")
.validator(CheatValueAllowed);
// Name = "Disable LOD"
REXCVAR_DEFINE_BOOL(disable_lod, false, "Nuts&Bolts/Graphics", "Disables LOD (Level of Detail) scaling");
// Name = "Infinite Fuel and Ammo"
REXCVAR_DEFINE_BOOL(infinite_fuel_and_ammo, false, "Nuts&Bolts/Cheats", "fuel never decreases. Off in online multiplayer unless the host allows cheats.")
.validator(CheatValueAllowed);
// Name = "Infinite Health"
REXCVAR_DEFINE_BOOL(infinite_health, false, "Nuts&Bolts/Cheats", "health never decreases. Off in online multiplayer unless the host allows cheats.")
.validator(CheatValueAllowed);
// Name = "Infinite Parts"
REXCVAR_DEFINE_BOOL(infinite_parts, false, "Nuts&Bolts/Cheats", "vehicle parts never decreases. Off in online multiplayer unless the host allows cheats.")
.validator(CheatValueAllowed);
// Name = "Banjo Skins"
REXCVAR_DEFINE_STRING(banjo_skin, "default", "Nuts&Bolts/Skins", "Banjo skin override")
.allowed({ "default", "robot", "tuxedo" });
// Name = "No Out of Bounds Reset"
REXCVAR_DEFINE_BOOL(no_out_of_bounds_reset, false, "Nuts&Bolts/Cheats", "Going past the edge of the world no longer resets Banjo with a jiggy wall. Off in online multiplayer unless the host allows cheats.")
.validator(CheatValueAllowed);
// Name = "Max Acquired Parts Access"
REXCVAR_DEFINE_BOOL(max_acquired_parts_access, false, "Nuts&Bolts/Cheats", "Allows max acquired vehicle parts. Off in online multiplayer unless the host allows cheats.")
.validator(CheatValueAllowed);




// True: the fuel/ammo use returns before it takes anything (return_on_true).
bool Infinite_fuel_and_ammo() {
    return REXCVAR_GET(infinite_fuel_and_ammo) && CheatsAllowed();
}

void overworld_vehicles_hook(PPCRegister& r23, PPCRegister& r26) {
	// If true, then set r26 to 1. This adds "Change Vehicle" to the pause menu list.
	// r23 is left alone: in TU3's menu builder (sub_8258DC38) it is "in Showdown Town",
	// and past this point it only hides "Return to Showdown Town" and "Quit" (entries 5, 6).
	(void)r23;
    if (REXCVAR_GET(change_vehicles_anywhere) && CheatsAllowed()) {
		r26.u8 = 1;
    }
}

// True: a jet pushes in Showdown Town. The jet's physics step (sub_8223FF30)
// applies no thrust while world+88 == 1 (the town) unless vehicle+0x624 is
// set, so a vehicle brought in with the cheat only moves by its other parts.
bool town_jet_thrust() {
    return REXCVAR_GET(change_vehicles_anywhere) && CheatsAllowed();
}

// Showdown Town's invisible ceiling (a lid at 107 m, rising to 214 m over the
// middle) and its edge walls are triangles of the town's collision mesh
// (aid_havok_banjox_background_showdowntown_default). Their materials (12
// bytes: filter info, surface, flags) carry flag 0x04000000 (the ceiling) or
// 0x08000000 (the edge walls). The game's contact callback (sub_823456A0)
// has a mesh contact's material flags at 0x8234591C, r25 for body A's and
// r23 for body B's (0 when that body isn't a mesh), r24 and r27 being bodies
// A and B; 0x82345960 rejects the contact. True rejects a vehicle's contact
// with them in the town: vehicle bodies are on filter layer 7 with the
// vehicle as user data (sub_825F87D0), and the vehicle's world + 88 is 1 there.
static constexpr uint32_t kTownBoundaryMaterialFlags = 0x0C000000;
static constexpr uint32_t kBodyUserData = 0x0C;
static constexpr uint32_t kBodyFilterInfo = 0x2C;
static constexpr uint32_t kVehicleBodyLayer = 7;
static constexpr uint32_t kObjectWorld = 0x4C;
static constexpr uint32_t kWorldIsTown = 0x58;

static uint32_t GuestLoad32(uint32_t va) {
    return rex::byte_swap(*REX_KERNEL_MEMORY()->TranslateVirtual<const uint32_t*>(va));
}

static bool IsVehicleInTown(uint32_t body) {
    if (!body || (GuestLoad32(body + kBodyFilterInfo) & 0x3F) != kVehicleBodyLayer)
        return false;
    const uint32_t vehicle = GuestLoad32(body + kBodyUserData);
    const uint32_t world = vehicle ? GuestLoad32(vehicle + kObjectWorld) : 0;
    return world && GuestLoad32(world + kWorldIsTown) == 1;
}

bool town_boundary_pass(PPCRegister& r23, PPCRegister& r24, PPCRegister& r25, PPCRegister& r27) {
    if (!REXCVAR_GET(change_vehicles_anywhere) || !CheatsAllowed())
        return false;
    return ((r25.u32 & kTownBoundaryMaterialFlags) && IsVehicleInTown(r27.u32)) ||
           ((r23.u32 & kTownBoundaryMaterialFlags) && IsVehicleInTown(r24.u32));
}

// Past the edge of the world's physics (the broadphase box, from the level's
// bounds in sub_8238E258) the game's border listener queues the object
// (sub_8238DCA0), and sub_8238DDD8 sends each one EscapedBackground (message
// 22), which deletes it with what rides on it (sub_82384340). For the
// player's vehicle, or Banjo himself, he goes, and the game puts him back at
// the start with the default vehicle behind a jiggy wall. At 0x8238DE18 (r30
// the list index * 4, r31 the border) true skips the message, so with the
// cheat on nothing is removed for leaving the box: Banjo, his vehicle and
// anything else carry on outside it.
bool keep_escaped_vehicle(PPCRegister& r30, PPCRegister& r31) {
    (void)r30;
    (void)r31;
    return REXCVAR_GET(no_out_of_bounds_reset) && CheatsAllowed();
}
bool no_notes_spent() {
    if (REXCVAR_GET(no_notes_spent) && CheatsAllowed()) {
        return true;
    }
    return false;
}

bool disable_lod() {
    return REXCVAR_GET(disable_lod);
}

bool Infinite_health() {
    if (REXCVAR_GET(infinite_health) && CheatsAllowed()) {
        return true;
    }
    return false;
}

void Infinite_parts(PPCRegister& r11) {
    if (REXCVAR_GET(infinite_parts) && CheatsAllowed()) {
        r11.u32 = 100;
    }
}

bool BanjoActorOverride(PPCRegister& r3, PPCRegister& r5) {
    const auto& skin = REXCVAR_GET(banjo_skin);

    if (skin == "robot") {
        r3.u32 = 0x82148098;  // "robotbanjo_actor"
        r5.u32 = 0;
        return false;
    }

    if (skin == "tuxedo") {
        r3.u32 = 0x821480AC;  // "tuxedobanjo_actor"
        r5.u32 = 0;
        return false;
    }

    // "default" — let the original function run
    return true;
}

void Max_Acquired_Parts_Access_hook_1(PPCRegister& r11) {
    if (REXCVAR_GET(max_acquired_parts_access) && CheatsAllowed()) {
        r11.u32 = 255;
    }
}

void Max_Acquired_Parts_Access_hook_2(PPCRegister& r10) {
    if (REXCVAR_GET(max_acquired_parts_access) && CheatsAllowed()) {
        r10.u32 = 255;
    }
}

void Max_Acquired_Parts_Access_hook_3(PPCRegister& r11) {
    if (REXCVAR_GET(max_acquired_parts_access) && CheatsAllowed()) {
        r11.u32 = 255;
    }
}
