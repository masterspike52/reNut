/**
 * @file    gpu/render/draw.cpp
 */
#include "renut_engine/gpu/render/draw.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include <fmt/format.h>
#include <xxhash.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/backend/render_profile.h"
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/render/graphics_options.h"
#include "renut_engine/gpu/render/parallel.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/pipeline.h"
#include "renut_engine/gpu/guest/resources.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/render/vertex_format.h"
#include "renut_engine/gpu/backend/video_internal.h"

namespace renut::gpu {

namespace {

constexpr u32 kMaxDrawVertices = 1u << 21;
constexpr u32 kMaxDrawIndices = 1u << 22;
constexpr u32 kMaxFetchCopyBytes = 32u << 20;
constexpr u32 kUpEndian = 2;  // Begin*Vertices fetch constants use 8in32

// Xenos primitive types (D3DPRIMITIVETYPE).
enum Primitive : u32 {
  kPointList = 1,
  kLineList = 2,
  kLineStrip = 3,
  kTriangleList = 4,
  kTriangleFan = 5,
  kTriangleStrip = 6,
  kRectangleList = 8,
  kLineLoop = 12,
  kQuadList = 13,
  kTrianglePatch = 17,  // tessellated draws only
  kQuadPatch = 18,
};

// SharedConstants as shader_common.h lays it out for RENUT_RECOMP (both the
// SPIR-V raw loads and the DXIL cbuffer packoffsets).
struct SharedConstants {
  u32 texture_indices[3][32];  // 0: [2D, 3D, cube][fetch constant]
  u32 sampler_indices[32];     // 384
  u32 booleans[8];             // 512: VS bools 0-127, PS bools 128-255
  // 544 / 568: screen-space draws (screen_space): what the Xenos viewport
  // transform left of scale and offset, per axis, in pixels (see
  // ComputeViewport).
  float screen_offset[2];      // 544
  float alpha_threshold;       // 552
  u32 alpha_test_func;         // 556: CompareFunction, 7 (always) when disabled
  uint64_t vertex_fetch_data;  // 560: device address of this draw's fetch copies (SPIR-V)
  float screen_scale[2];       // 568
  u32 vertex_fetch_slots[96];  // 576: per fetch index, copy offset | endian
  uint64_t memexport_address;  // 960: device address of the export buffer (SPIR-V)
  // 968: 1 when the vertex shader's positions are in pixels (the viewport
  // transform off) and it maps them to clip space itself; 0 when the host
  // viewport does it.
  u32 screen_space;
  u32 pad_972;
  // 976: memexport windows: guest byte address, size, byte offset in the
  // export buffer. None yet, so memExport stores nothing.
  u32 memexport_streams[4][4];
  // 1040: tessellated draws: patch kind (0 none, 1 triangles, 2 quads),
  // segments per edge, first patch index; other draws: [3] what the host vertex
  // ID lacks of the guest vertex index (renutVertexRegisters).
  u32 tessellation[4];
  // 1056: per vertex input the shader reads with vfetchBound, its element as the
  // bound declaration describes it: stream fetch index, offset and stride in
  // dwords, D3DDECLTYPE (see DescribeBoundFetches).
  u32 bound_fetch[16][4];
  // 1312: point sprites (point_sprite_gs.hlsl): the render state's point width
  // and height, minimum and maximum size, in pixels (PA_SU_POINT_SIZE holds
  // half the size in 12.4 fixed point, PA_SU_POINT_MINMAX the size).
  float point_state[4];
  // 1328: the size in guest pixels of what clip space spans (the host
  // viewport, which is 'resolution_scale' times that), and padding.
  float viewport_size[2];
  float resolution_scale;  // 1336: of the render targets (ResolutionScale)
  float pad_1340;
  // 1344: in guest pixels, what the pixel position is relative to: a draw
  // predicated to one tile has the tile's corner (ViewportState::pixel_origin).
  float pixel_origin[2];
  float pad_1352[2];
};
static_assert(offsetof(SharedConstants, sampler_indices) == 384);
static_assert(offsetof(SharedConstants, booleans) == 512);
static_assert(offsetof(SharedConstants, screen_offset) == 544);
static_assert(offsetof(SharedConstants, screen_scale) == 568);
static_assert(offsetof(SharedConstants, screen_space) == 968);
static_assert(offsetof(SharedConstants, alpha_test_func) == 556);
static_assert(offsetof(SharedConstants, vertex_fetch_data) == 560);
static_assert(offsetof(SharedConstants, vertex_fetch_slots) == 576);
static_assert(offsetof(SharedConstants, memexport_address) == 960);
static_assert(offsetof(SharedConstants, memexport_streams) == 976);
static_assert(offsetof(SharedConstants, tessellation) == 1040);
static_assert(offsetof(SharedConstants, bound_fetch) == 1056);
static_assert(offsetof(SharedConstants, point_state) == 1312);
static_assert(offsetof(SharedConstants, viewport_size) == 1328);
static_assert(offsetof(SharedConstants, resolution_scale) == 1336);
static_assert(offsetof(SharedConstants, pixel_origin) == 1344);
static_assert(sizeof(SharedConstants) == 1360);

// The guest D3DDevice a draw reads its state from: the live device, or a
// recorded copy of its first kDeviceSnapshotSize bytes.
class GuestDevice {
public:
  explicit GuestDevice(const u8 *bytes) : bytes_(bytes) {}
  bool valid() const { return bytes_ != nullptr; }
  const d3d::Device &dev() const { return *reinterpret_cast<const d3d::Device *>(bytes_); }
  u32 U32(u32 offset) const { return *reinterpret_cast<const be_u32 *>(bytes_ + offset); }
  u8 U8(u32 offset) const { return bytes_[offset]; }
  float F32(u32 offset) const {
    const u32 bits = U32(offset);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  }
  // Xenos fetch constant 'index' (0-95) word 0/1 as a vertex fetch constant.
  u32 VertexFetchWord(u32 index, u32 word) const {
    return U32(u32(offsetof(d3d::Device, m_Constants)) + index * 8 + word * 4);
  }

private:
  const u8 *bytes_;
};

// A shader stage's last float constant upload: the guest bytes of the registers
// it holds, reused while the next draw's shader reads only registers it holds
// with the same bytes.
struct ConstantUpload {
  UploadAllocation alloc;
  u32 valid[8] = {};  // registers written to alloc, bit i = register i
  u8 bytes[256 * 16];  // their guest (big-endian) bytes
  // The replay (ReplayConstants::replay) whose draw last made 'valid' match
  // its constants, since when that replay's dirty groups are the only ones
  // that can differ; 0 for none.
  uint64_t replay = 0;
};

// State bound on the current command list, reset whenever a new one begins.
struct Bound {
  uint64_t generation = UINT64_MAX;
  const plume::RenderFramebuffer *framebuffer = nullptr;
  const plume::RenderPipeline *pipeline = nullptr;
  bool layout = false;
  UploadAllocation zero_stream;
  ConstantUpload vs_constants, ps_constants;
  bool blend_constant_set = false;
  float blend_constant[4] = {};
  // D3D12's stencil reference on the list (SetStencilReference); a new list
  // starts at 0.
  uint32_t stencil_reference = 0;

  // State as the draws last set it on the list, which most draws in a row
  // share (the viewport, the zero stream, the memexport buffer): setting it
  // again cost a driver call each. Both backends keep all of it across
  // pipeline and framebuffer changes; D3D12 root descriptors are dropped with
  // the root signature, so they go with 'layout'.
  bool viewport_set = false;
  plume::RenderViewport viewport;
  plume::RenderRect scissor;
  uint32_t viewport_count = 0;  // viewports and scissors set (SetViewport)
  bool root_set[kDrawRootMemExport + 1] = {};
  plume::RenderBufferReference roots[kDrawRootMemExport + 1];
  struct Stream {
    bool set = false;
    plume::RenderVertexBufferView view;
    plume::RenderInputSlot slot;
  };
  Stream streams[kZeroStreamSlot + 1];
  bool index_set = false;
  plume::RenderIndexBufferView index;
};
Bound g_bound;

// The last declaration a draw looked up by VA (otherwise a registry lookup
// under its lock per draw): draws in a row mostly use the same one. Cleared
// when a declaration goes (ForgetVertexDeclarationLocked).
u32 g_last_declaration_va = 0;
VertexDeclarationRecord *g_last_declaration = nullptr;

bool SameReference(const plume::RenderBufferReference &a, const plume::RenderBufferReference &b) {
  return a.ref == b.ref && a.offset == b.offset;
}

// plume's Vulkan pipelines have one viewport and scissor per color target
// (all dynamic), each of which a draw must have set; they all get the same.
void SetViewport(plume::RenderCommandList *cmd, const plume::RenderViewport &viewport,
                 const plume::RenderRect &scissor, uint32_t color_count) {
  // Every index set so far is rewritten, so none keeps an older value.
  uint32_t count = std::clamp(color_count, 1u, uint32_t(d3d::kMaxRenderTargets));
  if (g_bound.viewport_set)
    count = std::max(count, g_bound.viewport_count);
  const plume::RenderViewport &v = g_bound.viewport;
  const plume::RenderRect &r = g_bound.scissor;
  const bool more = count > g_bound.viewport_count;
  if (!g_bound.viewport_set || more || v.x != viewport.x || v.y != viewport.y ||
      v.width != viewport.width || v.height != viewport.height ||
      v.minDepth != viewport.minDepth || v.maxDepth != viewport.maxDepth) {
    const plume::RenderViewport viewports[d3d::kMaxRenderTargets] = {viewport, viewport, viewport,
                                                                     viewport};
    cmd->setViewports(viewports, count);
    g_bound.viewport = viewport;
  }
  if (!g_bound.viewport_set || more || r.left != scissor.left || r.top != scissor.top ||
      r.right != scissor.right || r.bottom != scissor.bottom) {
    const plume::RenderRect scissors[d3d::kMaxRenderTargets] = {scissor, scissor, scissor, scissor};
    cmd->setScissors(scissors, count);
    g_bound.scissor = scissor;
  }
  if (!g_bound.viewport_set || more)
    g_bound.viewport_count = count;
  g_bound.viewport_set = true;
}

void SetRootDescriptor(plume::RenderCommandList *cmd, const plume::RenderBufferReference &ref,
                       uint32_t index) {
  if (g_bound.root_set[index] && SameReference(g_bound.roots[index], ref))
    return;
  cmd->setGraphicsRootDescriptor(ref, index);
  g_bound.roots[index] = ref;
  g_bound.root_set[index] = true;
}

void SetVertexBuffer(plume::RenderCommandList *cmd, const plume::RenderVertexBufferView &view,
                     const plume::RenderInputSlot &slot) {
  Bound::Stream &b = g_bound.streams[slot.index];
  if (b.set && SameReference(b.view.buffer, view.buffer) && b.view.size == view.size &&
      b.slot.stride == slot.stride && b.slot.classification == slot.classification)
    return;
  cmd->setVertexBuffers(slot.index, &view, 1, &slot);
  b.set = true;
  b.view = view;
  b.slot = slot;
}

void SetIndexBuffer(plume::RenderCommandList *cmd, const plume::RenderIndexBufferView &view) {
  const plume::RenderIndexBufferView &b = g_bound.index;
  if (g_bound.index_set && SameReference(b.buffer, view.buffer) && b.size == view.size &&
      b.format == view.format)
    return;
  cmd->setIndexBuffer(&view);
  g_bound.index = view;
  g_bound.index_set = true;
}

// The blend constant (RB_BLEND_RED..ALPHA) the draw's blend controls read, or
// false if none reads it. The host has a single constant for BLEND_FACTOR, so a
// colour factor that reads the constant alpha gets the alpha in rgb as well.
bool DrawBlendConstant(const d3d::Device &dev, const PipelineKey &key, float rgba[4]) {
  constexpr u32 kConstantColor = u32(D3DBlend::BlendFactor);
  constexpr u32 kOneMinusConstantAlpha = u32(D3DBlend::InvConstantAlpha);
  constexpr u32 kConstantAlpha = u32(D3DBlend::ConstantAlpha);
  bool used = false, color_reads_color = false, color_reads_alpha = false;
  for (u32 i = 0; i < key.color_count; ++i) {
    // RB_BLENDCONTROL factors: colour src 0, colour dst 8, alpha src 16, alpha dst 24.
    for (u32 shift : {0u, 8u, 16u, 24u}) {
      const u32 factor = (key.blend_control[i] >> shift) & 0x1F;
      if (factor < kConstantColor || factor > kOneMinusConstantAlpha)
        continue;
      used = true;
      if (shift < 16)
        (factor >= kConstantAlpha ? color_reads_alpha : color_reads_color) = true;
    }
  }
  if (!used)
    return false;
  const d3d::ValuesPacket &v = dev.m_ValuesPacket;
  const float alpha = v.BlendAlpha;
  const bool replicate_alpha = color_reads_alpha && !color_reads_color;
  if (color_reads_alpha && color_reads_color) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      RGPU_WARN("Blend control {:08X} reads both the constant colour and alpha; the host uses the colour",
                key.blend_control[0]);
    }
  }
  rgba[0] = replicate_alpha ? alpha : float(v.BlendRed);
  rgba[1] = replicate_alpha ? alpha : float(v.BlendGreen);
  rgba[2] = replicate_alpha ? alpha : float(v.BlendBlue);
  rgba[3] = alpha;
  return true;
}

std::vector<u32> g_indices;  // scratch for index reads

// SetTilesLocked / SetReplayPredicationLocked.
std::vector<ClearRect> g_tiles;
u32 g_replay_predication = ~0u;

// A vertex stream as a draw reads it: guest vertices through a host layout.
struct StreamKey {
  const u8 *guest;
  uint64_t layout_hash;
  u32 stream, stride, count, endian;
  bool operator==(const StreamKey &) const = default;
};
struct StreamKeyHash {
  size_t operator()(const StreamKey &k) const {
    uint64_t h = uint64_t(reinterpret_cast<uintptr_t>(k.guest)) * 0x9E3779B97F4A7C15ull;
    h ^= k.layout_hash + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
    h ^= (uint64_t(k.stream) << 48 | uint64_t(k.endian) << 40 | k.stride) + (h << 6) + (h >> 2);
    h ^= uint64_t(k.count) * 0xFF51AFD7ED558CCDull + (h << 6) + (h >> 2);
    return size_t(h);
  }
};

// Vertex data a draw reads from guest buffers, read when the ring is kicked
// off (FlushVertexFillsLocked) rather than at the draw call: the GPU starts on
// a draw only after the kick, and the game may still write the buffers the
// draw uses until then.
struct PendingStreamFill {
  HostVertexStream stream;
  const u8 *guest = nullptr;
  u32 stride = 0, count = 0, endian = 0;
  u8 *host = nullptr;
  plume::RenderBufferReference host_ref;  // 'host' on the GPU
  StreamKey key{};   // cacheable: the g_stream_cache entry
  bool cacheable = false;
};
struct PendingCopy {
  const u8 *src = nullptr;
  u8 *dst = nullptr;
  u32 size = 0;
};
// The first g_stream_fill_count entries are queued; the rest are kept from
// earlier flushes, for their memory.
std::vector<PendingStreamFill> g_stream_fills;
size_t g_stream_fill_count = 0;
std::vector<PendingCopy> g_copy_fills;

// Fetch data copies (UploadVertexFetchData) queued since the last flush, by
// guest bytes: they all read memory at the flush, so a later draw that reads
// the same bytes takes the queued copy rather than another. Where in its
// upload chunk the copy is (the draw's fetch slots count from the chunk's
// start). A generation, bumped at each flush, retires them all at once.
struct FetchCopy {
  const u8 *src = nullptr;
  u32 size = 0;
  uint64_t generation = 0;
  const plume::RenderBuffer *buffer = nullptr;
  u32 offset = 0;
};
constexpr u32 kFetchCopyBits = 8;
FetchCopy g_fetch_copies[1u << kFetchCopyBits];
uint64_t g_fetch_copy_generation = 1;

FetchCopy &FetchCopyFor(const u8 *src, u32 size) {
  const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(src)) ^ (uint64_t(size) << 40);
  return g_fetch_copies[(key * 0x9E3779B97F4A7C15ull) >> (64 - kFetchCopyBits)];
}

// Streams queued in g_stream_fills since the last flush. The fills all read
// guest memory at the flush, so a later draw that reads the same guest
// vertices through the same host layout draws from the upload already queued.
std::unordered_map<StreamKey, UploadAllocation, StreamKeyHash> g_queued_streams;

// Converted streams (and index ranges, below) kept across frames. Most are
// geometry the game never rewrites, which every draw would otherwise convert
// again. A stream whose guest bytes hash the same at kStableFlushes flushes in
// a row gets a persistent converted copy, which draws bind while the bytes
// still hash the same, checked off the render thread once every
// kCacheVerifyFrames frames a draw reads it: hashing every copy every frame
// cost more than any other part of a draw. A stream the game rewrites
// kMaxCacheDrops times is never cached again. A copy no draw has read for
// kStreamCacheIdleFrames goes: turning back to a scene within that converts
// nothing again.
constexpr u32 kStableFlushes = 3;
constexpr uint64_t kCacheVerifyFrames = 4;
constexpr u32 kMaxCacheDrops = 3;
constexpr uint64_t kStreamCacheIdleFrames = 3600;
constexpr uint64_t kStreamCacheMaxBytes = 768ull << 20;
uint64_t g_stream_cache_bytes = 0;
uint64_t g_stream_cache_frame = 0;

// A cached copy's memory: part of one of the pool's buffers.
struct PoolSpan {
  plume::RenderBuffer *buffer = nullptr;
  u32 chunk = 0;
  uint64_t offset = 0;
  u32 size = 0;
  explicit operator bool() const { return buffer != nullptr; }
  plume::RenderBufferReference Ref() const { return plume::RenderBufferReference(buffer, offset); }
};

// Where the cached copies live: a few large buffers in video memory that
// copies are carved from, filled by the GPU (FlushPoolFillsLocked). A buffer
// of its own per copy cost a driver allocation each, and a scene coming into
// view made hundreds of copies in a frame. (They were upload buffers once:
// then the GPU read every cached mesh over PCIe every frame, which a laptop's
// GPU, behind a narrow link, felt most.) A freed span is reused once the frame
// slot it was freed in has
// finished on the GPU (DrainSlot), as retired resources are. A chunk left
// with nothing in it is released (the first one stays): the copies of a
// scene the game has left go after kStreamCacheIdleFrames, and their memory
// with them.
class CopyPool {
public:
  // Vertex and index copies have pools of their own: a D3D12 buffer is read
  // in one state, and plume gives one with both flags the vertex buffer's.
  // 'addressable': read by shaders by address (AddressableBufferFlags, known
  // once the backend is).
  CopyPool(plume::RenderBufferFlags flags, uint64_t chunk_size, const char *name,
           bool addressable = false)
      : flags_(flags), chunk_size_(chunk_size), name_(name), addressable_(addressable) {}

  PoolSpan Allocate(u32 size) {
    const uint64_t need = (uint64_t(size) + kAlign - 1) & ~(kAlign - 1);
    for (u32 i = 0; i < chunks_.size(); ++i) {
      if (PoolSpan span = Take(i, need); span)
        return span;
    }
    const u32 index = AddChunk(need);
    if (index == UINT32_MAX)
      return {};
    return Take(index, need);
  }

  void Free(const PoolSpan &span) {
    if (span)
      deferred_[State().frame].push_back(span);
  }

  // The slot's lists have finished: what was freed while it recorded is free.
  void ReleaseSlot(uint32_t slot) {
    for (const PoolSpan &span : deferred_[slot]) {
      Chunk &chunk = chunks_[span.chunk];
      Insert(chunk, span.offset, span.size);
      // Spans hold their chunk's index, so the entry stays for AddChunk to
      // reuse. The other slot may still draw from it: retired, not freed.
      if (span.chunk != 0 && chunk.free.size() == 1 && chunk.free.begin()->second == chunk.size) {
        Video::RetireLocked(std::move(chunk.buffer));
        chunk.free.clear();
        chunk.size = 0;
      }
    }
    deferred_[slot].clear();
  }

  // Host memory the chunks hold.
  uint64_t Bytes() const {
    uint64_t bytes = 0;
    for (const Chunk &chunk : chunks_)
      bytes += chunk.size;
    return bytes;
  }

private:
  static constexpr uint64_t kAlign = 256;

  struct Chunk {
    std::unique_ptr<plume::RenderBuffer> buffer;  // null once released
    uint64_t size = 0;
    std::map<uint64_t, uint64_t> free;  // offset -> size, adjacent ranges merged
  };

