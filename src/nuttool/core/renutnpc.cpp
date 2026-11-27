#include "renutnpc.hpp"

#include "assetdb.hpp"
#include "blockmodel.hpp"
#include "caff.hpp"
#include "container.hpp"
#include "gamescript.hpp"
#include "gltfobj.hpp"
#include "json.hpp"
#include "npcmodel.hpp"
#include "renutpart.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nut::renutnpc {
namespace {

constexpr float kPi = 3.14159265358979f;

// The game's assets the NPCs' layouts are copied from: Showdown Town's common bundle (Pikelet's actor,
// body and states, Humba's chardata) and the always loaded common bundle (Park Prix 1's challenge).
constexpr const char* kTownBundle = "234cec";
constexpr const char* kCommonBundle = "685374";
constexpr const char* kActorTemplate = "aid_objparams_banjox_actor_npc_pikelet";      // objDefId_banjoactor, 636 bytes
constexpr const char* kBodyTemplate = "aid_objparams_banjox_actorbody_pikelet";       // objDefId_entityBodyHuman, 1032
constexpr const char* kStandTemplate = "aid_objparams_banjox_actorstate_pikelet_stand";  // objDefId_entityStateStand, 1404
constexpr const char* kIdleTemplate = "aid_objparams_banjox_actorstate_pikelet_idle";    // objDefId_entityStateIdle, 1092
constexpr const char* kCharTemplate = "aid_chardata_banjox_humba_default";            // 264 bytes, a woman's voice
constexpr const char* kChallengeTemplate = "aid_challenge_banjox_banjoland_act1_game3";  // Park Prix 1: header, gates, end

// A dialog's text comes from its loctext (category "dialog"), which must be loaded for the game to ask
// for it at all; the challenges' one always is, and reNut answers our keys.
constexpr const char* kDialogLoctext = "aid_loctext_banjox_challenges";
constexpr int kListener = 0;  // dbCharData_Id_Banjo

// Actor objparams fields (Pikelet's layout).
constexpr uint32_t kActorTag = 0x002, kActorModel = 0x0C0, kActorAnimtable = 0x0D0, kActorCallout = 0x0D4,
                   kActorBody = 0x1D8, kActorIndicator = 0x1F4, kActorTag2 = 0x23C;
constexpr uint32_t kBodyStatetable = 0x098;
// Chardata: its actor, listen, 4 talk and stand animations, its voice (4 moods' cues, each short, medium and
// long, which the game plays as each page starts: sub_823E9610, sub_823EFCA0), then 6 moods {mood, open, closed}.
constexpr uint32_t kCharActor = 0x20, kCharAnims = 0x24, kCharAnimCount = 6, kCharMoods = 0xC0, kCharMoodCount = 6;
constexpr uint32_t kCharVoice = 0x90, kCharVoiceSize = 48;

const std::vector<std::string> kLines = {"greeting", "objectives", "win", "lose", "completed"};

// The game's characters whose voice cues an NPC's chardata can carry (reNut loads their dialog wavebanks).
struct GameVoice {
    const char* name;
    const char* bundle;
    const char* chardata;
};
const GameVoice kGameVoices[] = {
    {"banjo", kCommonBundle, "aid_chardata_banjox_banjo_default"},
    {"kazooie", kCommonBundle, "aid_chardata_banjox_kazooie_default"},
    {"mumbo", kCommonBundle, "aid_chardata_banjox_mumbo_default"},
    {"humba", kTownBundle, "aid_chardata_banjox_humba_default"},
    {"grunty", kCommonBundle, "aid_chardata_banjox_grunty_default"},
    {"klungo", kTownBundle, "aid_chardata_banjox_klungo_default"},
    {"log", kCommonBundle, "aid_chardata_banjox_log_default"},
    {"bottles", kTownBundle, "aid_chardata_banjox_bottles_default"},
    {"jinjo", kTownBundle, "aid_chardata_banjox_jinjo_default"},
    {"pikelet", kTownBundle, "aid_chardata_banjox_pikelet_default"},
    {"pigpig", kTownBundle, "aid_chardata_banjox_pigpig_default"},
    {"jolly", kTownBundle, "aid_chardata_banjox_jolly_default"},
    {"rhino", kTownBundle, "aid_chardata_banjox_rhino_default"},
    {"piddles", kTownBundle, "aid_chardata_banjox_piddles_default"},
    {"blubber", kTownBundle, "aid_chardata_banjox_blubber_default"},
    {"ghostpig", kTownBundle, "aid_chardata_banjox_ghostpig_default"},
    {"penguin", kTownBundle, "aid_chardata_banjox_penguin_default"},
    {"boggy", kTownBundle, "aid_chardata_banjox_boggy_default"},
    {"mrfit", kTownBundle, "aid_chardata_banjox_mrfit_default"},
    {"kingjingaling", kTownBundle, "aid_chardata_banjox_kingjingaling_default"},
    {"thomas", kTownBundle, "aid_chardata_banjox_thomas_default"},
};

bool isSoundFile(const fs::path& p) {
    const auto u = p.extension().u8string();
    const std::string ext = lower(std::string(u.begin(), u.end()));
    return ext == ".ogg" || ext == ".mp3" || ext == ".flac" || ext == ".wav";
}

uint32_t assetId(const std::string& name) {
    auto id = assetdb::idFromName(name);
    if (!id) fail("no asset id for " + name);
    return *id;
}

void putF32(uint8_t* p, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    putBe32(p, v);
}

// A fixed-size string field: `s` and zeros after it.
void putString(Bytes& d, uint32_t at, uint32_t size, const std::string& s) {
    if (s.size() >= size) fail("'" + s + "' is longer than its field's " + std::to_string(size - 1) + " characters");
    if (at + size > d.size()) fail("a string field is outside its asset");
    std::fill(d.begin() + at, d.begin() + at + size, 0);
    std::memcpy(&d[at], s.data(), s.size());
}

// A CAFF holding one asset: a .data symbol named `name`, without pointers.
Bytes oneAsset(const std::string& name, const Bytes& data) {
    caff::Caff c;
    c.symbolNames.push_back(name);
    caff::Section s;
    s.name = ".data";
    s.alignLog2 = 4;
    s.data = data;
    while (s.data.size() % 16) s.data.push_back(0);
    c.sections.push_back(s);
    c.symbols.push_back({1, 0, uint32_t(data.size()), 1, 4});
    return c.serialize();
}

// A bundle of the game's, loaded once.
struct GameBundle {
    fs::path path;
    std::optional<caff::Caff> caff;

    const caff::Caff& get(const fs::path& assets, const char* hex) {
        if (!caff) {
            path = assets / "Bundle" / "4f" / hex;
            Loaded l = loadAsset(path);
            if (!l.caff) fail(path.string() + " is not one of the game's bundles (is the assets folder the game's?)");
            caff = std::move(l.caff);
        }
        return *caff;
    }
};

// The data of the asset `name` (a .data symbol) in `c`, which is `size` bytes and has no pointers.
Bytes templateData(const caff::Caff& c, const fs::path& bundle, const std::string& name, size_t size) {
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        const caff::Symbol& s = c.symbols[i];
        if (!s.section || s.section > c.sections.size() || c.sections[s.section - 1].name != ".data" ||
            assetdb::baseAssetName(c.symbolName(i)) != name)
            continue;
        for (int set = 0; set < 2; ++set)
            for (const caff::Relocation& r : c.relocs[set])
                if (r.from == i + 1) fail(name + " in " + bundle.string() + " has pointers, which its copy can't keep");
        Bytes d = c.symbolData(i);
        if (d.size() != size) fail(name + " in " + bundle.string() + " is " + std::to_string(d.size()) + " bytes, not " + std::to_string(size));
        return d;
    }
    fail(name + " is not in " + bundle.string());
}

