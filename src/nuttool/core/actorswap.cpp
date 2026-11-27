#include "actorswap.hpp"

#include "assetdb.hpp"
#include "caff.hpp"
#include "container.hpp"
#include "gltfobj.hpp"
#include "image.hpp"
#include "json.hpp"
#include "model.hpp"
#include "modelimport.hpp"
#include "npcmodel.hpp"
#include "renutworld.hpp"
#include "streambundle.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <set>

namespace nut::actorswap {
namespace {

constexpr uint32_t kModel = 0xC0, kTable = 0xD0;  // an actor objparams' model and animation table
// Animation tables: {u32 count, u32}, then entries {char name[64], anim id, events id, ...} of 0x5C bytes.
constexpr uint32_t kEntrySize = 0x5C, kEntryAnim = 0x40, kEntryEvents = 0x44;
constexpr uint8_t kTypeModel = 0x04, kTypeAnim = 0x02;
const std::string kActorPrefix = "aid_objparams_banjox_actor_";

std::string toUtf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

bool hasSection(const caff::Caff& c, size_t i) {
    const uint8_t s = c.symbols[i].section;
    return s && s <= c.sections.size();
}

bool isData(const caff::Caff& c, size_t i) { return hasSection(c, i) && c.sectionOf(c.symbols[i]).name == ".data"; }

std::vector<Entry> readTable(const Bytes& d) {
    std::vector<Entry> out;
    if (d.size() < 8) return out;
    const uint32_t n = be32(&d[0]);
    for (uint32_t i = 0; i < n && 8 + uint64_t(kEntrySize) * (i + 1) <= d.size(); ++i) {
        const uint8_t* p = &d[8 + kEntrySize * i];
        Entry e;
        e.name.assign(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), 64));
        e.anim = be32(p + kEntryAnim);
        e.events = be32(p + kEntryEvents);
        out.push_back(e);
    }
    return out;
}

// The symbols of the asset `name` in `c`: its parts share the name.
std::vector<size_t> partsOf(const caff::Caff& c, const std::string& name) {
    std::vector<size_t> out;
    for (size_t i = 0; i < c.symbols.size(); ++i)
        if (c.symbols[i].nameIndex && hasSection(c, i) && assetdb::baseAssetName(c.symbolName(i)) == name) out.push_back(i);
    return out;
}

std::string looseKey(const std::string& s) {
    std::string out;
    for (char ch : lower(s))
        if (ch != '_') out += ch;
    return out;
}

int sectionNumber(const caff::Caff& c, const std::string& name) {
    const int i = c.findSection(name);
    return i < 0 ? -1 : i + 1;
}

