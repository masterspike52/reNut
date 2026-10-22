/**
 * @file    gpu/backend/command_recorder.cpp
 */
#include "renut_engine/gpu/backend/command_recorder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/log.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

REXCVAR_DEFINE_BOOL(renut_threaded_recording, true, "Nuts&Bolts/Graphics",
                    "Make the host API's calls (the driver's share of the renderer's work) on "
                    "a thread of their own, beside the render thread. Takes effect at the next "
                    "start.");

namespace renut::gpu {

bool ThreadedRecordingEnabled() {
  return REXCVAR_GET(renut_threaded_recording);
}

struct CommandRecorder::Chunk {
  CommandRecorder *owner = nullptr;
  std::unique_ptr<uint8_t[]> data;
  size_t capacity = 0;
  size_t used = 0;
};

namespace {

using Chunk = CommandRecorder::Chunk;
using Clock = std::chrono::steady_clock;

enum Op : uint32_t {
  kBegin,
  kEnd,
  kBarriers,
  kDispatch,
  kDrawInstanced,
  kDrawIndexedInstanced,
  kSetPipeline,
  kSetComputePipelineLayout,
  kSetComputePushConstants,
  kSetComputeDescriptorSet,
  kSetGraphicsPipelineLayout,
  kSetGraphicsPushConstants,
  kSetGraphicsDescriptorSet,
  kSetGraphicsRootDescriptor,
  kSetIndexBuffer,
  kSetVertexBuffers,
  kSetViewports,
  kSetScissors,
  kSetFramebuffer,
  kSetDepthBias,
  kClearColor,
  kClearDepthStencil,
  kCopyBufferRegion,
  kCopyTextureRegion,
  kCopyBuffer,
  kCopyTexture,
  kResolveTexture,
  kResolveTextureRegion,
  kDiscardTexture,
  kResetQueryPool,
  kWriteTimestamp,
  kBlendConstant,
  kStencilReference,
  kCall,  // RecordCall: a heap std::function, run and deleted
};

// The recording thread ran a RecordCall call (defined with the Worker).
void CallDone();

// Recording thread: time in RecordCall calls (a present can hold for vsync),
// left out of its busy time.
Clock::duration g_call_time{};

// Each op is a header and its payload, every piece of which starts at a
// multiple of 8 bytes (the pieces are plume's plain structs, read in place).
struct Header {
  uint32_t op;
  uint32_t size;  // payload bytes after the header
};
static_assert(sizeof(Header) == 8);

constexpr size_t Aligned(size_t bytes) {
  return (bytes + 7) & ~size_t(7);
}
template <typename T>
constexpr size_t SizeOf(size_t count = 1) {
  static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= 8);
  return Aligned(sizeof(T) * count);
}

class Writer {
public:
  explicit Writer(uint8_t *at) : at_(at) {}
  template <typename T>
  void Value(const T &value) {
    std::memcpy(at_, &value, sizeof(T));
    at_ += SizeOf<T>();
  }
  template <typename T>
  void Array(const T *values, uint32_t count) {
    if (count)
      std::memcpy(at_, values, sizeof(T) * count);
    at_ += SizeOf<T>(count);
  }
  void Bytes(const void *data, uint32_t size) {
    std::memcpy(at_, data, size);
    at_ += Aligned(size);
  }

private:
  uint8_t *at_;
};

class Reader {
public:
  explicit Reader(const uint8_t *at) : at_(at) {}
  template <typename T>
  T Value() {
    T value;
    std::memcpy(&value, at_, sizeof(T));
    at_ += SizeOf<T>();
    return value;
  }
  template <typename T>
  const T *Array(uint32_t count) {
    const T *values = count ? reinterpret_cast<const T *>(at_) : nullptr;
    at_ += SizeOf<T>(count);
    return values;
  }
  const void *Bytes(uint32_t size) {
    const void *data = at_;
    at_ += Aligned(size);
    return data;
  }

private:
  const uint8_t *at_;
};

struct BarriersOp {
  plume::RenderBarrierStages stages;
  uint32_t buffers;
  uint32_t textures;
};
struct DispatchOp {
  uint32_t x, y, z;
};
struct DrawOp {
  uint32_t vertices, instances, first_vertex, first_instance;
};
struct DrawIndexedOp {
  uint32_t indices, instances, first_index;
  int32_t base_vertex;
  uint32_t first_instance;
};
struct PushConstantsOp {
  uint32_t range, offset, size;
};
struct DescriptorSetOp {
  plume::RenderDescriptorSet *set;
  uint32_t index;
};
struct RootDescriptorOp {
  plume::RenderBufferReference buffer;
  uint32_t index;
};
struct IndexBufferOp {
  plume::RenderIndexBufferView view;
  bool has;
};
struct VertexBuffersOp {
  uint32_t first, count;
  bool slots;
};
struct CountOp {
  uint32_t count;
};
struct DepthBiasOp {
  float bias, clamp, slope;
};
struct ClearColorOp {
  plume::RenderColor color;
  uint32_t attachment, rects;
};
struct ClearDepthStencilOp {
  float depth;
  uint32_t stencil, rects;
  bool clear_depth, clear_stencil;
};
struct CopyBufferRegionOp {
  plume::RenderBufferReference dst, src;
  uint64_t size;
};
struct CopyTextureRegionOp {
  plume::RenderTextureCopyLocation dst, src;
  plume::RenderBox box;
  uint32_t x, y, z;
  bool has_box;
};
struct PairOp {
  const void *dst, *src;
};
struct ResolveRegionOp {
  const plume::RenderTexture *dst, *src;
  plume::RenderRect rect;
  uint32_t x, y;
  plume::RenderResolveMode mode;
  bool has_rect;
};
struct QueryPoolOp {
  const plume::RenderQueryPool *pool;
  uint32_t first, count;
};

// Chunks are handed over once this full (so the thread starts on the frame's
// calls early: about every 80 draws), and are this big.
constexpr size_t kPublishBytes = 32 * 1024;
constexpr size_t kChunkBytes = 64 * 1024;
// How many pauses (a few microseconds) the thread looks for more work before
// it sleeps: a wake-up costs the render thread a system call, but spinning
// for longer (30 us, timed by the clock) was a seventh of the thread's time,
// taken from the game's threads on an 8-thread CPU (xperf, 2026-10-22).
constexpr uint32_t kSpinPauses = 256;

void Replay(plume::RenderCommandList *t, const uint8_t *at, const uint8_t *end) {
  while (at < end) {
    Header header;
    std::memcpy(&header, at, sizeof(header));
    Reader r(at + sizeof(header));
    at += sizeof(header) + header.size;
    switch (Op(header.op)) {
    case kBegin:
      t->begin();
      break;
    case kEnd:
      t->end();
      break;
    case kBarriers: {
      const auto op = r.Value<BarriersOp>();
      const auto *buffers = r.Array<plume::RenderBufferBarrier>(op.buffers);
      const auto *textures = r.Array<plume::RenderTextureBarrier>(op.textures);
      t->barriers(op.stages, buffers, op.buffers, textures, op.textures);
      break;
    }
    case kDispatch: {
      const auto op = r.Value<DispatchOp>();
      t->dispatch(op.x, op.y, op.z);
      break;
    }
    case kDrawInstanced: {
      const auto op = r.Value<DrawOp>();
      t->drawInstanced(op.vertices, op.instances, op.first_vertex, op.first_instance);
      break;
    }
    case kDrawIndexedInstanced: {
      const auto op = r.Value<DrawIndexedOp>();
      t->drawIndexedInstanced(op.indices, op.instances, op.first_index, op.base_vertex,
                              op.first_instance);
      break;
    }
    case kSetPipeline:
      t->setPipeline(r.Value<const plume::RenderPipeline *>());
      break;
    case kSetComputePipelineLayout:
      t->setComputePipelineLayout(r.Value<const plume::RenderPipelineLayout *>());
      break;
    case kSetGraphicsPipelineLayout:
      t->setGraphicsPipelineLayout(r.Value<const plume::RenderPipelineLayout *>());
      break;
    case kSetComputePushConstants:
    case kSetGraphicsPushConstants: {
      const auto op = r.Value<PushConstantsOp>();
      const void *data = r.Bytes(op.size);
      if (header.op == kSetComputePushConstants)
        t->setComputePushConstants(op.range, data, op.offset, op.size);
      else
        t->setGraphicsPushConstants(op.range, data, op.offset, op.size);
      break;
    }
    case kSetComputeDescriptorSet: {
      const auto op = r.Value<DescriptorSetOp>();
      t->setComputeDescriptorSet(op.set, op.index);
      break;
    }
    case kSetGraphicsDescriptorSet: {
      const auto op = r.Value<DescriptorSetOp>();
      t->setGraphicsDescriptorSet(op.set, op.index);
      break;
    }
    case kSetGraphicsRootDescriptor: {
      const auto op = r.Value<RootDescriptorOp>();
      t->setGraphicsRootDescriptor(op.buffer, op.index);
      break;
    }
    case kSetIndexBuffer: {
      const auto op = r.Value<IndexBufferOp>();
      t->setIndexBuffer(op.has ? &op.view : nullptr);
      break;
    }
    case kSetVertexBuffers: {
      const auto op = r.Value<VertexBuffersOp>();
      const auto *views = r.Array<plume::RenderVertexBufferView>(op.count);
      const auto *slots = r.Array<plume::RenderInputSlot>(op.slots ? op.count : 0);
      t->setVertexBuffers(op.first, views, op.count, slots);
      break;
    }
    case kSetViewports: {
      const auto op = r.Value<CountOp>();
      t->setViewports(r.Array<plume::RenderViewport>(op.count), op.count);
      break;
    }
    case kSetScissors: {
      const auto op = r.Value<CountOp>();
      t->setScissors(r.Array<plume::RenderRect>(op.count), op.count);
      break;
    }
    case kSetFramebuffer:
      t->setFramebuffer(r.Value<const plume::RenderFramebuffer *>());
      break;
    case kSetDepthBias: {
      const auto op = r.Value<DepthBiasOp>();
      t->setDepthBias(op.bias, op.clamp, op.slope);
      break;
    }
    case kClearColor: {
      const auto op = r.Value<ClearColorOp>();
      t->clearColor(op.attachment, op.color, r.Array<plume::RenderRect>(op.rects), op.rects);
      break;
    }
    case kClearDepthStencil: {
      const auto op = r.Value<ClearDepthStencilOp>();
      t->clearDepthStencil(op.clear_depth, op.clear_stencil, op.depth, op.stencil,
                           r.Array<plume::RenderRect>(op.rects), op.rects);
      break;
    }
    case kCopyBufferRegion: {
      const auto op = r.Value<CopyBufferRegionOp>();
      t->copyBufferRegion(op.dst, op.src, op.size);
      break;
    }
    case kCopyTextureRegion: {
      const auto op = r.Value<CopyTextureRegionOp>();
      t->copyTextureRegion(op.dst, op.src, op.x, op.y, op.z, op.has_box ? &op.box : nullptr);
      break;
    }
    case kCopyBuffer: {
      const auto op = r.Value<PairOp>();
      t->copyBuffer(static_cast<const plume::RenderBuffer *>(op.dst),
                    static_cast<const plume::RenderBuffer *>(op.src));
      break;
    }
    case kCopyTexture:
    case kResolveTexture: {
      const auto op = r.Value<PairOp>();
      const auto *dst = static_cast<const plume::RenderTexture *>(op.dst);
      const auto *src = static_cast<const plume::RenderTexture *>(op.src);
      if (header.op == kCopyTexture)
        t->copyTexture(dst, src);
      else
        t->resolveTexture(dst, src);
      break;
    }
    case kResolveTextureRegion: {
      const auto op = r.Value<ResolveRegionOp>();
      t->resolveTextureRegion(op.dst, op.x, op.y, op.src, op.has_rect ? &op.rect : nullptr, op.mode);
      break;
    }
    case kDiscardTexture:
      t->discardTexture(r.Value<const plume::RenderTexture *>());
      break;
    case kResetQueryPool: {
      const auto op = r.Value<QueryPoolOp>();
      t->resetQueryPool(op.pool, op.first, op.count);
      break;
    }
    case kWriteTimestamp: {
      const auto op = r.Value<QueryPoolOp>();
      t->writeTimestamp(op.pool, op.first);
      break;
    }
    case kBlendConstant: {
      float rgba[4];
      std::memcpy(rgba, r.Bytes(sizeof(rgba)), sizeof(rgba));
      SetBlendConstant(t, rgba);
      break;
    }
    case kStencilReference:
      SetStencilReference(t, r.Value<uint32_t>());
      break;
    case kCall: {
      auto *call = r.Value<std::function<void()> *>();
      const Clock::time_point start = Clock::now();
      (*call)();
      g_call_time += Clock::now() - start;
      delete call;
      CallDone();
      break;
    }
    }
  }
}

// Nanoseconds the recording thread spent replaying (TakeBusyMs).
std::atomic<int64_t> g_busy_ns{0};

// The recording thread, shared by the recorders (only one records at a time:
// a slot's list is finished before the next slot's begins), and the chunks.
class Worker {
public:
  static Worker &Get() {
    // Never destroyed, like the render thread: the thread may still wait on it
    // at exit.
    static Worker *worker = new Worker;
    return *worker;
  }

