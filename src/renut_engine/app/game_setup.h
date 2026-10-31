#pragma once
// The game's files, set up by reNut itself (ui/launcher_menu.h shows it):
// the game disc's files taken out of its image, Title Update #3 taken out of
// its package (default.xexp beside default.xex, where the SDK applies it as
// the game loads, and everything into the update folder, mounted as update:),
// DLC packages copied into the DLC folder (dlc_install.h installs them as the
// game starts), and where all of it is, kept in renut.cfg.
//
// One job runs at a time, on a thread of its own; GetProgress reports it.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/rex_app.h>

namespace renut::setup {

struct Paths {
    std::filesystem::path game;    // default.xex and the disc's files
    std::filesystem::path update;  // the title update's files
    std::filesystem::path user;    // saves and installed content
};

// renut.cfg's paths; what it lacks, from the SDK's defaults or reNut's own
// folders (<writable folder>/game, <game>/update).
Paths LoadPaths(const std::string &app_name, const rex::PathConfig &defaults);
void SavePaths(const std::string &app_name, const Paths &paths);

// reNut is the US (NTSC) release of Nuts & Bolts with Title Update #3: only
// that disc and that title update are taken (media ID 3E567DFF, in the XEX's
// execution info and the update package's header).
struct Status {
    bool game = false;          // the US default.xex there, its extraction finished
    bool other_region = false;  // a default.xex there, but not the US release's
    bool update = false;        // default.xexp beside it and the update folder filled
    bool dlc = false;           // the DLC (there is one): a package in the DLC
                                // folder, or installed in the save folder
};
Status Check(const Paths &paths);

enum class Job { kNone, kDisc, kTitleUpdate, kDlc };

struct Progress {
    Job job = Job::kNone;
    bool running = false;
    uint64_t bytes = 0;  // done of total
    uint64_t total = 0;
    std::string file;    // the one being written
    std::string error;   // the last job's failure, else empty
    std::string result;  // the last job's success, else empty
};

// Each returns false if a job is already running.
// The US disc's image (.iso): every file but $SystemUpdate into 'game'.
bool StartExtractDisc(const std::filesystem::path &image, const std::filesystem::path &game);
// The US Title Update #3's package: its files into 'update', default.xexp also into 'game'.
bool StartExtractTitleUpdate(const std::filesystem::path &package, const std::filesystem::path &game,
                             const std::filesystem::path &update);
// The DLC's package (one; refused when the DLC folder has it already) into the DLC folder.
bool StartAddDlc(std::vector<std::filesystem::path> packages);

Progress GetProgress();
// Stops a running job (its folder stays marked unfinished) and waits for it.
void Cancel();

} // namespace renut::setup
