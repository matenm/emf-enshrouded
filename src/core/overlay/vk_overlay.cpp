#include "vk_overlay.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/srw.h"
#include "imgui_impl_vulkan.h"
#include "imgui_internal.h"
#include "swapchain_book.h"
#include "vk_capture.h"

namespace emf::core {
namespace {

bool guarded_render(RenderFn render, VkQueue q, const VkPresentInfoKHR* info, VkSemaphore* sem, void* user) {
    __try {
        return render(q, info, sem, user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

VkResult present_through(VkQueue queue, const VkPresentInfoKHR* info, bool visible, std::atomic<bool>* disabled,
                         RenderFn render, void* user, PresentFn original) {
    if (!visible || !render || !info || (disabled && disabled->load())) return original(queue, info);
    VkSemaphore sem = VK_NULL_HANDLE;
    const bool ok = guarded_render(render, queue, info, &sem, user);
    if (!ok && disabled) disabled->store(true);
    if (sem) {  // our draw waited on the game's semaphores; the present now waits on ours
        VkPresentInfoKHR copy = *info;
        copy.waitSemaphoreCount = 1;
        copy.pWaitSemaphores = &sem;
        return original(queue, &copy);
    }
    return original(queue, info);
}

namespace {

// ---- logging (lines from before the core attached its log are kept) ----
SRWLOCK g_log_lock = SRWLOCK_INIT;
std::function<void(const char*)> g_log;
std::vector<std::string> g_pending_log;

void log(const std::string& s) {
    std::function<void(const char*)> fn;
    {
        const SrwExclusive lock(g_log_lock);
        if (!g_log) {
            if (g_pending_log.size() < 100) g_pending_log.push_back(s);
            return;
        }
        fn = g_log;
    }
    fn(s.c_str());  // outside the lock: a fault or exception in the logger leaves nothing locked
}

// ---- state ----
VkOverlayHooks g_hooks;
std::atomic<bool> g_hooks_set{false};
std::atomic<bool> g_disabled{false};

VkInstance g_instance = VK_NULL_HANDLE;
uint32_t g_api = VK_API_VERSION_1_0;
PFN_vkGetInstanceProcAddr g_gipa = nullptr;  // the next layer's, for the game's instance
PFN_vkGetDeviceProcAddr g_gdpa = nullptr;    // the next layer's, for the game's device
PFN_vkSetDeviceLoaderData g_set_loader_data = nullptr;
VkPhysicalDevice g_phys = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
std::atomic<bool> g_device_ready{false};
std::atomic<uint64_t> g_last_present_ms{0};
std::atomic<uint64_t> g_present_count{0};   // the debug bridge's state: frames through the layer
std::atomic<uint64_t> g_frame_ns{0};        // and the recent frame time (smoothed)
LARGE_INTEGER g_last_present_qpc{};         // present thread
std::vector<VkQueueFamilyProperties> g_families;
SRWLOCK g_queue_lock = SRWLOCK_INIT;
std::vector<std::pair<VkQueue, uint32_t>> g_queues;

// The next layer's functions for the game's device.
PFN_vkCreateSwapchainKHR o_create_swapchain = nullptr;
PFN_vkDestroySwapchainKHR o_destroy_swapchain = nullptr;
PresentFn o_present = nullptr;

struct DeviceFns {
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers FreeCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
} D{};

SRWLOCK g_book_lock = SRWLOCK_INIT;
SwapchainBook g_book;
VkCommandPool g_pool = VK_NULL_HANDLE;
uint32_t g_pool_family = UINT32_MAX;
std::vector<std::pair<VkFormat, VkRenderPass>> g_passes;
bool g_imgui = false;          // the ImGui context exists (it outlives devices)
bool g_backend = false;        // ImGui's Vulkan backend runs on g_device
VkRenderPass g_imgui_pass = VK_NULL_HANDLE;
uint32_t g_imgui_images = 0;
LARGE_INTEGER g_last{}, g_freq{};

// Command buffers EMF allocates by calling below the loader carry no loader dispatch pointer yet; layers below EMF
// key their tables by it, so the loader's callback stamps them.
bool stamp(VkCommandBuffer* cmds, uint32_t n) {
    if (!g_set_loader_data) return true;
    for (uint32_t i = 0; i < n; ++i)
        if (g_set_loader_data(g_device, cmds[i]) != VK_SUCCESS) return false;
    return true;
}

VkResult VKAPI_CALL imgui_allocate_command_buffers(VkDevice dev, const VkCommandBufferAllocateInfo* ai,
                                                   VkCommandBuffer* out) {
    const VkResult r = D.AllocateCommandBuffers(dev, ai, out);
    if (r == VK_SUCCESS && !stamp(out, ai->commandBufferCount)) {
        D.FreeCommandBuffers(dev, ai->commandPool, ai->commandBufferCount, out);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return r;
}

PFN_vkVoidFunction load_fn(const char* name, void*) {
    if (!std::strcmp(name, "vkAllocateCommandBuffers"))
        return reinterpret_cast<PFN_vkVoidFunction>(&imgui_allocate_command_buffers);
    PFN_vkVoidFunction f = nullptr;
    if (g_device && g_gdpa) f = g_gdpa(g_device, name);
    if (!f && g_instance && g_gipa) f = g_gipa(g_instance, name);
    return f;
}

uint32_t family_of(VkQueue q) {
    const SrwShared lock(g_queue_lock);
    uint32_t family = UINT32_MAX;
    for (const auto& [queue, fam] : g_queues)
        if (queue == q) family = fam;
    return family;
}

// The game's present, after the debug bridge's screenshot copy when one was asked for.
VkResult VKAPI_CALL present_next(VkQueue queue, const VkPresentInfoKHR* info) {
    return capture_present(queue, info, o_present, family_of);
}

VkRenderPass render_pass_for(VkFormat format) {
    for (const auto& [f, p] : g_passes)
        if (f == format) return p;
    VkAttachmentDescription att{};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // draw on top of the game's image
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1;
    ci.pAttachments = &att;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (D.CreateRenderPass(g_device, &ci, nullptr, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
    g_passes.emplace_back(format, pass);
    return pass;
}

bool ensure_pool(uint32_t family) {
    if (g_pool) return g_pool_family == family;
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = family;
    if (D.CreateCommandPool(g_device, &ci, nullptr, &g_pool) != VK_SUCCESS) return false;
    g_pool_family = family;
    return true;
}

void free_record(SwapchainRecord& r) {
    if (!r.fences.empty())
        D.WaitForFences(g_device, static_cast<uint32_t>(r.fences.size()), r.fences.data(), VK_TRUE, 1000000000ull);
    for (auto fb : r.framebuffers) D.DestroyFramebuffer(g_device, fb, nullptr);
    for (auto v : r.views) D.DestroyImageView(g_device, v, nullptr);
    for (auto s : r.done) D.DestroySemaphore(g_device, s, nullptr);
    for (auto f : r.fences) D.DestroyFence(g_device, f, nullptr);
    if (!r.commands.empty() && g_pool)
        D.FreeCommandBuffers(g_device, g_pool, static_cast<uint32_t>(r.commands.size()), r.commands.data());
    r = SwapchainRecord{};
}

// Our per-image objects for a swapchain (first draw after it was created).
bool prepare(SwapchainRecord& r, uint32_t family) {
    if (r.ready) return true;
    if (!ensure_pool(family)) return false;
    uint32_t n = 0;
    if (D.GetSwapchainImagesKHR(g_device, r.handle, &n, nullptr) != VK_SUCCESS || n == 0) return false;
    r.images.resize(n);
    if (D.GetSwapchainImagesKHR(g_device, r.handle, &n, r.images.data()) != VK_SUCCESS) return false;
    r.render_pass = render_pass_for(r.format);
    if (!r.render_pass) return false;
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = r.images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = r.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (D.CreateImageView(g_device, &vi, nullptr, &view) != VK_SUCCESS) return false;
        r.views.push_back(view);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = r.render_pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &view;
        fi.width = r.extent.width;
        fi.height = r.extent.height;
        fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (D.CreateFramebuffer(g_device, &fi, nullptr, &fb) != VK_SUCCESS) return false;
        r.framebuffers.push_back(fb);
        VkFenceCreateInfo fe{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fe.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkFence fence = VK_NULL_HANDLE;
        if (D.CreateFence(g_device, &fe, nullptr, &fence) != VK_SUCCESS) return false;
        r.fences.push_back(fence);
        VkSemaphoreCreateInfo se{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore sem = VK_NULL_HANDLE;
        if (D.CreateSemaphore(g_device, &se, nullptr, &sem) != VK_SUCCESS) return false;
        r.done.push_back(sem);
    }
    r.commands.resize(n);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = n;
    if (D.AllocateCommandBuffers(g_device, &ai, r.commands.data()) != VK_SUCCESS) {
        r.commands.clear();
        return false;
    }
    if (!stamp(r.commands.data(), n)) return false;
    r.ready = true;
    return true;
}

bool init_imgui(const SwapchainRecord& r, VkQueue queue, uint32_t family) {
    const uint32_t images = static_cast<uint32_t>(r.images.size());
    if (g_backend && g_imgui_pass == r.render_pass) {
        if (images != g_imgui_images) {
            ImGui_ImplVulkan_SetMinImageCount((std::max<uint32_t>)(2, (std::min)(r.min_images, images)));
            g_imgui_images = images;
        }
        return true;
    }
    if (!g_imgui) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        // Ctrl+Tab would open ImGui's window switcher over the console: no switcher (the console is one window)
        GImGui->ConfigNavWindowingKeyNext = ImGuiKey_None;
        GImGui->ConfigNavWindowingKeyPrev = ImGuiKey_None;
        ImGui::StyleColorsDark();
        if (g_hooks.fonts) g_hooks.fonts(io, static_cast<float>(r.extent.height));
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_last);
        g_imgui = true;
    }
    if (g_backend) {
        ImGui_ImplVulkan_Shutdown();  // the swapchain format changed: a new render pass
        g_backend = false;
    } else if (!ImGui_ImplVulkan_LoadFunctions(g_api, load_fn, nullptr)) {  // this device's functions
        log("overlay: could not load the Vulkan functions for ImGui");
        return false;
    }
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = g_api;
    ii.Instance = g_instance;
    ii.PhysicalDevice = g_phys;
    ii.Device = g_device;
    ii.QueueFamily = family;
    ii.Queue = queue;
    ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE + 8;
    ii.RenderPass = r.render_pass;
    ii.MinImageCount = (std::max<uint32_t>)(2, (std::min)(r.min_images, images));
    ii.ImageCount = (std::max)(ii.MinImageCount, images);
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        log("overlay: ImGui could not start on the game's Vulkan device");
        return false;
    }
    g_backend = true;
    g_imgui_pass = r.render_pass;
    g_imgui_images = images;
    return true;
}

bool render(VkQueue queue, const VkPresentInfoKHR* info, VkSemaphore* signal, void*) {
    if (info->swapchainCount != 1 || !info->pImageIndices) return true;
    const uint32_t family = family_of(queue);
    if (family >= g_families.size() || !(g_families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        log("overlay: the game presents from a queue EMF cannot draw on; the console is off");
        return false;
    }
    // No lock is held while calling the driver: a fault there (caught by present_through's guard) must not leave the
    // book locked for the swapchain create/destroy calls that come later on the game's window thread.
    const VkSwapchainKHR sc = info->pSwapchains[0];
    SwapchainRecord r;
    {
        const SrwExclusive lock(g_book_lock);
        const SwapchainRecord* rec = g_book.find(sc);
        if (!rec || !rec->drawable) return true;  // nothing to draw on
        r = *rec;
    }
    if (!r.ready) {  // first draw on this swapchain (the game cannot destroy it while presenting it)
        if (!prepare(r, family)) {
            free_record(r);
            log("overlay: could not create the drawing objects for the swapchain; the console is off");
            return false;
        }
        const SrwExclusive lock(g_book_lock);
        if (SwapchainRecord* rec = g_book.find(sc)) *rec = r;
    }
    if (!init_imgui(r, queue, family)) return false;
    const uint32_t i = info->pImageIndices[0];
    if (i >= r.images.size()) return true;
    if (D.WaitForFences(g_device, 1, &r.fences[i], VK_TRUE, 100000000ull) != VK_SUCCESS) return true;  // skip a frame
    D.ResetFences(g_device, 1, &r.fences[i]);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(r.extent.width), static_cast<float>(r.extent.height));
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    io.DeltaTime = std::clamp(static_cast<float>(now.QuadPart - g_last.QuadPart) / static_cast<float>(g_freq.QuadPart),
                              1.0f / 1000.0f, 0.25f);
    g_last = now;
    if (g_hooks.before_frame) g_hooks.before_frame(io);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (g_hooks.build) g_hooks.build(io.DisplaySize);
    ImGui::Render();

    VkCommandBuffer cmd = r.commands[i];
    D.ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    D.BeginCommandBuffer(cmd, &bi);
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = r.render_pass;
    rp.framebuffer = r.framebuffers[i];
    rp.renderArea.extent = r.extent;
    D.CmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    D.CmdEndRenderPass(cmd);
    D.EndCommandBuffer(cmd);

    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = info->waitSemaphoreCount;
    si.pWaitSemaphores = info->pWaitSemaphores;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &r.done[i];
    if (D.QueueSubmit(queue, 1, &si, r.fences[i]) != VK_SUCCESS) {
        log("overlay: drawing the console failed (vkQueueSubmit); the console is off");
        return false;
    }
    *signal = r.done[i];
    static bool first_draw = true;
    if (first_draw) {
        first_draw = false;
        log("overlay: first console frame drawn (" + std::to_string(r.extent.width) + "x" + std::to_string(r.extent.height) +
            ", " + std::to_string(r.images.size()) + " images)");
    }
    return true;
}

}  // namespace

// ---- called by the layer ----

void overlay_log(const std::string& line) { log(line); }

bool overlay_attach_device(const LayerDevice& d) {
    if (g_device || !d.device || !d.gdpa || !d.gipa) return false;
    g_instance = d.instance;
    g_api = d.api_version;
    g_gipa = d.gipa;
    g_gdpa = d.gdpa;
    g_set_loader_data = d.set_loader_data;
    g_phys = d.phys;
    g_device = d.device;
    auto qfp = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        d.gipa(d.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    g_families.clear();
    if (qfp) {
        uint32_t n = 0;
        qfp(d.phys, &n, nullptr);
        g_families.resize(n);
        qfp(d.phys, &n, g_families.data());
    }
#define EMF_LOAD(name) D.name = reinterpret_cast<PFN_vk##name>(d.gdpa(d.device, "vk" #name))
    EMF_LOAD(GetSwapchainImagesKHR);
    EMF_LOAD(CreateImageView);
    EMF_LOAD(DestroyImageView);
    EMF_LOAD(CreateFramebuffer);
    EMF_LOAD(DestroyFramebuffer);
    EMF_LOAD(CreateRenderPass);
    EMF_LOAD(CreateCommandPool);
    EMF_LOAD(AllocateCommandBuffers);
    EMF_LOAD(FreeCommandBuffers);
    EMF_LOAD(BeginCommandBuffer);
    EMF_LOAD(EndCommandBuffer);
    EMF_LOAD(ResetCommandBuffer);
    EMF_LOAD(CmdBeginRenderPass);
    EMF_LOAD(CmdEndRenderPass);
    EMF_LOAD(QueueSubmit);
    EMF_LOAD(CreateFence);
    EMF_LOAD(DestroyFence);
    EMF_LOAD(WaitForFences);
    EMF_LOAD(ResetFences);
    EMF_LOAD(CreateSemaphore);
    EMF_LOAD(DestroySemaphore);
    EMF_LOAD(DestroyRenderPass);
    EMF_LOAD(DestroyCommandPool);
    EMF_LOAD(DeviceWaitIdle);
#undef EMF_LOAD
    o_create_swapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(d.gdpa(d.device, "vkCreateSwapchainKHR"));
    o_destroy_swapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(d.gdpa(d.device, "vkDestroySwapchainKHR"));
    o_present = reinterpret_cast<PresentFn>(d.gdpa(d.device, "vkQueuePresentKHR"));
    bool all = true;
    for (const void* const* f = reinterpret_cast<const void* const*>(&D);
         f < reinterpret_cast<const void* const*>(&D + 1); ++f)
        all &= *f != nullptr;
    const bool ok = all && !g_families.empty() && o_create_swapchain && o_destroy_swapchain && o_present;
    g_disabled = !ok;
    g_device_ready.store(true, std::memory_order_release);
    log(std::string("overlay: the game's Vulkan device came through EMF's layer") +
        (ok ? "" : " (some functions are missing; the console is off)") +
        (d.set_loader_data ? "" : " (no loader-data callback)"));
    capture_attach(d);  // the debug bridge's screenshots (nothing happens until one is asked for)
    return true;
}

void overlay_detach_device(VkDevice device) {
    if (!device || device != g_device) return;
    g_device_ready = false;
    if (D.DeviceWaitIdle) D.DeviceWaitIdle(device);
    capture_detach(device);
    if (g_backend) {
        ImGui_ImplVulkan_Shutdown();
        g_backend = false;
        g_imgui_pass = VK_NULL_HANDLE;
        g_imgui_images = 0;
    }
    std::vector<SwapchainRecord> records;
    {
        const SrwExclusive lock(g_book_lock);
        records.swap(g_book.all());
    }
    for (auto& rec : records) free_record(rec);
    for (const auto& [format, pass] : g_passes) D.DestroyRenderPass(device, pass, nullptr);
    g_passes.clear();
    if (g_pool) D.DestroyCommandPool(device, g_pool, nullptr);
    g_pool = VK_NULL_HANDLE;
    g_pool_family = UINT32_MAX;
    {
        const SrwExclusive lock(g_queue_lock);
        g_queues.clear();
    }
    g_device = VK_NULL_HANDLE;  // a later device can take over
    g_disabled = false;
    log("overlay: the game's Vulkan device was destroyed; EMF freed its drawing objects");
}

void overlay_remember_queue(VkQueue q, uint32_t family) {
    if (!q) return;
    const SrwExclusive lock(g_queue_lock);
    bool known = false;
    for (const auto& [queue, fam] : g_queues) known |= queue == q;
    if (!known) g_queues.emplace_back(q, family);
}

VkResult overlay_present(VkQueue queue, const VkPresentInfoKHR* info) {
    g_last_present_ms.store(GetTickCount64(), std::memory_order_relaxed);
    {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        if (g_present_count.fetch_add(1, std::memory_order_relaxed) && freq.QuadPart) {
            const double ns = static_cast<double>(now.QuadPart - g_last_present_qpc.QuadPart) * 1e9 /
                              static_cast<double>(freq.QuadPart);
            const double old = static_cast<double>(g_frame_ns.load(std::memory_order_relaxed));
            g_frame_ns.store(static_cast<uint64_t>(old ? old * 0.9 + ns * 0.1 : ns), std::memory_order_relaxed);
        }
        g_last_present_qpc = now;
    }
    static bool first = true;
    if (first) {
        first = false;
        log("overlay: the game's frames pass through EMF's layer");
    }
    const bool visible = g_hooks_set.load(std::memory_order_acquire) && g_hooks.visible && g_hooks.visible();
    const bool was_disabled = g_disabled.load();
    // A screenshot without the overlay copies the image first; the overlay and the present then wait on that copy.
    const VkSemaphore copied = capture_before_overlay(queue, info, family_of);
    VkPresentInfoKHR after_copy{};
    if (copied) {
        after_copy = *info;
        after_copy.waitSemaphoreCount = 1;
        after_copy.pWaitSemaphores = &copied;
    }
    const VkResult r =
        present_through(queue, copied ? &after_copy : info, visible, &g_disabled, render, nullptr, present_next);
    if (!was_disabled && g_disabled.load()) {
        const char* a = emf_imgui_last_assert();
        log(std::string("overlay: turned off after a failure while drawing") +
            (a && *a ? std::string(" (") + a + ")" : ""));
        if (g_hooks.turned_off) g_hooks.turned_off();
    }
    return r;
}

VkResult overlay_create_swapchain(VkDevice dev, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* alloc,
                                  VkSwapchainKHR* out) {
    VkSwapchainCreateInfoKHR copy = *ci;
    copy.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;  // we render into the images
    // the debug bridge's screenshots copy from the images: only with the bridge on and when the surface allows it
    const bool add_read = capture_wanted() && !(ci->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
                          capture_surface_readable(ci->surface);
    if (add_read) copy.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult res = o_create_swapchain(dev, &copy, alloc, out);
    // A failed create retires oldSwapchain (the spec): a retry must not name it again (VUID 01933).
    if (res != VK_SUCCESS && add_read) {  // once without the read use only, still drawable
        copy.imageUsage &= ~static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        copy.oldSwapchain = VK_NULL_HANDLE;
        res = o_create_swapchain(dev, &copy, alloc, out);
    }
    bool drawable = true;
    if (res != VK_SUCCESS && copy.imageUsage != ci->imageUsage) {  // the driver refused the extra usage: as asked
        copy = *ci;
        copy.oldSwapchain = VK_NULL_HANDLE;
        res = o_create_swapchain(dev, &copy, alloc, out);
        drawable = (ci->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    }
    if (res == VK_SUCCESS && out) {
        const bool readable = (copy.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
        const char* space = ci->imageColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT           ? ", HDR10"
                            : ci->imageColorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT ? ", scRGB"
                                                                                             : "";
        log("overlay: swapchain " + std::to_string(ci->imageExtent.width) + "x" + std::to_string(ci->imageExtent.height) +
            " format " + std::to_string(ci->imageFormat) + " (" + capture_format_words(ci->imageFormat) + space + ")" +
            (drawable ? "" : " (cannot draw into it)") +
            (readable && capture_wanted() ? " (readable for screenshots)" : ""));
        {
            const SrwExclusive lock(g_book_lock);
            g_book.on_create(*out, ci->imageFormat, ci->imageExtent, ci->minImageCount, ci->oldSwapchain).drawable =
                drawable;
        }
        capture_swapchain_created(*out, ci->imageFormat, ci->imageColorSpace, ci->imageExtent, readable);
    }
    return res;
}

void overlay_destroy_swapchain(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks* alloc) {
    if (sc) {
        std::optional<SwapchainRecord> rec;
        {
            const SrwExclusive lock(g_book_lock);
            rec = g_book.take(sc);
        }
        if (rec) free_record(*rec);
        capture_swapchain_destroyed(sc);
    }
    o_destroy_swapchain(dev, sc, alloc);
}

// ---- the console side ----

void set_vulkan_log(std::function<void(const char*)> fn) {
    std::vector<std::string> pending;
    {
        const SrwExclusive lock(g_log_lock);
        g_log = fn;
        pending.swap(g_pending_log);
    }
    if (fn)
        for (const auto& s : pending) fn(s.c_str());  // outside the lock, like log()
}

void set_vulkan_overlay(VkOverlayHooks hooks) {
    if (g_hooks_set.load()) return;
    g_hooks = std::move(hooks);
    g_hooks_set.store(true, std::memory_order_release);
}

bool vulkan_overlay_ready() {
    return g_device_ready.load() && !g_disabled.load() && GetTickCount64() - g_last_present_ms.load() < 2000;
}

VulkanFrameStats vulkan_frame_stats() {
    VulkanFrameStats s;
    s.device = g_device_ready.load();
    s.presents = g_present_count.load(std::memory_order_relaxed);
    s.frame_ms = static_cast<double>(g_frame_ns.load(std::memory_order_relaxed)) / 1e6;
    s.recent = GetTickCount64() - g_last_present_ms.load() < 2000;
    return s;
}

const char* vulkan_overlay_ready_reason() {
    if (g_disabled.load()) return "overlay: the console is off after a failure (see the lines above)";
    if (!g_device_ready.load())
        return "overlay: the console cannot be shown: the game's Vulkan device did not come through EMF's layer (is "
               "emf\\vk\\VkLayer_EMF_console.json there, and was the game started with EMF installed?)";
    if (GetTickCount64() - g_last_present_ms.load() >= 2000)
        return "overlay: the console cannot be shown: no game frames passed through EMF's layer in the last 2 s";
    return "overlay: the console is ready";
}

}  // namespace emf::core
