// The bridge's screenshot on a fake Vulkan device: the copy is recorded only when asked, waits for the game's work,
// copies the presented image with the right barriers, and the present waits on the copy; the worker writes the PNG with
// the right pixels (8-bit, and a 10-bit HDR10 frame tone-mapped). Unreadable swapchains and unsupported formats get a
// clear error; one screenshot at a time (also while a copy is in flight); a copy that never finishes turns screenshots
// off; overlay:false copies before the overlay.
#include <windows.h>

#include <objbase.h>
#include <wincodec.h>

#include <doctest.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/overlay/vk_capture.h"
#include "test_scratch.h"

using namespace emf::core;

namespace {
struct FakeDispatchable {
    void* loader_table;
};
FakeDispatchable g_inst{nullptr}, g_phys{nullptr}, g_dev{nullptr}, g_queue{nullptr}, g_cmd{nullptr};

constexpr uint32_t kW = 4, kH = 2;
uint8_t g_mapped[kW * kH * 8];  // what the "GPU" copied
std::atomic<int> g_copies{0}, g_submits{0}, g_buffers{0};
VkImage g_copied_image = VK_NULL_HANDLE;
VkBufferImageCopy g_region{};
std::vector<VkSemaphore> g_submit_waits;
std::vector<VkPipelineStageFlags> g_submit_stages;
VkSemaphore g_submit_signal = VK_NULL_HANDLE;
std::vector<VkSemaphore> g_present_waits;
VkImageUsageFlags g_supported_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
VkResult g_fence_result = VK_SUCCESS;
HANDLE g_fence_gate = nullptr;  // when set, the fence wait blocks until it is signalled (a copy in flight)

struct Barrier {
    VkPipelineStageFlags src_stage, dst_stage;
    std::vector<VkImageMemoryBarrier> images;
    std::vector<VkBufferMemoryBarrier> buffers;
};
std::vector<Barrier> g_barriers;

template <typename T>
T handle(uintptr_t v) {
    return reinterpret_cast<T>(v);
}
VkResult VKAPI_CALL ok_any() { return VK_SUCCESS; }
void VKAPI_CALL queue_families(VkPhysicalDevice, uint32_t* n, VkQueueFamilyProperties* p) {
    if (p) {
        *p = VkQueueFamilyProperties{};
        p->queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;
        p->queueCount = 1;
    }
    *n = 1;
}
void VKAPI_CALL memory_props(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* p) {
    *p = VkPhysicalDeviceMemoryProperties{};
    p->memoryTypeCount = 2;
    p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    p->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    p->memoryHeapCount = 1;
}
VkResult VKAPI_CALL surface_caps(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* c) {
    *c = VkSurfaceCapabilitiesKHR{};
    c->supportedUsageFlags = g_supported_usage;
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
VkResult VKAPI_CALL create_pool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool* o) {
    *o = handle<VkCommandPool>(0x33);
    return VK_SUCCESS;
}
VkResult VKAPI_CALL alloc_cmds(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* o) {
    *o = reinterpret_cast<VkCommandBuffer>(&g_cmd);
    return VK_SUCCESS;
}
VkResult VKAPI_CALL create_fence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* o) {
    *o = handle<VkFence>(0x44);
    return VK_SUCCESS;
}
VkResult VKAPI_CALL wait_fences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) {
    if (g_fence_gate) WaitForSingleObject(g_fence_gate, 5000);
    return g_fence_result;
}
VkResult VKAPI_CALL create_sem(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* o) {
    *o = handle<VkSemaphore>(0x55);
    return VK_SUCCESS;
}
VkResult VKAPI_CALL create_buffer(VkDevice, const VkBufferCreateInfo* ci, const VkAllocationCallbacks*, VkBuffer* o) {
    CHECK((ci->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) != 0);
    CHECK(ci->size <= sizeof g_mapped);
    ++g_buffers;
    *o = handle<VkBuffer>(0x66);
    return VK_SUCCESS;
}
void VKAPI_CALL destroy_buffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) { --g_buffers; }
void VKAPI_CALL buffer_reqs(VkDevice, VkBuffer, VkMemoryRequirements* r) {
    r->size = sizeof g_mapped;
    r->alignment = 16;
    r->memoryTypeBits = 0x3;
}
VkResult VKAPI_CALL alloc_memory(VkDevice, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks*,
                                 VkDeviceMemory* o) {
    CHECK(ai->memoryTypeIndex == 1);  // the host-visible type
    *o = handle<VkDeviceMemory>(0x77);
    return VK_SUCCESS;
}
VkResult VKAPI_CALL map_memory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void** p) {
    *p = g_mapped;
    return VK_SUCCESS;
}
void VKAPI_CALL barrier(VkCommandBuffer, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags, uint32_t,
                        const VkMemoryBarrier*, uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni,
                        const VkImageMemoryBarrier* i) {
    g_barriers.push_back(Barrier{src, dst, std::vector<VkImageMemoryBarrier>(i, i + ni),
                                 std::vector<VkBufferMemoryBarrier>(b, b + nb)});
}
void VKAPI_CALL copy_image(VkCommandBuffer, VkImage image, VkImageLayout layout, VkBuffer, uint32_t n,
                           const VkBufferImageCopy* r) {
    CHECK(layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK(n == 1);
    g_copied_image = image;
    g_region = r[0];
    ++g_copies;
}
VkResult VKAPI_CALL submit(VkQueue, uint32_t n, const VkSubmitInfo* si, VkFence fence) {
    CHECK(n == 1);
    CHECK(fence == handle<VkFence>(0x44));
    g_submit_waits.assign(si->pWaitSemaphores, si->pWaitSemaphores + si->waitSemaphoreCount);
    g_submit_stages.assign(si->pWaitDstStageMask, si->pWaitDstStageMask + si->waitSemaphoreCount);
    g_submit_signal = si->signalSemaphoreCount ? si->pSignalSemaphores[0] : VK_NULL_HANDLE;
    ++g_submits;
    return VK_SUCCESS;
}
VkResult VKAPI_CALL present_next(VkQueue, const VkPresentInfoKHR* info) {
    g_present_waits.assign(info->pWaitSemaphores, info->pWaitSemaphores + info->waitSemaphoreCount);
    return VK_SUCCESS;
}
uint32_t family_zero(VkQueue) { return 0; }

PFN_vkVoidFunction VKAPI_CALL fake_gdpa(VkDevice, const char* name) {
    struct {
        const char* name;
        PFN_vkVoidFunction fn;
    } const table[] = {
        {"vkGetSwapchainImagesKHR", reinterpret_cast<PFN_vkVoidFunction>(&swapchain_images)},
        {"vkCreateCommandPool", reinterpret_cast<PFN_vkVoidFunction>(&create_pool)},
        {"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(&alloc_cmds)},
        {"vkCreateFence", reinterpret_cast<PFN_vkVoidFunction>(&create_fence)},
        {"vkWaitForFences", reinterpret_cast<PFN_vkVoidFunction>(&wait_fences)},
        {"vkCreateSemaphore", reinterpret_cast<PFN_vkVoidFunction>(&create_sem)},
        {"vkCreateBuffer", reinterpret_cast<PFN_vkVoidFunction>(&create_buffer)},
        {"vkDestroyBuffer", reinterpret_cast<PFN_vkVoidFunction>(&destroy_buffer)},
        {"vkGetBufferMemoryRequirements", reinterpret_cast<PFN_vkVoidFunction>(&buffer_reqs)},
        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(&alloc_memory)},
        {"vkMapMemory", reinterpret_cast<PFN_vkVoidFunction>(&map_memory)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(&barrier)},
        {"vkCmdCopyImageToBuffer", reinterpret_cast<PFN_vkVoidFunction>(&copy_image)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(&submit)},
    };
    for (const auto& e : table)
        if (!std::strcmp(name, e.name)) return e.fn;
    return reinterpret_cast<PFN_vkVoidFunction>(&ok_any);
}
PFN_vkVoidFunction VKAPI_CALL fake_gipa(VkInstance, const char* name) {
    if (!std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties"))
        return reinterpret_cast<PFN_vkVoidFunction>(&queue_families);
    if (!std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties"))
        return reinterpret_cast<PFN_vkVoidFunction>(&memory_props);
    if (!std::strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(&surface_caps);
    return nullptr;
}

std::wstring temp_emf_dir() {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    static int n = 0;
    std::wstring dir = std::wstring(base) + L"emf_test_capture_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                       std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(++n);
    CreateDirectoryW(dir.c_str(), nullptr);
    test_scratch::remove_at_exit(dir);  // when the run ends: the capture's worker may still write into it
    dir += L"\\emf";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

struct Waiter {
    std::mutex m;
    bool done = false;
    CaptureResult result;
    CaptureDone fn() {
        return [this](const CaptureResult& r) {
            const std::lock_guard<std::mutex> l(m);
            result = r;
            done = true;
        };
    }
    bool wait(DWORD ms = 5000) {
        for (DWORD t = 0; t < ms; t += 10) {
            {
                const std::lock_guard<std::mutex> l(m);
                if (done) return true;
            }
            Sleep(10);
        }
        return false;
    }
};

// Decodes a PNG file with WIC into 24-bit BGR.
bool decode_file(const std::wstring& path, uint32_t* w, uint32_t* h, std::vector<uint8_t>* bgr) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    const GUID clsid = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
    const GUID bgr24 = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};
    if (SUCCEEDED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                   reinterpret_cast<void**>(&f))) &&
        SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(w, h)) &&
        SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, bgr24, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        bgr->resize(static_cast<size_t>(*w) * *h * 3);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, *w * 3, static_cast<UINT>(bgr->size()), bgr->data()));
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{conv, frame, dec, f})
        if (u) u->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

