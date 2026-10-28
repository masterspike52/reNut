/**
 * @file    game/controllers.h
 * @brief   Which player each controller is (controllers.cpp).
 */
#pragma once

namespace renut::game {

// Any thread: the player (0-3) the controller in host slot 'pad' is, as of the
// last probe, whether or not that player is given to the game; -1 for none.
// The Guide opens for that player when the controller presses its Guide
// button.
int ControllerPlayer(int pad);

} // namespace renut::game
