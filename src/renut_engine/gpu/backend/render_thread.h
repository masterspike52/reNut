/**
 * @file    gpu/backend/render_thread.h
 * @brief   The renderer's own thread: everything that builds host GPU work runs
 *          there, in the order the game issued it.
 *
 * The game's D3D calls (hooks/) queue work instead of doing it: a draw takes a
 * snapshot of the device state it reads and copies of any data the game may
 * change after the call returns, and the render thread records it later. The
 * render thread owns every renderer structure (State(), textures, surfaces,
 * pipelines, the draw path) and takes State().mutex around each piece of work.
 *
 * Guest memory the draws read in place (vertex, index and texture data) stays
 * valid the way it does on the console: the game waits on a fence before
 * reusing it. Fences complete when the render thread has read the data of
 * the work kicked off before them (QueueFenceCompletion), and D3D's fence
 * waits wait for the render thread (hooks/device.cpp, CBlocker::Check).
 */
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

// Writes 'fence' to the guest word at 'completion_va' (the device's fence
// completion, which IsFencePending and BlockOnFence read) once the work queued
// before it has run, and the guest callbacks up to 'after_callback' (see
// QueueCallbackMarker) have run: on the console the GPU calls them on its way
// to the fence, and the game may free what they use once it has passed.
void QueueFenceCompletion(u32 completion_va, u32 fence, uint64_t after_callback);

// Guest callbacks (D3DDevice_InsertCallback) are numbered from 1 by the game
// side (callbacks.h). The render thread reaches callback 'seq' once the work
// queued before this marker has run; a guest thread then runs it and reports
// with CallbacksRun, which writes the fence completions that waited for it.
void QueueCallbackMarker(uint64_t seq);
uint64_t CallbacksReached();
void CallbacksRun(uint64_t seq);

// Waits up to 'timeout' for the render thread to complete another fence or
// reach another callback. Returns false when it is not running (nothing would
// ever complete).
bool WaitForFenceProgress(std::chrono::milliseconds timeout);

// A guest frame: 'work' (the Swap) is queued; then the game thread waits while
// more than kMaxFramesAhead frames are queued but not yet done. Returns the
// number of frames the render thread has finished.
constexpr uint64_t kMaxFramesAhead = 1;
uint64_t QueueFrame(std::function<void()> work);
uint64_t FramesDone();

// Waits until everything queued so far has run.
void Drain();

// The render thread blocked (on a GPU fence, for a back buffer, in a present
// held to the display's refresh) for the scope: not work, so not counted in
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

// Shutdown: the thread finishes what is queued and ends; nothing waits for it
// any more. Does not wait itself (the thread may be waiting on the UI thread
// for the overlay). Work queued afterwards runs on the calling thread.
void Stop();

} // namespace renut::gpu::render_thread