  Chunk *TakeChunk(CommandRecorder *owner, size_t capacity) {
    Chunk *chunk = nullptr;
    {
      std::lock_guard lock(mutex_);
      for (size_t i = 0; i < free_.size(); ++i) {
        if (free_[i]->capacity >= capacity) {
          chunk = free_[i];
          free_[i] = free_.back();
          free_.pop_back();
          break;
        }
      }
    }
    if (!chunk) {
      chunk = new Chunk;
      chunk->data = std::make_unique<uint8_t[]>(capacity);
      chunk->capacity = capacity;
    }
    chunk->owner = owner;
    chunk->used = 0;
    return chunk;
  }

  void Push(Chunk *chunk) {
    bool wake;
    {
      std::lock_guard lock(mutex_);
      queue_.push_back(chunk);
      pushed_.store(pushed_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
      wake = sleeping_;
    }
    if (wake)
      work_cv_.notify_one();
  }

  void WaitIdle() {
    std::unique_lock lock(mutex_);
    done_cv_.wait(lock, [this] { return done_ == pushed_.load(std::memory_order_relaxed); });
  }

  uint64_t NextCallToken() { return ++calls_recorded_; }

  void CallDone() {
    {
      std::lock_guard lock(mutex_);
      ++calls_done_;
    }
    done_cv_.notify_all();
  }

  void WaitCall(uint64_t token) {
    std::unique_lock lock(mutex_);
    done_cv_.wait(lock, [this, token] { return calls_done_ >= token; });
  }

private:
  Worker() {
    std::thread(&Worker::Run, this).detach();
    RGPU_INFO("Command recording thread started (renut_threaded_recording)");
  }

  void Run() {
#if defined(_WIN32)
    SetThreadDescription(GetCurrentThread(), L"renut recorder");
#endif
    for (;;) {
      Chunk *chunk = nullptr;
      {
        std::unique_lock lock(mutex_);
        if (queue_.empty()) {
          lock.unlock();
          for (uint32_t i = 0; i < kSpinPauses && pushed_.load(std::memory_order_acquire) == done_; ++i) {
#if defined(_M_X64) || defined(__x86_64__)
            _mm_pause();
#else
            std::this_thread::yield();
#endif
          }
          lock.lock();
          sleeping_ = true;
          work_cv_.wait(lock, [this] { return !queue_.empty(); });
          sleeping_ = false;
        }
        chunk = queue_.front();
        queue_.pop_front();
      }
      const Clock::time_point start = Clock::now();
      g_call_time = {};
      Replay(chunk->owner->Target(), chunk->data.get(), chunk->data.get() + chunk->used);
      g_busy_ns.fetch_add(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start - g_call_time)
              .count(),
          std::memory_order_relaxed);
      {
        std::lock_guard lock(mutex_);
        chunk->used = 0;
        free_.push_back(chunk);
        ++done_;
      }
      done_cv_.notify_all();
    }
  }

