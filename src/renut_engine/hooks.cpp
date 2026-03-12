#include "renut_engine/hooks.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(overworld_vehicles, false, "Nuts&Bolts", "Enables Overworld Vehicles");
REXCVAR_DEFINE_BOOL(no_notes_spent, false, "Nuts&Bolts", "hook created by serenity");
//REXCVAR_DEFINE_DOUBLE(fpsCount, 0.0, "Nuts&Bolts", ""); //This would be a frame timer display that updates every frame



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
    //REXCVAR_SET(fpsCount, 60.0);
    r3.u64 = 0;
}