// A fake device attached for the length of a test (capture state is global).
struct Device {
    LayerDevice d;
    VkQueue queue = reinterpret_cast<VkQueue>(&g_queue);
    VkSemaphore game_done = handle<VkSemaphore>(0xAB);
    uint32_t index = 1;
    VkSwapchainKHR sc = handle<VkSwapchainKHR>(0x5C);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    std::wstring emf = temp_emf_dir();
    explicit Device(VkFormat format = VK_FORMAT_B8G8R8A8_UNORM, VkColorSpaceKHR space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        g_fence_result = VK_SUCCESS;
        g_barriers.clear();
        g_submits = 0;
        g_copies = 0;
        d.instance = reinterpret_cast<VkInstance>(&g_inst);
        d.gipa = fake_gipa;
        d.phys = reinterpret_cast<VkPhysicalDevice>(&g_phys);
        d.device = reinterpret_cast<VkDevice>(&g_dev);
        d.gdpa = fake_gdpa;
        capture_attach(d);
        capture_swapchain_created(sc, format, space, {kW, kH}, true);
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &game_done;
        pi.swapchainCount = 1;
        pi.pSwapchains = &sc;
        pi.pImageIndices = &index;
        present();  // a frame came through
    }
    ~Device() {
        capture_swapchain_destroyed(sc);
        capture_detach(d.device);
    }
    VkResult present() { return capture_present(queue, &pi, present_next, family_zero); }
    std::wstring shot(const wchar_t* name) { return emf + L"\\screens\\" + name; }
};

}  // namespace