  std::mutex mutex_;
  std::condition_variable work_cv_;  // a chunk queued
  std::condition_variable done_cv_;  // a chunk replayed
  std::deque<Chunk *> queue_;
  std::vector<Chunk *> free_;
  std::atomic<uint64_t> pushed_{0};  // written under mutex_
  uint64_t done_ = 0;                // written under mutex_, by the thread
  bool sleeping_ = false;
  std::atomic<uint64_t> calls_recorded_{0};  // tokens handed out
  uint64_t calls_done_ = 0;                  // written under mutex_, by the thread
};

void CallDone() {
  Worker::Get().CallDone();
}

// The recorders, for From (made once, with the device).
constexpr uint32_t kMaxRecorders = 4;
CommandRecorder *g_recorders[kMaxRecorders] = {};

} // namespace

CommandRecorder::CommandRecorder(plume::RenderCommandList *target) : target_(target) {
  for (CommandRecorder *&slot : g_recorders) {
    if (!slot) {
      slot = this;
      break;
    }
  }
  open_ = Worker::Get().TakeChunk(this, kChunkBytes);
}

CommandRecorder::~CommandRecorder() {
  Finish();
  for (CommandRecorder *&slot : g_recorders) {
    if (slot == this)
      slot = nullptr;
  }
  delete open_;
}

