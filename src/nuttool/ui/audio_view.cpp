#include "audio_view.hpp"

#include "ops.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace nut::gui {

std::string formatDuration(double seconds) {
    if (!(seconds > 0)) seconds = 0;
    int m = int(seconds / 60);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%04.1f", m, seconds - m * 60);
    return buf;
}

// ---- AudioPlayer ------------------------------------------------------------------------------

AudioPlayer::~AudioPlayer() { unload(); }

void AudioPlayer::unload() {
    if (active_ == this) active_ = nullptr;
    if (stream_) SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    pcm_.reset();
    playing_ = false;
    segs_.clear();
    pushed_ = retired_ = cursor_ = 0;
    appliedVolume_ = -1;
}

void AudioPlayer::load(std::shared_ptr<const audio::Pcm> pcm, uint32_t loopStart, uint32_t loopLength) {
    unload();
    error.clear();
    if (!pcm || pcm->frames() == 0) return;
    pcm_ = std::move(pcm);
    size_t n = pcm_->frames();
    loopStart_ = loopLength ? std::min<size_t>(loopStart, n) : 0;
    loopEnd_ = loopLength ? std::min<size_t>(size_t(loopStart) + loopLength, n) : n;
    if (loopEnd_ <= loopStart_) loopStart_ = 0, loopEnd_ = n;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        error = std::string("no audio output: ") + SDL_GetError();
        return;
    }
    SDL_AudioSpec spec{SDL_AUDIO_S16, int(pcm_->channels), int(pcm_->rate)};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr); // opens paused
    if (!stream_) error = std::string("no audio output: ") + SDL_GetError();
}

size_t AudioPlayer::queuedFrames() const {
    if (!stream_ || !pcm_) return 0;
    int q = SDL_GetAudioStreamQueued(stream_);
    return q > 0 ? size_t(q) / (2 * pcm_->channels) : 0;
}

void AudioPlayer::push(size_t frames) {
    const size_t ch = pcm_->channels;
    while (frames > 0) {
        size_t end = loop ? loopEnd_ : pcm_->frames();
        if (cursor_ >= end) {
            if (!loop) break;
            cursor_ = loopStart_;
        }
        size_t n = std::min(frames, end - cursor_);
        if (n == 0) break;
        SDL_PutAudioStreamData(stream_, pcm_->samples.data() + cursor_ * ch, int(n * ch * 2));
        if (!segs_.empty() && segs_.back().first + segs_.back().second == cursor_) segs_.back().second += n;
        else segs_.push_back({cursor_, n});
        cursor_ += n;
        pushed_ += n;
        frames -= n;
    }
}

void AudioPlayer::play() {
    if (!stream_) return;
    if (active_ && active_ != this) active_->pause();
    active_ = this;
    playing_ = true;
    SDL_ResumeAudioStreamDevice(stream_);
    update();
}

void AudioPlayer::pause() {
    if (stream_) SDL_PauseAudioStreamDevice(stream_);
    playing_ = false;
}

void AudioPlayer::stop() {
    pause();
    seek(0);
}

void AudioPlayer::seek(double seconds) {
    if (!stream_ || !pcm_) return;
    SDL_ClearAudioStream(stream_);
    segs_.clear();
    pushed_ = retired_ = 0;
    cursor_ = std::min(pcm_->frames(), size_t(std::max(0.0, seconds) * pcm_->rate));
    if (playing_) update();
}

void AudioPlayer::update() {
    if (!stream_ || !pcm_) return;
    if (volume != appliedVolume_) {
        SDL_SetAudioStreamGain(stream_, volume);
        appliedVolume_ = volume;
    }
    // Forget runs that have been played.
    size_t consumed = pushed_ - std::min(pushed_, queuedFrames());
    while (!segs_.empty() && consumed - retired_ >= segs_.front().second) {
        retired_ += segs_.front().second;
        segs_.pop_front();
    }
    if (!playing_) return;
    size_t ahead = pcm_->rate / 4; // keep a quarter of a second queued
    size_t q = queuedFrames();
    if (q < ahead) push(ahead - q + pcm_->rate / 10);
    if (queuedFrames() == 0) stop(); // reached the end
}

