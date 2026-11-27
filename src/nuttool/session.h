#pragma once

#include <rex/ppc/hook.h>

#include <cstdint>
#include <functional>

namespace renut::nuttool {

// Work for the game's thread, where guest calls are safe.
using GameWork = std::function<void(PPCContext &ctx, uint8_t *base)>;

// Any thread: runs `work` at the studio's next tick (nothing runs outside the studio).
void OnGameThread(GameWork work);

// Any thread: whether the studio is the running world, as of its last tick.
bool InStudio();

// Any thread: which time the studio was entered this session (0 before the first); objects made on an earlier visit
// went with that world.
uint64_t StudioVisit();

// Any thread: back to the title (its main menu), as the pause menu's quit does.
void LeaveStudio();

// Game thread, as a world is torn down (its scene script `scene`): the studio's ending lets go of the camera, the music
// and the save block, however it was left.
void WorldEnding(PPCContext &ctx, uint8_t *base, uint32_t scene);

// Game thread, once a frame of a level (challengeManTick): runs the queued work while the studio runs.
void StudioTick(PPCContext &ctx, uint8_t *base);

// Once, before the game starts: NutTool's overlay window, shown while the studio runs.
void StartSession();

} // namespace renut::nuttool