CommandRecorder *CommandRecorder::From(plume::RenderCommandList *list) {
  for (CommandRecorder *recorder : g_recorders) {
    if (recorder && recorder == list)
      return recorder;
  }
  return nullptr;
}

double CommandRecorder::TakeBusyMs() {
  return double(g_busy_ns.exchange(0, std::memory_order_relaxed)) / 1e6;
}

uint8_t *CommandRecorder::Reserve(uint32_t op, size_t payload) {
  const size_t bytes = sizeof(Header) + payload;
  if (open_->used >= kPublishBytes || open_->used + bytes > open_->capacity) {
    Publish();
    if (bytes > open_->capacity) {
      delete open_;
      open_ = Worker::Get().TakeChunk(this, bytes);
    }
  }
  uint8_t *at = open_->data.get() + open_->used;
  const Header header{op, uint32_t(payload)};
  std::memcpy(at, &header, sizeof(header));
  open_->used += bytes;
  return at + sizeof(header);
}

void CommandRecorder::Publish() {
  if (!open_->used)
    return;
  Worker &worker = Worker::Get();
  worker.Push(open_);
  open_ = worker.TakeChunk(this, kChunkBytes);
}

void CommandRecorder::Finish() {
  Publish();
  Worker::Get().WaitIdle();
}