TEST_CASE("bridge capture: the formats a screenshot reads, and the others named in words") {
    CHECK(capture_pixel_format(VK_FORMAT_B8G8R8A8_UNORM) == emf::core::bridge::PixelFormat::BGRA8);
    CHECK(capture_pixel_format(VK_FORMAT_R8G8B8A8_SRGB) == emf::core::bridge::PixelFormat::RGBA8);
    CHECK(capture_pixel_format(VK_FORMAT_A2B10G10R10_UNORM_PACK32) == emf::core::bridge::PixelFormat::A2B10G10R10);
    CHECK(capture_pixel_format(VK_FORMAT_A2R10G10B10_UNORM_PACK32) == emf::core::bridge::PixelFormat::A2R10G10B10);
    CHECK(capture_pixel_format(VK_FORMAT_R16G16B16A16_SFLOAT) == emf::core::bridge::PixelFormat::RGBA16F);
    CHECK_FALSE(capture_pixel_format(VK_FORMAT_B10G11R11_UFLOAT_PACK32));
    CHECK(capture_transfer(VK_COLOR_SPACE_HDR10_ST2084_EXT) == emf::core::bridge::Transfer::Pq2020);
    CHECK(capture_transfer(VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) == emf::core::bridge::Transfer::LinearScrgb);
    CHECK(capture_transfer(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) == emf::core::bridge::Transfer::Srgb);
    CHECK(capture_format_words(VK_FORMAT_A2B10G10R10_UNORM_PACK32) == "A2B10G10R10 UNORM (10-bit)");
    CHECK(capture_format_words(VK_FORMAT_B10G11R11_UFLOAT_PACK32) == "B10G11R11 UFLOAT (11/10-bit float)");
    CHECK(capture_format_words(static_cast<VkFormat>(1000156000)).find("not an RGBA format EMF reads") !=
          std::string::npos);
}

