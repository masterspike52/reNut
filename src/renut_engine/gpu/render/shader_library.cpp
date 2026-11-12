#include "renut_engine/gpu/render/shader_library.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <format>

#include <rex/setting.h>
#include <smolv.h>
#include <xxhash.h>
#include <zstd.h>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/d3d/d3d.h"

#include "host_shaders/renut_part_ps.spirv.h"
#include "host_shaders/renut_part_vs.spirv.h"
#include "host_shaders/renut_skinned_vs.spirv.h"
#include "host_shaders/renut_sky_ps.spirv.h"
#include "host_shaders/renut_sky_vs.spirv.h"
#include "host_shaders/renut_world_ps.spirv.h"
#include "host_shaders/renut_world_vs.spirv.h"
#if defined(_WIN32)
#include "host_shaders/renut_part_ps.dxil.h"
#include "host_shaders/renut_part_vs.dxil.h"
#include "host_shaders/renut_skinned_vs.dxil.h"
#include "host_shaders/renut_sky_ps.dxil.h"
#include "host_shaders/renut_sky_vs.dxil.h"
#include "host_shaders/renut_world_ps.dxil.h"
#include "host_shaders/renut_world_vs.dxil.h"
#endif

namespace {
struct ShaderLibrarySettings final : rex::SettingOwner {
  ShaderLibrarySettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(std::string, renut_shader_capture_dir, "shader_capture",
              "Where shaders missing from the shader cache are written; the next build "
              "recompiles them into the cache");
};
ShaderLibrarySettings &g_shader_library_settings = *new ShaderLibrarySettings();  // never freed, registered by RenutApp::OnConfigure
} // namespace

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

const uint8_t *Decompress(DecompressedCache &cache, const uint8_t *compressed,
                          size_t compressed_size, size_t decompressed_size) {
  std::call_once(cache.once, [&] {
    cache.bytes.resize(decompressed_size);
    const size_t n = ZSTD_decompress(cache.bytes.data(), decompressed_size, compressed,
                                     compressed_size);
    if (ZSTD_isError(n) || n != decompressed_size) {
      cache.bytes.clear();
      cache.bytes.shrink_to_fit();
    }
  });
  return cache.bytes.empty() ? nullptr : cache.bytes.data();
}

std::mutex g_capture_mutex;
std::unordered_set<uint64_t> g_captured;  // CaptureMissingShader

// The built-in shaders (renut_part_vs / renut_part_ps). Pipelines keep entry pointers, so these
// stay put. The vertex shader reads c0-c3 (world x view-projection) and c120-c122 (world); the
// pixel shader c28, c32, c45, c51, c81 and c82 (renut_part_ps.hlsl).
constexpr uint8_t kBuiltInMagic[8] = {'R', 'N', 'U', 'T', 'P', 'A', 'R', 'T'};
constexpr uint32_t kBuiltInVertex = 1;
constexpr uint32_t kBuiltInPixel = 2;

// A 20-byte vertex in stream 0 (fetch constant 95): half4 position, then a 10:10:10 normal.
ShaderVertexInput g_built_in_vs_inputs[] = {
    {0, 0, 95, 32, 0, 5, 0},  // POSITION0, 16_16_16_16 float
    {3, 0, 95, 7, 1, 5, 2},   // NORMAL0, 2_10_10_10 signed
};

ShaderCacheEntry g_built_in_vs = {
    0x52454E5554565331ull,  // "RENUTVS1": names its pipelines
    0, 0, 0, 0, 0, 0, 0,
    {},
    {0x0000000Fu, 0, 0, 0x0F000000u, 0, 0, 0, 0},
    0, 2,
};

ShaderCacheEntry g_built_in_ps = {
    0x52454E5554505331ull,  // "RENUTPS1"
    0, 0, 0, 0, 0, 0, 0,
    {},
    {0x10000000u, 0x000C2001u, 0x00060000u, 0, 0, 0, 0, 0},
    0, 0,
};

// The world shaders (renut_world_vs / renut_world_ps), for NutTool's world models: their vertices
// have a full-precision position, a colour and a texture coordinate; each draw's texture is in
// Xenos texture fetch constant 0.
constexpr uint32_t kBuiltInWorldVertex = 3;
constexpr uint32_t kBuiltInWorldPixel = 4;

// A 32-byte vertex in stream 0: float3 position, a 10:10:10 normal, a half4 colour, float2 texcoord.
ShaderVertexInput g_built_in_world_vs_inputs[] = {
    {0, 0, 95, 57, 0, 8, 0},   // POSITION0, 32_32_32 float
    {3, 0, 95, 7, 1, 8, 3},    // NORMAL0, 2_10_10_10 signed
    {10, 0, 95, 32, 0, 8, 4},  // COLOR0, 16_16_16_16 float
    {5, 0, 95, 37, 0, 8, 6},   // TEXCOORD0, 32_32 float
};

