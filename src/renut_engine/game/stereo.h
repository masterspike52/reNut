/**
 * @file    game/stereo.h
 * @brief   Music reNut plays for two Spiral Mountain challenges (stereo.cpp,
 *          with the Stereo's game songs).
 */
#pragma once

#include <cstdint>

struct PPCContext;

namespace renut::game {

// The challenge manager's update (frameHooks.cpp): 'challenge' is the running
// challenge's asset id (0 for none), 'in_play' whether it has started and not
// reached its results. Six of the Best plays Spiral Challenge, the Spiral
// Mountain Showdown Grunty's Challenge, while in play (their wavebanks loading
// from when the challenge is chosen), the level's music paused meanwhile.
// Game thread.
void ChallengeMusicTick(PPCContext &ctx, uint8_t *base, uint32_t challenge, bool in_play);

}  // namespace renut::game
