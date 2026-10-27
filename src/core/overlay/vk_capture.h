#pragma once
// The debug bridge's screenshot. When one is asked for, the next present copies the image it shows into a host-visible
// buffer: a small command buffer on the present queue waits for the game's work (and, unless the request said
// overlay:false, EMF's overlay), copies, and signals the semaphore the next step then waits on. The bridge's worker
// thread waits for the copy's fence, converts the pixels (8-bit, 10-bit, HDR10 and scRGB swapchains) and writes the PNG
// into emf\screens. The readback buffer is made when the screenshot is asked for (not on the present thread) and kept
// for the next one. Nothing is copied while no screenshot is asked for, and the frame never waits for the copy.
// Readable swapchains: the layer adds VK_IMAGE_USAGE_TRANSFER_SRC_BIT when the bridge is on and the surface allows it.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "core/bridge/pixels.h"
#include "vk_overlay.h"
#include "vulkan/vulkan.h"

namespace emf::core {

struct CaptureOptions {
    bool overlay = true;     // false: copy before EMF's overlay draws on the frame
    uint32_t max_width = 0;  // 0 = full size
};

struct CaptureResult {
    bool ok = false;
    std::string code;  // when not ok: busy, unavailable or timeout
    std::string error;
    std::wstring path;
    uint32_t width = 0, height = 0;
    bool hdr = false;  // an HDR swapchain, tone-mapped to sRGB
};
using CaptureDone = std::function<void(const CaptureResult&)>;

// How a swapchain format is read (8-bit, 10-bit or 16-bit float RGBA), or nullopt.
std::optional<bridge::PixelFormat> capture_pixel_format(VkFormat format);
// What the colour space says about the values (sRGB, HDR10 PQ, scRGB linear).
bridge::Transfer capture_transfer(VkColorSpaceKHR space);
// A format in words: "A2B10G10R10 UNORM (10-bit)", or "format 1000156000 (not an RGBA format EMF reads)".
std::string capture_format_words(VkFormat format);

// True when [debug] bridge=1 in the emf_core.ini next to this DLL (read once; the layer may see the swapchain before the
// core starts). Tests set it.
bool capture_wanted();
void capture_set_wanted(bool on);

// Asks for the next presented image as the PNG `path` (inside <emf_dir>\screens). "" when asked: done then comes once,
// from the worker thread (or the present thread for an early refusal). Otherwise why it cannot, and *code (busy or
// unavailable).
std::string capture_request(const std::wstring& emf_dir, const std::wstring& path, const CaptureOptions& options,
                            CaptureDone done, std::string* code = nullptr);
// Withdraws the request if no present took it yet (true); false once a present took it (done will come).
bool capture_cancel();
// Why a screenshot cannot be taken now, "" when it can.
std::string capture_unavailable();

// ---- called by the overlay's device and present path (vk_overlay.cpp) ----
void capture_attach(const LayerDevice& d);
void capture_detach(VkDevice device);
// Whether the surface lets swapchain images be copied from (supportedUsageFlags has TRANSFER_SRC).
bool capture_surface_readable(VkSurfaceKHR surface);
void capture_swapchain_created(VkSwapchainKHR swapchain, VkFormat format, VkColorSpaceKHR space, VkExtent2D extent,
                               bool readable);
void capture_swapchain_destroyed(VkSwapchainKHR swapchain);  // waits (1 s at most) for a copy from it in flight

using QueueFamilyFn = uint32_t (*)(VkQueue queue);
// Before the overlay draws: with an overlay:false request waiting, records the copy of the presented image on `queue`
// (waiting on the present's semaphores) and returns the semaphore the overlay and the present must wait on instead;
// otherwise VK_NULL_HANDLE.
VkSemaphore capture_before_overlay(VkQueue queue, const VkPresentInfoKHR* info, QueueFamilyFn family_of);
// The present itself: with a request waiting (overlay:true), records the copy (after the overlay), then calls `next`
// waiting on the copy; otherwise just `next` (one atomic load).
VkResult capture_present(VkQueue queue, const VkPresentInfoKHR* info, PresentFn next, QueueFamilyFn family_of);

}  // namespace emf::core
