/**
 * @file    gpu/shaders/shader_cache.h
 * @brief   Layout of the XenosRecomp-generated shader cache (shader_cache.cpp in
 *          the build tree). The generator emits the definitions; this header
 *          is what it #includes and what the renderer reads.
 */
#pragma once

#include <cstddef>
#include <cstdint>

// One recompiled Xenos shader. hash is XXH3_64 of the whole guest shader
// container (virtual + physical), the same key tools/dump_shaders.py names
// the .bin files with. Offsets/sizes index the decompressed blob caches.
struct ShaderCacheEntry {
  uint64_t hash;
  uint32_t dxilOffset;
  uint32_t dxilSize;
  uint32_t spirvOffset;
  uint32_t spirvSize;
  uint32_t specConstantsMask;
  // Vertex fetch constants the shader reads by address (vfetchMemory), bit i =
  // fetch index 64 + i. Their buffers are copied for each draw.
  uint32_t vertexFetchMemoryMask;
  // Texture fetch constants the shader samples, bit i = fetch constant i.
  uint32_t textureFetchMask;
  // Float constant registers the shader uses as memexport stream constants,
  // bit i = register i.
  uint32_t memExportConstants[8];
  // Float constant registers the shader reads, bit i = register i (all of them
  // when it indexes the register file). Only these are uploaded per draw.
  uint32_t floatConstants[8];
  // The shader's vertex inputs: g_shaderVertexInputs[vertexInputOffset ..
  // + vertexInputCount).
  uint32_t vertexInputOffset;
  uint32_t vertexInputCount;
};

// A vertex element as the shader's vfetch describes it: what a vertex
// declaration bound to the shader would say.
struct ShaderVertexInput {
  uint8_t usage;        // D3DDECLUSAGE
  uint8_t usageIndex;
  uint8_t fetchIndex;   // Xenos vertex fetch constant 0..95
  uint8_t format;       // GPUVERTEXFORMAT
  uint8_t flags;        // 1 signed, 2 integer, 4 read with vfetchBound (kVertexInputBoundFetch)
  uint8_t strideDwords;
  uint16_t offsetDwords;
};

// ShaderVertexInput::flags: the shader reads the element from the bound
// declaration by an index other than the vertex index (vfetchBound in
// shader_common.h), described per draw in SharedConstants.
constexpr uint8_t kVertexInputBoundFetch = 4;
constexpr uint32_t kMaxBoundFetches = 16;

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;
extern ShaderVertexInput g_shaderVertexInputs[];

// Entry layout the generator wrote. A cache from an older XenosRecomp lacks
// the symbol and fails to link rather than zero-filling the newer fields.
// 6: the viewport epilogue (g_ScreenSpace) and the memexport rewrite.
constexpr uint32_t kShaderCacheLayout = 6;
extern const uint32_t g_shaderCacheLayout;

// zstd-compressed concatenations of every entry's DXIL / smol-v SPIR-V.
// The DXIL arrays only exist in Windows builds (XenosRecomp built with DXIL).
extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