uint64_t CommandRecorder::RecordCall(std::function<void()> call) {
  auto *heap = new std::function<void()>(std::move(call));
  Writer(Reserve(kCall, SizeOf<std::function<void()> *>())).Value(heap);
  // Calls run in token order: the thread replays in order.
  const uint64_t token = Worker::Get().NextCallToken();
  Publish();
  return token;
}

void CommandRecorder::WaitForCall(uint64_t token) {
  if (token)
    Worker::Get().WaitCall(token);
}

plume::RenderCommandList *CommandRecorder::Direct() {
  Finish();
  return target_;
}

void CommandRecorder::RecordBlendConstant(const float rgba[4]) {
  Writer(Reserve(kBlendConstant, Aligned(sizeof(float) * 4))).Bytes(rgba, sizeof(float) * 4);
}

void CommandRecorder::RecordStencilReference(uint32_t reference) {
  Writer(Reserve(kStencilReference, SizeOf<uint32_t>())).Value(reference);
}

void CommandRecorder::begin() {
  Reserve(kBegin, 0);
}

void CommandRecorder::end() {
  Reserve(kEnd, 0);
}

void CommandRecorder::barriers(plume::RenderBarrierStages stages,
                               const plume::RenderBufferBarrier *bufferBarriers,
                               uint32_t bufferBarriersCount,
                               const plume::RenderTextureBarrier *textureBarriers,
                               uint32_t textureBarriersCount) {
  const BarriersOp op{stages, bufferBarriers ? bufferBarriersCount : 0,
                      textureBarriers ? textureBarriersCount : 0};
  Writer w(Reserve(kBarriers, SizeOf<BarriersOp>() + SizeOf<plume::RenderBufferBarrier>(op.buffers) +
                                  SizeOf<plume::RenderTextureBarrier>(op.textures)));
  w.Value(op);
  w.Array(bufferBarriers, op.buffers);
  w.Array(textureBarriers, op.textures);
}

void CommandRecorder::dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY,
                               uint32_t threadGroupCountZ) {
  Writer(Reserve(kDispatch, SizeOf<DispatchOp>()))
      .Value(DispatchOp{threadGroupCountX, threadGroupCountY, threadGroupCountZ});
}

