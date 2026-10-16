/**
 * @file    gpu/guest/resources.h
 * @brief   Host records behind the guest D3D objects the renderer creates.
 *          Each starts with the X360 struct the game reads (see d3d.h).
 */
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include <plume_render_interface.h>

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/render/vertex_format.h"
#include "shader_cache.h"

namespace renut::gpu {

// Common bit the XDK sets on every resource it creates itself.
constexpr u32 kCommonD3DCreated = 0x100000;

struct VertexDeclarationRecord {
  d3d::Shader x360;  // D3DVertexDeclaration header

  RecordKind kind = RecordKind::VertexDeclaration;
  u32 self_va = 0;

  std::vector<GuestVertexElement> elements;  // without D3DDECL_END
  HostVertexLayout layout;
  uint64_t layout_hash = 0;  // identifies the host input layout for pipeline keys
};

struct ShaderRecord {
  d3d::Shader x360;  // D3DVertexShader / D3DPixelShader header

  RecordKind kind = RecordKind::VertexShader;
  u32 self_va = 0;

  uint64_t hash = 0;
  const ShaderCacheEntry *entry = nullptr;  // nullptr: not in the cache (captured)

  // Vertex shaders: what D3DVertexShader_Bind bound the shader to. The bind
  // bakes the declaration into the microcode, so a draw with no declaration
  // set uses it; a copy, as N&B releases the declaration right after binding.
  // And the per-stream strides in bytes, used where SetStreamSource was given
  // a zero stride (keep the bound one).
  std::unique_ptr<VertexDeclarationRecord> bound_declaration;
  u32 bound_strides[d3d::kMaxStreams] = {};
  // Vertex shaders: the declaration the shader's own vfetches describe, for
  // draws with neither a declaration set nor a bind (bundle meshes). Built on
  // first use; nullptr when the shader fetches no vertex elements.
  std::unique_ptr<VertexDeclarationRecord> fetch_declaration;
  u32 fetch_strides[d3d::kMaxStreams] = {};
  bool fetch_declaration_built = false;

  // Built on first use by a pipeline.
  std::mutex host_mutex;
  std::unique_ptr<plume::RenderShader> host;

  bool pixel() const { return kind == RecordKind::PixelShader; }
};

// The shader bound as 'shader_va': a record the renderer created, or the host
// side of a guest object set up in place (XGRegisterVertexShader/PixelShader).
// nullptr if it is neither. Requires State().mutex.
ShaderRecord *FindShaderLocked(u32 shader_va, RecordKind kind);

// Drops the host side of an in-place guest shader object, if it has one.
// Requires State().mutex.
void ReleaseGuestShaderLocked(u32 shader_va);

// Fills a declaration's host input layout and layout hash from its elements.
void BuildDeclarationLayout(VertexDeclarationRecord &declaration);

// The vertex shader's fetch declaration (see ShaderRecord::fetch_declaration).
// Requires State().mutex.
const VertexDeclarationRecord *ShaderFetchDeclarationLocked(ShaderRecord &vs);

// The shader's host object, building it on first call. nullptr if the shader
// is missing from the cache or creation failed. Requires State().mutex.
plume::RenderShader *GetHostShader(ShaderRecord *shader);

// Free a record's GPU objects (deferred past in-flight frames) and the record.
void DestroyShaderRecord(ShaderRecord *shader);
void DestroyVertexDeclarationRecord(VertexDeclarationRecord *declaration);

} // namespace renut::gpu
