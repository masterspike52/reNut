#include <atomic>
#include <chrono>
#include <thread>
#include <cstdint> // For uintptr_t
#include <rex/cvar.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include "rex_macros.h"
#include "globals.h"
#include <rex/logging.h>
#include <rex/graphics/flags.h>


// Name = "Showdown Town Vehicles"
REXCVAR_DEFINE_BOOL(overworld_vehicles, false, "Nuts&Bolts/Cheats", "Enables Overworld Vehicles");
// Name = "No Notes Spent"
REXCVAR_DEFINE_BOOL(no_notes_spent, false, "Nuts&Bolts/Cheats", "hook created by serenity");
// Name = "VSync Mode"
REXCVAR_DEFINE_INT32(target_fps, 60, "Nuts&Bolts/Performance", "Target frame rate cap. 30 = original, 60 = unlocked")
.range(30, 60)
.validator([](std::string_view v) {
    return v == "30" || v == "60";
    });
// Name = "Disable LOD"
REXCVAR_DEFINE_BOOL(disable_lod, false, "Nuts&Bolts/Graphics", "Disables LOD (Level of Detail) scaling");
// Name = "Infinite Fuel and Ammo"
REXCVAR_DEFINE_BOOL(infinite_fuel_and_ammo, false, "Nuts&Bolts/Cheats", "fuel never decreases");
// Name = "Infinite Health"
REXCVAR_DEFINE_BOOL(infinite_health, false, "Nuts&Bolts/Cheats", "health never decreases");
// Name = "No Timer"
REXCVAR_DEFINE_BOOL(no_timer, false, "Nuts&Bolts/Cheats", "timer never goes past 0 in missions with a timer");
// Name = "Infinite Parts"
REXCVAR_DEFINE_BOOL(infinite_parts, false, "Nuts&Bolts/Cheats", "vehicle parts never decreases");
// Name = "Extended Build Range"
REXCVAR_DEFINE_BOOL(extended_build_range, false, "Nuts&Bolts/Cheats", "Allows you to build in a bit bigger area of mumbos motors");
// Name = "Banjo Skins"
REXCVAR_DEFINE_STRING(banjo_skin, "default", "Nuts&Bolts/Skins", "Banjo skin override")
.allowed({ "default", "robot", "tuxedo" });



inline int bWidth = 640;
inline int bHeight = 480;
auto frameTime = std::chrono::system_clock::now();
int frame = 0;

bool overworld_vehicles_hook() {
    if (REXCVAR_GET(overworld_vehicles)) {
        return true;
    }
    return false;
}

bool no_notes_spent() {
    if (REXCVAR_GET(no_notes_spent)) {
        return true;
    }
    return false;
}

    void fps_hook(PPCRegister & r11) {
        if (REXCVAR_GET(target_fps) == 60) {
            r11.u32 = 1; 
        }
    }

void fpsCount_hook() {
    frame++;
    auto Time = std::chrono::system_clock::now();
    std::chrono::duration<double, std::milli> delta = Time - frameTime;
    frameTime = Time;
    double fpsfromMS = 1000 / delta.count();
    if (frame >= 60) {
        frame = 0;
        fpsCount = fpsfromMS;
    }
}


bool meGetResolutionParams_hook(PPCRegister& r5, PPCRegister& r6) {
    // r5.u32 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&bWidth));
     //r6.u32 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&bHeight));
    //if (REXCVAR_GET(lowres)) {
    //
    //    return true;
    //}
    return false;
}

void Optimization_Hook() {
    std::this_thread::yield();
}

bool disable_lod() {
    return REXCVAR_GET(disable_lod);
}

void Infinite_fuel_and_ammo() {
    if (REXCVAR_GET(infinite_fuel_and_ammo)) {
        return;
    }
}

bool Infinite_health() {
    if (REXCVAR_GET(infinite_health)) {
        return true;
    }
    return false;
}

bool No_Timer() {
    if (REXCVAR_GET(no_timer)) {
        return true;
    }
    return false;
}

void Infinite_parts(PPCRegister& r11) {
    if (REXCVAR_GET(infinite_parts)) {
        r11.u32 = 100;
    }
}

bool Extended_build_range() {
    if (REXCVAR_GET(extended_build_range)) {
        return true;
    }
    return false;
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



