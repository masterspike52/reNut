/**
 * @file    gpu/guest/command_buffers.cpp
 */
#include "renut_engine/gpu/guest/command_buffers.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"

namespace renut::gpu {

namespace {

constexpr size_t kNone = SIZE_MAX;
// RunCommandBuffer inside a recorded buffer, and so on.
constexpr u32 kMaxRunDepth = 8;

constexpr u32 kVertexShaderF =
    u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, VertexShaderF));
constexpr u32 kPixelShaderF =
    u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, PixelShaderF));
// One m_Pending bit per 4 float constants, most significant bit first
// (SetPending_AluConstants 0x8222D0E8).
constexpr u32 kConstantGroupBytes = 4 * 16;

// What an op keeps of the device (see Op::state). The draw, clear and resolve
// paths read nothing before the fetch constants (dirty masks, ring pointers,
// the Set/Get*State call tables), and the float constants, two thirds of the
// snapshot, are kept as the groups written since the device's previous op.
constexpr u32 kFetchBegin = u32(offsetof(d3d::Device, m_Constants));
constexpr u32 kFetchBytes = kVertexShaderF - kFetchBegin;
constexpr u32 kFloatBytes = 2 * 256 * 16;
constexpr u32 kTailBegin = kVertexShaderF + kFloatBytes;
constexpr u32 kTailBytes = d3d::kDeviceSnapshotSize - kTailBegin;
static_assert(kPixelShaderF == kVertexShaderF + 256 * 16);
static_assert(kTailBegin == u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, VertexShaderB)));

// The rest, the fetch constants and the tail, in 64-byte chunks: an op keeps
// the chunks that differ from the device's previous op (Op::plain_dirty, bit
// i = chunk i), which for most draws are a few bindings and render states.
// Replaying them was a copy of the whole of both per draw on the render
// thread; comparing them is the recording threads' work.
constexpr u32 kChunkBytes = 64;
constexpr u32 kFetchChunks = kFetchBytes / kChunkBytes;
constexpr u32 kTailChunks = (kTailBytes + kChunkBytes - 1) / kChunkBytes;
constexpr u32 kPlainChunks = kFetchChunks + kTailChunks;
static_assert(kFetchBytes % kChunkBytes == 0 && kPlainChunks <= 64);
constexpr uint64_t kAllChunks = kPlainChunks == 64 ? ~uint64_t(0) : (uint64_t(1) << kPlainChunks) - 1;

// Chunk i's offset in the device, and its size (the tail's last is short).
constexpr u32 ChunkOffset(u32 i) {
  return i < kFetchChunks ? kFetchBegin + i * kChunkBytes
                          : kTailBegin + (i - kFetchChunks) * kChunkBytes;
}
constexpr u32 ChunkSize(u32 i) {
  return i < kFetchChunks ? kChunkBytes
                          : std::min(kChunkBytes, kTailBytes - (i - kFetchChunks) * kChunkBytes);
}

// Calls f(first, count) for each run of consecutive float constant groups in
// an m_Pending mask (bit 63 - group).
template <typename F>
void ForEachGroupRun(uint64_t mask, F f) {
  while (mask) {
    const u32 first = u32(std::countl_zero(mask));
    const u32 count = u32(std::countl_one(mask << first));
    f(first, count);
    mask = first + count >= 64 ? 0 : mask & (~uint64_t(0) >> (first + count));
  }
}

enum class OpKind { Draw, Clear, Resolve, Run };

struct Op {
  OpKind kind = OpKind::Draw;
  // The device in the arena (CopyState): the chunks in plain_dirty, then the
  // float constant groups in vs_dirty and ps_dirty. Replay rebuilds the whole
  // snapshot from the previous op's (ApplyState).
  size_t state = kNone;
  // Chunks that differ from the device's previous op; all for a buffer's first.
  uint64_t plain_dirty = kAllChunks;
  // Float constant groups the buffer had set by this op (m_Pending[0] / [1]
  // bits); the rest are inherited from the running device.
  uint64_t vs_set = 0, ps_set = 0;
  // Those written since the device's previous op (ReplayConstants::vs_dirty),
  // all for a buffer's first op. Only the pending-mask consumers (the draw
  // calls, RunCommandBuffer and EndCommandBuffer, all but the last hooked)
  // clear them, so the others hold what the previous op of the buffer had.
  uint64_t vs_dirty = ~uint64_t(0), ps_dirty = ~uint64_t(0);

