#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace emf {

// A byte pattern with wildcards. mask[i] == 1 means bytes[i] must match.
struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;
};

// Parses "48 8B ?? 05" style text. Returns an empty pattern on any invalid token.
Pattern parse_pattern(std::string_view text);

struct ScanResult {
    size_t count = 0;     // number of matches found, capped at 2
    uintptr_t first = 0;  // address of the first match (region_base + offset)
};

// Scans `region` (which starts at address `region_base`). Never reads past the region.
ScanResult scan(std::span<const uint8_t> region, uintptr_t region_base, const Pattern& pattern);

}  // namespace emf
