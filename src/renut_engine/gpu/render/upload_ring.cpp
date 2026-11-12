#include "renut_engine/gpu/render/upload_ring.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace renut::gpu {

bool UploadRing::Init(plume::RenderDevice *device, const char *name) {
  device_ = device;
  name_ = name;
  chunks_.clear();
  current_ = 0;
  offset_ = 0;
  return AddChunk(kChunkSize);
}

bool UploadRing::AddChunk(uint64_t min_size) {
  const uint64_t size = std::max(kChunkSize, min_size);
  // CONSTANT gives plume's Vulkan buffer the device address guest shaders read
  // their constants through (vk::RawBufferLoad).
  auto buffer = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(
      size, plume::RenderBufferFlag::CONSTANT | plume::RenderBufferFlag::VERTEX |
                plume::RenderBufferFlag::INDEX | plume::RenderBufferFlag::DEVICE_ADDRESSABLE));
  if (!buffer)
    return false;
  void *mapped = buffer->map();
  if (!mapped)
    return false;
  buffer->setName(std::string(name_) + " #" + std::to_string(chunks_.size()));
  Chunk chunk;
  chunk.address = buffer->getDeviceAddress();
  chunk.mapped = static_cast<uint8_t *>(mapped);
  chunk.size = size;
  chunk.buffer = std::move(buffer);
  chunks_.push_back(std::move(chunk));
  return true;
}

void UploadRing::Reset() {
  window_chunks_ = std::max(window_chunks_, current_ + 1);
  if (++window_resets_ >= kTrimResets) {
    // A large chunk made for one big allocation goes too; the first one stays.
    if (chunks_.size() > window_chunks_)
      chunks_.resize(std::max<size_t>(window_chunks_, 1));
    window_chunks_ = 1;
    window_resets_ = 0;
  }
  current_ = 0;
  offset_ = 0;
}

UploadAllocation UploadRing::Allocate(uint64_t size, uint64_t alignment) {
  UploadAllocation out;
  if (size == 0 || chunks_.empty())
    return out;
  alignment = std::max<uint64_t>(alignment, 16);
  for (;;) {
    Chunk &chunk = chunks_[current_];
    const uint64_t aligned = (offset_ + alignment - 1) & ~(alignment - 1);
    if (aligned + size <= chunk.size) {
      out.ref = plume::RenderBufferReference(chunk.buffer.get(), aligned);
      out.cpu = chunk.mapped + aligned;
      out.gpu_address = chunk.address ? chunk.address + aligned : 0;
      out.size = static_cast<uint32_t>(size);
      offset_ = aligned + size;
      return out;
    }
    // Next chunk, growing the ring if this frame outgrew every existing one.
    if (current_ + 1 >= chunks_.size() && !AddChunk(size))
      return out;
    ++current_;
    offset_ = 0;
  }
}

UploadAllocation UploadRing::Upload(const void *data, uint64_t size, uint64_t alignment) {
  UploadAllocation out = Allocate(size, alignment);
  if (out && data)
    std::memcpy(out.cpu, data, size);
  return out;
}

} // namespace renut::gpu
