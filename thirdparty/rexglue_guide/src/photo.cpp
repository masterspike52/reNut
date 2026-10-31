#include "photo.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>


#include "image.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace rexglue_guide::photo {
namespace {

// Resamples the square [x0, x0 + side) x [y0, y0 + side) of an RGBA image to
// out_w x out_h: box-averaged when shrinking, bilinear when growing.
std::vector<uint8_t> Resample(const std::vector<uint8_t>& src, int width, int x0, int y0, int side,
                              int out_w = kSize, int out_h = kSize, int side_h = 0) {
  if (!side_h) side_h = side;
  std::vector<uint8_t> out(size_t(out_w) * out_h * 4);
  const float scale = float(side) / float(out_w);
  for (int y = 0; y < out_h; ++y) {
    for (int x = 0; x < out_w; ++x) {
      float sum[4] = {};
      if (scale > 1.0f) {
        // (Rounding the output size can reach a pixel past the edge.)
        const int sx0 = std::min(x0 + int(x * scale), x0 + side - 1);
        const int sy0 = std::min(y0 + int(y * scale), y0 + side_h - 1);
        const int sx1 = std::clamp(x0 + int((x + 1) * scale), sx0 + 1, x0 + side);
        const int sy1 = std::clamp(y0 + int((y + 1) * scale), sy0 + 1, y0 + side_h);
        for (int sy = sy0; sy < sy1; ++sy) {
          for (int sx = sx0; sx < sx1; ++sx) {
            const uint8_t* p = &src[(size_t(sy) * width + sx) * 4];
            for (int c = 0; c < 4; ++c) sum[c] += p[c];
          }
        }
        const float n = float((sx1 - sx0) * (sy1 - sy0));
        for (float& v : sum) v /= n;
      } else {
        const float fx = std::clamp((x + 0.5f) * scale - 0.5f, 0.0f, float(side - 1));
        const float fy = std::clamp((y + 0.5f) * scale - 0.5f, 0.0f, float(side_h - 1));
        const int ix = int(fx), iy = int(fy);
        const int jx = std::min(ix + 1, side - 1), jy = std::min(iy + 1, side_h - 1);
        const float tx = fx - ix, ty = fy - iy;
        auto at = [&](int px, int py) { return &src[(size_t(y0 + py) * width + x0 + px) * 4]; };
        for (int c = 0; c < 4; ++c) {
          const float top = at(ix, iy)[c] * (1 - tx) + at(jx, iy)[c] * tx;
          const float bottom = at(ix, jy)[c] * (1 - tx) + at(jx, jy)[c] * tx;
          sum[c] = top * (1 - ty) + bottom * ty;
        }
      }
      uint8_t* d = &out[(size_t(y) * out_w + x) * 4];
      for (int c = 0; c < 4; ++c) d[c] = uint8_t(std::clamp(sum[c] + 0.5f, 0.0f, 255.0f));
      d[3] = 255;  // gamerpics are opaque
    }
  }
  return out;
}

// The file's pixels, or empty with *error set.
std::vector<uint8_t> Decode(const std::filesystem::path& path, size_t max_bytes, int* w, int* h, std::string* error) {
  std::error_code ec;
  const auto bytes = std::filesystem::file_size(path, ec);
  if (ec) {
    *error = "That file can't be opened.";
    return {};
  }
  if (bytes > max_bytes) {
    *error = "Pictures can be up to " + std::to_string(max_bytes >> 20) + " MB.";
    return {};
  }
  std::ifstream file(path, std::ios::binary);
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  std::vector<uint8_t> rgba = image::Decode(data.data(), data.size(), w, h);
  if (rgba.empty() || *w <= 0 || *h <= 0) {
    *error = "That file isn't a picture the Guide can read (PNG, JPEG, BMP, GIF or TGA).";
    return {};
  }
  return rgba;
}

}  // namespace

std::string BackgroundFromFile(const std::filesystem::path& path, std::string* error) {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba = Decode(path, kMaxBackgroundFileBytes, &w, &h, error);
  if (rgba.empty()) return {};
  const float fit = std::min({1.0f, 1920.0f / float(w), 1080.0f / float(h)});
  const int out_w = std::max(1, int(float(w) * fit + 0.5f)), out_h = std::max(1, int(float(h) * fit + 0.5f));
  if (out_w != w || out_h != h) rgba = Resample(rgba, w, 0, 0, w, out_w, out_h, h);
  // JPEG has no alpha: blend any see-through parts over black.
  std::vector<uint8_t> rgb(size_t(out_w) * out_h * 3);
  for (size_t i = 0, n = size_t(out_w) * out_h; i < n; ++i) {
    for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = uint8_t(rgba[i * 4 + c] * rgba[i * 4 + 3] / 255);
  }
  std::string jpg;
  stbi_write_jpg_to_func(
      [](void* context, void* chunk, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(chunk), size_t(size));
      },
      &jpg, out_w, out_h, 3, rgb.data(), 88);
  if (jpg.empty()) *error = "The picture couldn't be prepared.";
  return jpg;
}

std::string FromFile(const std::filesystem::path& path, std::string* error) {
  std::error_code ec;
  const auto bytes = std::filesystem::file_size(path, ec);
  if (ec) {
    *error = "That file can't be opened.";
    return {};
  }
  if (bytes > kMaxFileBytes) {
    *error = "Photos can be up to 5 MB.";
    return {};
  }
  std::ifstream file(path, std::ios::binary);
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  int w = 0, h = 0;
  const std::vector<uint8_t> rgba = image::Decode(data.data(), data.size(), &w, &h);
  if (rgba.empty() || w <= 0 || h <= 0) {
    *error = "That file isn't a picture the Guide can read (PNG, JPEG, BMP, GIF or TGA).";
    return {};
  }
  const int side = std::min(w, h);
  const std::vector<uint8_t> square = Resample(rgba, w, (w - side) / 2, (h - side) / 2, side);
  std::string png;
  stbi_write_png_to_func(
      [](void* context, void* chunk, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(chunk), size_t(size));
      },
      &png, kSize, kSize, 4, square.data(), kSize * 4);
  if (png.empty()) *error = "The photo couldn't be prepared.";
  return png;
}

}  // namespace rexglue_guide::photo
