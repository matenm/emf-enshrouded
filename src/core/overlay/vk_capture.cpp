#include "vk_capture.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include "common/srw.h"
#include "core/bridge/screens.h"
#include "core/bridge/switch.h"

namespace emf::core {
namespace {

struct Fns {
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers FreeCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges;
} D{};
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR g_surface_caps = nullptr;

struct Chain {
    VkSwapchainKHR handle;
    VkFormat format;
    VkColorSpaceKHR space;
    VkExtent2D extent;
    bool readable;
};

struct Request {
    std::wstring emf_dir, path;
    CaptureOptions options;
    CaptureDone done;
};

// The readback buffer: made when a screenshot is asked for (the bridge's thread), kept mapped for the next one.
struct Readback {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    bool coherent = true;
    void* mapped = nullptr;
};

// A submitted copy, until the worker has read it (or the device goes away).
struct Flight {
    std::shared_ptr<Request> req;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    bridge::PixelFormat format = bridge::PixelFormat::BGRA8;
    bridge::Transfer transfer = bridge::Transfer::Srgb;
    VkSwapchainKHR chain = VK_NULL_HANDLE;
};

// Lock order: g_worker_lock, then g_flight_lock, then g_lock. The present and swapchain paths take only the last two,
// and only briefly; the worker's fence wait holds g_worker_lock alone.
SRWLOCK g_lock = SRWLOCK_INIT;  // the device, its swapchains, the request, why screenshots are off
VkDevice g_device = VK_NULL_HANDLE;
VkPhysicalDevice g_phys = VK_NULL_HANDLE;
PFN_vkSetDeviceLoaderData g_set_loader_data = nullptr;
std::vector<VkQueueFamilyProperties> g_families;
VkPhysicalDeviceMemoryProperties g_mem{};
std::string g_broken;  // why screenshots are off on this device ("" = they work)
std::vector<Chain> g_chains;
std::shared_ptr<Request> g_request;
std::atomic<bool> g_requested{false};  // the present path's quick check
std::atomic<bool> g_busy{false};       // a present took a request and its result has not been handed back
std::atomic<uint64_t> g_last_present_ms{0};
std::atomic<uint64_t> g_last_chain{0};  // the swapchain presented last (for sizing the readback buffer)
std::atomic<int> g_wanted{-1};

// The copy's objects: created and recorded on the present thread, never while a copy is in flight.
VkCommandPool g_pool = VK_NULL_HANDLE;
uint32_t g_pool_family = UINT32_MAX;
VkCommandBuffer g_cmd = VK_NULL_HANDLE;
VkFence g_fence = VK_NULL_HANDLE;
VkSemaphore g_sem = VK_NULL_HANDLE;

SRWLOCK g_worker_lock = SRWLOCK_INIT;  // the worker's device calls and the readback buffer (detach waits for them)
Readback g_rb;
SRWLOCK g_flight_lock = SRWLOCK_INIT;  // g_flight
std::optional<Flight> g_flight;
HANDLE g_work = nullptr;               // wakes the worker thread (one for the process, made with the first request)

constexpr uint64_t kCopyWaitNs = 2000000000ull;

void hand_back(const std::shared_ptr<Request>& req, const CaptureResult& r) {
    g_busy = false;  // a new screenshot may be asked for from here on
    if (req && req->done) req->done(r);
}

CaptureResult failure(const std::shared_ptr<Request>& req, const char* code, std::string error) {
    CaptureResult r;
    r.path = req ? req->path : std::wstring();
    r.code = code;
    r.error = std::move(error);
    return r;
}

int pick_memory(uint32_t bits, bool* coherent) {
    const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags coh = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const VkMemoryPropertyFlags wanted[] = {visible | coh | cached, visible | coh, visible | cached, visible};
    for (const VkMemoryPropertyFlags w : wanted)
        for (uint32_t i = 0; i < g_mem.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (g_mem.memoryTypes[i].propertyFlags & w) == w) {
                *coherent = (g_mem.memoryTypes[i].propertyFlags & coh) != 0;
                return static_cast<int>(i);
            }
    return -1;
}

void free_readback(VkDevice device) {
    if (g_rb.mapped) D.UnmapMemory(device, g_rb.mem);
    if (g_rb.buf) D.DestroyBuffer(device, g_rb.buf, nullptr);
    if (g_rb.mem) D.FreeMemory(device, g_rb.mem, nullptr);
    g_rb = Readback{};
}

// A readback buffer of at least `size` bytes (g_worker_lock held, no copy in flight). "" or why not.
std::string ensure_readback(VkDevice device, VkDeviceSize size) {
    if (g_rb.buf && g_rb.size >= size) return "";
    free_readback(device);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (D.CreateBuffer(device, &bi, nullptr, &g_rb.buf) != VK_SUCCESS) {
        g_rb = Readback{};
        return "no buffer for the copy";
    }
    VkMemoryRequirements req{};
    D.GetBufferMemoryRequirements(device, g_rb.buf, &req);
    const int type = pick_memory(req.memoryTypeBits, &g_rb.coherent);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type < 0 ? 0 : static_cast<uint32_t>(type);
    if (type < 0 || D.AllocateMemory(device, &ai, nullptr, &g_rb.mem) != VK_SUCCESS ||
        D.BindBufferMemory(device, g_rb.buf, g_rb.mem, 0) != VK_SUCCESS ||
        D.MapMemory(device, g_rb.mem, 0, VK_WHOLE_SIZE, 0, &g_rb.mapped) != VK_SUCCESS || !g_rb.mapped) {
        g_rb.mapped = nullptr;
        free_readback(device);
        return "no host-visible memory for the copy";
    }
    g_rb.size = size;
    return "";
}

bool ensure_objects(uint32_t family) {
    if (g_pool && g_pool_family != family) {  // the game presents from another queue family now
        if (g_cmd) D.FreeCommandBuffers(g_device, g_pool, 1, &g_cmd);
        D.DestroyCommandPool(g_device, g_pool, nullptr);
        g_pool = VK_NULL_HANDLE;
        g_cmd = VK_NULL_HANDLE;
    }
    if (!g_pool) {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = family;
        if (D.CreateCommandPool(g_device, &ci, nullptr, &g_pool) != VK_SUCCESS) {
            g_pool = VK_NULL_HANDLE;
            return false;
        }
        g_pool_family = family;
    }
    if (!g_cmd) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = g_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (D.AllocateCommandBuffers(g_device, &ai, &g_cmd) != VK_SUCCESS) {
            g_cmd = VK_NULL_HANDLE;
            return false;
        }
        // a command buffer made below the loader carries no dispatch pointer yet (as in vk_overlay.cpp)
        if (g_set_loader_data && g_set_loader_data(g_device, g_cmd) != VK_SUCCESS) {
            D.FreeCommandBuffers(g_device, g_pool, 1, &g_cmd);
            g_cmd = VK_NULL_HANDLE;
            return false;
        }
    }
    if (!g_fence) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (D.CreateFence(g_device, &fi, nullptr, &g_fence) != VK_SUCCESS) {
            g_fence = VK_NULL_HANDLE;
            return false;
        }
    }
    if (!g_sem) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (D.CreateSemaphore(g_device, &si, nullptr, &g_sem) != VK_SUCCESS) {
            g_sem = VK_NULL_HANDLE;
            return false;
        }
    }
    return true;
}

