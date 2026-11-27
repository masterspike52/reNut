// State of the windows that run the command-line tool's jobs in the GUI (tools.cpp).
#pragma once

#include "common.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nut::actorswap {
struct Index;
}

namespace nut::gui {

// What a file holds, for the name pickers: its models, animations and textures.
struct FileNames {
    bool loading = true;
    std::string error;
    std::vector<std::string> models, anims, textures;
};

// A .glb's mesh nodes for a picker, read again when `file` (UTF-8) differs from `seen`.
void glbNodes(const std::string& file, std::string& seen, std::vector<std::string>& nodes, std::string& error);

// A character in the world tool's editor, with renutnpc's fields (paths as typed, UTF-8).
struct NpcEdit {
    std::string id, name, model, rig = "auto", idle = "breathe", tag;
    float scale = 1, smooth = 40, colourScale = 1, position[3] = {}, yaw = 0;
    int speaker = 32;
    std::string lines[5];  // greeting, objectives, win, lose, completed
    bool challenge = false;
    std::string chId, chName, chSummary;
    float goal[3] = {}, goalYaw = 0, gate[2] = {16, 26}, timeLimit = 60, tt = 25, jiggy = 40;
    int notes = 1;
    bool countDown = false, startSet = false, startYawSet = false;
    float start[3] = {}, startYaw = 0;
    int voiceMode = 0;  // 0 none, 1 a game character's, 2 their own clips
    std::string gameVoice = "humba";
    std::vector<std::string> voiceClips;
    float voiceVolume = 0.9f;
    std::string sounds[5];  // a sound file per line, as lines
    std::string exclude;    // parts of names to leave out of the model, comma separated
    float armsDown = 0;     // degrees the arms are lowered by (a T-pose's)
    bool enemy = false;     // an enemy (no talking, no challenge)
    int enemyHealth = 3;
    int enemyBehaviour = 0;  // 0 mirror (copies Banjo), 1 none (your own Lua)
    std::string shapes;      // shape keys' starting weights: "name=weight, ..."
};

// A material in the world tool's table: renutworld's MaterialSettings with every setting given.
struct MaterialEdit {
    std::string name;
    int alpha = 1;  // 0 opaque, 1 cutout, 2 blend, 3 add
    float cutoff = 0.5f, opacity = 1, emissive[3] = {}, strength = 1, glow = 1;
    bool autoGlow = true, doubleSided = false, unlit = false;
    std::string emissiveTexture;  // an image file; empty: the model's own
};

// A part in the vehicle tool's editor (renutvehicle's vehicle file).
struct VehiclePartEdit {
    std::string part, base;
    int cell[3] = {};
    float rotation[3] = {};
    float colour[3] = {0.878f, 0.604f, 0.027f};  // e09a07, the vehicle file's default
    int button = 0;                              // 0 none, 1 A, 2 B, 3 X
};

struct ToolWindows {
    // Tools > Run a nuttool command
    bool showConsole = false, consoleScroll = false;
    std::string consoleLine, consoleOutput;

    // Tools > Pack and convert files
    bool showPack = false;
    std::string caffDir, caffOut, sbDir, sbOut, unxctdIn, unxctdOut, xctdIn, xctdOut;
    std::string cuFile, cuDir, suFile, suDir;
    bool caffXctd = false;

    // Tools > Inspect, extract and replace
    bool showFileJobs = false;
    std::string fjFile, fjSymbol, fjExtractOut, fjData, fjOut, fjMenuBundles, fjFontDir, fjFontOut, fjFontBase;
    bool fjGpu = false, fjXctd = false;

    // Tools > Export from a file
    bool showExport = false;
    std::string exFile, exFileSeen, exDir, exStream, exWaveName;
    int exTexFormat = 1;  // 0 DDS, 1 DDS + PNG, 2 PNG only
    bool exAllGroups = false, exModelPng = false, exWav = true;

    // Tools > Export a model
    bool showModel = false;
    std::string meFile, meModel, meOut, meAnimsFrom, meShapesOut;
    int meAnims = 0;  // 0 named after the model, 1 every animation, 2 none
    int meAxis = 1;   // X, Y, Z, -X, -Y, -Z
    bool meAllGroups = false, meTextures = true, meTint = true, meFur = true, meReorient = false, meEnds = false;
    float meFurLength = 1.5f, meFurTiling = 6, meEndSize = 1.5f;
    int meFurSize = 512;

    // Tools > Import into a file
    bool showImport = false;
    std::string tiFile, tiFileSeen, tiTexture, tiImage, tiStream, tiStreamOut, tiOut;
    std::string aiFile, aiAnim, aiKeys, aiOut;
    bool tiXctd = false, aiXctd = false;
    std::string miFile, miModel, miMesh, miOut, msFile, msModel, msSource, msSourceModel, msOut, msStreamOut;
    bool miRig = false, miXctd = false, msXctd = false;

