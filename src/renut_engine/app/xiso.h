#pragma once
// Extracts an Xbox 360 disc image (XDVDFS, what extract-xiso reads) to a
// folder: the game data reNut runs from. Redump images (XGD2/XGD3 with the
// video partition in front) and trimmed xiso images both work.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace renut::xiso {

// Shared with the UI while Extract runs on a worker thread.
struct Progress {
    std::atomic<uint64_t> done{0};   // bytes written
    std::atomic<uint64_t> total{0};  // bytes to write
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::string current;  // file being written
    std::string error;    // empty on success
};

// True when every file was written. On failure progress.error says why.
bool Extract(const std::filesystem::path &iso, const std::filesystem::path &out, Progress &progress);

} // namespace renut::xiso