  // The index of a new chunk of at least 'min_size' bytes, or UINT32_MAX.
  u32 AddChunk(uint64_t min_size) {
    const uint64_t size = std::max(chunk_size_, min_size);
    const plume::RenderBufferFlags flags = flags_ | (addressable_ ? AddressableBufferFlags() : 0);
    auto buffer = State().device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(size, flags));
    if (!BufferCreated(buffer.get()))
      return UINT32_MAX;
    u32 index = 0;
    while (index < chunks_.size() && chunks_[index].buffer)
      ++index;
    if (index == chunks_.size())
      chunks_.emplace_back();
    buffer->setName(std::string(name_) + " #" + std::to_string(index));
    Chunk &chunk = chunks_[index];
    chunk.buffer = std::move(buffer);
    chunk.size = size;
    chunk.free.emplace(0, size);
    return index;
  }

  // First fit in chunk 'index'.
  PoolSpan Take(u32 index, uint64_t size) {
    Chunk &chunk = chunks_[index];
    for (auto it = chunk.free.begin(); it != chunk.free.end(); ++it) {
      if (it->second < size)
        continue;
      const uint64_t offset = it->first, left = it->second - size;
      chunk.free.erase(it);
      if (left)
        chunk.free.emplace(offset + size, left);
      PoolSpan span;
      span.buffer = chunk.buffer.get();
      span.chunk = index;
      span.offset = offset;
      span.size = u32(size);
      return span;
    }
    return {};
  }

  static void Insert(Chunk &chunk, uint64_t offset, uint64_t size) {
    auto next = chunk.free.lower_bound(offset);
    if (next != chunk.free.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second == offset) {
        offset = prev->first;
        size += prev->second;
        chunk.free.erase(prev);
      }
    }
    if (next != chunk.free.end() && offset + size == next->first) {
      size += next->second;
      chunk.free.erase(next);
    }
    chunk.free.emplace(offset, size);
  }

  plume::RenderBufferFlags flags_;
  uint64_t chunk_size_;
  const char *name_;
  bool addressable_;
  std::vector<Chunk> chunks_;
  std::vector<PoolSpan> deferred_[kFramesInFlight];
};
CopyPool g_copy_pool(plume::RenderBufferFlag::VERTEX, 64ull << 20, "renut stream cache");
CopyPool g_index_pool(plume::RenderBufferFlag::INDEX, 32ull << 20, "renut index cache");
CopyPool g_fetch_pool(plume::RenderBufferFlag::NONE, 64ull << 20, "renut fetch cache", true);

// A direct-mapped memo of a node map's entries by key hash, in front of the
// stream and index caches: draws look up the same keys every frame, and the
// map's bucket and node chain were cache misses each time (a tenth of the
// render thread). Here a repeat is one line and the entry itself, which the
// draw reads anyway. The entries stay where they are until erased (the map is
// node-based, a rehash moves none), so the memo is cleared whenever the map
// erases any. Two ways to a set, the newer first: a frame's draws use several
// thousand keys, and with one way the ones sharing a slot pushed each other
// out, back to the map every time (2026-10-22 trace: the index memo alone
// was 4% of the render thread).
template <typename Map, typename Hash, u32 kBits>
class EntryMemo {
public:
  using Entry = typename Map::value_type;

  Entry *Find(const typename Map::key_type &key, size_t hash) const {
    const Slot *set = &slots_[SetIndex(hash) * 2];
    for (u32 way = 0; way < 2; ++way) {
      const Slot &slot = set[way];
      if (slot.entry && slot.hash == hash && slot.entry->first == key)
        return slot.entry;
    }
    return nullptr;
  }

  // The map's entry for 'key', made if 'make' (else null when it has none).
  Entry *Get(Map &map, const typename Map::key_type &key, bool make) {
    const size_t hash = Hash{}(key);
    if (Entry *entry = Find(key, hash))
      return entry;
    auto it = map.find(key);
    if (it == map.end()) {
      if (!make)
        return nullptr;
      it = map.try_emplace(key).first;
    }
    Slot *set = &slots_[SetIndex(hash) * 2];
    set[1] = set[0];
    set[0] = {hash, &*it};
    return &*it;
  }

  void Clear() { std::fill(std::begin(slots_), std::end(slots_), Slot{}); }

private:
  struct Slot {
    size_t hash = 0;
    Entry *entry = nullptr;
  };
  static u32 SetIndex(size_t hash) {
    return u32((uint64_t(hash) * 0x9E3779B97F4A7C15ull) >> (64 - kBits));
  }
  alignas(32) Slot slots_[2u << kBits];
};

struct CachedStream {
  uint64_t hash = 0;  // guest bytes at the last conversion, or of the copy
  u32 stable = 0;     // flushes in a row that converted the same bytes
  u32 drops = 0;      // copies dropped because the guest bytes changed
  bool dynamic = false;
  bool pending = false;  // a copy waits for the GPU to fill it (PoolFill)
  PoolSpan copy;
  u32 bytes = 0;
  uint64_t last_used = 0;               // g_stream_cache_frame
  uint64_t checked_frame = UINT64_MAX;  // g_stream_cache_frame the copy was last checked in
};
using StreamCache = std::unordered_map<StreamKey, CachedStream, StreamKeyHash>;
StreamCache g_stream_cache;
EntryMemo<StreamCache, StreamKeyHash, 14> g_stream_memo;

// A persistent copy waiting for its bytes: at the next flush the GPU copies
// them from the upload memory the draw that made it filled with the same
// bytes (FlushPoolFillsLocked), and only then do draws use it.
struct PoolFill {
  plume::RenderBufferReference source;
  PoolSpan span;
  u32 bytes = 0;
};
struct StreamPoolFill : PoolFill {
  StreamKey key{};
};
std::vector<StreamPoolFill> g_stream_pool_fills;

// Whether a cached copy last checked in 'checked_frame' is due a check now.
bool CacheCheckDue(uint64_t checked_frame) {
  return checked_frame == UINT64_MAX || g_stream_cache_frame - checked_frame >= kCacheVerifyFrames;
}

uint64_t HashGuestStream(const StreamKey &key) {
  return XXH3_64bits(key.guest, size_t(key.count) * key.stride);
}

// The cached copies' checks, hashed off the render thread: every copy's guest
// bytes each kCacheVerifyFrames frames was a twentieth of its time. A copy
// found stale is dropped when the render thread takes the results, at the next
// frame (AdvanceStreamCacheFrameLocked), so a rewritten range can draw from
// its old copy for about a frame longer than the check interval alone allows.
template <typename Key, uint64_t (*Hash)(const Key &)>
class ContentVerifier {
public:
  struct Check {
    Key key;
    uint64_t hash;  // the copy's hash when submitted
  };

  ~ContentVerifier() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable())
      thread_.join();
  }

  // Render thread. Collected until the frame's Flush: waking the worker for
  // each check was a system call per draw.
  void Submit(const Key &key, uint64_t hash) { submitted_.push_back({key, hash}); }

  // Render thread, once a frame: hands the frame's checks to the worker.
  void Flush() {
    if (submitted_.empty())
      return;
    {
      std::lock_guard lock(mutex_);
      if (!thread_.joinable())
        thread_ = std::thread([this] { Run(); });
      pending_.insert(pending_.end(), submitted_.begin(), submitted_.end());
    }
    submitted_.clear();
    wake_.notify_one();
  }

  // Render thread: the checks whose bytes no longer hash as submitted.
  void TakeStale(std::vector<Check> &out) {
    std::lock_guard lock(mutex_);
    out.swap(stale_);
    stale_.clear();
  }

private:
  void Run() {
    std::vector<Check> batch;
    std::unique_lock lock(mutex_);
    for (;;) {
      wake_.wait(lock, [this] { return stop_ || !pending_.empty(); });
      if (stop_)
        return;
      batch.swap(pending_);
      lock.unlock();
      // Guest memory stays mapped; a read torn by a game write only fails
      // the check, which converts the range again.
      std::erase_if(batch, [](const Check &c) { return Hash(c.key) == c.hash; });
      lock.lock();
      stale_.insert(stale_.end(), batch.begin(), batch.end());
      batch.clear();
    }
  }

  std::vector<Check> submitted_;  // render thread only
  std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<Check> pending_, stale_;
  bool stop_ = false;
  std::thread thread_;
};
ContentVerifier<StreamKey, HashGuestStream> g_stream_verifier;

// Buffers vertex shaders read by address (UploadVertexFetchData), kept in
// video memory like the streams: most are geometry the game never rewrites,
// and copying each of them into upload memory every frame was the largest
// part of the flushes (a tenth of the renderer's CPU time with the helpers'
// share, 2026-10-22), which the GPU then read over PCIe. A buffer that hashes
// the same at kStableFlushes flushes in a row gets a copy the GPU fills from
// that flush's upload copy (FlushPoolFillsLocked); a draw whose buffers all
// have copies, in one pool buffer, reads them there. Copies are checked like
// the streams'; one that keeps changing (kFetchChanges flushes in a row) is
// not hashed again.
struct FetchKey {
  const u8 *src = nullptr;
  u32 size = 0;
  bool operator==(const FetchKey &) const = default;
};
struct FetchKeyHash {
  size_t operator()(const FetchKey &k) const {
    return size_t((uint64_t(reinterpret_cast<uintptr_t>(k.src)) ^ (uint64_t(k.size) << 40)) *
                  0x9E3779B97F4A7C15ull);
  }
};
struct CachedFetch {
  uint64_t hash = 0;
  u32 stable = 0;   // flushes in a row the bytes hashed the same
  u32 changes = 0;  // flushes in a row they did not
  u32 drops = 0;    // copies dropped because the bytes changed
  bool dynamic = false;
  bool pending = false;  // a copy waits for the GPU to fill it (PoolFill)
  PoolSpan copy;
  u32 bytes = 0;
  uint64_t last_used = 0;               // g_stream_cache_frame
  uint64_t checked_frame = UINT64_MAX;  // frame last hashed or checked in
};
constexpr u32 kFetchChanges = 8;
using FetchCache = std::unordered_map<FetchKey, CachedFetch, FetchKeyHash>;
FetchCache g_fetch_cache;
EntryMemo<FetchCache, FetchKeyHash, 10> g_fetch_memo;

uint64_t HashFetch(const FetchKey &key) {
  return XXH3_64bits(key.src, key.size);
}
ContentVerifier<FetchKey, HashFetch> g_fetch_verifier;

// The buffers this flush's fetch copies read, and where in upload memory they
// go (UploadVertexFetchData); looked at once the copies are made.
struct FetchTrack {
  FetchKey key;
  plume::RenderBufferReference source;
};
std::vector<FetchTrack> g_fetch_tracks;
struct FetchPoolFill : PoolFill {
  FetchKey key{};
};
std::vector<FetchPoolFill> g_fetch_pool_fills;

void DropCachedFetch(CachedFetch &c) {
  if (!c.copy)
    return;
  g_stream_cache_bytes -= c.bytes;
  g_fetch_pool.Free(c.copy);
  c.copy = {};
  c.bytes = 0;
}

// After a flush made the fetch copies: count how long each buffer's bytes
// have held, and give one that has held long enough its persistent copy.
void TrackFetchCopies() {
  for (const FetchTrack &t : g_fetch_tracks) {
    CachedFetch &c = g_fetch_memo.Get(g_fetch_cache, t.key, true)->second;
    c.last_used = g_stream_cache_frame;
    if (c.dynamic || c.copy || c.pending || c.checked_frame == g_stream_cache_frame)
      continue;
    c.checked_frame = g_stream_cache_frame;
    const uint64_t hash = HashFetch(t.key);
    if (hash == c.hash) {
      ++c.stable;
      c.changes = 0;
    } else {
      c.stable = 0;
      c.dynamic = ++c.changes >= kFetchChanges;
    }
    c.hash = hash;
    if (c.stable + 1 < kStableFlushes || g_stream_cache_bytes + t.key.size > kStreamCacheMaxBytes)
      continue;
    const PoolSpan copy = g_fetch_pool.Allocate(t.key.size);
    if (!copy)
      continue;
    FetchPoolFill fill;
    fill.source = t.source;
    fill.span = copy;
    fill.bytes = t.key.size;
    fill.key = t.key;
    g_fetch_pool_fills.push_back(fill);
    c.pending = true;
    g_stream_cache_bytes += t.key.size;
  }
  g_fetch_tracks.clear();
}

void DropCachedCopy(CachedStream &c) {
  if (!c.copy)
    return;
  g_stream_cache_bytes -= c.bytes;
  g_copy_pool.Free(c.copy);
  c.copy = {};
  c.bytes = 0;
}

// After a flush converted 'f': count how long its bytes have held, and give a
// stream that has held long enough its persistent copy.
void TrackConvertedStream(const PendingStreamFill &f) {
  auto it = g_stream_cache.find(f.key);
  if (it == g_stream_cache.end())
    return;
  CachedStream &c = it->second;
  if (c.dynamic || c.copy || c.pending)
    return;
  const uint64_t hash = HashGuestStream(f.key);
  c.stable = hash == c.hash ? c.stable + 1 : 0;
  c.hash = hash;
  const u32 bytes = f.count * f.stream.host_stride;
  if (c.stable + 1 < kStableFlushes || g_stream_cache_bytes + bytes > kStreamCacheMaxBytes)
    return;
  const PoolSpan copy = g_copy_pool.Allocate(bytes);
  if (!copy)
    return;
  // The GPU copies what the flush just converted into f.host.
  StreamPoolFill fill;
  fill.source = f.host_ref;
  fill.span = copy;
  fill.bytes = bytes;
  fill.key = f.key;
  g_stream_pool_fills.push_back(fill);
  c.pending = true;
  g_stream_cache_bytes += bytes;
}

void SyncBound(VideoState &s) {
  if (g_bound.generation != s.list_generation) {
    g_bound = Bound{};
    g_bound.generation = s.list_generation;
  }
}

bool LogLimited(uint32_t &counter, uint32_t limit = 8) {
  return counter++ < limit;
}

// Guest VA or GPU physical address to a host pointer.
const u8 *AddressPtr(u32 address) {
  if (address < 0x20000000u)
    return GuestPhysicalPtr<const u8>(address);
  return GuestPtr<const u8>(address);
}

