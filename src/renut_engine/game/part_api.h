#pragma once

#include <rex/ppc/context.h>

#include <cstdint>
#include <string>
#include <vector>

namespace renut::mods {
struct CustomPart;
}

namespace renut::game::part_api {

using renut::mods::CustomPart;

// Before rexfabric::Start: declares the renut namespace for RexFabric mods (renut.parts,
// renut.vehicle, renut.fx and the renut.* events). docs/renut-api.md describes it.
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

// Any thread: the camera stands at `at` looking at `look` until ReleaseCamera (as renut.camera.set and .release).
// A `reach` above 0 is the view's far distance meanwhile (the game caps it at 5000).
void SetCamera(const float at[3], const float look[3], float reach = 0);
void ReleaseCamera();

// Game thread (NutTool's studio): loads data bundle `bundle` (and its stream bundle; 0 for none) into the running world,
// then shows model `model` there on a plain object at `at`. Returns the object; raises std::runtime_error as spawns do.
uint32_t ShowModel(PPCContext &ctx, uint8_t *base, uint32_t model, uint32_t bundle, const float at[3]);
// Game thread: removes what ShowModel made, and plays an animation asset on it (one not loaded here is skipped). The
// caller makes sure the object is still there (made in this world); these don't look it up.
void RemoveShown(PPCContext &ctx, uint8_t *base, uint32_t object);
void PlayShownAnimation(PPCContext &ctx, uint8_t *base, uint32_t object, uint32_t anim);
// Game thread: player 1's character goes out of sight (its body's Hidden state, as cutscenes hide him).
void HidePlayer(PPCContext &ctx, uint8_t *base);
// Game thread: the shown model's skin matrices this frame, in its own space (three rows of four floats a bone: a joint
// is where its row matrix takes its bind position). It is put back at `at` first, as root motion moves it; false when none.
bool ShownPose(PPCContext &ctx, uint8_t *base, uint32_t object, const float at[3], std::vector<float> &rows);
// Game thread: the shown model draws with these skin matrices (ShownPose's rows) over its animation's, every frame
// until called again; empty rows give it back to its animation.
void HoldShownPose(PPCContext &ctx, uint8_t *base, uint32_t object, const std::vector<float> &rows);
// Any thread: whether a render graph instance is a shown model's (lod.cpp keeps those at their top detail level).
bool ShownRenderInstance(uint32_t instance);
// Game thread: one of the shown model's switches (the parts it picks between, Kazooie in Banjo's) to a state; a
// negative state shows none of them.
void SetShownSwitch(PPCContext &ctx, uint8_t *base, uint32_t object, int id, int state);

} // namespace renut::game::part_api
