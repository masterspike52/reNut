#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace rex::thread {
class Thread;
}

namespace renut::nativevk {

enum class CommandType {
    kClear,
};

struct Command {
    CommandType type;
    float clear_color[4] = {0.05f, 0.05f, 0.2f, 1.0f};
};

class CommandQueue {
public:
    explicit CommandQueue(std::function<void(const Command&)> executor);
    ~CommandQueue();

    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    void Push(const Command& command);

private:
    void RenderThreadMain();

    std::function<void(const Command&)> executor_;
    std::mutex mutex_;
    std::condition_variable cond_;
    std::deque<Command> commands_;
    bool shutdown_ = false;
    std::unique_ptr<rex::thread::Thread> render_thread_;
};

}  // namespace renut::nativevk