// Adds the assets in `part` (a CAFF of their own, renutworld::copyAssets) to the bundle `dst`: their symbols at the
// ends of dst's sections of the same names, the pool entries they point into in dst's pools (sharing equal ones),
// their relocations and byte swaps, and a manifest entry each.
void merge(caff::Caff& dst, const caff::Caff& part) {
    std::map<size_t, size_t> poolTo;                       // part pool symbol -> dst pool symbol
    std::map<size_t, std::map<uint32_t, uint32_t>> moved;  // part pool symbol: entry offset -> its offset in dst's
    for (const caff::Pool& pp : part.pools) {
        const size_t ps = pp.symbol - 1;
        const std::string sec = part.sectionOf(part.symbols[ps]).name;
        caff::Pool* dp = nullptr;
        for (caff::Pool& p : dst.pools)
            if (dst.sectionOf(dst.symbols[p.symbol - 1]).name == sec) dp = &p;
        if (!dp) fail("the bundle has no " + sec + " pool");
        const size_t ds = dp->symbol - 1;
        poolTo[ps] = ds;
        Bytes pd = dst.symbolData(ds);
        const Bytes src = part.symbolData(ps);
        const uint32_t base = dst.symbols[ds].offset;
        for (const caff::PoolString& e : pp.strings) {
            if (uint64_t(e.offset) + e.length > src.size()) fail("a pool entry is outside its pool");
            std::optional<uint32_t> same;
            for (const caff::PoolString& x : dp->strings)
                if (x.hash == e.hash && x.length == e.length && uint64_t(x.offset) + x.length <= pd.size() &&
                    std::equal(src.begin() + e.offset, src.begin() + e.offset + e.length, pd.begin() + x.offset)) {
                    same = x.offset;
                    break;
                }
            if (!same) {
                // At the alignment it had (the largest power of two, up to 4096, its offset has).
                uint32_t align = 4096;
                while (align > 1 && e.offset % align) align >>= 1;
                uint32_t at = uint32_t(pd.size());
                while ((base + at) % align) ++at;
                pd.resize(at, 0);
                pd.insert(pd.end(), src.begin() + e.offset, src.begin() + e.offset + e.length);
                caff::PoolString ne = e;
                ne.offset = at;
                dp->strings.push_back(ne);
                same = at;
            }
            moved[ps][e.offset] = *same;
        }
        dst.replaceSymbolData(ds, pd);
    }

    std::map<size_t, Bytes> data;
    for (size_t k = 0; k < part.symbols.size(); ++k)
        if (!poolTo.count(k)) data[k] = part.symbolData(k);
    // Pointers into the pools follow their entries; values past a pool's end aren't pointers.
    for (int set = 0; set < 2; ++set)
        for (const caff::Relocation& r : part.relocs[set]) {
            const auto p = poolTo.find(r.to - 1);
            if (p == poolTo.end() || !data.count(r.from - 1)) continue;
            Bytes& d = data[r.from - 1];
            const auto& m = moved[r.to - 1];
            const uint32_t size = part.symbols[r.to - 1].size;
            for (uint32_t off : r.offsets) {
                if (uint64_t(off) + 4 > d.size()) fail("a relocation of " + part.symbolName(r.from - 1) + " is outside it");
                const uint32_t v = be32(&d[off]);
                if (v >= size) continue;
                auto it = m.upper_bound(v);
                if (it == m.begin()) fail(part.symbolName(r.from - 1) + " points outside its pool's entries");
                --it;
                putBe32(&d[off], it->second + (v - it->first));
            }
        }

    std::map<size_t, size_t> at;  // part symbol -> dst symbol
    std::set<uint32_t> names;     // the assets' names (1-based), for the manifest
    for (auto& [k, d] : data) {
        const std::string sec = part.sectionOf(part.symbols[k]).name;
        const int s = sectionNumber(dst, sec);
        if (s < 0) fail("the bundle has no " + sec + " section for " + part.symbolName(k));
        const std::string n = part.symbolName(k);
        const uint32_t ni = n.empty() ? 0 : dst.symbolNameIndex(n);
        // Symbols added later go before the manifest and move it up one: keep the indices current.
        const size_t i = dst.appendSymbol(ni, uint8_t(s), part.symbols[k].kind, d);
        for (auto& [pk, pv] : at)
            if (pv >= i) ++pv;
        for (auto& [pk, pv] : poolTo)
            if (pv >= i) ++pv;
        at[k] = i;
        if (sec == ".data" && ni) names.insert(ni);
    }
    for (int set = 0; set < 2; ++set) {
        for (const caff::Relocation& r : part.relocs[set]) {
            if (!at.count(r.from - 1)) continue;
            const auto p = poolTo.find(r.to - 1);
            const size_t to = p != poolTo.end() ? p->second : at.at(r.to - 1);
            dst.relocs[set].push_back({uint32_t(at.at(r.from - 1) + 1), uint32_t(to + 1), r.offsets});
        }
        for (const caff::SwapEntry& w : part.swaps[set])
            if (at.count(w.symbol - 1)) {
                caff::SwapEntry e = w;
                e.symbol = uint32_t(at.at(w.symbol - 1) + 1);
                dst.swaps[set].push_back(e);
            }
    }
    model::addToManifest(dst, std::vector<uint32_t>(names.begin(), names.end()));
}

// A bundle's stream bundle: a title update's 4f<id> pairs with 50<id> beside it, the game's Bundle/4f/<id> with
// Bundle/50/<id>. Empty when there is none.
fs::path streamOf(const fs::path& bundle) {
    const std::string f = toUtf8(bundle.filename());
    fs::path s = f.size() == 8 && startsWith(f, "4f") ? bundle.parent_path() / fromUtf8("50" + f.substr(2))
                                                       : bundle.parent_path().parent_path() / "50" / bundle.filename();
    std::error_code ec;
    return fs::exists(longPath(s), ec) ? s : fs::path();
}

// The bundles being changed, and those only read.
class Files {
public:
    explicit Files(const Index& ix) : ix_(ix) {}

    struct Edit {
        caff::Caff caff;
        bool changed = false;
        fs::path stream;
        std::optional<streambundle::StreamBundle> sb;
        bool streamChanged = false;
    };

    Edit& edit(size_t k) {
        auto it = edits_.find(k);
        if (it != edits_.end()) return it->second;
        Loaded l = loadAsset(ix_.bundles[k]);
        if (!l.caff) fail(toUtf8(ix_.bundles[k]) + " is not a bundle");
        Edit e;
        e.caff = std::move(*l.caff);
        e.stream = streamOf(ix_.bundles[k]);
        return edits_.emplace(k, std::move(e)).first->second;
    }

    const caff::Caff& read(size_t k) {
        auto it = reads_.find(k);
        if (it != reads_.end()) return *it->second;
        Loaded l = loadAsset(ix_.bundles[k]);
        if (!l.caff) fail(toUtf8(ix_.bundles[k]) + " is not a bundle");
        return *reads_.emplace(k, std::make_shared<caff::Caff>(std::move(*l.caff))).first->second;
    }

