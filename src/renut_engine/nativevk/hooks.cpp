#include "renut_engine/nativevk/command_queue.h"
#include "renut_engine/nativevk/renderer.h"

namespace renut::nativevk {
namespace {

class System {
public:
    System() : queue_([this](const Command& command) { renderer_.Execute(command); }) {}

    void Push(const Command& command) { queue_.Push(command); }

private:
    Renderer renderer_;
    CommandQueue queue_;
};

System& GetSystem() {
    static System system;
    return system;
}

}  // namespace
}  // namespace renut::nativevk

void nativevkClear_hook() {
    renut::nativevk::Command command;
    command.type = renut::nativevk::CommandType::kClear;
    renut::nativevk::GetSystem().Push(command);
}

void nativevkClearF_hook() {
    renut::nativevk::Command command;
    command.type = renut::nativevk::CommandType::kClear;
    renut::nativevk::GetSystem().Push(command);
}
