#include "vk_layer.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <unordered_map>

#include "vk_overlay.h"

// Layer rules followed here (Vulkan loader <-> layer interface, version 2):
// - vkCreateInstance / vkCreateDevice find the loader's link info in pNext, take the next layer's GetProcAddr from it,
//   advance the chain for the layer below, then call down.
// - Dispatchable handles (instance, physical device, device, queue, command buffer) start with the loader's dispatch
//   table pointer; a physical device shares its instance's, a queue its device's. Tables are keyed by that pointer.
namespace emf::core {
namespace {

void* key(const void* dispatchable) { return *static_cast<void* const*>(dispatchable); }

struct InstanceRec {
    VkInstance instance;
    uint32_t api;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy;
};

struct DeviceRec {
    VkDevice device;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice destroy;
    PFN_vkGetDeviceQueue get_queue;
    PFN_vkGetDeviceQueue2 get_queue2;
    PFN_vkCreateSwapchainKHR create_swapchain;
    PFN_vkDestroySwapchainKHR destroy_swapchain;
    PFN_vkQueuePresentKHR present;
    bool overlay;  // the device the console draws on
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<void*, InstanceRec> g_instances;
std::unordered_map<void*, DeviceRec> g_devices;

bool find_instance(const void* handle, InstanceRec* out) {
    if (!handle) return false;
    AcquireSRWLockShared(&g_lock);
    const auto it = g_instances.find(key(handle));
    const bool found = it != g_instances.end();
    if (found) *out = it->second;
    ReleaseSRWLockShared(&g_lock);
    return found;
}

bool find_device(const void* handle, DeviceRec* out) {
    if (!handle) return false;
    AcquireSRWLockShared(&g_lock);
    const auto it = g_devices.find(key(handle));
    const bool found = it != g_devices.end();
    if (found) *out = it->second;
    ReleaseSRWLockShared(&g_lock);
    return found;
}

// The loader's link info in a create info's pNext chain (const in the API; the loader expects layers to advance it).
template <typename Info, typename Create>
Info* find_link(const Create* ci, VkStructureType type, VkLayerFunction function) {
    for (auto* p = static_cast<const VkBaseInStructure*>(ci->pNext); p; p = p->pNext) {
        if (p->sType != type) continue;
        auto* info = reinterpret_cast<Info*>(const_cast<VkBaseInStructure*>(p));
        if (info->function == function) return info;
    }
    return nullptr;
}

bool has_extension(const VkDeviceCreateInfo* ci, const char* name) {
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i)
        if (ci->ppEnabledExtensionNames[i] && !std::strcmp(ci->ppEnabledExtensionNames[i], name)) return true;
    return false;
}

PFN_vkVoidFunction VKAPI_CALL layer_GetInstanceProcAddr(VkInstance instance, const char* name);
PFN_vkVoidFunction VKAPI_CALL layer_GetDeviceProcAddr(VkDevice device, const char* name);

VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc,
                                         VkInstance* out) {
    auto* link = find_link<VkLayerInstanceCreateInfo>(ci, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
                                                       VK_LAYER_LINK_INFO);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto next_create = reinterpret_cast<PFN_vkCreateInstance>(next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;  // the layer below sees its own link
    const VkResult r = next_create(ci, alloc, out);
    if (r != VK_SUCCESS) return r;
    InstanceRec rec{};
    rec.instance = *out;
    rec.api = ci->pApplicationInfo && ci->pApplicationInfo->apiVersion ? ci->pApplicationInfo->apiVersion
                                                                        : VK_API_VERSION_1_0;
    rec.gipa = next_gipa;
    rec.destroy = reinterpret_cast<PFN_vkDestroyInstance>(next_gipa(*out, "vkDestroyInstance"));
    AcquireSRWLockExclusive(&g_lock);
    g_instances[key(*out)] = rec;
    ReleaseSRWLockExclusive(&g_lock);
    return VK_SUCCESS;
}

void VKAPI_CALL layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    InstanceRec rec{};
    if (!find_instance(instance, &rec)) return;
    AcquireSRWLockExclusive(&g_lock);
    g_instances.erase(key(instance));
    ReleaseSRWLockExclusive(&g_lock);
    if (rec.destroy) rec.destroy(instance, alloc);
}

VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                       const VkAllocationCallbacks* alloc, VkDevice* out) {
    auto* link =
        find_link<VkLayerDeviceCreateInfo>(ci, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LAYER_LINK_INFO);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    InstanceRec inst{};
    const bool known_instance = find_instance(phys, &inst);  // a physical device carries its instance's key
    auto next_create = reinterpret_cast<PFN_vkCreateDevice>(
        next_gipa(known_instance ? inst.instance : VK_NULL_HANDLE, "vkCreateDevice"));
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    const VkResult r = next_create(phys, ci, alloc, out);
    if (r != VK_SUCCESS) return r;

    const VkDevice dev = *out;
    DeviceRec rec{};
    rec.device = dev;
    rec.gdpa = next_gdpa;
    rec.destroy = reinterpret_cast<PFN_vkDestroyDevice>(next_gdpa(dev, "vkDestroyDevice"));
    rec.get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(next_gdpa(dev, "vkGetDeviceQueue"));
    rec.get_queue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(next_gdpa(dev, "vkGetDeviceQueue2"));
    rec.create_swapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(next_gdpa(dev, "vkCreateSwapchainKHR"));
    rec.destroy_swapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(next_gdpa(dev, "vkDestroySwapchainKHR"));
    rec.present = reinterpret_cast<PFN_vkQueuePresentKHR>(next_gdpa(dev, "vkQueuePresentKHR"));
    if (known_instance && has_extension(ci, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        auto* loader_data = find_link<VkLayerDeviceCreateInfo>(ci, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                                                               VK_LOADER_DATA_CALLBACK);
        LayerDevice d;
        d.instance = inst.instance;
        d.api_version = inst.api;
        d.gipa = inst.gipa;
        d.phys = phys;
        d.device = dev;
        d.gdpa = next_gdpa;
        d.set_loader_data = loader_data ? loader_data->u.pfnSetDeviceLoaderData : nullptr;
        rec.overlay = overlay_attach_device(d);
    }
    AcquireSRWLockExclusive(&g_lock);
    g_devices[key(dev)] = rec;
    ReleaseSRWLockExclusive(&g_lock);
    return VK_SUCCESS;
}

void VKAPI_CALL layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    DeviceRec rec{};
    if (!find_device(device, &rec)) return;
    if (rec.overlay) overlay_detach_device(device);
    AcquireSRWLockExclusive(&g_lock);
    g_devices.erase(key(device));
    ReleaseSRWLockExclusive(&g_lock);
    if (rec.destroy) rec.destroy(device, alloc);
}