// FNV-1a, for names too long for their field.
uint32_t fnv(const std::string& s) {
    uint32_t h = 2166136261u;
    for (char c : s) h = (h ^ uint8_t(c)) * 16777619u;
    return h;
}

// `name` when it fits a 32-byte field, else `shorter`, else `prefix` and a hash of `name`.
std::string fit31(const std::string& name, const std::string& shorter, const std::string& prefix) {
    if (name.size() <= 31) return name;
    if (shorter.size() <= 31) return shorter;
    char b[16];
    std::snprintf(b, sizeof b, "%08x", unsigned(fnv(name)));
    return prefix + b;
}

std::string luaString(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        if (ch == '"' || ch == '\\') out += std::string("\\") + ch;
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else out += ch;
    }
    return out + "\"";
}

// ---- The JSON ----------------------------------------------------------------------------------

std::string str(const Json& o, const char* key, const std::string& where, bool required, const std::string& fallback = {}) {
    if (!o.has(key) || o[key].isNull()) {
        if (required) fail(where + ": \"" + key + "\" is missing");
        return fallback;
    }
    if (o[key].type() != Json::String) fail(where + ": \"" + key + "\" is a string");
    return o[key].asString();
}

float num(const Json& o, const char* key, const std::string& where, std::optional<float> fallback) {
    if (!o.has(key) || o[key].isNull()) {
        if (!fallback) fail(where + ": \"" + key + "\" is missing");
        return *fallback;
    }
    if (o[key].type() != Json::Number) fail(where + ": \"" + key + "\" is a number");
    return float(o[key].asNumber());
}

template <size_t N>
std::array<float, N> vec(const Json& o, const char* key, const std::string& where, std::optional<std::array<float, N>> fallback = {}) {
    if (!o.has(key) || o[key].isNull()) {
        if (!fallback) fail(where + ": \"" + key + "\" is missing");
        return *fallback;
    }
    const Json& v = o[key];
    if (v.type() != Json::Array || v.size() != N) fail(where + ": \"" + key + "\" is " + std::to_string(N) + " numbers in [ ]");
    std::array<float, N> out{};
    for (size_t k = 0; k < N; ++k) {
        if (v[k].type() != Json::Number) fail(where + ": \"" + key + "\" is " + std::to_string(N) + " numbers in [ ]");
        out[k] = float(v[k].asNumber());
    }
    return out;
}