struct RecordArgs {
    VkQueue queue;
    uint32_t family;
    const VkPresentInfoKHR* info;
    Flight* flight;  // filled on success
    bool faulted;
    const char* code;  // when it fails
    char error[300];
};

// Records and submits the copy of the presented image (present thread). False and a->error when it cannot.
bool record(RecordArgs* a) {
    auto fail = [a](const char* code, const char* text) {
        a->code = code;
        std::snprintf(a->error, sizeof a->error, "%s", text);
        return false;
    };
    const VkPresentInfoKHR* info = a->info;
    if (info->swapchainCount < 1 || !info->pSwapchains || !info->pImageIndices)
        return fail("unavailable", "the present showed no swapchain image");
    const VkSwapchainKHR sc = info->pSwapchains[0];
    Chain chain{};
    bool found = false;
    {
        const SrwShared lock(g_lock);
        for (const auto& c : g_chains)
            if (c.handle == sc) {
                chain = c;
                found = true;
            }
    }
    if (!found) return fail("unavailable", "the presented swapchain was made before EMF's layer saw the device");
    if (!chain.readable)
        return fail("unavailable", "the game's swapchain images cannot be read (made without transfer use; the bridge "
                                   "must be on when the game starts)");
    const auto format = capture_pixel_format(chain.format);
    if (!format) {
        a->code = "unavailable";
        std::snprintf(a->error, sizeof a->error, "the swapchain's pixel format is not one EMF reads: %s",
                      capture_format_words(chain.format).c_str());
        return false;
    }
    const VkQueueFlags copy_ok = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    if (a->family >= g_families.size() || !(g_families[a->family].queueFlags & copy_ok))
        return fail("unavailable", "the game presents from a queue that cannot copy images");
    uint32_t n = 0;
    if (D.GetSwapchainImagesKHR(g_device, sc, &n, nullptr) != VK_SUCCESS || n == 0)
        return fail("unavailable", "the swapchain's images could not be listed");
    std::vector<VkImage> images(n);
    if (D.GetSwapchainImagesKHR(g_device, sc, &n, images.data()) != VK_SUCCESS)
        return fail("unavailable", "the swapchain's images could not be listed");
    const uint32_t index = info->pImageIndices[0];
    if (index >= n) return fail("unavailable", "the presented image is not in its swapchain");
    const uint32_t w = chain.extent.width, h = chain.extent.height;
    if (!w || !h || w > 16384 || h > 16384) return fail("unavailable", "the swapchain has no usable size");
    const VkDeviceSize needed = static_cast<VkDeviceSize>(w) * h * bridge::bytes_per_pixel(*format);
    if (!g_rb.buf || g_rb.size < needed)  // made for the swapchain presented when the screenshot was asked for
        return fail("busy", "the game's window changed size since the screenshot was asked for: try again");
    if (!ensure_objects(a->family)) return fail("unavailable", "the copy's command buffer could not be made");

    D.ResetFences(g_device, 1, &g_fence);
    D.ResetCommandBuffer(g_cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    D.BeginCommandBuffer(g_cmd, &begin);
    VkImageMemoryBarrier to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_copy.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;  // whatever wrote the image before (the game, the overlay)
    to_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_copy.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_copy.image = images[index];
    to_copy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    D.CmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_copy);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    D.CmdCopyImageToBuffer(g_cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_rb.buf, 1, &region);
    VkImageMemoryBarrier back = to_copy;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = 0;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkBufferMemoryBarrier to_host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = g_rb.buf;
    to_host.size = VK_WHOLE_SIZE;
    D.CmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &to_host, 1,
                         &back);
    const bool ended = D.EndCommandBuffer(g_cmd) == VK_SUCCESS;
    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = info->waitSemaphoreCount;
    si.pWaitSemaphores = info->pWaitSemaphores;
    si.pWaitDstStageMask = stages.empty() ? nullptr : stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_sem;
    if (!ended || D.QueueSubmit(a->queue, 1, &si, g_fence) != VK_SUCCESS)
        return fail("unavailable", "the copy could not be submitted");
    Flight& f = *a->flight;
    f.device = g_device;
    f.w = w;
    f.h = h;
    f.format = *format;
    f.transfer = capture_transfer(chain.space);
    f.chain = sc;
    return true;
}

