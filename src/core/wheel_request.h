#pragma once
// A mod's request for a radial wheel (API v10 wheel_open), as the core copies it.
#include <cstdint>
#include <string>
#include <vector>

#include "emf/emf_api.h"

namespace emf::core {

struct WheelRequest {
    std::string title;
    std::vector<std::string> items;
    uint16_t hold_vk = 0;  // held to keep it open, its release picks (0 = click to pick)
    uint8_t world_kind = EMF_WORLD_SERVER;  // where the choice is delivered
    EmfWheelCallback cb = nullptr;
    void* user = nullptr;
};
constexpr size_t kMaxWheelItems = 64;

}  // namespace emf::core