plume::RenderPrimitiveTopology Topology(u32 primitive) {
  switch (primitive) {
  case kPointList:
    return plume::RenderPrimitiveTopology::POINT_LIST;
  case kLineList:
    return plume::RenderPrimitiveTopology::LINE_LIST;
  case kLineStrip:
  case kLineLoop:
    return plume::RenderPrimitiveTopology::LINE_STRIP;
  case kTriangleStrip:
    return plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  default:  // lists, and fans / quads expanded to lists
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
}

// ---------------------------------------------------------------------------
// Render targets
// ---------------------------------------------------------------------------

struct Targets {
  HostSurface *colors[d3d::kMaxRenderTargets] = {};
  uint32_t color_count = 0;
  HostSurface *depth = nullptr;
  // In guest pixels, as everything the draws and clears work out; host
  // viewports, scissors and rects are 'scale' times that (HostSurface::scale).
  uint32_t width = 0, height = 0;
  RenderScale scale;
  // The size the guest surface header gives; smaller than width x height for
  // a target grown for tiling (SetTilingExtentLocked).
  uint32_t guest_width = 0, guest_height = 0;
};

// A rect in guest pixels as host pixels of targets 'scale' times the size.
plume::RenderRect ScaleRect(const plume::RenderRect &rect, RenderScale scale) {
  return plume::RenderRect(scale.Coord(rect.left), scale.Coord(rect.top), scale.Coord(rect.right),
                           scale.Coord(rect.bottom));
}

bool GatherTargets(const GuestDevice &device, Targets &t) {
  for (u32 i = 0; i < d3d::kMaxRenderTargets; ++i) {
    const u32 va = device.U32(d3d::kDeviceOffsetRenderTargets + i * 4);
    if (!va)
      break;
    HostSurface *surface = GetHostSurfaceLocked(va);
    if (!surface || surface->depth)
      break;
    t.colors[t.color_count++] = surface;
  }
  HostSurface *depth = GetHostSurfaceLocked(device.U32(d3d::kDeviceOffsetDepthStencil));
  if (depth && depth->depth)
    t.depth = depth;
  if (!t.color_count && !t.depth)
    return false;
  const HostSurface *first = t.color_count ? t.colors[0] : t.depth;
  // plume's Vulkan framebuffer is the size of the first color target, and its
  // render pass loads and stores every attachment over all of it: one smaller
  // than that is written past its end and the device is lost. N&B binds such
  // sets (the garage's vehicle database: a 320x288 color target over a 288x180
  // depth surface) and draws within the smaller one, which the console and
  // D3D12 allow. Those attachments are grown to cover it.
  if (IsVulkan() && t.color_count) {
    const uint32_t host_width = first->scale.Size(first->width);
    const uint32_t host_height = first->scale.Size(first->height);
    for (uint32_t i = 1; i < t.color_count; ++i)
      CoverHostExtentLocked(t.colors[i], host_width, host_height);
    CoverHostExtentLocked(t.depth, host_width, host_height);
  }
  t.width = first->width;
  t.height = first->height;
  t.scale = first->scale;
  // A pass over targets rendered at different scales (graphics_options) would
  // address them alike; the options pick only targets drawn on their own. The
  // post-processing buffers render at 1x with the scene's depth still bound,
  // untested (on the console too its pitch is not theirs), so only colors count.
  bool mixed = false;
  for (uint32_t i = 1; i < t.color_count; ++i)
    mixed = mixed || t.colors[i]->scale != t.scale;
  if (mixed) {
    static uint32_t logged = 0;
    if (LogLimited(logged, 8)) {
      RGPU_WARN("Draw binds color targets of different render scales ({}x{} at {:.3g}x)",
                first->width, first->height, t.scale.Factor());
    }
  }
  t.guest_width = d3d::SurfaceWidth(first->size_word);
  t.guest_height = d3d::SurfaceHeight(first->size_word);
  return true;
}

// gpu_profile: draws into 't', by their host size and attachments.
uint32_t TargetsProfileLabel(const Targets &t) {
  HostSurface *first = t.color_count ? t.colors[0] : t.depth;
  const uint32_t variant = t.color_count ? (t.depth ? 1 : 0) : 2;
  uint32_t &label = first->profile_labels[variant];
  if (label == UINT32_MAX) {
    static const char *const kKinds[3] = {"color", "color+depth", "depth"};
    label = gpu_profile::Label(fmt::format("draws {}x{} {}", first->scale.Size(first->width),
                                           first->scale.Size(first->height), kKinds[variant]));
  }
  return label;
}

bool BindTargets(const Targets &t) {
  gpu_profile::Mark(State(), TargetsProfileLabel(t));
  for (uint32_t i = 0; i < t.color_count; ++i)
    TransitionSurfaceLocked(t.colors[i], plume::RenderTextureLayout::COLOR_WRITE);
  if (t.depth)
    TransitionSurfaceLocked(t.depth, plume::RenderTextureLayout::DEPTH_WRITE);
  plume::RenderFramebuffer *framebuffer = GetFramebufferLocked(t.colors, t.color_count, t.depth);
  if (!framebuffer)
    return false;
  if (g_bound.framebuffer != framebuffer) {
    State().command_list->setFramebuffer(framebuffer);
    g_bound.framebuffer = framebuffer;
  }
  return true;
}

// A draw's host viewport and scissor. The game's D3D sets the viewport one of
// two ways (N&B database):
// - D3DDevice_SetViewport (0x82222F20) writes scale w/2, -h/2, maxZ - minZ
//   and offset x + w/2, y + h/2, minZ, all enabled in PA_CL_VTE_CNTL: the
//   host viewport is that rectangle, and the shaders pass positions through.
// - D3DRS_VIEWPORTENABLE false (SetRenderState_ViewportEnable, 0x82205A80)
//   disables the transform (and clipping): the vertices are in pixels. The
//   host viewport then covers the target, and the vertex shader maps the
//   pixels to clip space itself (screen_space; any enabled part of the
//   transform comes along as screen_scale / screen_offset).
struct ViewportState {
  // Host pixels (Targets::scale).
  plume::RenderViewport viewport;
  plume::RenderRect scissor;
  // The viewport's size in guest pixels, which screen-space positions and
  // point sizes are in.
  float guest_size[2] = {};
  bool screen_space = false;
  float screen_scale[2] = {1.0f, 1.0f};
  float screen_offset[2] = {0.0f, 0.0f};
  // Guest pixels: the pixel position (VPOS) the GPU gives the pixel shader is
  // in the coordinates of the EDRAM it renders to. While tiling that is one
  // tile, and N&B adds the tile's corner back from a per-tile constant (the
  // deferred shadow pass, vs_f6965212d74a7552 / ps_d86a35e4f2b956b3: c0.x =
  // 384 in the lower tile). The host renders every tile into one target, so a
  // draw predicated to one tile has its position made relative to that tile.
  float pixel_origin[2] = {0.0f, 0.0f};
};

// The screen rect of the one tile a draw is predicated to while tiling
// (SetTilesLocked); false for a draw that runs in more tiles, or none.
bool PredicatedTile(const GuestDevice &device, ClearRect &rect) {
  if (g_tiles.size() < 2)
    return false;
  u32 mask = device.U32(d3d::kDeviceOffsetPredication);
  mask = (mask ? mask : ~0u) & g_replay_predication;
  int32_t tile = -1;
  for (size_t i = 0; i < g_tiles.size() && i < 16; ++i) {
    if (!(mask & (3u << (2 * i))))
      continue;
    if (tile >= 0)
      return false;
    tile = int32_t(i);
  }
  if (tile < 0)
    return false;
  rect = g_tiles[tile];
  return true;
}

ViewportState ComputeViewport(const GuestDevice &device, const Targets &t) {
  const d3d::Device &dev = device.dev();
  const u32 vte = dev.m_ControlPacket.VteControl;
  const d3d::ValuesPacket &v = dev.m_ValuesPacket;
  const float xs = (vte & 0x01) ? float(v.VportXScale) : 1.0f;
  float xo = (vte & 0x02) ? float(v.VportXOffset) : 0.0f;
  const float ys = (vte & 0x04) ? float(v.VportYScale) : 1.0f;
  float yo = (vte & 0x08) ? float(v.VportYOffset) : 0.0f;
  const float zs = (vte & 0x10) ? float(v.VportZScale) : 1.0f;
  const float zo = (vte & 0x20) ? float(v.VportZOffset) : 0.0f;

  // PA_SU_SC_MODE_CNTL.vtx_window_offset_enable adds PA_SC_WINDOW_OFFSET.
  int32_t wx = 0, wy = 0;
  if (u32(dev.m_ControlPacket.ModeControl) & 0x10000) {
    const u32 offset = dev.m_WindowPacket.WindowOffset;
    wx = int32_t(offset << 17) >> 17;
    wy = int32_t(offset << 1) >> 17;
    xo += float(wx);
    yo += float(wy);
  }
  // A draw predicated to one tile renders into that tile's EDRAM only, which
  // the GPU resolves to the tile's rect of the screen: it is cut to the rect.
  // Most such draws are in the tile's own coordinates, a viewport the size of
  // the tile-sized surface (N&B's per-tile scene command buffers: the vehicle
  // parts), which the host offsets to the rect. A viewport larger than that
  // surface addresses the whole screen (N&B's water, sub_823BA3A8, drawn once
  // per tile with a full-screen viewport and that tile's constants): it stays.
  ClearRect tile_rect{};
  const bool one_tile = PredicatedTile(device, tile_rect);
  const bool screen_wide = one_tile && (vte & 0x0F) == 0x0F && t.guest_width && t.guest_height &&
                           (2.0f * xs > float(t.guest_width) + 0.5f ||
                            -2.0f * ys > float(t.guest_height) + 0.5f);
  const bool tile_local = one_tile && !screen_wide;
  int32_t tx = 0, ty = 0;
  if (tile_local) {
    tx = tile_rect.x1;
    ty = tile_rect.y1;
  }
  xo += float(tx);
  yo += float(ty);

  ViewportState vs;
  if (one_tile) {
    vs.pixel_origin[0] = float(tile_rect.x1);
    vs.pixel_origin[1] = float(tile_rect.y1);
  }
  const float min_z = std::clamp(zo, 0.0f, 1.0f), max_z = std::clamp(zo + zs, 0.0f, 1.0f);
  // SetViewport's rectangle back from its scale and offset: x = offset - scale
  // and width = 2 * scale; y = offset + scale and height = -2 * scale (the
  // scale is negative: y goes down).
  vs.screen_space = (vte & 0x0F) != 0x0F || !(xs > 0.0f) || !(ys < 0.0f);
  if (!vs.screen_space) {
    vs.viewport = plume::RenderViewport(xo - xs, yo + ys, 2.0f * xs, -2.0f * ys, min_z, max_z);
  } else {
    vs.viewport = plume::RenderViewport(0.0f, 0.0f, float(t.width), float(t.height), min_z, max_z);
    vs.screen_scale[0] = xs;
    vs.screen_scale[1] = ys;
    vs.screen_offset[0] = xo;
    vs.screen_offset[1] = yo;
  }

  // PA_SC_WINDOW_SCISSOR_TL/BR: x 0-14, y 16-30.
  const u32 tl = dev.m_WindowPacket.WindowScissorTL;
  const u32 br = dev.m_WindowPacket.WindowScissorBR;
  int32_t sx1 = int32_t(tl & 0x7FFF) + wx + tx, sy1 = int32_t((tl >> 16) & 0x7FFF) + wy + ty;
  int32_t sx2 = int32_t(br & 0x7FFF) + wx + tx, sy2 = int32_t((br >> 16) & 0x7FFF) + wy + ty;
  // A target grown for tiling: D3D bounds the window scissor by the tile-sized
  // surface, and the GPU applies it per tile in window coordinates, so across
  // the tiles that bound covers the whole target (a one-tile draw is cut to
  // its tile below; one in tile coordinates has the bound offset there).
  if (!tile_local && t.guest_width && t.width > t.guest_width && sx2 >= int32_t(t.guest_width))
    sx2 = int32_t(t.width);
  if (!tile_local && t.guest_height && t.height > t.guest_height &&
      sy2 >= int32_t(t.guest_height))
    sy2 = int32_t(t.height);
  // A screen-space draw's clip space spans the whole target: what a partly
  // enabled transform would clip is cut by the scissor instead.
  if (vs.screen_space && (vte & 0x0F) == 0x0F) {
    sx1 = std::max(sx1, int32_t(std::floor(xo - std::fabs(xs))));
    sx2 = std::min(sx2, int32_t(std::ceil(xo + std::fabs(xs))));
    sy1 = std::max(sy1, int32_t(std::floor(yo - std::fabs(ys))));
    sy2 = std::min(sy2, int32_t(std::ceil(yo + std::fabs(ys))));
  }
  if (one_tile) {
    sx1 = std::max(sx1, tile_rect.x1);
    sy1 = std::max(sy1, tile_rect.y1);
    sx2 = std::min(sx2, tile_rect.x2);
    sy2 = std::min(sy2, tile_rect.y2);
  }
  sx1 = std::clamp(sx1, 0, int32_t(t.width));
  sy1 = std::clamp(sy1, 0, int32_t(t.height));
  sx2 = std::clamp(sx2, sx1, int32_t(t.width));
  sy2 = std::clamp(sy2, sy1, int32_t(t.height));

  // All of the above is in guest pixels; the host targets are t.scale times
  // that. Depth is not scaled.
  vs.guest_size[0] = vs.viewport.width;
  vs.guest_size[1] = vs.viewport.height;
  const float scale = t.scale.Factor();
  vs.viewport.x *= scale;
  vs.viewport.y *= scale;
  vs.viewport.width *= scale;
  vs.viewport.height *= scale;
  vs.scissor = ScaleRect(plume::RenderRect(sx1, sy1, sx2, sy2), t.scale);
  return vs;
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Where a draw's float constant registers are (ReplayConstants).
struct FloatConstants {
  const u8 *state = nullptr;
  const u8 *running = nullptr;  // null: all in 'state'
  uint64_t set = 0;
  u32 offset = 0;  // of VertexShaderF or PixelShaderF in the device layout
  uint64_t *dirty = nullptr;    // ReplayConstants::vs_dirty / ps_dirty
  uint64_t replay = 0;

  const u8 *Register(u32 r) const {
    const bool own = !running || ((set >> (63 - r / 4)) & 1);
    return (own ? state : running) + offset + r * 16;
  }
};

FloatConstants DrawFloatConstants(const u8 *state, const ReplayConstants *replay, bool pixel) {
  FloatConstants c;
  c.state = state;
  c.offset = u32(offsetof(d3d::Device, m_Constants)) +
             u32(pixel ? offsetof(d3d::Constants, PixelShaderF) : offsetof(d3d::Constants, VertexShaderF));
  if (replay) {
    c.running = replay->running;
    c.set = pixel ? replay->ps_set : replay->vs_set;
    c.dirty = pixel ? replay->ps_dirty : replay->vs_dirty;
    c.replay = c.dirty ? replay->replay : 0;
  }
  return c;
}

// Float constants go by groups of four registers (64 bytes): the unit m_Pending
// marks dirty, and the one a replayed draw takes from its snapshot or from the
// running device (FloatConstants::Register), so a group's bytes are contiguous.
// Bit 63 - group, as in m_Pending.
constexpr u32 kConstantGroupBytes = 4 * 16;

// The groups holding a register the shader reads.
uint64_t UsedGroups(const u32 used[8]) {
  uint64_t groups = 0;
  for (u32 word = 0; word < 8; ++word) {
    for (u32 bits = used[word]; bits;) {
      const u32 k = u32(std::countr_zero(bits)) / 4;
      groups |= uint64_t(1) << (63 - (word * 8 + k));
      bits &= ~(0xFu << (k * 4));
    }
  }
  return groups;
}

// The registers of a group the shader reads, bit i = the group's register i.
u32 GroupRegisters(const u32 used[8], u32 group) {
  return (used[group / 8] >> (group % 8 * 4)) & 0xFu;
}

// The registers a shader reads ('used', ShaderCacheEntry::floatConstants) as a
// little-endian upload of all 256, reusing the last upload on this command list
// when it holds them with the same bytes. Only the groups holding a register
// the shader reads are written, whole: fixed-size copies the compiler keeps
// inline, where a run of registers at a time cost a branch per register.
UploadAllocation UploadFloatConstants(const FloatConstants &constants, const u32 used[8],
                                      ConstantUpload &last) {
  bool reuse = bool(last.alloc);
  for (u32 word = 0; reuse && word < 8; ++word)
    reuse = !(used[word] & ~last.valid[word]);
  // Within the replay that last matched 'last', only groups written since can
  // differ (the running device is the same throughout a replay).
  const bool tracked = constants.replay && last.replay == constants.replay;
  const uint64_t dirty = tracked ? *constants.dirty : ~uint64_t(0);
  const uint64_t groups = UsedGroups(used);
  // Only the registers the shader reads count: the rest of a group may be
  // another shader's, and differ.
  for (uint64_t check = reuse ? groups & dirty : 0; reuse && check; check &= check - 1) {
    const u32 group = 63 - u32(std::countr_zero(check));
    const u32 registers = GroupRegisters(used, group);
    const u8 *cached = last.bytes + group * kConstantGroupBytes;
    const u8 *guest = constants.Register(group * 4);
    uint64_t diff = 0;
    for (u32 i = 0; i < 4; ++i) {
      if (!((registers >> i) & 1))
        continue;
      uint64_t a[2], b[2];
      std::memcpy(a, cached + i * 16, 16);
      std::memcpy(b, guest + i * 16, 16);
      diff |= (a[0] ^ b[0]) | (a[1] ^ b[1]);
    }
    reuse = diff == 0;
  }
  if (reuse) {
    if (constants.replay && !tracked) {
      // Every register the draw reads now matches: keep just those, and
      // track the replay's writes from here.
      std::memcpy(last.valid, used, sizeof(last.valid));
      last.replay = constants.replay;
      *constants.dirty = 0;
    }
    return last.alloc;
  }

  UploadAllocation alloc = Video::UploadsLocked().Allocate(256 * 16, 256);
  if (!alloc)
    return alloc;
  auto *out = static_cast<u8 *>(alloc.cpu);
  // The upload memory is write-combined: written in order, never read.
  for (uint64_t remaining = groups; remaining;) {
    const u32 group = u32(std::countl_zero(remaining));
    remaining &= ~(uint64_t(1) << (63 - group));
    u32 lanes[kConstantGroupBytes / 4];
    std::memcpy(lanes, constants.Register(group * 4), kConstantGroupBytes);
    std::memcpy(last.bytes + group * kConstantGroupBytes, lanes, kConstantGroupBytes);
    for (u32 &lane : lanes)
      lane = std::byteswap(lane);
    std::memcpy(out + group * kConstantGroupBytes, lanes, kConstantGroupBytes);
  }
  // Every register of those groups is in the upload, with the bytes it has
  // now.
  for (u32 word = 0; word < 8; ++word) {
    const u32 word_groups = u32(groups >> (56 - word * 8)) & 0xFFu;  // group word * 8 at bit 7
    u32 valid = 0;
    for (u32 k = 0; k < 8; ++k) {
      if ((word_groups >> (7 - k)) & 1)
        valid |= 0xFu << (k * 4);
    }
    last.valid[word] = valid;
  }
  last.alloc = alloc;
  last.replay = constants.replay;
  if (constants.replay)
    *constants.dirty = 0;
  return alloc;
}

// The fog object's composite (sub_82217628), with its depth of field in the
// pixel shader: blur = min(saturate(distance outside the focus range * c192.x
// (near) or c192.y (far)), c194.w).
constexpr uint64_t kFogCompositeVertexShader = 0x950F5CA8B083FA7Cull;
constexpr u32 kDepthOfFieldScales = 192;
constexpr u32 kDepthOfFieldMaxBlur = 194;

bool IsDepthOfFieldDraw(uint64_t vs_hash, const u32 ps_used[8]) {
  const u32 word = kDepthOfFieldScales / 32;
  const u32 bits = (1u << (kDepthOfFieldScales % 32)) | (1u << (kDepthOfFieldMaxBlur % 32));
  return vs_hash == kFogCompositeVertexShader && (ps_used[word] & bits) == bits;
}

// renut_depth_of_field: a copy of the upload UploadFloatConstants just made or
// reused ('last' holds its registers) with the blur scaled by 'strength'.
// 'last' keeps the game's values, for the draws after.
UploadAllocation ScaleDepthOfField(const ConstantUpload &last, float strength) {
  UploadAllocation alloc = Video::UploadsLocked().Allocate(256 * 16, 256);
  if (!alloc)
    return alloc;
  u32 lanes[256 * 4];
  std::memcpy(lanes, last.bytes, sizeof(lanes));
  for (u32 &lane : lanes)
    lane = std::byteswap(lane);
  float values[256 * 4];
  std::memcpy(values, lanes, sizeof(values));
  values[kDepthOfFieldScales * 4 + 0] *= strength;
  values[kDepthOfFieldScales * 4 + 1] *= strength;
  values[kDepthOfFieldMaxBlur * 4 + 3] *= strength;
  std::memcpy(alloc.cpu, values, sizeof(values));
  return alloc;
}

// The glow composite (ps_981a6a58f2abfa42): the scene plus its four bloom
// levels weighted by c0 (1, 1.2, 1.6, 2), compressed where the sum passes 1.
// renut_bloom scales those weights; the scene's own color is not one of them.
constexpr uint64_t kBloomCompositePixelShader = 0x981A6A58F2ABFA42ull;
constexpr u32 kBloomWeights = 0;

bool IsBloomDraw(uint64_t ps_hash, const u32 ps_used[8]) {
  return ps_hash == kBloomCompositePixelShader && (ps_used[kBloomWeights / 32] & (1u << (kBloomWeights % 32)));
}

// renut_bloom: a copy of the upload UploadFloatConstants just made or reused
// ('last' holds its registers) with the bloom weights scaled by 'strength'.
UploadAllocation ScaleBloom(const ConstantUpload &last, float strength) {
  UploadAllocation alloc = Video::UploadsLocked().Allocate(256 * 16, 256);
  if (!alloc)
    return alloc;
  u32 lanes[256 * 4];
  std::memcpy(lanes, last.bytes, sizeof(lanes));
  for (u32 &lane : lanes)
    lane = std::byteswap(lane);
  float values[256 * 4];
  std::memcpy(values, lanes, sizeof(values));
  for (u32 c = 0; c < 4; ++c)
    values[kBloomWeights * 4 + c] *= strength;
  std::memcpy(alloc.cpu, values, sizeof(values));
  return alloc;
}

TextureFetch DeviceTextureFetch(const d3d::Device &dev, u32 r) {
  return TextureFetch::FromGuest(dev.m_Constants.TextureFetch[r]);
}

void FillTextures(const GuestDevice &device, u32 fetch_mask, SharedConstants &sc) {
  for (u32 r = 0; r < 32; ++r) {
    sc.texture_indices[0][r] = kNullTextureSlot;
    sc.texture_indices[1][r] = kNullTexture3DSlot;
    sc.texture_indices[2][r] = kNullTextureCubeSlot;
    sc.sampler_indices[r] = kLinearClampSamplerSlot;
  }
  const d3d::Device &dev = device.dev();
  for (u32 r = 0; r < d3d::kTextureFetchSlots; ++r) {
    if (!(fetch_mask & (1u << r)))
      continue;
    const TextureFetch fetch = DeviceTextureFetch(dev, r);
    const TextureBinding binding =
        BindTextureLocked(device.U32(d3d::kDeviceOffsetTextures + r * 4), fetch);
    const uint32_t dim = binding.dimension == TextureDimension::Tex3D  ? 1
                         : binding.dimension == TextureDimension::Cube ? 2
                                                                       : 0;
    // Stacked textures go through the 3D fetches, flagged (tfetchStacked in
    // shader_common.h), with the fetch constant's filter between layers and
    // its Z address mode.
    u32 index = binding.slot | binding.scale.code << kResolutionScaleShift;
    if (binding.stacked) {
      index |= kStackedTextureBit;
      if (fetch.VolumeMagFilter() != TextureFilter::Point ||
          fetch.VolumeMinFilter() != TextureFilter::Point) {
        index |= kStackedLayerLinearBit;
      }
      if (fetch.Address(2) == TextureAddress::Wrap)
        index |= kStackedLayerWrapBit;
    }
    sc.texture_indices[dim][r] = index;
    sc.sampler_indices[r] = SamplerSlotLocked(fetch, binding.lod_scale);
  }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

struct Geometry {
  bool indexed = false;
  u32 index_count = 0;
  plume::RenderIndexBufferView index_view;  // indices from first_vertex
  u32 vertex_count = 0;  // non-indexed host draw count
  u32 first_vertex = 0;  // first guest vertex converted
  u32 vertex_range = 0;  // vertices converted per stream
  // Tessellated draws: one instance per patch, the streams per instance.
  u32 instance_count = 1;
  bool per_instance = false;
};

// Reads 'count' guest indices into g_indices.
bool ReadIndices(const u8 *src, u32 count, bool is32, u32 endian) {
  if (!src || count > kMaxDrawIndices)
    return false;
  g_indices.resize(count);
  if (is32) {
    SwapEndian(endian, src, g_indices.data(), size_t(count) * 4);
  } else {
    const bool swap = endian == 1 || endian == 2;
    for (u32 i = 0; i < count; ++i) {
      uint16_t v;
      std::memcpy(&v, src + i * 2, 2);
      g_indices[i] = swap ? uint16_t((v >> 8) | (v << 8)) : v;
    }
  }
  return true;
}

// Rewrites g_indices for fans and quads as triangle lists.
void ExpandIndices(u32 primitive) {
  if (primitive == kTriangleFan) {
    std::vector<u32> out;
    for (size_t i = 2; i < g_indices.size(); ++i) {
      out.push_back(g_indices[0]);
      out.push_back(g_indices[i - 1]);
      out.push_back(g_indices[i]);
    }
    g_indices.swap(out);
  } else if (primitive == kQuadList) {
    std::vector<u32> out;
    for (size_t i = 0; i + 3 < g_indices.size(); i += 4) {
      const u32 a = g_indices[i], b = g_indices[i + 1], c = g_indices[i + 2], d = g_indices[i + 3];
      out.insert(out.end(), {a, b, c, a, c, d});
    }
    g_indices.swap(out);
  }
}

// Guest index ranges kept across frames, rebased and in the host format, like
// the vertex streams (g_stream_cache): most draws read index buffers the game
// never rewrites. A range that hashes the same in kStableFlushes frames in a
// row gets a persistent copy, which draws use while the range still hashes the
// same, checked like the streams' (CacheCheckDue, off the render thread), and
// a range rewritten kMaxCacheDrops times is never cached again.
struct IndexKey {
  const u8 *src;
  u32 count, primitive, endian;
  bool is32;
  bool operator==(const IndexKey &) const = default;
};
struct IndexKeyHash {
  size_t operator()(const IndexKey &k) const {
    uint64_t h = uint64_t(reinterpret_cast<uintptr_t>(k.src)) * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t(k.count) << 16 | k.primitive << 3 | k.endian << 1 | u32(k.is32)) +
         0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
    return size_t(h);
  }
};
struct CachedIndices {
  uint64_t hash = 0;
  u32 stable = 0;  // frames in a row the range hashed the same
  u32 drops = 0;   // copies dropped because the guest range changed
  bool dynamic = false;
  bool pending = false;  // a copy waits for the GPU to fill it (PoolFill)
  PoolSpan copy;
  u32 bytes = 0, index_count = 0, lo = 0, max_index = 0;
  bool wide = false;
  uint64_t last_used = 0;               // g_stream_cache_frame
  uint64_t checked_frame = UINT64_MAX;  // frame the range was last hashed or checked in
};
using IndexCache = std::unordered_map<IndexKey, CachedIndices, IndexKeyHash>;
IndexCache g_index_cache;
EntryMemo<IndexCache, IndexKeyHash, 14> g_index_memo;

struct IndexPoolFill : PoolFill {
  IndexKey key{};
  u32 index_count = 0, lo = 0, max_index = 0;
  bool wide = false;
};
std::vector<IndexPoolFill> g_index_pool_fills;

uint64_t HashIndices(const IndexKey &key) {
  return XXH3_64bits(key.src, size_t(key.count) * (key.is32 ? 4 : 2));
}

ContentVerifier<IndexKey, HashIndices> g_index_verifier;

void DropCachedIndices(CachedIndices &c) {
  if (!c.copy)
    return;
  g_stream_cache_bytes -= c.bytes;
  g_index_pool.Free(c.copy);
  c.copy = {};
  c.bytes = 0;
}

// The draw's indices from the cache, if it has a copy of the guest range.
bool CachedIndexDraw(const IndexKey &key, const DrawCall &call, Geometry &g) {
  auto *entry = g_index_memo.Get(g_index_cache, key, false);
  if (!entry || !entry->second.copy)
    return false;
  CachedIndices &c = entry->second;
  c.last_used = g_stream_cache_frame;
  if (CacheCheckDue(c.checked_frame)) {
    c.checked_frame = g_stream_cache_frame;
    g_index_verifier.Submit(key, c.hash);
  }
  g.first_vertex = u32(int64_t(call.base_vertex) + c.lo);
  g.vertex_range = c.max_index + 1;
  g.indexed = true;
  g.index_count = c.index_count;
  g.index_view = plume::RenderIndexBufferView(
      c.copy.Ref(), c.bytes,
      c.wide ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
  return true;
}

// After a draw read the range into g_indices and wrote them rebased to
// 'source': count how long it has held, and give a range that has held long
// enough its persistent copy (filled from 'source' by the GPU).
void TrackIndices(const IndexKey &key, u32 lo, u32 max_index,
                  const plume::RenderBufferReference &source) {
  CachedIndices &c = g_index_memo.Get(g_index_cache, key, true)->second;
  c.last_used = g_stream_cache_frame;
  if (c.dynamic || c.copy || c.pending || c.checked_frame == g_stream_cache_frame)
    return;
  c.checked_frame = g_stream_cache_frame;
  const uint64_t hash = HashIndices(key);
  c.stable = hash == c.hash ? c.stable + 1 : 0;
  c.hash = hash;
  const bool wide = max_index > 0xFFFE;
  const u32 bytes = u32(g_indices.size()) * (wide ? 4 : 2);
  if (c.stable + 1 < kStableFlushes || g_stream_cache_bytes + bytes > kStreamCacheMaxBytes)
    return;
  const PoolSpan copy = g_index_pool.Allocate(bytes);
  if (!copy)
    return;
  IndexPoolFill fill;
  fill.source = source;
  fill.span = copy;
  fill.bytes = bytes;
  fill.key = key;
  fill.index_count = u32(g_indices.size());
  fill.lo = lo;
  fill.max_index = max_index;
  fill.wide = wide;
  g_index_pool_fills.push_back(fill);
  c.pending = true;
  g_stream_cache_bytes += bytes;
}

bool PrepareGeometry(const GuestDevice &device, const DrawCall &call, Geometry &g) {
  const bool expand = call.primitive == kTriangleFan || call.primitive == kQuadList;
  static uint32_t rect_logged = 0;
  if (call.primitive == kRectangleList && !State().rectangle_list_gs && LogLimited(rect_logged, 1))
    RGPU_WARN("Rectangle lists are drawn as triangle lists");

  u32 lo = 0, max_index = 0;  // host indices are g_indices - lo, the largest max_index
  IndexKey index_key{};
  bool cacheable_indices = false;
  if (call.indexed) {
    const u8 *src = nullptr;
    bool is32 = false;
    u32 endian = 0;
    if (call.up_indices) {
      is32 = call.up_indices_32;
      endian = is32 ? 2 : 1;
      src = call.up_indices + size_t(call.start_index) * (is32 ? 4 : 2);
    } else {
      const auto *ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
      if (!ib)
        return false;
      const u32 common = ib->resource.Common;
      is32 = (common & d3d::kIndexBufferCommon32Bit) != 0;
      endian = d3d::IndexBufferEndian(common);
      const u8 *base = AddressPtr(ib->Address);
      if (!base)
        return false;
      src = base + size_t(call.start_index) * (is32 ? 4 : 2);
      // Draw*IndexedPrimitiveUP indices are the game's own memory, only valid
      // during the call; these are an index buffer's.
      index_key = IndexKey{src, call.count, call.primitive, endian, is32};
      cacheable_indices = call.count <= kMaxDrawIndices;
      if (cacheable_indices && CachedIndexDraw(index_key, call, g))
        return true;
    }
    if (!ReadIndices(src, call.count, is32, endian))
      return false;
    if (expand)
      ExpandIndices(call.primitive);
    if (g_indices.empty())
      return false;
    lo = UINT32_MAX;
    u32 hi = 0;
    for (u32 index : g_indices) {
      lo = std::min(lo, index);
      hi = std::max(hi, index);
    }
    if (hi - lo >= kMaxDrawVertices)
      return false;
    g.first_vertex = u32(int64_t(call.base_vertex) + lo);
    g.vertex_range = hi - lo + 1;
    max_index = hi - lo;
  } else {
    if (!call.count || call.count > kMaxDrawVertices)
      return false;
    g.first_vertex = call.start_vertex;
    g.vertex_range = call.count;
    if (expand) {
      g_indices.resize(call.count);
      for (u32 i = 0; i < call.count; ++i)
        g_indices[i] = i;
      ExpandIndices(call.primitive);
      if (g_indices.empty())
        return false;
      max_index = call.count - 1;
    } else {
      g.vertex_count = call.count;
    }
  }

  if (call.indexed || expand) {
    // The host indices are rebased to the range rather than drawn with a base
    // vertex of -lo: D3D12's SV_VertexID leaves the base vertex out (Vulkan's
    // vertex index includes it), and vertex shaders that fetch by address
    // take the guest index from the vertex ID (renutVertexRegisters).
    // 16-bit when every index fits, like the guest's own format mostly is.
    const bool wide = max_index > 0xFFFE;
    const u32 bytes = u32(g_indices.size()) * (wide ? 4 : 2);
    UploadAllocation alloc = Video::UploadsLocked().Allocate(bytes, 16);
    if (!alloc)
      return false;
    if (wide) {
      auto *out = static_cast<u32 *>(alloc.cpu);
      for (size_t i = 0; i < g_indices.size(); ++i)
        out[i] = g_indices[i] - lo;
    } else {
      auto *out = static_cast<uint16_t *>(alloc.cpu);
      for (size_t i = 0; i < g_indices.size(); ++i)
        out[i] = uint16_t(g_indices[i] - lo);
    }
    g.indexed = true;
    g.index_count = u32(g_indices.size());
    g.index_view = plume::RenderIndexBufferView(
        alloc.ref, bytes, wide ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
    if (cacheable_indices)
      TrackIndices(index_key, lo, max_index, alloc.ref);
  }
  return true;
}

// Tessellated draws (DrawCall::tessellation): the patches become instances of a
// grid the vertex shader places (renutVertexRegisters in shader_common.h), at
// one level for the whole draw, the highest any patch edge asks for.
bool PrepareTessellation(const GuestDevice &device, const DrawCall &call, Geometry &g,
                         u32 tessellation[4]) {
  const bool triangles = call.primitive == kTrianglePatch;
  if (!triangles && call.primitive != kQuadPatch) {
    static uint32_t logged = 0;
    if (LogLimited(logged, 1))
      RGPU_WARN("Tessellated primitive {} not drawn", call.primitive);
    return false;
  }
  // Segments per edge are the tessellation level plus one.
  const float min_level = std::max(device.F32(d3d::kDeviceOffsetMinTessellationLevel), 0.0f) + 1.0f;
  const float max_level =
      std::max(device.F32(d3d::kDeviceOffsetMaxTessellationLevel) + 1.0f, min_level);
  float level = max_level;
  // The count is the patch count. (N&B's water: each section's patch stream is
  // sized 36 bytes x the count it draws, and its factor pass exports one
  // patch's edge factors per invocation.)
  const u32 patches = call.count;
  if ((device.U32(d3d::kDeviceOffsetTessellationControl) & 3) == 2) {
    // Adaptive: the index buffer holds each patch's edge factors (floats), from
    // the start index on. When a memexport draw in flight writes them, these
    // are from a frame or two before (DrawNeedsExportFlush does not wait for
    // them).
    const u32 edges = triangles ? 3 : 4;
    // Without them, the maximum level.
    const auto *ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
    const u8 *base = ib ? AddressPtr(ib->Address) : nullptr;
    if (base && ReadIndices(base + size_t(call.start_index) * 4, patches * edges, true,
                            d3d::IndexBufferEndian(ib->resource.Common))) {
      float highest = 0.0f;
      for (u32 bits : g_indices) {
        float factor;
        std::memcpy(&factor, &bits, 4);
        if (factor > highest && factor < 1024.0f)
          highest = factor;
      }
      level = std::clamp(highest + 1.0f, min_level, max_level);
    }
  }
  if (!patches || patches > kMaxDrawVertices)
    return false;
  constexpr u32 kMaxTessellatedVertices = 1u << 22;
  u32 segments = std::clamp(u32(std::ceil(level)), 1u, 64u);
  const u32 per_segment = triangles ? 3 : 6;  // vertices per segment^2
  while (segments > 1 &&
         uint64_t(patches) * segments * segments * per_segment > kMaxTessellatedVertices)
    --segments;

  g.vertex_count = segments * segments * per_segment;
  g.instance_count = patches;
  g.per_instance = true;
  g.first_vertex = u32(call.base_vertex);
  g.vertex_range = patches;
  tessellation[0] = triangles ? 1 : 2;
  tessellation[1] = segments;
  tessellation[2] = u32(call.base_vertex);
  return true;
}

struct StreamSource {
  const u8 *data = nullptr;  // guest vertex 0 of the stream
  u32 stride = 0;
  u32 endian = 0;
  u32 vertex_count = 0;      // vertices available from data
  u32 cpu_address = 0;       // guest VA of data, when read through the vertex buffer
};

// 'shader_strides': the strides in the vertex shader's microcode when the draw
// fetches as the shader was bound (or baked), with no declaration set on the
// device. The GPU uses those, whatever SetStreamSource was given; D3D patches
// the stream strides in only when it binds the shader to a device
// declaration.
StreamSource GetStreamSource(const GuestDevice &device, const DrawCall &call,
                             const ShaderRecord &vs, const u32 *shader_strides, u32 stream) {
  StreamSource src;
  if (stream == 0 && call.up_vertices) {
    src.data = call.up_vertices;
    src.stride = call.up_stride;
    src.endian = kUpEndian;
    src.vertex_count = call.up_vertex_count;
    return src;
  }
  const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
  const u32 w0 = device.VertexFetchWord(fetch_index, 0);
  const u32 w1 = device.VertexFetchWord(fetch_index, 1);
  if (shader_strides)
    src.stride = shader_strides[stream];
  if (!src.stride)
    src.stride = u32(device.U8(d3d::kDeviceOffsetStreamStrides + stream)) * 4;
  if (!src.stride)
    src.stride = vs.bound_strides[stream];  // SetStreamSource(..., 0): the Bind stride
  if (!src.stride)
    src.stride = vs.fetch_strides[stream];  // or the one the shader's vfetch encodes
  src.endian = w1 & 3;
  const u32 size = w1 & 0x3FFFFFCu;
  const u32 gpu_address = w0 & ~3u;
  src.data = gpu_address ? GuestPhysicalPtr<const u8>(gpu_address) : nullptr;
  // Read through the vertex buffer's CPU address, where the game wrote it,
  // when the fetch constant is the one SetStreamSource made from that buffer
  // (its GPU address plus the stream offset).
  if (const auto *vb = GuestPtr<const d3d::VertexBuffer>(
          device.U32(d3d::kDeviceOffsetStreamSources + stream * 4))) {
    const u32 cpu_base = u32(vb->Format.dword[0]) & ~3u;
    const u32 gpu_base = d3d::CpuToGpuAddress(cpu_base);
    if (cpu_base >= 0x20000000u && gpu_address >= gpu_base &&
        gpu_address - gpu_base < (u32(vb->Format.dword[1]) & 0x3FFFFFCu)) {
      src.cpu_address = cpu_base + (gpu_address - gpu_base);
      src.data = GuestPtr<const u8>(src.cpu_address);
    }
  }
  src.vertex_count = src.stride ? size / src.stride : (size ? 1 : 0);
  return src;
}

// Host vertices of the draw's streams built on the GPU from export windows
// (ConvertExportedStreamsLocked), from its first vertex; 'bytes' 0 for the
// others.
struct GpuStreams {
  plume::RenderBufferReference ref[kMaxVertexStreams];
  u32 bytes[kMaxVertexStreams] = {};
};

bool BindStreams(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                 const u32 *shader_strides, const HostVertexLayout &layout, uint64_t layout_hash,
                 const Geometry &g, const GpuStreams &gpu_streams) {
  plume::RenderCommandList *cmd = State().command_list;
  for (u32 stream = 0; stream < kMaxVertexStreams; ++stream) {
    const HostVertexStream &hs = layout.streams[stream];
    if (!hs.used)
      continue;
    if (gpu_streams.bytes[stream]) {
      const plume::RenderVertexBufferView view(gpu_streams.ref[stream], gpu_streams.bytes[stream]);
      const plume::RenderInputSlot slot(
          stream, hs.host_stride,
          g.per_instance ? plume::RenderInputSlotClassification::PER_INSTANCE_DATA
                         : plume::RenderInputSlotClassification::PER_VERTEX_DATA);
      SetVertexBuffer(cmd, view, slot);
      continue;
    }
    const StreamSource src = GetStreamSource(device, call, vs, shader_strides, stream);
    // A zero stride repeats one vertex for every index.
    const bool constant = src.stride == 0;
    const u32 first = constant ? 0 : g.first_vertex;
    u32 count = constant ? 1 : g.vertex_range;
    if (first < src.vertex_count)
      count = std::min(count, src.vertex_count - first);
    else
      count = src.data ? 0 : count;
    const u32 bytes = std::max<u32>(count, 1) * hs.host_stride;
    const bool fills = src.data && count;
    const u8 *guest = fills ? src.data + size_t(first) * src.stride : nullptr;
    const bool queued = fills && !(stream == 0 && call.up_vertices);
    const StreamKey key{guest, layout_hash, stream, src.stride, count, src.endian};
    CachedStream *cached = nullptr;
    if (queued && src.stride) {
      cached = &g_stream_memo.Get(g_stream_cache, key, true)->second;
      cached->last_used = g_stream_cache_frame;
      if (cached->copy && CacheCheckDue(cached->checked_frame)) {
        cached->checked_frame = g_stream_cache_frame;
        g_stream_verifier.Submit(key, cached->hash);
      }
      if (cached->copy) {
        const plume::RenderVertexBufferView view(cached->copy.Ref(), cached->bytes);
        const plume::RenderInputSlot slot(
            stream, hs.host_stride,
            g.per_instance ? plume::RenderInputSlotClassification::PER_INSTANCE_DATA
                           : plume::RenderInputSlotClassification::PER_VERTEX_DATA);
        SetVertexBuffer(cmd, view, slot);
        continue;
      }
    }
    const auto it = queued ? g_queued_streams.find(key) : g_queued_streams.end();
    const bool reused = it != g_queued_streams.end();
    UploadAllocation alloc = reused ? it->second : Video::UploadsLocked().Allocate(bytes, 16);
    if (!alloc)
      return false;
    // Conversion writes every vertex; only an unfilled buffer needs zeros.
    if (!fills)
      std::memset(alloc.cpu, 0, bytes);
    if (fills && !reused) {
      if (call.up_vertices && stream == 0) {
        // A copy taken at the call (queued.cpp).
        ConvertVertices(hs, guest, src.stride, count, src.endian, static_cast<u8 *>(alloc.cpu));
      } else {
        // Into an entry kept from an earlier flush when there is one: its
        // element list keeps its memory.
        if (g_stream_fill_count == g_stream_fills.size())
          g_stream_fills.emplace_back();
        PendingStreamFill &fill = g_stream_fills[g_stream_fill_count++];
        fill.stream = hs;
        fill.guest = guest;
        fill.stride = src.stride;
        fill.count = count;
        fill.endian = src.endian;
        fill.host = static_cast<u8 *>(alloc.cpu);
        fill.host_ref = alloc.ref;
        fill.key = key;
        fill.cacheable = cached && !cached->dynamic;
        g_queued_streams.emplace(key, alloc);
      }
    }
    const plume::RenderVertexBufferView view(alloc.ref, bytes);
    const plume::RenderInputSlot slot(stream, constant ? 0 : hs.host_stride,
                                      g.per_instance
                                          ? plume::RenderInputSlotClassification::PER_INSTANCE_DATA
                                          : plume::RenderInputSlotClassification::PER_VERTEX_DATA);
    SetVertexBuffer(cmd, view, slot);
  }

  if (!g_bound.zero_stream) {
    g_bound.zero_stream = Video::UploadsLocked().Allocate(kZeroStreamSize, 16);
    if (!g_bound.zero_stream)
      return false;
    std::memset(g_bound.zero_stream.cpu, 0, kZeroStreamSize);
  }
  const plume::RenderVertexBufferView zero_view(g_bound.zero_stream.ref, kZeroStreamSize);
  const plume::RenderInputSlot zero_slot(kZeroStreamSlot, 0);
  SetVertexBuffer(cmd, zero_view, zero_slot);
  return true;
}

// The vertex inputs the shader reads from its declaration by an index other
// than the vertex index (vfetchBound): each described from the declaration's
// element for it. Returns the fetch constants (bit i = index 64 + i) whose
// buffers those reads need copied.
u32 DescribeBoundFetches(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                         const VertexDeclarationRecord &declaration, const u32 *shader_strides,
                         SharedConstants &sc) {
  if (!vs.entry)
    return 0;
  u32 mask = 0;
  const ShaderVertexInput *inputs = g_shaderVertexInputs + vs.entry->vertexInputOffset;
  const u32 count = std::min<u32>(vs.entry->vertexInputCount, kMaxBoundFetches);
  for (u32 i = 0; i < count; ++i) {
    if (!(inputs[i].flags & kVertexInputBoundFetch))
      continue;
    for (const GuestVertexElement &e : declaration.elements) {
      if (e.Usage != inputs[i].usage || e.UsageIndex != inputs[i].usageIndex)
        continue;
      const u32 stream = e.Stream;
      if (stream >= kMaxVertexStreams)
        break;
      const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
      const StreamSource src = GetStreamSource(device, call, vs, shader_strides, stream);
      sc.bound_fetch[i][0] = fetch_index;
      sc.bound_fetch[i][1] = u32(e.Offset) / 4;
      sc.bound_fetch[i][2] = src.stride / 4;
      sc.bound_fetch[i][3] = BoundDeclType(u32(e.Type)) & 0x3FFFFFu;
      mask |= 1u << (fetch_index - 64);
      break;
    }
  }
  return mask;
}

bool FetchFromExportWindow(const GuestDevice &device, const DrawCall &call, u32 fetch_index,
                           u32 &slot, uint64_t &offset);
bool EnsureBuffer(std::unique_ptr<plume::RenderBuffer> &buffer, uint64_t &capacity, uint64_t size,
                  const plume::RenderBufferDesc &desc_template, const char *name);

// Copies the buffers of the fetch constants the vertex shader reads by address
// into one allocation and fills their SharedConstants slots. A buffer whose
// newest data is in a pending export's window comes from there instead: then
// the whole fetch data is put together on the GPU, in the slot's
// gpu_fetch_data, and the returned allocation's ref and gpu_address point
// there. Records copies: before the framebuffer is bound.
UploadAllocation UploadVertexFetchData(const GuestDevice &device, const DrawCall &call, u32 mask,
                                       SharedConstants &sc) {
  struct Copy {
    u32 index;
    const u8 *src;
    u32 size;
    u32 endian;
    bool exported;        // from the export window below
    u32 export_slot;
    uint64_t export_offset;
  };
  Copy copies[32];
  u32 copy_count = 0;
  u32 total = 16;
  bool any_exported = false;
  const u32 up_fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(0);
  for (u32 bit = 0; bit < 32; ++bit) {
    if (!(mask & (1u << bit)))
      continue;
    const u32 index = 64 + bit;
    Copy c{index, nullptr, 0, 0, false, 0, 0};
    if (index == up_fetch_index && call.up_vertices) {
      c.src = call.up_vertices;
      c.size = call.up_vertex_count * call.up_stride;
      c.endian = kUpEndian;
    } else {
      const u32 w0 = device.VertexFetchWord(index, 0);
      const u32 w1 = device.VertexFetchWord(index, 1);
      c.src = (w0 & ~3u) ? GuestPhysicalPtr<const u8>(w0 & ~3u) : nullptr;
      c.size = w1 & 0x3FFFFFCu;
      c.endian = w1 & 3;
    }
    if (!c.src)
      c.size = 0;
    c.size = std::min(c.size, kMaxFetchCopyBytes);
    if (c.size && FetchFromExportWindow(device, call, index, c.export_slot, c.export_offset))
      any_exported = c.exported = true;
    copies[copy_count++] = c;
    total += (c.size + 15u) & ~15u;
  }

  // From the fetch cache when it has a copy of every buffer, all in one pool
  // buffer, which the slots then count from (nothing copied).
  if (!any_exported && copy_count) {
    CachedFetch *found[32] = {};
    plume::RenderBuffer *pool = nullptr;
    bool all = true;
    for (u32 i = 0; all && i < copy_count; ++i) {
      const Copy &c = copies[i];
      if (!c.size)
        continue;
      if (c.index == up_fetch_index && call.up_vertices) {
        all = false;
        break;
      }
      auto *entry = g_fetch_memo.Get(g_fetch_cache, FetchKey{c.src, c.size}, false);
      CachedFetch *f = entry ? &entry->second : nullptr;
      all = f && f->copy && (!pool || f->copy.buffer == pool);
      if (all) {
        pool = f->copy.buffer;
        found[i] = f;
      }
    }
    if (all && pool) {
      for (u32 i = 0; i < copy_count; ++i) {
        const Copy &c = copies[i];
        CachedFetch *f = found[i];
        if (f) {
          f->last_used = g_stream_cache_frame;
          if (CacheCheckDue(f->checked_frame)) {
            f->checked_frame = g_stream_cache_frame;
            g_fetch_verifier.Submit(FetchKey{c.src, c.size}, f->hash);
          }
        }
        sc.vertex_fetch_slots[c.index] = (f ? u32(f->copy.offset) : 0u) | c.endian;
      }
      UploadAllocation cached;
      cached.ref = plume::RenderBufferReference(pool, 0);
      cached.gpu_address = IsVulkan() ? pool->getDeviceAddress() : 0;
      sc.vertex_fetch_data = cached.gpu_address;
      return cached;
    }
  }

  UploadAllocation alloc = Video::UploadsLocked().Allocate(total, 256);
  if (!alloc)
    return alloc;
  // Without exports the slots count from the start of the upload chunk, so a
  // buffer an earlier draw has queued a copy of in the same chunk is read
  // there (FetchCopy) and not copied again.
  const bool shared = !any_exported;
  const uint64_t chunk_offset = shared ? alloc.ref.offset : 0;
  u32 offset = 0;
  for (u32 i = 0; i < copy_count; ++i) {
    const Copy &c = copies[i];
    u8 *dst = static_cast<u8 *>(alloc.cpu) + offset;
    u32 at = u32(chunk_offset) + offset;
    if (c.size && !c.exported) {
      if (c.index == up_fetch_index && call.up_vertices) {
        std::memcpy(dst, c.src, c.size);
      } else if (shared) {
        FetchCopy &known = FetchCopyFor(c.src, c.size);
        if (known.generation == g_fetch_copy_generation && known.src == c.src &&
            known.size == c.size && known.buffer == alloc.ref.ref) {
          at = known.offset;
        } else {
          g_copy_fills.push_back({c.src, dst, c.size});
          g_fetch_tracks.push_back({FetchKey{c.src, c.size}, plume::RenderBufferReference(alloc.ref.ref, at)});
          known = {c.src, c.size, g_fetch_copy_generation, alloc.ref.ref, at};
        }
      } else {
        g_copy_fills.push_back({c.src, dst, c.size});
        g_fetch_tracks.push_back({FetchKey{c.src, c.size},
                                  plume::RenderBufferReference(alloc.ref.ref, alloc.ref.offset + offset)});
      }
    }
    sc.vertex_fetch_slots[c.index] = at | c.endian;
    offset += (c.size + 15u) & ~15u;
  }
  if (shared) {
    alloc.ref.offset = 0;
    alloc.gpu_address = alloc.gpu_address ? alloc.gpu_address - chunk_offset : 0;
    sc.vertex_fetch_data = alloc.gpu_address;
    return alloc;
  }
  sc.vertex_fetch_data = alloc.gpu_address;

  // The guest buffers' copies from the upload, the exported ones from their
  // windows (raw bytes, as guest memory would hold them), each into its own
  // part of the region.
  auto &s = State();
  uint64_t &used = s.gpu_fetch_data_used[s.frame];
  if (!s.gpu_fetch_data[s.frame] || used + total > s.gpu_fetch_data_capacity[s.frame]) {
    // The draws before still read the old buffer, retired with the slot.
    if (!EnsureBuffer(s.gpu_fetch_data[s.frame], s.gpu_fetch_data_capacity[s.frame], used + total,
                      plume::RenderBufferDesc::DefaultBuffer(0, AddressableBufferFlags()),
                      "renut gpu fetch data")) {
      return UploadAllocation{};
    }
    used = 0;
  }
  plume::RenderBuffer *dest = s.gpu_fetch_data[s.frame].get();
  const uint64_t region = used;
  used = (used + total + 255u) & ~uint64_t(255);
  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderBufferBarrier to_copy[kFramesInFlight + 1];
  u32 barrier_count = 0;
  to_copy[barrier_count++] = plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::WRITE);
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    if (s.memexport_buffer[slot]) {
      to_copy[barrier_count++] =
          plume::RenderBufferBarrier(s.memexport_buffer[slot].get(), plume::RenderBufferAccess::READ);
    }
  }
  cmd->barriers(plume::RenderBarrierStage::COPY, to_copy, barrier_count);
  offset = 0;
  for (u32 i = 0; i < copy_count; ++i) {
    const Copy &c = copies[i];
    if (c.size) {
      const plume::RenderBufferReference to(dest, region + offset);
      if (c.exported) {
        cmd->copyBufferRegion(
            to, plume::RenderBufferReference(s.memexport_buffer[c.export_slot].get(), c.export_offset),
            c.size);
      } else {
        cmd->copyBufferRegion(
            to, plume::RenderBufferReference(alloc.ref.ref, alloc.ref.offset + offset), c.size);
      }
    }
    offset += (c.size + 15u) & ~15u;
  }
  // Read by the vertex shader (as a root SRV on D3D12).
  to_copy[0] = plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::READ);
  for (u32 i = 1; i < barrier_count; ++i)
    to_copy[i].accessBits = plume::RenderBufferAccess::WRITE;
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE, to_copy, 1);
  if (barrier_count > 1)
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, to_copy + 1, barrier_count - 1);
  g_bound.framebuffer = nullptr;  // the copies end any render pass
  alloc.ref = plume::RenderBufferReference(dest, region);
  alloc.gpu_address = IsVulkan() ? dest->getDeviceAddress() + region : 0;
  sc.vertex_fetch_data = alloc.gpu_address;
  return alloc;
}

