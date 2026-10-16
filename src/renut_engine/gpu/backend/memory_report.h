/**
 * @file    gpu/backend/memory_report.h
 * @brief   Periodic log line of where reNut's memory goes.
 *
 * The process's memory (private and resident), the GPU's (local and shared,
 * from the backend) and the renderer's own pools: render targets, textures,
 * upload rings, the vertex stream cache, memexport buffers, recorded command
 * buffers, pipelines and the shader cache. Every renut_memory_report seconds.
 */
#pragma once

namespace renut::gpu {

// Once per guest Swap on the render thread, without State().mutex held (the
// command buffer lock is never taken inside it).
void MaybeReportMemory();

} // namespace renut::gpu
