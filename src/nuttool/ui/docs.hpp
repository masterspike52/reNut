// Document types of nuttool-gui. Constructors run on worker threads (parsing only);
// all rendering resources are created lazily in draw().
#pragma once

#include "app.hpp"
#include "catalog.hpp"
#include "container.hpp"
#include "texture.hpp"

namespace nut::gui {

std::unique_ptr<Document> makeCaffDocument(const fs::path& p, const fs::path& root, Loaded l);
std::unique_ptr<Document> makeStreamBundleDocument(const fs::path& p, const fs::path& root, Loaded l);
std::unique_ptr<Document> makeTextDocument(const fs::path& p, const fs::path& root, Loaded l);
std::unique_ptr<Document> makeFontDocument(const fs::path& p, Bytes data);
std::unique_ptr<Document> makeVideoDocument(const fs::path& p, Bytes data);
std::unique_ptr<Document> makeIniDocument(const fs::path& p, Bytes data);
std::unique_ptr<Document> makeImageDocument(const fs::path& p, Bytes data);
std::unique_ptr<Document> makeIndexDocument(const fs::path& p, const fs::path& root);
std::unique_ptr<Document> makeHexDocument(const fs::path& p, Bytes data, const std::string& kind);
std::unique_ptr<Document> makeTextureDocument(const TextureInfo& t, const fs::path& root);
std::unique_ptr<Document> makeModelDocument(const ModelInfo& m, std::shared_ptr<const Catalog> catalog, const fs::path& root);
std::unique_ptr<Document> makeAudioDocument(const AudioBankInfo& info, const fs::path& root, const std::string& filter);
std::unique_ptr<Document> makeWaveBankDocument(const fs::path& p, Bytes data);
std::unique_ptr<Document> makeScriptDocument(const ScriptInfo& s, std::shared_ptr<const Catalog> catalog, const fs::path& root);

// Text editor for a game script or objparams; the text is rebuilt whenever it changes, to report mistakes.
struct ScriptEditor {
    using Build = std::function<Bytes(const std::string&)>;
    Build build;
    std::string text, saved;
    std::string problem; // why the text does not build; empty when it does
    size_t bytes = 0;    // size of the built data
    void load(std::string initial, Build b);
    // Text and builder for an asset's data: a game script by its kind, or an objparams record. Throws when it is neither.
    void loadAsset(const std::string& assetName, const Bytes& data, const gamescript::NameLookup& names);
    void check();
    bool draw(const char* id, const ImVec2& size); // true when the text changed
    bool modified() const { return text != saved; }
};

// Asset names to click on: a click opens the asset when the catalog knows it.
void assetList(App& app, const char* id, const std::vector<std::string>& names, float height);

// "banjox_frontend"-style label from db_index for an asset file (thread-safe, cached per root).
std::string friendlyName(const fs::path& root, const fs::path& p);

// Shared texture preview widget (used by bundle and stream bundle documents).
struct TexturePreview {
    int level = 0, slice = 0, channel = 0; // channel: 0 RGBA, 1 RGB, 2 alpha
    float zoom = 1.0f;
    bool fit = true;
    bool dirty = true;
    std::shared_ptr<GpuImage> image;
    void draw(App& app, const texture::Header& h, const std::vector<texture::Level>& levels, float height);
};

} // namespace nut::gui
