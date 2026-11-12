// Texture packs: a game's textures dumped to files, and replaced from the textures folders of the enabled mods.
//
// A texture is known by a content hash: XXH3 over its raw guest bytes, tiled and big endian, exactly as the
// Xenos GPU reads them. The hash doesn't depend on where the texture sits in memory or on the run, so a file
// named after it replaces that texture wherever it turns up.
//
//   Dumps        <root>/dumps/textures/<hash16>_<w>x<h>_<format>.dds   (or .png with texture_dump_format = png)
//   Replacements mods/<mod>/textures/<hash16>.dds or .png             (an earlier mod in the load order wins)
//
// Compressed formats dump as DDS with their raw BC blocks, lossless and ready to edit; the rest dump as RGBA8,
// untiled, byte swapped and widened to 8 bits a channel. PNG dumps are RGBA8 throughout, BC formats decoded in
// software.
//
// For a game whose own renderer uploads textures: it hashes the guest bytes, asks FindReplacement, and dumps
// what it uploads while texture_dump_enabled is on. Video replacements (.mp4) are skipped: the SDK's FFmpeg has no
// video decoder.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>

REXCVAR_DECLARE(bool, texture_dump_enabled);
REXCVAR_DECLARE(bool, texture_load_enabled);
REXCVAR_DECLARE(std::string, texture_dump_format);
REXCVAR_DECLARE(bool, texture_dump_skip_video_sizes);

namespace rexfabric {

struct TextureReplacementData {
  // RGBA8 unorm, rows top first, tightly packed.
  std::vector<uint8_t> pixels;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mip_levels = 1;
};

class TextureReplacement {
 public:
  // `root` is where dumps go, under dumps/textures. The replacements come from the enabled mods.
  explicit TextureReplacement(std::filesystem::path root);
  ~TextureReplacement();

  TextureReplacement(const TextureReplacement&) = delete;
  TextureReplacement& operator=(const TextureReplacement&) = delete;

  // Rebuilds the index of replacement files.
  void Rescan();

  // Untiles, byte swaps and writes the guest texture to a file.
  //   guest_bytes   the raw guest memory at the texture's base
  //   pitch_blocks  the pitch in units of 32 texels
  void DumpTexture(uint64_t content_hash, uint32_t width, uint32_t height, uint32_t pitch_blocks, bool tiled, rex::graphics::xenos::TextureFormat format, rex::graphics::xenos::Endian endianness, const uint8_t* guest_bytes, uint32_t guest_size) const;

  // Untiles, byte swaps and decodes the guest texture, as DumpTexture reads it, to RGBA8 `width` x `height`, rows
  // top first. False for a format it can't decode.
  static bool DecodeToRgba8(uint32_t width, uint32_t height, uint32_t pitch_blocks, bool tiled, rex::graphics::xenos::TextureFormat format, rex::graphics::xenos::Endian endianness, const uint8_t* guest_bytes, uint32_t guest_size, std::vector<uint8_t>* rgba);

  // The replacement for a texture, or null. Valid until the next Rescan.
  [[nodiscard]] const TextureReplacementData* FindReplacement(uint64_t content_hash) const;

  static uint64_t HashGuestData(const uint8_t* data, size_t size);

  std::filesystem::path dump_dir() const { return dump_dir_; }
  // The enabled mods' textures folders, in load order.
  std::vector<std::filesystem::path> replace_dirs() const { return replace_dirs_; }

 private:
  std::filesystem::path root_;
  std::filesystem::path dump_dir_;
  std::vector<std::filesystem::path> replace_dirs_;
  std::unordered_map<uint64_t, std::filesystem::path> replacements_;

  // Loaded once, then kept, so FindReplacement reads no file twice; and what failed, so it isn't retried.
  mutable std::unordered_map<uint64_t, TextureReplacementData> pixel_cache_;
  mutable std::unordered_set<uint64_t> failed_cache_;

  static bool WriteDDS_RGBA8(const std::filesystem::path& path, uint32_t width, uint32_t height, const uint8_t* rgba8_rows, uint32_t row_pitch_bytes);
  static bool WriteDDS_BC(const std::filesystem::path& path, uint32_t width, uint32_t height, const uint8_t* bc_blocks, uint32_t bytes_per_block, uint32_t fourcc);
  static bool WritePNG_RGBA8(const std::filesystem::path& path, uint32_t width, uint32_t height, const uint8_t* rgba8_rows, uint32_t row_pitch_bytes);
  static bool ReadDDS(const std::filesystem::path& path, TextureReplacementData& out);
  static bool ReadPNG(const std::filesystem::path& path, TextureReplacementData& out);
};

}  // namespace rexfabric
