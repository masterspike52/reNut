#pragma once
#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>
#include <rex/utility.h>
#include "imgui.h"
#include "renut_engine/app/renut_logging.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

#include <deque>
#include <memory>
#include <mutex>
#include <string>

// ── Ring-buffer log sink ──────────────────────────────────────────────────────

struct RenuLogEntry {
    rex::log::Level level;
    std::string     message;
};

class RenuLogOverlaySink final : public rex::log::ILogSink {
public:
    static constexpr std::size_t kMaxEntries = 200;

    void clear() {
        std::lock_guard lock(mutex_);
        entries_.clear();
    }

    template<typename Fn>
    void with_entries(Fn&& fn) {
        std::lock_guard lock(mutex_);
        fn(entries_);
    }

    // reNut's own category only, as the old per-category sink took.
    void Write(const rex::log::Message& message) override {
        if (message.category != rex::log::renut_hooks.GetName())
            return;
        std::string line = rex::log::FormatLine(
            message, {.timestamp = rex::log::TimestampStyle::kDateTime, .category = true, .color = false});
        std::lock_guard lock(mutex_);
        if (entries_.size() >= kMaxEntries)
            entries_.pop_front();
        entries_.push_back({message.level, std::move(line)});
    }

    void Flush() override {}

private:
    std::mutex               mutex_;
    std::deque<RenuLogEntry> entries_;
};

// ── Overlay dialog ────────────────────────────────────────────────────────────

class RenuLogOverlayDialog : public renut::gpu::overlay::Dialog {
public:
    RenuLogOverlayDialog()
        : sink_(std::make_unique<RenuLogOverlaySink>()),
          guard_(REX_LOGGER().AddSink(*sink_))
    {
    }

    void Draw() override {
        // The overlay's own input (gpu/overlay_ui.cpp feeds it the window's keys).
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
            visible_ = !visible_;
        if (!visible_) return;

        ImGui::SetNextWindowSize(ImVec2(600.0f, 300.0f), ImGuiCond_Once);
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.75f);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus;

        if (!ImGui::Begin("renut log##renut_log_overlay", &visible_, flags)) {
            ImGui::End();
            return;
        }

        // ── Toolbar ──────────────────────────────────────────────────────────
        if (ImGui::Button("Clear"))
            sink_->clear();

        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &auto_scroll_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::Combo("Min level", &min_level_idx_,
            "Trace\0Dev\0Debug\0Info\0Warn\0Error\0Critical\0\0");
        ImGui::Separator();

        // ── Log lines ────────────────────────────────────────────────────────
        ImGui::BeginChild("##log_scroll", ImVec2(0.0f, 0.0f), false,
            ImGuiWindowFlags_HorizontalScrollbar);

        sink_->with_entries([&](const std::deque<RenuLogEntry>& entries) {
            for (const auto& e : entries) {
                if (static_cast<int>(e.level) < min_level_idx_)
                    continue;
                ImGui::TextColored(LevelColor(e.level), "[%s]", LevelTag(e.level));
                ImGui::SameLine();
                ImGui::TextUnformatted(e.message.c_str());
            }
            });

        if (auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndChild();
        ImGui::End();
    }

private:
    static ImVec4 LevelColor(rex::log::Level level) {
        switch (level) {
        case rex::log::Level::kTrace:    return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        case rex::log::Level::kDev:      return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        case rex::log::Level::kDebug:    return ImVec4(0.4f, 0.8f, 1.0f, 1.0f);
        case rex::log::Level::kInfo:     return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        case rex::log::Level::kWarn:     return ImVec4(1.0f, 0.85f, 0.0f, 1.0f);
        case rex::log::Level::kError:    return ImVec4(1.0f, 0.25f, 0.25f, 1.0f);
        case rex::log::Level::kCritical: return ImVec4(1.0f, 0.0f, 0.5f, 1.0f);
        default:                         return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        }
    }

    static const char* LevelTag(rex::log::Level level) {
        switch (level) {
        case rex::log::Level::kTrace:    return "TRACE";
        case rex::log::Level::kDev:      return "  DEV";
        case rex::log::Level::kDebug:    return "DEBUG";
        case rex::log::Level::kInfo:     return " INFO";
        case rex::log::Level::kWarn:     return " WARN";
        case rex::log::Level::kError:    return "ERROR";
        case rex::log::Level::kCritical: return " CRIT";
        default:                         return "  ???";
        }
    }

    bool visible_ = false;
    bool auto_scroll_ = true;
    int  min_level_idx_ = 0;

    // The guard after the sink, so it goes first.
    std::unique_ptr<RenuLogOverlaySink> sink_;
    rex::ScopeGuard                     guard_;
};