void VKAPI_CALL layer_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    DeviceRec rec{};
    if (!find_device(device, &rec) || !rec.get_queue) return;
    rec.get_queue(device, family, index, queue);
    if (rec.overlay && queue) overlay_remember_queue(*queue, family);
}

void VKAPI_CALL layer_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    DeviceRec rec{};
    if (!find_device(device, &rec) || !rec.get_queue2) return;
    rec.get_queue2(device, info, queue);
    if (rec.overlay && queue && info) overlay_remember_queue(*queue, info->queueFamilyIndex);
}

VkResult VKAPI_CALL layer_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                             const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    DeviceRec rec{};
    if (!find_device(device, &rec) || !rec.create_swapchain) return VK_ERROR_INITIALIZATION_FAILED;
    if (rec.overlay) return overlay_create_swapchain(device, ci, alloc, out);
    return rec.create_swapchain(device, ci, alloc, out);
}

void VKAPI_CALL layer_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                          const VkAllocationCallbacks* alloc) {
    DeviceRec rec{};
    if (!find_device(device, &rec) || !rec.destroy_swapchain) return;
    if (rec.overlay)
        overlay_destroy_swapchain(device, swapchain, alloc);
    else
        rec.destroy_swapchain(device, swapchain, alloc);
}

VkResult VKAPI_CALL layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    DeviceRec rec{};  // a queue carries its device's key
    if (!find_device(queue, &rec) || !rec.present) return VK_ERROR_DEVICE_LOST;
    if (rec.overlay) return overlay_present(queue, info);
    return rec.present(queue, info);
}

// The functions this layer answers itself (device-level ones are also handed out through vkGetInstanceProcAddr).
PFN_vkVoidFunction own_function(const char* name) {
#define EMF_OWN(fn) \
    if (!std::strcmp(name, "vk" #fn)) return reinterpret_cast<PFN_vkVoidFunction>(&layer_##fn)
    EMF_OWN(GetInstanceProcAddr);
    EMF_OWN(CreateInstance);
    EMF_OWN(DestroyInstance);
    EMF_OWN(CreateDevice);
    EMF_OWN(GetDeviceProcAddr);
    EMF_OWN(DestroyDevice);
    EMF_OWN(GetDeviceQueue);
    EMF_OWN(GetDeviceQueue2);
    EMF_OWN(CreateSwapchainKHR);
    EMF_OWN(DestroySwapchainKHR);
    EMF_OWN(QueuePresentKHR);
#undef EMF_OWN
    return nullptr;
}

PFN_vkVoidFunction VKAPI_CALL layer_GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    if (const auto own = own_function(name)) return own;
    InstanceRec rec{};
    if (!find_instance(instance, &rec)) return nullptr;
    return rec.gipa(instance, name);
}

PFN_vkVoidFunction VKAPI_CALL layer_GetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name) return nullptr;
    DeviceRec rec{};
    if (!find_device(device, &rec)) return nullptr;
    const PFN_vkVoidFunction next = rec.gdpa(device, name);
    if (const auto own = own_function(name)) {
        // Hand out a wrapper only for what the device has (no swapchain functions on a device without the extension).
        const bool always = !std::strcmp(name, "vkGetDeviceProcAddr") || !std::strcmp(name, "vkDestroyDevice");
        return always || next ? own : nullptr;
    }
    return next;
}

}  // namespace
}  // namespace emf::core

extern "C" __declspec(dllexport) VkResult VKAPI_CALL emf_vk_negotiate(VkNegotiateLayerInterface* version) {
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || version->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = emf::core::layer_GetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = emf::core::layer_GetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
