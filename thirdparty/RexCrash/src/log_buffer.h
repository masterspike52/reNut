// The log kept in memory for reports: a log sink that writes nothing to disk, and only turns its lines into text
// when a report asks for them.
//
// Two parts:
//   header  the startup lines (machine, mods, build), frozen by PinPrefix and never dropped, so they head every
//           report however long the game has run
//   ring    the most recent lines, up to a byte budget

#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <format>
#include <mutex>
#include <string>

#include <rex/logging/logger.h>

namespace rexcrash {

class LogBuffer final : public rex::log::ILogSink {
 public:
  static constexpr size_t kHeaderBytes = 64 * 1024;

  explicit LogBuffer(size_t ring_bytes) : ring_budget_(ring_bytes) {}

  // Applies a new ring budget, dropping the oldest lines that no longer fit.
  void SetRingBudget(size_t ring_bytes) {
    std::lock_guard lock(data_mutex_);
    ring_budget_ = ring_bytes;
    TrimLocked();
  }

  // Moves everything so far into the header. Called once the startup lines are in.
  void PinPrefix() {
    std::lock_guard lock(data_mutex_);
    for (auto& line : ring_) {
      if (header_bytes_ + line.size() + 1 > kHeaderBytes) break;
      header_bytes_ += line.size() + 1;
      header_.push_back(std::move(line));
    }
    ring_.clear();
    ring_bytes_ = 0;
  }

  // The header then the ring, oldest first, with a note where lines were dropped.
  std::string Snapshot() const {
    std::lock_guard lock(data_mutex_);
    std::string out;
    out.reserve(header_bytes_ + ring_bytes_ + 128);
    for (const auto& line : header_) out += line + '\n';
    if (dropped_) out += std::format("... {} earlier line(s) dropped from the in-memory log ...\n", dropped_);
    for (const auto& line : ring_) out += line + '\n';
    return out;
  }

  // Snapshot straight to a file, for the crash path: no large allocation, and a lock held by the thread that just
  // died gives up rather than hanging the crash handler.
  bool TryWriteTo(std::FILE* file) const {
    std::unique_lock lock(data_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !file) return false;
    for (const auto& line : header_) {
      std::fwrite(line.data(), 1, line.size(), file);
      std::fputc('\n', file);
    }
    for (const auto& line : ring_) {
      std::fwrite(line.data(), 1, line.size(), file);
      std::fputc('\n', file);
    }
    return true;
  }

  size_t ByteSize() const {
    std::lock_guard lock(data_mutex_);
    return header_bytes_ + ring_bytes_;
  }

  void Write(const rex::log::Message& message) override {
    std::string line = FormatFileLine(message);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    std::lock_guard lock(data_mutex_);
    ring_bytes_ += line.size() + 1;
    ring_.push_back(std::move(line));
    TrimLocked();
  }

  void Flush() override {}

 private:
  // The layout of the SDK's log file: date and time, level letter, category, thread, text.
  static std::string FormatFileLine(const rex::log::Message& message) {
    rex::log::Message stamp = message;
    stamp.text = {};
    std::string line = rex::log::FormatLine(
        stamp, {.timestamp = rex::log::TimestampStyle::kDateTime, .category = false, .thread = false, .color = false});
    line += rex::log::LevelLetter(message.level);
    line += ' ';
    line += rex::log::FormatLine(
        message, {.timestamp = rex::log::TimestampStyle::kNone, .category = true, .thread = true, .color = false});
    return line;
  }

  void TrimLocked() {
    while (ring_bytes_ > ring_budget_ && !ring_.empty()) {
      ring_bytes_ -= ring_.front().size() + 1;
      ring_.pop_front();
      ++dropped_;
    }
  }

  std::deque<std::string> header_;
  std::deque<std::string> ring_;
  size_t header_bytes_ = 0;
  size_t ring_bytes_ = 0;
  size_t ring_budget_;
  uint64_t dropped_ = 0;
  mutable std::mutex data_mutex_;
};

}  // namespace rexcrash
