#pragma once

#include <cstdint>
#include <memory>

#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>

#include "renut_engine/nativevk/command_queue.h"

namespace renut::nativevk {

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void Execute(const Command& command);

private:
    void EnsureInitialized();
    void EnsurePresenterAttached();
    void PresentFrame(const Command& command);

    static constexpr uint64_t kPresentEveryNClears = 20;

    bool initialized_ = false;
    bool init_failed_ = false;

    rex::ui::vulkan::VulkanDevice* device_ = nullptr;

    std::unique_ptr<rex::ui::Presenter> presenter_;
    bool presenter_attach_attempted_ = false;

    VkImage clear_image_ = VK_NULL_HANDLE;
    VkDeviceMemory clear_image_memory_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    uint64_t clears_ = 0;
    uint64_t presents_ = 0;
};

}  // namespace renut::nativevk
