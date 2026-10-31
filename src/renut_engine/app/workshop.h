#pragma once
// The community vehicle workshop (github.com/etonedemid/nutstaller-workshop:
// index.json lists the blueprints, each a folder with blueprint.bin and a
// preview.jpg) and the save folder a downloaded vehicle goes into, as the
// game keeps a vehicle: a content folder of its own (00000001/0x<id>/<id>)
// and its header ("VEHICLE: <name>"). ui/launcher_menu.h shows it.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace renut::workshop {

// The workshop's files, over HTTPS: https://<kHost><kRoot><path>.
constexpr const char kHost[] = "raw.githubusercontent.com";
constexpr const char kRoot[] = "/etonedemid/nutstaller-workshop/main/";

struct Entry {
    std::string id;
    std::string name;
    std::string description;
    std::string author;
    std::string created;    // ISO 8601
    std::string blueprint;  // repository paths
    std::string preview;
    int parts = 0;
    std::vector<std::string> needs;  // index.json's "requires": workshop part mods it needs
};

// index.json's blueprints, newest first as it lists them; empty and an error
// if it can't be read.
std::vector<Entry> ParseIndex(const std::string &json, std::string *error);

// The game's own save folder in the user data folder: the SDK profile's
// (B13EBABEBABEBABE), or another profile's that has saves when that one has none.
std::filesystem::path SaveFolder(const std::filesystem::path &user_root);

// A Nuts & Bolts blueprint: the magic, the part count at 0x08 (big-endian),
// the name at 0x28 (32 UTF-16 characters), 0x84 bytes, then 36 per part.
bool IsBlueprint(const std::string &data);

// Adds the blueprint to the save as a new vehicle, remembered as workshop
// entry 'id' (renut-workshop.json beside the profiles, so the game never
// sees it). False, with the error, if it can't.
bool Install(const std::filesystem::path &save, const Entry &entry, const std::string &blueprint,
             std::string *error);

// Takes a vehicle Install added back out of the save. Only those: never one
// the player built.
void Remove(const std::filesystem::path &save, const std::string &id);

// The workshop entries installed whose vehicle is still in the save (one
// deleted in the game drops out), by workshop id.
std::map<std::string, std::string> Installed(const std::filesystem::path &save);

// A PNG or JPEG to RGBA, or empty; scaled down to max_width if it is wider.
std::vector<uint8_t> DecodeImage(const void *data, size_t size, int *width, int *height, int max_width = 0);

// --- Sharing ----------------------------------------------------------------------
//
// A vehicle goes to the workshop through its GitHub issue form: the player
// pastes the submission (BuildSubmission) into kSubmitUrl's form and submits
// it, and the workshop's bot checks and publishes it (tools/workshop.py there).

constexpr const char kSubmitUrl[] =
    "https://github.com/etonedemid/nutstaller-workshop/issues/new?template=submit-blueprint.yml";
// What the bot keeps of a name and a description.
constexpr size_t kMaxName = 31;
constexpr size_t kMaxDescription = 200;

// A vehicle in the save: its name ("VEHICLE: <name>") and blueprint file.
struct LocalVehicle {
    std::string name;
    std::filesystem::path file;
};
std::vector<LocalVehicle> LocalVehicles(const std::filesystem::path &save);

// The submission for the vehicle: JSON with "name" first (the bot finds it by
// that), the blueprint and the screenshot (any PNG or JPEG, made a JPEG small
// enough for the issue) in base64, and the custom parts of reNut mods it
// uses (from <mods folder>/.renut_custom_parts.json). Empty, with the error,
// if it can't be made.
std::string BuildSubmission(const std::filesystem::path &blueprint_file, const std::string &name,
                            const std::string &description, const std::filesystem::path &screenshot,
                            const std::filesystem::path &mods_folder, std::string *error);

} // namespace renut::workshop
