#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace emf {

// FNV-1a 32-bit, as used by the engine for program and singleton names.
constexpr uint32_t fnv1a32(const void* data, size_t size) {
    uint32_t h = 0x811c9dc5u;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}
constexpr uint32_t fnv1a32(std::string_view s) {
    uint32_t h = 0x811c9dc5u;
    for (char c : s) h = (h ^ static_cast<unsigned char>(c)) * 0x01000193u;
    return h;
}

}  // namespace emf
