#include <doctest.h>

#include <cstring>
#include <vector>

#include "common/pattern_find.h"

using namespace emf;

namespace {
// A fake image: 0x1000 bytes at base 0x140000000; the "section" is the whole buffer.
struct Image {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x1000, 0xCC);
    uintptr_t base = 0x140000000ull;
    void put(size_t at, std::initializer_list<uint8_t> b) {
        size_t i = at;
        for (uint8_t x : b) bytes[i++] = x;
    }
    void put_rel32(size_t at, int32_t v) { std::memcpy(&bytes[at], &v, 4); }
    FindResult find(const PatternQuery& q) const { return find_unique(bytes, base, base, bytes.size(), q); }
};
}  // namespace

TEST_CASE("pattern find: one match gives its address (plus the offset); none, several and bad text are refused") {
    Image img;
    img.put(0x100, {0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44, 0xC3});
    PatternQuery q;
    q.bytes = "48 8B 05 ?? ?? ?? ?? C3";
    auto r = img.find(q);
    CHECK(r.error.empty());
    CHECK(r.matches == 1);
    CHECK(r.address == img.base + 0x100);
    q.offset = 7;
    CHECK(img.find(q).address == img.base + 0x107);

    PatternQuery none;
    none.bytes = "DE AD BE EF";
    r = img.find(none);
    CHECK(r.address == 0);
    CHECK(r.matches == 0);
    CHECK(r.error.find("not found") != std::string::npos);

    img.put(0x200, {0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0xC3});
    q.offset = 0;
    r = img.find(q);
    CHECK(r.address == 0);
    CHECK(r.matches == 2);
    CHECK(r.error.find("2 times") != std::string::npos);

    PatternQuery bad;
    bad.bytes = "48 ZZ";
    r = img.find(bad);
    CHECK(r.address == 0);
    CHECK(r.error.find("not a pattern") != std::string::npos);
}

TEST_CASE("pattern find: rel32 follow for a call, a RIP-relative load and an instruction with a tail") {
    Image img;
    // call +0x50 at 0x300: E8 rel32, next instruction at 0x305 -> target 0x355
    img.put(0x300, {0xE8});
    img.put_rel32(0x301, 0x50);
    img.put(0x305, {0x90, 0x90, 0x5A});
    PatternQuery call;
    call.bytes = "E8 ?? ?? ?? ?? 90 90 5A";
    call.offset = 1;
    call.follow_rel32 = true;
    CHECK(img.find(call).address == img.base + 0x355);

    // mov rax, [rip-0x10] at 0x400: 48 8B 05 rel32 (7 bytes) -> 0x407 - 0x10 = 0x3F7
    img.put(0x400, {0x48, 0x8B, 0x05});
    img.put_rel32(0x403, -0x10);
    img.put(0x407, {0x5B, 0x5C});
    PatternQuery load;
    load.bytes = "48 8B 05 ?? ?? ?? ?? 5B 5C";
    load.offset = 3;
    load.follow_rel32 = true;
    CHECK(img.find(load).address == img.base + 0x3F7);

    // cmp dword [rip+0x20], 1 at 0x500: 83 3D rel32 01 (7 bytes; one byte after the rel32) -> 0x507 + 0x20
    img.put(0x500, {0x83, 0x3D});
    img.put_rel32(0x502, 0x20);
    img.put(0x506, {0x01, 0x5D, 0x5E});
    PatternQuery cmp;
    cmp.bytes = "83 3D ?? ?? ?? ?? 01 5D 5E";
    cmp.offset = 2;
    cmp.follow_rel32 = true;
    cmp.rel32_tail = 1;
    CHECK(img.find(cmp).address == img.base + 0x527);
}

TEST_CASE("pattern find: a follow that leaves the image, and an offset past the region, are refused") {
    Image img;
    img.put(0x600, {0xE8});
    img.put_rel32(0x601, 0x7FFFFFF0);  // far outside
    img.put(0x605, {0x61, 0x62, 0x63});
    PatternQuery q;
    q.bytes = "E8 ?? ?? ?? ?? 61 62 63";
    q.offset = 1;
    q.follow_rel32 = true;
    auto r = img.find(q);
    CHECK(r.address == 0);
    CHECK(r.error.find("outside") != std::string::npos);

    img.put(0xFF0, {0x71, 0x72, 0x73});
    PatternQuery past;
    past.bytes = "71 72 73";
    past.offset = 0x100;
    past.follow_rel32 = true;
    r = img.find(past);
    CHECK(r.address == 0);
    CHECK_FALSE(r.error.empty());
}

TEST_CASE("pattern find: without a follow, match + offset must still lie in the image") {
    Image img;
    img.put(0xFF0, {0x71, 0x72, 0x73});
    PatternQuery q;
    q.bytes = "71 72 73";
    q.offset = 0x100;  // past the end
    auto r = img.find(q);
    CHECK(r.address == 0);
    CHECK(r.matches == 1);
    CHECK(r.error.find("outside") != std::string::npos);
    q.offset = 0x10;  // exactly the end: not in the image
    CHECK(img.find(q).address == 0);
    q.offset = -0x1000;  // before the start
    r = img.find(q);
    CHECK(r.address == 0);
    CHECK(r.error.find("outside") != std::string::npos);
    q.offset = 0xF;  // the last byte
    CHECK(img.find(q).address == img.base + 0xFFF);
    q.offset = -0xFF0;  // the first byte
    CHECK(img.find(q).address == img.base);

    // outside the searched section but inside the image is fine (the address need not be code)
    const auto region = std::span<const uint8_t>(img.bytes).subspan(0xF00, 0x100);
    PatternQuery other;
    other.bytes = "71 72 73";
    other.offset = -0x800;
    r = find_unique(region, img.base + 0xF00, img.base, img.bytes.size(), other);
    CHECK(r.address == img.base + 0x7F0);
}
