/**
 * @file    gpu/backend/backend.cpp
 */
#include "renut_engine/gpu/backend/backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <rex/cvar.h>

#include <plume_vulkan.h>
#if defined(_WIN32)
#include <plume_d3d12.h>
#endif

#include "renut_engine/gpu/backend/command_recorder.h"
#include "renut_engine/gpu/log.h"
#if defined(__ANDROID__)
#include "renut_engine/app/android_driver.h"
#endif

REXCVAR_DEFINE_STRING(renut_gpu_backend, "d3d12", "Nuts&Bolts/Graphics",
                      "Renderer backend: d3d12 or vulkan. Takes effect after a restart. "
                      "Non-Windows builds always use vulkan.")
    .allowed({"d3d12", "vulkan"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(renut_gpu_dred, false, "Nuts&Bolts/Graphics",
                    "D3D12 device removed diagnostics (DRED breadcrumbs and page faults) for "
                    "debugging GPU crashes. Costs CPU and GPU time on every command. Takes "
                    "effect after a restart.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(renut_vulkan_debug_layers, false, "Nuts&Bolts/Graphics",
                    "Vulkan validation and crash diagnostic layers (needs the Vulkan SDK): API "
                    "misuse goes to logs/vulkan_validation.log, and a lost device writes what the "
                    "GPU was executing to logs/vulkan_crash. Very slow. Takes effect after a "
                    "restart.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

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
  return REXCVAR_GET(renut_gpu_backend) == "vulkan" ? Backend::Vulkan : Backend::D3D12;
#else
  return Backend::Vulkan;
#endif
}

#if defined(_WIN32)

// DRED must be switched on before the device exists.
void EnableDeviceRemovedDiagnostics() {
  ID3D12DeviceRemovedExtendedDataSettings *settings = nullptr;
  if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))) || !settings) {
    RGPU_WARN("D3D12 device removed diagnostics (DRED) unavailable");
    return;
  }
  settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->Release();
}

const char *BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op) {
  switch (op) {
  case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:
    return "DrawInstanced";
  case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:
    return "DrawIndexedInstanced";
  case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:
    return "CopyBufferRegion";
  case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:
    return "CopyTextureRegion";
  case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:
    return "ResolveSubresource";
  case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCEREGION:
    return "ResolveSubresourceRegion";
  case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:
    return "ClearRenderTargetView";
  case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:
    return "ClearDepthStencilView";
  case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:
    return "ResourceBarrier";
  case D3D12_AUTO_BREADCRUMB_OP_PRESENT:
    return "Present";
  default:
    return "other";
  }
}

void LogAllocations(const char *what, const D3D12_DRED_ALLOCATION_NODE *node) {
  for (int n = 0; node && n < 16; node = node->pNext, ++n) {
    RGPU_ERROR("DRED:   {} allocation '{}' (type {})", what,
               node->ObjectNameA ? node->ObjectNameA : "", uint32_t(node->AllocationType));
  }
}

// Where the GPU stopped: each command list's breadcrumb trail up to the last
// operation it completed, and the allocation behind a page fault.
void LogDeviceRemovedData(ID3D12Device *d3d) {
  ID3D12DeviceRemovedExtendedData *dred = nullptr;
  if (FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dred))) || !dred) {
    RGPU_ERROR("DRED: no data (set renut_gpu_dred = true to record where the GPU stopped)");
    return;
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
    for (const D3D12_AUTO_BREADCRUMB_NODE *node = breadcrumbs.pHeadAutoBreadcrumbNode; node;
         node = node->pNext) {
      const UINT count = node->BreadcrumbCount;
      const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
      if (done >= count)
        continue;  // finished
      UINT draws = 0;
      for (UINT i = 0; i < done; ++i) {
        const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[i];
        draws += op == D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED ||
                 op == D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED;
      }
      const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[done];
      RGPU_ERROR("DRED: command list '{}' stopped at operation {} of {}: {} ({}), after {} draws",
                 node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "", done, count,
                 BreadcrumbOpName(op), uint32_t(op), draws);
    }
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA) {
    RGPU_ERROR("DRED: page fault at GPU address {:016X}", uint64_t(fault.PageFaultVA));
    LogAllocations("existing", fault.pHeadExistingAllocationNode);
    LogAllocations("recently freed", fault.pHeadRecentFreedAllocationNode);
  }
  dred->Release();
}

