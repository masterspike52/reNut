// Making reNut mod content: a part's own model and footprint (renut-part --model / --attachable) and
// a block of its own (renut-block). Shared by the command-line tool and the GUI.
#pragma once

#include "renutmodel.hpp"
#include "renutpart.hpp"

#include <functional>
#include <map>

namespace nut::renutmake {

// What the work did, a line at a time (the command-line tool prints them, the GUI logs them).
using Say = std::function<void(const std::string&)>;

// The mod's stream bundle name when none is given: the mod folder's name in a-z, 0-9 and _.
std::string defaultBundle(const fs::path& mod);

// A part's own model and footprint, in the mod's stream bundle.
struct PartAssets {
    fs::path model;             // OBJ, FBX or .glb; empty keeps the base's model
    std::string templateName;   // the part model it starts from; empty: the base's own
    renutmodel::GlbMesh glb;    // a .glb's node and the template's meshes that get it
    std::string collision;      // another part model's collision, "none", or empty for the template's
    std::string attachable;     // the faces that take other parts; empty keeps the base's footprint
    std::string bundle;         // the mod's stream bundle; empty: defaultBundle(mod)
};

// Builds `assets` from `stream` (the part bundle's paired stream bundle) and points `part` at them.
// Returns false when the part's script exists and is kept (no `force`): only the assets were made.
bool addPartAssets(const fs::path& mod, const fs::path& stream, renutpart::Part& part, const PartAssets& assets, bool force,
                   const Say& say);

// A block of its own. Empty fields take the defaults the command line documents.
struct Block {
    std::string id;             // 1-40 characters of a-z, 0-9 and _
    std::string name;           // its garage name; empty: the id
    fs::path model;             // the .glb
    std::string mesh;           // the node whose mesh it gets
    std::string shape;          // its collision: cube (default), wedge or corner
    std::string attachable;     // the faces that take other parts: all (default), none, top...
    std::string category;       // the garage category: body (default)
    std::string type;           // a garage type it shares with the blocks naming it
    std::string mass, health, count;  // Lua numbers, as text: 10, 5 and 99 by default
    std::string bundle;         // the mod's stream bundle; empty: defaultBundle(mod)
    // Shape keys' starting weights by name, over the .glb's own (its mesh's morph targets).
    std::map<std::string, float> shapeWeights;
};

// Writes the block's model and footprint into the mod's stream bundle, <mod>/scripts/<id>.lua (an
// existing one is kept without `force`) and <mod>/mod.toml when the folder has none.
void block(const fs::path& mod, const Block& b, bool force, const Say& say);

// A player model for reNut (renut.player.set_model, renut.worlds.add's player_model): a copy of Banjo's model
// `templateName` from the common bundle under `assets` ("actor_banjo" or "actor_banjotuxedo"; empty: the one a
// NutTool FBX export names) with the file's meshes applied, as aid_model_renut_player_<id> in the mod's bundle.
void playerModel(const fs::path& assets, const fs::path& mod, const fs::path& file, const std::string& id,
                 const std::string& templateName, const std::string& bundle, const Say& say);

} // namespace nut::renutmake
