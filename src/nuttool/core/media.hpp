// Video and audio containers that the game stores without Rare wrapping.
//
// XMV movies (asset type 0x36) are plain ASF/WMV files.
// Wave banks (asset type 0x12, inside stream bundles) are big-endian XACT wave banks
// ("WBND" stored byte-reversed as "DNBW").
#pragma once

#include "common.hpp"

namespace nut::media {

bool isAsf(const Bytes& d);
// Human-readable summary (duration, streams) of an ASF file.
std::string describeAsf(const Bytes& d);

bool isWaveBank(const Bytes& d);

struct Wave {
    std::string name;
    uint32_t formatTag = 0; // 0 PCM, 1 XMA, 2 ADPCM, 3 WMA
    uint32_t channels = 0, sampleRate = 0, blockAlign = 0, bitsPerSample = 0;
    uint32_t durationSamples = 0;
    uint32_t loopStart = 0, loopLength = 0;
    Bytes data;
};

struct WaveBank {
    std::string name;
    uint32_t version = 0;
    std::vector<Wave> waves;
};

// Without `withData` only the tables are read (names, formats, lengths): `d` may then be just the
// start of the bank, up to the end of its name table (headerBytes).
WaveBank parseWaveBank(const Bytes& d, bool withData = true);
// Bytes from the start of a bank that hold every table but the wave data, from its first 52 bytes.
size_t waveBankHeaderBytes(const Bytes& head);
// Playable file for one wave: RIFF WAVE for PCM/ADPCM, RIFF XMA2 for XMA, raw otherwise.
// `ext` receives the suggested file extension.
Bytes waveToFile(const Wave& w, std::string& ext);

} // namespace nut::media
