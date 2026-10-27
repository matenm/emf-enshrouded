#include "pattern_find.h"

#include <cstring>

#include "sigscan.h"

namespace emf {

FindResult find_unique(std::span<const uint8_t> region, uintptr_t region_base, uintptr_t image_base, size_t image_size,
                       const PatternQuery& q) {
    FindResult r;
    const Pattern p = parse_pattern(q.bytes);
    if (p.bytes.empty()) {
        r.error = "\"" + q.bytes + "\" is not a pattern (hex bytes and ?? separated by spaces)";
        return r;
    }
    const ScanResult s = scan(region, region_base, p);
    r.matches = s.count;
    if (s.count == 0) {
        r.error = "not found";
        return r;
    }
    if (s.count > 1) {
        r.error = "matches 2 times or more: make it longer so it matches once";
        return r;
    }
    const int64_t at = static_cast<int64_t>(s.first) + q.offset;
    const int64_t ilo = static_cast<int64_t>(image_base), ihi = ilo + static_cast<int64_t>(image_size);
    if (!q.follow_rel32) {
        if (at < ilo || at >= ihi) {
            r.error = "the offset points outside the game exe";
            return r;
        }
        r.address = static_cast<uintptr_t>(at);
        return r;
    }
    const int64_t lo = static_cast<int64_t>(region_base), hi = lo + static_cast<int64_t>(region.size());
    if (at < lo || at + 4 > hi) {
        r.error = "the offset points outside the section";
        return r;
    }
    int32_t rel = 0;
    std::memcpy(&rel, region.data() + (at - lo), 4);
    const int64_t target = at + 4 + q.rel32_tail + rel;
    if (target < ilo || target >= ihi) {
        r.error = "the followed address lies outside the game exe";
        return r;
    }
    r.address = static_cast<uintptr_t>(target);
    return r;
}

}  // namespace emf
