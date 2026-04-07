// renut_log_overlay.h
#pragma once
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/logging.h>
#include "imgui.h"
#include "renut_engine/renut_logging.h"

#include <spdlog/sinks/base_sink.h>
#include <deque>
#include <mutex>
#include <string>

// ── Ring-buffer spdlog sink ───────────────────────────────────────────────────

struct RenuLogEntry {
    spdlog::level::level_enum level;
    std::string               message;
};

class RenuLogOverlaySink : public spdlog::sinks::base_sink<std::mutex> {
public:
    static constexpr std::size_t kMaxEntries = 200;

    std::deque<RenuLogEntry> entries;

    void clear() {
        std::lock_guard lock(mutex_);
        entries.clear();
    }

    template<typename Fn>
    void with_entries(Fn&& fn) {
        std::lock_guard lock(mutex_);
        fn(entries);
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t buf;
        formatter_->format(msg, buf);
        if (entries.size() >= kMaxEntries)
            entries.pop_front();
        entries.push_back({ msg.level, std::string(buf.data(), buf.size()) });
    }
    void flush_() override {}
};

// ── Overlay dialog ────────────────────────────────────────────────────────────

class RenuLogOverlayDialog : public rex::ui::ImGuiDialog {
public:
    explicit RenuLogOverlayDialog(rex::ui::ImGuiDrawer* drawer)
        : rex::ui::ImGuiDialog(drawer)
        , sink_(std::make_shared<RenuLogOverlaySink>())
    {
        rex::ui::RegisterBind("bind_renut_log_overlay", "F2",
            "Toggle renut log overlay", [this] {
                visible_ = !visible_;
            });

        rex::AddSink(renut::log::Input, sink_);
    }

    ~RenuLogOverlayDialog() {
        rex::ui::UnregisterBind("bind_renut_log_overlay");
        rex::RemoveSink(renut::log::Input, sink_);
    }

    void OnDraw(ImGuiIO& io) override {
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
            "Trace\0Debug\0Info\0Warn\0Error\0Critical\0\0");
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
    static ImVec4 LevelColor(spdlog::level::level_enum level) {
        switch (level) {
        case spdlog::level::trace:    return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        case spdlog::level::debug:    return ImVec4(0.4f, 0.8f, 1.0f, 1.0f);
        case spdlog::level::info:     return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        case spdlog::level::warn:     return ImVec4(1.0f, 0.85f, 0.0f, 1.0f);
        case spdlog::level::err:      return ImVec4(1.0f, 0.25f, 0.25f, 1.0f);
        case spdlog::level::critical: return ImVec4(1.0f, 0.0f, 0.5f, 1.0f);
        default:                      return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        }
    }

    static const char* LevelTag(spdlog::level::level_enum level) {
        switch (level) {
        case spdlog::level::trace:    return "TRACE";
        case spdlog::level::debug:    return "DEBUG";
        case spdlog::level::info:     return " INFO";
        case spdlog::level::warn:     return " WARN";
        case spdlog::level::err:      return "ERROR";
        case spdlog::level::critical: return " CRIT";
        default:                      return "  ???";
        }
    }

    bool visible_ = false;
    bool auto_scroll_ = true;
    int  min_level_idx_ = 0;

    std::shared_ptr<RenuLogOverlaySink> sink_;
};