// ---------------------------------------------------------------------------

PipelineKey MakeKey(const GuestDevice &device, const ShaderRecord *vs, const ShaderRecord *ps,
                    const VertexDeclarationRecord *declaration, const Targets &t, u32 primitive,
                    bool per_instance) {
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  const d3d::Device &dev = device.dev();
  key.vertex_shader = vs->entry;
  key.pixel_shader = ps ? ps->entry : nullptr;
  key.layout_hash = declaration->layout_hash;
  key.topology = Topology(primitive);
  key.color_count = t.color_count;
  for (uint32_t i = 0; i < t.color_count; ++i)
    key.color_formats[i] = t.colors[i]->format;
  key.depth_format = t.depth ? t.depth->format : plume::RenderFormat::UNKNOWN;
  const HostSurface *first = t.color_count ? t.colors[0] : t.depth;
  key.samples = uint32_t(first->samples);
  const d3d::ControlPacket &cp = dev.m_ControlPacket;
  key.blend_control[0] = cp.BlendControl0;
  key.blend_control[1] = cp.BlendControl1;
  key.blend_control[2] = cp.BlendControl2;
  key.blend_control[3] = cp.BlendControl3;
  key.color_mask = u32(dev.m_ValuesPacket.ColorMask) & 0xFFFF;
  key.depth_control = t.depth ? u32(cp.DepthControl) : 0;
  key.stencil_ref_mask = t.depth ? u32(dev.m_ValuesPacket.StencilRefMask) & 0xFFFFFF : 0;
  key.stencil_ref_mask_bf = t.depth ? u32(dev.m_ValuesPacket.StencilRefMaskBF) & 0xFFFFFF : 0;
  key.mode_control = u32(cp.ModeControl) & 0x7;
  key.alpha_to_coverage = (u32(cp.ColorControl) >> 4) & 1;
  if (primitive == kRectangleList) {
    if (State().rectangle_list_gs)
      key.rectangle_list = 1;
    // Rectangles are not culled. N&B's D3D clears through rectangles
    // (ClearRects_D3D, 0x822043D8) whose corners run clockwise on screen, under
    // whatever cull mode the game left set: with back faces culled and
    // counterclockwise front faces (after hitting some enemies) the clear of
    // the post-process surface was culled, so its resolve took the previous
    // frame's final image back into the bloom, which grew to white.
    key.mode_control &= ~3u;
  }
  if (primitive == kPointList && State().point_sprite_gs)
    key.point_sprite = 1;
  if (per_instance) {
    key.per_instance_streams = 1;
    // Which way the tessellated grid winds depends on how the shader weights
    // the control points, so it is not culled.
    key.mode_control &= ~3u;
  }
  return key;
}

