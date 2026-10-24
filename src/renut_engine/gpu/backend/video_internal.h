/**
 * @file    gpu/backend/video_internal.h
 * @brief   Frame-ring helpers shared by video.cpp and present.cpp.
 *          All require State().mutex to be held.
 */
#pragma once

#include "renut_engine/gpu/backend/video.h"

namespace renut::gpu::detail {

// (Re)builds swapchain framebuffers and per-image present semaphores.
bool BuildSwapChainTargets(VideoState &s);

// Creates the swapchain on the window's current surface, with its targets.
bool CreateSwapChain(VideoState &s, plume::RenderWindow window);

// Frees everything retired into 'slot' and rewinds its upload ring. Only once
// the slot's fence has been waited.
void DrainSlot(VideoState &s, uint32_t slot);

// Waits every submitted slot's fence (resize / shutdown).
void WaitAllSubmitted(VideoState &s);

// Ends and submits the open list for the current slot, then moves to the next
// slot, waiting its fence and draining it.
void SubmitAndAdvance(VideoState &s, plume::RenderCommandSemaphore *wait,
                      plume::RenderCommandSemaphore *signal);

// Completes draw work recorded ahead of its data (draw.cpp) and closes any
// open render pass state before the list is ended.
void FinishRecordingLocked(VideoState &s);

// Submits the open list, waits for the GPU to finish it and opens a new list
// in the same slot: for results the CPU needs before recording goes on.
void SubmitAndWaitLocked(VideoState &s);

// Waits for a slot's submitted list, if it has one (its fence is waited only
// once), ahead of the slot's reuse.
void WaitSlotLocked(VideoState &s, uint32_t slot);

// Once a slot's lists have finished: writes the results of its memexport
// draws to guest memory (draw.cpp).
void CompleteMemExportsLocked(uint32_t slot);

// Once a slot's lists have finished: the cached vertex and index copies freed
// while it recorded can be reused (draw.cpp).
void ReleaseCacheCopiesLocked(uint32_t slot);

// A new descriptor set for stream_convert_cs (its buffers unset).
std::unique_ptr<plume::RenderDescriptorSet> CreateStreamConvertSet(VideoState &s);
// A new descriptor set for target_readback_cs's words buffer (unset).
std::unique_ptr<plume::RenderDescriptorSet> CreateTargetReadbackSet(VideoState &s);
// Right after the current list begins / before it ends: its GPU timestamps.
void BeginListTiming(VideoState &s);
void EndListTiming(VideoState &s, bool last_in_frame);

} // namespace renut::gpu::detail
