/**
 * @file    gpu/d3d/hooks/shader.cpp
 * @brief   Shader creation. The renderer owns the shader objects: each is a
 *          record keyed to its XenosRecomp cache entry by container hash.
 *
 * The ~150 shaders N&B compiles from HLSL at boot (hcg*ShaderCreateByHlsl)
 * reach D3DDevice_CreateVertexShader/CreatePixelShader too, so a cache miss
 * here is captured to disk for the next shader cache build.
 *
 * Shaders loaded with asset bundles are D3D objects the bundle already holds,
 * set up in place by XGRegisterVertexShader/XGRegisterPixelShader. Those stay
 * guest objects; the host side is kept per object address, keyed by the same
 * container hash.
 */
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/types.h>

#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/guest/resources.h"
#include "renut_engine/gpu/render/pipeline.h"
#include "renut_engine/gpu/render/shader_library.h"
#include "renut_engine/gpu/backend/video.h"

using namespace renut::gpu;

namespace {

// Where D3DDevice_CreateVertexShader / CreatePixelShader (0x8264E8B0 /
// 0x8264E6A8) lay out a shader object: the container's virtual part copied
// after the D3D header, and a pointer to its physical part (literal constants
// and microcode). Objects in bundles have the same layout.
constexpr u32 kVertexShaderContainer = 0x368;
constexpr u32 kVertexShaderPhysical = 0x20;
constexpr u32 kPixelShaderContainer = 0x28;
constexpr u32 kPixelShaderPhysical = 0x18;

// Guest shader objects registered in place, by object address. The render
// thread's (State().mutex).
std::unordered_map<u32, std::unique_ptr<ShaderRecord>> g_guest_shaders;

// Recent FindShaderLocked results (two lookups a draw otherwise, one under a
// shared lock). An entry goes when its shader does (ForgetShader): both kinds
// are destroyed on the render thread, the only user, so a memo'd record is
// alive. (Checking the record registry's generation instead missed whenever
// any record came or went, which busy levels do all the time.) State().mutex.
// A miss is kept too (record null): draws keep binding addresses that are no
// shader of the kind (depth-only passes), and those cost a registry lookup and
// a map search a draw. A shader made at an address forgets it first (both are
// in order with the draws on the render thread). va 0 is an empty entry
// (FindShaderLocked never looks it up).
struct ShaderMemo {
  u32 va = 0;
  ShaderRecord *record = nullptr;
};
// A table per kind, a set of four entries per cache line, the most recent
// first. A single direct-mapped table for both kinds had the shaders of a busy
// level (Showdown Town binds hundreds a frame) evicting each other every frame,
// and each miss was a registry lookup and a map search.
constexpr u32 kShaderMemoWays = 4;
constexpr u32 kShaderMemoSetBits = 10;
struct alignas(64) ShaderMemoSet {
  ShaderMemo way[kShaderMemoWays];
};
static_assert(sizeof(ShaderMemoSet) == 64);
ShaderMemoSet g_shader_memo[2][1u << kShaderMemoSetBits];

u32 KindIndex(RecordKind kind) {
  return kind == RecordKind::PixelShader ? 1 : 0;
}

ShaderMemoSet &ShaderMemoSetFor(u32 kind_index, u32 shader_va) {
  return g_shader_memo[kind_index][(shader_va * 0x9E3779B1u) >> (32 - kShaderMemoSetBits)];
}

// The last shader found of each kind: consecutive draws mostly keep their
// shaders, and this is one compare where the memo is a cache miss.
ShaderMemo g_last_shader[2];

void ForgetShader(u32 shader_va) {
  for (u32 kind = 0; kind < 2; ++kind) {
    for (ShaderMemo &memo : ShaderMemoSetFor(kind, shader_va).way) {
      if (memo.va == shader_va)
        memo = {};
    }
    if (g_last_shader[kind].va == shader_va)
      g_last_shader[kind] = {};
  }
}

} // namespace