// ---------------------------------------------------------------------------
// Memexport. N&B fills vertex buffers (and other data) from vertex shaders that
// write to memory: `mad eA, index, const0100, stream` then eM# (see memExport in
// shader_common.h). A draw whose shaders do gets a window in the export buffer
// per stream constant it uses, seeded from guest memory so unwritten bytes keep
// their value; after the draw the windows are copied to the readback buffer,
// and before anything reads that memory (a draw that reads it, a kickoff or
// present) the list is submitted and waited, and the
// results are written to guest memory, where every other path reads them from.
// Waiting at every draw instead cost a GPU round trip per export draw.

struct MemExportStream {
  u32 base = 0;  // guest physical byte address
  u32 size = 0;  // bytes
  u32 format = 0;      // color format (MemExportElementSize)
  u32 endian = 0;      // Endian128
  u32 num_format = 0;  // SurfaceNumberFormat
};

// Bytes per element of a memexport format: the game's bits per pixel for a
// format of single texels in whole dwords, which is what the shaders write
// (memExport in shader_common.h); 0 for any other.
u32 MemExportElementSize(u32 format) {
  const GpuTextureFormat f = GpuTextureFormat(format & 0x3F);
  u32 block_width, block_height;
  FormatBlockSize(f, block_width, block_height);
  const u32 bpp = FormatBitsPerPixel(f);
  return block_width == 1 && block_height == 1 && bpp % 32 == 0 && bpp <= 128 && bpp != 96
             ? bpp / 8
             : 0;
}

constexpr u32 kMaxMemExportStreams = 4;
constexpr u32 kMaxMemExportBytes = 64u << 20;

// The streams the shader's stream constants describe, laid out as N&B's own
// export passes build them (sub_82317020 before D3DDevice_EndExport): x the
// GPU address in dwords (bits 0-29) tagged 01 in bits 30-31, z the endian in
// bits 0-2, the format in 8-13 and the number format in 16-18, w the element
// count in bits 0-22 (y, z and w carry 0x4B high bits to stay floats).
void GatherMemExportStreams(const ShaderCacheEntry *entry, const FloatConstants &constants,
                            MemExportStream *streams, u32 &count) {
  if (!entry)
    return;
  // Only the registers the shader exports through (most draws have none).
  for (u32 word = 0; word < 8; ++word) {
    for (u32 bits = entry->memExportConstants[word]; bits; bits &= bits - 1) {
      const u32 r = word * 32 + u32(std::countr_zero(bits));
      const auto *words = reinterpret_cast<const be_u32 *>(constants.Register(r));
      const u32 x = words[0], z = words[2], w = words[3];
      const u32 element = MemExportElementSize((z >> 8) & 0x3F);
      const u32 elements = w & 0x7FFFFF;
      if ((x >> 30) != 1 || !element || !elements) {
        static uint32_t logged = 0;
        if ((x >> 30) == 1 && LogLimited(logged, 8)) {
          RGPU_WARN("Memexport stream in c{} not exported: {:08X} {:08X} {:08X} {:08X} (format "
                    "{}, {} elements)",
                    r, x, u32(words[1]), z, w, (z >> 8) & 0x3F, elements);
        }
        continue;
      }
      MemExportStream stream;
      stream.base = (x & 0x3FFFFFFF) * 4;
      stream.size = std::min(elements * element, kMaxMemExportBytes);
      stream.format = (z >> 8) & 0x3F;
      stream.endian = z & 7;
      stream.num_format = (z >> 16) & 7;
      if (stream.base >= 0x20000000u)
        continue;
      stream.size = std::min(stream.size, 0x20000000u - stream.base);
      bool known = false;
      for (u32 i = 0; i < count; ++i)
        known |= streams[i].base == stream.base && streams[i].size == stream.size;
      if (!known && count < kMaxMemExportStreams)
        streams[count++] = stream;
    }
  }
}

struct PendingExport {
  u32 base = 0;
  u32 size = 0;
  // Its window in the slot's export buffer, and its copy in the slot's
  // readback buffer (the same offset in both).
  uint64_t offset = 0;
};
// The exports a slot's lists made. They reach guest memory once the slot's
// fence is waited (detail::CompleteMemExportsLocked), a frame later; until
// then their windows stay in the slot's export buffer, where the next frame's
// draws seed from them on the GPU (SeedFromPendingExportsLocked).
struct SlotExports {
  std::vector<PendingExport> pending;
  uint64_t used = 0;
};
SlotExports g_exports[kFramesInFlight];

// The 64 KiB granules of physical memory any pending export writes: most
// ranges a draw reads touch none, and skip the lists (PendingExportSlots runs
// for every draw's streams and textures while exports are pending, which is
// always).
constexpr u32 kExportGranuleShift = 16;
constexpr u32 kExportGranules = 0x20000000u >> kExportGranuleShift;
uint64_t g_export_granules[kExportGranules / 64];

void MarkExportGranules(u32 base, u32 size) {
  if (!size)
    return;
  const u32 first = (base & 0x1FFFFFFFu) >> kExportGranuleShift;
  const u32 last = std::min((((base & 0x1FFFFFFFu) + size - 1) >> kExportGranuleShift),
                            kExportGranules - 1);
  for (u32 g = first; g <= last; ++g)
    g_export_granules[g / 64] |= uint64_t(1) << (g % 64);
}

void RebuildExportGranules() {
  std::memset(g_export_granules, 0, sizeof(g_export_granules));
  for (const SlotExports &slot : g_exports) {
    for (const PendingExport &p : slot.pending)
      MarkExportGranules(p.base, p.size);
  }
}

// A word of the bitmap at a time: the bits of the first and last words that
// are in the range, the words between whole.
bool ExportGranulesTouched(u32 base, u32 size) {
  const u32 first = (base & 0x1FFFFFFFu) >> kExportGranuleShift;
  const u32 last = std::min((((base & 0x1FFFFFFFu) + size - 1) >> kExportGranuleShift),
                            kExportGranules - 1);
  if (last < first)
    return false;
  const u32 first_word = first / 64, last_word = last / 64;
  const uint64_t first_mask = ~uint64_t(0) << (first % 64);
  const uint64_t last_mask = ~uint64_t(0) >> (63 - last % 64);
  if (first_word == last_word)
    return (g_export_granules[first_word] & first_mask & last_mask) != 0;
  if (g_export_granules[first_word] & first_mask)
    return true;
  for (u32 w = first_word + 1; w < last_word; ++w) {
    if (g_export_granules[w])
      return true;
  }
  return (g_export_granules[last_word] & last_mask) != 0;
}

bool AnyPendingExports() {
  for (const SlotExports &slot : g_exports) {
    if (!slot.pending.empty())
      return true;
  }
  return false;
}

// A GPU physical address, or a guest VA in the physical windows, as a physical
// address (other VAs come out as unrelated addresses, which at worst flush the
// exports early).
u32 PhysicalAddress(u32 address) {
  return address >= 0x20000000u ? d3d::CpuToGpuAddress(address) & 0x1FFFFFFFu : address;
}

// The slots (a mask) with a pending export that will write memory overlapping
// [address, address + size).
u32 PendingExportSlots(u32 address, u32 size) {
  if (!size)
    return 0;
  const u32 base = PhysicalAddress(address);
  if (!ExportGranulesTouched(base, size))
    return 0;
  u32 slots = 0;
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    for (const PendingExport &p : g_exports[slot].pending) {
      if (base < p.base + p.size && p.base < base + size) {
        slots |= 1u << slot;
        break;
      }
    }
  }
  return slots;
}

// The pending export window holding the newest data of all of [base, base +
// size) (physical): the newest pending export overlapping it, if it covers it
// whole. Its slot and the offset of 'base' in that slot's export buffer.
bool FindExportWindow(u32 base, u32 size, u32 &slot, uint64_t &offset) {
  const auto &s = State();
  // Oldest slot first, and a slot's exports in the order they were drawn.
  const PendingExport *newest = nullptr;
  u32 newest_slot = 0;
  for (u32 k = 1; k <= kFramesInFlight; ++k) {
    const u32 at = (s.frame + k) % kFramesInFlight;
    for (const PendingExport &p : g_exports[at].pending) {
      if (base < p.base + p.size && p.base < base + size) {
        newest = &p;
        newest_slot = at;
      }
    }
  }
  if (!newest || base < newest->base || base + size > newest->base + newest->size)
    return false;
  slot = newest_slot;
  offset = newest->offset + (base - newest->base);
  return true;
}

// Whether UploadVertexFetchData copies the buffer of fetch constant
// 'fetch_index' (read by vfetchMemory / vfetchBound) from a pending export's
// window on the GPU, which it does when the window holds all of it.
bool FetchFromExportWindow(const GuestDevice &device, const DrawCall &call, u32 fetch_index,
                           u32 &slot, uint64_t &offset) {
  if (call.up_vertices && fetch_index == d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(0))
    return false;
  const u32 base = device.VertexFetchWord(fetch_index, 0) & ~3u;
  const u32 size = std::min(device.VertexFetchWord(fetch_index, 1) & 0x3FFFFFCu, kMaxFetchCopyBytes);
  return base && size && PendingExportSlots(base, size) && FindExportWindow(base, size, slot, offset);
}

// A vertex stream built on the GPU from the export window that holds its
// newest data (ConvertExportedStreamsLocked).
struct ExportedStream {
  u32 slot = 0;          // whose export buffer holds the window
  uint64_t offset = 0;   // of the stream's first byte there
  u32 stride = 0;        // guest
  u32 host_stride = 0;
  u32 vertex_count = 0;  // in the stream's buffer
  u32 element_count = 0;
  u32 elements[2 * kStreamConvertMaxElements] = {};  // StreamConvertPush::elements
};

// Whether a draw can read vertex stream 'stream' from a pending export's
// window instead of guest memory: the window holds the newest data of all of
// the stream's buffer (FindExportWindow), and stream_convert_cs can build its
// host layout (word-aligned elements, few enough of them). Shader reads
// through vfetchBound get the window's bytes too (UploadVertexFetchData).
bool FindExportedStream(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                        const u32 *shader_strides, const VertexDeclarationRecord &declaration,
                        u32 stream, ExportedStream &out) {
  const auto &s = State();
  if (!s.stream_convert_pipeline || stream >= kMaxVertexStreams || (stream == 0 && call.up_vertices))
    return false;
  const HostVertexStream &hs = declaration.layout.streams[stream];
  if (!hs.used)
    return false;
  const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
  const u32 base = device.VertexFetchWord(fetch_index, 0) & ~3u;
  const u32 size = device.VertexFetchWord(fetch_index, 1) & 0x3FFFFFCu;
  if (!base || !size || !PendingExportSlots(base, size))
    return false;
  const StreamSource src = GetStreamSource(device, call, vs, shader_strides, stream);
  if (!src.stride || src.stride % 4 || src.stride / 4 > kStreamConvertMaxStrideWords ||
      hs.host_stride % 4 || hs.elements.size() > kStreamConvertMaxElements) {
    return false;
  }
  u32 elements[2 * kStreamConvertMaxElements] = {};
  for (size_t i = 0; i < hs.elements.size(); ++i) {
    const HostVertexElement &e = hs.elements[i];
    if (e.guest_offset % 4 || e.guest_offset + 4 > src.stride)
      return false;
    const bool expand = e.conversion == ElementConversion::ExpandFloat;
    // As ConvertVertices: swapping the halves too is the endian mode ^ 3.
    const u32 endian = !expand && e.swap_halves ? src.endian ^ 3 : src.endian;
    u32 type = e.type.format | (e.type.is_signed ? 1u << 8 : 0u) | (e.type.is_integer ? 1u << 9 : 0u);
    for (u32 c = 0; c < 4; ++c)
      type |= u32(e.type.swizzle[c] & 7) << (10 + 3 * c);
    elements[2 * i] = (e.guest_offset / 4) | (e.host_offset / 4) << 8 | endian << 16 |
                      (expand ? 1u : 0u) << 18 | (e.host_size / 4 & 7) << 19;
    elements[2 * i + 1] = type;
  }
  if (!FindExportWindow(base, size, out.slot, out.offset))
    return false;
  out.stride = src.stride;
  out.host_stride = hs.host_stride;
  out.vertex_count = size / src.stride;
  out.element_count = u32(hs.elements.size());
  std::memcpy(out.elements, elements, sizeof(elements));
  return true;
}

// Whether a pending export writes anywhere a texture's data could be: an
// over-estimate of its base and mip data from the size alone (each level in
// whole 32x32x4 tiles at 16 bytes a texel, twice over, and the padding of a
// mip chain's small levels), tested against the granule bitmap before the
// exact extent (TextureGuestExtent), whose memo lookup per texture per draw
// cost more than the rest of DrawNeedsExportFlush.
bool ExportsNearTexture(const TextureFetch &fetch) {
  u32 width, height, depth;
  fetch.Size(width, height, depth);
  const uint64_t w = (uint64_t(std::max(width, fetch.PitchTexels())) + 31) & ~uint64_t(31);
  const uint64_t h = (uint64_t(height) + 31) & ~uint64_t(31);
  const uint64_t d = (uint64_t(std::max(depth, 1u)) + 3) & ~uint64_t(3);
  const u32 bound = u32(std::min<uint64_t>(w * h * d * 16 * 2 + (256u << 10), 0x20000000u));
  for (const u32 page : {fetch.BasePage(), fetch.MipPage()}) {
    if (page && ExportGranulesTouched(page << 12, bound))
      return true;
  }
  return false;
}

// Why a draw needs the pending exports in guest memory first (see
// DrawNeedsExportFlush).
enum class ExportRead : u32 {
  None,
  Stream,    // a declared vertex stream
  Fetch,     // a vertex fetch by address (vfetchMemory)
  Index,     // the index buffer
  Texture,
  Capacity,  // its exports would not fit the readback buffer beside theirs
};

