/**
 * @file    game/controllers.h
 * @brief   Player 1's controller (controllers.cpp).
 */
#pragma once

#include <cstdint>

namespace renut::controllers {

// Player 1's buttons (XINPUT_GAMEPAD wButtons) from the game's last read of its
// state; 0 without a controller.
uint16_t Player1Buttons();

}  // namespace renut::controllers
