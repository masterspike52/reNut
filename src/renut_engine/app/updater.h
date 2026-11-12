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

void Start();

Status Get();

bool Restart();

}