    streambundle::StreamBundle& stream(Edit& e, const std::string& bundle) {
        if (!e.sb) {
            if (e.stream.empty()) fail(bundle + " has no stream bundle for the streamed texture levels");
            Loaded l = loadAsset(e.stream);
            if (!l.sb) fail(toUtf8(e.stream) + " is not a stream bundle");
            e.sb = std::move(*l.sb);
        }
        return *e.sb;
    }

    // Writes what changed, each after a one-time .bak; returns the files.
    std::vector<fs::path> write() {
        std::vector<fs::path> out;
        for (auto& [k, e] : edits_) {
            if (e.streamChanged) {
                writeFile(prepareOutput(e.stream, ""), e.sb->serialize());
                out.push_back(e.stream);
            }
            if (e.changed) {
                saveCaff(prepareOutput(ix_.bundles[k], ""), e.caff);
                out.push_back(ix_.bundles[k]);
            }
        }
        return out;
    }

private:
    const Index& ix_;
    std::map<size_t, Edit> edits_;
    std::map<size_t, std::shared_ptr<caff::Caff>> reads_;
};

std::string bundleName(const Index& ix, size_t k) { return toUtf8(ix.bundles[k].filename()); }

// ---- an actor of your own from a rigged .glb

// Lower case letters, digits and underscores.
std::string assetWord(const std::string& s) {
    std::string out;
    for (char ch : lower(s)) out += std::isalnum(uint8_t(ch)) ? ch : '_';
    return out.empty() ? "x" : out;
}

std::string stemOf(const fs::path& glb) { return "renutactor_" + assetWord(toUtf8(glb.stem())); }

using M3 = std::array<std::array<double, 3>, 3>;  // [row][column]

// A transform's rotation: its columns made unit length (its scale taken out).
M3 rotationOf(const std::array<float, 16>& m) {
    M3 r{};
    for (int c = 0; c < 3; ++c) {
        double len = std::sqrt(double(m[size_t(c * 4)]) * m[size_t(c * 4)] + double(m[size_t(c * 4 + 1)]) * m[size_t(c * 4 + 1)] +
                               double(m[size_t(c * 4 + 2)]) * m[size_t(c * 4 + 2)]);
        if (len <= 0) len = 1;
        for (int row = 0; row < 3; ++row) r[size_t(row)][size_t(c)] = m[size_t(c * 4 + row)] / len;
    }
    return r;
}

M3 mul3(const M3& a, const M3& b) {
    M3 r{};
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j)
            for (size_t k = 0; k < 3; ++k) r[i][j] += a[i][k] * b[k][j];
    return r;
}

M3 transposed(const M3& a) {
    M3 r{};
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j) r[i][j] = a[j][i];
    return r;
}

// x y z w of a rotation matrix.
void quatOf(const M3& m, float* q) {
    const double tr = m[0][0] + m[1][1] + m[2][2];
    double x, y, z, w;
    if (tr > 0) {
        const double s = std::sqrt(tr + 1) * 2;
        w = s / 4, x = (m[2][1] - m[1][2]) / s, y = (m[0][2] - m[2][0]) / s, z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        w = (m[2][1] - m[1][2]) / s, x = s / 4, y = (m[0][1] + m[1][0]) / s, z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const double s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        w = (m[0][2] - m[2][0]) / s, x = (m[0][1] + m[1][0]) / s, y = s / 4, z = (m[1][2] + m[2][1]) / s;
    } else {
        const double s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        w = (m[1][0] - m[0][1]) / s, x = (m[0][2] + m[2][0]) / s, y = (m[1][2] + m[2][1]) / s, z = s / 4;
    }
    q[0] = float(x), q[1] = float(y), q[2] = float(z), q[3] = float(w);
}

// The game's assets for a .glb's actor: its model and textures (CAFFs of their own) and its animations by name.
struct Custom {
    std::string model;
    std::vector<caff::Caff> modelParts;  // the model, then its textures
    std::map<std::string, std::pair<std::string, caff::Caff>> anims;  // the file's animation -> its asset name and CAFF
};

