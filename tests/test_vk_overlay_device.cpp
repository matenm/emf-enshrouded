// The overlay's device path with a fake Vulkan device below it (the functions the layer would get from the next
// layer). A fault while preparing the drawing objects turns the overlay off, still presents the frame, and leaves no
// lock held: destroying the swapchain afterwards (the game's window thread on a resize) must not hang.
#include <windows.h>

#include <doctest.h>

#include <atomic>
#include <cstring>
#include <thread>

#include "core/overlay/vk_overlay.h"

using namespace emf::core;

namespace {
struct FakeDispatchable {
    void* loader_table;
};
FakeDispatchable g_inst{nullptr}, g_phys{nullptr}, g_dev{nullptr}, g_queue{nullptr};
std::atomic<int> g_presents{0}, g_swapchains_destroyed{0}, g_image_views{0};

template <typename T>
T handle(uintptr_t v) {
    return reinterpret_cast<T>(v);
}

// Any other device function: succeeds and does nothing (x64: extra arguments are ignored by the callee).
VkResult VKAPI_CALL ok_any() { return VK_SUCCESS; }

void VKAPI_CALL queue_families(VkPhysicalDevice, uint32_t* n, VkQueueFamilyProperties* p) {
    if (p) {
        *p = VkQueueFamilyProperties{};
        p->queueFlags = VK_QUEUE_GRAPHICS_BIT;
        p->queueCount = 1;
    }
    *n = 1;
}
VkResult VKAPI_CALL create_swapchain(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*,
                                     VkSwapchainKHR* out) {
    *out = handle<VkSwapchainKHR>(0x5C);
    return VK_SUCCESS;
}
void VKAPI_CALL destroy_swapchain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) { ++g_swapchains_destroyed; }
VkResult VKAPI_CALL present(VkQueue, const VkPresentInfoKHR*) {
    ++g_presents;
    return VK_SUCCESS;
}
VkResult VKAPI_CALL swapchain_images(VkDevice, VkSwapchainKHR, uint32_t* n, VkImage* out) {
    if (out) {
        out[0] = handle<VkImage>(0x11);
        out[1] = handle<VkImage>(0x12);
    }
    *n = 2;
    return VK_SUCCESS;
}
VkResult VKAPI_CALL create_render_pass(VkDevice, const VkRenderPassCreateInfo*, const VkAllocationCallbacks*,
                                       VkRenderPass* out) {
    *out = handle<VkRenderPass>(0x22);
    return VK_SUCCESS;
}
VkResult VKAPI_CALL create_command_pool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*,
                                        VkCommandPool* out) {
    *out = handle<VkCommandPool>(0x33);
    return VK_SUCCESS;
}
VkResult VKAPI_CALL faulting_image_view(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*,
                                        VkImageView*) {
    ++g_image_views;
    *static_cast<volatile int*>(nullptr) = 0;  // a driver crash
    return VK_SUCCESS;
}

PFN_vkVoidFunction VKAPI_CALL fake_gdpa(VkDevice, const char* name) {
    struct {
        const char* name;
        PFN_vkVoidFunction fn;
    } const table[] = {
        {"vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(&create_swapchain)},
        {"vkDestroySwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(&destroy_swapchain)},
        {"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(&present)},
        {"vkGetSwapchainImagesKHR", reinterpret_cast<PFN_vkVoidFunction>(&swapchain_images)},
        {"vkCreateRenderPass", reinterpret_cast<PFN_vkVoidFunction>(&create_render_pass)},
        {"vkCreateCommandPool", reinterpret_cast<PFN_vkVoidFunction>(&create_command_pool)},
        {"vkCreateImageView", reinterpret_cast<PFN_vkVoidFunction>(&faulting_image_view)},
    };
    for (const auto& e : table)
        if (!std::strcmp(name, e.name)) return e.fn;
    return reinterpret_cast<PFN_vkVoidFunction>(&ok_any);
}
PFN_vkVoidFunction VKAPI_CALL fake_gipa(VkInstance, const char* name) {
    if (!std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties"))
        return reinterpret_cast<PFN_vkVoidFunction>(&queue_families);
    return nullptr;
}
}  // namespace

TEST_CASE("vk overlay: a driver fault while preparing to draw still presents, turns the overlay off, holds no lock") {
    std::atomic<int> turned_off{0};
    VkOverlayHooks hooks;
    hooks.visible = [] { return true; };
    hooks.turned_off = [&turned_off] { ++turned_off; };
    set_vulkan_overlay(std::move(hooks));

    LayerDevice d;
    d.instance = reinterpret_cast<VkInstance>(&g_inst);
    d.gipa = fake_gipa;
    d.phys = reinterpret_cast<VkPhysicalDevice>(&g_phys);
    d.device = reinterpret_cast<VkDevice>(&g_dev);
    d.gdpa = fake_gdpa;
    REQUIRE(overlay_attach_device(d));
    const auto queue = reinterpret_cast<VkQueue>(&g_queue);
    overlay_remember_queue(queue, 0);

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    ci.imageExtent = {640, 360};
    ci.minImageCount = 2;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    REQUIRE(overlay_create_swapchain(d.device, &ci, nullptr, &sc) == VK_SUCCESS);

    const uint32_t index = 0;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &index;
    CHECK(overlay_present(queue, &pi) == VK_SUCCESS);
    CHECK(g_image_views.load() == 1);  // the fault happened
    CHECK(g_presents.load() == 1);     // the frame was still presented
    CHECK(turned_off.load() == 1);
    CHECK_FALSE(vulkan_overlay_ready());

    std::atomic<bool> done{false};
    std::thread window_thread([&] {
        overlay_destroy_swapchain(d.device, sc, nullptr);
        done = true;
    });
    for (int i = 0; i < 200 && !done; ++i) Sleep(10);
    CHECK(done.load());  // a lock left held by the fault would block here forever
    if (done) {
        window_thread.join();
        CHECK(g_swapchains_destroyed.load() == 1);
        overlay_detach_device(d.device);
    } else {
        window_thread.detach();  // stuck on the lock; the process exits with it
    }
}
