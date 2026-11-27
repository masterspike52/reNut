// Characters of a reNut world's own from a JSON file (renut-world --npcs): their models, actors, dialogs,
// challenges, marker records and scene lines, and the Lua giving reNut their text, tag, flags and counters.
#pragma once

#include "common.hpp"
#include "image.hpp"
#include "streambundle.hpp"

#include <array>
#include <functional>
#include <map>
#include <optional>

namespace nut::renutnpc {

using Say = std::function<void(const std::string&)>;

// A "reach a spot" challenge the character gives: drive through the goal gate within the time limit.
struct Challenge {
    std::string id;                      // a-z, 0-9 and _
    std::string name, summary;           // its title and one line about it
    std::array<float, 3> goal{};         // the goal gate's middle, on the ground
    float goalYaw = 0;                   // degrees: the gate faces +z turned by this
    std::array<float, 2> gate{16, 26};   // the gate's width and height
    float timeLimit = 60;                // seconds: notes for finishing within it
    float tt = 25, jiggy = 40;           // seconds: a T.T. trophy, a jiggy for finishing within these
    uint32_t notes = 1;                  // the reward count (the notes paid are 10 times it)
    bool countDown = false;              // the clock counts down from timeLimit (failing at 0), else up from 0
    // Where the player's vehicle starts it (kind 21); empty: 12 m from the character towards the goal.
    std::optional<std::array<float, 3>> start;
    std::optional<float> startYaw;       // degrees; empty: facing the goal
};

// An enemy: hits wear its health down and it is defeated at 0 (reNut counts them). Its behaviour is a Lua
// script: "mirror" writes one that copies Banjo as a mirror image; "none" leaves it standing for your own.
struct Enemy {
    int health = 3;
    std::string behaviour = "mirror";
};

struct Npc {
    std::string id;                      // a-z, 0-9 and _, unique in the world
    std::string name;                    // who they are (the Lua's comment and name)
    fs::path model;                      // .dae, .glb or .obj; y up after loading, feet at the bottom
    float scale = 1, smoothDegrees = 40, colourScale = 1;
    std::array<float, 3> position{};     // their feet (the game puts them on the ground below)
    float yaw = 0;                       // degrees: yaw 0 faces +z
    std::string rig = "auto";            // the skeleton: "auto" (by height)
    std::string idle = "breathe";        // the looping idle: a built-in one, or a JSON timeline file
    // Parts of object, material or texture names to leave out of the model (a rip's shadow sheet...).
    std::vector<std::string> exclude;
    float armsDown = 0;                  // degrees the arms are lowered by in every animation (a T-pose's: about 70)
    // Shape keys' starting weights by name (0 to 1), over the model's own (glTF morph targets' weights).
    std::map<std::string, float> shapeWeights;
    std::string tag;                     // their objTag name (reNut maps it onto a spare one)
    int speaker = 32;                    // the dialogs' speaker slot (dbCharData_Id), given to their chardata
    std::map<std::string, std::string> lines;  // greeting, objectives, win, lose, completed
    std::optional<Challenge> challenge;
    std::optional<Enemy> enemy;           // an enemy rather than someone to talk to (no lines, no challenge)
    // Their voice as each page of a line starts: a game character's (gameVoices()), their own sound files
    // (one at random), or neither (silent).
    std::string voice;
    std::vector<fs::path> voiceClips;
    float voiceVolume = 0.9f;                // 0-1, for the clips and the sounds
    std::map<std::string, fs::path> sounds;  // a line's own sound file, played as it starts
};

// The game characters whose voices an NPC can have, by name ("humba", ...).
const std::vector<std::string>& gameVoices();

// The NPCs of a JSON file {"npcs": [...]}; relative model, idle and sound paths are read beside the file.
std::vector<Npc> load(const fs::path& json);

// What load would refuse in `n` (empty: nothing), each without the NPC's name. Paths aren't opened.
std::vector<std::string> problems(const Npc& n);

// `npcs` as the JSON load reads, to be written to `json`: paths under its folder are written relative to it.
std::string toJson(const std::vector<Npc>& npcs, const fs::path& json);

// Makes a texture asset `name` showing `img` (the world's texture maker); `summary` says what it became.
using TextureMaker = std::function<Bytes(const std::string& name, const image::Rgba& img, std::string& summary)>;

// What the world gets from its NPCs.
struct Built {
    std::vector<streambundle::Entry> stream;    // models, animations, objparams, dialogs, scripts, challenges
    std::vector<streambundle::Entry> textures;  // for the world's resident bundle
    Bytes markerRecords;                        // kind 6 (each NPC) and kind 22 (each goal gate) records
    std::string sceneText;                      // scene lines: givers (op_6F, op_71) and talk blocks (op_58)
    std::vector<std::pair<fs::path, std::string>> scripts;  // the Lua files, relative to the mod folder
    std::vector<std::pair<fs::path, fs::path>> files;       // sound files to copy: from, to (relative to the mod folder)
};

// Builds `npcs` for the world `worldId` (named `worldName`), their marker records numbered from `firstMarkerId`.
// `assets` is the game's assets folder: the objparams, chardata and challenge layouts (and game voices) are
// copied from its own. Sound files go to sounds\<npc id>\ in the mod (Built::files).
Built build(const fs::path& assets, const std::string& worldId, const std::string& worldName, const std::vector<Npc>& npcs,
            uint16_t firstMarkerId, const TextureMaker& texture, const Say& say);

} // namespace nut::renutnpc