namespace renut::gpu {

ShaderRecord *FindShaderLocked(u32 shader_va, RecordKind kind) {
  if (!shader_va)
    return nullptr;
  const u32 kind_index = KindIndex(kind);
  ShaderMemo &last = g_last_shader[kind_index];
  if (last.va == shader_va)
    return last.record;
  ShaderMemo *ways = ShaderMemoSetFor(kind_index, shader_va).way;
  u32 hit = 0;
  while (hit < kShaderMemoWays && ways[hit].va != shader_va)
    ++hit;
  ShaderMemo found;
  if (hit < kShaderMemoWays) {
    found = ways[hit];
  } else {
    hit = kShaderMemoWays - 1;  // the least recent goes
    ShaderRecord *record = FindRecord<ShaderRecord>(shader_va, kind);
    if (!record) {
      auto it = g_guest_shaders.find(shader_va);
      record = it != g_guest_shaders.end() && it->second->kind == kind ? it->second.get() : nullptr;
    }
    found = {shader_va, record};
  }
  for (u32 i = hit; i > 0; --i)
    ways[i] = ways[i - 1];
  ways[0] = found;
  last = found;
  return found.record;
}

void ReleaseGuestShaderLocked(u32 shader_va) {
  if (g_guest_shaders.erase(shader_va))
    ForgetShader(shader_va);
}

void BuildDeclarationLayout(VertexDeclarationRecord &declaration) {
  declaration.layout =
      BuildHostVertexLayout(declaration.elements.data(), u32(declaration.elements.size()));
  // FNV-1a over what the input layout depends on.
  uint64_t hash = 0xCBF29CE484222325ull;
  auto mix = [&hash](uint64_t v) { hash = (hash ^ v) * 0x100000001B3ull; };
  for (const plume::RenderInputElement &e : declaration.layout.input_elements) {
    mix(e.location);
    mix(uint64_t(e.format));
    mix(e.slotIndex);
    mix(e.alignedByteOffset);
  }
  for (u32 s = 0; s < kMaxVertexStreams; ++s)
    mix(declaration.layout.streams[s].host_stride);
  declaration.layout_hash = hash;
}

const VertexDeclarationRecord *ShaderFetchDeclarationLocked(ShaderRecord &vs) {
  if (vs.fetch_declaration_built)
    return vs.fetch_declaration.get();
  vs.fetch_declaration_built = true;
  if (!vs.entry || !vs.entry->vertexInputCount)
    return nullptr;
  auto declaration = std::make_unique<VertexDeclarationRecord>();
  const ShaderVertexInput *inputs = g_shaderVertexInputs + vs.entry->vertexInputOffset;
  for (u32 i = 0; i < vs.entry->vertexInputCount; ++i) {
    const ShaderVertexInput &input = inputs[i];
    // Stream n's fetch constant is Xenos index 95 - n (d3d::StreamFetchIndex).
    const u32 first_stream_fetch = d3d::kFirstVertexFetchIndex;
    if (input.fetchIndex < first_stream_fetch || input.fetchIndex > 95)
      continue;
    const u32 stream = 95 - input.fetchIndex;
    const u32 type = FetchDeclType(input.format, input.flags & 1, input.flags & 2);
    if (type == kDeclTypeUnused)
      continue;
    GuestVertexElement element{};
    element.Stream = u16(stream);
    element.Offset = u16(input.offsetDwords * 4);
    element.Type = type;
    element.Method = 0;
    element.Usage = input.usage;
    element.UsageIndex = input.usageIndex;
    declaration->elements.push_back(element);
    if (stream < d3d::kMaxStreams && !vs.fetch_strides[stream])
      vs.fetch_strides[stream] = input.strideDwords * 4u;
  }
  if (declaration->elements.empty())
    return nullptr;
  BuildDeclarationLayout(*declaration);
  vs.fetch_declaration = std::move(declaration);
  return vs.fetch_declaration.get();
}

plume::RenderShader *GetHostShader(ShaderRecord *shader) {
  if (!shader || !shader->entry)
    return nullptr;
  // Built already: draws call this under State().mutex, which destruction
  // (DestroyShaderRecord) takes too, and only this function sets 'host'.
  if (plume::RenderShader *host = shader->host.get())
    return host;
  std::lock_guard lock(shader->host_mutex);
  if (!shader->host) {
    shader->host = CreateCachedShader(Video::Device(), *shader->entry);
    if (!shader->host) {
      RGPU_ERROR("Creating shader {:016X} failed", shader->hash);
      shader->entry = nullptr;  // don't retry every draw
      return nullptr;
    }
  }
  return shader->host.get();
}

void DestroyVertexDeclarationRecord(VertexDeclarationRecord *declaration) {
  if (!declaration)
    return;
  std::lock_guard lock(State().mutex);
  ForgetVertexDeclarationLocked(declaration->self_va);
  DestroyRecord(declaration);
}

void DestroyShaderRecord(ShaderRecord *shader) {
  if (!shader)
    return;
  // Pipelines built from it keep their own compiled copy; only the module goes.
  {
    auto &s = State();
    std::lock_guard lock(s.mutex);
    ForgetShader(shader->self_va);
    std::lock_guard shader_lock(shader->host_mutex);
    shader->host.reset();
  }
  DestroyRecord(shader);
}

} // namespace renut::gpu