ShaderCacheEntry g_built_in_world_vs = {
    0x52454E5554565732ull,  // "RENUTVW2"
    0, 0, 0, 0, 0, 0, 0,
    {},
    {0x0000000Fu, 0, 0, 0x0F000000u, 0, 0, 0, 0},
    0, 4,
};

ShaderCacheEntry g_built_in_world_ps = {
    0x52454E5554505732ull,  // "RENUTPW2"
    0, 0, 0, 0, 0, 0,
    1,  // texture fetch constant 0
    {},
    {0, 0x000C2001u, 0x00060000u, 0, 0, 0, 0, 0},
    0, 0,
};

// The sky shaders (renut_sky_vs / renut_sky_ps), for NutTool's sky models: the world's vertex,
// drawn unlit at infinity.
constexpr uint32_t kBuiltInSkyVertex = 5;
constexpr uint32_t kBuiltInSkyPixel = 6;

ShaderCacheEntry g_built_in_sky_vs = {
    0x52454E5554534B56ull,  // "RENUTSKV"
    0, 0, 0, 0, 0, 0, 0,
    {},
    {0x0000000Fu, 0, 0, 0, 0, 0, 0, 0},
    0, 4,
};

ShaderCacheEntry g_built_in_sky_ps = {
    0x52454E5554534B50ull,  // "RENUTSKP"
    0, 0, 0, 0, 0, 0,
    1,  // texture fetch constant 0
    {},
    {0, 0, 0, 0, 0, 0, 0, 0},
    0, 0,
};

// The skinned vertex shader (renut_skinned_vs), for NutTool's skinned models, drawn with the world
// pixel shader: N&B points vertex fetch constant 85 at the model instance's bone matrices.
constexpr uint32_t kBuiltInSkinnedVertex = 7;

// A 40-byte vertex in stream 0: the world's vertex, then four bone indices and their weights.
ShaderVertexInput g_built_in_skinned_vs_inputs[] = {
    {0, 0, 95, 57, 0, 10, 0},   // POSITION0, 32_32_32 float
    {3, 0, 95, 7, 1, 10, 3},    // NORMAL0, 2_10_10_10 signed
    {10, 0, 95, 32, 0, 10, 4},  // COLOR0, 16_16_16_16 float
    {5, 0, 95, 37, 0, 10, 6},   // TEXCOORD0, 32_32 float
    {2, 0, 95, 6, 2, 10, 8},    // BLENDINDICES0, 8_8_8_8 integer
    {1, 0, 95, 6, 0, 10, 9},    // BLENDWEIGHT0, 8_8_8_8 unsigned normalised
};

ShaderCacheEntry g_built_in_skinned_vs = {
    0x52454E5554534B4Eull,  // "RENUTSKN"
    0, 0, 0, 0, 0,
    1u << (85 - 64),  // the bone matrices, read by address from vertex fetch constant 85
    0,
    {},
    {0x0000000Fu, 0, 0, 0x07000000u, 0, 0, 0, 0},
    0, 6,
};

} // namespace

const ShaderCacheEntry *BuiltInShaderEntry(const uint8_t *container, bool pixel_shader) {
  if (!container)
    return nullptr;
  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const uint32_t virtual_size = u32(header->virtualSize);
  if (u32(header->physicalSize) < sizeof(kBuiltInMagic) + 4)
    return nullptr;
  const uint8_t *physical = container + virtual_size;
  if (std::memcmp(physical, kBuiltInMagic, sizeof(kBuiltInMagic)) != 0)
    return nullptr;
  const uint8_t *k = physical + sizeof(kBuiltInMagic);
  const uint32_t kind = uint32_t(k[0]) << 24 | uint32_t(k[1]) << 16 | uint32_t(k[2]) << 8 | k[3];
  if (kind == kBuiltInVertex && !pixel_shader)
    return &g_built_in_vs;
  if (kind == kBuiltInPixel && pixel_shader)
    return &g_built_in_ps;
  if (kind == kBuiltInWorldVertex && !pixel_shader)
    return &g_built_in_world_vs;
  if (kind == kBuiltInWorldPixel && pixel_shader)
    return &g_built_in_world_ps;
  if (kind == kBuiltInSkyVertex && !pixel_shader)
    return &g_built_in_sky_vs;
  if (kind == kBuiltInSkyPixel && pixel_shader)
    return &g_built_in_sky_ps;
  if (kind == kBuiltInSkinnedVertex && !pixel_shader)
    return &g_built_in_skinned_vs;
  return nullptr;
}

