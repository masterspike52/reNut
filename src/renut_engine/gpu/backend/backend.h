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
// the game window, which Vulkan on Linux asks for surface instance extensions.
std::unique_ptr<plume::RenderInterface> CreateRenderInterface(plume::RenderWindow window);

// Per-backend buffer flags: plume's Vulkan buffers need CONSTANT for a device
// address and STORAGE for a storage binding, or shaders access garbage.
plume::RenderBufferFlags AddressableBufferFlags();  // read/written by device address
plume::RenderBufferFlags StorageBufferFlags();      // bound as a storage buffer / UAV

// plume returns a graphics pipeline object even when the backend rejected the
// description; true if the backend pipeline exists.
bool GraphicsPipelineCreated(const plume::RenderPipeline *pipeline);

// Same for textures and buffers.
bool TextureCreated(const plume::RenderTexture *texture);
bool BufferCreated(const plume::RenderBuffer *buffer);

// True once the device is lost (D3D12 device removed).
bool ReportDeviceLost(plume::RenderDevice *device);

// Video memory the process uses and its budgets: 'local' is dedicated GPU
// memory, 'shared' system memory the GPU reaches (from DXGI on D3D12, VMA on Vulkan).
struct GpuMemoryUsage {
  bool valid = false;
  uint64_t local_used = 0, local_budget = 0;
  uint64_t shared_used = 0, shared_budget = 0;
};
GpuMemoryUsage QueryGpuMemory(plume::RenderDevice *device);

// Sets the blend constant on the native command list (plume has no call for it).
// On Vulkan it lasts only until a pipeline that does not read it is bound.
bool SetBlendConstant(plume::RenderCommandList *cmd, const float rgba[4]);

// Sets the stencil reference for the rest of the D3D12 command list, which plume
// pipelines ignore; Vulkan pipelines have the reference built in.
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
