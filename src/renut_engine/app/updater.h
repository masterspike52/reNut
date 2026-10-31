#pragma once
// reNut keeping itself up to date from its GitHub releases
// (github.com/masterspike52/reNut/releases), for release builds only (a dev
// build is never replaced by a download).
//
// The launcher (ui/launcher_menu.h) starts it. It asks GitHub for the latest
// release, and when that is not the one installed (installed_release.txt in
// reNut's writable folder; the first run takes the latest as installed), it
// downloads the release's file for this system, checks it against GitHub's
// SHA-256 and puts it in place: on Windows the .zip's files beside renut.exe
// (the running ones renamed to .old, which the next start deletes), on Linux
// the AppImage over the one running. reNut then starts again into it.

#include <string>

namespace renut::update {

enum class State {
    kOff,          // a dev build, or nothing to update from (not an AppImage on Linux)
    kChecking,     // asking GitHub
    kUpToDate,
    kDownloading,  // the new release, into memory
    kInstalling,
    kReady,        // installed: Restart starts it
    kFailed,
};

struct Status {
    State state = State::kOff;
    std::string release;  // the new release's name ("beta-nut 16: ...")
    std::string error;    // kFailed's reason
};

// Once, as the launcher opens: deletes what the last update left (.old
// files), then checks and updates on a thread of its own.
void Start();

Status Get();

// After kReady: starts the updated reNut with this one's command line. True
// when it started (this one should close now).
bool Restart();

} // namespace renut::update
