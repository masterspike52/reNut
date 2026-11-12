#pragma once

#include <cstdint>
#include <filesystem>

struct PPCContext;

namespace renut::game {

// Game thread: plays the running challenge's song (asset id 'challenge', 0 for
// none) while 'in_play', pausing the level's music meanwhile.
void ChallengeMusicTick(PPCContext &ctx, uint8_t *base, uint32_t challenge, bool in_play);

// Game thread, each frame of a level: loops the music of the mod world whose scene script is
// 'scene' (renut.worlds.add's music), stopping it in any other.
void WorldMusicTick(PPCContext &ctx, uint8_t *base, uint32_t scene);

// A mod's music in every world, over a mod world's own (renut.sound.music): `file` looped at `volume` (0-1) with
// the level's music paused, as a mod world's is; an empty file goes back to the world's own.
void SetMusicOverride(const std::filesystem::path &file, float volume);

// Plays a sound file once, without a position, at 'volume' (0-1), stopping what 'channel' last played.
// It pauses with the game and stops as the level changes (NpcSoundsTick).
void PlayOneShot(const std::filesystem::path &file, float volume, uint32_t channel);

// A mod's sound (renut.sound): a file at 'volume' and 'pitch', looping or once, and with 'positional' at (x, y, z),
// fading out by 'range' units from the camera. Its handle, or 0 when it won't play. It pauses and stops as one-shots do.
uint32_t PlayModSound(const std::filesystem::path &file, float volume, float pitch, bool loop, bool positional, float x,
                      float y, float z, float range);
// Moves it or changes its volume; stops it. A handle that's done is fine.
void SetModSound(uint32_t handle, float volume, bool positional, float x, float y, float z);
// Its speed and pitch together (2: an octave up), 0.05 to 8.
void SetModSoundPitch(uint32_t handle, float pitch);
void StopModSound(uint32_t handle);
// It's still playing (or paused with the game).
bool ModSoundPlaying(uint32_t handle);

// Game thread, each frame of a level: keeps PlayOneShot's and the mods' sounds going (placing those with a position),
// and loads the dialog wavebanks of the game voices that scene script 'scene''s NPCs have (renut.npcs.add's voice).
void NpcSoundsTick(PPCContext &ctx, uint8_t *base, uint32_t scene);

}  // namespace renut::game