bool guarded_record(RecordArgs* a) {
    __try {
        return record(a);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        a->faulted = true;
        return false;
    }
}

void set_broken(const std::string& why) {
    const SrwExclusive lock(g_lock);
    g_broken = why;
}

// One copy: wait for it, convert it, write the PNG, hand the result back.
void work_one() {
    std::shared_ptr<Request> req;
    CaptureResult r;
    std::vector<uint8_t> bgr;
    std::wstring emf_dir;
    uint32_t max_width = 0;
    {
        const SrwExclusive work(g_worker_lock);  // the device and the buffer stay while this holds it (detach waits)
        Flight f;
        {
            const SrwShared lock(g_flight_lock);
            if (!g_flight) return;  // the device went away first; detach answered
            f = *g_flight;
        }
        req = f.req;
        r.path = req->path;
        r.width = f.w;
        r.height = f.h;
        emf_dir = req->emf_dir;
        max_width = req->options.max_width;
        if (D.WaitForFences(f.device, 1, &g_fence, VK_TRUE, kCopyWaitNs) != VK_SUCCESS) {
            r.code = "unavailable";
            r.error = "the GPU copy did not finish within 2 s; screenshots are off until the game restarts";
            set_broken(r.error);
        } else if (!g_rb.mapped) {
            r.code = "unavailable";
            r.error = "the copy could not be read";
        } else {
            if (!g_rb.coherent) {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                range.memory = g_rb.mem;
                range.size = VK_WHOLE_SIZE;
                D.InvalidateMappedMemoryRanges(f.device, 1, &range);
            }
            bgr = bridge::to_srgb_bgr(static_cast<const uint8_t*>(g_rb.mapped),
                                      static_cast<size_t>(f.w) * bridge::bytes_per_pixel(f.format), f.w, f.h, f.format,
                                      f.transfer, &r.hdr);
        }
        const SrwExclusive lock(g_flight_lock);
        g_flight.reset();
    }
    if (r.error.empty()) {
        uint32_t w = r.width, h = r.height;
        if (max_width) bgr = bridge::scale_down_bgr(bgr, r.width, r.height, max_width, &w, &h);
        r.width = w;
        r.height = h;
        std::string why;
        const auto png = bridge::encode_png_bgr(bgr.data(), w, h, &why);
        if (png.empty()) {
            r.code = "unavailable";
            r.error = "the PNG could not be made: " + why;
        } else if (!bridge::write_screenshot(emf_dir, r.path, png, &why)) {
            r.code = "unavailable";
            r.error = why;
        } else {
            r.ok = true;
        }
    }
    hand_back(req, r);
}