#endif

// Vulkan blend constants. plume's pipelines keep the constant static (zero)
// and it has no call to set it, so pipeline creation goes through
// CreateGraphicsPipelines, which makes the constant dynamic state in pipelines
// whose blend factors read it, and SetBlendConstant records it. Both reach
// Vulkan through volk's function pointers, which plume calls as well.
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

#if defined(__ANDROID__)
// Android's Vulkan drivers keep no disk shader cache for apps, so every start
// compiled every pipeline again: a pipeline
// cache kept in pipeline_cache.bin (next to the pipeline list) holds the
// compiled shaders. Saved from whichever thread created pipelines, at most
// every 10 s (and when Android sends the app to the background); the game is
// killed, not shut down.
std::string PipelineCacheFile() {
#if defined(__ANDROID__)
  // One per driver: a cache only loads into the driver that wrote it.
  const std::string &driver = renut::android::LoadedDriver();
  if (!driver.empty())
    return "pipeline_cache_" + driver + ".bin";
#endif
  return "pipeline_cache.bin";
}
std::string g_cache_file;  // named once the driver is loaded
VkDevice g_cache_device = VK_NULL_HANDLE;
std::mutex g_cache_mutex;
VkPipelineCache g_cache = VK_NULL_HANDLE;
uint32_t g_cache_unsaved = 0;
std::chrono::steady_clock::time_point g_cache_saved;

VkPipelineCache SharedPipelineCache(VkDevice device) {
  std::lock_guard lock(g_cache_mutex);
  if (g_cache == VK_NULL_HANDLE) {
    std::vector<char> data;
    g_cache_device = device;
    g_cache_file = PipelineCacheFile();
    if (std::ifstream in{g_cache_file, std::ios::binary})
      data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.data();
    if (vkCreatePipelineCache(device, &info, nullptr, &g_cache) != VK_SUCCESS) {
      info.initialDataSize = 0;  // stale or from another driver
      vkCreatePipelineCache(device, &info, nullptr, &g_cache);
    }
    g_cache_saved = std::chrono::steady_clock::now();
    RGPU_INFO("Pipeline cache: {} KB loaded from {}", data.size() >> 10, g_cache_file);
  }
  return g_cache;
}

void SavePipelineCache(VkDevice device, uint32_t created, bool now_please = false) {
  std::lock_guard lock(g_cache_mutex);
  g_cache_unsaved += created;
  const auto now = std::chrono::steady_clock::now();
  if (!g_cache_unsaved || g_cache == VK_NULL_HANDLE ||
      (!now_please && now - g_cache_saved < std::chrono::seconds(10)))
    return;
  size_t size = 0;
  vkGetPipelineCacheData(device, g_cache, &size, nullptr);
  std::vector<char> data(size);
  if (vkGetPipelineCacheData(device, g_cache, &size, data.data()) != VK_SUCCESS)
    return;
  const std::string tmp = g_cache_file + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(size));
    if (!out)
      return;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, g_cache_file, ec);
  if (ec) {  // filesystems that do not rename over an existing file
    std::filesystem::remove(g_cache_file, ec);
    std::filesystem::rename(tmp, g_cache_file, ec);
  }
  g_cache_unsaved = 0;
  g_cache_saved = now;
  RGPU_INFO("Pipeline cache: {} KB saved", size >> 10);
}
#endif

VkResult CreateGraphicsPipelinesImpl(VkDevice device, VkPipelineCache cache, uint32_t count,
                                     const VkGraphicsPipelineCreateInfo *infos,
                                     const VkAllocationCallbacks *allocator, VkPipeline *pipelines);