  // Draw
  DrawCall call;
  size_t up_vertices = kNone, up_indices = kNone;  // arena offsets

  // Clear / Resolve
  u32 flags = 0;
  bool has_rect = false;
  ClearRect rect{};
  bool has_color = false;
  float color[4] = {};
  float depth = 0.0f;
  u32 stencil = 0;
  u32 dest_texture = 0;
  // The destination header when recorded; the game often builds it on the
  // recording thread's stack.
  bool has_dest_format = false;
  d3d::TextureFetchConstant dest_format{};
  i32 dest_x = 0, dest_y = 0;
  u32 dest_level = 0, dest_slice = 0;

  // Run
  u32 command_buffer = 0;
};

struct HostCommandBuffer {
  std::vector<Op> ops;
  std::vector<u8> arena;
};

// Vertex data of a recorded Begin*Vertices draw, still being written.
struct PendingVertices {
  u32 device = 0;
  u32 command_buffer = 0;
  size_t offset = 0;
  const u8 *source = nullptr;
  u32 size = 0;
};

// What NewOp keeps per recording device.
struct DeviceRecording {
  // Float constant groups the buffer has set (Op::vs_set / ps_set).
  uint64_t vs = 0, ps = 0;
  // The device as its last op saw it, in the device layout; only the chunks
  // (ChunkOffset) are kept.
  u8 shadow[d3d::kDeviceSnapshotSize];
};

// A RunCommandBuffer's buffer and every buffer it runs in turn, as they were
// when it was issued: the render thread replays it later, by when the game
// may be recording the next frame's into the same D3DCommandBuffers.
using BufferSet = std::unordered_map<u32, std::shared_ptr<HostCommandBuffer>>;

// Guards the three below; never taken with State().mutex (the render thread
// replays captured buffers without it). A buffer a queued run still holds is
// replaced, not cleared, when the game records it again (BeginRecording).
std::mutex g_record_mutex;
std::unordered_map<u32, std::shared_ptr<HostCommandBuffer>> g_buffers;

// Buffers no recording or queued run holds any more, kept with their capacity:
// a buffer recorded again is a fresh one while a run holds the old, and
// growing a fresh arena to a frame's worth of snapshots cost the recording
// threads more than recording. Their own lock: the last reference may drop
// with g_record_mutex held (BeginRecording) or on the render thread.
// Bounded by count and by the capacity they keep: one frame's worth of
// recordings is what is worth keeping, and an arena one unusual frame grew
// past kMaxKeptArena goes rather than holding that memory for good.
constexpr size_t kMaxFreeBuffers = 256;
constexpr size_t kMaxFreeBytes = 64u << 20;
constexpr size_t kMaxKeptArena = 8u << 20;
std::mutex g_free_mutex;
std::vector<std::unique_ptr<HostCommandBuffer>> g_free_buffers;
size_t g_free_bytes = 0;  // arena and op capacity of g_free_buffers
std::atomic<size_t> g_live_buffers{0};

size_t CapacityBytes(const HostCommandBuffer &buffer) {
  return buffer.arena.capacity() + buffer.ops.capacity() * sizeof(buffer.ops[0]);
}

std::shared_ptr<HostCommandBuffer> NewBuffer() {
  std::unique_ptr<HostCommandBuffer> buffer;
  {
    std::lock_guard lock(g_free_mutex);
    if (!g_free_buffers.empty()) {
      buffer = std::move(g_free_buffers.back());
      g_free_buffers.pop_back();
      g_free_bytes -= CapacityBytes(*buffer);
    }
  }
  g_live_buffers.fetch_add(1, std::memory_order_relaxed);
  if (buffer) {
    buffer->ops.clear();
    buffer->arena.clear();
  } else {
    buffer = std::make_unique<HostCommandBuffer>();
  }
  return std::shared_ptr<HostCommandBuffer>(buffer.release(), [](HostCommandBuffer *retired) {
    g_live_buffers.fetch_sub(1, std::memory_order_relaxed);
    const size_t bytes = CapacityBytes(*retired);
    {
      std::lock_guard lock(g_free_mutex);
      if (g_free_buffers.size() < kMaxFreeBuffers && retired->arena.capacity() <= kMaxKeptArena &&
          g_free_bytes + bytes <= kMaxFreeBytes) {
        g_free_buffers.emplace_back(retired);
        g_free_bytes += bytes;
        return;
      }
    }
    delete retired;
  });
}
std::unordered_map<u32, DeviceRecording> g_recording_devices;
std::vector<PendingVertices> g_pending_vertices;