TEST_CASE("bridge capture: without a device a screenshot says why not") {
    CHECK_FALSE(capture_unavailable().empty());
    Waiter w;
    std::string code;
    CHECK_FALSE(capture_request(L"C:\\x\\emf", L"C:\\x\\emf\\screens\\a.png", {}, w.fn(), &code).empty());
    CHECK(code == "unavailable");
}

TEST_CASE("bridge capture: the next present after a request copies its image with the right barriers; the PNG follows") {
    Device dev;
    CHECK(g_submits.load() == 0);  // no request: the present is the game's own
    CHECK(g_present_waits == std::vector<VkSemaphore>{dev.game_done});
    CHECK(capture_unavailable().empty());
    for (uint32_t i = 0; i < kW * kH; ++i) {  // BGRA pixels the "GPU" copied
        g_mapped[i * 4 + 0] = static_cast<uint8_t>(10 + i);
        g_mapped[i * 4 + 1] = static_cast<uint8_t>(100 + i);
        g_mapped[i * 4 + 2] = static_cast<uint8_t>(200 + i);
        g_mapped[i * 4 + 3] = 0;  // alpha is junk on a swapchain
    }
    Waiter bad;
    std::string code;
    CHECK(capture_request(dev.emf, dev.emf + L"\\emf_core.png", {}, bad.fn(), &code).find("emf\\screens") !=
          std::string::npos);
    CHECK(code == "bad_request");
    const std::wstring path = dev.shot(L"shot.png");
    Waiter w;
    REQUIRE(capture_request(dev.emf, path, {}, w.fn()).empty());
    CHECK(g_buffers.load() == 1);  // the readback buffer is made here, not on the present thread
    Waiter w2;
    CHECK(capture_request(dev.emf, dev.shot(L"other.png"), {}, w2.fn(), &code) == "a screenshot is already being taken");
    CHECK(code == "busy");
    CHECK(dev.present() == VK_SUCCESS);
    CHECK(g_submits.load() == 1);
    CHECK(g_copies.load() == 1);
    CHECK(g_copied_image == handle<VkImage>(0x12));  // image 1 of the swapchain
    CHECK(g_region.imageExtent.width == kW);
    CHECK(g_region.imageExtent.height == kH);
    CHECK(g_submit_waits == std::vector<VkSemaphore>{dev.game_done});      // the copy waits for the game's work
    CHECK(g_submit_stages == std::vector<VkPipelineStageFlags>{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT});
    CHECK(g_present_waits == std::vector<VkSemaphore>{g_submit_signal});  // the present waits for the copy
    // the barriers: present layout -> transfer source (after any write), copy, back to present, buffer to the host
    REQUIRE(g_barriers.size() == 2);
    REQUIRE(g_barriers[0].images.size() == 1);
    const auto& in = g_barriers[0].images[0];
    CHECK(g_barriers[0].src_stage == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    CHECK(g_barriers[0].dst_stage == VK_PIPELINE_STAGE_TRANSFER_BIT);
    CHECK(in.oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    CHECK(in.newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK(in.srcAccessMask == VK_ACCESS_MEMORY_WRITE_BIT);
    CHECK(in.dstAccessMask == VK_ACCESS_TRANSFER_READ_BIT);
    CHECK(in.image == handle<VkImage>(0x12));
    REQUIRE(g_barriers[1].images.size() == 1);
    REQUIRE(g_barriers[1].buffers.size() == 1);
    const auto& out = g_barriers[1].images[0];
    CHECK(g_barriers[1].src_stage == VK_PIPELINE_STAGE_TRANSFER_BIT);
    CHECK((g_barriers[1].dst_stage & VK_PIPELINE_STAGE_HOST_BIT) != 0);
    CHECK(out.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK(out.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    CHECK(g_barriers[1].buffers[0].srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT);
    CHECK(g_barriers[1].buffers[0].dstAccessMask == VK_ACCESS_HOST_READ_BIT);
    REQUIRE(w.wait());
    CHECK_MESSAGE(w.result.ok, w.result.error);
    CHECK(w.result.width == kW);
    CHECK(w.result.height == kH);
    CHECK_FALSE(w.result.hdr);
    uint32_t pw = 0, ph = 0;
    std::vector<uint8_t> bgr;
    REQUIRE(decode_file(path, &pw, &ph, &bgr));
    CHECK(pw == kW);
    CHECK(ph == kH);
    REQUIRE(bgr.size() == kW * kH * 3);
    CHECK(bgr[0] == 10);
    CHECK(bgr[1] == 100);
    CHECK(bgr[2] == 200);
    CHECK(bgr[3 * 7 + 2] == 207);

    CHECK(dev.present() == VK_SUCCESS);  // the next present copies nothing
    CHECK(g_submits.load() == 1);
    Waiter w3;  // a request no present takes can be withdrawn; the buffer is reused
    REQUIRE(capture_request(dev.emf, dev.shot(L"never.png"), {}, w3.fn()).empty());
    CHECK(g_buffers.load() == 1);
    CHECK(capture_cancel());
    CHECK_FALSE(capture_cancel());
    CHECK(dev.present() == VK_SUCCESS);
    CHECK(g_submits.load() == 1);
}

TEST_CASE("bridge capture: a 10-bit HDR10 swapchain (format 64) is tone-mapped to an 8-bit PNG and says hdr") {
    Device dev(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT);
    const uint32_t white = static_cast<uint32_t>(std::lround(0.5807 * 1023));  // about 203 nits: SDR white
    for (uint32_t i = 0; i < kW * kH; ++i) {
        const uint32_t v = i == 0 ? ((3u << 30) | (white << 20) | (white << 10) | white) : (3u << 30);
        std::memcpy(g_mapped + i * 4, &v, 4);
    }
    const std::wstring path = dev.shot(L"hdr.png");
    Waiter w;
    REQUIRE(capture_request(dev.emf, path, {}, w.fn()).empty());
    CHECK(dev.present() == VK_SUCCESS);
    REQUIRE(w.wait());
    CHECK_MESSAGE(w.result.ok, w.result.error);
    CHECK(w.result.hdr);
    uint32_t pw = 0, ph = 0;
    std::vector<uint8_t> bgr;
    REQUIRE(decode_file(path, &pw, &ph, &bgr));
    CHECK(bgr[0] > 200);  // white stays bright
    CHECK(bgr[3] == 0);   // black stays black
}

TEST_CASE("bridge capture: overlay:false copies before the overlay; the after-overlay point leaves it alone") {
    Device dev;
    Waiter w;
    CaptureOptions opt;
    opt.overlay = false;
    opt.max_width = 2;
    const std::wstring path = dev.shot(L"bare.png");
    REQUIRE(capture_request(dev.emf, path, opt, w.fn()).empty());
    CHECK(dev.present() == VK_SUCCESS);  // the point after the overlay: not this request
    CHECK(g_submits.load() == 0);
    const VkSemaphore copied = capture_before_overlay(dev.queue, &dev.pi, family_zero);
    CHECK(copied != VK_NULL_HANDLE);
    CHECK(g_submits.load() == 1);
    CHECK(g_submit_waits == std::vector<VkSemaphore>{dev.game_done});
    REQUIRE(w.wait());
    CHECK_MESSAGE(w.result.ok, w.result.error);
    CHECK(w.result.width == 2);  // scaled down to max_width
    CHECK(w.result.height == 1);
    CHECK(capture_before_overlay(dev.queue, &dev.pi, family_zero) == VK_NULL_HANDLE);  // nothing asked
}

TEST_CASE("bridge capture: a screenshot asked for while a copy is in flight is refused as busy") {
    Device dev;
    g_fence_gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Waiter w;
    REQUIRE(capture_request(dev.emf, dev.shot(L"slow.png"), {}, w.fn()).empty());
    CHECK(dev.present() == VK_SUCCESS);  // taken: the worker waits on the fence
    Waiter w2;
    std::string code;
    CHECK(capture_request(dev.emf, dev.shot(L"second.png"), {}, w2.fn(), &code) == "a screenshot is already being taken");
    CHECK(code == "busy");
    SetEvent(g_fence_gate);
    REQUIRE(w.wait());
    CHECK(w.result.ok);
    CloseHandle(g_fence_gate);
    g_fence_gate = nullptr;
    Waiter w3;
    CHECK(capture_request(dev.emf, dev.shot(L"third.png"), {}, w3.fn()).empty());  // free again
    CHECK(capture_cancel());
}

TEST_CASE("bridge capture: a copy that never finishes turns screenshots off until the device is made again") {
    {
        Device dev;
        g_fence_result = VK_TIMEOUT;
        Waiter w;
        REQUIRE(capture_request(dev.emf, dev.shot(L"stuck.png"), {}, w.fn()).empty());
        CHECK(dev.present() == VK_SUCCESS);
        REQUIRE(w.wait());
        CHECK_FALSE(w.result.ok);
        CHECK(w.result.error.find("did not finish") != std::string::npos);
        CHECK(capture_unavailable().find("did not finish") != std::string::npos);
        Waiter w2;
        std::string code;
        CHECK_FALSE(capture_request(dev.emf, dev.shot(L"after.png"), {}, w2.fn(), &code).empty());
        CHECK(code == "unavailable");
        g_fence_result = VK_SUCCESS;
    }
    Device again;  // a new device: screenshots work again
    CHECK(capture_unavailable().empty());
}

TEST_CASE("bridge capture: an unsupported format and an unreadable swapchain answer with the reason in words") {
    Device dev;
    const auto other = handle<VkSwapchainKHR>(0x5D);
    capture_swapchain_created(other, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, {kW, kH}, true);
    dev.pi.pSwapchains = &other;
    Waiter w;
    REQUIRE(capture_request(dev.emf, dev.shot(L"other.png"), {}, w.fn()).empty());
    CHECK(dev.present() == VK_SUCCESS);
    REQUIRE(w.wait());
    CHECK_FALSE(w.result.ok);
    CHECK(w.result.code == "unavailable");
    CHECK(w.result.error.find("B10G11R11 UFLOAT") != std::string::npos);
    CHECK(g_present_waits == std::vector<VkSemaphore>{dev.game_done});
    const auto closed = handle<VkSwapchainKHR>(0x5E);
    capture_swapchain_created(closed, VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, {kW, kH}, false);
    dev.pi.pSwapchains = &closed;
    Waiter w2;
    REQUIRE(capture_request(dev.emf, dev.shot(L"closed.png"), {}, w2.fn()).empty());
    CHECK(dev.present() == VK_SUCCESS);
    REQUIRE(w2.wait());
    CHECK_FALSE(w2.result.ok);
    CHECK(w2.result.error.find("cannot be read") != std::string::npos);
    CHECK(g_submits.load() == 0);
    capture_swapchain_destroyed(other);
    capture_swapchain_destroyed(closed);
    dev.pi.pSwapchains = &dev.sc;
}

TEST_CASE("bridge capture: surface capabilities decide whether swapchain images can be read") {
    Device dev;
    CHECK(capture_surface_readable(handle<VkSurfaceKHR>(0x99)));
    g_supported_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    CHECK_FALSE(capture_surface_readable(handle<VkSurfaceKHR>(0x99)));
    g_supported_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
}
