// Lua scripting: a `nut` table over the asset code, shared by `nuttool script` and the GUI's
// script window. reNut's docs/nuttool/cli.md documents the API.
#pragma once

#include "common.hpp"

#include <atomic>
#include <functional>

namespace nut::script {

struct Options {
    fs::path assets;                                // nut.assets; relative paths are tried here first
    fs::path scriptDir;                             // nut.script_dir; relative paths not in `assets` go here
    std::vector<std::string> args;                  // the global `arg` table
    std::function<void(const std::string&)> output; // print() and nut.run() output
    const std::atomic<bool>* cancel = nullptr;      // set to stop the script
};

// Runs Lua source. Returns false after an error (reported through `output`).
bool run(const std::string& code, const std::string& chunkName, const Options& opt);

bool available(); // false when built without Lua

} // namespace nut::script