DWORD WINAPI worker_main(void*) {
    for (;;) {
        WaitForSingleObject(g_work, INFINITE);
        work_one();
    }
}

// Takes the waiting request if it is for this point of the present (before or after the overlay).
std::shared_ptr<Request> take_request(bool before_overlay) {
    if (!g_requested.load(std::memory_order_acquire)) return nullptr;
    const SrwExclusive lock(g_lock);
    if (!g_request || g_request->options.overlay == before_overlay) return nullptr;
    std::shared_ptr<Request> req = std::move(g_request);
    g_request.reset();
    g_requested = false;
    g_busy = true;
    return req;
}

// Records the copy for `req`; the semaphore to wait on, or VK_NULL_HANDLE (and the request answered) on a failure.
VkSemaphore copy_for(const std::shared_ptr<Request>& req, VkQueue queue, const VkPresentInfoKHR* info,
                     QueueFamilyFn family_of) {
    Flight flight;
    flight.req = req;
    RecordArgs a{queue, family_of ? family_of(queue) : UINT32_MAX, info, &flight, false, "unavailable", {}};
    if (!guarded_record(&a)) {
        const std::string error =
            a.faulted ? "a fault in the graphics driver while copying the screen; screenshots are off" : a.error;
        if (a.faulted) set_broken(error);
        hand_back(req, failure(req, a.code, error));
        return VK_NULL_HANDLE;
    }
    {
        const SrwExclusive lock(g_flight_lock);
        g_flight = flight;
    }
    SetEvent(g_work);  // the worker waits for the copy, the frame does not
    return g_sem;
}

void note_present(const VkPresentInfoKHR* info) {
    g_last_present_ms.store(GetTickCount64(), std::memory_order_relaxed);
    if (info && info->swapchainCount && info->pSwapchains)
        g_last_chain.store(reinterpret_cast<uint64_t>(info->pSwapchains[0]), std::memory_order_relaxed);
}

}  // namespace