double AudioPlayer::position() const {
    if (!pcm_ || !pcm_->rate) return 0;
    size_t consumed = pushed_ - std::min(pushed_, queuedFrames());
    size_t off = consumed - std::min(consumed, retired_);
    for (auto& [first, n] : segs_) {
        if (off < n) return double(first + off) / pcm_->rate;
        off -= n;
    }
    return double(cursor_) / pcm_->rate;
}

// ---- WaveBankView -----------------------------------------------------------------------------

void WaveBankView::setBank(std::shared_ptr<const media::WaveBank> bank, const std::string& exportBase, std::shared_ptr<const Bytes> raw) {
    player_.unload();
    bank_ = std::move(bank);
    raw_ = std::move(raw);
    exportBase_ = exportBase;
    selected_ = -1;
    playWhenReady_ = false;
    decoded_.clear();
    recent_.clear();
    decodeError_.clear();
    peaks_.clear();
}

void WaveBankView::select(int i, bool andPlay) {
    if (i == selected_) {
        if (andPlay && player_.loaded()) player_.playing() ? player_.pause() : player_.play();
        else playWhenReady_ = andPlay;
        return;
    }
    player_.unload();
    selected_ = i;
    playWhenReady_ = andPlay;
    decodeError_.clear();
    peaks_.clear();
}

void WaveBankView::draw(App& app) {
    if (!bank_) return;
    const auto& waves = bank_->waves;

    // Decoding happens on a worker thread, one wave at a time, the selected one first.
    if (decoding_.valid() && decoding_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            decoded_[decodingIndex_] = decoding_.get();
            recent_.push_back(decodingIndex_);
            if (recent_.size() > 8) {
                if (recent_.front() != selected_) decoded_.erase(recent_.front());
                recent_.erase(recent_.begin());
            }
        } catch (std::exception& e) {
            if (decodingIndex_ == selected_) decodeError_ = e.what();
        }
        decodingIndex_ = -1;
    }
    if (selected_ >= 0 && !player_.loaded() && decodeError_.empty()) {
        auto it = decoded_.find(selected_);
        if (it != decoded_.end()) {
            const auto& w = waves[size_t(selected_)];
            player_.load(it->second, w.loopStart, w.loopLength);
            const audio::Pcm& p = *it->second;
            // Waveform: min/max of every channel over 1024 columns.
            constexpr size_t kCols = 1024;
            peaks_.assign(kCols * 2, 0.0f);
            size_t n = p.frames();
            for (size_t c = 0; c < kCols && n; ++c) {
                size_t a = c * n / kCols, b = std::max(a + 1, (c + 1) * n / kCols);
                int lo = 0, hi = 0;
                for (size_t f = a; f < b && f < n; ++f)
                    for (size_t k = 0; k < p.channels; ++k) {
                        int s = p.samples[f * p.channels + k];
                        lo = std::min(lo, s), hi = std::max(hi, s);
                    }
                peaks_[c * 2] = float(lo) / 32768.0f;
                peaks_[c * 2 + 1] = float(hi) / 32767.0f;
            }
            if (playWhenReady_) player_.play();
            playWhenReady_ = false;
        } else if (!decoding_.valid()) {
            decodingIndex_ = selected_;
            auto bank = bank_;
            int i = selected_;
            decoding_ = std::async(std::launch::async, [bank, i] { return std::shared_ptr<const audio::Pcm>(std::make_shared<audio::Pcm>(audio::decode(bank->waves[size_t(i)]))); });
        }
    }
    player_.update();

    double total = 0;
    for (auto& w : waves) total += w.sampleRate ? double(w.durationSamples) / w.sampleRate : 0;
    ImGui::Text("%s: %zu waves, %s in all", bank_->name.c_str(), waves.size(), formatDuration(total).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Export all as WAV...")) exportAll(app, true);
    ImGui::SameLine();
    if (ImGui::Button("Export bank + originals...")) exportAll(app, false);
    helpMarker("WAV: every wave decoded to 16-bit PCM. Originals: the .xwb bank and each wave as stored (.xma for XMA, which few players read).");
    if (!audio::haveDecoder()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "This build has no XMA decoder (FFmpeg): only PCM waves play.");

    if (selected_ >= 0 && size_t(selected_) < waves.size()) drawTransport(app);
    else ImGui::TextDisabled("Click a wave to play it.");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Filter");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(260));
    inputText("##wavefilter", filter_);
    std::string needle = lower(filter_);
    static const char* fmts[] = {"PCM", "XMA", "ADPCM", "WMA"};
    if (ImGui::BeginTable("##waves", 7, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, em(40));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, em(55));
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, em(30));
        ImGui::TableSetupColumn("Rate", ImGuiTableColumnFlags_WidthFixed, em(60));
        ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, em(60));
        ImGui::TableSetupColumn("Loop", ImGuiTableColumnFlags_WidthFixed, em(40));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < waves.size(); ++i) {
            const auto& w = waves[i];
            if (!needle.empty() && lower(w.name).find(needle) == std::string::npos) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(int(i));
            bool sel = int(i) == selected_;
            char num[16];
            std::snprintf(num, sizeof num, "%zu", i);
            if (ImGui::Selectable(num, sel, ImGuiSelectableFlags_SpanAllColumns)) select(int(i), true);
            if (sel && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
            ImGui::PopID();
            ImGui::TableNextColumn();
            if (sel && player_.playing()) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%s", w.name.c_str());
            else ImGui::TextUnformatted(w.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmts[w.formatTag & 3]);
            ImGui::TableNextColumn();
            ImGui::Text("%u", w.channels);
            ImGui::TableNextColumn();
            ImGui::Text("%u", w.sampleRate);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(formatDuration(w.sampleRate ? double(w.durationSamples) / w.sampleRate : 0).c_str());
            ImGui::TableNextColumn();
            if (w.loopLength) ImGui::TextUnformatted("yes");
        }
        ImGui::EndTable();
    }
}