// Any thread (the pipeline prebuild workers create pipelines too).
VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache,
                                                       uint32_t count,
                                                       const VkGraphicsPipelineCreateInfo *infos,
                                                       const VkAllocationCallbacks *allocator,
                                                       VkPipeline *pipelines) {
#if defined(__ANDROID__)
  if (cache == VK_NULL_HANDLE) {
    const VkResult result =
        CreateGraphicsPipelinesImpl(device, SharedPipelineCache(device), count, infos, allocator,
                                    pipelines);
    if (result == VK_SUCCESS)
      SavePipelineCache(device, count);
    return result;
  }
#endif
  return CreateGraphicsPipelinesImpl(device, cache, count, infos, allocator, pipelines);
}

VkResult CreateGraphicsPipelinesImpl(VkDevice device, VkPipelineCache cache, uint32_t count,
                                     const VkGraphicsPipelineCreateInfo *infos,
                                     const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
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

// renut_vulkan_debug_layers: the Vulkan loader enables layers named in the
// environment when the instance is created, and the layers read their
// settings from it too (VK_<LAYER>_<SETTING>).
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
  RGPU_WARN("Vulkan debug layers requested (renut_vulkan_debug_layers): validation to {}, "
            "crash dumps to {}",
            (logs / "vulkan_validation.log").string(), (logs / "vulkan_crash").string());
}

// After plume loaded the Vulkan functions (plume::CreateVulkanInterface).
void RouteVulkanPipelineCreation() {
  if (!vkCreateGraphicsPipelines || vkCreateGraphicsPipelines == CreateGraphicsPipelines)
    return;
  g_create_graphics_pipelines = vkCreateGraphicsPipelines;
  vkCreateGraphicsPipelines = CreateGraphicsPipelines;
}

} // namespace

void FlushPipelineCache() {
#if defined(__ANDROID__)
  if (g_cache_device != VK_NULL_HANDLE)
    SavePipelineCache(g_cache_device, 0, true);
#endif
}

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
    if (REXCVAR_GET(renut_gpu_dred))
      EnableDeviceRemovedDiagnostics();
    return plume::CreateD3D12Interface();
  }
#endif
  if (REXCVAR_GET(renut_vulkan_debug_layers))
    EnableVulkanDebugLayers();
#if defined(__ANDROID__)
  // A custom driver the manager picked (Turnip...), before plume loads volk.
  renut::android::LoadVulkanDriver();
#endif
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

// Vulkan has no device-removed query: a fence's status reports a lost
// device. plume's own calls only print their errors to stderr, and after a
// loss acquire and present may go on succeeding, which leaves the screen on
// the last image presented while the game runs on.
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
  if (IsVulkan()) {
    if (!VulkanDeviceLost(device) || lost.exchange(true))
      return lost.load();
    RGPU_ERROR("Vulkan device lost: the GPU faulted or hung executing an earlier frame; nothing "
               "is drawn from here on (plume's messages are in the stderr log)");
    return true;
  }
#if defined(_WIN32)
  ID3D12Device *d3d = static_cast<plume::D3D12Device *>(device)->d3d;
  if (!d3d)
    return false;
  const HRESULT reason = d3d->GetDeviceRemovedReason();
  if (SUCCEEDED(reason) || lost.exchange(true))
    return lost.load();
  RGPU_ERROR("D3D12 device removed (reason {:08X})", uint32_t(reason));
  LogDeviceRemovedData(d3d);
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
  if (!device || !data || size == 0) {
    RGPU_ERROR("Host shader '{}' has no {} blob", name, BackendName(ActiveBackend()));
    return nullptr;
  }
  auto shader = device->createShader(data, size, "main", HostShaderFormat());
  if (!shader)
    RGPU_ERROR("createShader failed for host shader '{}'", name);
  else
    shader->setName(name);
  return shader;
}

} // namespace renut::gpu
