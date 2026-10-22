/**
 * @file    gpu/backend/smaa.h
 * @brief   renut_anti_aliasing SMAA: SMAA 1x (thirdparty/smaa) over the
 *          finished frame, before it is scanned out.
 *
 * Three fullscreen passes at the frame's own size (shaders/host/smaa_*):
 * luma edges into a cleared target, blending weights from the edges with
 * SMAA's precomputed area and search textures, then the frame blended with
 * its neighbours into a target the present pass scans out instead of the
 * frame. Unlike FXAA it follows edges' real shapes, so it keeps text and
 * texture detail sharp. The targets follow the frame's size; the lookup
 * textures are uploaded once.
 */
#pragma once

#include "renut_engine/gpu/backend/video.h"

namespace renut::gpu {

// Records SMAA over 'image' and returns the image to scan out: SMAA's output,
// or 'image' itself when SMAA cannot run (its pipelines or textures could not
// be made). Records render passes of its own: before the back buffer's. The
// command list is open; caller holds s.mutex.
PresentImage ApplySmaaLocked(VideoState &s, const PresentImage &image);

}  // namespace renut::gpu
