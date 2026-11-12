/**
 * @file        rex/perf/frame_stats.h
 * @brief       Always-on frame pacing summary for logs and crash reports
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

#include <rex/setting.h>

namespace rexcrash {

// The owner of fps_log_interval and fps_hitch_ms (scope "rexcrash"), registered by Configure.
rex::SettingOwner& FrameStatsSettings();

/**
 * Frame pacing summary, published once per interval rather than per frame.
 *
 * The point is to leave a usable performance trail in the log without flooding
 * it: one INFO line per `fps_log_interval` seconds, plus a rate-limited WARN
 * when a single frame blows past `fps_hitch_ms`. Crash and bug reports read
 * Snapshot() to tag the report with how the game was running at the time.
 *
 * OnPresent() is called from the GPU worker thread; Snapshot() is safe from
 * any thread.
 */
class FrameStats {
 public:
  struct Sample {
    double average_fps = 0.0;    ///< Over the last completed interval.
    double average_ms = 0.0;     ///< Mean frame time over that interval.
    double worst_ms = 0.0;       ///< Slowest single frame in that interval.
    uint64_t hitches = 0;        ///< Frames over fps_hitch_ms, session total.
    uint64_t total_frames = 0;   ///< Session total.
    double uptime_seconds = 0.0; ///< Since the first presented frame.
  };

  static FrameStats& Global();

  /// Record a presented frame. Emits the periodic summary when due.
  void OnPresent();

  /// Most recently published interval summary plus session totals.
  Sample Snapshot() const;

  /// Emit the periodic summary now, whatever the interval, and start a new one.
  /// Used when a report is being assembled so it carries current numbers.
  void FlushSummary();

  /// Called on the presenting thread every time an interval summary is
  /// published. Telemetry uses it to keep the frame rate tag on crash reports
  /// current without polling. Pass {} to clear.
  void SetSummaryCallback(std::function<void(const Sample&)> callback);

 private:
  void PublishInterval(uint64_t now_ticks, uint64_t tick_frequency, bool log_it);

  // Written only by the presenting thread.
  uint64_t first_frame_tick_ = 0;
  uint64_t last_frame_tick_ = 0;
  uint64_t interval_start_tick_ = 0;
  uint64_t interval_frames_ = 0;
  uint64_t interval_worst_ticks_ = 0;
  uint64_t last_hitch_log_tick_ = 0;

  // Published snapshot, read from other threads.
  std::atomic<double> average_fps_{0.0};
  std::atomic<double> average_ms_{0.0};
  std::atomic<double> worst_ms_{0.0};
  std::atomic<uint64_t> hitches_{0};
  std::atomic<uint64_t> total_frames_{0};
  std::atomic<double> uptime_seconds_{0.0};

  std::function<void(const Sample&)> summary_callback_;
  mutable std::mutex callback_mutex_;
};

}  // namespace rexcrash
