// Decoding the game's audio (wave bank waves, media.hpp) to 16-bit PCM.
//
// Almost every wave is XMA (Xbox 360 WMA Pro variant); FFmpeg decodes it, reading the RIFF XMA2
// file that media::waveToFile writes. ADPCM and WMA go the same way. PCM is converted directly,
// so it plays even in builds without FFmpeg.
#pragma once

#include "common.hpp"
#include "media.hpp"

namespace nut::audio {

struct Pcm {
    uint32_t rate = 0;
    uint32_t channels = 0;
    std::vector<int16_t> samples; // interleaved
    size_t frames() const { return channels ? samples.size() / channels : 0; }
    double seconds() const { return rate ? double(frames()) / rate : 0; }
};

bool haveDecoder();                     // FFmpeg is linked in
bool canDecode(const media::Wave& w);   // PCM always, the rest with FFmpeg
Pcm decode(const media::Wave& w);       // throws when it cannot
Bytes toWav(const Pcm& p);              // RIFF WAVE, 16-bit PCM

} // namespace nut::audio
