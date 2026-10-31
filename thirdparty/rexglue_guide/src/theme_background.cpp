#include "theme_background.h"

#include <algorithm>
#include <cmath>

#include "image.h"

namespace rexglue_guide {
namespace {

constexpr const char* kTextureKey = "theme_background";
// Full blur: this many pixels of a 1080-line picture.
constexpr float kMaxBlurRadius = 48.0f;

// One box-blur pass along rows (or columns, through the strides), radius r,
// edges clamped.
void BoxPass(const uint8_t* src, uint8_t* dst, int count, int length, size_t line_stride, size_t step, int r) {
  const int window = 2 * r + 1;
  for (int line = 0; line < count; ++line) {
    const uint8_t* s = src + line * line_stride;
    uint8_t* d = dst + line * line_stride;
    for (int c = 0; c < 4; ++c) {
      auto at = [&](int i) { return int(s[size_t(std::clamp(i, 0, length - 1)) * step + c]); };
      int sum = 0;
      for (int i = -r; i <= r; ++i) sum += at(i);
      for (int i = 0; i < length; ++i) {
        d[size_t(i) * step + c] = uint8_t(sum / window);
        sum += at(i + r + 1) - at(i - r);
      }
    }
  }
}

// Three box passes each way: close to a Gaussian.
void Blur(std::vector<uint8_t>& rgba, int w, int h, int r) {
  if (r <= 0) return;
  std::vector<uint8_t> tmp(rgba.size());
  for (int pass = 0; pass < 3; ++pass) {
    BoxPass(rgba.data(), tmp.data(), h, w, size_t(w) * 4, 4, r);
    BoxPass(tmp.data(), rgba.data(), w, h, 4, size_t(w) * 4, r);
  }
}

// Halves the picture (2 x 2 averages).
std::vector<uint8_t> Half(const std::vector<uint8_t>& src, int w, int h, int* out_w, int* out_h) {
  *out_w = std::max(1, w / 2);
  *out_h = std::max(1, h / 2);
  std::vector<uint8_t> out(size_t(*out_w) * *out_h * 4);
  for (int y = 0; y < *out_h; ++y) {
    for (int x = 0; x < *out_w; ++x) {
      for (int c = 0; c < 4; ++c) {
        auto at = [&](int px, int py) {
          return int(src[(size_t(std::min(py, h - 1)) * w + std::min(px, w - 1)) * 4 + c]);
        };
        const int sum = at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1);
        out[(size_t(y) * *out_w + x) * 4 + c] = uint8_t(sum / 4);
      }
    }
  }
  return out;
}

// Thumbnails are at most this wide.
constexpr int kThumbWidth = 360;

}  // namespace

ImTextureID ThemeThumbnails::Get(ui::Textures& textures, const std::string& key, const std::string& image, float blur,
                                 ImVec2* size) {
  if (auto it = thumbs_.find(key); it != thumbs_.end()) {
    *size = it->second.size;
    return it->second.texture;
  }
  if (image.empty()) return 0;
  int w = 0, h = 0;
  std::vector<uint8_t> rgba = rexglue_guide::image::Decode(image.data(), image.size(), &w, &h);
  Thumb& thumb = thumbs_[key];  // made once, even if it won't decode
  if (rgba.empty()) return 0;
  while (w > kThumbWidth) rgba = Half(rgba, w, h, &w, &h);
  // Blurred as the theme's picture is, for its size.
  const int radius = int(std::round(std::clamp(blur, 0.0f, 1.0f) * kMaxBlurRadius * float(h) / 1080.0f / 3.0f));
  Blur(rgba, w, h, radius);
  thumb.texture = textures.Rgba("theme_thumb:" + key, rgba.data(), w, h);
  thumb.size = ImVec2(float(w), float(h));
  *size = thumb.size;
  return thumb.texture;
}

ImTextureID ThemeBackground::Get(ui::Textures& textures, const std::string& id, const std::string& image, float blur,
                                 ImVec2* size) {
  if (id.empty() || image.empty()) return 0;
  if (id != id_) {
    id_ = id;
    width_ = height_ = 0;
    rgba_ = rexglue_guide::image::Decode(image.data(), image.size(), &width_, &height_);
    blur_ = -1.0f;
  }
  if (rgba_.empty()) return 0;
  // Steps of 1%: dragging the slider doesn't blur every frame for nothing.
  blur = std::round(std::clamp(blur, 0.0f, 1.0f) * 100.0f) / 100.0f;
  if (blur != blur_ || !texture_) {
    blur_ = blur;
    if (blur <= 0.0f) {
      texture_ = textures.Rgba(kTextureKey, rgba_.data(), width_, height_);
    } else {
      // Blurred at half size: quicker, and nothing sharp is lost.
      int w = 0, h = 0;
      std::vector<uint8_t> small = Half(rgba_, width_, height_, &w, &h);
      const int radius = int(std::round(blur * kMaxBlurRadius * float(h) / 540.0f / 3.0f));
      Blur(small, w, h, std::max(1, radius));
      texture_ = textures.Rgba(kTextureKey, small.data(), w, h);
    }
  }
  *size = ImVec2(float(width_), float(height_));
  return texture_;
}

}  // namespace rexglue_guide
