// A world of a reNut mod's own, from a .glb, .obj or .dae: its model, textures, sky, collision, marker
// and scene script, and the Lua that declares it and puts its door in Showdown Town. CLI and GUI share it.
#pragma once

#include "caff.hpp"
#include "common.hpp"
#include "image.hpp"
#include "streambundle.hpp"

#include <array>
#include <functional>
#include <map>
#include <optional>

namespace nut::renutworld {

using Say = std::function<void(const std::string&)>;

// A material's settings over the model's own (each set one replaces it): how its alpha shows ("opaque",
// "cutout", "blend", "add"), cutoff, opacity, emissive (sRGB), its strength and texture, glow, faces.
struct MaterialSettings {
    std::optional<std::string> alpha;
    std::optional<float> cutoff, opacity, strength, glow;
    std::optional<std::array<float, 3>> emissive;
    std::optional<fs::path> emissiveTexture;  // an image file; an empty path takes the model's away
    std::optional<bool> doubleSided, unlit;
    std::optional<bool> solid;  // in the collision; unset: unless it adds (light shafts, glow)
};

// A materials file, {"materials": {"<name>": {"alpha": "blend", "emissive": "ffcc00", ...}}} as
// reNut's docs/nuttool/worlds.md lays it out, its texture paths beside it; and the same written back.
std::map<std::string, MaterialSettings> loadMaterials(const fs::path& json);
std::string materialsJson(const std::map<std::string, MaterialSettings>& materials, const fs::path& json);

// A model's materials as its file gives them (names, then each one's settings, all set), for an editor.
std::vector<std::pair<std::string, MaterialSettings>> modelMaterials(const fs::path& model);

struct World {
    std::string id;                               // 1-40 characters of a-z, 0-9 and _
    std::string name;                             // shown to players; empty: the id
    fs::path model;                               // the .glb (every mesh node in it, in world space), .obj or .dae
    std::vector<fs::path> moreModels;             // more of the world in the same space (a level's other rooms)
    std::optional<std::array<float, 3>> spawn;    // the player's start; empty: 2 m above ground nearest the middle
    float spawnYaw = 0;                           // degrees
    std::optional<std::array<float, 3>> exit;     // the door back to Showdown Town; empty: on ground 10-15 m from the start
    std::optional<float> exitYaw;                 // degrees; empty: facing the start when `exit` is found, else 0
    bool exitDoor = true;                         // false: no door home (the world is left from Lua, renut.worlds.go)
    std::array<float, 3> door{-45.8f, 0.8f, 315.8f};  // the world's door in Showdown Town (beside Mumbo's Motors')
    float doorYaw = 92;
    // The door's kind: "wall" (the world door, one-sided: against a wall), "arch" (the stone arch home,
    // two-sided: free-standing), "palace" (L.O.G.'s palace door) or "hidden" (no model: a doorway already there).
    std::string doorStyle = "wall";
    bool townDoor = true;                         // false: no door in Showdown Town (reached with renut.worlds.go)
    float smoothDegrees = 40;                     // faces meeting at less than this share a normal
    float scale = 1;                              // the model's size, times this
    float ground = 0;                             // a flat square ground this wide under the model; 0: none
    std::array<float, 3> groundColour{0.45f, 0.58f, 0.35f};  // sRGB
    std::string bundle;                           // the mod's stream bundle for its scene; empty: the mod folder's name
    fs::path hologram;                            // an image the door's hologram shows (wall and arch doors); empty: the game's
    float colourScale = 1;                        // the vertex colours times this (2 for 3DS rips, whose colours are halved)
    uint32_t maxTexture = 1024;                   // textures' largest side (a power of two): lower for worlds with many textures
    fs::path music;                               // .ogg/.mp3/.flac/.wav it loops while played in (copied into the mod); empty: none
    float musicVolume = 0.8f;                     // 0 to 1
    std::string playerModel;                      // the player model Banjo is made with there (renut-player-model's id); empty: his own
    // The totals its door's stats panel shows (jiggies, T.T. trophies, Jinjo tokens, notes); -1: reNut
    // counts them from the world's challenges.
    std::array<int, 4> statTotals{-1, -1, -1, -1};
    // What else is water, besides objects named with "_water": parts of object, material or texture
    // names (any case), for models whose objects can't be renamed.
    std::vector<std::string> waterNames;
    // Shape keys' starting weights by name (0 to 1), over the model's own (glTF morph targets' weights).
    std::map<std::string, float> shapeWeights;
    // The sky around the camera: a 360-degree panorama image (equirectangular, its top straight up), or
    // two colours "rrggbb,rrggbb" (overhead, horizon) for a gradient. Empty: none.
    std::string sky;
    // Characters of the world's own and their challenges (renutnpc.hpp's JSON). Empty: none.
    fs::path npcs;
    // Settings over the model's materials, by material name (any case): a materials file's, then these.
    fs::path materialsFile;
    std::map<std::string, MaterialSettings> materials;
};

// A CAFF of its own holding `syms` of `src` (as its symbols 0, 1...) and the pool entries they point into.
caff::Caff copyAssets(const caff::Caff& src, const std::vector<size_t>& syms);
// Makes texture assets (a CAFF each) showing an image, at most `maxSide` a side, from the world textures' template in the
// game's assets folder; `summary` says what each became.
std::function<Bytes(const std::string& name, const image::Rgba& img, std::string& summary)> textureMaker(const fs::path& assets,
                                                                                                       uint32_t maxSide = 1024);
// Renames asset `from` to `to` in `c`: its symbol names and the name that ends a model's .stream part.
void renameAssetIn(caff::Caff& c, const std::string& from, const std::string& to);
// Points the model's texture names (in its .data part `sym`) that are keys of `names` at their values; how many it pointed.
int retextureIn(caff::Caff& c, size_t sym, const std::map<std::string, std::string>& names);
// A copy of the game's texture `from` as `to`, showing `img` (at most `maxSide` a side, else at the texture's own size when
// its layout can't change): a whole texture from `res`, or a split one's "mip" half from `res` and its top level from the
// stream bundle `tops`, an entry each. Adds the old names to new to `names`; `summary` says what it became.
std::vector<streambundle::Entry> textureCopy(const caff::Caff& res, const fs::path& tops, const std::string& from, const std::string& to,
                                             const image::Rgba& img, uint32_t maxSide, std::map<std::string, std::string>& names,
                                             std::string& summary);

// Writes the world into `mod` (scene in the mod's stream bundle, world in worlds/Bundle/50, its NPCs'
// scripts, scripts/renutworld_<id>.lua unless it exists and `force` is off). `assets`: the game's assets.
void make(const fs::path& assets, const fs::path& mod, const World& w, bool force, const Say& say);

} // namespace nut::renutworld