void WaveBankView::drawWaveform(float height) {
    ImVec2 size(ImGui::GetContentRegionAvail().x, height);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##waveform", size);
    bool active = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg), fg = ImGui::GetColorU32(ImGuiCol_PlotLines), loopCol = IM_COL32(90, 140, 220, 50);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), bg);
    const auto& w = bank_->waves[size_t(selected_)];
    double dur = player_.duration();
    if (w.loopLength && w.sampleRate && dur > 0) {
        float a = float(double(w.loopStart) / w.sampleRate / dur), b = float(double(w.loopStart + w.loopLength) / w.sampleRate / dur);
        dl->AddRectFilled(ImVec2(p0.x + size.x * std::clamp(a, 0.0f, 1.0f), p0.y), ImVec2(p0.x + size.x * std::clamp(b, 0.0f, 1.0f), p0.y + size.y), loopCol);
    }
    size_t cols = peaks_.size() / 2;
    float mid = p0.y + size.y * 0.5f, half = size.y * 0.5f - 2;
    for (int x = 0; cols && x < int(size.x); ++x) {
        size_t c = std::min(cols - 1, size_t(double(x) / size.x * double(cols)));
        float lo = peaks_[c * 2], hi = peaks_[c * 2 + 1];
        dl->AddLine(ImVec2(p0.x + float(x) + 0.5f, mid - hi * half), ImVec2(p0.x + float(x) + 0.5f, mid - lo * half + 1), fg);
    }
    if (dur > 0) {
        float px = p0.x + size.x * float(std::clamp(player_.position() / dur, 0.0, 1.0));
        dl->AddLine(ImVec2(px, p0.y), ImVec2(px, p0.y + size.y), IM_COL32(255, 210, 80, 255), 2);
    }
    if (active && dur > 0) {
        float f = std::clamp((ImGui::GetIO().MousePos.x - p0.x) / size.x, 0.0f, 1.0f);
        player_.seek(f * dur);
    }
    ImGui::SetItemTooltip("Click or drag to seek. The shaded part is the wave's authored loop region.");
}

