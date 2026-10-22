/**
 * @file    gpu/guest/widescreen.h
 * @brief   Widescreen: N&B renders its frame at the window's aspect ratio.
 *
 * The game's screen is 1280x720 (sub_823ED828). For a window wider than 16:9
 * (renut_aspect_ratio) the game is told a wider screen of the same height, so
 * its back buffer, viewports, safe areas and XUI root follow it, and its
 * projection widens (Hor+): the vertical field of view stays the game's.
 * The guest buffers it sizes for 1280 (sub_823191B8) are sized for the wider
 * screen. Decided the first time the game asks, and kept until restart.
 * XUI (menus, title screen, HUD) keeps its 1280x720 root, as its scenes are
 * laid out for, and is drawn centred; markers that follow the world are moved
 * into that centred space.
 */
#pragma once

#include <cstdint>

namespace renut::gpu {

// The game's screen width: 1280, or wider for a widescreen window. The height
// stays 720.
uint32_t GuestScreenWidth();

} // namespace renut::gpu
