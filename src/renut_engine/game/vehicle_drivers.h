/**
 * @file    game/vehicle_drivers.h
 * @brief   The Remote Control (vehicle_drivers.cpp): a vehicle driven by a pad
 *          of reNut's own.
 */
#pragma once

#include <cstdint>

struct PPCContext;

namespace renut::game {

// padStatePush (keyboard_input.cpp's hook, game thread): true when it pushed
// the entry itself (the player's pad while a vehicle is driven for him).
bool VehicleDriversPadPush(PPCContext &ctx, uint8_t *base);

// The camera update (mouse_camera.cpp's hook, game thread): while the Remote
// Control drives, player 1's camera follows the vehicle.
void VehicleDriversCamera(PPCContext &ctx, uint8_t *base);

}  // namespace renut::game
