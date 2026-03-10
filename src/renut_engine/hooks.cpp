#include "renut_engine/hooks.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(overworld_vehicles, false, "Nuts&Bolts", "Enables Overworld Vehicles");

//forword delare loc_825A8F24



bool overworld_vehicles_hook() {
  if (REXCVAR_GET(overworld_vehicles)) {
    return true;
  }
  return false;
}
