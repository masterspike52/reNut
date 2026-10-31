#include "image.h"

#include <climits>

// Static, so it can't clash with the stb_image rexglue builds in.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#include "stb/stb_image.h"

namespace rexglue_guide::image {

std::vector<uint8_t> Decode(const void* data, size_t size, int* width, int* height) {
  *width = *height = 0;
  if (!data || size == 0 || size > size_t(INT_MAX)) return {};
  int w = 0, h = 0, channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(data), int(size), &w, &h, &channels, 4);
  if (!pixels) return {};
  std::vector<uint8_t> rgba(pixels, pixels + size_t(w) * h * 4);
  stbi_image_free(pixels);
  *width = w;
  *height = h;
  return rgba;
}

}  // namespace rexglue_guide::image
