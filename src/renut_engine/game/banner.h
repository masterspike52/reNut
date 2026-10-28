/**
 * @file    game/banner.h
 * @brief   A line of text shown a few seconds at the top of the screen
 *          (the Stereo's track, the Remote Control's state).
 */
#pragma once

#include <string>

namespace renut::game {

// Any thread: shows 'text' for three seconds, in place of the last.
void ShowBanner(std::string text);

}  // namespace renut::game
