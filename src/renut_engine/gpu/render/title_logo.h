#pragma once

#include <cstdint>

namespace renut::gpu {

// Puts reNut's logo in a level of BC2 blocks ('row_pitch' bytes a row) that is
// the game's logo; true when it did. Any thread.
bool ReplaceTitleLogo(uint8_t *blocks, uint32_t blocks_w, uint32_t blocks_h, uint32_t row_pitch);

} // namespace renut::gpu
