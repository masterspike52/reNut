/**
 * @file    gpu/backend/backend.h
 * @brief   Runtime choice between the D3D12 and Vulkan plume backends.
 *
 * Windows builds carry both backends and pick one at startup from the
 * renut_gpu_backend cvar. Other platforms are Vulkan only.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <plume_render_interface.h>

namespace renut::gpu {

enum class Backend { D3D12, Vulkan };

// Resolved once from renut_gpu_backend on first call; stable afterwards, so a
// change from the settings takes effect on the next start.
Backend ActiveBackend();
inline bool IsVulkan() { return ActiveBackend() == Backend::Vulkan; }
const char *BackendName(Backend backend);

plume::RenderShaderFormat HostShaderFormat();

// Creates the plume interface for the active backend, or nullptr. 'window' is
// the game window (GetNativeRenderWindow): on Linux plume's Vulkan interface
// asks it for the surface's instance extensions.
std::unique_ptr<plume::RenderInterface> CreateRenderInterface(plume::RenderWindow window);

// Writes the persistent pipeline cache now (Android), e.g. before the
// app is sent to the background and maybe killed.
void FlushPipelineCache();

// Buffer flags for how the renderer uses a buffer, per backend. plume's
// Vulkan buffers take their usage from particular flags: a device address
// (vk::RawBufferLoad/Store in the shaders) only with CONSTANT, a storage
// buffer binding only with STORAGE; DEVICE_ADDRESSABLE and UNORDERED_ACCESS
// alone give neither, and a shader then reads or writes through a garbage
// address (a lost device). D3D12 binds these as root descriptors and UAVs,
// which the flags below leave as they were.
plume::RenderBufferFlags AddressableBufferFlags();  // read/written by device address
plume::RenderBufferFlags StorageBufferFlags();      // bound as a storage buffer / UAV

// plume returns a graphics pipeline object even when the backend rejected the
// description; true if the backend pipeline exists.
bool GraphicsPipelineCreated(const plume::RenderPipeline *pipeline);

// Same for textures and buffers.
bool TextureCreated(const plume::RenderTexture *texture);
bool BufferCreated(const plume::RenderBuffer *buffer);

// True once the device is lost (D3D12 device removed). The first call that
// sees it logs the reason and, on D3D12, what DRED recorded: the command list
// and operation the GPU was executing and any page fault address.
bool ReportDeviceLost(plume::RenderDevice *device);

// Video memory the process uses: 'local' the GPU's own (dedicated) memory,
// 'shared' system memory the GPU reaches (upload and readback heaps, and what
// the driver moves out of local memory when it runs short). The budgets are
// what the OS lets the process use before that happens. D3D12 reports the
// process's usage (DXGI); Vulkan what the device's allocator made (VMA).
struct GpuMemoryUsage {
  bool valid = false;
  uint64_t local_used = 0, local_budget = 0;
  uint64_t shared_used = 0, shared_budget = 0;
};
GpuMemoryUsage QueryGpuMemory(plume::RenderDevice *device);

// Sets the blend constant BLEND_FACTOR / INV_BLEND_FACTOR read for the rest of
// the command list. plume has no call for it, so this goes to the native
// command list. On Vulkan it lasts only until a pipeline that does not read
// the constant is bound (only those that read it have it as dynamic state).
bool SetBlendConstant(plume::RenderCommandList *cmd, const float rgba[4]);

// Sets the stencil reference for the rest of the command list (D3D12). plume's
// D3D12 pipelines never take RenderGraphicsPipelineDesc::stencilReference: they
// all set 0 before each draw (checkStencilRef), which, being the cached value,
// it then never sends; the reference set here stays. Vulkan pipelines have the
// reference built in: nothing to do there.
void SetStencilReference(plume::RenderCommandList *cmd, uint32_t reference);

// A host shader compiled at build time for both backends (see gpu.cmake).
struct HostShaderBlob {
  const uint8_t *spirv = nullptr;
  size_t spirv_size = 0;
  const uint8_t *dxil = nullptr;
  size_t dxil_size = 0;
};

std::unique_ptr<plume::RenderShader> CreateHostShader(plume::RenderDevice *device,
                                                      const HostShaderBlob &blob,
                                                      const char *name);

} // namespace renut::gpu

// RENUT_HOST_SHADER(overlay_vs) -> HostShaderBlob for gpu/shaders/host/overlay_vs.hlsl.
// The including TU must include "host_shaders/<stem>.spirv.h" (and .dxil.h on Windows).
#if defined(_WIN32)
#define RENUT_HOST_SHADER(stem)                                                    \
  ::renut::gpu::HostShaderBlob {                                                   \
    g_##stem##_spirv, sizeof(g_##stem##_spirv), g_##stem##_dxil, sizeof(g_##stem##_dxil) \
  }
#else
#define RENUT_HOST_SHADER(stem)                                                    \
  ::renut::gpu::HostShaderBlob { g_##stem##_spirv, sizeof(g_##stem##_spirv), nullptr, 0 }
#endif
