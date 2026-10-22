/**
 * @file    gpu/render/parallel.cpp
 */
#include "renut_engine/gpu/render/parallel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace renut::gpu::parallel {

namespace {

// Never destroyed, like the render thread: the helpers wait on it until exit.
class Pool {
public:
  static Pool &Get() {
    static Pool *pool = new Pool;
    return *pool;
  }

  size_t Helpers() const { return helpers_; }

  void Run(size_t count, size_t grain, const std::function<void(size_t, size_t)> &body) {
    {
      std::lock_guard lock(mutex_);
      body_ = &body;
      count_ = count;
      grain_ = grain;
      next_.store(0, std::memory_order_relaxed);
      running_ = helpers_;
      ++generation_;
    }
    start_cv_.notify_all();
    Work(body, count, grain);
    std::unique_lock lock(mutex_);
    done_cv_.wait(lock, [this] { return running_ == 0; });
    body_ = nullptr;
  }

private:
  Pool() {
    // A few, leaving the game's threads, the render thread and the recording
    // thread their cores.
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    helpers_ = cores >= 8 ? std::min<size_t>(3, cores / 2 - 2) : cores >= 6 ? 1 : 0;
    for (size_t i = 0; i < helpers_; ++i)
      std::thread(&Pool::Helper, this).detach();
  }

  void Work(const std::function<void(size_t, size_t)> &body, size_t count, size_t grain) {
    for (;;) {
      const size_t begin = next_.fetch_add(grain, std::memory_order_relaxed);
      if (begin >= count)
        return;
      body(begin, std::min(begin + grain, count));
    }
  }

  void Helper() {
#if defined(_WIN32)
    SetThreadDescription(GetCurrentThread(), L"renut render helper");
#endif
    uint64_t seen = 0;
    for (;;) {
      const std::function<void(size_t, size_t)> *body;
      size_t count, grain;
      {
        std::unique_lock lock(mutex_);
        start_cv_.wait(lock, [&] { return generation_ != seen; });
        seen = generation_;
        body = body_;
        count = count_;
        grain = grain_;
      }
      Work(*body, count, grain);
      bool last;
      {
        std::lock_guard lock(mutex_);
        last = --running_ == 0;
      }
      if (last)
        done_cv_.notify_one();
    }
  }

  size_t helpers_ = 0;
  std::mutex mutex_;
  std::condition_variable start_cv_, done_cv_;
  const std::function<void(size_t, size_t)> *body_ = nullptr;
  size_t count_ = 0, grain_ = 1;
  std::atomic<size_t> next_{0};
  size_t running_ = 0;  // helpers still on the job
  uint64_t generation_ = 0;
};

} // namespace

void For(size_t count, size_t grain, const std::function<void(size_t, size_t)> &body) {
  if (!count)
    return;
  grain = std::max<size_t>(grain, 1);
  Pool &pool = Pool::Get();
  if (!pool.Helpers() || count <= grain) {
    body(0, count);
    return;
  }
  pool.Run(count, grain, body);
}

}  // namespace renut::gpu::parallel
