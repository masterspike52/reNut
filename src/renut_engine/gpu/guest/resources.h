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

  // Vertex shaders: a copy of the declaration and strides D3DVertexShader_Bind
  // bound, used by draws with no declaration set or a zero stream stride.
  std::unique_ptr<VertexDeclarationRecord> bound_declaration;
  u32 bound_strides[d3d::kMaxStreams] = {};
  // Vertex shaders: the declaration the shader's own vfetches describe, for draws
  // with neither a declaration nor a bind; nullptr when it fetches no elements.
  std::unique_ptr<VertexDeclarationRecord> fetch_declaration;
  u32 fetch_strides[d3d::kMaxStreams] = {};
  bool fetch_declaration_built = false;

  // Built on first use by a pipeline.
  std::mutex host_mutex;
  std::unique_ptr<plume::RenderShader> host;

  bool pixel() const { return kind == RecordKind::PixelShader; }
};

// The record bound as 'shader_va' (the renderer's, or the host side of an
// in-place XGRegister*Shader object), or nullptr. Requires State().mutex.
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
