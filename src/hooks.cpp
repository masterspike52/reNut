#include <rex/hook.h>

#include <string_view>

#include "renut_engine/online/coop.h"
#include "renut_engine/online/online.h"

// Cheats are off in online multiplayer: while the game has a session
// (renut::online::InSession) they cannot be turned on, and those already on do
// nothing.
static bool CheatsAllowed() {
    return !renut::online::InSession();
}

// A cheat's new value: turning one off is always allowed, turning one on only
// outside multiplayer.
static bool CheatValueAllowed(std::string_view value) {
    return CheatsAllowed() || (value != "true" && value != "1");
}

// Changing vehicles anywhere is also allowed in co-op (online/coop.h), which
// is no contest.
static bool VehicleCheatAllowed() {
    return CheatsAllowed() || renut::online::coop::CurrentRole() != renut::online::coop::Role::kNone;
}

static bool VehicleCheatValueAllowed(std::string_view value) {
    return VehicleCheatAllowed() || (value != "true" && value != "1");
}

// Name = "Showdown Town Vehicles"
REXCVAR_DEFINE_BOOL(change_vehicles_anywhere, false, "Nuts&Bolts/Cheats", "Enables Changing Vehicles Anywhere. Off in online multiplayer, but for co-op.")
.validator(VehicleCheatValueAllowed);
// Name = "No Notes Spent"
REXCVAR_DEFINE_BOOL(no_notes_spent, false, "Nuts&Bolts/Cheats", "hook created by serenity. Off in online multiplayer.")
.validator(CheatValueAllowed);
// Name = "Disable LOD"
REXCVAR_DEFINE_BOOL(disable_lod, false, "Nuts&Bolts/Graphics", "Disables LOD (Level of Detail) scaling");
// Name = "Infinite Fuel and Ammo"
REXCVAR_DEFINE_BOOL(infinite_fuel_and_ammo, false, "Nuts&Bolts/Cheats", "fuel never decreases. Off in online multiplayer.")
.validator(CheatValueAllowed);
// Name = "Infinite Health"
REXCVAR_DEFINE_BOOL(infinite_health, false, "Nuts&Bolts/Cheats", "health never decreases. Off in online multiplayer.")
.validator(CheatValueAllowed);
// Name = "Infinite Parts"
REXCVAR_DEFINE_BOOL(infinite_parts, false, "Nuts&Bolts/Cheats", "vehicle parts never decreases. Off in online multiplayer.")
.validator(CheatValueAllowed);
// Name = "Banjo Skins"
REXCVAR_DEFINE_STRING(banjo_skin, "default", "Nuts&Bolts/Skins", "Banjo skin override")
.allowed({ "default", "robot", "tuxedo" });
// Name = "Max Acquired Parts Access"
REXCVAR_DEFINE_BOOL(max_acquired_parts_access, false, "Nuts&Bolts/Cheats", "Allows max acquired vehicle parts. Off in online multiplayer.")
.validator(CheatValueAllowed);




void Infinite_fuel_and_ammo() {
    if (REXCVAR_GET(infinite_fuel_and_ammo) && CheatsAllowed()) {
        return;
    }
}

void overworld_vehicles_hook(PPCRegister& r23, PPCRegister& r26) {
	// If true, then set r23 and r26 to 1. This will add "Change Vehicle" (r26) and "Build Vehicle" (r23) to the pause menu list.
    if (REXCVAR_GET(change_vehicles_anywhere) && VehicleCheatAllowed()) {
		r23.u32 = 1;
		r26.u8 = 1;
    }
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
        r3.u32 = 0x8216B690;  // "robotbanjo_actor"
        r5.u32 = 0;
        return false;
    }

    if (skin == "tuxedo") {
        r3.u32 = 0x8216B6A4;  // "tuxedobanjo_actor"
        r5.u32 = 0;
        return false;
    }

    // "default" — let the original function run
    return true;
}

void Max_Acquired_Parts_Access_hook_1(PPCRegister& r7) {
    if (REXCVAR_GET(max_acquired_parts_access) && CheatsAllowed()) {
        r7.u32 = 255;
    }
}

void Max_Acquired_Parts_Access_hook_2(PPCRegister& r10) {
    if (REXCVAR_GET(max_acquired_parts_access) && CheatsAllowed()) {
        r10.u32 = 255;
    }
}

void Max_Acquired_Parts_Access_hook_3(PPCRegister& r9) {
    if (REXCVAR_GET(max_acquired_parts_access) && CheatsAllowed()) {
        r9.u32 = 255;
    }
}
