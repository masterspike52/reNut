#include "renut_engine/game_activity_stats.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace renut::game_activity_stats {

namespace {

struct Counters {
    std::atomic_uint64_t frame = 0;
    std::atomic_uint64_t session = 0;
    std::atomic_uint64_t frame_ns = 0;
};

std::atomic_bool g_enabled = false;
Counters g_animation_stream;
Counters g_body_blend;
Counters g_actor_script;
Counters g_actor_generation;
Snapshot g_snapshot;
std::mutex g_snapshot_mutex;

bool Record(Counters& counters) {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return false;
    }
    counters.frame.fetch_add(1, std::memory_order_relaxed);
    counters.session.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Finish(Counters& counters, uint64_t start) {
    if (!start || !g_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    counters.frame_ns.fetch_add(now - start, std::memory_order_relaxed);
}

uint64_t TakeFrame(Counters& counters) {
    return counters.frame.exchange(0, std::memory_order_relaxed);
}

uint64_t Session(const Counters& counters) {
    return counters.session.load(std::memory_order_relaxed);
}

uint64_t TakeFrameTime(Counters& counters) {
    return counters.frame_ns.exchange(0, std::memory_order_relaxed);
}

thread_local uint64_t g_animation_stream_start = 0;
thread_local uint64_t g_body_blend_start = 0;
thread_local uint64_t g_actor_script_start = 0;
thread_local uint64_t g_actor_generation_start = 0;

uint64_t Now() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}

void SetEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
}

void RecordAnimationStream() {
    if (Record(g_animation_stream)) {
        g_animation_stream_start = Now();
    }
}

void FinishAnimationStream() {
    Finish(g_animation_stream, g_animation_stream_start);
    g_animation_stream_start = 0;
}

void RecordBodyBlend() {
    if (Record(g_body_blend)) {
        g_body_blend_start = Now();
    }
}

void FinishBodyBlend() {
    Finish(g_body_blend, g_body_blend_start);
    g_body_blend_start = 0;
}

void RecordActorScript() {
    if (Record(g_actor_script)) {
        g_actor_script_start = Now();
    }
}

void FinishActorScript() {
    Finish(g_actor_script, g_actor_script_start);
    g_actor_script_start = 0;
}

void RecordActorGeneration() {
    if (Record(g_actor_generation)) {
        g_actor_generation_start = Now();
    }
}

void FinishActorGeneration() {
    Finish(g_actor_generation, g_actor_generation_start);
    g_actor_generation_start = 0;
}

void EndFrame() {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    Snapshot snapshot;
    snapshot.animation_stream_frame = TakeFrame(g_animation_stream);
    snapshot.animation_stream_session = Session(g_animation_stream);
    snapshot.animation_stream_frame_ns = TakeFrameTime(g_animation_stream);
    snapshot.body_blend_frame = TakeFrame(g_body_blend);
    snapshot.body_blend_session = Session(g_body_blend);
    snapshot.body_blend_frame_ns = TakeFrameTime(g_body_blend);
    snapshot.actor_script_frame = TakeFrame(g_actor_script);
    snapshot.actor_script_session = Session(g_actor_script);
    snapshot.actor_script_frame_ns = TakeFrameTime(g_actor_script);
    snapshot.actor_generation_frame = TakeFrame(g_actor_generation);
    snapshot.actor_generation_session = Session(g_actor_generation);
    snapshot.actor_generation_frame_ns = TakeFrameTime(g_actor_generation);
    std::scoped_lock lock(g_snapshot_mutex);
    g_snapshot = snapshot;
}

Snapshot GetSnapshot() {
    std::scoped_lock lock(g_snapshot_mutex);
    return g_snapshot;
}

}
