#pragma once
// Bookkeeping of the game's swapchains and the per-image Vulkan objects the console draws with.
#include <optional>
#include <vector>

#include "vulkan/vulkan.h"

namespace emf::core {

struct SwapchainRecord {
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t min_images = 2;
    bool retired = false;   // replaced by a newer swapchain (oldSwapchain), not destroyed yet
    bool drawable = true;   // its images allow COLOR_ATTACHMENT (we add that usage when the driver lets us)
    // Our objects (created on first use by vk_overlay):
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkCommandBuffer> commands;
    std::vector<VkFence> fences;
    std::vector<VkSemaphore> done;  // signalled by our draw, waited on by the present
    VkRenderPass render_pass = VK_NULL_HANDLE;
    bool ready = false;
};

class SwapchainBook {
public:
    SwapchainRecord& on_create(VkSwapchainKHR handle, VkFormat format, VkExtent2D extent, uint32_t min_images,
                               VkSwapchainKHR old_swapchain);
    SwapchainRecord* find(VkSwapchainKHR handle);
    std::optional<SwapchainRecord> take(VkSwapchainKHR handle);  // on destroy: the record, to free our objects
    size_t size() const { return records_.size(); }
    std::vector<SwapchainRecord>& all() { return records_; }

private:
    std::vector<SwapchainRecord> records_;
};

}  // namespace emf::core
