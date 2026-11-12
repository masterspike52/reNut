#include "renut_engine/perf/frame_stats.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

#include <rex/ppc/hook.h>
#include <rex/setting.h>
#include <rexcrash/rexcrash.h>
#include <rexfabric/rexfabric.h>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/online/net_api.h"

namespace {
struct FrameStatsSettings final : rex::SettingOwner {
    FrameStatsSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
    REX_SETTING(bool, fps_overlay, false, "Shows the FPS overlay (toggle with F1).");
    REX_SETTING(f64, fps_overlay_size, 34.0,
                "Text size of the FPS overlay's frame rate, in pixels; the rest scales with it.");
};
FrameStatsSettings &g_frame_stats_settings = *new FrameStatsSettings();  // never freed, registered by RenutApp::OnConfigure
}  // namespace

void renutFrameLimit();  // frameHooks.cpp

namespace renut::frame_stats {

bool FpsOverlayEnabled() {
    return g_frame_stats_settings.fps_overlay.Get();
}

void SetFpsOverlayEnabled(bool on) {
    (void)g_frame_stats_settings.fps_overlay.Set(on);
}

double FpsOverlaySize() {
    return g_frame_stats_settings.fps_overlay_size.Get();
}

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kWindowMs = 1000.0;

std::mutex g_mutex;
Clock::time_point g_last_frame{};
bool g_have_frame = false;
float g_ring[Snapshot::kHistory] = {};
int g_ring_next = 0;
int g_ring_count = 0;
double g_update_ms = 0.0;
double g_render_ms = 0.0;
// The game thread's waits for the render thread since its frame began, and
// the game thread (the one running the update).
double g_wait_ms = 0.0;
std::thread::id g_game_thread;

// Recent CPU (update + render) and GPU frame times, newest last.
struct Recent {
    double ms[Snapshot::kHistory] = {};
    int next = 0;
    int count = 0;

    void Add(double value) {
        ms[next] = value;
        next = (next + 1) % Snapshot::kHistory;
        if (count < Snapshot::kHistory)
            ++count;
    }

    // Average of the newest 'n' (at least one when there are any).
    double Average(int n) const {
        if (!count)
            return 0.0;
        n = std::clamp(n, 1, count);
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
            sum += ms[(next - 1 - i + Snapshot::kHistory) % Snapshot::kHistory];
        return sum / n;
    }
};
Recent g_cpu;
Recent g_gpu;
Recent g_render_thread;

double MsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

} // namespace

void OnGpuFrame(double ms) {
    std::lock_guard lock(g_mutex);
    g_gpu.Add(ms);
}

void OnRenderThreadFrame(double busy_ms) {
    std::lock_guard lock(g_mutex);
    g_render_thread.Add(busy_ms);
}

void OnGameThreadWait(double ms) {
    std::lock_guard lock(g_mutex);
    if (std::this_thread::get_id() == g_game_thread)
        g_wait_ms += ms;
}

void OnGameFrame() {
    rexcrash::OnPresent();  // the frame rate crash reports carry
    const Clock::time_point now = Clock::now();
    std::lock_guard lock(g_mutex);
    if (g_have_frame) {
        g_ring[g_ring_next] =
            float(std::chrono::duration<double, std::milli>(now - g_last_frame).count());
        g_ring_next = (g_ring_next + 1) % Snapshot::kHistory;
        if (g_ring_count < Snapshot::kHistory)
            ++g_ring_count;
    }
    g_last_frame = now;
    g_have_frame = true;
}

Snapshot Get() {
    Snapshot s;
    std::lock_guard lock(g_mutex);
    s.update_ms = g_update_ms;
    s.render_ms = g_render_ms;
    s.history_count = g_ring_count;
    for (int i = 0; i < g_ring_count; ++i) {
        const int at = (g_ring_next - g_ring_count + i + Snapshot::kHistory) % Snapshot::kHistory;
        s.history[i] = g_ring[at];
    }
    if (!g_have_frame)
        return s;

    // The frames of the last second, counting the one in progress when it has
    // already run long (a stall shows at once instead of freezing the number).
    const double pending = MsSince(g_last_frame);
    double span = 0.0;
    int frames = 0;
    for (int i = s.history_count - 1; i >= 0 && span < kWindowMs; --i) {
        span += s.history[i];
        s.worst_ms = std::max(s.worst_ms, double(s.history[i]));
        ++frames;
    }
    if (frames && pending > s.history[s.history_count - 1]) {
        span += pending;
        s.worst_ms = std::max(s.worst_ms, pending);
    }
    if (frames && span > 0.0) {
        s.fps = frames * 1000.0 / span;
        s.frame_ms = span / frames;
    }
    s.cpu_ms = g_cpu.Average(frames);
    s.gpu_ms = g_gpu.Average(frames);
    s.gpu_known = g_gpu.count != 0;
    s.render_thread_ms = g_render_thread.Average(frames);
    s.render_thread_known = g_render_thread.count != 0;
    return s;
}

} // namespace renut::frame_stats

// The game's update, then its render. Timed here, and the frame cap applied
// after the render.
REX_EXTERN(__imp__rex_appMainTickPreDraw);
REX_HOOK_RAW(rex_appMainTickPreDraw) {
    {
        std::lock_guard lock(renut::frame_stats::g_mutex);
        renut::frame_stats::g_wait_ms = 0.0;
        renut::frame_stats::g_game_thread = std::this_thread::get_id();
    }
    const auto start = renut::frame_stats::Clock::now();
    __imp__rex_appMainTickPreDraw(ctx, base);
    const double ms = renut::frame_stats::MsSince(start);
    {
        std::lock_guard lock(renut::frame_stats::g_mutex);
        renut::frame_stats::g_update_ms = ms;
    }
    renut::online::net_api::Tick(ctx, base);  // renut.net's messages, after the game's update
}

REX_EXTERN(__imp__rex_appMainDraw);
REX_HOOK_RAW(rex_appMainDraw) {
    const auto start = renut::frame_stats::Clock::now();
    __imp__rex_appMainDraw(ctx, base);
    const double ms = renut::frame_stats::MsSince(start);
    {
        std::lock_guard lock(renut::frame_stats::g_mutex);
        renut::frame_stats::g_render_ms = ms;
        renut::frame_stats::g_cpu.Add(std::max(
            renut::frame_stats::g_update_ms + ms - renut::frame_stats::g_wait_ms, 0.0));
    }
    // The mods' frame event (RexFabric), on the game thread after its render.
    rexfabric::Tick();
    renutFrameLimit();
}