u32 RecordingBuffer(u32 device_va) {
  const auto *word = GuestPtr<const be_u32>(device_va + d3d::kDeviceOffsetCommandBuffer);
  return word ? u32(*word) : 0u;
}

size_t Reserve(HostCommandBuffer &buffer, size_t size) {
  const size_t offset = (buffer.arena.size() + 15) & ~size_t(15);
  buffer.arena.resize(offset + size);
  return offset;
}

// Appends rather than going through Reserve: resize would zero the bytes the
// copy then overwrites (a device snapshot per op).
size_t CopyIn(HostCommandBuffer &buffer, const u8 *data, size_t size) {
  buffer.arena.resize((buffer.arena.size() + 15) & ~size_t(15));
  const size_t offset = buffer.arena.size();
  buffer.arena.insert(buffer.arena.end(), data, data + size);
  return offset;
}

// Whether two whole chunks differ: fixed-size loads the compiler keeps inline
// (and vectorizes), where a memcmp call per chunk cost more than the bytes.
bool ChunkDiffers(const u8 *a, const u8 *b) {
  uint64_t diff = 0;
  for (u32 k = 0; k < kChunkBytes; k += 8) {
    uint64_t x, y;
    std::memcpy(&x, a + k, 8);
    std::memcpy(&y, b + k, 8);
    diff |= x ^ y;
  }
  return diff != 0;
}

// The chunks of 'device' that differ from 'shadow', which takes them.
uint64_t ChangedChunks(const u8 *device, u8 *shadow) {
  uint64_t changed = 0;
  for (u32 i = 0; i < kPlainChunks; ++i) {
    const u32 at = ChunkOffset(i), size = ChunkSize(i);
    const bool differs = size == kChunkBytes ? ChunkDiffers(device + at, shadow + at)
                                             : std::memcmp(device + at, shadow + at, size) != 0;
    if (differs) {
      std::memcpy(shadow + at, device + at, size);
      changed |= uint64_t(1) << i;
    }
  }
  return changed;
}

// An op's state (Op::state) from the device.
size_t CopyState(HostCommandBuffer &buffer, const u8 *device, uint64_t plain_dirty,
                 uint64_t vs_dirty, uint64_t ps_dirty) {
  auto &arena = buffer.arena;
  arena.resize((arena.size() + 15) & ~size_t(15));
  const size_t offset = arena.size();
  for (uint64_t chunks = plain_dirty; chunks; chunks &= chunks - 1) {
    const u32 i = u32(std::countr_zero(chunks));
    const u8 *from = device + ChunkOffset(i);
    arena.insert(arena.end(), from, from + ChunkSize(i));
  }
  for (const auto [dirty, base] : {std::pair{vs_dirty, kVertexShaderF}, std::pair{ps_dirty, kPixelShaderF}}) {
    ForEachGroupRun(dirty, [&](u32 first, u32 count) {
      const u8 *from = device + base + first * kConstantGroupBytes;
      arena.insert(arena.end(), from, from + count * kConstantGroupBytes);
    });
  }
  return offset;
}

// Brings 'state', the previous op's device (or anything, before a buffer's
// first op), to the op's.
void ApplyState(u8 *state, const u8 *from, uint64_t plain_dirty, uint64_t vs_dirty,
                uint64_t ps_dirty) {
  for (uint64_t chunks = plain_dirty; chunks; chunks &= chunks - 1) {
    const u32 i = u32(std::countr_zero(chunks));
    const u32 size = ChunkSize(i);
    // Whole chunks as a fixed-size copy, which stays inline.
    if (size == kChunkBytes)
      std::memcpy(state + ChunkOffset(i), from, kChunkBytes);
    else
      std::memcpy(state + ChunkOffset(i), from, size);
    from += size;
  }
  for (const auto [dirty, base] : {std::pair{vs_dirty, kVertexShaderF}, std::pair{ps_dirty, kPixelShaderF}}) {
    ForEachGroupRun(dirty, [&](u32 first, u32 count) {
      const u32 bytes = count * kConstantGroupBytes;
      std::memcpy(state + base + first * kConstantGroupBytes, from, bytes);
      from += bytes;
    });
  }
}

void FlushDevicePending(u32 device_va);

