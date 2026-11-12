#pragma once

#include <cstdint>

namespace renut::gpu {

struct HostSurface;
struct HostTexture;

// Once a frame (LatchGraphicsOptionsLocked): the next scene depth resolve gets
// the occlusion.
void AmbientOcclusionNewFrameLocked();

// After a depth resolve that is the frame's scene depth, draws the occlusion
// over 'color'; the caller must rebind its pipeline state. True if it drew.
bool ApplyAmbientOcclusionLocked(HostSurface *depth, HostTexture *resolved, int32_t bottom,
                                 HostSurface *color);

} // namespace renut::gpu
