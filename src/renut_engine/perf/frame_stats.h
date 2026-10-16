#pragma once
// Frame statistics for the FPS overlay. A game frame is one D3DDevice_Swap:
// the frames the game actually finishes, whatever its update loop does.

namespace renut::frame_stats {

struct Snapshot {
    static constexpr int kHistory = 180;

    double fps = 0.0;        // game frames per second over the last second
    double frame_ms = 0.0;   // their average length
    double worst_ms = 0.0;   // the longest of them
    // Averages over the same frames. CPU is the game thread's update plus
    // render (appMainTickPreDraw + appMainDraw, wall time) less the time it
    // waited for the render thread (OnGameThreadWait); GPU is the time the
    // GPU spent executing each frame's command lists (timestamp queries).
    double cpu_ms = 0.0;
    double gpu_ms = 0.0;
    bool gpu_known = false;  // false until the renderer has reported a GPU time
    // The render thread's busy time per frame (it records the GPU work the
    // game thread queues), less its waits for the GPU and the display
    // (render_thread::WaitScope), averaged the same way.
    double render_thread_ms = 0.0;
    bool render_thread_known = false;
    double update_ms = 0.0;  // appMainTickPreDraw, last frame
    double render_ms = 0.0;  // appMainDraw, last frame (CPU time issuing the frame)
    float history[kHistory] = {};  // frame lengths in ms, oldest first
    int history_count = 0;
};

// Called by the renderer at each guest D3DDevice_Swap.
void OnGameFrame();

// Called by the renderer when a frame's GPU time is known (a frame or two
// after it was submitted).
void OnGpuFrame(double ms);

// Called by the render thread at the end of each frame's work.
void OnRenderThreadFrame(double busy_ms);

// Called by the renderer when the game thread has waited for the render
// thread (Swap's frame pacing, fence waits), for the CPU time to leave out.
void OnGameThreadWait(double ms);

Snapshot Get();

} // namespace renut::frame_stats
