#pragma once
#include <string>

namespace renut::update {

enum class State {
    kOff,
    kChecking,
    kUpToDate,
    kDownloading,
    kInstalling,
    kReady,
    kFailed,
};

struct Status {
    State state = State::kOff;
    std::string release;
    std::string error;
};

// The launcher: checks GitHub once, while renut_auto_update is on, and downloads and installs a new release.
void Start();
// PLAY: nothing is downloaded or installed after it (an install underway finishes first). False when an update
// was installed, so reNut restarts into it rather than play the old version.
bool Stop();

Status Get();

bool Restart();

}