void CommandRecorder::traceRays(uint32_t width, uint32_t height, uint32_t depth,
                                plume::RenderBufferReference shaderBindingTable,
                                const plume::RenderShaderBindingGroupsInfo &shaderBindingGroupsInfo) {
  Direct()->traceRays(width, height, depth, shaderBindingTable, shaderBindingGroupsInfo);
}

void CommandRecorder::drawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount,
                                    uint32_t startVertexLocation, uint32_t startInstanceLocation) {
  Writer(Reserve(kDrawInstanced, SizeOf<DrawOp>()))
      .Value(DrawOp{vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation});
}

void CommandRecorder::drawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount,
                                           uint32_t startIndexLocation, int32_t baseVertexLocation,
                                           uint32_t startInstanceLocation) {
  Writer(Reserve(kDrawIndexedInstanced, SizeOf<DrawIndexedOp>()))
      .Value(DrawIndexedOp{indexCountPerInstance, instanceCount, startIndexLocation,
                           baseVertexLocation, startInstanceLocation});
}

void CommandRecorder::setPipeline(const plume::RenderPipeline *pipeline) {
  Writer(Reserve(kSetPipeline, SizeOf<const plume::RenderPipeline *>())).Value(pipeline);
}

void CommandRecorder::setComputePipelineLayout(const plume::RenderPipelineLayout *pipelineLayout) {
  Writer(Reserve(kSetComputePipelineLayout, SizeOf<const plume::RenderPipelineLayout *>()))
      .Value(pipelineLayout);
}

void CommandRecorder::setComputePushConstants(uint32_t rangeIndex, const void *data, uint32_t offset,
                                              uint32_t size) {
  if (!size) {
    // The range's size is the layout's, which is not known here.
    static bool logged = false;
    if (!logged) {
      logged = true;
      RGPU_WARN("Command recorder: push constants without a size are set synchronously");
    }
    Direct()->setComputePushConstants(rangeIndex, data, offset, size);
    return;
  }
  Writer w(Reserve(kSetComputePushConstants, SizeOf<PushConstantsOp>() + Aligned(size)));
  w.Value(PushConstantsOp{rangeIndex, offset, size});
  w.Bytes(data, size);
}

void CommandRecorder::setComputeDescriptorSet(plume::RenderDescriptorSet *descriptorSet,
                                              uint32_t setIndex) {
  Writer(Reserve(kSetComputeDescriptorSet, SizeOf<DescriptorSetOp>()))
      .Value(DescriptorSetOp{descriptorSet, setIndex});
}

void CommandRecorder::setGraphicsPipelineLayout(const plume::RenderPipelineLayout *pipelineLayout) {
  Writer(Reserve(kSetGraphicsPipelineLayout, SizeOf<const plume::RenderPipelineLayout *>()))
      .Value(pipelineLayout);
}

void CommandRecorder::setGraphicsPushConstants(uint32_t rangeIndex, const void *data,
                                               uint32_t offset, uint32_t size) {
  if (!size) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      RGPU_WARN("Command recorder: push constants without a size are set synchronously");
    }
    Direct()->setGraphicsPushConstants(rangeIndex, data, offset, size);
    return;
  }
  Writer w(Reserve(kSetGraphicsPushConstants, SizeOf<PushConstantsOp>() + Aligned(size)));
  w.Value(PushConstantsOp{rangeIndex, offset, size});
  w.Bytes(data, size);
}

void CommandRecorder::setGraphicsDescriptorSet(plume::RenderDescriptorSet *descriptorSet,
                                               uint32_t setIndex) {
  Writer(Reserve(kSetGraphicsDescriptorSet, SizeOf<DescriptorSetOp>()))
      .Value(DescriptorSetOp{descriptorSet, setIndex});
}

void CommandRecorder::setGraphicsRootDescriptor(plume::RenderBufferReference bufferReference,
                                                uint32_t rootDescriptorIndex) {
  Writer(Reserve(kSetGraphicsRootDescriptor, SizeOf<RootDescriptorOp>()))
      .Value(RootDescriptorOp{bufferReference, rootDescriptorIndex});
}

