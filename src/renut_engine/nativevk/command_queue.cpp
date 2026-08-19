#include "renut_engine/nativevk/command_queue.h"

#include <rex/thread.h>

namespace renut::nativevk {

CommandQueue::CommandQueue(std::function<void(const Command&)> executor)
    : executor_(std::move(executor)) {
    render_thread_ = rex::thread::Thread::Create({}, [this]() { RenderThreadMain(); });
    render_thread_->set_name("NativeVK Render Thread");
}

CommandQueue::~CommandQueue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }
    cond_.notify_all();
    rex::thread::Wait(render_thread_.get(), false);
}

void CommandQueue::Push(const Command& command) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.push_back(command);
    }
    cond_.notify_one();
}

void CommandQueue::RenderThreadMain() {
    for (;;) {
        Command command;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [this]() { return shutdown_ || !commands_.empty(); });
            if (shutdown_ && commands_.empty()) {
                return;
            }
            command = commands_.front();
            commands_.pop_front();
        }
        executor_(command);
    }
}

}  // namespace renut::nativevk
