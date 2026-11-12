#pragma once

struct ImGuiContext;

namespace renut::gpu::overlay {

// Lets the SDK context's lists grow past 65536 vertices and cuts each such list into lists drawn from vertex 0.
// UI thread, once, after RexGlass's hook (rexglass::Install).
void SplitLargeDrawLists(ImGuiContext *context);

} // namespace renut::gpu::overlay
