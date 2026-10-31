/**
 * @file    gpu/render/settings_icon.h
 * @brief   The reNut pause tab's gear icon in place of the unused map tab's.
 *
 * reNut's pause tab (game/graphics_menu.cpp) is the game's unused map tab,
 * whose icon is aid_texture_banjox_ui_pausemenuicons_townmap: 96x96, BC2, one
 * level, shown nowhere else. Its blocks are recognised by their hash as the
 * texture uploads, and the gear (settings_icon_data.inc, made by
 * tools/make_menu_icon.py from icon/settings_gear.png) is uploaded in their
 * place.
 */
#pragma once

#include <cstdint>

namespace renut::gpu {

// Puts the gear in a level of BC2 blocks ('blocks_h' rows of 'blocks_w'
// blocks, 'row_pitch' bytes apart) that is the map tab's icon; true when it
// did. Any thread.
bool ReplaceSettingsIcon(uint8_t *blocks, uint32_t blocks_w, uint32_t blocks_h, uint32_t row_pitch);

} // namespace renut::gpu
