#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

#include <rex/types.h>

namespace renut::gpu::render_thread {

// Runs 'work' on the render thread after everything queued before it, with
// State().mutex held.
void Queue(std::function<void()> work);

// The same without taking State().mutex, for work that takes it itself
// (Video::Present, record destruction).
void QueueUnlocked(std::function<void()> work);

// Writes 'fence' to the device's fence completion word at 'completion_va' once
// the work queued before it and guest callbacks up to 'after_callback' have run.
void QueueFenceCompletion(u32 completion_va, u32 fence, uint64_t after_callback);

// Guest callback 'seq' (D3DDevice_InsertCallback) is reached once the work queued
// before its marker has run; a guest thread runs it and reports with CallbacksRun.
void QueueCallbackMarker(uint64_t seq);
uint64_t CallbacksReached();
void CallbacksRun(uint64_t seq);

// Waits up to 'timeout' for the render thread to complete another fence or reach
// another callback. Returns false when it is not running.
bool WaitForFenceProgress(std::chrono::milliseconds timeout);

// Queues a guest frame's Swap, then waits while more than kMaxFramesAhead frames
// are pending. Returns the number of frames the render thread has finished.
constexpr uint64_t kMaxFramesAhead = 1;
uint64_t QueueFrame(std::function<void()> work);
uint64_t FramesDone();

// Waits until everything queued so far has run.
void Drain();

// Marks the render thread as blocked for the scope, so the time is left out of
// its busy time (frame_stats' Render). Nothing on other threads.
class WaitScope {
public:
  WaitScope();
  ~WaitScope();
  WaitScope(const WaitScope &) = delete;
  WaitScope &operator=(const WaitScope &) = delete;

private:
  std::chrono::steady_clock::time_point start_;
  bool counted_;
};

// Makes the thread finish what is queued and end, without waiting for it; work
// queued afterwards runs on the calling thread.
void Stop();

} // namespace renut::gpu::render_thread
