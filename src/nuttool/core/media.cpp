#include "media.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nut::media {
namespace {

// ASF object GUIDs in file byte order.
const uint8_t kHeaderGuid[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
const uint8_t kFilePropsGuid[16] = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
const uint8_t kStreamPropsGuid[16] = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11, 0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
const uint8_t kAudioGuid[16] = {0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
const uint8_t kVideoGuid[16] = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};

uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }

} // namespace

bool isAsf(const Bytes& d) { return d.size() >= 30 && std::memcmp(d.data(), kHeaderGuid, 16) == 0; }

std::string describeAsf(const Bytes& d) {
    if (!isAsf(d)) return "not an ASF file";
    std::string out;
    uint32_t count = le32(&d[24]);
    size_t p = 30;
    char t[256];
    for (uint32_t i = 0; i < count && p + 24 <= d.size(); ++i) {
        uint64_t size = le64(&d[p + 16]);
        if (size < 24 || p + size > d.size()) break;
        const uint8_t* o = &d[p];
        if (std::memcmp(o, kFilePropsGuid, 16) == 0 && size >= 104) {
            uint64_t dur = le64(o + 24 + 40), preroll = le64(o + 24 + 56);
            double secs = double(dur) / 1e7 - double(preroll) / 1e3;
            std::snprintf(t, sizeof t, "duration %.2f s, packets %llu\n", secs, (unsigned long long)le64(o + 24 + 32));
            out += t;
        } else if (std::memcmp(o, kStreamPropsGuid, 16) == 0 && size >= 78) {
            const uint8_t* body = o + 24;
            uint32_t typeLen = le32(body + 40);
            uint16_t flags = le16(body + 48);
            const uint8_t* ts = body + 54;
            if (std::memcmp(body, kAudioGuid, 16) == 0 && typeLen >= 16) {
                std::snprintf(t, sizeof t, "stream %u: audio, codec 0x%04x, %u ch, %u Hz\n", flags & 0x7F, le16(ts), le16(ts + 2), le32(ts + 4));
            } else if (std::memcmp(body, kVideoGuid, 16) == 0 && typeLen >= 51) {
                const uint8_t* bih = ts + 11;
                char fcc[5] = {char(bih[16]), char(bih[17]), char(bih[18]), char(bih[19]), 0};
                std::snprintf(t, sizeof t, "stream %u: video %s %ux%u\n", flags & 0x7F, fcc, le32(bih + 4), le32(bih + 8));
            } else {
                std::snprintf(t, sizeof t, "stream %u: other\n", flags & 0x7F);
            }
            out += t;
        }
        p += size_t(size);
    }
    return out.empty() ? "ASF file (no properties found)\n" : out;
}

bool isWaveBank(const Bytes& d) { return d.size() >= 52 && (std::memcmp(d.data(), "DNBW", 4) == 0 || std::memcmp(d.data(), "WBND", 4) == 0); }

size_t waveBankHeaderBytes(const Bytes& head) {
    if (!isWaveBank(head)) fail("not an XACT wave bank");
    bool be = std::memcmp(head.data(), "DNBW", 4) == 0;
    Reader r(head, be);
    r.skip(12);
    size_t end = 52;
    for (int s = 0; s < 5; ++s) {
        uint32_t off = r.u32(), len = r.u32();
        if (s < 4) end = std::max(end, size_t(off) + len); // segment 4 is the wave data
    }
    return end;
}

