#include "image_decode.h"

#include <climits>

// Static, so it can sit beside another copy of stb_image in the same program.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>

namespace rexfabric::ui {

std::vector<uint8_t> DecodeImageRGBA(const uint8_t* data, size_t size, int& width, int& height) {
  width = 0;
  height = 0;
  if (!data || size == 0 || size > static_cast<size_t>(INT_MAX)) return {};
  int channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &channels, 4);
  if (!pixels) {
    width = 0;
    height = 0;
    return {};
  }
  std::vector<uint8_t> rgba(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
  stbi_image_free(pixels);
  return rgba;
}

}  // namespace rexfabric::ui
