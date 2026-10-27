#pragma once
// Draws Dear ImGui into the game's frames. The game's Vulkan calls reach this file through EMF's Vulkan layer
// (vk_layer.cpp): it learns the instance and the device, tracks the swapchains, and on each present renders a small
// command buffer on top of the image while the console, a note, a wheel or mod HUD lines are visible.
#include <atomic>
#include <functional>
#include <string>

#include "imgui.h"
#include "vulkan/vulkan.h"
#include "vulkan/vk_layer.h"

namespace emf::core {

using PresentFn = VkResult(VKAPI_PTR*)(VkQueue, const VkPresentInfoKHR*);
// Draws and submits; on a submit the present must wait for, sets *signal. Returns false on a failure.
using RenderFn = bool (*)(VkQueue queue, const VkPresentInfoKHR* info, VkSemaphore* signal, void* user);

// The present path: not visible (or disabled) -> straight to `original`. Visible -> `render` under a fault guard, then
// `original`, waiting on render's semaphore if it submitted. A fault or false disables the overlay (*disabled).
VkResult present_through(VkQueue queue, const VkPresentInfoKHR* info, bool visible, std::atomic<bool>* disabled,
                         RenderFn render, void* user, PresentFn original);

struct VkOverlayHooks {
    std::function<bool()> visible;                                  // draw this frame?
    std::function<void(ImGuiIO&, float display_height)> fonts;      // add fonts (once, before the backend starts)
    std::function<void(ImGuiIO&)> before_frame;                     // feed input into ImGui
    std::function<void(ImVec2 display)> build;                      // ImGui calls between NewFrame and Render
    std::function<void()> turned_off;                               // drawing failed: the overlay is off for good
};

// Where the overlay's log lines go (lines from before this call are kept and written then).
void set_vulkan_log(std::function<void(const char*)> log);
// Connects the console (set once; until then frames pass through untouched).
void set_vulkan_overlay(VkOverlayHooks hooks);
// True once the game's device is known, its frames pass through the layer and nothing has failed.
bool vulkan_overlay_ready();
// Why the console cannot be shown (for the log), when vulkan_overlay_ready() is false.
const char* vulkan_overlay_ready_reason();
// Frames through the layer, for the debug bridge's state.
struct VulkanFrameStats {
    bool device = false;  // the game's device came through the layer
    bool recent = false;  // a frame within the last 2 s
    uint64_t presents = 0;
    double frame_ms = 0;  // smoothed time between presents
};
VulkanFrameStats vulkan_frame_stats();

// ---- called by the layer (vk_layer.cpp) ----
// A device the game created, with what the layer learned about it: the next layer's GetProcAddr functions (EMF's own
// Vulkan calls go there, below EMF) and the loader's callback for stamping dispatchable objects EMF creates.
struct LayerDevice {
    VkInstance instance = VK_NULL_HANDLE;
    uint32_t api_version = VK_API_VERSION_1_0;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkSetDeviceLoaderData set_loader_data = nullptr;
};
// The first device created with VK_KHR_swapchain is the one the console draws on; true if it is this one.
bool overlay_attach_device(const LayerDevice& d);
// The device is about to be destroyed: free EMF's objects on it (a later device can then take over).
void overlay_detach_device(VkDevice device);
void overlay_remember_queue(VkQueue queue, uint32_t family);
void overlay_log(const std::string& line);
VkResult overlay_present(VkQueue queue, const VkPresentInfoKHR* info);
VkResult overlay_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* alloc,
                                  VkSwapchainKHR* out);
void overlay_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* alloc);

}  // namespace emf::core
