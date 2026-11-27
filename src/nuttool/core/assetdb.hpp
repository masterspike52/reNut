// Asset identifiers and on-disk layout.
//
// Every asset is named "aid_<type>_<rest>". Its 32-bit id is
//   (typeIndex << 24) | (~crc32(rest) & 0xFFFFFF)
// where typeIndex is the position of <type> in the game's type table.
// Files live at:
//   Bundle\<tt>\<hhhhhh>          for bundle (0x4f) and streambundle (0x50)
//   Debug\<tt>\<hh>\<hh>\<hh>     for loose assets (loctext, xmv, ...)
//   loctext\<language>\<id>       for localised loctext
#pragma once

#include "common.hpp"

#include <map>
#include <optional>

namespace nut::assetdb {

constexpr uint8_t kTypeLoctext = 0x11;
constexpr uint8_t kTypeXmv = 0x36;
constexpr uint8_t kTypeBundle = 0x4F;
constexpr uint8_t kTypeStreamBundle = 0x50;
constexpr uint8_t kTypeTexture = 0x01;

// Type name for an index, or "type_xx" when unknown.
std::string typeName(uint8_t type);
std::optional<uint8_t> typeIndex(std::string_view name);
// Content version the game expects for a type (from the type table), 0 if none.
float typeVersion(uint8_t type);
size_t typeCount();

// Accepts "aid_texture_foo", "aid_texture_foo,123,4.05" or "texture_foo".
std::optional<uint32_t> idFromName(std::string_view name);
// Strips the ",timestamp,version" suffix and any "D:\...\" path wrapper used by texture symbols.
std::string baseAssetName(std::string_view symbolName);

struct IndexEntry {
    std::string name;
    uint32_t id = 0;
    uint32_t timestamp = 0;
    std::string version;
};

std::vector<IndexEntry> readDbIndex(const fs::path& file);

// Known-name registry used to label ids. Populated from db_index and from
// symbol names seen inside containers.
class Names {
public:
    void add(std::string_view name);
    std::string lookup(uint32_t id) const; // empty when unknown
    size_t size() const { return byId_.size(); }

private:
    std::map<uint32_t, std::string> byId_;
};

fs::path pathForId(const fs::path& assetsRoot, uint32_t id);
// Like pathForId, but also resolves the font cache assets, which are stored by name:
// aid_xuifontcachefont_banjox_default_<lang> -> xuifontcachefont\<lang>.sbp and
// aid_xuifontcachemeta_banjox_<default|precache>_<lang> -> xuifontcachemeta\<metafont|precache>_<lang>.ini
fs::path pathForEntry(const fs::path& assetsRoot, const IndexEntry& e);
// Reverse of pathForId for files under Bundle\ or Debug\ (and loctext\<lang>\<id>).
std::optional<uint32_t> idFromPath(const fs::path& p);

} // namespace nut::assetdb