WaveBank parseWaveBank(const Bytes& d, bool withData) {
    if (!isWaveBank(d)) fail("not an XACT wave bank");
    bool be = std::memcmp(d.data(), "DNBW", 4) == 0;
    Reader r(d, be);
    r.skip(4);
    WaveBank wb;
    wb.version = r.u32();
    r.u32(); // header version
    struct Seg { uint32_t off, len; } seg[5];
    for (auto& s : seg) { s.off = r.u32(); s.len = r.u32(); }
    r.seek(seg[0].off);
    uint32_t flags = r.u32(), count = r.u32();
    char name[65] = {};
    r.bytes(name, 64);
    wb.name = name;
    uint32_t metaSize = r.u32(), nameSize = r.u32(), alignment = r.u32();
    bool compact = (flags & 0x00020000) != 0;
    if (compact) fail("compact wave banks are not supported");
    if (metaSize < 24) fail("unexpected wave bank entry size");
    (void)alignment;
    for (uint32_t i = 0; i < count; ++i) {
        Reader m(d, be);
        m.seek(seg[1].off + size_t(i) * metaSize);
        Wave w;
        uint32_t fd = m.u32();
        uint32_t fmt = m.u32();
        uint32_t off = m.u32(), len = m.u32();
        w.loopStart = m.u32();
        w.loopLength = m.u32();
        w.durationSamples = fd >> 4;
        w.formatTag = fmt & 3;
        w.channels = (fmt >> 2) & 7;
        w.sampleRate = (fmt >> 5) & 0x3FFFF;
        // XMA entries store the encoder's exact rate (e.g. 44087); decoders expect standard rates.
        for (uint32_t std : {8000u, 11025u, 16000u, 22050u, 24000u, 32000u, 44100u, 48000u})
            if (w.formatTag == 1 && w.sampleRate * 1000 > std * 995 && w.sampleRate * 1000 < std * 1005) w.sampleRate = std;
        w.blockAlign = (fmt >> 23) & 0xFF;
        w.bitsPerSample = (fmt >> 31) ? 16 : 8;
        if (withData) {
            if (seg[4].off + uint64_t(off) + len > d.size()) fail("wave " + std::to_string(i) + " data out of range");
            w.data.assign(d.begin() + seg[4].off + off, d.begin() + seg[4].off + off + len);
        }
        if (seg[3].len && nameSize) {
            Reader nr(d, be);
            nr.seek(seg[3].off + size_t(i) * nameSize);
            std::string n(reinterpret_cast<const char*>(nr.cur()), std::min<size_t>(nameSize, nr.remaining()));
            w.name = n.substr(0, n.find('\0'));
        }
        if (w.name.empty()) w.name = "wave_" + std::to_string(i);
        wb.waves.push_back(std::move(w));
    }
    return wb;
}

Bytes waveToFile(const Wave& w, std::string& ext) {
    Writer o(false);
    auto riff = [&](const Bytes& fmtChunk, const Bytes& data) {
        o.bytes("RIFF", 4);
        o.u32(uint32_t(4 + 8 + fmtChunk.size() + 8 + data.size()));
        o.bytes("WAVE", 4);
        o.bytes("fmt ", 4);
        o.u32(uint32_t(fmtChunk.size()));
        o.bytes(fmtChunk);
        o.bytes("data", 4);
        o.u32(uint32_t(data.size()));
        o.bytes(data);
    };
    Writer f(false);
    if (w.formatTag == 0) {
        ext = ".wav";
        Bytes data = w.data;
        if (w.bitsPerSample == 16) for (size_t i = 0; i + 1 < data.size(); i += 2) std::swap(data[i], data[i + 1]);
        uint32_t ba = w.channels * w.bitsPerSample / 8;
        f.u16(1);
        f.u16(uint16_t(w.channels));
        f.u32(w.sampleRate);
        f.u32(w.sampleRate * ba);
        f.u16(uint16_t(ba));
        f.u16(uint16_t(w.bitsPerSample));
        riff(f.buf(), data);
    } else if (w.formatTag == 1) {
        // XMA2WAVEFORMATEX; the XMA packets themselves are stored as-is.
        ext = ".xma";
        uint32_t blocks = 1;
        f.u16(0x166);
        f.u16(uint16_t(w.channels));
        f.u32(w.sampleRate);
        f.u32(uint32_t(uint64_t(w.data.size()) * w.sampleRate / std::max<uint32_t>(1, w.durationSamples)));
        f.u16(uint16_t(2 * w.channels));
        f.u16(16);
        f.u16(34);
        f.u16(uint16_t((w.channels + 1) / 2)); // NumStreams
        f.u32(w.channels == 1 ? 0x4 : 0x3);    // ChannelMask
        f.u32(w.durationSamples);              // SamplesEncoded
        f.u32(uint32_t((w.data.size() + 2047) / 2048 * 2048)); // BytesPerBlock
        f.u32(0);
        f.u32(w.durationSamples);
        f.u32(w.loopStart);
        f.u32(w.loopLength);
        f.u8(w.loopLength ? 255 : 0);
        f.u8(4);
        f.u16(uint16_t(blocks));
        riff(f.buf(), w.data);
    } else if (w.formatTag == 2) {
        ext = ".wav";
        // Xbox ADPCM: 36 bytes per channel per 64-sample block.
        f.u16(0x69);
        f.u16(uint16_t(w.channels));
        f.u32(w.sampleRate);
        f.u32(w.sampleRate * 36 * w.channels / 64);
        f.u16(uint16_t(36 * w.channels));
        f.u16(4);
        f.u16(2);
        f.u16(64);
        riff(f.buf(), w.data);
    } else {
        ext = ".wma";
        return w.data;
    }
    return o.buf();
}

} // namespace nut::media