std::optional<bridge::PixelFormat> capture_pixel_format(VkFormat format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB: return bridge::PixelFormat::BGRA8;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB: return bridge::PixelFormat::RGBA8;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return bridge::PixelFormat::A2B10G10R10;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return bridge::PixelFormat::A2R10G10B10;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return bridge::PixelFormat::RGBA16F;
        default: return std::nullopt;
    }
}

bridge::Transfer capture_transfer(VkColorSpaceKHR space) {
    switch (space) {
        case VK_COLOR_SPACE_HDR10_ST2084_EXT: return bridge::Transfer::Pq2020;
        case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return bridge::Transfer::LinearScrgb;
        default: return bridge::Transfer::Srgb;
    }
}

std::string capture_format_words(VkFormat format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8 UNORM (8-bit)";
        case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8 SRGB (8-bit)";
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8 UNORM (8-bit)";
        case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8 SRGB (8-bit)";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10 UNORM (10-bit)";
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10 UNORM (10-bit)";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16 SFLOAT (16-bit float)";
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "B10G11R11 UFLOAT (11/10-bit float)";
        case VK_FORMAT_R5G6B5_UNORM_PACK16: return "R5G6B5 UNORM (16-bit)";
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return "A1R5G5B5 UNORM (16-bit)";
        default:
            return "format " + std::to_string(static_cast<int>(format)) + " (not an RGBA format EMF reads)";
    }
}

bool capture_wanted() {
    int w = g_wanted.load();
    if (w < 0) {
        HMODULE m = nullptr;
        wchar_t path[MAX_PATH] = {};
        w = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&capture_wanted), &m) &&
            GetModuleFileNameW(m, path, MAX_PATH)) {
            std::wstring ini(path);  // <game>\emf\emf_core.dll -> <game>\emf\emf_core.ini
            ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"emf_core.ini";
            w = bridge::bridge_switch_on(ini) ? 1 : 0;
        }
        g_wanted = w;
    }
    return w == 1;
}

void capture_set_wanted(bool on) { g_wanted = on ? 1 : 0; }

std::string capture_unavailable() {
    const SrwShared lock(g_lock);
    if (!g_device) return "no game frames come through EMF's Vulkan layer (the dedicated server has no screen)";
    if (!g_broken.empty()) return g_broken;
    if (GetTickCount64() - g_last_present_ms.load() > 2000)
        return "the game showed no frame in the last 2 s (is it minimised?)";
    return "";
}

std::string capture_request(const std::wstring& emf_dir, const std::wstring& path, const CaptureOptions& options,
                            CaptureDone done, std::string* code) {
    auto refuse = [code](const char* c, std::string why) {
        if (code) *code = c;
        return why;
    };
    if (!bridge::screenshot_path_ok(emf_dir, path))
        return refuse("bad_request", "not a screenshot path: pictures only go into emf\\screens");
    std::string why = capture_unavailable();
    if (!why.empty()) return refuse("unavailable", why);
    if (g_busy || g_requested) return refuse("busy", "a screenshot is already being taken");
    {
        // The readback buffer, made here and not on the present thread, for the swapchain presented last.
        const SrwExclusive work(g_worker_lock);
        VkDevice device = VK_NULL_HANDLE;
        VkDeviceSize needed = 0;
        {
            const SrwShared lock(g_lock);
            device = g_device;
            const auto last = reinterpret_cast<VkSwapchainKHR>(g_last_chain.load());
            for (const auto& c : g_chains)
                if (c.handle == last) {
                    const auto f = capture_pixel_format(c.format);
                    needed = static_cast<VkDeviceSize>(c.extent.width) * c.extent.height *
                             bridge::bytes_per_pixel(f ? *f : bridge::PixelFormat::BGRA8);
                }
        }
        if (!device) return refuse("unavailable", "no game frames come through EMF's Vulkan layer");
        if (needed) {  // (a swapchain EMF does not know is refused at the present, with the reason)
            const std::string no = ensure_readback(device, needed);
            if (!no.empty()) return refuse("unavailable", no);
        }
        if (!g_work) {
            g_work = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            HANDLE t = g_work ? CreateThread(nullptr, 0, worker_main, nullptr, 0, nullptr) : nullptr;
            if (!t) {
                if (g_work) CloseHandle(g_work);
                g_work = nullptr;
                return refuse("unavailable", "no worker thread could be started for screenshots");
            }
            CloseHandle(t);
        }
    }
    const SrwExclusive lock(g_lock);
    if (g_request || g_busy) return refuse("busy", "a screenshot is already being taken");
    g_request = std::make_shared<Request>(Request{emf_dir, path, options, std::move(done)});
    g_requested.store(true, std::memory_order_release);
    return "";
}