bool validTag(const std::string& s) {
    if (s.empty() || s.size() > 63) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// ---- The model ---------------------------------------------------------------------------------

struct Model {
    Bytes model, anim, walk, animtable;
    std::vector<std::string> shapeNames;  // its shape keys, and their weights in the file
    std::vector<float> shapeWeights;
    std::vector<streambundle::Entry> textures;
    size_t vertices = 0, triangles = 0, bones = 0;
    float height = 0;
};

// The NPC's skinned model on its own skeleton (feet at 0, centred), its textures and its idle animation.
Model makeModel(const Npc& n, const std::string& stem, const TextureMaker& texture, const Say& say) {
    const std::string ext = lower(n.model.extension().string());
    // What `exclude` names is picked out as the loaders pick out water, then left out.
    gltfobj::SceneMesh src = ext == ".obj"   ? gltfobj::objScene(n.model, n.smoothDegrees, n.exclude)
                             : ext == ".dae" ? gltfobj::daeScene(n.model, n.smoothDegrees, n.exclude)
                                             : gltfobj::scene(readFile(n.model), n.smoothDegrees, n.model.parent_path(), n.exclude);
    for (const std::string& s : src.warnings) say("warning: " + s);
    if (!src.water.empty()) {
        gltfobj::SceneMesh kept = src;
        kept.positions.clear(), kept.normals.clear(), kept.colours.clear(), kept.uvs.clear();
        kept.indices.clear(), kept.triangleImage.clear(), kept.triangleWater.clear(), kept.water.clear(), kept.vertexShapes.clear();
        std::vector<uint32_t> at(src.positions.size(), UINT32_MAX);
        size_t left = 0;
        for (size_t t = 0; t < src.indices.size() / 3; ++t) {
            if (t < src.triangleWater.size() && src.triangleWater[t] >= 0) {
                ++left;
                continue;
            }
            for (size_t c = 0; c < 3; ++c) {
                const uint32_t i = src.indices[3 * t + c];
                if (at[i] == UINT32_MAX) {
                    at[i] = uint32_t(kept.positions.size());
                    kept.positions.push_back(src.positions[i]);
                    kept.normals.push_back(src.normals[i]);
                    kept.colours.push_back(src.colours[i]);
                    kept.uvs.push_back(src.uvs[i]);
                    if (!src.vertexShapes.empty()) kept.vertexShapes.push_back(src.vertexShapes[i]);
                }
                kept.indices.push_back(at[i]);
            }
            kept.triangleImage.push_back(t < src.triangleImage.size() ? src.triangleImage[t] : -1);
        }
        std::string names;
        for (const auto& w : src.water) names += (names.empty() ? "" : ", ") + w.name;
        say("left out " + std::to_string(left) + " triangles: " + names);
        src = std::move(kept);
    }
    if (src.positions.empty() || src.indices.size() < 3) fail(n.model.string() + " has no triangles");
    std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (auto& p : src.positions)
        for (size_t k = 0; k < 3; ++k) {
            p[k] *= n.scale;
            lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
        }
    // The actor stands on its origin: feet at 0, the middle above it.
    const float cx = (lo[0] + hi[0]) / 2, cz = (lo[2] + hi[2]) / 2;
    for (auto& p : src.positions) p = {p[0] - cx, p[1] - lo[1], p[2] - cz};
    for (auto& vertex : src.vertexShapes)
        for (blockmodel::ShapeDelta& d : vertex)
            for (float& c : d.position) c *= n.scale;
    for (auto& c : src.colours)
        for (size_t k = 0; k < 3; ++k) c[k] = std::min(1.0f, c[k] * n.colourScale);

    if (n.rig != "auto") fail("the rig \"" + n.rig + "\" is not known (\"auto\" makes one by height)");
    const npcmodel::Rig rig = npcmodel::autoRig(src.positions);
    std::vector<std::array<uint8_t, 4>> boneOf(src.positions.size()), weightOf(src.positions.size());
    for (size_t i = 0; i < src.positions.size(); ++i) npcmodel::skinWeights(rig, src.positions[i], boneOf[i], weightOf[i]);

    // The textures: each image a triangle shows, and a small white one for the triangles without.
    Model m;
    m.triangles = src.indices.size() / 3;
    m.bones = rig.bones.size();
    m.height = hi[1] - lo[1];
    std::vector<int> textureOf(src.images.size() + 1, -1);
    std::vector<std::string> textureNames;
    std::vector<uint32_t> triangleTexture(m.triangles);
    for (size_t t = 0; t < m.triangles; ++t) {
        const int image = t < src.triangleImage.size() ? src.triangleImage[t] : -1;
        const size_t key = image >= 0 && size_t(image) < src.images.size() ? size_t(image) : src.images.size();
        if (textureOf[key] < 0) {
            textureOf[key] = int(textureNames.size());
            textureNames.push_back("aid_texture_" + stem + "_" + (key < src.images.size() ? std::to_string(key) : std::string("white")));
        }
        triangleTexture[t] = uint32_t(textureOf[key]);
    }
    for (size_t key = 0; key < textureOf.size(); ++key) {
        if (textureOf[key] < 0) continue;
        const std::string& name = textureNames[size_t(textureOf[key])];
        image::Rgba white;
        white.width = white.height = 4;
        white.pixels.assign(4 * 4 * 4, 255);
        const bool none = key == src.images.size();
        std::string summary;
        m.textures.push_back({assetId(name), texture(name, none ? white : src.images[key], summary)});
        say(name + ": " + (none ? std::string("white, for what shows no texture") : src.imageNames[key]) + ", " + summary);
    }

    // The draws: each texture's triangles, in draws of at most 65535 vertices.
    std::vector<npcmodel::SkinDraw> draws;
    std::vector<uint32_t> local(src.positions.size(), UINT32_MAX);
    for (uint32_t tex = 0; tex < textureNames.size(); ++tex) {
        npcmodel::SkinDraw dr;
        dr.texture = tex;
        auto finish = [&] {
            if (!dr.indices.empty()) draws.push_back(std::move(dr));
            dr = npcmodel::SkinDraw{};
            dr.texture = tex;
            std::fill(local.begin(), local.end(), UINT32_MAX);
        };
        for (size_t t = 0; t < m.triangles; ++t) {
            if (triangleTexture[t] != tex) continue;
            size_t fresh = 0;
            for (size_t c = 0; c < 3; ++c) fresh += local[src.indices[3 * t + c]] == UINT32_MAX ? 1 : 0;
            if (dr.vertices.size() + fresh > 65535) finish();
            for (size_t c = 0; c < 3; ++c) {
                const uint32_t i = src.indices[3 * t + c];
                if (local[i] == UINT32_MAX) {
                    local[i] = uint32_t(dr.vertices.size());
                    npcmodel::SkinVertex sv;
                    sv.v = {src.positions[i], src.normals[i], src.colours[i], src.uvs[i]};
                    sv.bones = boneOf[i];
                    sv.weights = weightOf[i];
                    if (i < src.vertexShapes.size()) sv.shapes = src.vertexShapes[i];
                    dr.vertices.push_back(sv);
                }
                dr.indices.push_back(local[i]);
            }
        }
        finish();
    }
    for (const npcmodel::SkinDraw& dr : draws) m.vertices += dr.vertices.size();
    // Its shape keys: a table of its own, its model's (renut.shapes; npc:set_shape in Lua).
    m.shapeNames = src.vertexShapes.empty() ? std::vector<std::string>{} : src.shapeNames;
    m.shapeWeights = src.shapeWeights;
    const blockmodel::ShapeTable shapes{assetId("aid_model_" + stem), m.shapeNames};
    m.model = npcmodel::writeSkinnedModel("aid_model_" + stem, rig, draws, textureNames, m.shapeNames.empty() ? nullptr : &shapes);

    // Its one animation loops while it stands, and plays once for the moments it reacts.
    const std::string anim = "aid_anim_" + stem + "_idle";
    m.anim = npcmodel::writeIdleAnim(anim, rig, n.idle, 4.0f, n.armsDown);
    // A walk, a stride a second, for Lua moving it (npc:play("walk")).
    const std::string walk = "aid_anim_" + stem + "_walk";
    m.walk = npcmodel::writeIdleAnim(walk, rig, "walk", 1.0f, n.armsDown);
    m.animtable = npcmodel::writeAnimtable("aid_animtable_" + stem, {{"stand", anim, true},
                                                                      {"walk", walk, true},
                                                                      {"run", walk, true},
                                                                      {"idle1", anim, false},
                                                                      {"idle2", anim, false},
                                                                      {"hit", anim, false},
                                                                      {"leapoutofway", anim, false},
                                                                      // Dialogs play talk anims on layers 1-3 (sub_823EC858), but
                                                                      // sub_8229BD00 sizes a talker's layers from this table's highest.
                                                                      {"renut_talklayers", anim, false, 3}});
    return m;
}

// ---- Assets ------------------------------------------------------------------------------------

// A dialog of one line: its header (a record of type 1, the loctext the text is in, the flags of the
// game's one-line dialogs), the line (type 2: speaker, listener, its text's key) and the end.
Bytes dialog(int speaker, const std::string& key) {
    Writer w;
    w.u32(1);
    w.u32(0x38);
    w.u32(assetId(kDialogLoctext));
    for (uint32_t v : {1u, 0u, 1u, 1u, 0u, 2u, 1u, 1u, 0u, 0u, 0u}) w.u32(v);
    w.u32(2);
    w.u32(0x68);
    w.u32(uint32_t(speaker));
    w.u32(uint32_t(kListener));
    w.u32(1);
    w.u32(0);
    Bytes k(64, 0);
    putString(k, 0, 64, key);
    w.bytes(k);
    for (uint32_t v : {0u, 2u, 0u, 0u}) w.u32(v);
    w.u32(0);
    w.u32(8);
    return w.buf();
}

// The body's state table {state, state objparams, move objparams, animation}: the game's common states
// and the NPC's own stand and idle, sorted by state as the game's are.
Bytes stateTable(const std::string& stand, const std::string& idle) {
    const std::string move = "aid_objparams_banjox_actormove_common_xzgravityex";
    struct Row {
        const char* state;
        std::string params, move;
        const char* anim;
    };
    const Row rows[] = {
        {"actorBodyStateId_Anim2Stand", "aid_objparams_banjox_actorstate_common_anim2stand", move, ""},
        {"actorBodyStateId_Dialog", "aid_objparams_banjox_actorstate_common_dialog", move, ""},
        {"actorBodyStateId_Frozen", "aid_objparams_banjox_actorstate_common_frozen", "", ""},
        {"actorBodyStateId_Hit", "aid_objparams_banjox_actorstate_common_hit", move, "hit"},
        {"actorBodyStateId_LeapOutOfWay", "aid_objparams_banjox_actorstate_common_leapoutofwaynogetup", move, "leapoutofway"},
        {"actorBodyStateId_Stand", stand, move, "stand"},
        {"actorBodyStateId_StandIdle", idle, move, ""},
    };
    Writer w;
    for (const Row& r : rows) {
        Bytes s(64, 0), a(64, 0);
        putString(s, 0, 64, r.state);
        putString(a, 0, 64, r.anim);
        w.bytes(s);
        w.u32(assetId(r.params));
        w.u32(r.move.empty() ? 0 : assetId(r.move));
        w.bytes(a);
    }
    return w.buf();
}

// The "mirror" behaviour's Lua: the enemy copies Banjo as his mirror image, across the plane halfway
// between where they were as the world began. Written once: it is the modder's to change.
std::string mirrorScript(const std::string& worldId, const Npc& n) {
    std::string s = "-- " + n.name + "'s behaviour: copies what Banjo does, as his mirror image. NutTool wrote it once\n"
                    "-- (renut-world --force writes it anew); change it as you like. reNut's\n"
                    "-- docs/tutorials/characters-and-challenges.md describes renut.npcs.on, NPC objects and renut.player.\n\n";
    s += "local WORLD, ID = " + luaString(worldId) + ", " + luaString(n.id) + "\n";
    s += R"lua(local STAND_SPEED = 0.5   -- metres a second: slower than this it stands

-- The mirror lies halfway between where Banjo and it are as the world starts, facing from Banjo to it.
local home, banjo_start, normal, walking

local function mine(npc)
  return npc.world == WORLD and npc.id == ID
end

-- A direction mirrored: what goes towards the mirror comes back from it.
local function mirrored(v)
  return v - normal * (2 * v:dot(normal))
end

renut.npcs.on("spawn", function(npc)
  if mine(npc) then
    home, banjo_start, walking = npc:position(), nil, nil
  end
end)

renut.npcs.on("tick", function(npc, dt)
  if not mine(npc) or not home then return end
  local banjo = renut.player.position()
  if not banjo then return end
  if not banjo_start then
    banjo_start = banjo
    local across = home - banjo
    normal = renut.vec(across.x, 0, across.z):normalized()
    if normal:length() == 0 then normal = renut.vec(0, 0, 1) end
    return
  end

  -- Where it stands: its start, moved as Banjo has moved, mirrored.
  local moved = mirrored(banjo - banjo_start)
  local before = npc:position()
  local target = renut.vec(home.x + moved.x, before.y, home.z + moved.z)
  if (target - before):length() > 0.005 then   -- only when it moves: standing, it's left alone
    npc:set_position(target)
  end

  -- Which way it faces: Banjo's way, mirrored.
  local yaw = math.rad(renut.player.yaw())
  local facing = mirrored(renut.vec(math.sin(yaw), 0, math.cos(yaw)))
  local turn = math.deg(math.atan(facing.x, facing.z))
  if math.abs((turn - npc:yaw() + 180) % 360 - 180) > 0.5 then
    npc:set_yaw(turn)
  end

  -- Walking while it moves, standing when it doesn't.
  local speed = dt > 0 and (target - before):length() / dt or 0
  local moving = speed > STAND_SPEED
  if moving ~= walking then
    walking = moving
    npc:play(moving and "walk" or "stand")
  end
end)

renut.npcs.on("defeated", function(npc)
  if mine(npc) then
    print(npc.name .. " is beaten!")
  end
end)
)lua";
    return s;
}

} // namespace

