/**
 * @file    gpu/render/shader_library.cpp
 */
#include "renut_engine/gpu/render/shader_library.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <smolv.h>
#include <xxhash.h>
#include <zstd.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/log.h"

REXCVAR_DEFINE_STRING(renut_shader_capture_dir, "shader_capture", "Nuts&Bolts/Graphics",
                      "Where shaders missing from the shader cache are written; the next build "
                      "recompiles them into the cache");

namespace renut::gpu {

namespace {

struct DecompressedCache {
  std::once_flag once;
  std::vector<uint8_t> bytes;
};

DecompressedCache g_spirv;
#if defined(_WIN32)
DecompressedCache g_dxil;
#endif
std::atomic<uint64_t> g_decompressed_bytes{0};  // ShaderCacheHostBytes

const uint8_t *Decompress(DecompressedCache &cache, const uint8_t *compressed,
                          size_t compressed_size, size_t decompressed_size, const char *name) {
  std::call_once(cache.once, [&] {
    cache.bytes.resize(decompressed_size);
    const size_t n = ZSTD_decompress(cache.bytes.data(), decompressed_size, compressed,
                                     compressed_size);
    if (ZSTD_isError(n) || n != decompressed_size) {
      RGPU_ERROR("{} shader cache decompression failed", name);
      cache.bytes.clear();
      cache.bytes.shrink_to_fit();
    }
    g_decompressed_bytes.fetch_add(cache.bytes.capacity(), std::memory_order_relaxed);
  });
  return cache.bytes.empty() ? nullptr : cache.bytes.data();
}

std::mutex g_capture_mutex;
std::unordered_set<uint64_t> g_captured;  // CaptureMissingShader

} // namespace

uint64_t HashShaderContainer(const uint8_t *container) {
  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const size_t size = size_t(u32(header->virtualSize)) + u32(header->physicalSize);
  return XXH3_64bits(container, size);
}

const ShaderCacheEntry *FindShaderEntry(uint64_t hash) {
  if (g_shaderCacheLayout != kShaderCacheLayout) {
    static bool logged = false;
    if (!logged) {
      RGPU_ERROR("Shader cache layout {} does not match the renderer's {}; rebuild it",
                 g_shaderCacheLayout, kShaderCacheLayout);
      logged = true;
    }
    return nullptr;
  }
  const ShaderCacheEntry *begin = g_shaderCacheEntries;
  const ShaderCacheEntry *end = begin + g_shaderCacheEntryCount;
  const ShaderCacheEntry *it = std::lower_bound(
      begin, end, hash, [](const ShaderCacheEntry &e, uint64_t h) { return e.hash < h; });
  return (it != end && it->hash == hash) ? it : nullptr;
}

std::unique_ptr<plume::RenderShader> CreateCachedShader(plume::RenderDevice *device,
                                                        const ShaderCacheEntry &entry) {
  if (!device)
    return nullptr;

  if (IsVulkan()) {
    const uint8_t *cache = Decompress(g_spirv, g_compressedSpirvCache, g_spirvCacheCompressedSize,
                                      g_spirvCacheDecompressedSize, "SPIR-V");
    if (!cache || entry.spirvSize == 0)
      return nullptr;
    const uint8_t *smol = cache + entry.spirvOffset;
    std::vector<uint8_t> spirv(smolv::GetDecodedBufferSize(smol, entry.spirvSize));
    if (spirv.empty() || !smolv::Decode(smol, entry.spirvSize, spirv.data(), spirv.size())) {
      RGPU_ERROR("SPIR-V decode failed for shader {:016X}", entry.hash);
      return nullptr;
    }
    return device->createShader(spirv.data(), spirv.size(), "main",
                                plume::RenderShaderFormat::SPIRV);
  }

#if defined(_WIN32)
  const uint8_t *cache = Decompress(g_dxil, g_compressedDxilCache, g_dxilCacheCompressedSize,
                                    g_dxilCacheDecompressedSize, "DXIL");
  if (!cache || entry.dxilSize == 0)
    return nullptr;
  return device->createShader(cache + entry.dxilOffset, entry.dxilSize, "main",
                              plume::RenderShaderFormat::DXIL);
#else
  return nullptr;
#endif
}

uint64_t ShaderCacheHostBytes() {
  return g_decompressed_bytes.load(std::memory_order_relaxed);
}

void CaptureMissingShader(const uint8_t *container, bool pixel_shader, uint64_t hash) {
  std::lock_guard lock(g_capture_mutex);
  if (!g_captured.insert(hash).second)
    return;

  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const size_t size = size_t(u32(header->virtualSize)) + u32(header->physicalSize);
  const std::filesystem::path dir(REXCVAR_GET(renut_shader_capture_dir));
  const std::filesystem::path path =
      dir / fmt::format("{}_{:016x}.bin", pixel_shader ? "ps" : "vs", hash);

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(path, std::ios::binary);
  if (out)
    out.write(reinterpret_cast<const char *>(container), std::streamsize(size));
  if (!out) {
    RGPU_WARN("Shader {:016X} is not in the shader cache and could not be captured to {}", hash,
              path.string());
    return;
  }
  RGPU_WARN("Shader {:016X} is not in the shader cache: captured to {}, the next build adds it",
            hash, path.string());
}

} // namespace renut::gpu