void WaveBankView::drawTransport(App& app) {
    const auto& w = bank_->waves[size_t(selected_)];
    ImGui::SeparatorText(w.name.c_str());
    if (!decodeError_.empty()) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Cannot decode: %s", decodeError_.c_str());
    } else if (!player_.loaded()) {
        ImGui::TextDisabled("Decoding...");
        ImGui::Dummy(ImVec2(0, em(70) - ImGui::GetTextLineHeightWithSpacing()));
    } else {
        drawWaveform(em(70));
        if (ImGui::Button(player_.playing() ? "Pause" : "Play", ImVec2(em(60), 0))) player_.playing() ? player_.pause() : player_.play();
        ImGui::SameLine();
        if (ImGui::Button("Stop")) player_.stop();
        ImGui::SameLine();
        ImGui::Text("%s / %s", formatDuration(player_.position()).c_str(), formatDuration(player_.duration()).c_str());
        ImGui::SameLine();
        ImGui::Checkbox("Loop", &player_.loop);
        ImGui::SetItemTooltip(w.loopLength ? "Repeats the wave's authored loop region (shaded), as the game does." : "Repeats the whole wave.");
        ImGui::SameLine();
        float pct = player_.volume * 100;
        ImGui::SetNextItemWidth(em(110));
        if (ImGui::SliderFloat("##volume", &pct, 0, 100, "volume %.0f%%")) player_.volume = pct / 100;
        if (!player_.error.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", player_.error.c_str());
    }
    ImGui::TextDisabled("%s, %u ch, %u Hz, %s stored%s", w.formatTag == 1 ? "XMA" : w.formatTag == 0 ? "PCM" : w.formatTag == 2 ? "ADPCM" : "WMA", w.channels,
                        w.sampleRate, formatSize(w.data.size()).c_str(), w.loopLength ? ", loops" : "");
    ImGui::SameLine();
    ImGui::BeginDisabled(!player_.loaded());
    if (ImGui::SmallButton("Export WAV...") && player_.pcm()) {
        Bytes f = audio::toWav(*player_.pcm());
        app.saveFileDialog("Export WAV", {{"WAV audio", "wav"}}, [&app, f](const fs::path& p) {
            writeFile(p, f);
            app.status("Exported " + toUtf8(p));
        }, fromUtf8(ops::safeName(w.name) + ".wav"));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Export original...")) {
        std::string ext;
        Bytes f = media::waveToFile(w, ext);
        app.saveFileDialog("Export wave", {{"Audio", ext.substr(1)}}, [&app, f](const fs::path& p) {
            writeFile(p, f);
            app.status("Exported " + toUtf8(p));
        }, fromUtf8(ops::safeName(w.name) + ext));
    }
    ImGui::Spacing();
}

void WaveBankView::exportAll(App& app, bool wav) {
    auto bank = bank_;
    auto raw = raw_;
    std::string base = ops::safeName(exportBase_.empty() ? bank->name : exportBase_);
    app.openFolderDialog(wav ? "Export the waves as WAV to" : "Export the bank and waves to", [&app, bank, raw, base, wav](const fs::path& dir) {
        app.runTask("Exporting " + base, false, [&app, bank, raw, base, wav, dir](Task& t) -> std::function<void()> {
            fs::path out = dir / base;
            fs::create_directories(out);
            if (!wav && raw) writeFile(dir / (base + ".xwb"), *raw);
            int written = 0, failed = 0;
            std::vector<std::string> problems;
            for (size_t i = 0; i < bank->waves.size(); ++i) {
                if (t.cancel) break;
                const auto& w = bank->waves[i];
                t.report(double(i) / double(bank->waves.size()), w.name);
                char prefix[16];
                std::snprintf(prefix, sizeof prefix, "%03zu_", i);
                std::string ext;
                Bytes f;
                if (wav && audio::canDecode(w)) {
                    try {
                        f = audio::toWav(audio::decode(w));
                        ext = ".wav";
                    } catch (std::exception& e) {
                        problems.push_back(w.name + ": " + e.what());
                        ++failed;
                    }
                }
                if (f.empty()) f = media::waveToFile(w, ext);
                writeFile(out / (prefix + ops::safeName(w.name) + ext), f);
                ++written;
            }
            return [&app, written, failed, out, problems] {
                for (auto& p : problems) app.log("WAV export: " + p);
                app.status("Exported " + std::to_string(written) + " wave(s) to " + toUtf8(out) +
                           (failed ? " (" + std::to_string(failed) + " could not be decoded, kept as stored)" : std::string()));
            };
        });
    });
}

} // namespace nut::gui
