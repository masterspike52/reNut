#pragma once

#include <cstdint>

namespace renut::gpu {

// Puts the gear in a level of BC2 blocks ('row_pitch' bytes a row) that is the
// map tab's icon; true when it did. Any thread.
bool ReplaceSettingsIcon(uint8_t *blocks, uint32_t blocks_w, uint32_t blocks_h, uint32_t row_pitch);

} // namespace renut::gpu
