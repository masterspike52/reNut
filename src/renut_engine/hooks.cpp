#include "renut_engine/hooks.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdint> // For uintptr_t

#include <rex/cvar.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(overworld_vehicles, false, "Nuts&Bolts", "Enables Overworld Vehicles");
REXCVAR_DEFINE_BOOL(no_notes_spent, false, "Nuts&Bolts", "hook created by serenity");
REXCVAR_DEFINE_INT32(fpsvsync, 0, "Nuts&Bolts", "Immediate (0) 60Hz (1) 30Hz (2) VSync = 30Hz, Threshold 20 (7)")
.range(0, 7)
.lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(lowres, false, "Nuts&Bolts", "Makes the game 480p");
REXCVAR_DEFINE_DOUBLE(fpsCount, 0.0, "Nuts&Bolts","");  // This would be a frame timer display that updates every frame

inline int bWidth = 640;
inline int bHeight = 480;
auto frameTime=std::chrono::system_clock::now();
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

    r3.u64 = REXCVAR_GET(fpsvsync);
}

void fpsCount_hook() {
  frame++;
  auto Time = std::chrono::system_clock::now();
  std::chrono::duration<double, std::milli> delta = Time - frameTime;
  frameTime = Time;
  double fpsfromMS = 1000 / delta.count();
  if (frame >= 60) {
    frame = 0;
    REXCVAR_SET(fpsCount, fpsfromMS);
  }
}

bool meGetResolutionParams_hook(PPCRegister& r5, PPCRegister& r6){
 // r5.u32 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&bWidth));
  //r6.u32 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&bHeight));
  if(REXCVAR_GET(lowres)){
    
  return true;
  }
  return false;
}

void Optimization_Hook(){
  std::this_thread::yield();
}

