#pragma once

#include <cstdint>
#include <memory>

#include <plume_render_interface.h>

#include "shader_cache.h"

namespace renut::gpu {

// XXH3_64 of a whole guest shader container (virtual + physical), the key the
// cache and tools/dump_shaders.py use.
uint64_t HashShaderContainer(const uint8_t *container);

const ShaderCacheEntry *FindShaderEntry(uint64_t hash);

// reNut's own shaders for NutTool's models: a container whose physical part starts with "RNUTPART"
// and its kind (1 vertex, 2 pixel) runs them. nullptr for any other container.
const ShaderCacheEntry *BuiltInShaderEntry(const uint8_t *container, bool pixel_shader);

// Whether it is one of the built-in vertex shaders that blend NutTool's shape keys (part, world, skinned).
bool IsShapedVertexShader(const ShaderCacheEntry &entry);

// Whether it is the built-in part vertex shader (blocks of their own, whose pixel c28.w names the part).
bool IsPartVertexShader(const ShaderCacheEntry &entry);

// Whether it is the built-in world pixel shader (worlds and characters), which takes their materials.
bool IsWorldPixelShader(const ShaderCacheEntry &entry);

// A shader's vertex inputs (a built-in shader's own, else the cache's).
const ShaderVertexInput *ShaderVertexInputs(const ShaderCacheEntry &entry);

// Builds the active backend's shader from a cache entry (decompressing the
// cache on first use). nullptr on failure.
std::unique_ptr<plume::RenderShader> CreateCachedShader(plume::RenderDevice *device,
                                                        const ShaderCacheEntry &entry);
// Writes a shader FindShaderEntry did not find to renut_shader_capture_dir,
// once per hash, for the next build to recompile into the cache.
void CaptureMissingShader(const uint8_t *container, bool pixel_shader, uint64_t hash);

} // namespace renut::gpu
