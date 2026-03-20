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


// Name = "Showdown Town Vehicles"
REXCVAR_DEFINE_BOOL(overworld_vehicles, false, "Nuts&Bolts", "Enables Overworld Vehicles");
// Name = "No Notes Spent"
REXCVAR_DEFINE_BOOL(no_notes_spent, false, "Nuts&Bolts", "hook created by serenity");
// Name = "VSync Mode"
REXCVAR_DEFINE_INT32(vsync, 0, "Nuts&Bolts", "Immediate (0) 60Hz (1) 30Hz (2) VSync = 30Hz, Threshold 20 (7)")
.range(0, 7)
.lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Name = "Disable LOD"
REXCVAR_DEFINE_BOOL(disable_lod, false, "Nuts&Bolts", "Disables LOD (Level of Detail) scaling");
// Name = "Infinite Fuel and Ammo"
REXCVAR_DEFINE_BOOL(infinite_fuel_and_ammo, false, "Nuts&Bolts", "fuel never decreases");
// Name = "Infinite Health"
REXCVAR_DEFINE_BOOL(infinite_health, false, "Nuts&Bolts", "health never decreases");
// Name = "Infinite Weight and Capacity"
REXCVAR_DEFINE_BOOL(infinite_weight_and_capacity, false, "Nuts&Bolts", "weight and parts capacity never decreases in mumbos motors");




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

void fps_hook(PPCRegister& r3) {

    r3.u64 = REXCVAR_GET(vsync);
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

bool Infinite_weight_part_capacity() {
    if (REXCVAR_GET(infinite_weight_and_capacity)) {
        return true;
    }
    return false;
}

bool Infinite_health() {
    if (REXCVAR_GET(infinite_health)) {
        return true;
    }
    return false;
}