// Whether pending exports must reach guest memory before a draw, and whose
// ('slots', a mask): it reads memory they write (its vertex streams, bound or
// read by address, indices or textures), or its exports would not fit this
// slot's buffers beside theirs (growing them would lose their windows). Any
// other draw can be recorded while they are still on the GPU, and so can one
// whose streams are built from the export windows (FindExportedStream) or
// whose own exports are seeded from them (SeedFromPendingExportsLocked).
ExportRead DrawNeedsExportFlush(const GuestDevice &device, const DrawCall &call,
                                const ShaderRecord &vs, const ShaderRecord *ps,
                                const VertexDeclarationRecord &declaration,
                                const u32 *shader_strides, const FloatConstants &vs_constants,
                                const FloatConstants &ps_constants, u32 &slots) {
  const u32 current = 1u << State().frame;
  auto fetch_slots = [&](u32 fetch_index) {
    const u32 w0 = device.VertexFetchWord(fetch_index, 0);
    const u32 w1 = device.VertexFetchWord(fetch_index, 1);
    return PendingExportSlots(w0 & ~3u, w1 & 0x3FFFFFCu);
  };
  auto found = [&](ExportRead read, u32 found_slots) {
    slots = found_slots;
    return read;
  };
  u32 streams_checked = 0;
  for (const GuestVertexElement &e : declaration.elements) {
    const u32 stream = u16(e.Stream);
    if (stream >= kMaxVertexStreams || (streams_checked & (1u << stream)))
      continue;
    streams_checked |= 1u << stream;
    // A Begin/DrawVerticesUP draw's stream 0 is its own vertices, whatever
    // buffer the fetch constant still names: nothing of it is read from
    // memory.
    if (stream == 0 && call.up_vertices)
      continue;
    const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
    if (const u32 found_slots = fetch_slots(fetch_index)) {
      // A stream the host layout reads is built from the export window; one it
      // does not (no host input) is read only by vfetchBound, whose copy the
      // window supplies (UploadVertexFetchData).
      ExportedStream exported;
      u32 slot;
      uint64_t offset;
      const bool on_gpu =
          declaration.layout.streams[stream].used
              ? FindExportedStream(device, call, vs, shader_strides, declaration, stream, exported)
              : FetchFromExportWindow(device, call, fetch_index, slot, offset);
      if (!on_gpu)
        return found(ExportRead::Stream, found_slots);
    }
  }
  if (vs.entry) {
    for (u32 bits = vs.entry->vertexFetchMemoryMask; bits; bits &= bits - 1) {
      const u32 fetch_index = 64 + u32(std::countr_zero(bits));
      if (const u32 found_slots = fetch_slots(fetch_index)) {
        u32 slot;
        uint64_t offset;
        if (!FetchFromExportWindow(device, call, fetch_index, slot, offset))
          return found(ExportRead::Fetch, found_slots);
      }
    }
  }
  // A tessellated draw's "indices" are only adaptive edge factors, which set
  // its level (PrepareTessellation): the ones in guest memory, from a frame
  // before, set it as well. N&B's water computes them with memexport before
  // each draw, and waiting for them was two GPU round trips a frame.
  if (call.indexed && !call.up_indices && !call.tessellation) {
    const auto *ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
    if (ib) {
      if (const u32 found_slots = PendingExportSlots(ib->Address, ib->Size))
        return found(ExportRead::Index, found_slots);
    }
  }
  const u32 texture_mask =
      (vs.entry ? vs.entry->textureFetchMask : 0) | (ps && ps->entry ? ps->entry->textureFetchMask : 0);
  for (u32 bits = texture_mask; bits; bits &= bits - 1) {
    const TextureFetch fetch = DeviceTextureFetch(device.dev(), u32(std::countr_zero(bits)));
    if (!ExportsNearTexture(fetch))
      continue;
    u32 pages[2], sizes[2];
    TextureGuestExtent(fetch, pages, sizes);
    for (u32 i = 0; i < 2; ++i) {
      if (!pages[i])
        continue;
      if (const u32 found_slots = PendingExportSlots(pages[i] << 12, sizes[i]))
        return found(ExportRead::Texture, found_slots);
    }
  }
  MemExportStream exports[kMaxMemExportStreams];
  u32 count = 0;
  GatherMemExportStreams(vs.entry, vs_constants, exports, count);
  if (ps)
    GatherMemExportStreams(ps->entry, ps_constants, exports, count);
  uint64_t bytes = 0;
  for (u32 i = 0; i < count; ++i)
    bytes += (exports[i].size + 255u) & ~255u;
  const auto &s = State();
  const uint64_t used = g_exports[s.frame].used;
  if (bytes && used &&
      used + bytes > std::min(s.memexport_capacity[s.frame], s.memexport_readback_capacity[s.frame])) {
    return found(ExportRead::Capacity, current);
  }
  return ExportRead::None;
}

// Recreates 'buffer' with at least 'size' bytes when it is smaller.
bool EnsureBuffer(std::unique_ptr<plume::RenderBuffer> &buffer, uint64_t &capacity, uint64_t size,
                  const plume::RenderBufferDesc &desc_template, const char *name) {
  if (buffer && capacity >= size)
    return true;
  uint64_t grown = std::max<uint64_t>(capacity * 2, 4u << 20);
  while (grown < size)
    grown *= 2;
  plume::RenderBufferDesc desc = desc_template;
  desc.size = grown;
  if (buffer)
    Video::RetireLocked(std::move(buffer));
  buffer = State().device->createBuffer(desc);
  capacity = buffer ? grown : 0;
  if (buffer)
    buffer->setName(name);
  return buffer != nullptr;
}

// The current slot's export buffer, in its unordered-access state (created on
// first use).
plume::RenderBuffer *MemExportBufferLocked(VideoState &s, uint64_t size = 256) {
  auto &buffer = s.memexport_buffer[s.frame];
  const bool had = buffer && s.memexport_capacity[s.frame] >= size;
  if (!EnsureBuffer(buffer, s.memexport_capacity[s.frame], size,
                    plume::RenderBufferDesc::DefaultBuffer(
                        0, StorageBufferFlags() | AddressableBufferFlags()),
                    "renut memexport buffer")) {
    return nullptr;
  }
  if (!had) {
    s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                             plume::RenderBufferBarrier(buffer.get(),
                                                        plume::RenderBufferAccess::WRITE));
  }
  return buffer.get();
}

// A part of a draw's export window whose seed comes from a pending export's
// window instead of guest memory.
struct SeedPiece {
  u32 slot = 0;
  uint64_t source = 0;  // in the slot's export buffer
  uint64_t dest = 0;    // in the current slot's export buffer
  u32 size = 0;
};

// Guest memory lacks what the pending exports will write there, the older
// slots' and this slot's own earlier draws': copies their windows over the
// parts of the draw's windows they overlap, each byte from the newest export
// of it (the copies of one batch are unordered, so no two overlap). Waiting
// for this slot's instead cost a GPU round trip per such draw, the whole
// frame's GPU work so far. A slot's own windows pass through its scratch
// buffer, as a buffer cannot be copied into itself. The copies wait for the
// guest seed through the buffer's unordered-access state in between.
bool SeedFromPendingExportsLocked(VideoState &s, plume::RenderBuffer *buffer,
                                  const MemExportStream *streams, u32 count, const u32 *offsets) {
  static std::vector<SeedPiece> pieces;
  static std::vector<std::pair<u32, u32>> covered;  // [begin, end), newer exports
  static std::vector<std::pair<u32, u32>> parts;
  pieces.clear();
  for (u32 i = 0; i < count; ++i) {
    const u32 stream_begin = streams[i].base, stream_end = streams[i].base + streams[i].size;
    covered.clear();
    // Newest first: this slot's latest draw, back to the oldest slot's first.
    for (u32 k = kFramesInFlight; k >= 1; --k) {
      const u32 slot = (s.frame + k) % kFramesInFlight;
      if (!s.memexport_buffer[slot])
        continue;
      const auto &pending = g_exports[slot].pending;
      for (auto p = pending.rbegin(); p != pending.rend(); ++p) {
        const u32 begin = std::max(p->base, stream_begin);
        const u32 end = std::min(p->base + p->size, stream_end);
        if (begin >= end)
          continue;
        // What of [begin, end) no newer export covers.
        parts.assign(1, {begin, end});
        for (const auto &[cb, ce] : covered) {
          for (size_t j = 0; j < parts.size();) {
            const auto [pb, pe] = parts[j];
            if (ce <= pb || pe <= cb) {
              ++j;
              continue;
            }
            parts.erase(parts.begin() + j);
            if (pb < cb)
              parts.insert(parts.begin() + j++, {pb, cb});
            if (ce < pe)
              parts.insert(parts.begin() + j++, {ce, pe});
          }
        }
        for (const auto &[pb, pe] : parts) {
          pieces.push_back({slot, p->offset + (pb - p->base),
                            offsets[i] + uint64_t(pb - stream_begin), pe - pb});
        }
        covered.push_back({begin, end});
      }
    }
  }
  if (pieces.empty())
    return true;

  // This slot's own pieces go to the scratch buffer first.
  uint64_t own_bytes = 0;
  for (const SeedPiece &piece : pieces) {
    if (piece.slot == s.frame)
      own_bytes += piece.size;
  }
  plume::RenderBuffer *scratch = nullptr;
  uint64_t scratch_offset = 0;
  if (own_bytes) {
    uint64_t &used = s.memexport_scratch_used[s.frame];
    if (!s.memexport_scratch[s.frame] || used + own_bytes > s.memexport_scratch_capacity[s.frame]) {
      // Copies recorded before still use the old one, retired with the slot.
      if (!EnsureBuffer(s.memexport_scratch[s.frame], s.memexport_scratch_capacity[s.frame],
                        used + own_bytes, plume::RenderBufferDesc::DefaultBuffer(0),
                        "renut memexport scratch")) {
        return false;
      }
      used = 0;
    }
    scratch = s.memexport_scratch[s.frame].get();
    scratch_offset = used;
    used += own_bytes;
  }

  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderBufferBarrier barriers[kFramesInFlight + 1];
  u32 barrier_count = 0;
  if (scratch) {
    barriers[0] = plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::READ);
    barriers[1] = plume::RenderBufferBarrier(scratch, plume::RenderBufferAccess::WRITE);
    cmd->barriers(plume::RenderBarrierStage::COPY, barriers, 2);
    uint64_t at = scratch_offset;
    for (const SeedPiece &piece : pieces) {
      if (piece.slot != s.frame)
        continue;
      cmd->copyBufferRegion(plume::RenderBufferReference(scratch, at),
                            plume::RenderBufferReference(buffer, piece.source), piece.size);
      at += piece.size;
    }
    barriers[barrier_count++] = plume::RenderBufferBarrier(scratch, plume::RenderBufferAccess::READ);
  }
  barriers[barrier_count++] = plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE);
  for (u32 k = 1; k < kFramesInFlight; ++k) {
    const u32 slot = (s.frame + k) % kFramesInFlight;
    if (s.memexport_buffer[slot]) {
      barriers[barrier_count++] =
          plume::RenderBufferBarrier(s.memexport_buffer[slot].get(), plume::RenderBufferAccess::READ);
    }
  }
  cmd->barriers(plume::RenderBarrierStage::COPY, barriers, barrier_count);
  uint64_t at = scratch_offset;
  for (const SeedPiece &piece : pieces) {
    if (piece.slot == s.frame) {
      cmd->copyBufferRegion(plume::RenderBufferReference(buffer, piece.dest),
                            plume::RenderBufferReference(scratch, at), piece.size);
      at += piece.size;
    } else {
      cmd->copyBufferRegion(plume::RenderBufferReference(buffer, piece.dest),
                            plume::RenderBufferReference(s.memexport_buffer[piece.slot].get(),
                                                         piece.source),
                            piece.size);
    }
  }
  // Back to unordered access, as the other export buffers were.
  barrier_count = 0;
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    if (s.memexport_buffer[slot]) {
      barriers[barrier_count++] =
          plume::RenderBufferBarrier(s.memexport_buffer[slot].get(), plume::RenderBufferAccess::WRITE);
    }
  }
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, barrier_count);
  return true;
}

// Seeds the draw's export windows from guest memory (and the older slots'
// pending exports) and fills its stream table. The windows follow the slot's
// earlier ones in its export buffer, which keeps them for later draws' seeds;
// returns their offsets.
bool PrepareMemExportLocked(VideoState &s, const MemExportStream *streams, u32 count,
                            SharedConstants &sc, u32 *offsets) {
  const uint64_t used = g_exports[s.frame].used;
  uint64_t total = used;
  for (u32 i = 0; i < count; ++i) {
    offsets[i] = u32(total);
    total += (streams[i].size + 255u) & ~255u;
  }
  plume::RenderBuffer *buffer = MemExportBufferLocked(s, total);
  if (!buffer || !EnsureBuffer(s.memexport_readback[s.frame], s.memexport_readback_capacity[s.frame],
                               total, plume::RenderBufferDesc::ReadbackBuffer(0),
                               "renut memexport readback")) {
    return false;
  }
  plume::RenderCommandList *cmd = s.command_list;
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
  for (u32 i = 0; i < count; ++i) {
    UploadAllocation seed = Video::UploadsLocked().Allocate(streams[i].size, 256);
    const u8 *guest = GuestPhysicalPtr<const u8>(streams[i].base);
    if (!seed || !guest)
      return false;
    std::memcpy(seed.cpu, guest, streams[i].size);
    cmd->copyBufferRegion(plume::RenderBufferReference(buffer, offsets[i]), seed.ref,
                          streams[i].size);
    sc.memexport_streams[i][0] = streams[i].base;
    sc.memexport_streams[i][1] = streams[i].size;
    sc.memexport_streams[i][2] = offsets[i];
  }
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
  return SeedFromPendingExportsLocked(s, buffer, streams, count, offsets);
}

// After the draw: copies its windows to the slot's readback buffer, pending
// until the slot's fence is waited.
void FinishMemExportLocked(VideoState &s, const MemExportStream *streams, u32 count,
                           const u32 *offsets) {
  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderBuffer *buffer = s.memexport_buffer[s.frame].get();
  plume::RenderBuffer *readback = s.memexport_readback[s.frame].get();
  SlotExports &slot = g_exports[s.frame];
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::READ));
  for (u32 i = 0; i < count; ++i) {
    cmd->copyBufferRegion(plume::RenderBufferReference(readback, offsets[i]),
                          plume::RenderBufferReference(buffer, offsets[i]), streams[i].size);
    slot.pending.push_back({streams[i].base, streams[i].size, offsets[i]});
    MarkExportGranules(streams[i].base, streams[i].size);
    slot.used = offsets[i] + ((streams[i].size + 255u) & ~255u);
  }
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
}

// Brings the pending exports of 'slots' to guest memory, with those of every
// older slot (so a later export of the same memory lands last): waits for the
// older slots' fences, and submits the open list and waits for it when the
// current slot is one of them.
void WaitForExportsLocked(u32 slots) {
  auto &s = State();
  u32 last = 0;  // in age order: 1 is the oldest slot, kFramesInFlight the current one
  for (u32 k = 1; k <= kFramesInFlight; ++k) {
    if (slots & (1u << ((s.frame + k) % kFramesInFlight)))
      last = k;
  }
  for (u32 k = 1; k <= last; ++k) {
    const u32 slot = (s.frame + k) % kFramesInFlight;
    if (g_exports[slot].pending.empty())
      continue;
    if (slot == s.frame)
      detail::SubmitAndWaitLocked(s);
    else
      detail::WaitSlotLocked(s, slot);
    detail::CompleteMemExportsLocked(slot);
  }
}

// Builds the host vertices of the draw's streams whose newest data is in a
// pending export's window (FindExportedStream; DrawNeedsExportFlush did not
// wait for those) with stream_convert_cs, into the slot's stream buffer.
// Records dispatches: before the framebuffer is bound.
bool ConvertExportedStreamsLocked(VideoState &s, const GuestDevice &device, const DrawCall &call,
                                  const ShaderRecord &vs, const u32 *shader_strides,
                                  const VertexDeclarationRecord &declaration, const Geometry &g,
                                  GpuStreams &out) {
  if (!s.stream_convert_pipeline || !AnyPendingExports())
    return true;
  // The streams whose memory a pending export writes, first: exports are
  // pending nearly always, and for most draws none is (the jobs below are
  // big to set up).
  u32 pending = 0;
  for (u32 stream = 0; stream < kMaxVertexStreams; ++stream) {
    if (!declaration.layout.streams[stream].used)
      continue;
    const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
    const u32 base = device.VertexFetchWord(fetch_index, 0) & ~3u;
    const u32 size = device.VertexFetchWord(fetch_index, 1) & 0x3FFFFFCu;
    if (PendingExportSlots(base, size))
      pending |= 1u << stream;
  }
  if (!pending)
    return true;
  struct Job {
    u32 stream;
    ExportedStream src;
    u32 first, count;
  };
  Job jobs[kMaxVertexStreams];
  u32 job_count = 0;
  uint64_t total = 0;
  for (u32 bits = pending; bits; bits &= bits - 1) {
    const u32 stream = u32(std::countr_zero(bits));
    Job job{stream};
    if (!FindExportedStream(device, call, vs, shader_strides, declaration, stream, job.src))
      continue;
    // The vertices BindStreams would convert.
    job.first = g.first_vertex;
    if (job.first >= job.src.vertex_count)
      continue;
    job.count = std::min(g.vertex_range, job.src.vertex_count - job.first);
    if (!job.count)
      continue;
    total += (uint64_t(job.count) * job.src.host_stride + 255u) & ~uint64_t(255);
    jobs[job_count++] = job;
  }
  if (!job_count)
    return true;

  // A buffer too small is replaced; the draws before still read the old one,
  // retired with the slot.
  uint64_t &used = s.exported_streams_used[s.frame];
  if (!s.exported_streams[s.frame] || used + total > s.exported_streams_capacity[s.frame]) {
    if (!EnsureBuffer(s.exported_streams[s.frame], s.exported_streams_capacity[s.frame],
                      used + total,
                      plume::RenderBufferDesc::DefaultBuffer(
                          0, StorageBufferFlags() | plume::RenderBufferFlag::VERTEX),
                      "renut exported streams")) {
      return false;
    }
    used = 0;
  }
  plume::RenderBuffer *dest = s.exported_streams[s.frame].get();
  const plume::RenderBuffer *buffers[kStreamConvertBuffers];
  for (u32 slot = 0; slot < kFramesInFlight; ++slot)
    buffers[slot] = s.memexport_buffer[slot] ? s.memexport_buffer[slot].get() : dest;
  buffers[kFramesInFlight] = dest;
  if (!s.stream_convert_set ||
      std::memcmp(buffers, s.stream_convert_set_buffers, sizeof(buffers)) != 0) {
    // Dispatches recorded before still use the old set.
    Video::RetireLocked(std::move(s.stream_convert_set));
    s.stream_convert_set = detail::CreateStreamConvertSet(s);
    if (!s.stream_convert_set)
      return false;
    for (u32 i = 0; i < kStreamConvertBuffers; ++i)
      s.stream_convert_set->setBuffer(i, buffers[i]);
    std::memcpy(s.stream_convert_set_buffers, buffers, sizeof(buffers));
  }

  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderBufferBarrier barriers[kFramesInFlight + 1];
  u32 barrier_count = 0;
  barriers[barrier_count++] = plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::WRITE);
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    if (s.memexport_buffer[slot]) {
      barriers[barrier_count++] =
          plume::RenderBufferBarrier(s.memexport_buffer[slot].get(), plume::RenderBufferAccess::WRITE);
    }
  }
  static const uint32_t kProfileLabel = gpu_profile::Label("exported stream conversion");
  gpu_profile::Mark(s, kProfileLabel);
  cmd->barriers(plume::RenderBarrierStage::COMPUTE, barriers, barrier_count);
  cmd->setComputePipelineLayout(s.stream_convert_layout.get());
  cmd->setPipeline(s.stream_convert_pipeline.get());
  cmd->setComputeDescriptorSet(s.stream_convert_set.get(), 0);
  for (u32 i = 0; i < job_count; ++i) {
    const Job &job = jobs[i];
    const u32 bytes = job.count * job.src.host_stride;
    StreamConvertPush push{};
    push.source = job.src.slot;
    push.source_offset = u32(job.src.offset + uint64_t(job.first) * job.src.stride);
    push.dest_offset = u32(used);
    push.vertices = job.count;
    push.guest_stride_words = job.src.stride / 4;
    push.host_stride_words = job.src.host_stride / 4;
    push.element_count = job.src.element_count;
    std::memcpy(push.elements, job.src.elements, sizeof(push.elements));
    cmd->setComputePushConstants(0, &push, 0, sizeof(push));
    cmd->dispatch((job.count * job.src.element_count + 63) / 64, 1, 1);
    out.ref[job.stream] = plume::RenderBufferReference(dest, used);
    out.bytes[job.stream] = bytes;
    used += (uint64_t(bytes) + 255u) & ~uint64_t(255);
  }
  barriers[0] = plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::READ);
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, barrier_count);
  // The compute pipeline replaced the graphics one (D3D12), and the barriers
  // end any render pass.
  g_bound.pipeline = nullptr;
  g_bound.framebuffer = nullptr;
  return true;
}

