#include "swapchain_book.h"

#include <utility>

namespace emf::core {

SwapchainRecord& SwapchainBook::on_create(VkSwapchainKHR handle, VkFormat format, VkExtent2D extent,
                                          uint32_t min_images, VkSwapchainKHR old_swapchain) {
    if (old_swapchain)
        if (SwapchainRecord* old = find(old_swapchain)) old->retired = true;
    if (SwapchainRecord* again = find(handle)) *again = SwapchainRecord{};  // a handle value reused: start over
    SwapchainRecord r;
    r.handle = handle;
    r.format = format;
    r.extent = extent;
    r.min_images = min_images;
    if (SwapchainRecord* slot = find(handle)) {
        *slot = std::move(r);
        return *slot;
    }
    records_.push_back(std::move(r));
    return records_.back();
}

SwapchainRecord* SwapchainBook::find(VkSwapchainKHR handle) {
    if (!handle) return nullptr;
    for (auto& r : records_)
        if (r.handle == handle) return &r;
    return nullptr;
}

std::optional<SwapchainRecord> SwapchainBook::take(VkSwapchainKHR handle) {
    for (auto it = records_.begin(); it != records_.end(); ++it)
        if (it->handle == handle) {
            SwapchainRecord r = std::move(*it);
            records_.erase(it);
            return r;
        }
    return std::nullopt;
}

}  // namespace emf::core
