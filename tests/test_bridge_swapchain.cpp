// The swapchain create path with the debug bridge: images are made readable (TRANSFER_SRC) only with the bridge on and
// when the surface allows it; a refused create is retried once without the read use only (still drawable), and no
// retry names the old swapchain the failed call retired (VUID-VkSwapchainCreateInfoKHR-oldSwapchain-01933).
#include <windows.h>

#include <doctest.h>

#include <cstring>
#include <vector>

#include "core/bridge/switch.h"
#include "core/overlay/vk_capture.h"
#include "core/overlay/vk_overlay.h"

using namespace emf::core;

namespace {
struct FakeDispatchable {
    void* loader_table;
};
FakeDispatchable g_inst{nullptr}, g_phys{nullptr}, g_dev{nullptr};

struct Call {
    VkImageUsageFlags usage;
    VkSwapchainKHR old;
};
std::vector<Call> g_calls;
int g_fail = 0;  // how many creates fail next
VkImageUsageFlags g_supported = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

VkResult VKAPI_CALL ok_any() { return VK_SUCCESS; }
void VKAPI_CALL queue_families(VkPhysicalDevice, uint32_t* n, VkQueueFamilyProperties* p) {
    if (p) {
        *p = VkQueueFamilyProperties{};
        p->queueFlags = VK_QUEUE_GRAPHICS_BIT;
        p->queueCount = 1;
    }
    *n = 1;
}
void VKAPI_CALL memory_props(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* p) { *p = {}; }
VkResult VKAPI_CALL surface_caps(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* c) {
    *c = VkSurfaceCapabilitiesKHR{};
    c->supportedUsageFlags = g_supported;
    return VK_SUCCESS;
}
VkResult VKAPI_CALL create_swapchain(VkDevice, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks*,
                                     VkSwapchainKHR* out) {
    g_calls.push_back({ci->imageUsage, ci->oldSwapchain});
    if (g_fail > 0) {
        --g_fail;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *out = reinterpret_cast<VkSwapchainKHR>(uintptr_t{0x5C});
    return VK_SUCCESS;
}
PFN_vkVoidFunction VKAPI_CALL fake_gdpa(VkDevice, const char* name) {
    if (!std::strcmp(name, "vkCreateSwapchainKHR")) return reinterpret_cast<PFN_vkVoidFunction>(&create_swapchain);
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

std::vector<Call> create(VkDevice dev, VkSwapchainKHR old = VK_NULL_HANDLE) {
    g_calls.clear();
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = reinterpret_cast<VkSurfaceKHR>(uintptr_t{0x99});
    ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    ci.imageExtent = {640, 360};
    ci.minImageCount = 2;
    ci.imageUsage = VK_IMAGE_USAGE_STORAGE_BIT;  // what the game asks for
    ci.oldSwapchain = old;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    const VkResult r = overlay_create_swapchain(dev, &ci, nullptr, &sc);
    if (r == VK_SUCCESS) overlay_destroy_swapchain(dev, sc, nullptr);
    return g_calls;
}
}  // namespace

TEST_CASE("bridge switch: only bridge=1 turns it on") {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    const std::wstring ini = std::wstring(base) + L"emf_test_switch_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
    DeleteFileW(ini.c_str());
    CHECK_FALSE(emf::core::bridge::bridge_switch_on(ini));  // no file
    const wchar_t* const values[] = {L"1", L"0", L"2", L"yes", L"", L"-1"};
    const bool expected[] = {true, false, false, false, false, false};
    for (int i = 0; i < 6; ++i) {
        WritePrivateProfileStringW(L"debug", L"bridge", values[i], ini.c_str());
        CHECK_MESSAGE(emf::core::bridge::bridge_switch_on(ini) == expected[i], i);
    }
    WritePrivateProfileStringW(L"debug", L"bridge", nullptr, ini.c_str());  // the key gone
    CHECK_FALSE(emf::core::bridge::bridge_switch_on(ini));
    DeleteFileW(ini.c_str());
}

TEST_CASE("bridge swapchain: readable only with the bridge on and a surface that allows it; one retry without it") {
    LayerDevice d;
    d.instance = reinterpret_cast<VkInstance>(&g_inst);
    d.gipa = fake_gipa;
    d.phys = reinterpret_cast<VkPhysicalDevice>(&g_phys);
    d.device = reinterpret_cast<VkDevice>(&g_dev);
    d.gdpa = fake_gdpa;
    REQUIRE(overlay_attach_device(d));
    const VkImageUsageFlags asked = VK_IMAGE_USAGE_STORAGE_BIT;
    const VkImageUsageFlags color = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const VkImageUsageFlags read = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const auto old = reinterpret_cast<VkSwapchainKHR>(uintptr_t{0x50});

    capture_set_wanted(false);  // the bridge is off: no read use
    auto calls = create(d.device);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].usage == (asked | color));

    capture_set_wanted(true);
    calls = create(d.device);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].usage == (asked | color | read));

    g_supported = color;  // the surface does not allow reading
    calls = create(d.device);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].usage == (asked | color));
    g_supported = color | read;

    g_fail = 1;  // the driver refuses the read use: once more without it only, still drawable, no retired old swapchain
    calls = create(d.device, old);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].usage == (asked | color | read));
    CHECK(calls[0].old == old);
    CHECK(calls[1].usage == (asked | color));
    CHECK(calls[1].old == VK_NULL_HANDLE);

    g_fail = 2;  // and then as asked
    calls = create(d.device, old);
    REQUIRE(calls.size() == 3);
    CHECK(calls[2].usage == asked);
    CHECK(calls[2].old == VK_NULL_HANDLE);

    capture_set_wanted(false);
    g_fail = 1;  // without the bridge: the old single retry as asked, without the retired swapchain
    calls = create(d.device, old);
    REQUIRE(calls.size() == 2);
    CHECK(calls[1].usage == asked);
    CHECK(calls[1].old == VK_NULL_HANDLE);
    overlay_detach_device(d.device);
}
