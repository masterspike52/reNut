#pragma once

#include <rex/apu/audio_system.h>

namespace renut::game::audio_driver {

// Before the title launches: reNut renders the game's XAudio engine into one stream of `audio`, open for the
// whole run. Without it (no stream), the kernel's render clients stay in use.
void Attach(rex::apu::IAudioSystem &audio);

} // namespace renut::game::audio_driver