// The GPU fills the persistent copies made since the last flush from the
// upload memory their draws filled (PoolFill), and the cache entries take
// them: draws from here on bind them. Records copies, which end any render
// pass. Without an open list (none were made then) they are let go.
void FlushPoolFillsLocked() {
  if (g_stream_pool_fills.empty() && g_index_pool_fills.empty() && g_fetch_pool_fills.empty())
    return;
  auto &s = State();
  plume::RenderCommandList *cmd = s.list_open ? s.command_list : nullptr;
  // The pool buffers written to, once each: the vertex and index pools' read
  // as such after, the fetch pool's by shaders.
  static std::vector<plume::RenderBufferBarrier> barriers, fetch_barriers;
  barriers.clear();
  fetch_barriers.clear();
  auto written = [](std::vector<plume::RenderBufferBarrier> &list, const PoolSpan &span) {
    for (const plume::RenderBufferBarrier &b : list) {
      if (b.buffer == span.buffer)
        return;
    }
    list.push_back(plume::RenderBufferBarrier(span.buffer, plume::RenderBufferAccess::WRITE));
  };
  for (const StreamPoolFill &f : g_stream_pool_fills)
    written(barriers, f.span);
  for (const IndexPoolFill &f : g_index_pool_fills)
    written(barriers, f.span);
  for (const FetchPoolFill &f : g_fetch_pool_fills)
    written(fetch_barriers, f.span);
  if (cmd) {
    static std::vector<plume::RenderBufferBarrier> all;
    all.assign(barriers.begin(), barriers.end());
    all.insert(all.end(), fetch_barriers.begin(), fetch_barriers.end());
    cmd->barriers(plume::RenderBarrierStage::COPY, all.data(), u32(all.size()));
  }

  for (const StreamPoolFill &f : g_stream_pool_fills) {
    auto it = g_stream_cache.find(f.key);
    CachedStream *c = it != g_stream_cache.end() ? &it->second : nullptr;
    if (cmd && c && c->pending && !c->copy) {
      cmd->copyBufferRegion(f.span.Ref(), f.source, f.bytes);
      c->copy = f.span;
      c->bytes = f.bytes;
      c->checked_frame = g_stream_cache_frame;
    } else {
      g_copy_pool.Free(f.span);
      g_stream_cache_bytes -= f.bytes;
    }
    if (c)
      c->pending = false;
  }
  for (const IndexPoolFill &f : g_index_pool_fills) {
    auto it = g_index_cache.find(f.key);
    CachedIndices *c = it != g_index_cache.end() ? &it->second : nullptr;
    if (cmd && c && c->pending && !c->copy) {
      cmd->copyBufferRegion(f.span.Ref(), f.source, f.bytes);
      c->copy = f.span;
      c->bytes = f.bytes;
      c->index_count = f.index_count;
      c->lo = f.lo;
      c->max_index = f.max_index;
      c->wide = f.wide;
    } else {
      g_index_pool.Free(f.span);
      g_stream_cache_bytes -= f.bytes;
    }
    if (c)
      c->pending = false;
  }
  for (const FetchPoolFill &f : g_fetch_pool_fills) {
    auto it = g_fetch_cache.find(f.key);
    CachedFetch *c = it != g_fetch_cache.end() ? &it->second : nullptr;
    if (cmd && c && c->pending && !c->copy) {
      cmd->copyBufferRegion(f.span.Ref(), f.source, f.bytes);
      c->copy = f.span;
      c->bytes = f.bytes;
      c->checked_frame = g_stream_cache_frame;
    } else {
      g_fetch_pool.Free(f.span);
      g_stream_cache_bytes -= f.bytes;
    }
    if (c)
      c->pending = false;
  }
  g_stream_pool_fills.clear();
  g_index_pool_fills.clear();
  g_fetch_pool_fills.clear();

  if (cmd) {
    for (plume::RenderBufferBarrier &b : barriers)
      b.accessBits = plume::RenderBufferAccess::READ;
    for (plume::RenderBufferBarrier &b : fetch_barriers)
      b.accessBits = plume::RenderBufferAccess::READ;
    if (!barriers.empty())
      cmd->barriers(plume::RenderBarrierStage::GRAPHICS, barriers.data(), u32(barriers.size()));
    // As gpu_fetch_data is (UploadVertexFetchData): a root SRV on D3D12.
    if (!fetch_barriers.empty()) {
      cmd->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE, fetch_barriers.data(),
                    u32(fetch_barriers.size()));
    }
    g_bound.framebuffer = nullptr;
  }
}

// Stands in for a null vertex declaration: the vertex shader fetches its data
// by address (vfetchMemory), and any input it declares reads the zero stream.
VertexDeclarationRecord &NoDeclaration() {
  static VertexDeclarationRecord record = [] {
    VertexDeclarationRecord r;
    r.layout = BuildHostVertexLayout(nullptr, 0);
    r.layout_hash = 0x4E4F44454300ull;  // any value no FNV hash of a real layout hits
    return r;
  }();
  return record;
}

} // namespace

namespace detail {

void FinishRecordingLocked(VideoState &) {
  FlushVertexFillsLocked();
  FlushTextureFillsLocked();
}

} // namespace detail

void FlushVertexFillsLocked() {
  // Each fill and copy reads its own guest bytes into its own upload memory:
  // they go on the helper threads too (parallel.h).
  const size_t fills = g_stream_fill_count;
  parallel::For(fills + g_copy_fills.size(), 4, [fills](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      if (i < fills) {
        const PendingStreamFill &f = g_stream_fills[i];
        ConvertVertices(f.stream, f.guest, f.stride, f.count, f.endian, f.host);
      } else {
        const PendingCopy &c = g_copy_fills[i - fills];
        std::memcpy(c.dst, c.src, c.size);
      }
    }
  });
  for (size_t i = 0; i < fills; ++i) {
    if (g_stream_fills[i].cacheable)
      TrackConvertedStream(g_stream_fills[i]);
  }
  TrackFetchCopies();
  // The copies those (and the draws' index ranges) made.
  FlushPoolFillsLocked();
  g_stream_fill_count = 0;
  g_queued_streams.clear();
  g_copy_fills.clear();
  ++g_fetch_copy_generation;
}

void SetTilesLocked(const ClearRect *rects, u32 count) {
  g_tiles.assign(rects, rects + (rects ? count : 0));
}

u32 SetReplayPredicationLocked(u32 mask) {
  const u32 previous = g_replay_predication;
  g_replay_predication = mask;
  return previous;
}

bool DrawStateLocked(const u8 *device_state, const DrawCall &call, const ReplayConstants *replay) {
  auto &s = State();
  const GuestDevice device(device_state);
  if (!device.valid())
    return false;
  render_profile::Stages stages;
  stages.Enter(render_profile::Stage::kShaders);

  const u32 vs_va = device.U32(d3d::kDeviceOffsetVertexShader);
  auto *vs = FindShaderLocked(vs_va, RecordKind::VertexShader);
  const u32 declaration_va = device.U32(d3d::kDeviceOffsetVertexDeclaration);
  auto *ps = FindShaderLocked(device.U32(d3d::kDeviceOffsetPixelShader), RecordKind::PixelShader);
  // Without a declaration set, the shader fetches as D3DVertexShader_Bind
  // bound it, or else as its own vfetches describe (bundle meshes).
  const VertexDeclarationRecord *declaration = nullptr;
  const u32 *shader_strides = nullptr;  // see GetStreamSource
  if (declaration_va) {
    if (declaration_va != g_last_declaration_va) {
      g_last_declaration =
          FindRecord<VertexDeclarationRecord>(declaration_va, RecordKind::VertexDeclaration);
      g_last_declaration_va = g_last_declaration ? declaration_va : 0;
    }
    declaration = g_last_declaration_va == declaration_va ? g_last_declaration : nullptr;
  } else if (vs && vs->bound_declaration) {
    declaration = vs->bound_declaration.get();
    shader_strides = vs->bound_strides;
  } else if (const VertexDeclarationRecord *fetch = vs ? ShaderFetchDeclarationLocked(*vs) : nullptr) {
    declaration = fetch;
    shader_strides = vs->fetch_strides;
  } else {
    declaration = &NoDeclaration();
  }
  if (!vs || !declaration) {
    static uint32_t logged = 0;
    if (LogLimited(logged, 16)) {
      const u32 va = vs ? declaration_va : vs_va;
      const auto *words = GuestPtr<const be_u32>(va);
      RGPU_WARN("Draw dropped: {} {:08X} is not one the renderer knows (words {:08X} {:08X})",
                vs ? "vertex declaration" : "vertex shader", va, words ? u32(words[0]) : 0,
                words ? u32(words[1]) : 0);
    }
    return false;
  }

  // Earlier memexport results reach guest memory before this draw reads them
  // (WaitForExportsLocked may submit the list: before anything is recorded).
  stages.Enter(render_profile::Stage::kExportWait);
  const FloatConstants vs_constants = DrawFloatConstants(device_state, replay, false);
  const FloatConstants ps_constants = DrawFloatConstants(device_state, replay, true);
  // Waiting for an older slot's results leaves the current slot's, which the
  // draw may need as well: another pass checks (none are left after it).
  while (AnyPendingExports()) {
    u32 slots = 0;
    const ExportRead read = DrawNeedsExportFlush(device, call, *vs, ps, *declaration,
                                                 shader_strides, vs_constants, ps_constants, slots);
    if (read == ExportRead::None)
      break;
    static uint32_t wait_logged = 0;
    if (LogLimited(wait_logged)) {
      static constexpr const char *kReadNames[] = {"none",    "vertex stream", "vertex fetch", "indices",
                                                   "texture", "capacity"};
      RGPU_WARN("Draw waits for memexport results ({}; VS {:016X}, PS {:016X}{})",
                kReadNames[u32(read)], vs->hash, ps ? ps->hash : 0,
                (slots & (1u << s.frame)) ? ", this frame's: GPU round trip" : "");
    }
    WaitForExportsLocked(slots);
  }
  stages.Enter(render_profile::Stage::kShaders);
  if (!Video::EnsureCommandListLocked())
    return false;
  SyncBound(s);

  plume::RenderShader *host_vs = GetHostShader(vs);
  plume::RenderShader *host_ps = ps ? GetHostShader(ps) : nullptr;
  if (!host_vs || (ps && !host_ps))
    return false;  // missing from the shader cache (captured for the next build)

  Targets targets;
  if (!GatherTargets(device, targets))
    return false;

  // Everything that records copies or barriers comes before the framebuffer.
  stages.Enter(render_profile::Stage::kTextures);
  SharedConstants sc;
  std::memset(&sc, 0, sizeof(sc));
  const u32 texture_mask =
      (vs->entry ? vs->entry->textureFetchMask : 0) | (ps && ps->entry ? ps->entry->textureFetchMask : 0);
  FillTextures(device, texture_mask, sc);

  stages.Enter(render_profile::Stage::kConstants);
  const d3d::Device &dev = device.dev();
  const u8 *bools = reinterpret_cast<const u8 *>(&dev.m_Constants.VertexShaderB[0]);
  SwapEndian(2, bools, sc.booleans, sizeof(sc.booleans));
  const u32 color_control = dev.m_ControlPacket.ColorControl;
  sc.alpha_test_func = (color_control & 8) ? (color_control & 7) : 7;
  sc.alpha_threshold = dev.m_ValuesPacket.AlphaRef;
  const ViewportState viewport = ComputeViewport(device, targets);
  sc.screen_space = viewport.screen_space ? 1 : 0;
  std::memcpy(sc.screen_scale, viewport.screen_scale, sizeof(sc.screen_scale));
  std::memcpy(sc.screen_offset, viewport.screen_offset, sizeof(sc.screen_offset));
  sc.viewport_size[0] = viewport.guest_size[0];
  sc.viewport_size[1] = viewport.guest_size[1];
  sc.resolution_scale = targets.scale.Factor();
  std::memcpy(sc.pixel_origin, viewport.pixel_origin, sizeof(sc.pixel_origin));
  if (call.primitive == kPointList) {
    // PA_SU_POINT_SIZE: height 0-15, width 16-31, half the size in 12.4 fixed
    // point (SetRenderState_PointSize 0x8264C6C0 stores the size * 8);
    // PA_SU_POINT_MINMAX: minimum 0-15, maximum 16-31, the size in 12.4
    // (SetRenderState_PointSizeMin 0x8264C720 stores the minimum * 16).
    const auto *tessellator = reinterpret_cast<const be_u32 *>(dev.m_TessellatorPacket);
    const u32 point_size = tessellator[0], point_min_max = tessellator[1];
    sc.point_state[0] = float(point_size >> 16) / 8.0f;
    sc.point_state[1] = float(point_size & 0xFFFF) / 8.0f;
    sc.point_state[2] = float(point_min_max & 0xFFFF) / 16.0f;
    sc.point_state[3] = float(point_min_max >> 16) / 16.0f;
  }

  stages.Enter(render_profile::Stage::kGeometry);
  Geometry geometry;
  if (call.tessellation ? !PrepareTessellation(device, call, geometry, sc.tessellation)
                        : !PrepareGeometry(device, call, geometry)) {
    return false;
  }
  // The host draws the vertex range in use from vertex 0 (indices rebased to
  // it in PrepareGeometry); the guest's r0.x is the vertex index itself.
  if (!call.tessellation)
    sc.tessellation[3] = geometry.first_vertex;
  stages.Enter(render_profile::Stage::kVertexFetch);
  GpuStreams gpu_streams;
  if (!ConvertExportedStreamsLocked(s, device, call, *vs, shader_strides, *declaration, geometry,
                                    gpu_streams)) {
    return false;
  }

  const u32 fetch_mask = (vs->entry ? vs->entry->vertexFetchMemoryMask : 0) |
                         DescribeBoundFetches(device, call, *vs, *declaration, shader_strides, sc);
  UploadAllocation fetch_alloc = UploadVertexFetchData(device, call, fetch_mask, sc);
  stages.Enter(render_profile::Stage::kConstants);
  static constexpr u32 kAllRegisters[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
  static constexpr u32 kNoRegisters[8] = {};
  const u32 *vs_used = vs->entry ? vs->entry->floatConstants : kAllRegisters;
  const u32 *ps_used = !ps ? kNoRegisters : ps->entry ? ps->entry->floatConstants : kAllRegisters;
  UploadAllocation vs_alloc = UploadFloatConstants(vs_constants, vs_used, g_bound.vs_constants);
  UploadAllocation ps_alloc = UploadFloatConstants(ps_constants, ps_used, g_bound.ps_constants);
  if (ps_alloc && ps && DepthOfFieldStrength() != 1.0f && IsDepthOfFieldDraw(vs->hash, ps_used))
    ps_alloc = ScaleDepthOfField(g_bound.ps_constants, DepthOfFieldStrength());
  if (ps_alloc && ps && BloomStrength() != 1.0f && IsBloomDraw(ps->hash, ps_used))
    ps_alloc = ScaleBloom(g_bound.ps_constants, BloomStrength());
  MemExportStream exports[kMaxMemExportStreams];
  u32 export_count = 0;
  u32 export_offsets[kMaxMemExportStreams] = {};
  GatherMemExportStreams(vs->entry, vs_constants, exports, export_count);
  if (ps)
    GatherMemExportStreams(ps->entry, ps_constants, exports, export_count);
  if (export_count &&!PrepareMemExportLocked(s, exports, export_count, sc, export_offsets)) {
    export_count = 0;
    std::memset(sc.memexport_streams, 0, sizeof(sc.memexport_streams));
  }
  plume::RenderBuffer *memexport = MemExportBufferLocked(s);
  if (memexport && IsVulkan())
    sc.memexport_address = memexport->getDeviceAddress();
  UploadAllocation sc_alloc = Video::UploadsLocked().Upload(&sc, sizeof(sc), 256);
  // (Fetch data from the fetch cache has no upload memory, only the buffer.)
  if (!fetch_alloc.ref.ref || !vs_alloc || !ps_alloc || !sc_alloc || !memexport)
    return false;

  stages.Enter(render_profile::Stage::kPipeline);
  const PipelineKey key =
      MakeKey(device, vs, ps, declaration, targets, call.primitive, geometry.per_instance);
  plume::RenderPipeline *pipeline =
      GetPipelineLocked(key, {host_vs, host_ps, &declaration->layout});
  if (!pipeline)
    return false;
  stages.Enter(render_profile::Stage::kTargets);
  if (!BindTargets(targets))
    return false;
  stages.Enter(render_profile::Stage::kRecord);
  plume::RenderCommandList *cmd = s.command_list;
  SetViewport(cmd, viewport.viewport, viewport.scissor, targets.color_count);

  if (!g_bound.layout) {
    cmd->setGraphicsPipelineLayout(s.draw_layout.get());
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 1);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 2);
    cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 3);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 4);
    g_bound.layout = true;
    g_bound.pipeline = nullptr;
    std::fill(std::begin(g_bound.root_set), std::end(g_bound.root_set), false);
  }
  if (g_bound.pipeline != pipeline) {
    cmd->setPipeline(pipeline);
    g_bound.pipeline = pipeline;
    // Vulkan: a pipeline without the constant as dynamic state overwrites it.
    if (IsVulkan())
      g_bound.blend_constant_set = false;
  }
  // RB_STENCILREFMASK's reference (D3D12 has one for both faces). N&B's
  // silhouettes (the Jinjo Bingo prize previewer, sub_823E7558) mark the model
  // with reference 1 and draw black where the stencil equals it.
  if (!IsVulkan() && g_bound.stencil_reference != (key.stencil_ref_mask & 0xFF)) {
    g_bound.stencil_reference = key.stencil_ref_mask & 0xFF;
    SetStencilReference(cmd, g_bound.stencil_reference);
  }
  float blend_constant[4];
  if (DrawBlendConstant(dev, key, blend_constant) &&
      !(g_bound.blend_constant_set &&
        std::memcmp(blend_constant, g_bound.blend_constant, sizeof(blend_constant)) == 0)) {
    if (SetBlendConstant(cmd, blend_constant)) {
      std::memcpy(g_bound.blend_constant, blend_constant, sizeof(blend_constant));
      g_bound.blend_constant_set = true;
    } else {
      static bool logged = false;
      if (!logged) {
        logged = true;
        RGPU_WARN("Draws blend with the blend constant, which this backend cannot set");
      }
    }
  }
  if (IsVulkan()) {
    const DrawPushConstants push{vs_alloc.gpu_address, ps_alloc.gpu_address, sc_alloc.gpu_address};
    cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  } else {
    SetRootDescriptor(cmd, vs_alloc.ref, kDrawRootVertexConstants);
    SetRootDescriptor(cmd, ps_alloc.ref, kDrawRootPixelConstants);
    SetRootDescriptor(cmd, sc_alloc.ref, kDrawRootSharedConstants);
    SetRootDescriptor(cmd, fetch_alloc.ref, kDrawRootVertexFetchData);
    SetRootDescriptor(cmd, plume::RenderBufferReference(memexport), kDrawRootMemExport);
  }

  stages.Enter(render_profile::Stage::kStreams);
  if (!BindStreams(device, call, *vs, shader_strides, declaration->layout,
                   declaration->layout_hash, geometry, gpu_streams)) {
    return false;
  }
  stages.Enter(render_profile::Stage::kRecord);
  render_profile::CountDraw();
  if (geometry.indexed) {
    SetIndexBuffer(cmd, geometry.index_view);
    cmd->drawIndexedInstanced(geometry.index_count, 1, 0, 0, 0);
  } else {
    cmd->drawInstanced(geometry.vertex_count, geometry.instance_count, 0, 0);
  }
  if (export_count) {
    FinishMemExportLocked(s, exports, export_count, export_offsets);
    g_bound.framebuffer = nullptr;  // the copies end any render pass
  }
  return true;
}

namespace detail {

void ReleaseCacheCopiesLocked(uint32_t slot) {
  g_copy_pool.ReleaseSlot(slot);
  g_index_pool.ReleaseSlot(slot);
  g_fetch_pool.ReleaseSlot(slot);
}

void CompleteMemExportsLocked(uint32_t slot) {
  SlotExports &exports = g_exports[slot];
  if (exports.pending.empty())
    return;
  auto &s = State();
  plume::RenderBuffer *readback = s.memexport_readback[slot].get();
  if (const auto *data = readback ? static_cast<const u8 *>(readback->map()) : nullptr) {
    for (const PendingExport &p : exports.pending) {
      if (u8 *guest = GuestPhysicalPtr<u8>(p.base))
        std::memcpy(guest, data + p.offset, p.size);
    }
    readback->unmap();
  }
  exports.pending.clear();
  exports.used = 0;
  RebuildExportGranules();
}

} // namespace detail

void ForgetVertexDeclarationLocked(u32 declaration_va) {
  if (declaration_va == g_last_declaration_va) {
    g_last_declaration_va = 0;
    g_last_declaration = nullptr;
  }
}

void AdvanceStreamCacheFrameLocked() {
  // Copies whose guest bytes changed (ContentVerifier), unless made again
  // since they were checked.
  static std::vector<decltype(g_stream_verifier)::Check> stale;
  g_stream_verifier.Flush();
  g_stream_verifier.TakeStale(stale);
  for (const auto &check : stale) {
    auto it = g_stream_cache.find(check.key);
    if (it == g_stream_cache.end())
      continue;
    CachedStream &c = it->second;
    if (!c.copy || c.hash != check.hash)
      continue;
    DropCachedCopy(c);
    c.stable = 0;
    c.checked_frame = UINT64_MAX;
    c.dynamic = ++c.drops >= kMaxCacheDrops;
  }
  stale.clear();

  static std::vector<decltype(g_index_verifier)::Check> stale_indices;
  g_index_verifier.Flush();
  g_index_verifier.TakeStale(stale_indices);
  for (const auto &check : stale_indices) {
    auto it = g_index_cache.find(check.key);
    if (it == g_index_cache.end())
      continue;
    CachedIndices &c = it->second;
    if (!c.copy || c.hash != check.hash)
      continue;
    DropCachedIndices(c);
    c.stable = 0;
    c.checked_frame = UINT64_MAX;  // hashed again by TrackIndices
    c.dynamic = ++c.drops >= kMaxCacheDrops;
  }
  stale_indices.clear();

  static std::vector<decltype(g_fetch_verifier)::Check> stale_fetches;
  g_fetch_verifier.Flush();
  g_fetch_verifier.TakeStale(stale_fetches);
  for (const auto &check : stale_fetches) {
    auto it = g_fetch_cache.find(check.key);
    if (it == g_fetch_cache.end())
      continue;
    CachedFetch &c = it->second;
    if (!c.copy || c.hash != check.hash)
      continue;
    DropCachedFetch(c);
    c.stable = 0;
    c.checked_frame = UINT64_MAX;  // hashed again by TrackFetchCopies
    c.dynamic = ++c.drops >= kMaxCacheDrops;
  }
  stale_fetches.clear();

  if (++g_stream_cache_frame % 60)
    return;
  // The memos may hold entries about to go.
  g_stream_memo.Clear();
  g_index_memo.Clear();
  g_fetch_memo.Clear();
  std::erase_if(g_fetch_cache, [](auto &entry) {
    if (g_stream_cache_frame - entry.second.last_used < kStreamCacheIdleFrames)
      return false;
    DropCachedFetch(entry.second);
    return true;
  });
  std::erase_if(g_stream_cache, [](auto &entry) {
    if (g_stream_cache_frame - entry.second.last_used < kStreamCacheIdleFrames)
      return false;
    DropCachedCopy(entry.second);
    return true;
  });
  std::erase_if(g_index_cache, [](auto &entry) {
    if (g_stream_cache_frame - entry.second.last_used < kStreamCacheIdleFrames)
      return false;
    DropCachedIndices(entry.second);
    return true;
  });
}

