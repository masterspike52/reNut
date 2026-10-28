#pragma once
// A player's own gamerpic from an image file on disk.

#include <filesystem>
#include <string>

namespace rexglue_guide::photo {

constexpr size_t kMaxFileBytes = 5 << 20;
constexpr size_t kMaxBackgroundFileBytes = 20 << 20;
constexpr int kSize = 256;

// The image at `path` centre-cropped to a square and scaled to kSize x kSize,
// as a PNG. Empty, with *error set, if the file is too big or not an image.
std::string FromFile(const std::filesystem::path& path, std::string* error);

// A theme's background picture from the image at `path`: scaled down to fit in
// 1920 x 1080 (never cropped), as a JPEG. Empty, with *error set, if the file
// is too big or not an image.
std::string BackgroundFromFile(const std::filesystem::path& path, std::string* error);

}  // namespace rexglue_guide::photo
