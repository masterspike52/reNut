#pragma once
// A theme's background picture as a texture: decoded once, blurred to the
// theme's setting (again only when that changes).

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "ui.h"

namespace rexglue_guide {

// Small copies of themes' pictures for the theme gallery, one texture each.
class ThemeThumbnails {
 public:
  // `key` names the picture (its theme and version); `image` its file, empty
  // while it downloads. Null until there's one; *size is the thumbnail's.
  ImTextureID Get(ui::Textures& textures, const std::string& key, const std::string& image, float blur, ImVec2* size);

 private:
  struct Thumb {
    ImTextureID texture = 0;
    ImVec2 size;
  };
  std::unordered_map<std::string, Thumb> thumbs_;
};

class ThemeBackground {
 public:
  // The picture `id` names (a new id decodes `image`, its JPEG or PNG file),
  // blurred by `blur` (0..1). Null if there's no picture; *size is its size.
  ImTextureID Get(ui::Textures& textures, const std::string& id, const std::string& image, float blur, ImVec2* size);

 private:
  std::string id_;
  std::vector<uint8_t> rgba_;
  int width_ = 0, height_ = 0;
  float blur_ = -1.0f;
  ImTextureID texture_ = 0;
};

}  // namespace rexglue_guide
