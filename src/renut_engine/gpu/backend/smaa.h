#pragma once

#include "renut_engine/gpu/backend/video.h"

namespace renut::gpu {

// Records SMAA 1x over 'image' and returns SMAA's output to scan out, or 'image'
// itself when SMAA cannot run. Caller holds s.mutex with the command list open.
PresentImage ApplySmaaLocked(VideoState &s, const PresentImage &image);

}  // namespace renut::gpu
