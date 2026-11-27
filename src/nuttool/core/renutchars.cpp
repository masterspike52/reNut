#include "renutchars.hpp"

#include "assetdb.hpp"
#include "caff.hpp"
#include "container.hpp"
#include "model.hpp"
#include "renutworld.hpp"
#include "streambundle.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <set>

namespace nut::renutchars {
namespace {

constexpr uint32_t kParamsModel = 0xC0;  // an objparams' model
constexpr uint8_t kTypeModel = 0x04, kTypeObjparams = 0x1F;
const std::string kBundlePrefix = "aid_bundle_banjox_", kActorPrefix = "aid_bundle_banjox_actor_";
const std::string kParamsPrefix = "aid_objparams_banjox_actor_";
constexpr size_t kOpenBundles = 8;  // parsed bundles kept at once

struct Bundle {
    std::string name;  // "aid_bundle_banjox_..."
    fs::path file, stream;
    size_t size = 0;  // unpacked
};

// Every asset of the game's bundles: its name and the bundles holding it.
struct Index {
    std::vector<Bundle> bundles;
    std::map<std::string, size_t> byName;
    std::map<uint32_t, std::string> names;
    std::map<uint32_t, std::vector<size_t>> holders;
};

bool hasSection(const caff::Caff& c, size_t i) {
    const uint8_t s = c.symbols[i].section;
    return s && s <= c.sections.size();
}

Index readIndex(const fs::path& assets, const Say& say) {
    Index ix;
    for (const assetdb::IndexEntry& e : assetdb::readDbIndex(assets / "Debug" / "db_index.txt")) {
        if (!startsWith(e.name, kBundlePrefix)) continue;
        Bundle b;
        b.name = e.name;
        b.file = assetdb::pathForEntry(assets, e);
        b.stream = b.file.parent_path().parent_path() / "50" / b.file.filename();
        if (fs::exists(longPath(b.file))) ix.bundles.push_back(b);
    }
    if (ix.bundles.empty()) fail(assets.string() + " has no bundles listed in Debug\\db_index.txt (is it the game's assets folder?)");
    for (size_t k = 0; k < ix.bundles.size(); ++k) {
        Bundle& b = ix.bundles[k];
        ix.byName[b.name] = k;
        if (k % 25 == 0) say("indexing bundles: " + std::to_string(k) + " of " + std::to_string(ix.bundles.size()));
        Loaded l = loadAsset(b.file);
        if (!l.caff) continue;
        b.size = l.raw.size();
        const caff::Caff& c = *l.caff;
        for (size_t i = 0; i < c.symbols.size(); ++i) {
            if (!hasSection(c, i) || c.sectionOf(c.symbols[i]).name != ".data") continue;
            const std::string n = assetdb::baseAssetName(c.symbolName(i));
            if (!startsWith(n, "aid_")) continue;
            const auto id = assetdb::idFromName(n);
            if (!id) continue;
            ix.names.emplace(*id, n);
            std::vector<size_t>& h = ix.holders[*id];
            if (h.empty() || h.back() != k) h.push_back(k);
        }
    }
    return ix;
}

// A parsed bundle and its symbols by asset name.
struct Open {
    std::shared_ptr<caff::Caff> caff;
    std::map<std::string, std::vector<size_t>> byName;
};

class Bundles {
public:
    explicit Bundles(const Index& ix) : ix_(ix) {}
    const Open& get(size_t k) {
        if (auto it = open_.find(k); it != open_.end()) return it->second;
        if (open_.size() >= kOpenBundles) {
            open_.erase(order_.front());
            order_.pop_front();
        }
        Loaded l = loadAsset(ix_.bundles[k].file);
        if (!l.caff) fail(ix_.bundles[k].file.string() + " is not a bundle");
        Open o;
        o.caff = std::make_shared<caff::Caff>(std::move(*l.caff));
        for (size_t i = 0; i < o.caff->symbols.size(); ++i)
            if (o.caff->symbols[i].nameIndex && hasSection(*o.caff, i)) {
                const std::string n = assetdb::baseAssetName(o.caff->symbolName(i));
                if (startsWith(n, "aid_")) o.byName[n].push_back(i);
            }
        order_.push_back(k);
        return open_.emplace(k, std::move(o)).first->second;
    }

private:
    const Index& ix_;
    std::map<size_t, Open> open_;
    std::deque<size_t> order_;
};

struct Need {
    uint32_t id;
    std::string name;
    size_t from;  // the bundle it's copied from
};

// The assets a character's objparams `params` reaches that none of `have` holds: ids in objparams (and the other
// assets but models and textures), aid_ names their pool strings hold, and a model's textures.
std::vector<Need> needed(uint32_t params, const std::set<size_t>& have, const Index& ix, Bundles& bundles) {
    std::vector<Need> out;
    std::set<uint32_t> seen;
    std::deque<uint32_t> queue{params};
    auto reach = [&](uint32_t id) {
        if (ix.holders.count(id) && !seen.count(id)) queue.push_back(id);
    };
    while (!queue.empty()) {
        const uint32_t id = queue.front();
        queue.pop_front();
        if (!seen.insert(id).second) continue;
        const std::vector<size_t>& holders = ix.holders.at(id);
        size_t from = SIZE_MAX;
        for (size_t k : holders)
            if (have.count(k)) from = k;
        const bool held = from != SIZE_MAX;
        if (!held)
            for (size_t k : holders)
                if (from == SIZE_MAX || ix.bundles[k].size < ix.bundles[from].size) from = k;
        const std::string& name = ix.names.at(id);
        if (!held) out.push_back({id, name, from});
        if (uint8_t(id >> 24) == assetdb::kTypeTexture) continue;

        const Open& o = bundles.get(from);
        const caff::Caff& c = *o.caff;
        const auto syms = o.byName.find(name);
        if (syms == o.byName.end()) continue;
        for (size_t s : syms->second) {
            if (c.sectionOf(c.symbols[s]).name != ".data") continue;
            if (model::isModelSymbol(c, s)) {
                std::vector<std::string> textures;
                try {
                    textures = model::parse(c, s).textures;
                } catch (const std::exception&) {
                }
                for (const std::string& t : textures)
                    for (const std::string& n : {t, t + "mip"})
                        if (const auto tid = assetdb::idFromName(n)) reach(*tid);
                continue;
            }
            // Ids are read from objparams only: other assets' data (animations, collision) would match some by chance.
            const Bytes d = c.symbolData(s);
            if (uint8_t(id >> 24) == kTypeObjparams)
                for (size_t at = 0; at + 4 <= d.size(); at += 4)
                    if (const uint32_t v = be32(&d[at]); v != id) reach(v);
            for (int set = 0; set < 2; ++set)
                for (const caff::Relocation& r : c.relocs[set]) {
                    if (r.from != s + 1 || !r.to || r.to > c.symbols.size()) continue;
                    const Bytes p = c.symbolData(r.to - 1);
                    for (uint32_t off : r.offsets) {
                        if (off + 4 > d.size()) continue;
                        const uint32_t v = be32(&d[off]);
                        if (v >= p.size() || p.size() - v < 5 || std::memcmp(&p[v], "aid_", 4) != 0) continue;
                        size_t e = v;
                        while (e < p.size() && e - v < 256 && p[e]) ++e;
                        if (const auto sid = assetdb::idFromName(std::string(reinterpret_cast<const char*>(&p[v]), e - v))) reach(*sid);
                    }
                }
        }
    }
    return out;
}

// The bundles reNut loads with a character's own (part_api.cpp's LoadCharacterBundles), and the common one.
std::set<size_t> haveWith(size_t actor, const Index& ix) {
    std::set<size_t> have{actor};
    const std::string own = ix.bundles[actor].name.substr(kActorPrefix.size());
    std::vector<std::string> more{"common"};
    if (startsWith(own, "gruntbot") && own != "gruntbotcommon") more.push_back("actor_gruntbotcommon");
    if (startsWith(own, "npc_")) {
        more.push_back("actor_npccommon");
        more.push_back("actor_townsfolkcommon");
    }
    for (const std::string& m : more)
        if (auto it = ix.byName.find(kBundlePrefix + m); it != ix.byName.end()) have.insert(it->second);
    return have;
}

std::string kb(size_t bytes) { return std::to_string((bytes + 1023) / 1024) + " KB"; }

} // namespace

int build(const fs::path& assets, const fs::path& mods, const std::vector<std::string>& only, const Say& say) {
    const Index ix = readIndex(assets, say);
    Bundles bundles(ix);
    const fs::path out = mods / ".characters" / "Bundle" / "50";
    fs::create_directories(longPath(out));
    std::set<std::string> done;
    int written = 0;
    for (size_t actor = 0; actor < ix.bundles.size(); ++actor) {
        if (!startsWith(ix.bundles[actor].name, kActorPrefix)) continue;
        // Its characters: objparams with a model.
        std::vector<std::pair<std::string, uint32_t>> characters;
        {
            const Open& o = bundles.get(actor);
            for (const auto& [name, syms] : o.byName) {
                if (!startsWith(name, kParamsPrefix)) continue;
                const std::string character = name.substr(kParamsPrefix.size());
                if (done.count(character) || (!only.empty() && std::find(only.begin(), only.end(), character) == only.end())) continue;
                for (size_t s : syms) {
                    if (o.caff->sectionOf(o.caff->symbols[s]).name != ".data") continue;
                    const Bytes d = o.caff->symbolData(s);
                    if (d.size() >= kParamsModel + 4 && d[kParamsModel] == kTypeModel)
                        if (const auto id = assetdb::idFromName(name)) characters.emplace_back(character, *id);
                    break;
                }
            }
        }
        const std::set<size_t> have = haveWith(actor, ix);
        for (const auto& [character, params] : characters) {
            done.insert(character);
            std::vector<Need> needs;
            try {
                needs = needed(params, have, ix, bundles);
            } catch (const std::exception& e) {
                say("warning: " + character + " has no pack (" + e.what() + ")");
                continue;
            }
            std::vector<streambundle::Entry> entries;
            std::set<std::string> from;
            size_t bytes = 0;
            for (const Need& n : needs) {
                try {
                    const Open& o = bundles.get(n.from);
                    caff::Caff c = renutworld::copyAssets(*o.caff, o.byName.at(n.name));
                    entries.push_back({n.id, c.serialize()});
                    bytes += entries.back().data.size();
                    from.insert(ix.bundles[n.from].name.substr(kBundlePrefix.size()));
                    // A split texture's top level is its own asset in the paired stream bundle.
                    if (uint8_t(n.id >> 24) == assetdb::kTypeTexture && endsWith(n.name, "mip")) {
                        const std::string top = n.name.substr(0, n.name.size() - 3) + "top";
                        const auto tid = assetdb::idFromName(top);
                        std::vector<streambundle::IndexEntry> index;
                        if (tid && streambundle::readIndex(longPath(ix.bundles[n.from].stream), index))
                            for (const auto& e : index)
                                if (e.id == *tid && e.size) {
                                    entries.push_back({e.id, streambundle::readEntry(longPath(ix.bundles[n.from].stream), e)});
                                    bytes += e.size;
                                    break;
                                }
                    }
                } catch (const std::exception& e) {
                    say("  " + character + ": left out " + n.name + " (" + e.what() + ")");
                }
            }
            streambundle::StreamBundle sb;
            sb.timestamp = uint32_t(std::time(nullptr));
            sb.entries = std::move(entries);
            char hex[8];
            std::snprintf(hex, sizeof hex, "%06x", unsigned(*assetdb::idFromName("aid_streambundle_renutchar_" + character) & 0xFFFFFF));
            writeFile(out / hex, sb.serialize());
            ++written;
            std::string list;
            for (const std::string& f : from) list += (list.empty() ? "" : ", ") + f;
            say("renutchar_" + character + " (" + hex + "): " + std::to_string(sb.entries.size()) + " assets, " + kb(bytes) +
                (list.empty() ? std::string() : " from " + list));
        }
    }
    for (const std::string& o : only)
        if (!done.count(o)) say("warning: no character '" + o + "' (objparams aid_objparams_banjox_actor_" + o + " with a model)");
    return written;
}

} // namespace nut::renutchars
