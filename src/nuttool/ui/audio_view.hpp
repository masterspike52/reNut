// Audio playback (SDL3) and the wave bank widget shared by the Audio tab and stream bundle tabs.
#pragma once

#include "app.hpp"
#include "audio.hpp"
#include "media.hpp"

#include <deque>
#include <future>

namespace nut::gui {

// Plays decoded PCM through an SDL audio stream, fed a little at a time so it can loop and seek.
// Only one player sounds at a time: starting one pauses the others.
class AudioPlayer {
public:
    AudioPlayer() = default;
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;
    ~AudioPlayer();

    // Loop region in frames (length 0: the whole sound).
    void load(std::shared_ptr<const audio::Pcm> pcm, uint32_t loopStart = 0, uint32_t loopLength = 0);
    void unload();
    bool loaded() const { return pcm_ != nullptr; }
    const audio::Pcm* pcm() const { return pcm_.get(); }

    void play();
    void pause();
    void stop(); // pause and go back to the start
    void seek(double seconds);
    void update(); // every frame: keeps the stream fed, notices the end

    bool playing() const { return playing_; }
    double position() const; // seconds
    double duration() const { return pcm_ ? pcm_->seconds() : 0; }
    bool loop = false;
    float volume = 0.8f;
    std::string error;

private:
    void push(size_t frames);
    size_t queuedFrames() const;

    std::shared_ptr<const audio::Pcm> pcm_;
    SDL_AudioStream* stream_ = nullptr;
    bool playing_ = false;
    size_t cursor_ = 0;                          // next frame to queue
    size_t loopStart_ = 0, loopEnd_ = 0;
    std::deque<std::pair<size_t, size_t>> segs_; // queued runs (first frame, frames), oldest first
    size_t pushed_ = 0, retired_ = 0;            // frames queued in total / in runs already played
    float appliedVolume_ = -1;
    static inline AudioPlayer* active_ = nullptr;
};

// Wave list, player (waveform, transport) and export of one wave bank.
class WaveBankView {
public:
    // `raw` (the bank file, optional) is written along with the originals.
    void setBank(std::shared_ptr<const media::WaveBank> bank, const std::string& exportBase, std::shared_ptr<const Bytes> raw = {});
    void setFilter(const std::string& f) { filter_ = f; }
    void draw(App& app);

private:
    void select(int i, bool andPlay);
    void drawTransport(App& app);
    void drawWaveform(float height);
    void exportAll(App& app, bool wav);

    std::shared_ptr<const media::WaveBank> bank_;
    std::shared_ptr<const Bytes> raw_;
    std::string exportBase_;
    std::string filter_;
    int selected_ = -1;
    bool playWhenReady_ = false;
    int decodingIndex_ = -1;
    std::future<std::shared_ptr<const audio::Pcm>> decoding_;
    std::map<int, std::shared_ptr<const audio::Pcm>> decoded_; // a few recent ones
    std::vector<int> recent_;
    std::string decodeError_;
    std::vector<float> peaks_; // min/max pairs per waveform column, for the selected wave
    AudioPlayer player_;
};

std::string formatDuration(double seconds); // "1:05.3"

} // namespace nut::gui