const std::vector<std::string>& gameVoices() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const GameVoice& g : kGameVoices) v.push_back(g.name);
        return v;
    }();
    return names;
}

std::vector<std::string> problems(const Npc& n) {
    std::vector<std::string> out;
    if (!renutpart::validId(n.id)) out.push_back("an id is 1-40 characters of a-z, 0-9 and _");
    if (n.model.empty()) out.push_back("it needs a model (.dae, .glb or .obj)");
    if (!(n.scale > 0)) out.push_back("the scale is more than 0");
    if (n.rig != "auto") out.push_back("the rig \"" + n.rig + "\" is not known (\"auto\" makes one by height)");
    if (n.idle != "breathe" && !endsWith(lower(n.idle), ".json")) out.push_back("the idle is \"breathe\" or a timeline .json file");
    if (!(n.armsDown >= 0 && n.armsDown <= 90)) out.push_back("armsDown is 0 to 90 degrees");
    if (!validTag(n.tag)) out.push_back("the tag is 1-63 characters of a-z, 0-9 and _");
    if (n.speaker < 1 || n.speaker > 32) out.push_back("the speaker slot is 1-32 (dbCharData_Id; 0 is Banjo)");
    for (const auto& [k, v] : n.lines)
        if (std::find(kLines.begin(), kLines.end(), k) == kLines.end())
            out.push_back("the line \"" + k + "\" is not one of greeting, objectives, win, lose, completed");
    auto has = [&](const std::string& k) {
        auto it = n.lines.find(k);
        return it != n.lines.end() && !trim(it->second).empty();
    };
    if (!has("greeting") && !n.enemy) out.push_back("it needs a \"greeting\" line");
    if (n.enemy) {
        if (n.challenge) out.push_back("an enemy gives no challenge");
        if (n.enemy->health < 1 || n.enemy->health > 1000) out.push_back("an enemy's health is a whole number from 1 to 1000");
        if (n.enemy->behaviour != "mirror" && n.enemy->behaviour != "none")
            out.push_back("an enemy's behaviour is \"mirror\" (copies Banjo) or \"none\" (your own Lua)");
    }
    if (n.challenge) {
        const Challenge& ch = *n.challenge;
        if (!renutpart::validId(ch.id)) out.push_back("its challenge's id is 1-40 characters of a-z, 0-9 and _");
        if (trim(ch.name).empty()) out.push_back("its challenge needs a name");
        if (!(ch.tt > 0 && ch.tt <= ch.jiggy && ch.jiggy <= ch.timeLimit))
            out.push_back("its challenge's times are 0 < tt <= jiggy <= timeLimit seconds");
        if (!(ch.gate[0] > 0 && ch.gate[1] > 0)) out.push_back("its challenge's gate width and height are more than 0");
        if (ch.notes < 1 || ch.notes > 1000) out.push_back("its challenge's \"notes\" is a whole number from 1 to 1000");
        for (const std::string& l : kLines)
            if (l != "greeting" && !has(l)) out.push_back("with a challenge it needs every line (missing \"" + l + "\")");
    }
    const std::vector<std::string>& voices = gameVoices();
    if (!n.voice.empty() && !n.voiceClips.empty()) out.push_back("the voice is a game character's or sound files, not both");
    if (!n.voice.empty() && std::find(voices.begin(), voices.end(), n.voice) == voices.end())
        out.push_back("the voice \"" + n.voice + "\" is not a game character's (humba, mumbo, klungo, ...)");
    if (!(n.voiceVolume >= 0 && n.voiceVolume <= 1)) out.push_back("the voice volume is from 0 to 1");
    for (const fs::path& f : n.voiceClips) {
        const auto u = f.filename().u8string();
        if (!isSoundFile(f)) out.push_back("a voice clip is an .ogg, .mp3, .flac or .wav file (" + std::string(u.begin(), u.end()) + ")");
    }
    for (const auto& [k, f] : n.sounds) {
        if (std::find(kLines.begin(), kLines.end(), k) == kLines.end())
            out.push_back("the sound for \"" + k + "\" is not for one of greeting, objectives, win, lose, completed");
        else if (!isSoundFile(f))
            out.push_back("the " + k + " sound is an .ogg, .mp3, .flac or .wav file");
    }
    return out;
}

