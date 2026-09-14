#include <atomic>
#include <chrono>
#include <thread>
#include <cstdint> 
#include <rex/cvar.h>
#include <rex/system/kernel_state.h>
#include <rex/hook.h>
#include "globals.h"
#include <rex/logging.h>
#include <rex/graphics/flags.h>

#if defined(_MSC_VER)
#include <stdlib.h>
#define REX_BSWAP32(x) _byteswap_ulong(x)
#else
#define REX_BSWAP32(x) __builtin_bswap32(x)
#endif


// Name = "Showdown Town Vehicles"
REXCVAR_DEFINE_BOOL(change_vehicles_anywhere, false, "Nuts&Bolts/Cheats", "Enables Changing Vehicles Anywhere.");
// Name = "No Notes Spent"
REXCVAR_DEFINE_BOOL(no_notes_spent, false, "Nuts&Bolts/Cheats", "hook created by serenity");
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
// disable particle effects
REXCVAR_DEFINE_BOOL(disable_particles, false, "Nuts&Bolts/Graphics", "Disables particle effects");
// Name = "Disable Shadows"
REXCVAR_DEFINE_BOOL(disable_shadows, false, "Nuts&Bolts/Graphics", "Disables shadows");
// Name = "Disable Contact Shadows"
REXCVAR_DEFINE_BOOL(disable_cao, false, "Nuts&Bolts/Graphics", "Disables the dark contact patches under characters (CAO)");
// Name = "Disable MSAA"
REXCVAR_DEFINE_BOOL(disable_msaa, false, "Nuts&Bolts/Graphics", "Disables MSAA on the scene render target. Matrices/aspect are unaffected. Applies on the next resolution change or restart.");
// Name = "Disable Motion Blur"
REXCVAR_DEFINE_BOOL(disable_motion_blur, false, "Nuts&Bolts/Graphics", "Disables the full-screen speed/camera motion blur");
// Name = "Max Acquired Parts Access"
REXCVAR_DEFINE_BOOL(max_acquired_parts_access, false, "Nuts&Bolts/Cheats", "Allows max acquired vehicle parts");
// Name = "Disable Screen Glow"
REXCVAR_DEFINE_BOOL(disable_screen_glow, true, "Nuts&Bolts/Graphics", "Fixes the split-second black flash on heal/damage by skipping the broken full-screen glow (it aliases EDRAM tile 0 in the recomp). Off = restore the glow.");


inline int bWidth = 640;
inline int bHeight = 480;
auto frameTime = std::chrono::system_clock::now();
int frame = 0;

void overworld_vehicles_hook(PPCRegister& r23, PPCRegister& r26) {
	// If true, then set r23 and r26 to 1. This will add "Change Vehicle" (r26) and "Build Vehicle" (r23) to the pause menu list.
    if (REXCVAR_GET(change_vehicles_anywhere)) {
		r23.u32 = 1;
		r26.u8 = 1;
    }
}

bool no_notes_spent() {
    if (REXCVAR_GET(no_notes_spent)) {
        return true;
    }
    return false;
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

bool disable_shadows() {
    return REXCVAR_GET(disable_shadows);
}

bool disable_shadows_cached() {
    return REXCVAR_GET(disable_shadows);
}

void shadows_atlas_rebuild(PPCRegister& r10) {
    static bool prevDisabled = false;
    static int  rebuildCallsLeft = 0;

    const bool disabled = REXCVAR_GET(disable_shadows);
    if (disabled != prevDisabled) {
        prevDisabled = disabled;
        rebuildCallsLeft = 20;
    }

    if (rebuildCallsLeft > 0) {
        --rebuildCallsLeft;
        r10.u32 = 1; // take the rebuild-all path
    }
}

bool disable_cao() {
    return REXCVAR_GET(disable_cao);
}

void disable_msaa_color(PPCRegister& r6) {
    if (REXCVAR_GET(disable_msaa)) {
        r6.u32 = 0; 
    }
}

void disable_msaa_depth(PPCRegister& r6) {
    if (REXCVAR_GET(disable_msaa)) {
        r6.u32 = 0; 
    }
}


bool disable_motion_blur() {
    return REXCVAR_GET(disable_motion_blur);
}

bool disable_screen_glow() {
    return REXCVAR_GET(disable_screen_glow);
}


bool disable_particles_sim()   { return REXCVAR_GET(disable_particles); }
bool disable_particles_spawn() { return REXCVAR_GET(disable_particles); }

bool disable_particles_draw0() { return REXCVAR_GET(disable_particles); }
bool disable_particles_draw1() { return REXCVAR_GET(disable_particles); }
bool disable_particles_draw2() { return REXCVAR_GET(disable_particles); }
bool disable_particles_draw3() { return REXCVAR_GET(disable_particles); }

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

void Max_Acquired_Parts_Access_hook_1(PPCRegister& r7) {
    if (REXCVAR_GET(max_acquired_parts_access)) {
        r7.u32 = 255;
    }
}

void Max_Acquired_Parts_Access_hook_2(PPCRegister& r10) {
    if (REXCVAR_GET(max_acquired_parts_access)) {
        r10.u32 = 255;
    }
}

void Max_Acquired_Parts_Access_hook_3(PPCRegister& r9) {
    if (REXCVAR_GET(max_acquired_parts_access)) {
        r9.u32 = 255;
    }
}