Custom buildCustom(const Index& ix, const Plan& plan, const Say& say) {
    const Bytes file = readFile(plan.glb);
    const float s = plan.scale > 0 ? plan.scale : 1.0f;
    const std::vector<gltfobj::Joint> joints = gltfobj::skeleton(file);
    if (joints.empty() || joints.size() > 255) fail(toUtf8(plan.glb.filename()) + ": a skeleton of 1 to 255 bones is needed (it has " +
                                                    std::to_string(joints.size()) + ")");
    // The game's rig keeps only each bone's rest position; its rest rotation goes into the animations.
    npcmodel::Rig rig;
    std::map<std::string, int> boneOf;
    for (size_t i = 0; i < joints.size(); ++i) {
        rig.bones.push_back({joints[i].name, joints[i].parent, {joints[i].bind[12] * s, joints[i].bind[13] * s, joints[i].bind[14] * s}});
        boneOf.emplace(joints[i].name, int(i));
    }
    const std::string stem = stemOf(plan.glb);
    Custom c;
    c.model = "aid_model_" + stem;

    // The draws: each mesh's triangles, in draws of at most 65535 vertices, a texture per image (white for plain colours).
    const std::vector<gltfobj::SkinnedMesh> meshes = gltfobj::skinnedMeshes(file);
    if (meshes.empty()) fail(toUtf8(plan.glb.filename()) + " has no mesh");
    const auto makeTexture = renutworld::textureMaker(ix.assets);
    std::map<std::string, uint32_t> textureOf;
    std::vector<std::string> textureNames;
    std::vector<caff::Caff> textures;
    std::vector<npcmodel::SkinDraw> draws;
    for (const gltfobj::SkinnedMesh& sm : meshes) {
        const std::string key = sm.colourImage.empty() ? std::string() : sm.colourImageName.empty() ? "image of " + sm.name : sm.colourImageName;
        const auto [it, fresh] = textureOf.emplace(key, uint32_t(textureNames.size()));
        const uint32_t tex = it->second;
        if (fresh) {
            const std::string name = "aid_texture_" + stem + "_" + std::to_string(tex);
            image::Rgba img;
            if (key.empty()) {
                img.width = img.height = 4;
                img.pixels.assign(4 * 4 * 4, 255);
            } else {
                img = image::decodeImage(sm.colourImage);
            }
            std::string summary;
            textures.push_back(caff::Caff::parse(makeTexture(name, img, summary)));
            textureNames.push_back(name);
            say(name + ": " + (key.empty() ? std::string("white, for plain colours") : key) + ", " + summary);
        }
        const std::array<float, 4> colour =
            sm.plainColour ? std::array<float, 4>{(*sm.plainColour)[0], (*sm.plainColour)[1], (*sm.plainColour)[2], 1} : std::array<float, 4>{1, 1, 1, 1};
        npcmodel::SkinDraw dr;
        dr.texture = tex;
        std::vector<uint32_t> local(sm.points.size(), UINT32_MAX);
        auto finish = [&] {
            if (!dr.indices.empty()) draws.push_back(std::move(dr));
            dr = npcmodel::SkinDraw{};
            dr.texture = tex;
            std::fill(local.begin(), local.end(), UINT32_MAX);
        };
        for (const auto& tri : sm.triangles) {
            size_t added = 0;
            for (uint32_t i : tri) added += local[i] == UINT32_MAX;
            if (dr.vertices.size() + added > 65535) finish();
            for (uint32_t i : tri) {
                if (local[i] == UINT32_MAX) {
                    local[i] = uint32_t(dr.vertices.size());
                    npcmodel::SkinVertex sv;
                    const auto& p = sm.points[i];
                    sv.v.pos = {p[0] * s, p[1] * s, p[2] * s};
                    if (i < sm.normals.size()) sv.v.normal = sm.normals[i];
                    sv.v.colour = colour;
                    // skinnedMeshes gives v up; the game's draws take it down from the image's top, as glTF does.
                    if (i < sm.uvs.size()) sv.v.uv = {sm.uvs[i][0], 1 - sm.uvs[i][1]};
                    // Its four heaviest bones the skeleton has.
                    std::vector<std::pair<int, float>> w;
                    if (i < sm.skin.size())
                        for (const auto& [name, weight] : sm.skin[i])
                            if (auto b = boneOf.find(name); b != boneOf.end() && weight > 0) w.push_back({b->second, weight});
                    std::stable_sort(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                    if (w.size() > 4) w.resize(4);
                    if (w.empty()) w.push_back({0, 1.0f});
                    float total = 0;
                    for (const auto& [b, x] : w) total += x;
                    sv.bones = {};
                    sv.weights = {};
                    for (size_t k = 0; k < w.size(); ++k) {
                        sv.bones[k] = uint8_t(w[k].first);
                        sv.weights[k] = uint8_t(std::max(1L, std::lround(255.0f * w[k].second / total)));
                    }
                    dr.vertices.push_back(std::move(sv));
                }
                dr.indices.push_back(local[i]);
            }
        }
        finish();
    }
    c.modelParts.push_back(caff::Caff::parse(npcmodel::writeSkinnedModel(c.model, rig, draws, textureNames)));
    for (caff::Caff& t : textures) c.modelParts.push_back(std::move(t));
    say(c.model + ": " + std::to_string(draws.size()) + " draw(s), " + std::to_string(rig.bones.size()) + " bones");

    // The animations. A bone's world turn in the game is the file's times its rest turn's inverse (the game's bind
    // turns are none); its key is that turn and offset from its parent's, less its rest offset.
    const size_t n = joints.size();
    std::vector<M3> restInverse(n);
    for (size_t j = 0; j < n; ++j) restInverse[j] = transposed(rotationOf(joints[j].bind));
    std::set<std::string> used;
    for (const gltfobj::JointAnimation& ja : gltfobj::jointAnimations(file, 30)) {
        std::string word = assetWord(ja.name);
        for (int k = 2; used.count(word); ++k) word = assetWord(ja.name) + "_" + std::to_string(k);
        used.insert(word);
        const std::string asset = "aid_anim_" + stem + "_" + word;
        const uint32_t frames = uint32_t(ja.frames.size());
        if (frames > 0xFFFF) fail(ja.name + ": an animation of at most 65535 frames (36 minutes)");
        std::vector<model::Key> keys(size_t(frames) * n);
        for (uint32_t f = 0; f < frames; ++f) {
            std::vector<M3> turn(n);
            std::vector<std::array<double, 3>> at(n);
            for (size_t j = 0; j < n; ++j) {
                const auto& m = ja.frames[f][j];
                turn[j] = mul3(rotationOf(m), restInverse[j]);
                at[j] = {m[12] * double(s), m[13] * double(s), m[14] * double(s)};
            }
            for (size_t j = 0; j < n; ++j) {
                model::Key& k = keys[size_t(f) * n + j];
                const int p = joints[j].parent;
                std::array<double, 3> offset = at[j], rest = {rig.bones[j].world[0], rig.bones[j].world[1], rig.bones[j].world[2]};
                M3 localTurn = turn[j];
                if (p >= 0) {
                    const M3 back = transposed(turn[size_t(p)]);
                    localTurn = mul3(back, turn[j]);
                    const std::array<double, 3> d = {at[j][0] - at[size_t(p)][0], at[j][1] - at[size_t(p)][1], at[j][2] - at[size_t(p)][2]};
                    for (size_t r = 0; r < 3; ++r) offset[r] = back[r][0] * d[0] + back[r][1] * d[1] + back[r][2] * d[2];
                    for (size_t r = 0; r < 3; ++r) rest[r] -= rig.bones[size_t(p)].world[r];
                }
                quatOf(localTurn, k.rotation);
                for (size_t r = 0; r < 3; ++r) k.translation[r] = float(offset[r] - rest[r]);
            }
        }
        c.anims[ja.name] = {asset, caff::Caff::parse(npcmodel::writeAnim(asset, rig, keys, frames))};
        say(asset + ": " + ja.name + ", " + std::to_string(frames) + " frames");
    }
    if (c.anims.empty()) fail(toUtf8(plan.glb.filename()) + " has no animations for its skeleton");
    return c;
}

// The first choice among (name, value) pairs for each target entry: the same name, then ignoring case and
// underscores, else `fallback`.
std::map<std::string, std::string> matchEntries(const Actor& target, const std::vector<std::pair<std::string, std::string>>& choices,
                                                const std::string& fallback) {
    std::map<std::string, std::string> exact, loose, out;
    for (const auto& [name, value] : choices) {
        exact.emplace(name, value);
        loose.emplace(looseKey(name), value);
    }
    for (const Entry& e : target.entries) {
        if (!e.anim) continue;
        std::string v = fallback;
        if (auto x = exact.find(e.name); x != exact.end()) v = x->second;
        else if (auto y = loose.find(looseKey(e.name)); y != loose.end()) v = y->second;
        if (!v.empty()) out[e.name] = v;
    }
    return out;
}

} // namespace

