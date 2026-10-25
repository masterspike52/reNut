/**
 * @file    gpu/render/title_logo.h
 * @brief   reNut's title logo in place of the game's (renut_title_logo).
 *
 * The game's logo is one texture, aid_texture_banjox_ui_logonew: 1280x720,
 * BC2, one level, drawn over the title screen. Its blocks are recognised by
 * their hash as the texture uploads, and reNut's logo (title_logo_data.inc,
 * made by tools/make_title_logo.py from icon/title_logo.png) is uploaded in
 * their place.
 */
#pragma once

#include <cstdint>

namespace renut::gpu {

// Puts reNut's logo in a level of BC2 blocks ('blocks_h' rows of 'blocks_w'
// blocks, 'row_pitch' bytes apart) that is the game's; true when it did. Any
// thread.
bool ReplaceTitleLogo(uint8_t *blocks, uint32_t blocks_w, uint32_t blocks_h, uint32_t row_pitch);

} // namespace renut::gpu
