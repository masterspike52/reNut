// VideoPlayer decoder for Windows: Media Foundation's source reader.
#include "video_player.hpp"

#include <SDL3/SDL.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>

#include <chrono>
#include <map>

namespace nut::gui {
namespace {

template <class T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// Media Foundation is started once per process (and never shut down: other players may be open).
bool startMediaFoundation() {
    static bool ok = [] {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        (void)hr; // RPC_E_CHANGED_MODE is fine: COM is already initialised on this thread
        return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }();
    return ok;
}

} // namespace

struct VideoPlayer::Impl {
    IMFSourceReader* reader = nullptr;
    std::map<DWORD, bool> isVideo; // stream index -> video (false: audio)
    ~Impl() { release(reader); }
};

VideoPlayer::VideoPlayer() = default;
VideoPlayer::~VideoPlayer() { close(); }

void VideoPlayer::closeSource() { impl_.reset(); }

bool VideoPlayer::openSource() {
    if (!startMediaFoundation()) {
        error_ = "Media Foundation is not available";
        return false;
    }
    impl_ = std::make_unique<Impl>();
    IStream* mem = SHCreateMemStream(data_.data(), UINT(data_.size()));
    IMFByteStream* bs = nullptr;
    IMFAttributes* bsAttr = nullptr;
    IMFAttributes* attr = nullptr;
    IMFMediaType* type = nullptr;
    HRESULT hr = mem ? MFCreateMFByteStreamOnStream(mem, &bs) : E_OUTOFMEMORY;
    // XMV files have no telling extension: say what they are.
    if (SUCCEEDED(hr) && SUCCEEDED(bs->QueryInterface(IID_PPV_ARGS(&bsAttr)))) bsAttr->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"video/x-ms-asf");
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attr, 1);
    if (SUCCEEDED(hr)) attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromByteStream(bs, attr, &impl_->reader);
    release(attr);
    release(bsAttr);
    release(bs);
    release(mem);
    if (FAILED(hr)) {
        error_ = "Windows could not open this movie (error " + hex32(uint32_t(hr)) + ")";
        return false;
    }
    IMFSourceReader* r = impl_->reader;
    // Video as 32-bit BGRX frames.
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = r->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type);
    release(type);
    if (FAILED(hr)) {
        error_ = "no decodable video stream (error " + hex32(uint32_t(hr)) + ")";
        return false;
    }
    if (SUCCEEDED(r->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type))) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
        width_ = int(w);
        height_ = int(h);
        release(type);
    }
    // Audio as 16-bit PCM; a movie without decodable sound still plays.
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    hr = r->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, type);
    release(type);
    if (SUCCEEDED(hr) && SUCCEEDED(r->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &type))) {
        audioRate_ = int(MFGetAttributeUINT32(type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100));
        audioChannels_ = int(MFGetAttributeUINT32(type, MF_MT_AUDIO_NUM_CHANNELS, 2));
        release(type);
        openAudio();
    }
    if (!audio_) r->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), FALSE);
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(r->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var))) duration_ = double(var.uhVal.QuadPart) / 1e7;
    PropVariantClear(&var);
    return true;
}

void VideoPlayer::seekSource(double seconds) {
    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = LONGLONG(seconds * 1e7);
    impl_->reader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);
}

void VideoPlayer::worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMFSourceReader* r = impl_->reader;
    while (!stop_) {
        if (bufferFull()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;
        HRESULT hr = r->ReadSample(DWORD(MF_SOURCE_READER_ANY_STREAM), 0, &stream, &flags, &ts, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
            ended_ = true;
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            // With both streams selected, the reader reports each end; stop after the video.
            IMFMediaType* t = nullptr;
            GUID major{};
            if (SUCCEEDED(r->GetCurrentMediaType(stream, &t))) { t->GetGUID(MF_MT_MAJOR_TYPE, &major); t->Release(); }
            if (major == MFMediaType_Video) {
                ended_ = true;
                release(sample);
                break;
            }
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            IMFMediaType* t = nullptr;
            if (SUCCEEDED(r->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &t))) {
                UINT32 w = 0, h = 0;
                MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
                width_ = int(w);
                height_ = int(h);
                t->Release();
            }
        }
        if (!sample) continue;
        auto kind = impl_->isVideo.find(stream);
        if (kind == impl_->isVideo.end()) {
            IMFMediaType* t = nullptr;
            GUID major{};
            if (SUCCEEDED(r->GetCurrentMediaType(stream, &t))) { t->GetGUID(MF_MT_MAJOR_TYPE, &major); t->Release(); }
            kind = impl_->isVideo.emplace(stream, major == MFMediaType_Video).first;
        }
        double time = double(ts) / 1e7;
        IMFMediaBuffer* buf = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
            if (kind->second) {
                // BGRX rows (pitch may be padded or negative) -> RGBA.
                Frame f;
                f.time = time;
                f.rgba.resize(size_t(width_) * height_ * 4);
                IMF2DBuffer* b2 = nullptr;
                BYTE* base = nullptr;
                LONG pitch = 0;
                DWORD len = 0;
                bool locked2d = SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&base, &pitch));
                if (!locked2d && SUCCEEDED(buf->Lock(&base, nullptr, &len))) pitch = width_ * 4;
                if (base) {
                    for (int y = 0; y < height_; ++y) {
                        const uint8_t* src = base + ptrdiff_t(y) * pitch;
                        uint8_t* dst = &f.rgba[size_t(y) * width_ * 4];
                        for (int x = 0; x < width_; ++x) {
                            dst[4 * x] = src[4 * x + 2];
                            dst[4 * x + 1] = src[4 * x + 1];
                            dst[4 * x + 2] = src[4 * x];
                            dst[4 * x + 3] = 255;
                        }
                    }
                    if (locked2d) b2->Unlock2D();
                    else buf->Unlock();
                    pushFrame(std::move(f));
                }
                release(b2);
            } else if (audio_) {
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                    pushAudio(p, len, time);
                    buf->Unlock();
                }
            }
            buf->Release();
        }
        sample->Release();
    }
    CoUninitialize();
}

} // namespace nut::gui