void CommandRecorder::setRaytracingPipelineLayout(const plume::RenderPipelineLayout *pipelineLayout) {
  Direct()->setRaytracingPipelineLayout(pipelineLayout);
}

void CommandRecorder::setRaytracingPushConstants(uint32_t rangeIndex, const void *data,
                                                 uint32_t offset, uint32_t size) {
  Direct()->setRaytracingPushConstants(rangeIndex, data, offset, size);
}

void CommandRecorder::setRaytracingDescriptorSet(plume::RenderDescriptorSet *descriptorSet,
                                                 uint32_t setIndex) {
  Direct()->setRaytracingDescriptorSet(descriptorSet, setIndex);
}

void CommandRecorder::setIndexBuffer(const plume::RenderIndexBufferView *view) {
  IndexBufferOp op{};
  op.has = view != nullptr;
  if (view)
    op.view = *view;
  Writer(Reserve(kSetIndexBuffer, SizeOf<IndexBufferOp>())).Value(op);
}

void CommandRecorder::setVertexBuffers(uint32_t startSlot, const plume::RenderVertexBufferView *views,
                                       uint32_t viewCount, const plume::RenderInputSlot *inputSlots) {
  const VertexBuffersOp op{startSlot, views ? viewCount : 0, inputSlots != nullptr};
  Writer w(Reserve(kSetVertexBuffers, SizeOf<VertexBuffersOp>() +
                                          SizeOf<plume::RenderVertexBufferView>(op.count) +
                                          SizeOf<plume::RenderInputSlot>(op.slots ? op.count : 0)));
  w.Value(op);
  w.Array(views, op.count);
  w.Array(inputSlots, op.slots ? op.count : 0);
}

void CommandRecorder::setViewports(const plume::RenderViewport *viewports, uint32_t count) {
  const CountOp op{viewports ? count : 0};
  Writer w(Reserve(kSetViewports, SizeOf<CountOp>() + SizeOf<plume::RenderViewport>(op.count)));
  w.Value(op);
  w.Array(viewports, op.count);
}

void CommandRecorder::setScissors(const plume::RenderRect *scissorRects, uint32_t count) {
  const CountOp op{scissorRects ? count : 0};
  Writer w(Reserve(kSetScissors, SizeOf<CountOp>() + SizeOf<plume::RenderRect>(op.count)));
  w.Value(op);
  w.Array(scissorRects, op.count);
}

void CommandRecorder::setFramebuffer(const plume::RenderFramebuffer *framebuffer) {
  Writer(Reserve(kSetFramebuffer, SizeOf<const plume::RenderFramebuffer *>())).Value(framebuffer);
}

void CommandRecorder::setDepthBias(float depthBias, float depthBiasClamp, float slopeScaledDepthBias) {
  Writer(Reserve(kSetDepthBias, SizeOf<DepthBiasOp>()))
      .Value(DepthBiasOp{depthBias, depthBiasClamp, slopeScaledDepthBias});
}

void CommandRecorder::clearColor(uint32_t attachmentIndex, plume::RenderColor colorValue,
                                 const plume::RenderRect *clearRects, uint32_t clearRectsCount) {
  ClearColorOp op{};
  op.color = colorValue;
  op.attachment = attachmentIndex;
  op.rects = clearRects ? clearRectsCount : 0;
  Writer w(Reserve(kClearColor, SizeOf<ClearColorOp>() + SizeOf<plume::RenderRect>(op.rects)));
  w.Value(op);
  w.Array(clearRects, op.rects);
}

void CommandRecorder::clearDepthStencil(bool clearDepth, bool clearStencil, float depthValue,
                                        uint32_t stencilValue, const plume::RenderRect *clearRects,
                                        uint32_t clearRectsCount) {
  ClearDepthStencilOp op{};
  op.depth = depthValue;
  op.stencil = stencilValue;
  op.rects = clearRects ? clearRectsCount : 0;
  op.clear_depth = clearDepth;
  op.clear_stencil = clearStencil;
  Writer w(Reserve(kClearDepthStencil, SizeOf<ClearDepthStencilOp>() + SizeOf<plume::RenderRect>(op.rects)));
  w.Value(op);
  w.Array(clearRects, op.rects);
}

