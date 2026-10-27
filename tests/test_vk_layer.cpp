// EMF's Vulkan layer with a fake "next" in the chain: negotiation, instance chain walking, and which functions the
// layer answers itself.
#include <doctest.h>

#include <cstring>
#include <string>

#include "core/overlay/vk_layer.h"

using namespace emf::core;

namespace {
// A dispatchable handle: the loader puts its dispatch table pointer first; layers key their tables by it.
struct FakeDispatchable {
    void* loader_table;
};
void* g_fake_table = reinterpret_cast<void*>(uintptr_t(0xD15)), *g_fake_table2 = reinterpret_cast<void*>(uintptr_t(0xD16));
FakeDispatchable g_fake_instance{g_fake_table};
int g_next_creates = 0;
const void* g_next_create_link = nullptr;
int g_foo_calls = 0;

void VKAPI_CALL next_foo() { ++g_foo_calls; }

VkResult VKAPI_CALL next_create_instance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks*, VkInstance* out) {
    ++g_next_creates;
    const auto* link = static_cast<const VkLayerInstanceCreateInfo*>(ci->pNext);
    g_next_create_link = link ? link->u.pLayerInfo : nullptr;
    *out = reinterpret_cast<VkInstance>(&g_fake_instance);
    return VK_SUCCESS;
}
void VKAPI_CALL next_destroy_instance(VkInstance, const VkAllocationCallbacks*) {}

PFN_vkVoidFunction VKAPI_CALL next_gipa(VkInstance, const char* name) {
    if (!std::strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(&next_create_instance);
    if (!std::strcmp(name, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(&next_destroy_instance);
    if (!std::strcmp(name, "vkFoo")) return reinterpret_cast<PFN_vkVoidFunction>(&next_foo);
    return nullptr;
}
}  // namespace

TEST_CASE("vk layer: negotiation hands the loader interface version 2 and the layer's GetProcAddr functions") {
    VkNegotiateLayerInterface n{};
    n.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
    n.loaderLayerInterfaceVersion = 2;
    REQUIRE(emf_vk_negotiate(&n) == VK_SUCCESS);
    CHECK(n.loaderLayerInterfaceVersion == 2);
    CHECK(n.pfnGetInstanceProcAddr != nullptr);
    CHECK(n.pfnGetDeviceProcAddr != nullptr);
    VkNegotiateLayerInterface old{};
    old.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
    old.loaderLayerInterfaceVersion = 1;
    CHECK(emf_vk_negotiate(&old) == VK_ERROR_INITIALIZATION_FAILED);  // too old a loader: stay out of the chain
}

TEST_CASE("vk layer: vkCreateInstance walks the chain; the instance's other functions are forwarded to the next layer") {
    VkNegotiateLayerInterface n{};
    n.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
    n.loaderLayerInterfaceVersion = 2;
    REQUIRE(emf_vk_negotiate(&n) == VK_SUCCESS);
    const auto gipa = n.pfnGetInstanceProcAddr;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    REQUIRE(create != nullptr);

    VkLayerInstanceLink below{nullptr, nullptr, nullptr};
    VkLayerInstanceLink mine{&below, next_gipa, nullptr};
    VkLayerInstanceCreateInfo link{};
    link.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
    link.function = VK_LAYER_LINK_INFO;
    link.u.pLayerInfo = &mine;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pNext = &link;
    VkInstance inst = VK_NULL_HANDLE;
    g_next_creates = 0;
    REQUIRE(create(&ci, nullptr, &inst) == VK_SUCCESS);
    CHECK(g_next_creates == 1);
    CHECK(inst == reinterpret_cast<VkInstance>(&g_fake_instance));
    CHECK(g_next_create_link == &below);  // the chain was advanced for the next layer

    // The layer answers its own functions and forwards the rest.
    CHECK(gipa(inst, "vkGetInstanceProcAddr") == reinterpret_cast<PFN_vkVoidFunction>(n.pfnGetInstanceProcAddr));
    CHECK(gipa(inst, "vkCreateDevice") != nullptr);
    CHECK(gipa(inst, "vkQueuePresentKHR") != nullptr);
    CHECK(gipa(inst, "vkFoo") == reinterpret_cast<PFN_vkVoidFunction>(&next_foo));
    CHECK(gipa(inst, "vkNothing") == nullptr);
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
    REQUIRE(destroy != nullptr);
    destroy(inst, nullptr);
    FakeDispatchable other{g_fake_table2};
    CHECK(gipa(reinterpret_cast<VkInstance>(&other), "vkFoo") == nullptr);  // unknown instance: nothing to forward to
}

TEST_CASE("vk layer: vkCreateInstance without the loader's link info fails instead of crashing") {
    VkNegotiateLayerInterface n{};
    n.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
    n.loaderLayerInterfaceVersion = 2;
    REQUIRE(emf_vk_negotiate(&n) == VK_SUCCESS);
    auto create = reinterpret_cast<PFN_vkCreateInstance>(n.pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance inst = VK_NULL_HANDLE;
    CHECK(create(&ci, nullptr, &inst) == VK_ERROR_INITIALIZATION_FAILED);
}
