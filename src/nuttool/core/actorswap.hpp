// Replacing a game actor with another: the actor (its objparams) draws the other's model, rig included, and its
// animation table plays the other's animations, chosen entry by entry. Its moves, body and states stay its own.
#pragma once

#include "common.hpp"

#include <functional>
#include <map>

namespace nut::actorswap {

using Say = std::function<void(const std::string&)>;

// An animation table entry: the name states play it by, and its anim and animation events (asset ids, 0: none).
struct Entry {
    std::string name;
    uint32_t anim = 0, events = 0;
};

// A character: an actor objparams that has a model.
struct Actor {
    std::string name;               // aid_objparams_banjox_actor_...
    uint32_t model = 0, table = 0;  // its model and animation table (asset ids)
    std::vector<Entry> entries;     // its table's
};

// The game's bundles (a title update's copy in place of its own when there is one) and their assets.
struct Index {
    fs::path assets;  // the game's assets folder
    std::vector<fs::path> bundles;
    std::map<uint32_t, std::vector<size_t>> holders;  // asset id -> the bundles holding it
    std::map<uint32_t, std::string> names;             // every asset name seen, by id
    std::vector<Actor> actors;                         // sorted by name
    std::string nameOf(uint32_t id) const;             // "" when unknown
    const Actor* actor(const std::string& name) const;
};

// Reads every bundle db_index.txt lists under `assets`, preferring the copy in `update` (a title update folder,
// files named 4f<id>) when there is one.
Index scan(const fs::path& assets, const fs::path& update, const Say& say);

// Which animation events the target's table entries get.
enum class Events { Keep, Source, None };

struct Plan {
    std::string target, source;  // the actors' objparams names; `source` empty when `glb` is given
    // Or an actor of your own: a rigged .glb's skinned meshes on its own skeleton, its textures and its animations,
    // `scale` times the file's size (metres). Its assets are named aid_model_renutactor_<stem>...
    fs::path glb;
    float scale = 1;
    // Each target table entry -> the anim it plays: a source anim (aid_anim_...), or for a .glb one of its animations' names.
    std::map<std::string, std::string> anims;
    Events events = Events::Keep;  // a .glb has no events: Source gives none
};

// A first choice for each target entry: the source's entry of the same name (exactly, then ignoring case and
// underscores), else the source's "stand", else its first entry.
std::map<std::string, std::string> autoMatch(const Actor& target, const Actor& source, const Index& ix);

// A rigged .glb's animation names, and a first choice for each target entry among them: the same name (exactly, then
// ignoring case and underscores), else one named like stand or idle, else the first.
std::vector<std::string> glbAnimations(const fs::path& glb);
std::map<std::string, std::string> autoMatchGlb(const Actor& target, const std::vector<std::string>& animations);

// The anims `source` can play: its table's, then the other anims named after its model's character, sorted.
std::vector<std::string> sourceAnims(const Actor& source, const Index& ix);

// Applies `plan`: the bundles holding the target's objparams get the source's model (and the textures it needs,
// their streamed levels in the paired stream bundles) and the actor draws it; those holding its table get the
// anims (and events) it now plays. Every entry needs an anim, as the game animates a model only with anims made for
// its skeleton. Files are written in place after a one-time .bak; returns them.
std::vector<fs::path> apply(const Index& ix, const Plan& plan, const Say& say);

// A plan as JSON {"target", "source" or "glb" and "scale", "events": "keep"|"source"|"none", "anims": {entry: anim}},
// and back (any of them may be left out).
std::string planToJson(const Plan& plan);
Plan planFromJson(const std::string& text);

} // namespace nut::actorswap
