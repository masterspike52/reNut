#include "renut_engine/nativevk/renderer.h"

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

namespace renut::nativevk {

Renderer::~Renderer() {
    if (!initialized_) {
        return;
    }
    auto* device = device_;
    const auto& fn = device->functions();
    fn.vkWaitForFences(device->device(), 1, &fence_, VK_TRUE, UINT64_MAX);
    if (fence_) {
        fn.vkDestroyFence(device->device(), fence_, nullptr);
    }
    if (command_pool_) {
        fn.vkDestroyCommandPool(device->device(), command_pool_, nullptr);
    }
    if (clear_image_) {
        fn.vkDestroyImage(device->device(), clear_image_, nullptr);
    }
    if (clear_image_memory_) {
        fn.vkFreeMemory(device->device(), clear_image_memory_, nullptr);
    }
}

void Renderer::EnsureInitialized() {
    if (initialized_ || init_failed_) {
        return;
    }

    auto* graphics = rex::Runtime::instance()->graphics_system();
    auto* provider = graphics ? graphics->provider() : nullptr;
    if (!provider) {
        REXLOG_ERROR("nativevk: no live GraphicsProvider from the active GPU plugin yet");
        init_failed_ = true;
        return;
    }
    device_ = static_cast<rex::ui::vulkan::VulkanProvider*>(provider)->vulkan_device();
    if (!device_) {
        REXLOG_ERROR("nativevk: GraphicsProvider has no VulkanDevice");
        init_failed_ = true;
        return;
    }

    auto* device = device_;
    const auto& fn = device->functions();

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {64, 64, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (fn.vkCreateImage(device->device(), &image_info, nullptr, &clear_image_) != VK_SUCCESS) {
        REXLOG_ERROR("nativevk: vkCreateImage failed");
        init_failed_ = true;
        return;
    }

    VkMemoryRequirements mem_reqs;
    fn.vkGetImageMemoryRequirements(device->device(), clear_image_, &mem_reqs);

    uint32_t memory_type_bits = mem_reqs.memoryTypeBits & device->memory_types().device_local;
    if (!memory_type_bits) {
        memory_type_bits = mem_reqs.memoryTypeBits;
    }

    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = static_cast<uint32_t>(__builtin_ctz(memory_type_bits));

    if (fn.vkAllocateMemory(device->device(), &alloc_info, nullptr, &clear_image_memory_) !=
        VK_SUCCESS) {
        REXLOG_ERROR("nativevk: vkAllocateMemory failed");
        init_failed_ = true;
        return;
    }
    fn.vkBindImageMemory(device->device(), clear_image_, clear_image_memory_, 0);

    uint32_t queue_family = device->queue_family_graphics_compute();

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;
    if (fn.vkCreateCommandPool(device->device(), &pool_info, nullptr, &command_pool_) !=
        VK_SUCCESS) {
        REXLOG_ERROR("nativevk: vkCreateCommandPool failed");
        init_failed_ = true;
        return;
    }

    VkCommandBufferAllocateInfo cb_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb_alloc.commandPool = command_pool_;
    cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_alloc.commandBufferCount = 1;
    if (fn.vkAllocateCommandBuffers(device->device(), &cb_alloc, &command_buffer_) != VK_SUCCESS) {
        REXLOG_ERROR("nativevk: vkAllocateCommandBuffers failed");
        init_failed_ = true;
        return;
    }

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (fn.vkCreateFence(device->device(), &fence_info, nullptr, &fence_) != VK_SUCCESS) {
        REXLOG_ERROR("nativevk: vkCreateFence failed");
        init_failed_ = true;
        return;
    }

    initialized_ = true;
    REXLOG_INFO("nativevk: Phase 1 headless Vulkan bootstrap OK");
}

void Renderer::EnsurePresenterAttached() {
    if (presenter_attach_attempted_) {
        return;
    }
    presenter_attach_attempted_ = true;

    auto* graphics = rex::Runtime::instance()->graphics_system();
    auto* provider = graphics ? graphics->provider() : nullptr;
    auto* window = rex::Runtime::instance()->display_window();
    auto* app_context = rex::Runtime::instance()->app_context();
    if (!provider || !window || !app_context) {
        REXLOG_ERROR("nativevk: missing provider/window/app_context, cannot attach presenter");
        return;
    }

    app_context->CallInUIThreadSynchronous([this, provider, window]() {
        presenter_ = static_cast<rex::ui::vulkan::VulkanProvider*>(provider)->CreatePresenter(
            rex::ui::Presenter::FatalErrorHostGpuLossCallback);
        if (presenter_) {
            window->SetPresenter(presenter_.get());
        }
    });

    if (presenter_) {
        REXLOG_INFO("nativevk: window takeover complete, presenter attached");
    } else {
        REXLOG_ERROR("nativevk: failed to create/attach Presenter");
    }
}

void Renderer::Execute(const Command& command) {
    EnsureInitialized();
    if (!initialized_) {
        return;
    }

    auto* device = device_;
    const auto& fn = device->functions();

    if (command.type == CommandType::kClear) {
        fn.vkResetCommandPool(device->device(), command_pool_, 0);

        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        fn.vkBeginCommandBuffer(command_buffer_, &begin_info);

        VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_dst.srcAccessMask = 0;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = clear_image_;
        to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        fn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                &to_dst);

        VkClearColorValue clear_color;
        clear_color.float32[0] = command.clear_color[0];
        clear_color.float32[1] = command.clear_color[1];
        clear_color.float32[2] = command.clear_color[2];
        clear_color.float32[3] = command.clear_color[3];
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        fn.vkCmdClearColorImage(command_buffer_, clear_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &clear_color, 1, &range);

        fn.vkEndCommandBuffer(command_buffer_);

        fn.vkResetFences(device->device(), 1, &fence_);

        VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer_;

        auto queue = device->AcquireQueue(device->queue_family_graphics_compute(), 0);
        fn.vkQueueSubmit(queue.queue(), 1, &submit_info, fence_);
        fn.vkWaitForFences(device->device(), 1, &fence_, VK_TRUE, UINT64_MAX);

        ++clears_;
        if ((clears_ % 500) == 0) {
            REXLOG_INFO("nativevk: {} clears executed on render thread", clears_);
        }
    }
}

void Renderer::PresentFrame(const Command& command) {
    EnsurePresenterAttached();
    if (!presenter_) {
        return;
    }

    auto* device = device_;
    const auto& fn = device->functions();

    auto refresher = [this, &command, device, &fn](
                         rex::ui::Presenter::GuestOutputRefreshContext& ctx) {
        auto& vkctx =
            static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(ctx);
        const bool written_before = vkctx.image_ever_written_previously();

        fn.vkResetCommandPool(device->device(), command_pool_, 0);

        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        fn.vkBeginCommandBuffer(command_buffer_, &begin_info);

        VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_dst.srcAccessMask = written_before ? VK_ACCESS_SHADER_READ_BIT : 0;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_dst.oldLayout = written_before ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                          : VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = vkctx.image();
        to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        fn.vkCmdPipelineBarrier(
            command_buffer_,
            written_before ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                           : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

        VkClearColorValue clear_color;
        clear_color.float32[0] = command.clear_color[0];
        clear_color.float32[1] = command.clear_color[1];
        clear_color.float32[2] = command.clear_color[2];
        clear_color.float32[3] = command.clear_color[3];
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        fn.vkCmdClearColorImage(command_buffer_, vkctx.image(),
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1, &range);

        VkImageMemoryBarrier to_read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.image = vkctx.image();
        to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        fn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                1, &to_read);

        fn.vkEndCommandBuffer(command_buffer_);

        fn.vkResetFences(device->device(), 1, &fence_);
        VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer_;
        auto submit_queue = device->AcquireQueue(device->queue_family_graphics_compute(), 0);
        fn.vkQueueSubmit(submit_queue.queue(), 1, &submit_info, fence_);
        fn.vkWaitForFences(device->device(), 1, &fence_, VK_TRUE, UINT64_MAX);

        return true;
    };
    presenter_->RefreshGuestOutput(1280, 720, 16, 9, refresher);

    ++presents_;
    if ((presents_ % 500) == 0) {
        REXLOG_INFO("nativevk: {} present frames submitted", presents_);
    }
}

}  // namespace renut::nativevk
