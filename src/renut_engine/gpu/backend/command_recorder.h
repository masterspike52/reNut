/**
 * @file    gpu/backend/command_recorder.h
 * @brief   A command list that hands its calls to a thread of their own
 *          (renut_threaded_recording).
 *
 * The render thread's work is the game's draws turned into host calls, and a
 * fifth of it was the host API's own: the driver behind setPipeline, the
 * barriers, copies and the rest of a list's calls (xperf, a GTX 1650 laptop,
 * 2026-10-22). A CommandRecorder stands in for a frame slot's backend list:
 * each call is written to memory, by value, and a recording thread makes the
 * real call on the backend list a batch at a time, while the render thread
 * works on the next draws. Finish waits for the thread to catch up. The
 * frame's submission and present are calls the thread makes after the list's
 * (RecordCall), so the render thread goes on to the next frame; a submission
 * in the middle of a frame (SubmitAndWaitLocked) finishes the list first.
 *
 * What a call refers to (pipelines, buffers, textures, framebuffers, sets,
 * query pools) must outlive the call reaching the backend list, which the
 * renderer's objects do: they are retired until the slot's fence (or kept),
 * and the fence is only waited after the list has been finished and
 * submitted. Push constants are copied, so they need their size.
 *
 * Calls come from one thread at a time (the render thread, or the UI thread
 * drawing the overlay while the render thread waits for it).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include <plume_render_interface.h>

namespace renut::gpu {

// renut_threaded_recording: whether the frame slots record through
// CommandRecorders (read when the device is created).
bool ThreadedRecordingEnabled();

class CommandRecorder final : public plume::RenderCommandList {
public:
  explicit CommandRecorder(plume::RenderCommandList *target);
  ~CommandRecorder() override;
  CommandRecorder(const CommandRecorder &) = delete;
  CommandRecorder &operator=(const CommandRecorder &) = delete;

  // The recorder 'list' is, or null.
  static CommandRecorder *From(plume::RenderCommandList *list);
  // Milliseconds the recording thread spent on calls since the last time
  // asked (render thread).
  static double TakeBusyMs();

  plume::RenderCommandList *Target() const { return target_; }

  // Returns once every call made so far has been made on the backend list.
  void Finish();

  // Runs 'call' on the recording thread once the calls recorded before it
  // have been made (the queue's submission of an ended list, a present), and
  // hands it over at once. The token is for WaitForCall.
  uint64_t RecordCall(std::function<void()> call);
  // Returns once the call RecordCall gave 'token' for has run (0: at once).
  static void WaitForCall(uint64_t token);

  // backend.h's SetBlendConstant / SetStencilReference, recorded.
  void RecordBlendConstant(const float rgba[4]);
  void RecordStencilReference(uint32_t reference);

  using plume::RenderCommandList::barriers;
  using plume::RenderCommandList::setScissors;
  using plume::RenderCommandList::setViewports;

  void begin() override;
  void end() override;
  void barriers(plume::RenderBarrierStages stages, const plume::RenderBufferBarrier *bufferBarriers,
                uint32_t bufferBarriersCount, const plume::RenderTextureBarrier *textureBarriers,
                uint32_t textureBarriersCount) override;
  void dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY,
                uint32_t threadGroupCountZ) override;
  void traceRays(uint32_t width, uint32_t height, uint32_t depth,
                 plume::RenderBufferReference shaderBindingTable,
                 const plume::RenderShaderBindingGroupsInfo &shaderBindingGroupsInfo) override;
  void drawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount,
                     uint32_t startVertexLocation, uint32_t startInstanceLocation) override;
  void drawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount,
                            uint32_t startIndexLocation, int32_t baseVertexLocation,
                            uint32_t startInstanceLocation) override;
  void setPipeline(const plume::RenderPipeline *pipeline) override;
  void setComputePipelineLayout(const plume::RenderPipelineLayout *pipelineLayout) override;
  void setComputePushConstants(uint32_t rangeIndex, const void *data, uint32_t offset,
                               uint32_t size) override;
  void setComputeDescriptorSet(plume::RenderDescriptorSet *descriptorSet,
                               uint32_t setIndex) override;
  void setGraphicsPipelineLayout(const plume::RenderPipelineLayout *pipelineLayout) override;
  void setGraphicsPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset,
                                uint32_t size) override;
  void setGraphicsDescriptorSet(plume::RenderDescriptorSet *descriptorSet,
                                uint32_t setIndex) override;
  void setGraphicsRootDescriptor(plume::RenderBufferReference bufferReference,
                                 uint32_t rootDescriptorIndex) override;
  void setRaytracingPipelineLayout(const plume::RenderPipelineLayout *pipelineLayout) override;
  void setRaytracingPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset,
                                  uint32_t size) override;
  void setRaytracingDescriptorSet(plume::RenderDescriptorSet *descriptorSet,
                                  uint32_t setIndex) override;
  void setIndexBuffer(const plume::RenderIndexBufferView *view) override;
  void setVertexBuffers(uint32_t startSlot, const plume::RenderVertexBufferView *views,
                        uint32_t viewCount, const plume::RenderInputSlot *inputSlots) override;
  void setViewports(const plume::RenderViewport *viewports, uint32_t count) override;
  void setScissors(const plume::RenderRect *scissorRects, uint32_t count) override;
  void setFramebuffer(const plume::RenderFramebuffer *framebuffer) override;
  void setDepthBias(float depthBias, float depthBiasClamp, float slopeScaledDepthBias) override;
  void clearColor(uint32_t attachmentIndex, plume::RenderColor colorValue,
                  const plume::RenderRect *clearRects, uint32_t clearRectsCount) override;
  void clearDepthStencil(bool clearDepth, bool clearStencil, float depthValue, uint32_t stencilValue,
                         const plume::RenderRect *clearRects, uint32_t clearRectsCount) override;
  void copyBufferRegion(plume::RenderBufferReference dstBuffer,
                        plume::RenderBufferReference srcBuffer, uint64_t size) override;
  void copyTextureRegion(const plume::RenderTextureCopyLocation &dstLocation,
                         const plume::RenderTextureCopyLocation &srcLocation, uint32_t dstX,
                         uint32_t dstY, uint32_t dstZ, const plume::RenderBox *srcBox) override;
  void copyBuffer(const plume::RenderBuffer *dstBuffer, const plume::RenderBuffer *srcBuffer) override;
  void copyTexture(const plume::RenderTexture *dstTexture,
                   const plume::RenderTexture *srcTexture) override;
  void resolveTexture(const plume::RenderTexture *dstTexture,
                      const plume::RenderTexture *srcTexture) override;
  void resolveTextureRegion(const plume::RenderTexture *dstTexture, uint32_t dstX, uint32_t dstY,
                            const plume::RenderTexture *srcTexture, const plume::RenderRect *srcRect,
                            plume::RenderResolveMode resolveMode) override;
  void buildBottomLevelAS(const plume::RenderAccelerationStructure *dstAccelerationStructure,
                          plume::RenderBufferReference scratchBuffer,
                          const plume::RenderBottomLevelASBuildInfo &buildInfo) override;
  void buildTopLevelAS(const plume::RenderAccelerationStructure *dstAccelerationStructure,
                       plume::RenderBufferReference scratchBuffer,
                       plume::RenderBufferReference instancesBuffer,
                       const plume::RenderTopLevelASBuildInfo &buildInfo) override;
  void discardTexture(const plume::RenderTexture *texture) override;
  void resetQueryPool(const plume::RenderQueryPool *queryPool, uint32_t queryFirstIndex,
                      uint32_t queryCount) override;
  void writeTimestamp(const plume::RenderQueryPool *queryPool, uint32_t queryIndex) override;

  struct Chunk;

private:
  // Room for an op and 'payload' bytes after its header, in the open chunk.
  uint8_t *Reserve(uint32_t op, size_t payload);
  // Hands the open chunk to the recording thread.
  void Publish();
  // For the calls not recorded (ray tracing, push constants of unknown size):
  // the thread caught up, the call is made here.
  plume::RenderCommandList *Direct();

  plume::RenderCommandList *target_;
  Chunk *open_ = nullptr;
};

} // namespace renut::gpu
