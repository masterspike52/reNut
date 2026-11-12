#include "renut_engine/gpu/backend/backend.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <rex/setting.h>

#include <plume_vulkan.h>
#if defined(_WIN32)
#include <plume_d3d12.h>
#endif

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/gpu/backend/command_recorder.h"

namespace {
struct BackendSettings final : rex::SettingOwner {
  BackendSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(std::string, renut_gpu_backend, "d3d12",
              "Renderer backend: d3d12 or vulkan. Takes effect after a restart. "
              "Non-Windows builds always use vulkan.",
              rex::Allowed("d3d12", "vulkan"), rex::kRestart);
  REX_SETTING(bool, renut_gpu_dred, false,
              "D3D12 device removed diagnostics (DRED breadcrumbs and page faults) for "
              "debugging GPU crashes. Costs CPU and GPU time on every command. Takes "
              "effect after a restart.",
              rex::kRestart);
  REX_SETTING(bool, renut_vulkan_debug_layers, false,
              "Vulkan validation and crash diagnostic layers (needs the Vulkan SDK): API "
              "misuse goes to logs/vulkan_validation.log, and a lost device writes what the "
              "GPU was executing to logs/vulkan_crash. Very slow. Takes effect after a "
              "restart.",
              rex::kRestart);
};
BackendSettings &g_backend_settings = *new BackendSettings();  // never freed, registered by RenutApp::OnConfigure
} // namespace

namespace plume {
#if defined(PLUME_SDL_VULKAN_ENABLED)
std::unique_ptr<RenderInterface> CreateVulkanInterface(RenderWindow sdlWindow);
#else
std::unique_ptr<RenderInterface> CreateVulkanInterface();
#endif
#if defined(_WIN32)
std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
} // namespace plume

namespace renut::gpu {

namespace {

Backend ResolveBackend() {
#if defined(_WIN32)
  return g_backend_settings.renut_gpu_backend.Get() == "vulkan" ? Backend::Vulkan : Backend::D3D12;
#else
  return Backend::Vulkan;
#endif
}

#if defined(_WIN32)

// DRED must be switched on before the device exists (a debugger reads what
// it records).
void EnableDeviceRemovedDiagnostics() {
  ID3D12DeviceRemovedExtendedDataSettings *settings = nullptr;
  if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))) || !settings)
    return;
  settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->Release();
}

#endif

// Vulkan pipeline creation goes through CreateGraphicsPipelines, which makes the
// blend constant dynamic state in pipelines whose blend factors read it.
PFN_vkCreateGraphicsPipelines g_create_graphics_pipelines = nullptr;

bool ReadsBlendConstant(VkBlendFactor factor) {
  return factor >= VK_BLEND_FACTOR_CONSTANT_COLOR &&
         factor <= VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
}

bool ReadsBlendConstant(const VkGraphicsPipelineCreateInfo &info) {
  const VkPipelineColorBlendStateCreateInfo *blend = info.pColorBlendState;
  if (!blend || !blend->pAttachments)
    return false;
  for (uint32_t i = 0; i < blend->attachmentCount; ++i) {
    const VkPipelineColorBlendAttachmentState &a = blend->pAttachments[i];
    if (a.blendEnable &&
        (ReadsBlendConstant(a.srcColorBlendFactor) || ReadsBlendConstant(a.dstColorBlendFactor) ||
         ReadsBlendConstant(a.srcAlphaBlendFactor) || ReadsBlendConstant(a.dstAlphaBlendFactor))) {
      return true;
    }
  }
  return false;
}