bool capture_cancel() {
    const SrwExclusive lock(g_lock);
    if (!g_request) return false;
    g_request.reset();
    g_requested = false;
    return true;
}

void capture_attach(const LayerDevice& d) {
    const SrwExclusive lock(g_lock);
    if (g_device || !d.device || !d.gdpa || !d.gipa) return;
#define EMF_LOAD(name) D.name = reinterpret_cast<PFN_vk##name>(d.gdpa(d.device, "vk" #name))
    EMF_LOAD(GetSwapchainImagesKHR);
    EMF_LOAD(CreateCommandPool);
    EMF_LOAD(DestroyCommandPool);
    EMF_LOAD(AllocateCommandBuffers);
    EMF_LOAD(FreeCommandBuffers);
    EMF_LOAD(BeginCommandBuffer);
    EMF_LOAD(EndCommandBuffer);
    EMF_LOAD(ResetCommandBuffer);
    EMF_LOAD(CmdPipelineBarrier);
    EMF_LOAD(CmdCopyImageToBuffer);
    EMF_LOAD(QueueSubmit);
    EMF_LOAD(CreateFence);
    EMF_LOAD(DestroyFence);
    EMF_LOAD(WaitForFences);
    EMF_LOAD(ResetFences);
    EMF_LOAD(CreateSemaphore);
    EMF_LOAD(DestroySemaphore);
    EMF_LOAD(CreateBuffer);
    EMF_LOAD(DestroyBuffer);
    EMF_LOAD(GetBufferMemoryRequirements);
    EMF_LOAD(AllocateMemory);
    EMF_LOAD(FreeMemory);
    EMF_LOAD(BindBufferMemory);
    EMF_LOAD(MapMemory);
    EMF_LOAD(UnmapMemory);
    EMF_LOAD(InvalidateMappedMemoryRanges);
#undef EMF_LOAD
    bool all = true;
    for (const void* const* f = reinterpret_cast<const void* const*>(&D); f < reinterpret_cast<const void* const*>(&D + 1);
         ++f)
        all &= *f != nullptr;
    g_surface_caps = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        d.gipa(d.instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    auto qfp = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        d.gipa(d.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto mem = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        d.gipa(d.instance, "vkGetPhysicalDeviceMemoryProperties"));
    g_families.clear();
    if (qfp) {
        uint32_t n = 0;
        qfp(d.phys, &n, nullptr);
        g_families.resize(n);
        qfp(d.phys, &n, g_families.data());
    }
    g_mem = VkPhysicalDeviceMemoryProperties{};
    if (mem) mem(d.phys, &g_mem);
    g_device = d.device;
    g_phys = d.phys;
    g_set_loader_data = d.set_loader_data;
    g_broken = all && qfp && mem && !g_families.empty() ? "" : "screenshots are off: Vulkan functions EMF needs are missing";
}

void capture_detach(VkDevice device) {
    std::shared_ptr<Request> waiting, flying;
    {
        const SrwExclusive work(g_worker_lock);  // a worker's device calls end first
        const SrwExclusive flight(g_flight_lock);
        const SrwExclusive lock(g_lock);
        if (!device || device != g_device) return;
        if (g_flight) {  // a copy no worker has read yet
            if (g_fence) D.WaitForFences(device, 1, &g_fence, VK_TRUE, 1000000000ull);
            flying = g_flight->req;
            g_flight.reset();
        }
        free_readback(device);
        if (g_sem) D.DestroySemaphore(device, g_sem, nullptr);
        if (g_fence) D.DestroyFence(device, g_fence, nullptr);
        if (g_cmd && g_pool) D.FreeCommandBuffers(device, g_pool, 1, &g_cmd);
        if (g_pool) D.DestroyCommandPool(device, g_pool, nullptr);
        g_sem = VK_NULL_HANDLE;
        g_fence = VK_NULL_HANDLE;
        g_cmd = VK_NULL_HANDLE;
        g_pool = VK_NULL_HANDLE;
        g_pool_family = UINT32_MAX;
        g_chains.clear();
        waiting = std::move(g_request);
        g_request.reset();
        g_requested = false;
        g_device = VK_NULL_HANDLE;
        g_phys = VK_NULL_HANDLE;
        g_broken.clear();
        g_last_chain = 0;
    }
    if (flying) hand_back(flying, failure(flying, "unavailable", "the game's Vulkan device went away"));
    if (waiting) hand_back(waiting, failure(waiting, "unavailable", "the game's Vulkan device went away"));
    g_busy = false;
}

bool capture_surface_readable(VkSurfaceKHR surface) {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR caps_fn = nullptr;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    {
        const SrwShared lock(g_lock);
        caps_fn = g_surface_caps;
        phys = g_phys;
    }
    if (!caps_fn || !phys || !surface) return false;
    VkSurfaceCapabilitiesKHR caps{};
    return caps_fn(phys, surface, &caps) == VK_SUCCESS && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
}

void capture_swapchain_created(VkSwapchainKHR swapchain, VkFormat format, VkColorSpaceKHR space, VkExtent2D extent,
                               bool readable) {
    const SrwExclusive lock(g_lock);
    for (auto& c : g_chains)
        if (c.handle == swapchain) {
            c = Chain{swapchain, format, space, extent, readable};
            return;
        }
    g_chains.push_back(Chain{swapchain, format, space, extent, readable});
}

void capture_swapchain_destroyed(VkSwapchainKHR swapchain) {
    {
        const SrwExclusive lock(g_lock);
        for (size_t i = 0; i < g_chains.size(); ++i)
            if (g_chains[i].handle == swapchain) {
                g_chains.erase(g_chains.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
    }
    // a copy from this swapchain's image must be done before the image goes (waiting on a fence from two threads is
    // allowed; the worker may be waiting on it too)
    VkDevice device = VK_NULL_HANDLE;
    bool from_it = false;
    {
        const SrwShared flight(g_flight_lock);
        from_it = g_flight && g_flight->chain == swapchain;
        if (from_it) device = g_flight->device;
    }
    if (from_it && g_fence) D.WaitForFences(device, 1, &g_fence, VK_TRUE, 1000000000ull);
}

VkSemaphore capture_before_overlay(VkQueue queue, const VkPresentInfoKHR* info, QueueFamilyFn family_of) {
    if (!info) return VK_NULL_HANDLE;
    std::shared_ptr<Request> req = take_request(true);
    if (!req) return VK_NULL_HANDLE;
    return copy_for(req, queue, info, family_of);
}

VkResult capture_present(VkQueue queue, const VkPresentInfoKHR* info, PresentFn next, QueueFamilyFn family_of) {
    note_present(info);
    if (!info) return next(queue, info);
    std::shared_ptr<Request> req = take_request(false);
    if (!req) return next(queue, info);
    const VkSemaphore copied = copy_for(req, queue, info, family_of);
    if (!copied) return next(queue, info);
    VkPresentInfoKHR copy = *info;  // the present waits for the copy, which waited for the game's work
    copy.waitSemaphoreCount = 1;
    copy.pWaitSemaphores = &copied;
    return next(queue, &copy);
}

}  // namespace emf::core