namespace {

u32 CreateShaderRecord(const be_u32 *function, bool pixel) {
  if (!function)
    return 0;
  const auto *container = reinterpret_cast<const uint8_t *>(function);
  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  if ((u32(header->flags) & 0xFFFFFF00u) != 0x102A1100u) {
    RGPU_ERROR("Create{}Shader: not a shader container (flags {:08X})", pixel ? "Pixel" : "Vertex",
               u32(header->flags));
    return 0;
  }

  auto *shader = CreateRecord<ShaderRecord>(pixel ? RecordKind::PixelShader
                                                  : RecordKind::VertexShader);
  if (!shader)
    return 0;
  const auto type = pixel ? d3d::ResourceType::PixelShader : d3d::ResourceType::VertexShader;
  shader->x360.resource.Common = kCommonD3DCreated | u32(type);
  shader->x360.resource.ReferenceCount = 1u;

  shader->hash = HashShaderContainer(container);
  shader->entry = FindShaderEntry(shader->hash);
  if (!shader->entry)
    CaptureMissingShader(container, pixel, shader->hash);
  PipelineShaderLoaded(shader->entry);
  // Draws before it may have found no shader at the address (FindShaderLocked).
  render_thread::Queue([va = shader->self_va] { ForgetShader(va); });
  return shader->self_va;
}

// The host side of a shader object the guest set up in place: its container is
// the virtual part inside the object followed by the physical part.
void RegisterGuestShader(u32 shader_va, u32 physical_va, bool pixel) {
  const u32 container_va = shader_va + (pixel ? kPixelShaderContainer : kVertexShaderContainer);
  const auto *header = GuestPtr<const d3d::ShaderContainer>(container_va);
  const u8 *physical = physical_va < 0x20000000u ? GuestPhysicalPtr<const u8>(physical_va)
                                                 : GuestPtr<const u8>(physical_va);
  if (!header || !physical)
    return;
  const u32 flags = header->flags;
  const u32 virtual_size = header->virtualSize;
  const u32 physical_size = header->physicalSize;
  if ((flags & 0xFFFFFF00u) != 0x102A1100u || ((flags & 1) != 0) == pixel ||
      virtual_size < sizeof(d3d::ShaderContainer) || virtual_size > 0x10000 ||
      physical_size == 0 || physical_size > 0x100000) {
    RGPU_ERROR("XGRegister{}Shader {:08X}: no shader container at {:08X} (flags {:08X})",
               pixel ? "Pixel" : "Vertex", shader_va, container_va, flags);
    return;
  }
  std::vector<uint8_t> container(size_t(virtual_size) + physical_size);
  std::memcpy(container.data(), header, virtual_size);
  std::memcpy(container.data() + virtual_size, physical, physical_size);
  const uint64_t hash = HashShaderContainer(container.data());
  const RecordKind kind = pixel ? RecordKind::PixelShader : RecordKind::VertexShader;
  // Captured here, where its container is at hand, only when the cache lacks it.
  if (!FindShaderEntry(hash))
    CaptureMissingShader(container.data(), pixel, hash);

  // In order with the draws that use it (render thread).
  render_thread::Queue([shader_va, pixel, hash, kind] {
    auto &shader = g_guest_shaders[shader_va];
    if (shader && shader->hash == hash && shader->kind == kind)
      return;
    // A different shader in the same memory: pipelines keep their own compiled
    // copy of the old one, so its module can go now. Draws before it may also
    // have found none there (FindShaderLocked).
    ForgetShader(shader_va);
    shader = std::make_unique<ShaderRecord>();
    shader->kind = kind;
    shader->self_va = shader_va;
    shader->hash = hash;
    shader->entry = FindShaderEntry(hash);
    PipelineShaderLoaded(shader->entry);
  });
}

// void XGRegisterVertexShader(D3DVertexShader *pShader, void *pPhysicalPart) [0x8264E7B8]
// As the original: the header (a vertex shader of one reference), and for
// each of its bind slots (two when the container's flag 0x20 is set) the
// vertex fetch instructions copied from the microcode into the slot.
void XGRegisterVertexShader_hook(u32 shader, u32 physical) {
  auto *bytes = GuestPtr<u8>(shader);
  if (!bytes)
    return;
  auto word = [bytes](u32 offset) -> be_u32 & {
    return *reinterpret_cast<be_u32 *>(bytes + offset);
  };
  std::memset(bytes, 0, kVertexShaderContainer);
  word(kVertexShaderPhysical) = physical;
  word(0x00) = 6u;            // Common: D3DRTYPE_VERTEXSHADER
  word(0x04) = 1u;            // ReferenceCount
  word(0x14) = 0xFFFF0000u;   // BaseFlush
  const u32 slots = (u32(word(kVertexShaderContainer)) & 0x20) ? 2 : 1;
  for (u32 slot = 0; slot < slots; ++slot) {
    const u32 bind = 0x28 + slot * 0x1A0;
    const u32 table = u32(word(0x380 + slot * 8));
    const u32 fetches = u32(word(table + 0x384));
    const u32 microcode = u32(word(kVertexShaderPhysical)) + u32(word(table + 0x368));
    const u32 list = table + 0x368 + (u32(word(table + 0x380)) + 9) * 4;
    for (u32 i = 0; i < fetches; ++i) {
      const u32 source = (u32(word(list + i * 4)) & 0xFFF) * 12 + microcode;
      const u32 destination = shader + bind + 0x1C + i * 12;
      if (destination != source) {
        u8 *to = GuestPtr<u8>(destination);
        const u8 *from = GuestPtr<const u8>(source);
        if (to && from)
          std::memmove(to, from, 12);
      }
    }
    word(bind) = 0xFFFFFFFFu;
  }
  RegisterGuestShader(shader, physical, false);
}

// void XGRegisterPixelShader(D3DPixelShader *pShader, void *pPhysicalPart) [0x8264E670]
void XGRegisterPixelShader_hook(u32 shader, u32 physical) {
  auto *bytes = GuestPtr<u8>(shader);
  if (!bytes)
    return;
  auto *words = reinterpret_cast<be_u32 *>(bytes);
  std::memset(bytes, 0, kPixelShaderContainer);
  words[0] = 7u;            // Common: D3DRTYPE_PIXELSHADER
  words[1] = 1u;            // ReferenceCount
  words[5] = 0xFFFF0000u;   // BaseFlush
  words[6] = physical;      // the physical part (+0x18)
  RegisterGuestShader(shader, physical, true);
}

// D3DVertexShader *D3DDevice_CreateVertexShader(const DWORD *pFunction) [0x8264E8B0]
u32 D3DDevice_CreateVertexShader_hook(mapped_u32 function) {
  return CreateShaderRecord(static_cast<const be_u32 *>(function), false);
}

// D3DPixelShader *D3DDevice_CreatePixelShader(const DWORD *pFunction) [0x8264E6A8]
u32 D3DDevice_CreatePixelShader_hook(mapped_u32 function) {
  return CreateShaderRecord(static_cast<const be_u32 *>(function), true);
}

// D3DVertexDeclaration *D3DDevice_CreateVertexDeclaration(
//     const D3DVERTEXELEMENT9 *pVertexElements) [0x8264EA90]
u32 D3DDevice_CreateVertexDeclaration_hook(rex::MappedPtr<GuestVertexElement> elements) {
  if (!elements)
    return 0;
  auto *declaration = CreateRecord<VertexDeclarationRecord>(RecordKind::VertexDeclaration);
  if (!declaration)
    return 0;
  declaration->x360.resource.Common =
      kCommonD3DCreated | u32(d3d::ResourceType::VertexDeclaration);
  declaration->x360.resource.ReferenceCount = 1u;

  const GuestVertexElement *element = elements;
  for (u32 i = 0; i < 64 && u32(element[i].Type) != kDeclTypeUnused; ++i)
    declaration->elements.push_back(element[i]);
  BuildDeclarationLayout(*declaration);
  return declaration->self_va;
}

// void D3DVertexShader_Bind(D3DVertexShader *pShader, D3DVertexDeclaration *pDecl,
//     const DWORD *pStreamStrides) [0x8263FBD0] (TU3's build dropped the unused
// Flags and Index of the first release's)
// Patches a CVertexShader's vertex fetches to a declaration and the strides of
// streams 0..(highest stream in the declaration). Draws take their input layout
// from a declaration record instead, so nothing is patched; the declaration and
// strides are kept for draws that rely on the bind: N&B binds its shaders when
// it creates them and then draws with no declaration set, or with a zero
// SetStreamSource stride.
void D3DVertexShader_Bind_hook(u32 shader_va, u32 declaration_va, mapped_u32 stream_strides) {
  // The declaration and strides as they are now; the shader record is the
  // render thread's (draws read it), so it is updated there, in order.
  const auto *declaration =
      FindRecord<VertexDeclarationRecord>(declaration_va, RecordKind::VertexDeclaration);
  if (!declaration)
    return;
  auto bound = std::make_shared<VertexDeclarationRecord>(*declaration);
  std::vector<std::pair<u32, u32>> strides;  // stream, stride
  if (stream_strides) {
    const auto *guest_strides = static_cast<const be_u32 *>(stream_strides);
    for (const GuestVertexElement &element : declaration->elements) {
      const u32 stream = element.Stream;
      if (stream < d3d::kMaxStreams)
        strides.emplace_back(stream, u32(guest_strides[stream]));
    }
  }
  render_thread::Queue([shader_va, bound, strides = std::move(strides)] {
    auto *shader = FindShaderLocked(shader_va, RecordKind::VertexShader);
    if (!shader)
      return;
    shader->bound_declaration = std::make_unique<VertexDeclarationRecord>(*bound);
    for (const auto &[stream, stride] : strides)
      shader->bound_strides[stream] = stride;
  });
}

} // namespace

REX_HOOK(D3DDevice_CreateVertexShader, D3DDevice_CreateVertexShader_hook);
REX_HOOK(D3DDevice_CreatePixelShader, D3DDevice_CreatePixelShader_hook);
REX_HOOK(D3DDevice_CreateVertexDeclaration, D3DDevice_CreateVertexDeclaration_hook);
REX_HOOK(D3DVertexShader_Bind, D3DVertexShader_Bind_hook);
REX_HOOK(XGRegisterVertexShader, XGRegisterVertexShader_hook);
REX_HOOK(XGRegisterPixelShader, XGRegisterPixelShader_hook);