    // Tools > Game scripts and objparams
    bool showScripts = false;
    std::string scBundle, scName, scAssets, scDir, scIn, scOut;
    int scKind = 0;  // 0 game scripts, 1 objparams, 2 vehicle parts only
    bool scXctd = false;

    // Tools > Replace an actor: the game's actors (read once), the two chosen and each target entry's anim.
    bool showActor = false;
    std::shared_ptr<const actorswap::Index> actorIndex;
    std::string asTarget, asSource, asTargetFilter, asSourceFilter, asMatched;  // asMatched: "target|source" asAnims was made for
    std::map<std::string, std::string> asAnims;
    int asEvents = 0;  // 0 keep the target's, 1 the source's, 2 none
    int asMode = 0;    // 0 a game actor, 1 a rigged .glb of your own
    std::string asGlb, asGlbSeen, asGlbError;  // the .glb (UTF-8), the one its animations were read from, and why not
    std::vector<std::string> asGlbAnims;
    float asScale = 1;

    // reNut menu
    bool showPartPicker = false, showBlock = false, showWorld = false, showVehicle = false, showGraph = false;
    std::string graphMod;  // the script graph editor's mod folder
    std::string graphTheme = "default";  // its nodes' look: default, bubble(oval), bubble(square) or glass
    std::string partFilter;
    std::string blMod, blId, blName, blModel, blMesh, blShape = "cube", blAttach = "all", blCategory = "body", blType, blBundle;
    std::string blNodesSeen, blNodesError;
    std::string blShapes;  // shape keys' starting weights: "name=weight, ..."
    std::vector<std::string> blNodes;
    float blMass = 10, blHealth = 5;
    int blCount = 99;
    bool blForce = false;
    std::string veBundle, veVehicle, veMod;
    int veMode = 0;  // 0 the editor, 1 a vehicle file
    std::string veName;
    std::vector<VehiclePartEdit> veParts;
    int veSel = -1;
    std::string wdMod, wdId, wdName, wdModel, wdBundle, wdHologram, wdNpcs;
    int wdNpcMode = 0;  // 0 none, 1 the editor, 2 a JSON file
    std::vector<NpcEdit> wdNpcList;
    int wdNpcSel = -1;
    std::string wdNpcFile;  // the editor's last loaded or saved file
    bool wdSpawnSet = false, wdExitSet = false, wdForce = false;
    float wdSpawn[3] = {}, wdExit[3] = {}, wdDoor[3] = {-45.8f, 0.8f, 315.8f};
    float wdSpawnYaw = 0, wdExitYaw = 0, wdDoorYaw = 92, wdSmooth = 40;
    float wdScale = 1, wdGround = 0, wdGroundColour[3] = {0.45f, 0.58f, 0.35f};
    float wdColourScale = 1;
    int wdMaxTexture = 1024;
    int wdDoorStyle = 0;  // 0 wall, 1 arch, 2 palace, 3 hidden
    int wdStatTotals[4] = {-1, -1, -1, -1};  // the door panel's jiggies, trophies, jinjos, notes; -1: counted
    std::string wdMusic;
    std::string wdWater;  // more water: parts of object, material or texture names, comma separated
    std::string wdShapes;  // shape keys' starting weights: "name=weight, ..."
    std::vector<MaterialEdit> wdMaterials;  // written to <mod>/materials.json when the world is made
    std::string wdMaterialsNote;            // what reading them said
    float wdMusicVolume = 0.8f;
    int wdSkyMode = 0;  // 0 none, 1 a panorama image, 2 a gradient
    std::string wdSkyImage;
    float wdSkyTop[3] = {0.23f, 0.47f, 0.85f}, wdSkyHorizon[3] = {0.72f, 0.85f, 0.97f};

    // Names in the files the pickers were given, by UTF-8 path.
    std::map<std::string, std::shared_ptr<FileNames>> names;

    // The fields kept between sessions in the GUI's settings, by key: folders, mod and world fields.
    std::vector<std::pair<std::string, std::string*>> remembered() {
        return {{"exDir", &exDir},     {"scDir", &scDir},       {"scAssets", &scAssets},     {"blMod", &blMod},
                {"blBundle", &blBundle}, {"wdMod", &wdMod},     {"wdId", &wdId},             {"wdName", &wdName},
                {"wdModel", &wdModel}, {"wdBundle", &wdBundle}, {"wdHologram", &wdHologram}, {"wdNpcs", &wdNpcs},
                {"wdSkyImage", &wdSkyImage}, {"wdNpcFile", &wdNpcFile}, {"veBundle", &veBundle}, {"veMod", &veMod},
                {"veVehicle", &veVehicle}, {"veName", &veName}, {"graphMod", &graphMod}, {"graphTheme", &graphTheme}};
    }
};

} // namespace nut::gui