StreamCacheMemory StreamCacheMemoryLocked() {
  StreamCacheMemory usage;
  usage.pool_bytes = g_copy_pool.Bytes() + g_index_pool.Bytes() + g_fetch_pool.Bytes();
  usage.cached_bytes = g_stream_cache_bytes;
  usage.streams = g_stream_cache.size();
  usage.index_ranges = g_index_cache.size();
  return usage;
}

namespace {

// Clears on explicit targets (framebuffer bound as for a draw). A color clear
// reaches the targets over the same EDRAM in other host formats too
// (ColorAliasesLocked): on the console the clear writes the memory, whatever
// format a surface reads it as. N&B's paint shop clears its glow buffer
// through the 8-bit view of that EDRAM, then adds the glow particles through a
// 16-bit float view; the float target kept the frames before, and the glow
// piled up into drips.
void ClearTargets(const Targets &t, u32 flags, const plume::RenderRect &rect, const float color[4],
                  float depth, u32 stencil) {
  if (rect.right <= rect.left || rect.bottom <= rect.top || !BindTargets(t))
    return;
  plume::RenderCommandList *cmd = State().command_list;
  const plume::RenderRect host_rect = ScaleRect(rect, t.scale);
  const plume::RenderColor clear_color(color[0], color[1], color[2], color[3]);
  std::vector<HostSurface *> aliases;
  for (uint32_t i = 0; i < t.color_count; ++i) {
    if (!(flags & (1u << i)))
      continue;
    cmd->clearColor(i, clear_color, &host_rect, 1);
    std::vector<HostSurface *> found;
    ColorAliasesLocked(t.colors[i], found);
    for (HostSurface *alias : found) {
      if (std::find(aliases.begin(), aliases.end(), alias) == aliases.end() &&
          std::find(t.colors, t.colors + t.color_count, alias) == t.colors + t.color_count)
        aliases.push_back(alias);
    }
  }
  if (t.depth && (flags & 0x30)) {
    cmd->clearDepthStencil((flags & 0x10) != 0, (flags & 0x20) != 0, depth, stencil & 0xFF,
                           &host_rect, 1);
  }
  for (HostSurface *alias : aliases) {
    Targets a;
    a.colors[a.color_count++] = alias;
    a.width = alias->width;
    a.height = alias->height;
    a.scale = alias->scale;
    const plume::RenderRect alias_rect(
        std::min(rect.left, int32_t(alias->width)), std::min(rect.top, int32_t(alias->height)),
        std::min(rect.right, int32_t(alias->width)), std::min(rect.bottom, int32_t(alias->height)));
    if (alias_rect.right <= alias_rect.left || alias_rect.bottom <= alias_rect.top || !BindTargets(a))
      continue;
    const plume::RenderRect alias_host = ScaleRect(alias_rect, a.scale);
    cmd->clearColor(0, clear_color, &alias_host, 1);
  }
}

// Depth resolve (shaders/host/depth_resolve.hlsli): 'src' of the depth target
// to ('dest_x', 'dest_y') of the destination and of its guest words, in guest
// pixels (the destination is of the surface's scale).
bool ResolveDepth(HostSurface *surface, HostTexture *dest, const ClearRect &src, i32 dest_x,
                  i32 dest_y) {
  const RenderScale scale = surface->scale;
  auto &s = State();
  const uint32_t variant = surface->samples != plume::RenderSampleCount::COUNT_1 ? 1 : 0;
  const uint32_t source_slot = SurfaceSampleSlotLocked(surface);
  if (source_slot == kInvalidDescriptor || !dest->depth_framebuffer ||
      !s.depth_resolve_pipeline[variant]) {
    return false;
  }
  plume::RenderCommandList *cmd = s.command_list;
  static const uint32_t kProfileLabel = gpu_profile::Label("depth resolves");
  gpu_profile::Mark(s, kProfileLabel);
  TransitionSurfaceLocked(surface, plume::RenderTextureLayout::SHADER_READ);
  TransitionTextureLocked(dest, plume::RenderTextureLayout::COLOR_WRITE);
  TransitionTextureLocked(dest->words.get(), plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setFramebuffer(dest->depth_framebuffer.get());
  // Pixel coordinates are destination texels.
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dest->alloc_width),
                                          float(dest->alloc_height)));
  cmd->setScissors(ScaleRect(plume::RenderRect(dest_x, dest_y, dest_x + (src.x2 - src.x1),
                                               dest_y + (src.y2 - src.y1)),
                             surface->scale));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(s.depth_resolve_pipeline[variant].get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = push.scale[1] = 1.0f;
  push.translate[0] = float(scale.Coord(src.x1) - scale.Coord(dest_x));
  push.translate[1] = float(scale.Coord(src.y1) - scale.Coord(dest_y));
  push.texture_index = source_slot;
  push.sampler_index = kPointClampSamplerSlot;
  // D3DFMT_D24FS8 (GPU texture format 24_8_FLOAT) stores 20e4 float depth.
  push.param0 = GpuTextureFormat(surface->format_word & 0x3F) == GpuTextureFormat::Fmt24_8_Float
                    ? 1.0f
                    : 0.0f;
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  TransitionTextureLocked(dest, plume::RenderTextureLayout::SHADER_READ);

  // The draw path binds its own layout, pipeline, targets and viewport again.
  g_bound.framebuffer = nullptr;
  g_bound.pipeline = nullptr;
  g_bound.layout = false;
  g_bound.viewport_set = false;
  return true;
}

plume::RenderRect ClampRect(const ClearRect *rect, uint32_t width, uint32_t height) {
  if (!rect)
    return plume::RenderRect(0, 0, int32_t(width), int32_t(height));
  const int32_t x1 = std::clamp(rect->x1, 0, int32_t(width));
  const int32_t y1 = std::clamp(rect->y1, 0, int32_t(height));
  const int32_t x2 = std::clamp(rect->x2, x1, int32_t(width));
  const int32_t y2 = std::clamp(rect->y2, y1, int32_t(height));
  return plume::RenderRect(x1, y1, x2, y2);
}

} // namespace

void ClearStateLocked(const u8 *device_state, u32 flags, const ClearRect *rect,
                      const float color[4], float depth, u32 stencil) {
  render_profile::Scope profile(render_profile::Stage::kClear);
  auto &s = State();
  if (!Video::EnsureCommandListLocked())
    return;
  SyncBound(s);
  const GuestDevice device(device_state);
  if (!device.valid())
    return;
  Targets targets;
  if (!GatherTargets(device, targets))
    return;

  // D3D clamps clears to the viewport (CDevice viewport floats at 0x3168).
  ClearRect r = rect ? *rect : ClearRect{0, 0, int32_t(targets.width), int32_t(targets.height)};
  const float vx = device.F32(0x3168), vy = device.F32(0x316C);
  const float vw = device.F32(0x3170), vh = device.F32(0x3174);
  if (vw > 0.0f && vh > 0.0f) {
    int32_t vx2 = int32_t(vx + vw), vy2 = int32_t(vy + vh);
    // A target grown for tiling: D3D bounds the viewport by the tile-sized
    // surface, which covers the whole target across the tiles (as for the
    // scissor in ComputeViewport).
    if (targets.width > targets.guest_width && vx2 >= int32_t(targets.guest_width))
      vx2 = int32_t(targets.width);
    if (targets.height > targets.guest_height && vy2 >= int32_t(targets.guest_height))
      vy2 = int32_t(targets.height);
    r.x1 = std::max(r.x1, int32_t(vx));
    r.y1 = std::max(r.y1, int32_t(vy));
    r.x2 = std::min(r.x2, vx2);
    r.y2 = std::min(r.y2, vy2);
  }
  const plume::RenderRect clear_rect = ClampRect(&r, targets.width, targets.height);
  if (clear_rect.right <= clear_rect.left || clear_rect.bottom <= clear_rect.top)
    return;
  ClearTargets(targets, flags, clear_rect, color, depth, stencil);
}

void ResolveStateLocked(const u8 *device_state, u32 flags, const ClearRect *source_rect,
                        u32 dest_texture_va, const d3d::TextureFetchConstant *dest_format,
                        i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                        const float clear_color[4], float clear_depth, u32 clear_stencil) {
  render_profile::Scope profile(render_profile::Stage::kResolve);
  auto &s = State();
  static const uint32_t kProfileLabel = gpu_profile::Label("resolves");
  gpu_profile::Mark(s, kProfileLabel);
  // Pending memexport results stay on the GPU: a resolve writes host textures
  // only, and a clear reads no memory.
  if (!Video::EnsureCommandListLocked())
    return;
  SyncBound(s);
  const GuestDevice device(device_state);
  if (!device.valid())
    return;

  const u32 source = flags & 7;
  const u32 source_va = source == 4 ? device.U32(d3d::kDeviceOffsetDepthStencil)
                                    : device.U32(d3d::kDeviceOffsetRenderTargets + source * 4);
  HostSurface *surface = GetHostSurfaceLocked(source_va);
  plume::RenderCommandList *cmd = s.command_list;
  static uint32_t depth_logged = 0;

  // The source rect within the target and the destination level.
  auto fit = [&](const HostTexture *dest, ClearRect &src) {
    const uint32_t level_w = std::max(1u, dest->width >> dest_level);
    const uint32_t level_h = std::max(1u, dest->height >> dest_level);
    src = source_rect ? *source_rect : ClearRect{0, 0, int32_t(level_w), int32_t(level_h)};
    src.x1 = std::clamp(src.x1, 0, int32_t(surface->width));
    src.y1 = std::clamp(src.y1, 0, int32_t(surface->height));
    src.x2 = std::clamp(src.x2, src.x1, int32_t(surface->width));
    src.y2 = std::clamp(src.y2, src.y1, int32_t(surface->height));
    dest_x = std::clamp(dest_x, 0, int32_t(level_w));
    dest_y = std::clamp(dest_y, 0, int32_t(level_h));
    src.x2 = std::min(src.x2, src.x1 + int32_t(level_w) - dest_x);
    src.y2 = std::min(src.y2, src.y1 + int32_t(level_h) - dest_y);
    return src.x2 > src.x1 && src.y2 > src.y1;
  };

  if (surface && dest_texture_va) {
    if (surface->depth) {
      HostTexture *dest =
          dest_level == 0 && dest_slice == 0
              ? GetDepthResolveTextureLocked(dest_texture_va, surface->scale, dest_format)
              : nullptr;
      ClearRect src;
      if (!dest) {
        if (LogLimited(depth_logged, 2)) {
          RGPU_WARN("Depth resolve into {:08X} (level {}, slice {}) not done", dest_texture_va,
                    dest_level, dest_slice);
        }
      } else if (fit(dest, src)) {
        ResolveDepth(surface, dest, src, dest_x, dest_y);
      }
    } else if (HostTexture *dest = GetResolveTextureLocked(dest_texture_va, surface->format,
                                                           surface->scale, dest_format)) {
      ClearRect src;
      // A volume's slices are depth; stacked textures and cubes have layers.
      const bool volume = dest->dimension == TextureDimension::Tex3D && !dest->stacked;
      const uint32_t slice = volume ? 0 : dest_slice;
      const uint32_t z = volume ? dest_slice : 0;
      if (fit(dest, src)) {
        g_bound.framebuffer = nullptr;
        // The destination is of the surface's scale.
        plume::RenderRect rect =
            ScaleRect(plume::RenderRect(src.x1, src.y1, src.x2, src.y2), surface->scale);
        const uint32_t host_x = uint32_t(surface->scale.Coord(dest_x));
        const uint32_t host_y = uint32_t(surface->scale.Coord(dest_y));
        // At a fractional scale the source rect, rounded at its own edges, can
        // pass the destination's host size by a texel.
        rect.right = std::min(rect.right, rect.left + int32_t(dest->alloc_width - host_x));
        rect.bottom = std::min(rect.bottom, rect.top + int32_t(dest->alloc_height - host_y));
        const plume::RenderRect dest_rect(int32_t(host_x), int32_t(host_y),
                                          int32_t(host_x) + (rect.right - rect.left),
                                          int32_t(host_y) + (rect.bottom - rect.top));
        const bool multisampled = surface->samples != plume::RenderSampleCount::COUNT_1;
        if (multisampled && dest->extra_levels && IsVulkan()) {
          // A destination with host levels takes it through a single-level
          // target (ResolveStagingLocked): plume's Vulkan resolve writes every
          // level of its destination (D3D12's writes level 0 only, and
          // resolves in place below). Without one (device lost) nothing of
          // the resolve lands.
          if (HostTexture *staging = ResolveStagingLocked(dest)) {
            TransitionSurfaceLocked(surface, plume::RenderTextureLayout::RESOLVE_SOURCE);
            TransitionTextureLocked(staging, plume::RenderTextureLayout::RESOLVE_DEST);
            cmd->resolveTextureRegion(staging->texture.get(), host_x, host_y,
                                      surface->texture.get(), &rect);
            TransitionTextureLocked(staging, plume::RenderTextureLayout::COPY_SOURCE);
            TransitionTextureLocked(dest, plume::RenderTextureLayout::COPY_DEST);
            const plume::RenderBox box(dest_rect.left, dest_rect.top, dest_rect.right,
                                       dest_rect.bottom);
            cmd->copyTextureRegion(
                plume::RenderTextureCopyLocation::Subresource(dest->texture.get(), 0, slice),
                plume::RenderTextureCopyLocation::Subresource(staging->texture.get()), host_x,
                host_y, z, &box);
          }
        } else if (multisampled) {
          TransitionSurfaceLocked(surface, plume::RenderTextureLayout::RESOLVE_SOURCE);
          TransitionTextureLocked(dest, plume::RenderTextureLayout::RESOLVE_DEST);
          cmd->resolveTextureRegion(dest->texture.get(), host_x, host_y, surface->texture.get(),
                                    &rect);
        } else {
          TransitionSurfaceLocked(surface, plume::RenderTextureLayout::COPY_SOURCE);
          TransitionTextureLocked(dest, plume::RenderTextureLayout::COPY_DEST);
          const plume::RenderBox box(rect.left, rect.top, rect.right, rect.bottom);
          cmd->copyTextureRegion(
              plume::RenderTextureCopyLocation::Subresource(dest->texture.get(), 0, slice),
              plume::RenderTextureCopyLocation::Subresource(surface->texture.get()), host_x,
              host_y, z, &box);
        }
        TransitionTextureLocked(dest, plume::RenderTextureLayout::SHADER_READ);
        if (dest->extra_levels) {
          UpdateResolveMipsLocked(dest, dest_rect);
          // The draw path binds its own layout, pipeline and viewport again.
          g_bound.pipeline = nullptr;
          g_bound.layout = false;
          g_bound.viewport_set = false;
        }
      }
    }
  }

  // Resolve-and-clear.
  if ((flags & 0x300) && surface) {
    Targets t;
    if (surface->depth)
      t.depth = surface;
    else
      t.colors[t.color_count++] = surface;
    t.width = surface->width;
    t.height = surface->height;
    t.scale = surface->scale;
    const plume::RenderRect rect = ClampRect(source_rect, t.width, t.height);
    static const float kBlack[4] = {0, 0, 0, 0};
    const u32 clear_flags = ((flags & 0x100) ? 1u : 0u) | ((flags & 0x200) ? 0x30u : 0u);
    ClearTargets(t, clear_flags, rect, clear_color ? clear_color : kBlack, clear_depth,
                 clear_stencil);
  }
}

bool ReadRenderTargetLocked(const u8 *device_state, u32 source, u32 &width, u32 &height,
                            std::vector<u32> &words) {
  auto &s = State();
  if (!s.target_readback_pipeline || !Video::EnsureCommandListLocked())
    return false;
  SyncBound(s);
  const GuestDevice device(device_state);
  if (!device.valid() || source >= d3d::kMaxRenderTargets)
    return false;
  HostSurface *surface =
      GetHostSurfaceLocked(device.U32(d3d::kDeviceOffsetRenderTargets + source * 4));
  if (!surface || surface->depth || !surface->texture)
    return false;
  width = std::min(width, surface->width);
  height = std::min(height, surface->height);
  if (!width || !height)
    return false;
  static const uint32_t kProfileLabel = gpu_profile::Label("render target readback");
  gpu_profile::Mark(s, kProfileLabel);

  // A single-sampled copy of the rect, which the shader reads by its slot.
  const RenderScale scale = surface->scale;
  const plume::RenderRect rect(0, 0, int32_t(scale.Size(width)), int32_t(scale.Size(height)));
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
      uint32_t(rect.right), uint32_t(rect.bottom), 1, surface->format);
  desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  std::unique_ptr<plume::RenderTexture> copy = s.device->createTexture(desc);
  if (!TextureCreated(copy.get()))
    return false;
  std::unique_ptr<plume::RenderTextureView> view =
      copy->createTextureView(plume::RenderTextureViewDesc::Texture2D(surface->format));
  const uint64_t bytes = uint64_t(width) * height * 4;
  std::unique_ptr<plume::RenderBuffer> output = s.device->createBuffer(
      plume::RenderBufferDesc::DefaultBuffer(bytes, StorageBufferFlags()));
  std::unique_ptr<plume::RenderBuffer> readback =
      s.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes));
  std::unique_ptr<plume::RenderDescriptorSet> set = detail::CreateTargetReadbackSet(s);
  const uint32_t slot = view ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
  if (!view || !output || !readback || !set || slot == kInvalidDescriptor)
    return false;
  Video::SetTextureSlotLocked(slot, copy.get(), view.get());
  set->setBuffer(0, output.get());

  plume::RenderCommandList *cmd = s.command_list;
  g_bound.framebuffer = nullptr;
  if (surface->samples != plume::RenderSampleCount::COUNT_1) {
    TransitionSurfaceLocked(surface, plume::RenderTextureLayout::RESOLVE_SOURCE);
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(copy.get(), plume::RenderTextureLayout::RESOLVE_DEST));
    cmd->resolveTextureRegion(copy.get(), 0, 0, surface->texture.get(), &rect);
  } else {
    TransitionSurfaceLocked(surface, plume::RenderTextureLayout::COPY_SOURCE);
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(copy.get(), plume::RenderTextureLayout::COPY_DEST));
    const plume::RenderBox box(rect.left, rect.top, rect.right, rect.bottom);
    cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(copy.get()),
                           plume::RenderTextureCopyLocation::Subresource(surface->texture.get()),
                           0, 0, 0, &box);
  }
  const plume::RenderTextureBarrier read(copy.get(), plume::RenderTextureLayout::SHADER_READ);
  const plume::RenderBufferBarrier write(output.get(), plume::RenderBufferAccess::WRITE);
  cmd->barriers(plume::RenderBarrierStage::COMPUTE, &write, 1, &read, 1);
  cmd->setComputePipelineLayout(s.target_readback_layout.get());
  cmd->setPipeline(s.target_readback_pipeline.get());
  cmd->setComputeDescriptorSet(s.texture_set.get(), 0);
  cmd->setComputeDescriptorSet(set.get(), 1);
  TargetReadbackPush push{};
  push.texture_index = slot;
  push.width = width;
  push.height = height;
  push.scale_numerator = RenderScale::kNumerators[scale.code];
  push.scale_denominator = RenderScale::kDenominators[scale.code];
  cmd->setComputePushConstants(0, &push, 0, sizeof(push));
  cmd->dispatch((width + 7) / 8, (height + 7) / 8, 1);
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderBufferBarrier(output.get(), plume::RenderBufferAccess::READ));
  cmd->copyBufferRegion(plume::RenderBufferReference(readback.get()),
                        plume::RenderBufferReference(output.get()), bytes);
  // The compute pipeline replaced the graphics one (D3D12), and the barriers
  // ended any render pass.
  g_bound.pipeline = nullptr;
  g_bound.layout = false;

  detail::SubmitAndWaitLocked(s);
  bool read_back = false;
  if (const auto *data = static_cast<const u32 *>(readback->map())) {
    words.assign(data, data + size_t(width) * height);
    readback->unmap();
    read_back = true;
  }
  Video::FreeTextureSlotLocked(slot);
  Video::RetireLocked(std::move(view));
  Video::RetireLocked(std::move(copy));
  Video::RetireLocked(std::move(set));
  Video::RetireLocked(std::move(output));
  Video::RetireLocked(std::move(readback));
  return read_back;
}

PresentImage FrontBufferImageLocked(u32 texture_va) {
  PresentImage image;
  HostTexture *texture = FindHostTextureLocked(texture_va);
  if (!texture || !texture->texture || texture->slot == UINT32_MAX)
    return image;
  image.texture = texture->texture.get();
  image.layout = &texture->layout;
  image.descriptor = texture->slot;
  image.width = texture->alloc_width;
  image.height = texture->alloc_height;
  image.source_width = texture->scale.Size(texture->width);
  image.source_height = texture->scale.Size(texture->height);
  return image;
}

} // namespace renut::gpu
