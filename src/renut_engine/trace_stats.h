#pragma once

#include <string>
#include <unordered_map>

namespace renut::trace_stats {

struct Snapshot {
    bool available = false;
    std::string path;
    std::unordered_map<std::string, double> values;
};

// Latest per-frame GPU trace row, updated live from the render thread via
// RenutEmitTraceRow (see rex/graphics/vulkan/renut_trace_hook.h and this
// file's .cpp) -- no file on disk, no polling.
Snapshot GetLatest();

}
