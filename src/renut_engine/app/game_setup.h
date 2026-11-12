#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace renut::setup {

struct Paths {
    std::filesystem::path game;
    std::filesystem::path update;
    std::filesystem::path user;
};

Paths LoadPaths(const std::string &app_name, const Paths &defaults);
void SavePaths(const std::string &app_name, const Paths &paths);

struct Status {
    bool game = false;          
    bool other_region = false;  
    bool update = false;       
    bool dlc = false;
};

Status Check(const Paths &paths);

enum class Job { kNone, kDisc, kTitleUpdate, kDlc };

struct Progress {
    Job job = Job::kNone;
    bool running = false;
    uint64_t bytes = 0;
    uint64_t total = 0;
    std::string file;
    std::string error;
    std::string result;
};

bool StartExtractDisc(const std::filesystem::path &image, const std::filesystem::path &game);
bool StartExtractTitleUpdate(const std::filesystem::path &package, const std::filesystem::path &game, const std::filesystem::path &update);
bool StartAddDlc(std::vector<std::filesystem::path> packages);

Progress GetProgress();
void Cancel();

} 