std::string Index::nameOf(uint32_t id) const {
    auto it = names.find(id);
    return it == names.end() ? std::string() : it->second;
}

const Actor* Index::actor(const std::string& name) const {
    for (const Actor& a : actors)
        if (a.name == name) return &a;
    return nullptr;
}

Index scan(const fs::path& assets, const fs::path& update, const Say& say) {
    Index ix;
    ix.assets = assets;
    for (const assetdb::IndexEntry& e : assetdb::readDbIndex(assets / "Debug" / "db_index.txt")) {
        if (!startsWith(e.name, "aid_bundle_")) continue;
        fs::path f = assetdb::pathForEntry(assets, e);
        std::error_code ec;
        if (!update.empty()) {
            const fs::path u = update / fromUtf8(toUtf8(f.parent_path().filename()) + toUtf8(f.filename()));
            if (fs::exists(longPath(u), ec)) f = u;
        }
        if (fs::exists(longPath(f), ec)) ix.bundles.push_back(f);
    }
    if (ix.bundles.empty()) fail(toUtf8(assets) + " has no bundles listed in Debug\\db_index.txt (is it the game's assets folder?)");
    std::map<uint32_t, std::vector<Entry>> tables;
    std::map<std::string, Actor> actors;
    for (size_t k = 0; k < ix.bundles.size(); ++k) {
        if (k % 25 == 0) say("reading bundles: " + std::to_string(k) + " of " + std::to_string(ix.bundles.size()));
        Loaded l;
        try {
            l = loadAsset(ix.bundles[k]);
        } catch (const std::exception&) {
            continue;
        }
        if (!l.caff) continue;
        const caff::Caff& c = *l.caff;
        for (size_t i = 0; i < c.symbols.size(); ++i) {
            if (!isData(c, i)) continue;
            const std::string n = assetdb::baseAssetName(c.symbolName(i));
            if (!startsWith(n, "aid_")) continue;
            const auto id = assetdb::idFromName(n);
            if (!id) continue;
            ix.names.emplace(*id, n);
            std::vector<size_t>& h = ix.holders[*id];
            if (h.empty() || h.back() != k) h.push_back(k);
            if (startsWith(n, "aid_animtable_") && !tables.count(*id)) tables[*id] = readTable(c.symbolData(i));
            if (startsWith(n, kActorPrefix) && !actors.count(n)) {
                const Bytes d = c.symbolData(i);
                if (d.size() >= kTable + 4 && d[kModel] == kTypeModel) actors[n] = Actor{n, be32(&d[kModel]), be32(&d[kTable]), {}};
            }
        }
    }
    for (auto& [n, a] : actors) {
        if (auto t = tables.find(a.table); t != tables.end()) a.entries = t->second;
        ix.actors.push_back(std::move(a));
    }
    say(std::to_string(ix.actors.size()) + " actors with a model in " + std::to_string(ix.bundles.size()) + " bundles");
    return ix;
}

