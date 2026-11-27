// Plays Windows Media (WMV3 + WMA) movies: a decoder thread (Media Foundation) feeds frames to an
// overlay picture and sound to an SDL audio stream, paced by the audio clock.
#pragma once

#include "app.hpp"
#include "common.hpp"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

struct SDL_AudioStream;

namespace nut::gui {

class VideoPlayer {
public:
    VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;
    ~VideoPlayer();

    // Opens a movie held in memory; `error()` explains a failure.
    bool open(Bytes data);
    void close();
    bool isOpen() const { return open_; }
    const std::string& error() const { return error_; }

    void play();
    void pause();
    bool playing() const { return playing_; }
    void seek(double seconds);
    void setVolume(float v);
    void setLoop(bool loop) { loop_ = loop; }

    double duration() const { return duration_; }
    double position() const; // seconds
    int width() const { return width_; }
    int height() const { return height_; }
    bool hasAudio() const { return audio_ != nullptr; }

    // Uploads the frame due now; call once per UI frame. Returns the texture (null before the first frame).
    const GpuImage* update();

private:
    struct Frame {
        double time = 0;
        std::vector<uint8_t> rgba;
    };
    // Provided by the decoder (video_player_mf.cpp).
    bool openSource(); // creates impl_ from data_, sets the size, duration and audio format; error_ on failure
    void closeSource();
    void seekSource(double seconds);
    void worker();

    // Shared by the decoders (video_player.cpp).
    bool openAudio(); // after audioRate_/audioChannels_ are known
    bool bufferFull();
    void pushFrame(Frame f);
    void pushAudio(const uint8_t* pcm, size_t bytes, double time);
    void stopWorker();
    void startWorker();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    Bytes data_;
    bool open_ = false;
    std::string error_;
    int width_ = 0, height_ = 0;
    double duration_ = 0;

    std::thread thread_;
    std::atomic<bool> stop_{false}, playing_{false}, ended_{false}, loop_{false};
    std::mutex m_;
    std::deque<Frame> frames_;
    double startPos_ = 0;          // stream position where the clock starts
    double clockBase_ = 0;         // wall-clock fallback when there is no audio
    uint64_t clockStart_ = 0;
    std::atomic<int64_t> audioBytes_{0}; // bytes queued to the audio stream since the last seek
    int audioRate_ = 44100, audioChannels_ = 2;
    SDL_AudioStream* audio_ = nullptr;
    float volume_ = 1;
    std::unique_ptr<GpuImage> tex_;
    bool haveFrame_ = false;
    double shownTime_ = -1;
};

} // namespace nut::gui
