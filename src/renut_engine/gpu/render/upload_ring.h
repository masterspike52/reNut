/**
 * @file    gpu/render/upload_ring.h
 * @brief   Per-frame linear allocator over host-visible UPLOAD memory.
 *
 * One ring per frame slot. Allocations live until that slot's fence is waited
 * at reuse, when Reset() rewinds it. Overflow grows into extra chunks that are
 * kept (and reused) rather than failing the frame, until no frame has needed
 * them for a while: a load that uploads hundreds of megabytes of textures in
 * one frame would otherwise keep that much host memory for good.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

namespace renut::gpu {

struct UploadAllocation {
  plume::RenderBufferReference ref;  // buffer + offset, for copies / views
  void *cpu = nullptr;               // mapped write pointer
  uint64_t gpu_address = 0;          // device address (Vulkan raw loads)
  uint32_t size = 0;
  explicit operator bool() const { return cpu != nullptr; }
};

class UploadRing {
public:
  static constexpr uint64_t kChunkSize = 32ull << 20;
  // Resets (frames of this slot) after which chunks beyond what those frames
  // used are released.
  static constexpr uint32_t kTrimResets = 300;

  bool Init(plume::RenderDevice *device, const char *name);
  // The slot's fence has been waited: nothing uses the ring's memory.
  void Reset();
  UploadAllocation Allocate(uint64_t size, uint64_t alignment = 256);
  UploadAllocation Upload(const void *data, uint64_t size, uint64_t alignment = 256);
  uint64_t Bytes() const;

private:
  struct Chunk {
    std::unique_ptr<plume::RenderBuffer> buffer;
    uint8_t *mapped = nullptr;
    uint64_t size = 0;
    uint64_t address = 0;
  };

  bool AddChunk(uint64_t min_size);

  plume::RenderDevice *device_ = nullptr;
  const char *name_ = "upload";
  std::vector<Chunk> chunks_;
  size_t current_ = 0;
  uint64_t offset_ = 0;
  // The most chunks a frame used since the last trim, and the resets since.
  size_t window_chunks_ = 1;
  uint32_t window_resets_ = 0;
};

} // namespace renut::gpu