std::map<std::string, std::string> autoMatch(const Actor& target, const Actor& source, const Index& ix) {
    std::vector<std::pair<std::string, std::string>> choices;
    std::string first, stand;
    for (const Entry& e : source.entries) {
        const std::string n = ix.nameOf(e.anim);
        if (n.empty()) continue;
        choices.push_back({e.name, n});
        if (first.empty()) first = n;
        if (e.name == "stand") stand = n;
    }
    return matchEntries(target, choices, stand.empty() ? first : stand);
}

std::vector<std::string> glbAnimations(const fs::path& glb) {
    std::vector<std::string> out;
    for (const gltfobj::JointAnimation& a : gltfobj::jointAnimations(readFile(glb), 1)) out.push_back(a.name);
    return out;
}

std::map<std::string, std::string> autoMatchGlb(const Actor& target, const std::vector<std::string>& animations) {
    std::vector<std::pair<std::string, std::string>> choices;
    std::string fallback;
    for (const char* like : {"stand", "idle"})
        for (const std::string& a : animations)
            if (fallback.empty() && lower(a).find(like) != std::string::npos) fallback = a;
    for (const std::string& a : animations) choices.push_back({a, a});
    if (fallback.empty() && !animations.empty()) fallback = animations.front();
    return matchEntries(target, choices, fallback);
}

std::vector<std::string> sourceAnims(const Actor& source, const Index& ix) {
    std::set<std::string> seen;
    std::vector<std::string> own, more;
    for (const Entry& e : source.entries) {
        const std::string n = ix.nameOf(e.anim);
        if (!n.empty() && seen.insert(n).second) own.push_back(n);
    }
    const std::string model = ix.nameOf(source.model);
    if (startsWith(model, "aid_model_")) {
        const std::string prefix = "aid_anim_" + model.substr(10) + "_";
        for (const auto& [id, n] : ix.names)
            if (startsWith(n, prefix) && !seen.count(n)) more.push_back(n);
    }
    std::sort(own.begin(), own.end());
    std::sort(more.begin(), more.end());
    own.insert(own.end(), more.begin(), more.end());
    return own;
}

