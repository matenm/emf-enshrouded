#include "sigscan.h"

namespace emf {
namespace {
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

Pattern parse_pattern(std::string_view text) {
    Pattern p;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ' ') { ++i; continue; }
        if (i + 1 >= text.size() || (i + 2 < text.size() && text[i + 2] != ' ')) return {};
        if (text[i] == '?' && text[i + 1] == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(0);
        } else {
            int hi = hex_value(text[i]), lo = hex_value(text[i + 1]);
            if (hi < 0 || lo < 0) return {};
            p.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
            p.mask.push_back(1);
        }
        i += 2;
    }
    return p;
}

ScanResult scan(std::span<const uint8_t> region, uintptr_t region_base, const Pattern& pattern) {
    ScanResult r;
    const size_t n = pattern.bytes.size();
    if (n == 0 || region.size() < n) return r;
    // Anchor on the first non-wildcard byte to skip quickly.
    size_t anchor = 0;
    while (anchor < n && !pattern.mask[anchor]) ++anchor;
    for (size_t off = 0; off + n <= region.size(); ++off) {
        if (anchor < n && region[off + anchor] != pattern.bytes[anchor]) continue;
        bool ok = true;
        for (size_t k = 0; k < n; ++k) {
            if (pattern.mask[k] && region[off + k] != pattern.bytes[k]) { ok = false; break; }
        }
        if (!ok) continue;
        if (r.count == 0) r.first = region_base + off;
        if (++r.count == 2) break;
    }
    return r;
}

}  // namespace emf
