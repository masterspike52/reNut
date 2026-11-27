// Audio tab: one wave bank from the catalog, with its player.
#include "audio_view.hpp"
#include "docs.hpp"

#include "streambundle.hpp"

namespace nut::gui {
namespace {

class AudioDocument : public Document {
public:
    AudioDocument(const AudioBankInfo& info, const fs::path& root, const std::string& filter) : info_(info) {
        const AssetLoc& loc = info.locs.front();
        path = loc.file;
        where_ = toUtf8Generic(root.empty() ? loc.file : fs::relative(loc.file, root)) + ", entry " + std::to_string(loc.entry);
        // Stream bundles are read entry by entry (the one holding the audio is close to 700 MB).
        Bytes data;
        std::vector<streambundle::IndexEntry> index;
        if (streambundle::readIndex(loc.file, index)) {
            if (loc.entry < 0 || size_t(loc.entry) >= index.size()) fail("bad stream bundle entry");
            data = streambundle::readEntry(loc.file, index[size_t(loc.entry)]);
        } else {
            Loaded l = loadAsset(loc.file);
            if (!l.sb || loc.entry < 0 || size_t(loc.entry) >= l.sb->entries.size()) fail("bad stream bundle entry");
            data = std::move(l.sb->entries[size_t(loc.entry)].data);
        }
        auto raw = std::make_shared<const Bytes>(std::move(data));
        view_.setBank(std::make_shared<const media::WaveBank>(media::parseWaveBank(*raw)), info.name, raw);
        view_.setFilter(filter);
    }

    std::string title() const override { return "audio " + info_.name; }

    void draw(App& app) override {
        ImGui::TextDisabled("%s", where_.c_str());
        if (info_.locs.size() > 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("(+%zu more copies)", info_.locs.size() - 1);
        }
        view_.draw(app);
    }

private:
    AudioBankInfo info_;
    std::string where_;
    WaveBankView view_;
};

// A loose wave bank file (.xwb), opened from disk.
class WaveBankFileDocument : public Document {
public:
    WaveBankFileDocument(const fs::path& p, Bytes data) {
        path = p;
        title_ = toUtf8(p.filename());
        auto raw = std::make_shared<const Bytes>(std::move(data));
        view_.setBank(std::make_shared<const media::WaveBank>(media::parseWaveBank(*raw)), toUtf8(p.stem()), raw);
    }

    std::string title() const override { return title_; }

    void draw(App& app) override {
        ImGui::TextDisabled("%s", toUtf8(path).c_str());
        view_.draw(app);
    }

private:
    std::string title_;
    WaveBankView view_;
};

} // namespace

std::unique_ptr<Document> makeWaveBankDocument(const fs::path& p, Bytes data) {
    return std::make_unique<WaveBankFileDocument>(p, std::move(data));
}

std::unique_ptr<Document> makeAudioDocument(const AudioBankInfo& info, const fs::path& root, const std::string& filter) {
    return std::make_unique<AudioDocument>(info, root, filter);
}

} // namespace nut::gui
