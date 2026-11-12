/**
 * @file        core/frame_stats.cpp
 * @brief       Frame pacing summary implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "frame_stats.h"

#include <algorithm>
#include <chrono>

#include <rex/logging/log_category.h>

namespace rexcrash {

namespace {

struct Settings final : rex::SettingOwner {
  Settings() : rex::SettingOwner("rexcrash") {}
  REX_SETTING(int32_t, fps_log_interval, 60,
              "Seconds between frame rate summary lines in the log (0 = off). One line "
              "per interval, not per frame.",
              rex::Range{0, 3600});
  REX_SETTING(int32_t, fps_hitch_ms, 250,
              "Log a warning when a single frame takes longer than this many "
              "milliseconds (0 = off). Rate limited to one line per second.",
              rex::Range{0, 60000});
};

Settings& GetSettings() {
  static Settings* settings = new Settings();
  return *settings;
}

using HostClock = std::chrono::steady_clock;
static_assert(HostClock::period::num == 1);

uint64_t QueryHostTickFrequency() { return static_cast<uint64_t>(HostClock::period::den); }

uint64_t QueryHostTickCount() { return static_cast<uint64_t>(HostClock::now().time_since_epoch().count()); }

double TicksToMs(uint64_t ticks, uint64_t frequency) {
  return frequency ? static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency) : 0.0;
}

}  // namespace

rex::SettingOwner& FrameStatsSettings() { return GetSettings(); }

FrameStats& FrameStats::Global() {
  static FrameStats instance;
  return instance;
}

void FrameStats::OnPresent() {
  const uint64_t frequency = QueryHostTickFrequency();
  const uint64_t now = QueryHostTickCount();

  total_frames_.fetch_add(1, std::memory_order_relaxed);

  if (!last_frame_tick_) {
    first_frame_tick_ = now;
    interval_start_tick_ = now;
    last_frame_tick_ = now;
    return;
  }

  const uint64_t delta = now - last_frame_tick_;
  last_frame_tick_ = now;
  ++interval_frames_;
  interval_worst_ticks_ = std::max(interval_worst_ticks_, delta);
  uptime_seconds_.store(TicksToMs(now - first_frame_tick_, frequency) / 1000.0,
                        std::memory_order_relaxed);

  const int32_t hitch_ms = GetSettings().fps_hitch_ms.Get();
  if (hitch_ms > 0 && TicksToMs(delta, frequency) >= static_cast<double>(hitch_ms)) {
    hitches_.fetch_add(1, std::memory_order_relaxed);
    // One line per second at most: a stall usually comes in bursts, and the
    // count in the interval summary is the number that matters.
    if (!last_hitch_log_tick_ || (now - last_hitch_log_tick_) >= frequency) {
      last_hitch_log_tick_ = now;
      REXLOG_WARN("Frame hitch: {:.1f} ms (frame {})", TicksToMs(delta, frequency),
                  total_frames_.load(std::memory_order_relaxed));
    }
  }

  const int32_t interval = GetSettings().fps_log_interval.Get();
  if (interval > 0 && (now - interval_start_tick_) >= frequency * static_cast<uint64_t>(interval)) {
    PublishInterval(now, frequency, true);
  }
}

void FrameStats::FlushSummary() {
  if (!last_frame_tick_)
    return;
  PublishInterval(QueryHostTickCount(), QueryHostTickFrequency(), false);
}

void FrameStats::PublishInterval(uint64_t now_ticks, uint64_t tick_frequency, bool log_it) {
  const double elapsed_ms = TicksToMs(now_ticks - interval_start_tick_, tick_frequency);
  if (elapsed_ms <= 0.0 || interval_frames_ == 0) {
    interval_start_tick_ = now_ticks;
    interval_frames_ = 0;
    interval_worst_ticks_ = 0;
    return;
  }

  const double average_ms = elapsed_ms / static_cast<double>(interval_frames_);
  const double average_fps = 1000.0 / average_ms;
  const double worst_ms = TicksToMs(interval_worst_ticks_, tick_frequency);

  average_ms_.store(average_ms, std::memory_order_relaxed);
  average_fps_.store(average_fps, std::memory_order_relaxed);
  worst_ms_.store(worst_ms, std::memory_order_relaxed);

  if (log_it) {
    REXLOG_INFO("FPS: {:.1f} avg ({:.2f} ms), worst frame {:.1f} ms over {} frames / {:.0f}s "
                "[{} hitches this session]",
                average_fps, average_ms, worst_ms, interval_frames_, elapsed_ms / 1000.0,
                hitches_.load(std::memory_order_relaxed));
  }

  interval_start_tick_ = now_ticks;
  interval_frames_ = 0;
  interval_worst_ticks_ = 0;

  std::lock_guard lock(callback_mutex_);
  if (summary_callback_)
    summary_callback_(Snapshot());
}

void FrameStats::SetSummaryCallback(std::function<void(const Sample&)> callback) {
  std::lock_guard lock(callback_mutex_);
  summary_callback_ = std::move(callback);
}

FrameStats::Sample FrameStats::Snapshot() const {
  Sample sample;
  sample.average_fps = average_fps_.load(std::memory_order_relaxed);
  sample.average_ms = average_ms_.load(std::memory_order_relaxed);
  sample.worst_ms = worst_ms_.load(std::memory_order_relaxed);
  sample.hitches = hitches_.load(std::memory_order_relaxed);
  sample.total_frames = total_frames_.load(std::memory_order_relaxed);
  sample.uptime_seconds = uptime_seconds_.load(std::memory_order_relaxed);
  return sample;
}

}  // namespace rexcrash
