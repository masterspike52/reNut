#include "video_player.hpp"

#include <SDL3/SDL.h>

#include "renut_engine/gpu/overlay/overlay_ui.h"

#include <algorithm>

namespace nut::gui {

bool VideoPlayer::open(Bytes data) {
    close();
    error_.clear();
    data_ = std::move(data);
    if (!openSource()) {
        if (audio_) SDL_DestroyAudioStream(audio_);
        audio_ = nullptr;
        closeSource();
        return false;
    }
    open_ = true;
    seek(0);
    return true;
}

void VideoPlayer::close() {
    stopWorker();
    if (audio_) SDL_DestroyAudioStream(audio_);
    audio_ = nullptr;
    closeSource();
    frames_.clear();
    open_ = false;
    playing_ = false;
    haveFrame_ = false;
}

bool VideoPlayer::openAudio() {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    SDL_AudioSpec spec{SDL_AUDIO_S16, audioChannels_, audioRate_};
    audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (audio_) SDL_SetAudioStreamGain(audio_, volume_);
    return audio_ != nullptr;
}

void VideoPlayer::stopWorker() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    stop_ = false;
}

void VideoPlayer::startWorker() {
    stop_ = false;
    thread_ = std::thread([this] { worker(); });
}

bool VideoPlayer::bufferFull() {
    // Stay a little ahead of playback: a few frames and up to a second of sound.
    size_t queued;
    {
        std::lock_guard<std::mutex> lk(m_);
        queued = frames_.size();
    }
    bool audioFull = !audio_ || SDL_GetAudioStreamQueued(audio_) > audioRate_ * audioChannels_ * 2;
    return queued >= 8 && audioFull;
}

void VideoPlayer::pushFrame(Frame f) {
    if (f.time + 0.05 < startPos_) return;
    std::lock_guard<std::mutex> lk(m_);
    frames_.push_back(std::move(f));
}

void VideoPlayer::pushAudio(const uint8_t* pcm, size_t bytes, double time) {
    if (!audio_) return;
    // After a seek, skip sound from before the target (decoding restarts at a key frame).
    const double bytesPerSecond = double(audioRate_) * audioChannels_ * 2;
    double skip = startPos_ - time;
    size_t offset = 0;
    if (skip > 0) offset = std::min<size_t>(bytes, size_t(skip * bytesPerSecond) & ~size_t(3));
    if (offset < bytes) {
        SDL_PutAudioStreamData(audio_, pcm + offset, int(bytes - offset));
        audioBytes_ += int64_t(bytes - offset);
    }
}

void VideoPlayer::play() {
    if (!open_) return;
    bool restart;
    {
        std::lock_guard<std::mutex> lk(m_);
        restart = ended_ && frames_.empty();
    }
    if (restart) seek(0);
    clockBase_ = position();
    clockStart_ = SDL_GetTicksNS();
    playing_ = true;
    if (audio_) SDL_ResumeAudioStreamDevice(audio_);
}

void VideoPlayer::pause() {
    if (!open_) return;
    clockBase_ = position();
    playing_ = false;
    if (audio_) SDL_PauseAudioStreamDevice(audio_);
}

void VideoPlayer::seek(double seconds) {
    if (!impl_) return;
    stopWorker();
    seconds = std::clamp(seconds, 0.0, std::max(0.0, duration_ - 0.05));
    seekSource(seconds);
    {
        std::lock_guard<std::mutex> lk(m_);
        frames_.clear();
    }
    if (audio_) SDL_ClearAudioStream(audio_);
    audioBytes_ = 0;
    startPos_ = clockBase_ = seconds;
    clockStart_ = SDL_GetTicksNS();
    ended_ = false;
    haveFrame_ = false;
    startWorker();
}

void VideoPlayer::setVolume(float v) {
    volume_ = v;
    if (audio_) SDL_SetAudioStreamGain(audio_, v);
}

double VideoPlayer::position() const {
    if (!open_) return 0;
    if (audio_) {
        // Sound actually handed to the device so far, after the last seek.
        int64_t played = audioBytes_ - SDL_GetAudioStreamQueued(audio_);
        double t = startPos_ + double(std::max<int64_t>(0, played)) / (double(audioRate_) * audioChannels_ * 2);
        return std::min(t, duration_);
    }
    if (!playing_) return clockBase_;
    return std::min(duration_, clockBase_ + double(SDL_GetTicksNS() - clockStart_) / 1e9);
}

const GpuImage* VideoPlayer::update() {
    if (!open_) return nullptr;
    double pos = position();
    Frame show;
    bool got = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        // Drop frames that are already late; show the newest one that is due.
        while (frames_.size() > 1 && frames_[1].time <= pos) frames_.pop_front();
        if (!frames_.empty() && (frames_.front().time <= pos + 0.01 || !haveFrame_)) {
            show = std::move(frames_.front());
            frames_.pop_front();
            got = true;
        }
    }
    if (got && !show.rgba.empty()) {
        // A new picture each frame: frames already recorded keep the old one until they're replaced.
        tex_ = std::make_unique<GpuImage>();
        tex_->tex = ImTextureID(renut::gpu::overlay::AddImage(uint32_t(width_), uint32_t(height_), std::move(show.rgba)));
        tex_->width = width_;
        tex_->height = height_;
        haveFrame_ = true;
        shownTime_ = show.time;
    }
    if (ended_ && playing_) {
        bool drained;
        {
            std::lock_guard<std::mutex> lk(m_);
            drained = frames_.empty();
        }
        if (drained && (!audio_ || SDL_GetAudioStreamQueued(audio_) == 0)) {
            if (loop_) {
                seek(0);
                play();
            } else {
                pause();
            }
        }
    }
    return haveFrame_ ? tex_.get() : nullptr;
}

} // namespace nut::gui