// Any thread (the pipeline prebuild workers create pipelines too).
VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache,
                                                       uint32_t count,
                                                       const VkGraphicsPipelineCreateInfo *infos,
                                                       const VkAllocationCallbacks *allocator,
                                                       VkPipeline *pipelines) {
  if (!std::any_of(infos, infos + count,
                   [](const VkGraphicsPipelineCreateInfo &info) { return ReadsBlendConstant(info); })) {
    return g_create_graphics_pipelines(device, cache, count, infos, allocator, pipelines);
  }
  std::vector<VkGraphicsPipelineCreateInfo> patched(infos, infos + count);
  std::vector<std::vector<VkDynamicState>> states(count);
  std::vector<VkPipelineDynamicStateCreateInfo> dynamic(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (!ReadsBlendConstant(patched[i]))
      continue;
    if (const VkPipelineDynamicStateCreateInfo *old = patched[i].pDynamicState) {
      dynamic[i] = *old;
      states[i].assign(old->pDynamicStates, old->pDynamicStates + old->dynamicStateCount);
    } else {
      dynamic[i] = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    }
    if (std::find(states[i].begin(), states[i].end(), VK_DYNAMIC_STATE_BLEND_CONSTANTS) ==
        states[i].end()) {
      states[i].push_back(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
    }
    dynamic[i].dynamicStateCount = uint32_t(states[i].size());
    dynamic[i].pDynamicStates = states[i].data();
    patched[i].pDynamicState = &dynamic[i];
  }
  return g_create_graphics_pipelines(device, cache, count, patched.data(), allocator, pipelines);
}

void SetEnv(const char *name, const std::string &value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

// Enables the validation and crash diagnostic layers and their settings through
// environment variables, read by the Vulkan loader at instance creation.
void EnableVulkanDebugLayers() {
  std::error_code ec;
  const std::filesystem::path logs = std::filesystem::absolute("logs", ec);
  std::filesystem::create_directories(logs / "vulkan_crash", ec);
#if defined(_WIN32)
  constexpr const char *kSeparator = ";";
#else
  constexpr const char *kSeparator = ":";
#endif
  SetEnv("VK_INSTANCE_LAYERS", std::string("VK_LAYER_KHRONOS_validation") + kSeparator +
                                   "VK_LAYER_LUNARG_crash_diagnostic");
  SetEnv("VK_LOADER_LAYERS_ENABLE", "*validation,*crash_diagnostic");
  SetEnv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_LOG_MSG");
  SetEnv("VK_KHRONOS_VALIDATION_REPORT_FLAGS", "error,warn");
  SetEnv("VK_KHRONOS_VALIDATION_LOG_FILENAME", (logs / "vulkan_validation.log").string());
  // Crash diagnostic layer (its manifest's names): where the GPU stopped, and
  // semaphores too (a GPU hang waiting on one never signalled).
  SetEnv("CDL_OUTPUT_PATH", (logs / "vulkan_crash").string());
  SetEnv("CDL_TRACK_SEMAPHORES", "1");
  SetEnv("CDL_DUMP_QUEUE_SUBMITS", "all");
  SetEnv("CDL_LOG_FILE", (logs / "vulkan_crash" / "cdl.log").string());
}

// After plume loaded the Vulkan functions (plume::CreateVulkanInterface).
void RouteVulkanPipelineCreation() {
  if (!vkCreateGraphicsPipelines || vkCreateGraphicsPipelines == CreateGraphicsPipelines)
    return;
  g_create_graphics_pipelines = vkCreateGraphicsPipelines;
  vkCreateGraphicsPipelines = CreateGraphicsPipelines;
}

} // namespace

Backend ActiveBackend() {
  static const Backend backend = ResolveBackend();
  return backend;
}

const char *BackendName(Backend backend) {
  return backend == Backend::Vulkan ? "Vulkan" : "D3D12";
}

plume::RenderShaderFormat HostShaderFormat() {
  return IsVulkan() ? plume::RenderShaderFormat::SPIRV : plume::RenderShaderFormat::DXIL;
}

std::unique_ptr<plume::RenderInterface> CreateRenderInterface([[maybe_unused]] plume::RenderWindow window) {
#if defined(_WIN32)
  if (!IsVulkan()) {
    if (g_backend_settings.renut_gpu_dred.Get())
      EnableDeviceRemovedDiagnostics();
    return plume::CreateD3D12Interface();
  }
#endif
  if (g_backend_settings.renut_vulkan_debug_layers.Get())
    EnableVulkanDebugLayers();
#if defined(PLUME_SDL_VULKAN_ENABLED)
  auto render_interface = plume::CreateVulkanInterface(window);
#else
  auto render_interface = plume::CreateVulkanInterface();
#endif
  if (render_interface)
    RouteVulkanPipelineCreation();
  return render_interface;
}

plume::RenderBufferFlags AddressableBufferFlags() {
  return plume::RenderBufferFlag::DEVICE_ADDRESSABLE |
         (IsVulkan() ? plume::RenderBufferFlags(plume::RenderBufferFlag::CONSTANT) : 0);
}

plume::RenderBufferFlags StorageBufferFlags() {
  return plume::RenderBufferFlag::UNORDERED_ACCESS |
         (IsVulkan() ? plume::RenderBufferFlags(plume::RenderBufferFlag::STORAGE) : 0);
}

bool TextureCreated(const plume::RenderTexture *texture) {
  if (!texture)
    return false;
#if defined(_WIN32)
  if (!IsVulkan())
    return static_cast<const plume::D3D12Texture *>(texture)->d3d != nullptr;
#endif
  return static_cast<const plume::VulkanTexture *>(texture)->vk != VK_NULL_HANDLE;
}

bool BufferCreated(const plume::RenderBuffer *buffer) {
  if (!buffer)
    return false;
#if defined(_WIN32)
  if (!IsVulkan())
    return static_cast<const plume::D3D12Buffer *>(buffer)->d3d != nullptr;
#endif
  return static_cast<const plume::VulkanBuffer *>(buffer)->vk != VK_NULL_HANDLE;
}

namespace {

// Vulkan has no device-removed query, so a never-submitted fence's status
// reports a lost device.
bool VulkanDeviceLost(plume::RenderDevice *device) {
  VkDevice vk = static_cast<plume::VulkanDevice *>(device)->vk;
  if (!vk)
    return false;
  // Never submitted: its status is VK_NOT_READY until the device is lost.
  static const VkFence probe = [vk] {
    VkFenceCreateInfo info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    return vkCreateFence(vk, &info, nullptr, &fence) == VK_SUCCESS ? fence : VK_NULL_HANDLE;
  }();
  return probe && vkGetFenceStatus(vk, probe) == VK_ERROR_DEVICE_LOST;
}

} // namespace

bool ReportDeviceLost(plume::RenderDevice *device) {
  static std::atomic<bool> lost{false};
  if (lost.load(std::memory_order_acquire))
    return true;
  if (!device)
    return false;
  // The GPU faulted or hung executing an earlier frame: nothing is drawn from
  // here on.
  if (IsVulkan()) {
    if (!VulkanDeviceLost(device) || lost.exchange(true))
      return lost.load();
    return true;
  }
#if defined(_WIN32)
  ID3D12Device *d3d = static_cast<plume::D3D12Device *>(device)->d3d;
  if (!d3d)
    return false;
  if (SUCCEEDED(d3d->GetDeviceRemovedReason()) || lost.exchange(true))
    return lost.load();
  return true;
#else
  return false;
#endif
}

GpuMemoryUsage QueryGpuMemory(plume::RenderDevice *device) {
  GpuMemoryUsage usage;
  if (!device)
    return usage;
#if defined(_WIN32)
  if (!IsVulkan()) {
    IDXGIAdapter1 *adapter = static_cast<plume::D3D12Device *>(device)->adapter;
    IDXGIAdapter3 *adapter3 = nullptr;
    if (!adapter || FAILED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))) || !adapter3)
      return usage;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
    usage.valid =
        SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
        SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared));
    adapter3->Release();
    usage.local_used = local.CurrentUsage;
    usage.local_budget = local.Budget;
    usage.shared_used = shared.CurrentUsage;
    usage.shared_budget = shared.Budget;
    return usage;
  }