void CommandRecorder::copyBufferRegion(plume::RenderBufferReference dstBuffer,
                                       plume::RenderBufferReference srcBuffer, uint64_t size) {
  Writer(Reserve(kCopyBufferRegion, SizeOf<CopyBufferRegionOp>()))
      .Value(CopyBufferRegionOp{dstBuffer, srcBuffer, size});
}

void CommandRecorder::copyTextureRegion(const plume::RenderTextureCopyLocation &dstLocation,
                                        const plume::RenderTextureCopyLocation &srcLocation,
                                        uint32_t dstX, uint32_t dstY, uint32_t dstZ,
                                        const plume::RenderBox *srcBox) {
  CopyTextureRegionOp op{};
  op.dst = dstLocation;
  op.src = srcLocation;
  op.has_box = srcBox != nullptr;
  if (srcBox)
    op.box = *srcBox;
  op.x = dstX;
  op.y = dstY;
  op.z = dstZ;
  Writer(Reserve(kCopyTextureRegion, SizeOf<CopyTextureRegionOp>())).Value(op);
}

void CommandRecorder::copyBuffer(const plume::RenderBuffer *dstBuffer,
                                 const plume::RenderBuffer *srcBuffer) {
  Writer(Reserve(kCopyBuffer, SizeOf<PairOp>())).Value(PairOp{dstBuffer, srcBuffer});
}

void CommandRecorder::copyTexture(const plume::RenderTexture *dstTexture,
                                  const plume::RenderTexture *srcTexture) {
  Writer(Reserve(kCopyTexture, SizeOf<PairOp>())).Value(PairOp{dstTexture, srcTexture});
}

void CommandRecorder::resolveTexture(const plume::RenderTexture *dstTexture,
                                     const plume::RenderTexture *srcTexture) {
  Writer(Reserve(kResolveTexture, SizeOf<PairOp>())).Value(PairOp{dstTexture, srcTexture});
}

void CommandRecorder::resolveTextureRegion(const plume::RenderTexture *dstTexture, uint32_t dstX,
                                           uint32_t dstY, const plume::RenderTexture *srcTexture,
                                           const plume::RenderRect *srcRect,
                                           plume::RenderResolveMode resolveMode) {
  ResolveRegionOp op{};
  op.dst = dstTexture;
  op.src = srcTexture;
  op.has_rect = srcRect != nullptr;
  if (srcRect)
    op.rect = *srcRect;
  op.x = dstX;
  op.y = dstY;
  op.mode = resolveMode;
  Writer(Reserve(kResolveTextureRegion, SizeOf<ResolveRegionOp>())).Value(op);
}

void CommandRecorder::buildBottomLevelAS(const plume::RenderAccelerationStructure *dstAccelerationStructure,
                                         plume::RenderBufferReference scratchBuffer,
                                         const plume::RenderBottomLevelASBuildInfo &buildInfo) {
  Direct()->buildBottomLevelAS(dstAccelerationStructure, scratchBuffer, buildInfo);
}

void CommandRecorder::buildTopLevelAS(const plume::RenderAccelerationStructure *dstAccelerationStructure,
                                      plume::RenderBufferReference scratchBuffer,
                                      plume::RenderBufferReference instancesBuffer,
                                      const plume::RenderTopLevelASBuildInfo &buildInfo) {
  Direct()->buildTopLevelAS(dstAccelerationStructure, scratchBuffer, instancesBuffer, buildInfo);
}

void CommandRecorder::discardTexture(const plume::RenderTexture *texture) {
  Writer(Reserve(kDiscardTexture, SizeOf<const plume::RenderTexture *>())).Value(texture);
}

void CommandRecorder::resetQueryPool(const plume::RenderQueryPool *queryPool, uint32_t queryFirstIndex,
                                     uint32_t queryCount) {
  Writer(Reserve(kResetQueryPool, SizeOf<QueryPoolOp>()))
      .Value(QueryPoolOp{queryPool, queryFirstIndex, queryCount});
}

void CommandRecorder::writeTimestamp(const plume::RenderQueryPool *queryPool, uint32_t queryIndex) {
  Writer(Reserve(kWriteTimestamp, SizeOf<QueryPoolOp>())).Value(QueryPoolOp{queryPool, queryIndex, 0});
}

} // namespace renut::gpu