namespace {

// A float as JSON, rounded to what a float holds (0.0374, not 0.037399999797).
Json number(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.7g", double(v));
    return Json(std::strtod(b, nullptr));
}

template <size_t N>
Json numbers(const std::array<float, N>& v) {
    Json a = Json::array();
    for (float f : v) a.push(number(f));
    return a;
}

} // namespace

std::string toJson(const std::vector<Npc>& npcs, const fs::path& json) {
    std::error_code ec;
    const fs::path dir = fs::absolute(json, ec).parent_path().lexically_normal();
    // UTF-8, as load reads it: relative when under the file's folder, else absolute.
    auto pathText = [&](const fs::path& p) {
        fs::path abs = fs::absolute(p, ec).lexically_normal();
        if (ec) abs = p;
        fs::path rel = abs.lexically_relative(dir);
        const bool under = !rel.empty() && *rel.begin() != "..";
        auto u = (under ? rel : abs).generic_u8string();
        return std::string(u.begin(), u.end());
    };
    Json list = Json::array();
    for (const Npc& n : npcs) {
        Json o = Json::object();
        o["id"] = n.id;
        o["name"] = n.name;
        o["model"] = pathText(n.model);
        o["scale"] = number(n.scale);
        o["smooth"] = number(n.smoothDegrees);
        o["colourScale"] = number(n.colourScale);
        o["position"] = numbers(n.position);
        o["yaw"] = number(n.yaw);
        o["rig"] = n.rig;
        o["idle"] = n.idle == "breathe" ? n.idle : pathText(fs::path(n.idle));
        if (!n.exclude.empty()) {
            Json ex = Json::array();
            for (const std::string& e : n.exclude) ex.push(Json(e));
            o["exclude"] = ex;
        }
        if (n.armsDown != 0) o["armsDown"] = number(n.armsDown);
        if (!n.shapeWeights.empty()) {
            Json shapes = Json::object();
            for (const auto& [name, weight] : n.shapeWeights) shapes[name] = number(weight);
            o["shapes"] = shapes;
        }
        o["tag"] = n.tag;
        o["speaker"] = n.speaker;
        Json lines = Json::object();
        for (const std::string& k : kLines) {
            auto it = n.lines.find(k);
            if (it != n.lines.end() && !it->second.empty()) lines[k] = it->second;
        }
        o["lines"] = lines;
        if (!n.voice.empty()) o["voice"] = n.voice;
        if (!n.voiceClips.empty()) {
            Json clips = Json::array();
            for (const fs::path& f : n.voiceClips) clips.push(Json(pathText(f)));
            o["voice"] = clips;
        }
        if (!n.voice.empty() || !n.voiceClips.empty() || !n.sounds.empty()) o["voiceVolume"] = number(n.voiceVolume);
        if (!n.sounds.empty()) {
            Json sounds = Json::object();
            for (const auto& [k, f] : n.sounds) sounds[k] = pathText(f);
            o["sounds"] = sounds;
        }
        if (n.challenge) {
            const Challenge& ch = *n.challenge;
            Json c = Json::object();
            c["id"] = ch.id;
            c["name"] = ch.name;
            c["summary"] = ch.summary;
            c["goal"] = numbers(ch.goal);
            c["goalYaw"] = number(ch.goalYaw);
            c["gate"] = numbers(ch.gate);
            c["timeLimit"] = number(ch.timeLimit);
            c["tt"] = number(ch.tt);
            c["jiggy"] = number(ch.jiggy);
            c["notes"] = ch.notes;
            c["countDown"] = ch.countDown;
            if (ch.start) c["start"] = numbers(*ch.start);
            if (ch.startYaw) c["startYaw"] = number(*ch.startYaw);
            o["challenge"] = c;
        }
        if (n.enemy) {
            Json e = Json::object();
            e["health"] = n.enemy->health;
            e["behaviour"] = n.enemy->behaviour;
            o["enemy"] = e;
        }
        list.push(o);
    }
    Json root = Json::object();
    root["npcs"] = list;
    return root.dump(2);
}

std::vector<Npc> load(const fs::path& file) {
    const Json j = Json::parse(readText(file));
    if (!j.has("npcs") || j["npcs"].type() != Json::Array) fail(file.string() + ": it holds {\"npcs\": [ ... ]}");
    const fs::path dir = file.parent_path();
    // Paths are UTF-8, as JSON is.
    auto beside = [&](const std::string& p) {
        fs::path f = std::u8string(p.begin(), p.end());
        return f.is_relative() ? dir / f : f;
    };
    std::vector<Npc> out;
    for (size_t i = 0; i < j["npcs"].size(); ++i) {
        const Json& o = j["npcs"][i];
        Npc n;
        n.id = lower(str(o, "id", "npc " + std::to_string(i + 1), true));
        const std::string where = "npc " + n.id;
        if (!renutpart::validId(n.id)) fail(where + ": an id is 1-40 characters of a-z, 0-9 and _");
        for (const Npc& other : out)
            if (other.id == n.id) fail(where + ": two NPCs have this id");
        n.name = str(o, "name", where, false, n.id);
        n.model = beside(str(o, "model", where, true));
        n.scale = num(o, "scale", where, 1.0f);
        n.smoothDegrees = num(o, "smooth", where, 40.0f);
        n.colourScale = num(o, "colourScale", where, 1.0f);
        n.position = vec<3>(o, "position", where);
        n.yaw = num(o, "yaw", where, 0.0f);
        n.rig = str(o, "rig", where, false, "auto");
        n.idle = str(o, "idle", where, false, "breathe");
        if (endsWith(lower(n.idle), ".json")) n.idle = beside(n.idle).string();
        if (o.has("exclude") && !o["exclude"].isNull()) {
            const Json& ex = o["exclude"];
            if (ex.type() != Json::Array) fail(where + ": \"exclude\" is [\"part of a name\", ...]");
            for (size_t k = 0; k < ex.size(); ++k) {
                if (ex[k].type() != Json::String) fail(where + ": \"exclude\" is [\"part of a name\", ...]");
                if (!trim(ex[k].asString()).empty()) n.exclude.push_back(trim(ex[k].asString()));
            }
        }
        n.armsDown = num(o, "armsDown", where, 0.0f);
        if (o.has("shapes") && !o["shapes"].isNull()) {
            if (o["shapes"].type() != Json::Object) fail(where + ": \"shapes\" is {\"shape key\": weight, ...}");
            for (const auto& [k, v] : o["shapes"].members()) {
                if (v.type() != Json::Number) fail(where + ": the shape key \"" + k + "\"'s weight is a number");
                n.shapeWeights[k] = float(v.asNumber());
            }
        }
        n.tag = str(o, "tag", where, true);
        n.speaker = int(num(o, "speaker", where, 32.0f));
        if (o.has("lines") && !o["lines"].isNull()) {
            if (o["lines"].type() != Json::Object) fail(where + ": \"lines\" is {\"greeting\": \"...\", ...}");
            for (const auto& [k, v] : o["lines"].members()) {
                if (v.type() != Json::String) fail(where + ": the line \"" + k + "\" is a string");
                n.lines[k] = v.asString();
            }
        }
        if (o.has("voice") && !o["voice"].isNull()) {
            const Json& v = o["voice"];
            if (v.type() == Json::String) {
                n.voice = lower(v.asString());
            } else if (v.type() == Json::Array) {
                for (size_t k = 0; k < v.size(); ++k) {
                    if (v[k].type() != Json::String) fail(where + ": \"voice\" is a game character's name or [\"sound file\", ...]");
                    n.voiceClips.push_back(beside(v[k].asString()));
                }
            } else {
                fail(where + ": \"voice\" is a game character's name or [\"sound file\", ...]");
            }
        }
        n.voiceVolume = num(o, "voiceVolume", where, 0.9f);
        if (o.has("sounds") && !o["sounds"].isNull()) {
            if (o["sounds"].type() != Json::Object) fail(where + ": \"sounds\" is {\"greeting\": \"sound file\", ...}");
            for (const auto& [k, v] : o["sounds"].members()) {
                if (v.type() != Json::String) fail(where + ": the sound for \"" + k + "\" is a file's path");
                n.sounds[k] = beside(v.asString());
            }
        }
        if (o.has("challenge") && !o["challenge"].isNull()) {
            const Json& c = o["challenge"];
            Challenge ch;
            ch.id = lower(str(c, "id", where + "'s challenge", true));
            const std::string cw = where + "'s challenge " + ch.id;
            ch.name = str(c, "name", cw, true);
            ch.summary = str(c, "summary", cw, false);
            ch.goal = vec<3>(c, "goal", cw);
            ch.goalYaw = num(c, "goalYaw", cw, 0.0f);
            ch.gate = vec<2>(c, "gate", cw, std::array<float, 2>{16, 26});
            ch.timeLimit = num(c, "timeLimit", cw, 60.0f);
            ch.countDown = c.has("countDown") && c["countDown"].type() == Json::Bool && c["countDown"].asBool();
            if (c.has("start") && !c["start"].isNull()) ch.start = vec<3>(c, "start", cw);
            if (c.has("startYaw") && !c["startYaw"].isNull()) ch.startYaw = num(c, "startYaw", cw, 0.0f);
            ch.tt = num(c, "tt", cw, std::nullopt);
            ch.jiggy = num(c, "jiggy", cw, std::nullopt);
            const float notes = num(c, "notes", cw, 1.0f);
            if (!(notes >= 1 && notes <= 1000 && notes == std::floor(notes))) fail(cw + ": \"notes\" is a whole number from 1 to 1000");
            ch.notes = uint32_t(notes);
            n.challenge = ch;
        }
        if (o.has("enemy") && !o["enemy"].isNull()) {
            const Json& e = o["enemy"];
            if (e.type() != Json::Object) fail(where + ": \"enemy\" is {\"health\": 3, \"behaviour\": \"mirror\"}");
            Enemy en;
            const float health = num(e, "health", where + "'s enemy", 3.0f);
            if (!(health >= 1 && health <= 1000 && health == std::floor(health))) fail(where + ": an enemy's health is a whole number from 1 to 1000");
            en.health = int(health);
            en.behaviour = lower(str(e, "behaviour", where + "'s enemy", false, "mirror"));
            n.enemy = en;
        }
        const std::vector<std::string> wrong = problems(n);
        if (!wrong.empty()) fail(where + ": " + wrong.front());
        out.push_back(std::move(n));
    }
    if (out.empty()) fail(file.string() + ": it lists no NPCs");
    return out;
}

