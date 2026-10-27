#pragma once
// Finds a byte pattern that must match exactly once in a region (a section of the game exe), with an offset and an
// optional rel32 follow (call/jmp targets, RIP-relative data). Pure logic (unit-tested); API v14 find_pattern uses it.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace emf {

struct PatternQuery {
    std::string bytes;        // "48 8B 05 ?? ?? ?? ??"
    int32_t offset = 0;       // added to the match
    bool follow_rel32 = false;  // the int32 at match+offset is relative to the end of its instruction
    int32_t rel32_tail = 0;   // bytes between the rel32's end and the instruction's end (an immediate after it)
};

struct FindResult {
    uintptr_t address = 0;  // 0 when refused
    size_t matches = 0;     // capped at 2
    std::string error;      // why it was refused
};

// `region` starts at region_base; the result (match + offset, or the followed target) must lie in
// [image_base, image_base + image_size).
FindResult find_unique(std::span<const uint8_t> region, uintptr_t region_base, uintptr_t image_base, size_t image_size,
                       const PatternQuery& q);

}  // namespace emf
