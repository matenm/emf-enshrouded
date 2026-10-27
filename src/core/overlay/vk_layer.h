#pragma once
// EMF's Vulkan layer, VK_LAYER_EMF_console. emf_core.dll is the layer library: the manifest emf\vk\VkLayer_EMF_console.json
// names it, and EMF's dinput8.dll loader enables it for the game process only (VK_ADD_LAYER_PATH + VK_INSTANCE_LAYERS,
// set before the game starts). The Vulkan loader then puts the layer in the game's instance and device chains, so the
// console sees the device, its swapchains and every present without patching any code.
#include "vulkan/vulkan.h"
#include "vulkan/vk_layer.h"

extern "C" __declspec(dllexport) VkResult VKAPI_CALL emf_vk_negotiate(VkNegotiateLayerInterface* version);

namespace emf::core {
constexpr const char* kVkLayerName = "VK_LAYER_EMF_console";
}