std::vector<fs::path> apply(const Index& ix, const Plan& plan, const Say& say) {
    const Actor* target = ix.actor(plan.target);
    if (!target) fail(plan.target + " is not an actor (objparams with a model) in the game's bundles");
    const bool own = !plan.glb.empty();
    const Actor* source = own ? nullptr : ix.actor(plan.source);
    if (!own && !source) fail(plan.source + " is not an actor (objparams with a model) in the game's bundles");
    if (target == source) fail("an actor can't replace itself");
    const std::string tableName = ix.nameOf(target->table);
    if (tableName.empty() || !ix.holders.count(target->table)) fail(plan.target + " has no animation table in the game's bundles");
    std::optional<Custom> custom;
    if (own) custom = buildCustom(ix, plan, say);
    const std::string modelName = own ? custom->model : ix.nameOf(source->model);
    const std::string sourceName = own ? toUtf8(plan.glb.filename()) : plan.source;
    const uint32_t modelId = own ? *assetdb::idFromName(modelName) : source->model;
    if (!own && (modelName.empty() || !ix.holders.count(modelId))) fail(plan.source + "'s model isn't in the game's bundles");

    // Every entry that plays something needs one of the source's anims: the game animates a model only with anims
    // made for its skeleton.
    std::map<std::string, uint32_t> animOf;
    std::map<uint32_t, const caff::Caff*> ownAnim;  // a .glb's anims, by id
    for (const Entry& e : target->entries) {
        if (!e.anim) continue;
        const auto it = plan.anims.find(e.name);
        if (it == plan.anims.end() || it->second.empty())
            fail("no animation is chosen for the target's entry " + e.name + " (each needs one made for the new model's skeleton)");
        if (own) {
            const auto a = custom->anims.find(it->second);
            if (a == custom->anims.end()) fail(it->second + " is not one of " + sourceName + "'s animations");
            const uint32_t id = *assetdb::idFromName(a->second.first);
            animOf[e.name] = id;
            ownAnim[id] = &a->second.second;
            continue;
        }
        const auto id = assetdb::idFromName(it->second);
        if (!id || uint8_t(*id >> 24) != kTypeAnim || !ix.holders.count(*id)) fail(it->second + " is not an animation in the game's bundles");
        animOf[e.name] = *id;
    }
    std::map<uint32_t, uint32_t> eventsOf;  // a source anim -> the events its source entry plays with it
    if (source)
        for (const Entry& e : source->entries)
            if (e.anim && e.events) eventsOf.emplace(e.anim, e.events);

    Files files(ix);
    // Adds a .glb actor's asset; a bundle holding one by that name already was given it before.
    auto mergeOwn = [&](Files::Edit& ed, const caff::Caff& part, const std::string& where) {
        const std::string name = assetdb::baseAssetName(part.symbolName(0));
        if (!partsOf(ed.caff, name).empty())
            fail(where + " already holds " + name + ": restore it first (Tools > Restore original files), or rename the .glb");
        merge(ed.caff, part);
        ed.changed = true;
    };
    // Copies the asset `id` into the bundle being edited, from a bundle holding it; false when it is there already.
    auto copyInto = [&](Files::Edit& ed, uint32_t id) {
        const std::string name = ix.nameOf(id);
        if (name.empty() || !ix.holders.count(id)) fail("asset " + hex32(id) + " isn't in the game's bundles");
        if (!partsOf(ed.caff, name).empty()) return false;
        const caff::Caff& src = files.read(ix.holders.at(id).front());
        const std::vector<size_t> parts = partsOf(src, name);
        if (parts.empty()) fail(name + " wasn't found where the index put it");
        merge(ed.caff, renutworld::copyAssets(src, parts));
        ed.changed = true;
        return true;
    };

    // Actors sharing the table play the new anims too, so they draw the new model as well.
    std::vector<std::string> drawing{plan.target};
    for (const Actor& a : ix.actors)
        if (&a != target && a.table == target->table) drawing.push_back(a.name);
    if (drawing.size() > 1) {
        std::string list;
        for (size_t i = 1; i < drawing.size(); ++i) list += (i > 1 ? ", " : "") + drawing[i];
        say(tableName + " is also the table of " + list + ": they draw " + modelName + " too");
    }
    std::set<size_t> modelBundles;
    for (const std::string& n : drawing)
        if (const auto id = assetdb::idFromName(n); id && ix.holders.count(*id))
            modelBundles.insert(ix.holders.at(*id).begin(), ix.holders.at(*id).end());

    // The model, its textures and the actors drawing it, in every bundle holding one of them.
    std::vector<std::string> textures;
    if (!own) {
        const caff::Caff& modelSrc = files.read(ix.holders.at(modelId).front());
        for (size_t s : partsOf(modelSrc, modelName))
            if (model::isModelSymbol(modelSrc, s)) textures = model::parse(modelSrc, s).textures;
    }
    for (size_t k : modelBundles) {
        Files::Edit& ed = files.edit(k);
        const std::string where = bundleName(ix, k);
        int copied = 0;
        if (own) {
            for (const caff::Caff& part : custom->modelParts) mergeOwn(ed, part, where);
            say(where + ": added " + modelName + " and its " + std::to_string(custom->modelParts.size() - 1) + " texture(s)");
        } else if (copyInto(ed, modelId)) {
            say(where + ": copied " + modelName);
        }
        for (const std::string& t : textures) {
            if (!partsOf(ed.caff, t).empty() || !partsOf(ed.caff, t + "mip").empty()) continue;
            const auto whole = assetdb::idFromName(t), mip = assetdb::idFromName(t + "mip");
            if (whole && ix.holders.count(*whole)) {
                copied += copyInto(ed, *whole);
                continue;
            }
            if (!mip || !ix.holders.count(*mip)) fail("the texture " + t + " of " + modelName + " isn't in the game's bundles");
            // A split texture: its mip half here, its streamed top level in the paired stream bundle.
            const size_t from = ix.holders.at(*mip).front();
            copied += copyInto(ed, *mip);
            const auto top = assetdb::idFromName(t + "top");
            const fs::path fromStream = streamOf(ix.bundles[from]);
            std::vector<streambundle::IndexEntry> index;
            std::optional<streambundle::Entry> entry;
            if (top && !fromStream.empty() && streambundle::readIndex(longPath(fromStream), index))
                for (const auto& e : index)
                    if (e.id == *top && e.size) entry = streambundle::Entry{e.id, streambundle::readEntry(longPath(fromStream), e)};
            if (!entry) fail("the streamed top level of " + t + " wasn't found beside " + bundleName(ix, from));
            if (model::addStreamEntries(files.stream(ed, where), {*entry})) ed.streamChanged = true;
        }
        if (copied) say(where + ": copied " + std::to_string(copied) + " texture(s) the model needs");
        for (const std::string& n : drawing)
            for (size_t s : partsOf(ed.caff, n)) {
                if (!isData(ed.caff, s)) continue;
                Bytes d = ed.caff.symbolData(s);
                if (d.size() < kModel + 4) continue;
                putBe32(&d[kModel], modelId);
                ed.caff.replaceSymbolData(s, d);
                ed.changed = true;
                say(where + ": " + n + " draws " + modelName);
            }
    }

    // The anims (and events) and the table playing them, in every bundle holding the table.
    std::set<uint32_t> anims;
    for (const auto& [n, a] : animOf) anims.insert(a);
    for (size_t k : ix.holders.at(target->table)) {
        Files::Edit& ed = files.edit(k);
        const std::string where = bundleName(ix, k);
        int copied = 0;
        for (uint32_t a : anims) {
            if (own) {
                mergeOwn(ed, *ownAnim.at(a), where);
                ++copied;
                continue;
            }
            copied += copyInto(ed, a);
            if (plan.events == Events::Source)
                if (auto ev = eventsOf.find(a); ev != eventsOf.end() && ix.holders.count(ev->second)) copied += copyInto(ed, ev->second);
        }
        if (copied) say(where + ": copied " + std::to_string(copied) + " animation(s) and event list(s)");
        for (size_t s : partsOf(ed.caff, tableName)) {
            if (!isData(ed.caff, s)) continue;
            Bytes d = ed.caff.symbolData(s);
            const std::vector<Entry> entries = readTable(d);
            for (size_t i = 0; i < entries.size(); ++i) {
                const auto a = animOf.find(entries[i].name);
                if (a == animOf.end()) continue;
                uint8_t* p = &d[8 + kEntrySize * i];
                putBe32(p + kEntryAnim, a->second);
                if (plan.events == Events::None) putBe32(p + kEntryEvents, 0);
                if (plan.events == Events::Source) {
                    const auto ev = eventsOf.find(a->second);
                    putBe32(p + kEntryEvents, ev != eventsOf.end() && ix.holders.count(ev->second) ? ev->second : 0);
                }
            }
            ed.caff.replaceSymbolData(s, d);
            ed.changed = true;
        }
        say(where + ": " + tableName + " plays " + std::to_string(anims.size()) + " of " + sourceName + "'s animations");
    }
    return files.write();
}