// A new op on the device's buffer, with the device's state and the constant
// groups set so far, after the device's pending Begin*Vertices data. Consumes
// the device's dirty masks the way the original calls do when they write the
// state into the buffer. g_record_mutex is held for the lookups only: N&B
// records on several threads at once, and the snapshot copy, most of the
// work, goes into the device's own buffer, which only its recording thread
// touches until the buffer runs.
Op *NewOp(u32 device_va, OpKind kind, HostCommandBuffer **out_buffer) {
  const u32 command_buffer = RecordingBuffer(device_va);
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!command_buffer || !device)
    return nullptr;
  HostCommandBuffer *found = nullptr;
  DeviceRecording *found_set = nullptr;
  {
    std::lock_guard lock(g_record_mutex);
    FlushDevicePending(device_va);
    auto &slot = g_buffers[command_buffer];
    if (!slot)
      slot = NewBuffer();
    found = slot.get();
    found_set = &g_recording_devices[device_va];
  }
  HostCommandBuffer &buffer = *found;
  DeviceRecording &set = *found_set;
  const uint64_t vs_dirty = u64(device->m_Pending[0]);
  const uint64_t ps_dirty = u64(device->m_Pending[1]);
  set.vs |= vs_dirty;
  set.ps |= ps_dirty;
  for (auto &mask : device->m_Pending)
    mask = 0;

  Op &op = buffer.ops.emplace_back();
  op.kind = kind;
  op.vs_set = set.vs;
  op.ps_set = set.ps;
  // The first op of a buffer follows ops of other buffers: nothing known.
  const bool first = buffer.ops.size() == 1;
  const auto *bytes = reinterpret_cast<const u8 *>(device);
  const uint64_t changed = ChangedChunks(bytes, set.shadow);
  op.plain_dirty = first ? kAllChunks : changed;
  op.vs_dirty = first ? ~uint64_t(0) : vs_dirty;
  op.ps_dirty = first ? ~uint64_t(0) : ps_dirty;
  op.state = CopyState(buffer, bytes, op.plain_dirty, op.vs_dirty, op.ps_dirty);
  *out_buffer = &buffer;
  return &op;
}

void CopyPending(const PendingVertices &p) {
  auto it = g_buffers.find(p.command_buffer);
  if (it == g_buffers.end() || !it->second || p.offset + p.size > it->second->arena.size())
    return;
  std::memcpy(it->second->arena.data() + p.offset, p.source, p.size);
}

// Copies pending vertex data selected by 'match' into its buffer. Requires
// g_record_mutex.
template <typename Match>
void FlushPending(Match match) {
  for (size_t i = 0; i < g_pending_vertices.size();) {
    if (match(g_pending_vertices[i])) {
      CopyPending(g_pending_vertices[i]);
      g_pending_vertices[i] = g_pending_vertices.back();
      g_pending_vertices.pop_back();
    } else {
      ++i;
    }
  }
}

void FlushDevicePending(u32 device_va) {
  if (g_pending_vertices.empty())
    return;
  FlushPending([device_va](const PendingVertices &p) { return p.device == device_va; });
}

void MergeInherited(u8 *state, const u8 *running, uint64_t set, u32 offset) {
  if (set == ~uint64_t(0))
    return;
  for (u32 group = 0; group < 64; ++group) {
    if (!(set & (uint64_t(1) << (63 - group)))) {
      const u32 at = offset + group * kConstantGroupBytes;
      std::memcpy(state + at, running + at, kConstantGroupBytes);
    }
  }
}

// The tile predication mask of a device or snapshot; ~0 when none is set.
u32 Predication(const u8 *state) {
  const u32 mask = *reinterpret_cast<const be_u32 *>(state + d3d::kDeviceOffsetPredication);
  return mask ? mask : ~0u;
}

// The device each Replay depth rebuilds its ops' states in (ApplyState); only
// the render thread replays. The bytes before the fetch constants stay zero.
alignas(64) u8 g_replay_state[kMaxRunDepth][d3d::kDeviceSnapshotSize];