bool IsShapedVertexShader(const ShaderCacheEntry &entry) {
  return &entry == &g_built_in_vs || &entry == &g_built_in_world_vs || &entry == &g_built_in_skinned_vs;
}

bool IsPartVertexShader(const ShaderCacheEntry &entry) {
  return &entry == &g_built_in_vs;
}

bool IsWorldPixelShader(const ShaderCacheEntry &entry) {
  return &entry == &g_built_in_world_ps;
}

const ShaderVertexInput *ShaderVertexInputs(const ShaderCacheEntry &entry) {
  if (&entry == &g_built_in_vs)
    return g_built_in_vs_inputs;
  if (&entry == &g_built_in_world_vs || &entry == &g_built_in_sky_vs)
    return g_built_in_world_vs_inputs;
  if (&entry == &g_built_in_skinned_vs)
    return g_built_in_skinned_vs_inputs;
  return g_shaderVertexInputs + entry.vertexInputOffset;
}

uint64_t HashShaderContainer(const uint8_t *container) {
  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const size_t size = size_t(u32(header->virtualSize)) + u32(header->physicalSize);
  return XXH3_64bits(container, size);
}

const ShaderCacheEntry *FindShaderEntry(uint64_t hash) {
  // A shader cache of another layout than the renderer's (rebuild it).
  if (g_shaderCacheLayout != kShaderCacheLayout)
    return nullptr;
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
  if (&entry == &g_built_in_vs)
    return CreateHostShader(device, RENUT_HOST_SHADER(renut_part_vs), "renut_part_vs");
  if (&entry == &g_built_in_ps)
    return CreateHostShader(device, RENUT_HOST_SHADER(renut_part_ps), "renut_part_ps");
  if (&entry == &g_built_in_world_vs)
    return CreateHostShader(device, RENUT_HOST_SHADER(renut_world_vs), "renut_world_vs");
  if (&entry == &g_built_in_world_ps)
    return CreateHostShader(device, RENUT_HOST_SHADER(renut_world_ps), "renut_world_ps");
  if (&entry == &g_built_in_sky_vs)
    return CreateHostShader(device, RENUT_HOST_SHADER(renut_sky_vs), "renut_sky_vs");
  if (&entry == &g_built_in_sky_ps)
    return CreateHostShader(device, RENUT_HOST_SHADER(renut_sky_ps), "renut_sky_ps");
  if (&entry == &g_built_in_skinned_vs)
    return CreateHostShader(device, RENUT_HOST_SHADER(renut_skinned_vs), "renut_skinned_vs");

  if (IsVulkan()) {
    const uint8_t *cache = Decompress(g_spirv, g_compressedSpirvCache, g_spirvCacheCompressedSize,
                                      g_spirvCacheDecompressedSize);
    if (!cache || entry.spirvSize == 0)
      return nullptr;
    const uint8_t *smol = cache + entry.spirvOffset;
    std::vector<uint8_t> spirv(smolv::GetDecodedBufferSize(smol, entry.spirvSize));
    if (spirv.empty() || !smolv::Decode(smol, entry.spirvSize, spirv.data(), spirv.size()))
      return nullptr;
    return device->createShader(spirv.data(), spirv.size(), "main",
                                plume::RenderShaderFormat::SPIRV);
  }

#if defined(_WIN32)
  const uint8_t *cache = Decompress(g_dxil, g_compressedDxilCache, g_dxilCacheCompressedSize,
                                    g_dxilCacheDecompressedSize);
  if (!cache || entry.dxilSize == 0)
    return nullptr;
  return device->createShader(cache + entry.dxilOffset, entry.dxilSize, "main",
                              plume::RenderShaderFormat::DXIL);
#else
  return nullptr;
#endif
}

void CaptureMissingShader(const uint8_t *container, bool pixel_shader, uint64_t hash) {
  std::lock_guard lock(g_capture_mutex);
  if (!g_captured.insert(hash).second)
    return;

  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const size_t size = size_t(u32(header->virtualSize)) + u32(header->physicalSize);
  const std::filesystem::path dir(g_shader_library_settings.renut_shader_capture_dir.Get());
  const std::filesystem::path path =
      dir / std::format("{}_{:016x}.bin", pixel_shader ? "ps" : "vs", hash);

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(path, std::ios::binary);
  if (out)
    out.write(reinterpret_cast<const char *>(container), std::streamsize(size));
}

} // namespace renut::gpu
