/**
 * @file    gpu/render/ambient_occlusion.h
 * @brief   renut_ssao: screen-space ambient occlusion over the scene.
 *
 * N&B has an SSAO pass of its own (sub_8232E488 loads its fxSSAO* shaders and
 * makes its 640x360 targets at EDRAM 1864), but nothing ever draws it. reNut's
 * occlusion is drawn when the game resolves the scene's depth (the frame's
 * first depth resolve of a screen-sized float depth target, after its opaque
 * geometry; with tiling, the resolve of the last tile): three fullscreen passes
 * (shaders/host/ambient_occlusion*) read the resolved depth, find each pixel's
 * occlusion, blur it along the depth in two directions, and multiply it into
 * the scene's color target. What the game draws later (water, particles, the
 * HUD) is drawn over it. The depth is made view space with the camera's near
 * and far distances and vertical field of view (meCalculateProjection,
 * 0x822BC0E0).
 */
#pragma once

#include <cstdint>

namespace renut::gpu {

struct HostSurface;
struct HostTexture;

// Once a frame (LatchGraphicsOptionsLocked): the next scene depth resolve gets
// the occlusion.
void AmbientOcclusionNewFrameLocked();

// After the depth resolve of 'depth' into 'resolved', whose rect reaches guest
// row 'bottom' of it, with 'color' the device's first color target: when it
// resolves the frame's scene depth, draws the occlusion over 'color'. Records
// render passes of its own; the caller binds its pipeline, layout, targets
// and viewport again. True if it drew.
bool ApplyAmbientOcclusionLocked(HostSurface *depth, HostTexture *resolved, int32_t bottom,
                                 HostSurface *color);

} // namespace renut::gpu
