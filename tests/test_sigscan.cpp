#include <doctest.h>
#include "common/sigscan.h"

using emf::parse_pattern;
using emf::scan;

TEST_CASE("parse_pattern reads hex bytes and ?? wildcards") {
    auto p = parse_pattern("48 8B ?? 05");
    REQUIRE(p.bytes.size() == 4);
    CHECK(p.bytes[0] == 0x48);
    CHECK(p.bytes[1] == 0x8B);
    CHECK(p.bytes[3] == 0x05);
    CHECK(p.mask == std::vector<uint8_t>{1, 1, 0, 1});
}

TEST_CASE("parse_pattern rejects invalid tokens") {
    CHECK(parse_pattern("48 ZZ").bytes.empty());
    CHECK(parse_pattern("").bytes.empty());
    CHECK(parse_pattern("4").bytes.empty());
}

TEST_CASE("scan finds a single match and reports its address") {
    const uint8_t mem[] = {0x90, 0x48, 0x8B, 0x11, 0x05, 0x90};
    auto r = scan(mem, 0x1000, parse_pattern("48 8B ?? 05"));
    CHECK(r.count == 1);
    CHECK(r.first == 0x1001);
}

TEST_CASE("scan counts duplicates (capped at 2) and keeps the first address") {
    const uint8_t mem[] = {0x48, 0x8B, 0x00, 0x05, 0x48, 0x8B, 0x7F, 0x05, 0x48, 0x8B, 0x01, 0x05};
    auto r = scan(mem, 0x2000, parse_pattern("48 8B ?? 05"));
    CHECK(r.count == 2);
    CHECK(r.first == 0x2000);
}

TEST_CASE("scan reports zero when the pattern is absent") {
    const uint8_t mem[] = {0x90, 0x90, 0x90, 0x90};
    CHECK(scan(mem, 0, parse_pattern("48 8B ?? 05")).count == 0);
}

TEST_CASE("scan never reads past the end of the region") {
    // The full pattern exists in the buffer, but only the first 3 bytes are in the region.
    const uint8_t mem[] = {0x48, 0x8B, 0x11, 0x05};
    CHECK(scan(std::span<const uint8_t>(mem, 3), 0, parse_pattern("48 8B ?? 05")).count == 0);
}

TEST_CASE("scan with an empty pattern finds nothing") {
    const uint8_t mem[] = {0x48};
    CHECK(scan(mem, 0, emf::Pattern{}).count == 0);
}