#endif
  VmaAllocator allocator = static_cast<plume::VulkanDevice *>(device)->allocator;
  if (!allocator)
    return usage;
  const VkPhysicalDeviceMemoryProperties *properties = nullptr;
  vmaGetMemoryProperties(allocator, &properties);
  if (!properties)
    return usage;
  VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
  vmaGetHeapBudgets(allocator, budgets);
  for (uint32_t i = 0; i < properties->memoryHeapCount; ++i) {
    const bool local = properties->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    (local ? usage.local_used : usage.shared_used) += budgets[i].usage;
    (local ? usage.local_budget : usage.shared_budget) += budgets[i].budget;
  }
  usage.valid = true;
  return usage;
}

namespace {

// Whether the backend list can have the blend constant set.
bool BlendConstantSettable(plume::RenderCommandList *cmd) {
#if defined(_WIN32)
  if (!IsVulkan())
    return static_cast<plume::D3D12CommandList *>(cmd)->d3d != nullptr;
#endif
  return static_cast<plume::VulkanCommandList *>(cmd)->vk && vkCmdSetBlendConstants;
}

} // namespace

bool SetBlendConstant(plume::RenderCommandList *cmd, const float rgba[4]) {
  // A recorder makes the call on its backend list, through here.
  if (CommandRecorder *recorder = CommandRecorder::From(cmd)) {
    if (!BlendConstantSettable(recorder->Target()))
      return false;
    recorder->RecordBlendConstant(rgba);
    return true;
  }
#if defined(_WIN32)
  if (!IsVulkan()) {
    ID3D12GraphicsCommandList *d3d = static_cast<plume::D3D12CommandList *>(cmd)->d3d;
    if (!d3d)
      return false;
    d3d->OMSetBlendFactor(rgba);
    return true;
  }
#endif
  // Pipelines that read the constant have it as dynamic state
  // (CreateGraphicsPipelines).
  VkCommandBuffer vk = static_cast<plume::VulkanCommandList *>(cmd)->vk;
  if (!vk || !vkCmdSetBlendConstants)
    return false;
  vkCmdSetBlendConstants(vk, rgba);
  return true;
}

