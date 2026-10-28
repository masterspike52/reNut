/**
 * @file    game/restored_parts.h
 * @brief   The game's own parts reNut gives the player from the start: the
 *          Stereo (renut_stereo, a Jinjo Bingo prize; stereo.cpp plays it) and
 *          the cut Remote Control (renut_cut_parts).
 */
#pragma once

#include <cstdint>
#include <vector>

namespace renut::game {

// A part owned from the start, 'count' of it, from the garage inventory's
// build (custom_parts.cpp adds it).
struct RestoredPart {
  uint32_t aid = 0;  // its objparams' asset id
  uint32_t count = 0;
};

// Any thread.
std::vector<RestoredPart> RestoredParts();

}  // namespace renut::game
