#include "audio.hpp"

#include <algorithm>
#include <cstring>

#ifndef NUTTOOL_FFMPEG
#define NUTTOOL_FFMPEG 0
#endif

#if NUTTOOL_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#endif

namespace nut::audio {
namespace {

// Big-endian PCM (8-bit unsigned or 16-bit signed) to interleaved 16-bit.
Pcm decodePcm(const media::Wave& w) {
    Pcm p;
    p.rate = w.sampleRate;
    p.channels = std::max<uint32_t>(1, w.channels);
    if (w.bitsPerSample == 16) {
        p.samples.resize(w.data.size() / 2);
        for (size_t i = 0; i < p.samples.size(); ++i) p.samples[i] = int16_t(be16(&w.data[i * 2]));
    } else {
        p.samples.resize(w.data.size());
        for (size_t i = 0; i < p.samples.size(); ++i) p.samples[i] = int16_t((int(w.data[i]) - 128) << 8);
    }
    p.samples.resize(p.samples.size() / p.channels * p.channels);
    return p;
}

#if NUTTOOL_FFMPEG

struct Memory {
    const Bytes* data;
    size_t pos = 0;
};

int readMemory(void* opaque, uint8_t* buf, int size) {
    auto* m = static_cast<Memory*>(opaque);
    size_t left = m->data->size() - m->pos;
    if (left == 0) return AVERROR_EOF;
    size_t n = std::min(left, size_t(size));
    std::memcpy(buf, m->data->data() + m->pos, n);
    m->pos += n;
    return int(n);
}

int64_t seekMemory(void* opaque, int64_t offset, int whence) {
    auto* m = static_cast<Memory*>(opaque);
    if (whence & AVSEEK_SIZE) return int64_t(m->data->size());
    whence &= ~AVSEEK_FORCE;
    int64_t base = whence == SEEK_CUR ? int64_t(m->pos) : whence == SEEK_END ? int64_t(m->data->size()) : 0;
    int64_t p = base + offset;
    if (p < 0 || p > int64_t(m->data->size())) return -1;
    m->pos = size_t(p);
    return p;
}

std::string avError(int e) {
    char buf[128];
    av_strerror(e, buf, sizeof buf);
    return buf;
}

// Decodes a RIFF file (XMA2, ADPCM...) held in memory.
Pcm decodeRiff(const Bytes& file) {
    av_log_set_level(AV_LOG_ERROR); // not FFmpeg's notes about estimated durations and skipped samples
    Memory mem{&file};
    struct State {
        AVFormatContext* fmt = nullptr;
        AVIOContext* io = nullptr;
        AVCodecContext* codec = nullptr;
        SwrContext* swr = nullptr;
        AVPacket* pkt = nullptr;
        AVFrame* frame = nullptr;
        ~State() {
            av_frame_free(&frame);
            av_packet_free(&pkt);
            swr_free(&swr);
            avcodec_free_context(&codec);
            avformat_close_input(&fmt);
            if (io) av_freep(&io->buffer);
            avio_context_free(&io);
        }
    } s;
    constexpr int kIo = 32768;
    auto* buf = static_cast<uint8_t*>(av_malloc(kIo));
    if (!buf) fail("out of memory");
    s.io = avio_alloc_context(buf, kIo, 0, &mem, readMemory, nullptr, seekMemory);
    if (!s.io) {
        av_free(buf);
        fail("out of memory");
    }
    s.fmt = avformat_alloc_context();
    s.fmt->pb = s.io;
    s.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    int e = avformat_open_input(&s.fmt, nullptr, av_find_input_format("wav"), nullptr);
    if (e < 0) fail("cannot read the audio: " + avError(e));
    avformat_find_stream_info(s.fmt, nullptr);
    const AVCodec* dec = nullptr;
    int stream = av_find_best_stream(s.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (stream < 0 || !dec) fail("no decoder for this audio format");
    s.codec = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(s.codec, s.fmt->streams[stream]->codecpar);
    if ((e = avcodec_open2(s.codec, dec, nullptr)) < 0) fail("cannot open the decoder: " + avError(e));

    Pcm p;
    p.rate = uint32_t(s.codec->sample_rate);
    p.channels = uint32_t(std::max(1, s.codec->ch_layout.nb_channels));
    if (swr_alloc_set_opts2(&s.swr, &s.codec->ch_layout, AV_SAMPLE_FMT_S16, s.codec->sample_rate, &s.codec->ch_layout, s.codec->sample_fmt,
                            s.codec->sample_rate, 0, nullptr) < 0 ||
        swr_init(s.swr) < 0)
        fail("cannot convert the decoded samples");
    s.pkt = av_packet_alloc();
    s.frame = av_frame_alloc();
    auto drain = [&] {
        while (avcodec_receive_frame(s.codec, s.frame) == 0) {
            int out = swr_get_out_samples(s.swr, s.frame->nb_samples);
            size_t at = p.samples.size();
            p.samples.resize(at + size_t(out) * p.channels);
            uint8_t* dst = reinterpret_cast<uint8_t*>(p.samples.data() + at);
            int got = swr_convert(s.swr, &dst, out, const_cast<const uint8_t**>(s.frame->extended_data), s.frame->nb_samples);
            p.samples.resize(at + size_t(std::max(got, 0)) * p.channels);
            av_frame_unref(s.frame);
        }
    };
    while (av_read_frame(s.fmt, s.pkt) >= 0) {
        if (s.pkt->stream_index == stream && avcodec_send_packet(s.codec, s.pkt) >= 0) drain();
        av_packet_unref(s.pkt);
    }
    avcodec_send_packet(s.codec, nullptr);
    drain();
    if (p.samples.empty()) fail("the decoder produced no audio");
    return p;
}

#endif

} // namespace

bool haveDecoder() { return NUTTOOL_FFMPEG != 0; }

bool canDecode(const media::Wave& w) { return w.formatTag == 0 || haveDecoder(); }

Pcm decode(const media::Wave& w) {
    if (w.formatTag == 0) return decodePcm(w);
#if NUTTOOL_FFMPEG
    std::string ext;
    Bytes file = media::waveToFile(w, ext);
    if (ext != ".wav" && ext != ".xma") fail("WMA waves are not decoded");
    Pcm p = decodeRiff(file);
    // XMA decodes whole 512-sample frames: trim to the wave's length.
    if (w.durationSamples && p.frames() > w.durationSamples) p.samples.resize(size_t(w.durationSamples) * p.channels);
    return p;
#else
    fail("this build has no XMA/ADPCM decoder (FFmpeg)");
#endif
}

Bytes toWav(const Pcm& p) {
    Writer o(false);
    uint32_t bytes = uint32_t(p.samples.size() * 2), ba = p.channels * 2;
    o.bytes("RIFF", 4);
    o.u32(36 + bytes);
    o.bytes("WAVE", 4);
    o.bytes("fmt ", 4);
    o.u32(16);
    o.u16(1);
    o.u16(uint16_t(p.channels));
    o.u32(p.rate);
    o.u32(p.rate * ba);
    o.u16(uint16_t(ba));
    o.u16(16);
    o.bytes("data", 4);
    o.u32(bytes);
    for (int16_t s : p.samples) o.u16(uint16_t(s));
    return o.buf();
}

} // namespace nut::audio