// On the render thread. 'predication': the mask the buffer runs under
// (SetPredication before RunCommandBuffer), which the draws' own masks narrow
// (see SetReplayPredicationLocked).
void Replay(const u8 *running, const BufferSet &buffers, u32 command_buffer_va, u32 depth,
            u32 predication) {
  if (depth >= kMaxRunDepth)
    return;
  auto it = buffers.find(command_buffer_va);
  if (it == buffers.end() || !it->second)
    return;
  // Only this thread touches a captured buffer: the game records into a new
  // one (BeginRecording).
  HostCommandBuffer &buffer = *it->second;
  static uint64_t replays = 0;
  const uint64_t replay = ++replays;
  uint64_t vs_dirty = ~uint64_t(0), ps_dirty = ~uint64_t(0);
  u8 *state = g_replay_state[depth];
  for (const Op &op : buffer.ops) {
    ApplyState(state, buffer.arena.data() + op.state, op.plain_dirty, op.vs_dirty, op.ps_dirty);
    vs_dirty |= op.vs_dirty;
    ps_dirty |= op.ps_dirty;
    switch (op.kind) {
    case OpKind::Draw: {
      DrawCall call = op.call;
      if (op.up_vertices != kNone)
        call.up_vertices = buffer.arena.data() + op.up_vertices;
      if (op.up_indices != kNone)
        call.up_indices = buffer.arena.data() + op.up_indices;
      // The draw reads its inherited constants from the running device.
      const ReplayConstants constants{running, op.vs_set, op.ps_set, &vs_dirty, &ps_dirty, replay};
      const u32 previous = SetReplayPredicationLocked(predication);
      DrawStateLocked(state, call, &constants);
      SetReplayPredicationLocked(previous);
      break;
    }
    case OpKind::Clear:
      ClearStateLocked(state, op.flags, op.has_rect ? &op.rect : nullptr, op.color, op.depth,
                       op.stencil);
      break;
    case OpKind::Resolve:
      ResolveStateLocked(state, op.flags, op.has_rect ? &op.rect : nullptr, op.dest_texture,
                         op.has_dest_format ? &op.dest_format : nullptr, op.dest_x, op.dest_y,
                         op.dest_level, op.dest_slice, op.has_color ? op.color : nullptr,
                         op.depth, op.stencil);
      break;
    case OpKind::Run:
      // The nested buffer runs on this op's state: complete its constants.
      // Idempotent: the inherited groups are rewritten on every run.
      MergeInherited(state, running, op.vs_set, kVertexShaderF);
      MergeInherited(state, running, op.ps_set, kPixelShaderF);
      Replay(state, buffers, op.command_buffer, depth + 1, predication & Predication(state));
      break;
    }
  }
}

// The buffer and those it runs, nested up to kMaxRunDepth, with their pending
// Begin*Vertices data copied in. Requires g_record_mutex.
void CaptureBuffers(u32 command_buffer_va, u32 depth, BufferSet &buffers) {
  if (depth >= kMaxRunDepth || buffers.contains(command_buffer_va))
    return;
  FlushPending([command_buffer_va](const PendingVertices &p) {
    return p.command_buffer == command_buffer_va;
  });
  auto it = g_buffers.find(command_buffer_va);
  if (it == g_buffers.end() || !it->second)
    return;
  buffers.emplace(command_buffer_va, it->second);
  for (const Op &op : it->second->ops) {
    if (op.kind == OpKind::Run)
      CaptureBuffers(op.command_buffer, depth + 1, buffers);
  }
}

} // namespace

bool IsRecordingCommandBuffer(u32 device_va) {
  return RecordingBuffer(device_va) != 0;
}

void BeginRecording(u32 device_va, u32 command_buffer_va) {
  std::lock_guard lock(g_record_mutex);
  FlushPending([command_buffer_va](const PendingVertices &p) {
    return p.command_buffer == command_buffer_va;
  });
  // A queued run may still hold the old recording.
  auto &slot = g_buffers[command_buffer_va];
  if (!slot || slot.use_count() > 1) {
    slot = NewBuffer();
  } else {
    slot->ops.clear();
    slot->arena.clear();
  }
  // BeginCommandBuffer marks the state the buffer does not inherit dirty.
  // (The shadow stays: a buffer's first op keeps every chunk anyway.)
  DeviceRecording &set = g_recording_devices[device_va];
  set.vs = 0;
  set.ps = 0;
  if (const auto *device = GuestPtr<const d3d::Device>(device_va)) {
    set.vs = u64(device->m_Pending[0]);
    set.ps = u64(device->m_Pending[1]);
  }
}

void RecordDraw(u32 device_va, const DrawCall &call, u32 up_vertex_bytes, u32 up_index_bytes) {
  HostCommandBuffer *buffer = nullptr;
  Op *op = NewOp(device_va, OpKind::Draw, &buffer);
  if (!op)
    return;
  op->call = call;
  op->call.up_vertices = nullptr;
  op->call.up_indices = nullptr;
  if (call.up_vertices)
    op->up_vertices = CopyIn(*buffer, call.up_vertices, up_vertex_bytes);
  if (call.up_indices)
    op->up_indices = CopyIn(*buffer, call.up_indices, up_index_bytes);
}

