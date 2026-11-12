#pragma once

#include <cstdint>

namespace renut::gpu {

// Reads menu_background_color; with the graphics options, once a frame.
void LatchMenuColourLocked();

// Changes whenever the panels' colour does: a panel uploaded under another
// generation is uploaded again (HostTexture::menu_colour).
uint32_t MenuColourGeneration();

// Recolours a level of BC2 blocks ('blocks_h' rows of 'blocks_w' blocks,
// 'row_pitch' bytes apart) when it is a menu panel; any thread.
void RecolourMenuPanel(uint8_t *blocks, uint32_t blocks_w, uint32_t blocks_h, uint32_t row_pitch);

} // namespace renut::gpu
