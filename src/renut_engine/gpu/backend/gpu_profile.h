/**
 * @file    gpu/backend/gpu_profile.h
 * @brief   Where the GPU's time goes, by kind of work (renut_gpu_profile).
 *
 * With renut_gpu_profile above 0, each frame's command lists write a
 * timestamp whenever the work changes kind (draws into another target, a
 * resolve, a depth resolve, host mip levels, present); once the frame's fence
 * has passed, the spans between them are added to their kind, and every
 * renut_gpu_profile seconds the log lists the kinds by GPU milliseconds per
 * frame. Off, it records nothing. All functions run on the render thread with
 * State().mutex held.
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace renut::gpu {
struct VideoState;
}

namespace renut::gpu::gpu_profile {

// The id of a kind of work, by name.
uint32_t Label(std::string_view name);

// Work recorded from here on in the open command list is 'label''s.
void Mark(VideoState &s, uint32_t label);

// A command list of the frame begins or ends (BeginListTiming,
// EndListTiming).
void BeginList(VideoState &s);
void EndList(VideoState &s, bool last_in_frame);

// The slot's frame has finished on the GPU (its fence was waited).
void Collect(VideoState &s, uint32_t slot);

} // namespace renut::gpu::gpu_profile