std::string planToJson(const Plan& plan) {
    Json j = Json::object();
    j["target"] = plan.target;
    if (plan.glb.empty()) {
        j["source"] = plan.source;
    } else {
        j["glb"] = toUtf8(plan.glb);
        j["scale"] = double(plan.scale);
    }
    j["events"] = plan.events == Events::Source ? "source" : plan.events == Events::None ? "none" : "keep";
    Json anims = Json::object();
    for (const auto& [entry, anim] : plan.anims) anims[entry] = anim;
    j["anims"] = anims;
    return j.dump();
}

Plan planFromJson(const std::string& text) {
    const Json j = Json::parse(text);
    Plan p;
    if (j.has("target")) p.target = j["target"].asString();
    if (j.has("source")) p.source = j["source"].asString();
    if (j.has("glb")) p.glb = fromUtf8(j["glb"].asString());
    if (j.has("scale")) p.scale = float(j["scale"].asNumber());
    if (j.has("events")) {
        const std::string e = j["events"].asString();
        if (e == "source") p.events = Events::Source;
        else if (e == "none") p.events = Events::None;
        else if (e != "keep") fail("events is keep, source or none, not " + e);
    }
    if (j.has("anims"))
        for (const auto& [entry, anim] : j["anims"].members()) p.anims[entry] = anim.asString();
    return p;
}

} // namespace nut::actorswap