void SetStencilReference(plume::RenderCommandList *cmd, uint32_t reference) {
  if (CommandRecorder *recorder = CommandRecorder::From(cmd)) {
    recorder->RecordStencilReference(reference);
    return;
  }
#if defined(_WIN32)
  if (!IsVulkan()) {
    if (ID3D12GraphicsCommandList *d3d = static_cast<plume::D3D12CommandList *>(cmd)->d3d)
      d3d->OMSetStencilRef(reference);
  }
#else
  (void)cmd;
  (void)reference;
#endif
}

bool GraphicsPipelineCreated(const plume::RenderPipeline *pipeline) {
  if (!pipeline)
    return false;
#if defined(_WIN32)
  if (!IsVulkan())
    return static_cast<const plume::D3D12GraphicsPipeline *>(pipeline)->d3d != nullptr;
#endif
  return static_cast<const plume::VulkanGraphicsPipeline *>(pipeline)->vk != VK_NULL_HANDLE;
}

std::unique_ptr<plume::RenderShader> CreateHostShader(plume::RenderDevice *device,
                                                      const HostShaderBlob &blob,
                                                      const char *name) {
  const bool vulkan = IsVulkan();
  const void *data = vulkan ? static_cast<const void *>(blob.spirv) : blob.dxil;
  const size_t size = vulkan ? blob.spirv_size : blob.dxil_size;
  if (!device || !data || size == 0)
    return nullptr;
  auto shader = device->createShader(data, size, "main", HostShaderFormat());
  if (shader)
    shader->setName(name);
  return shader;
}

} // namespace renut::gpu
