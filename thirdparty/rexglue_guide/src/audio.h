#pragma once

#include "assets.h"

namespace rexglue_guide::audio {

// Plays one of the embedded Guide sounds. Notification sounds use their own
// stream so they don't cut off navigation sounds.
void Play(assets::Sound sound);
void SetEnabled(bool enabled);
void Shutdown();

}  // namespace rexglue_guide::audio
