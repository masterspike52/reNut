// Game-wide index of textures, models, animations, UI icons and audio, built by scanning every bundle and
// stream bundle once (cached in the settings folder, keyed by file sizes and times).
#pragma once

#include "caff.hpp"
#include "common.hpp"
#include "gamescript.hpp"
#include "ops.hpp"
#include "streambundle.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace nut::gui {

struct AssetLoc {
    fs::path file;
    int entry = -1;       // stream bundle entry, -1 for a bundle
    uint32_t symbol = 0;  // texture header / model .data symbol inside the CAFF
};

struct TextureInfo {
    std::string name;                // merged name (split "mip"/"top" suffix removed)
    std::vector<AssetLoc> full;      // complete textures, or mip halves of split ones
    std::vector<AssetLoc> tops;      // streamed level 0 of split textures
    uint32_t width = 0, height = 0, mips = 0;
    std::string format;
    bool ui = false;
};

struct ModelInfo {
    std::string name;
    std::vector<AssetLoc> locs;
};

// An XACT wave bank in a stream bundle (AssetLoc::symbol unused) and its waves' details.
struct AudioWaveInfo {
    std::string name;
    uint8_t format = 0, channels = 0; // format: media::Wave::formatTag
    uint32_t rate = 0, samples = 0;
    bool loops = false;
    double seconds() const { return rate ? double(samples) / rate : 0; }
};

struct AudioBankInfo {
    std::string name;
    std::vector<AssetLoc> locs;
    std::vector<AudioWaveInfo> waves;
    double seconds() const;
};

// A scene script, actor goal list, script table or state table, and the assets that point at it.
struct ScriptInfo {
    std::string name;
    gamescript::Kind kind = gamescript::Kind::Script;
    bool objParams = false;          // a vehicle part's objparams instead of a script
    std::vector<AssetLoc> locs;      // its .data symbol in each copy
    std::vector<std::string> usedBy; // assets whose data holds its id, sorted
};

struct Catalog {
    std::vector<TextureInfo> textures; // sorted by name
    std::vector<ModelInfo> models;
    std::map<std::string, size_t> textureIndex;
    std::map<std::string, size_t> modelIndex;
    std::vector<ModelInfo> anims; // animations (name and locations, like models)
    std::map<std::string, size_t> animIndex;
    std::vector<AudioBankInfo> audio; // wave banks, sorted by name
    std::map<std::string, size_t> audioIndex;
    std::vector<fs::path> menuBundles; // bundles holding menu packages (aid_xuipackage_*)
    std::vector<ScriptInfo> scripts;   // sorted by name
    std::map<std::string, size_t> scriptIndex;
    std::vector<ScriptInfo> parts;     // vehicle part objparams, sorted by name
    std::map<std::string, size_t> partIndex;
    std::map<uint32_t, std::string> names; // every asset name seen in the bundles and db_index, by id

    const TextureInfo* texture(const std::string& name) const;
    const ModelInfo* model(const std::string& name) const;
    const ModelInfo* anim(const std::string& name) const;
    const AudioBankInfo* audioBank(const std::string& name) const;
    const ScriptInfo* script(const std::string& name) const;
    const ScriptInfo* part(const std::string& name) const;
    std::string nameOf(uint32_t id) const; // "" when unknown

    // Scans the bundles of `root`, and those of the title update when `updateDir` is given.
    static std::shared_ptr<Catalog> build(const fs::path& root, const fs::path& cacheFile, const ops::Progress& progress, const fs::path& updateDir = {});
    // `base` with `over`'s assets in place of the same-named ones and added to it (a mod's over the game's).
    static std::shared_ptr<Catalog> overlay(const Catalog& base, const Catalog& over);
};

// Loads CAFFs from bundles / stream bundle entries, keeping recently used files decompressed.
class CaffCache {
public:
    // Returns the CAFF holding `loc` (the bundle itself, or the stream bundle entry).
    caff::Caff& get(const AssetLoc& loc);

private:
    struct Item {
        std::unique_ptr<caff::Caff> bundle;                         // for bundles
        std::map<int, std::unique_ptr<caff::Caff>> entries;          // for stream bundles
        std::unique_ptr<streambundle::StreamBundle> sb;
        std::vector<streambundle::IndexEntry> index; // stream bundles read entry by entry
        bool indexed = false;
    };
    std::map<fs::path, Item> items_;
    std::vector<fs::path> order_;
};

// Decodes a catalog texture (level 0 merged from the stream bundle when split).
ops::DecodedTexture decodeCatalogTexture(const TextureInfo& t, CaffCache& cache);

} // namespace nut::gui