void RecordDeferredDraw(u32 device_va, const DrawCall &call, const u8 *scratch, u32 bytes) {
  HostCommandBuffer *buffer = nullptr;
  Op *op = NewOp(device_va, OpKind::Draw, &buffer);
  if (!op)
    return;
  op->call = call;
  op->call.up_vertices = nullptr;
  op->up_vertices = Reserve(*buffer, bytes);
  std::lock_guard lock(g_record_mutex);
  g_pending_vertices.push_back(
      {device_va, RecordingBuffer(device_va), op->up_vertices, scratch, bytes});
}

void RecordClear(u32 device_va, u32 flags, const ClearRect *rect, const float color[4],
                 float depth, u32 stencil) {
  HostCommandBuffer *buffer = nullptr;
  Op *op = NewOp(device_va, OpKind::Clear, &buffer);
  if (!op)
    return;
  op->flags = flags;
  op->has_rect = rect != nullptr;
  if (rect)
    op->rect = *rect;
  std::memcpy(op->color, color, sizeof(op->color));
  op->depth = depth;
  op->stencil = stencil;
}

void RecordResolve(u32 device_va, u32 flags, const ClearRect *source_rect, u32 dest_texture_va,
                   i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                   const float clear_color[4], float clear_depth, u32 clear_stencil) {
  HostCommandBuffer *buffer = nullptr;
  Op *op = NewOp(device_va, OpKind::Resolve, &buffer);
  if (!op)
    return;
  op->flags = flags;
  op->has_rect = source_rect != nullptr;
  if (source_rect)
    op->rect = *source_rect;
  op->has_color = clear_color != nullptr;
  if (clear_color)
    std::memcpy(op->color, clear_color, sizeof(op->color));
  op->depth = clear_depth;
  op->stencil = clear_stencil;
  op->dest_texture = dest_texture_va;
  if (const auto *header = GuestPtr<const d3d::BaseTexture>(dest_texture_va)) {
    op->has_dest_format = true;
    op->dest_format = header->Format;
  }
  op->dest_x = dest_x;
  op->dest_y = dest_y;
  op->dest_level = dest_level;
  op->dest_slice = dest_slice;
}

void RecordRun(u32 device_va, u32 command_buffer_va) {
  HostCommandBuffer *buffer = nullptr;
  if (Op *op = NewOp(device_va, OpKind::Run, &buffer))
    op->command_buffer = command_buffer_va;
}

void FlushRecordedVertices(u32 device_va) {
  std::lock_guard lock(g_record_mutex);
  FlushDevicePending(device_va);
}

void QueueRunCommandBuffer(u32 device_va, u32 command_buffer_va) {
  FlushDeferredVertices(device_va);
  const u8 *device = GuestPtr<const u8>(device_va);
  if (!device)
    return;
  struct Run {
    std::vector<u8> running;  // the running device, whose constants the buffer inherits
    BufferSet buffers;
  };
  auto run = std::make_shared<Run>();
  run->running.assign(device, device + d3d::kDeviceSnapshotSize);
  {
    std::lock_guard lock(g_record_mutex);
    CaptureBuffers(command_buffer_va, 0, run->buffers);
  }
  render_thread::Queue([run, command_buffer_va] {
    const u8 *running = run->running.data();
    Replay(running, run->buffers, command_buffer_va, 0, Predication(running));
  });
}

void ReleaseCommandBuffer(u32 command_buffer_va) {
  std::lock_guard lock(g_record_mutex);
  if (g_buffers.erase(command_buffer_va)) {
    for (size_t i = 0; i < g_pending_vertices.size();) {
      if (g_pending_vertices[i].command_buffer == command_buffer_va) {
        g_pending_vertices[i] = g_pending_vertices.back();
        g_pending_vertices.pop_back();
      } else {
        ++i;
      }
    }
  }
}

CommandBufferMemory CommandBufferMemoryUsage() {
  CommandBufferMemory usage;
  {
    std::lock_guard lock(g_record_mutex);
    for (const auto &[va, buffer] : g_buffers) {
      if (buffer) {
        usage.recorded_bytes += CapacityBytes(*buffer);
        ++usage.recorded;
      }
    }
  }
  {
    std::lock_guard lock(g_free_mutex);
    usage.spare_bytes = g_free_bytes;
    usage.spare = g_free_buffers.size();
  }
  usage.live = g_live_buffers.load(std::memory_order_relaxed);
  return usage;
}

} // namespace renut::gpu
