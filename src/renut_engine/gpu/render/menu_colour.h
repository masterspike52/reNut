/**
 * @file    gpu/render/menu_colour.h
 * @brief   The colour of the see-through blue panels behind the game's menus
 *          (menu_background_color).
 *
 * The panels are XUI images (the garage's dial panel, its part list, the
 * blueprints' and the pause menu's), each a texture of its own shape, BC2,
 * one level: the blue (41, 52, 99) at alpha 204 inside, darker shades of it
 * toward the edges, a white rim. The image draw multiplies the texture by a
 * colour (pixel constant c1), which can't turn blue into another hue, so the
 * colour is changed in the texture as it is uploaded: a BC2 texture whose
 * blocks are mostly that blue is a panel, and each block's two colours, read
 * as a mix of the panel blue and white (grey), get the chosen colour in place
 * of the blue. The rim stays white, the shading stays, colours that are no
 * such mix (an icon) stay as they are.
 */
#pragma once

#include <cstdint>

namespace renut::gpu {

// Reads menu_background_color; with the graphics options, once a frame.
void LatchMenuColourLocked();

// Changes whenever the panels' colour does: a panel uploaded under another
// generation is uploaded again (HostTexture::menu_colour).
uint32_t MenuColourGeneration();

// Recolours a level of BC2 blocks ('blocks_h' rows of 'blocks_w' blocks,
// 'row_pitch' bytes apart) when it is a menu panel. Any thread; the latched
// colour does not change while uploads are read.
void RecolourMenuPanel(uint8_t *blocks, uint32_t blocks_w, uint32_t blocks_h, uint32_t row_pitch);

} // namespace renut::gpu