Built build(const fs::path& assets, const std::string& worldId, const std::string& worldName, const std::vector<Npc>& npcs,
            uint16_t firstMarkerId, const TextureMaker& texture, const Say& say) {
    Built out;
    GameBundle town, common;
    uint16_t markerId = firstMarkerId;
    uint32_t challenges = 0;
    std::string givers, talks;
    const std::string jiggies = "renut_" + worldId + "_jiggies";
    const std::string worldKey = fit31("renutworld_" + worldId, "rnw_" + worldId, "rnw_");
    if (jiggies.size() > 63) fail("the world id is too long for its jiggy counter's name");

    for (const Npc& n : npcs) {
        const std::string stem = "renutnpc_" + worldId + "_" + n.id;
        say(n.name + " (" + stem + ")");
        auto add = [&](const std::string& name, const Bytes& caffFile) { out.stream.push_back({assetId(name), caffFile}); };

        // Model, textures and animations.
        const Model m = makeModel(n, stem, texture, say);
        add("aid_model_" + stem, m.model);
        add("aid_anim_" + stem + "_idle", m.anim);
        add("aid_anim_" + stem + "_walk", m.walk);
        add("aid_animtable_" + stem, m.animtable);
        for (const auto& t : m.textures) out.textures.push_back(t);
        char made[160];
        std::snprintf(made, sizeof made, ": %zu vertices, %zu triangles, %.2f m tall, %zu bones, idle \"%s\"", m.vertices, m.triangles,
                      double(m.height), m.bones, fs::path(n.idle).filename().string().c_str());
        say("aid_model_" + stem + made);

        // The actor: Pikelet's, with the NPC's model, animations, body and tag, and the game's common callouts.
        const caff::Caff& tc = town.get(assets, kTownBundle);
        const std::string actor = "aid_objparams_" + stem, body = actor + "_body", stand = actor + "_stand", idle = actor + "_idle";
        const std::string statetable = "aid_statetable_" + stem, chardata = "aid_chardata_" + stem;
        Bytes a = templateData(tc, town.path, kActorTemplate, 636);
        putString(a, kActorTag, 64, n.tag);
        putBe32(&a[kActorModel], assetId("aid_model_" + stem));
        putBe32(&a[kActorAnimtable], assetId("aid_animtable_" + stem));
        putBe32(&a[kActorCallout], assetId("aid_callout_banjox_actor_default"));
        putBe32(&a[kActorBody], assetId(body));
        putString(a, kActorIndicator, 64, "sceneIndicatorType_Null");
        putString(a, kActorTag2, 64, n.tag);
        add(actor, oneAsset(actor, a));
        Bytes b = templateData(tc, town.path, kBodyTemplate, 1032);
        putBe32(&b[kBodyStatetable], assetId(statetable));
        add(body, oneAsset(body, b));
        add(stand, oneAsset(stand, templateData(tc, town.path, kStandTemplate, 1404)));
        add(idle, oneAsset(idle, templateData(tc, town.path, kIdleTemplate, 1092)));
        add(statetable, oneAsset(statetable, stateTable(stand, idle)));

        // Its chardata: Humba's dialog camera, its own actor, every animation its idle, and the voice cues of
        // the game character it sounds like (none with its own clips, or no voice).
        Bytes cd = templateData(tc, town.path, kCharTemplate, 264);
        const uint32_t animId = assetId("aid_anim_" + stem + "_idle");
        putBe32(&cd[kCharActor], assetId(actor));
        for (uint32_t k = 0; k < kCharAnimCount; ++k) putBe32(&cd[kCharAnims + 4 * k], animId);
        for (uint32_t k = 0; k < kCharMoodCount; ++k) {
            putBe32(&cd[kCharMoods + 12 * k + 4], animId);
            putBe32(&cd[kCharMoods + 12 * k + 8], animId);
        }
        std::fill(cd.begin() + kCharVoice, cd.begin() + kCharVoice + kCharVoiceSize, 0);
        for (const GameVoice& g : kGameVoices) {
            if (n.voice != g.name) continue;
            GameBundle& gb = std::string(g.bundle) == kTownBundle ? town : common;
            const Bytes voice = templateData(gb.get(assets, g.bundle), gb.path, g.chardata, 264);
            std::copy(voice.begin() + kCharVoice, voice.begin() + kCharVoice + kCharVoiceSize, cd.begin() + kCharVoice);
        }
        add(chardata, oneAsset(chardata, cd));
        say(actor + ", its body, stand and idle states, state table and " + chardata + " (Pikelet's and Humba's layouts" +
            (n.voice.empty() ? std::string(")") : ", " + n.voice + "'s voice)"));

        // Its sound files, copied to sounds\<id>\ in the mod; the Lua names them from the mod's folder.
        auto soundFile = [&](const fs::path& from) {
            std::error_code ec;
            if (!fs::is_regular_file(from, ec)) fail(n.id + ": no sound file " + from.string());
            const fs::path to = fs::path("sounds") / n.id / from.filename();
            for (const auto& [f, t] : out.files)
                if (t == to && fs::absolute(f, ec) != fs::absolute(from, ec))
                    fail(n.id + ": two sound files are named " + from.filename().string() + " (rename one)");
            out.files.push_back({from, to});
            auto u = to.generic_u8string();
            return std::string(u.begin(), u.end());
        };
        std::vector<std::string> clips;
        for (const fs::path& f : n.voiceClips) clips.push_back(soundFile(f));
        std::vector<std::pair<std::string, std::string>> sounds;
        for (const auto& [line, f] : n.sounds) sounds.push_back({line, soundFile(f)});

        // The dialogs, one line each, their text served by reNut under dialog__<key>.
        std::vector<std::pair<std::string, std::string>> text;  // Lua text keys, in order
        for (const std::string& l : kLines) {
            if (!n.lines.count(l)) continue;
            const std::string key = "renut_" + worldId + "_" + n.id + "_" + l, name = "aid_dialog_" + stem + "_" + l;
            if (key.size() > 63) fail("the dialog key " + key + " is longer than 63 characters (shorten the world or NPC id)");
            add(name, oneAsset(name, dialog(n.speaker, key)));
            text.push_back({"dialog__" + key, n.lines.at(l)});
        }

        // The challenge: Park Prix 1's records, its header made the NPC's.
        std::string talk;
        std::array<std::string, 4> flags;
        const std::string challengeAsset = "aid_challenge_renutchallenge_" + worldId + "_" + n.id;
        if (n.challenge) {
            const Challenge& ch = *n.challenge;
            if (++challenges > 12) fail("a scene registers at most 12 challenge givers");
            const caff::Caff& cc = common.get(assets, kCommonBundle);
            Bytes c = templateData(cc, common.path, kChallengeTemplate, 1796);
            if (be32(&c[0]) != 1 || be32(&c[0x0C]) != 0x60C || be32(&c[0x60C]) != 5 || be32(&c[0x60C + 0x1C]) != 1)
                fail(std::string(kChallengeTemplate) + " is not laid out as expected");
            const std::string prefix = "renut_" + worldId + "_" + ch.id;
            const std::string name = fit31(prefix, "rn_" + worldId + "_" + ch.id, "rn_");
            flags = {prefix + "_seen", prefix + "_notes", prefix + "_beaten", prefix + "_tt"};
            const uint32_t mask = 0x80000000u >> (challenges - 1);
            auto dlg = [&](const char* line) { return assetId("aid_dialog_" + stem + "_" + line); };
            putString(c, 0x010, 32, worldKey);
            putString(c, 0x030, 32, name);
            putBe32(&c[0x070], mask);  // its goal gates' marker mask
            putBe32(&c[0x0B4], assetId("aid_misc_banjox_challengesfx_default"));
            putBe32(&c[0x0BC], dlg("greeting"));
            putBe32(&c[0x0C0], dlg("completed"));
            putBe32(&c[0x0C4], dlg("objectives"));
            putBe32(&c[0x0C8], dlg("win"));
            putBe32(&c[0x0CC], dlg("lose"));
            putBe32(&c[0x0D0], dlg("objectives"));  // the game style's introduction, should its flag be unset
            putBe32(&c[0x0D4], 0);                   // no wimp-out line
            putString(c, 0x0D8, 64, flags[0]);       // seen the objectives
            putString(c, 0x158, 64, flags[1]);       // notes only
            putString(c, 0x198, 64, flags[2]);       // beaten (the jiggy)
            putString(c, 0x1D8, 64, flags[3]);       // beaten with the T.T. trophy
            putString(c, 0x218, 64, jiggies);
            putBe32(&c[0x258], ch.notes);
            putString(c, 0x25C, 64, "gameCounter_BanjoX_Null");  // no best time kept
            putBe32(&c[0x2A0], 0);
            putBe32(&c[0x2A8], 0);  // scored by time
            putBe32(&c[0x2AC], 1);  // lower is better
            putF32(&c[0x2C8], 0);
            putF32(&c[0x2CC], ch.tt);
            putF32(&c[0x2D0], ch.jiggy);
            putF32(&c[0x2D4], ch.timeLimit);
            putF32(&c[0x2EC], ch.countDown ? ch.timeLimit : 0.0f);  // 0: the clock counts up
            putBe32(&c[0x2F8], 0);
            putBe32(&c[0x2FC], 152);
            for (const auto& f : flags)
                if (f.size() > 63) fail("the flag " + f + " is longer than 63 characters (shorten the world or challenge id)");
            add(challengeAsset, oneAsset(challengeAsset, c));
            text.push_back({"challenge__" + name, ch.name});
            text.push_back({"challenge__" + name + "summary", ch.summary});
            text.push_back({"challenge__" + worldKey, worldName});
            char what[200];
            std::snprintf(what, sizeof what, ": \"%s\", to the gate within %gs (notes x%u), jiggy under %gs, T.T. trophy under %gs",
                          ch.name.c_str(), double(ch.timeLimit), unsigned(ch.notes), double(ch.jiggy), double(ch.tt));
            say(challengeAsset + what);

            // Its vehicle start (kind 21, as Park Prix's): with no vehicle named (+0x38), the challenge puts the
            // player's own vehicle there (sub_824FCFD0); without one the player starts on foot.
            std::array<float, 3> start = n.position;
            float startYaw = 0;
            {
                const float dx = ch.goal[0] - n.position[0], dz = ch.goal[2] - n.position[2];
                const float len = std::sqrt(dx * dx + dz * dz);
                if (len > 0) start = {n.position[0] + 12 * dx / len, n.position[1] + 1, n.position[2] + 12 * dz / len};
                if (ch.start) start = *ch.start;
                const float fx = ch.goal[0] - start[0], fz = ch.goal[2] - start[2];
                startYaw = ch.startYaw ? *ch.startYaw : std::atan2(fx, fz) * 180 / kPi;
            }
            Bytes v(0x5C, 0);
            putBe32(&v[0], 0x5C);
            putBe16(&v[4], 21);
            putBe16(&v[6], markerId++);
            putBe32(&v[0x0C], mask);
            for (size_t k = 0; k < 3; ++k) putF32(&v[0x14 + 4 * k], start[k]);
            putF32(&v[0x24], startYaw * kPi / 180);
            putF32(&v[0x2C], 1.0f);
            out.markerRecords.insert(out.markerRecords.end(), v.begin(), v.end());

            // Its goal gate (kind 22): the challenge's mask, a gate of the given size, checkpoint group 1.
            Bytes g(0x68, 0);
            putBe32(&g[0], 0x68);
            putBe16(&g[4], 22);
            putBe16(&g[6], markerId++);
            putBe32(&g[0x0C], mask);
            for (size_t k = 0; k < 3; ++k) putF32(&g[0x14 + 4 * k], ch.goal[k]);
            putF32(&g[0x24], ch.goalYaw * kPi / 180);
            putF32(&g[0x2C], 1.0f);
            putF32(&g[0x34], ch.gate[0]);
            putF32(&g[0x38], ch.gate[1]);
            putBe32(&g[0x3C], 2);  // the goal
            putBe32(&g[0x40], 1);
            putBe32(&g[0x48], 1);
            out.markerRecords.insert(out.markerRecords.end(), g.begin(), g.end());

            givers += "op_6F \"" + n.tag + "\" \"" + flags[2] + "\" @" + challengeAsset + " 0 0 0\n";
            // The challenge plays the greeting (or, once beaten, the completed line) and the objectives itself.
            talk = "op_60 \"" + ch.id + "\" @" + challengeAsset + " 1\nop_00\n";
        } else {
            talk = "op_11 @aid_dialog_" + stem + "_greeting 0\nop_00\n";
        }
        if (!n.enemy) {  // enemies aren't talked to
            const std::string talkName = "aid_script_" + stem + "_talk";
            add(talkName, oneAsset(talkName, gamescript::fromText(gamescript::Kind::Script, talk)));
            talks += "op_58 \"" + n.tag + "\" 0 0 0\nop_5A @" + talkName + " 1\nop_1B\n";
        }

        // The NPC (kind 6): snapped to the ground, its actor, its tag.
        Bytes r(300, 0);
        putBe32(&r[0], 300);
        putBe16(&r[4], 6);
        putBe16(&r[6], markerId++);
        for (size_t k = 0; k < 3; ++k) putF32(&r[0x14 + 4 * k], n.position[k]);
        putF32(&r[0x24], n.yaw * kPi / 180);
        putF32(&r[0x2C], 1.0f);
        putBe32(&r[0x34], 1);
        putBe32(&r[0x38], assetId(actor));
        putString(r, 0x44, 64, n.tag);
        putF32(&r[0xE4], 0.5f);
        putF32(&r[0xE8], 1.0f);
        putString(r, 0xEC, 64, "\"sceneIndicatorType_Null\"");
        out.markerRecords.insert(out.markerRecords.end(), r.begin(), r.end());

        // The Lua that gives reNut the NPC's text, tag, chardata and the challenge's names.
        std::string lua = "-- " + n.name + ": a character of " + worldName + ", made with NutTool from " + n.model.filename().string() +
                          ".\n-- Written anew by renut-world --npcs each time: edit the NPC JSON file, not this script.\n\n";
        lua += "renut.npcs.add{\n  world = " + luaString(worldId) + ",\n  id = " + luaString(n.id) + ",\n  name = " + luaString(n.name) +
               ",\n  tag = " + luaString(n.tag) + ",   -- the objTag of its marker, objparams and scene lines\n  scene = " +
               luaString("renutworld_" + worldId) + ",   -- the scene it stands in\n  chardata = " +
               luaString(stem) + ",\n  speaker = " + std::to_string(n.speaker) +
               ",   -- the dbCharData_Id slot its dialogs name as their speaker\n  text = {\n";
        for (const auto& [k, v] : text) lua += "    [" + luaString(k) + "] = " + luaString(v) + ",\n";
        lua += "  },\n";
        if (n.challenge) {
            lua += "  challenge = {\n    id = " + luaString(n.challenge->id) + ",\n    asset = " +
                   luaString("renutchallenge_" + worldId + "_" + n.id) + ",\n    flags = {" + luaString(flags[0]) + ", " + luaString(flags[1]) +
                   ", " + luaString(flags[2]) + ", " + luaString(flags[3]) + "},   -- seen, notes, beaten, T.T. trophy\n    jiggies = " +
                   luaString(jiggies) + ",\n    notes = " + std::to_string(10 * n.challenge->notes) +
                   ",   -- the notes it pays, for the world door's stats\n  },\n";
        }
        if (!n.voice.empty()) lua += "  voice = " + luaString(n.voice) + ",   -- the game character it sounds like\n";
        if (!clips.empty()) {
            lua += "  voice = {";
            for (size_t k = 0; k < clips.size(); ++k) lua += (k ? ", " : "") + luaString(clips[k]);
            lua += "},   -- one at random as each page starts\n";
        }
        if (!clips.empty() || !sounds.empty()) {
            char volume[32];
            std::snprintf(volume, sizeof volume, "%.3g", double(n.voiceVolume));
            lua += std::string("  voice_volume = ") + volume + ",\n";
        }
        if (!sounds.empty()) {
            lua += "  sounds = {   -- played as the line starts, instead of the voice\n";
            for (const auto& [line, file] : sounds) lua += "    " + line + " = " + luaString(file) + ",\n";
            lua += "  },\n";
        }
        if (n.enemy)
            lua += "  enemy = {health = " + std::to_string(n.enemy->health) + "},   -- hits it takes; its behaviour is " +
                   (n.enemy->behaviour == "mirror" ? stem + "_ai.lua" : std::string("yours to script")) + "\n";
        lua += "}\n";
        // Its shape keys: their names and starting weights (npc:set_shape in Lua).
        if (!m.shapeNames.empty()) {
            std::string names, weights;
            for (size_t i = 0; i < m.shapeNames.size(); ++i) {
                const auto set = n.shapeWeights.find(m.shapeNames[i]);
                const float weight = set != n.shapeWeights.end() ? set->second : i < m.shapeWeights.size() ? m.shapeWeights[i] : 0.0f;
                char value[32];
                std::snprintf(value, sizeof value, "%g", double(weight));
                names += (i ? ", " : "") + luaString(m.shapeNames[i]);
                weights += (i ? ", " : "") + std::string(value);
            }
            lua += "\nrenut.shapes.define{\n  model = " + luaString(stem) + ",   -- its shape keys: npc:set_shape(name, weight)\n  shapes = {" +
                   names + "},\n  weights = {" + weights + "},\n}\n";
            say(std::to_string(m.shapeNames.size()) + " shape key(s)");
        }
        for (const auto& [name, weight] : n.shapeWeights)
            if (std::find(m.shapeNames.begin(), m.shapeNames.end(), name) == m.shapeNames.end())
                say("warning: " + n.id + " has no shape key \"" + name + "\" to set");
        out.scripts.push_back({fs::path("scripts") / (stem + ".lua"), lua});
        if (n.enemy && n.enemy->behaviour == "mirror")
            out.scripts.push_back({fs::path("scripts") / (stem + "_ai.lua"), mirrorScript(worldId, n)});
    }
    if (!givers.empty()) givers += "op_71\n";
    out.sceneText = givers + talks;
    return out;
}

} // namespace nut::renutnpc
