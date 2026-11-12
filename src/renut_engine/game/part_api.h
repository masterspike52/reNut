#pragma once

#include <rex/ppc/context.h>

#include <cstdint>

namespace renut::mods {
struct CustomPart;
}

namespace renut::game::part_api {

using renut::mods::CustomPart;

// Before rexfabric::Start: declares the renut namespace for RexFabric mods (renut.parts,
// renut.vehicle, renut.fx and the renut.* events). docs/lua-mods/lua-parts.md describes it.
void Declare();

// Game thread, from a behaviour part's class callbacks (their guest context is lent to
// what mods call). Each fires its renut.part_* event.
void Spawn(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance);
void Destroy(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance);
void Attach(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle);
void Detach(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle);
void Tick(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle, double dt);
void Physics(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle,
             double dt);
void Power(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle, bool on);
void Button(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle,
            bool down);

// Game thread, once a frame of a level (challengeManTick): fires renut.game_tick, which also runs the calls mods
// made outside the renut events (a key press's) that need one.
void GameTick(PPCContext &ctx, uint8_t *base);

// While a mod spawns something: notes that asset `aid` failed to open and returns true, so the open stays quiet
// (no dirty-disc screen) and the spawn raises instead. Otherwise false.
bool QuietFailedOpen(uint32_t aid);
bool Spawning();

// Whether a mod spawned `object` (renut.actors.spawn and the like): a mod's character spawned so isn't its world's.
bool ModSpawned(uint32_t object);

} // namespace renut::game::part_api
