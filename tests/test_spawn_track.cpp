#include <doctest.h>

#include <string>

#include "../mods/sample_mod/spawn_track.h"

TEST_CASE("parse_guid: textual byte order") {
    uint8_t g[16];
    REQUIRE(sample::parse_guid("e284b901-c3dd-4282-8a4e-0a618e5763f5", g, false));
    CHECK(g[0] == 0xe2);
    CHECK(g[3] == 0x01);
    CHECK(g[4] == 0xc3);
    CHECK(g[8] == 0x8a);
    CHECK(g[15] == 0xf5);
}

TEST_CASE("parse_guid: Windows GUID layout (first three groups little-endian)") {
    uint8_t g[16];
    REQUIRE(sample::parse_guid("e284b901-c3dd-4282-8a4e-0a618e5763f5", g, true));
    CHECK(g[0] == 0x01);
    CHECK(g[3] == 0xe2);
    CHECK(g[4] == 0xdd);
    CHECK(g[5] == 0xc3);
    CHECK(g[6] == 0x82);
    CHECK(g[7] == 0x42);
    CHECK(g[8] == 0x8a);
    CHECK(g[15] == 0xf5);
}

TEST_CASE("parse_guid rejects malformed text") {
    uint8_t g[16];
    CHECK_FALSE(sample::parse_guid("", g, false));
    CHECK_FALSE(sample::parse_guid("e284b901c3dd42828a4e0a618e5763f5", g, false));
    CHECK_FALSE(sample::parse_guid("e284b901-c3dd-4282-8a4e-0a618e5763fZ", g, false));
}

TEST_CASE("spawn_key names the key that triggered each spawn slot") {
    CHECK(std::string(sample::spawn_key(0)) == "F9");
    CHECK(std::string(sample::spawn_key(1)) == "F11");
}

TEST_CASE("sample_mod: guid_layout is the manifest's choice (stored as its index), older ini text still reads") {
    CHECK(sample::guid_layout_ms(L"0"));  // what the Mods tab and the core store: choice 0 = ms
    CHECK_FALSE(sample::guid_layout_ms(L"1"));
    CHECK(sample::guid_layout_ms(L""));  // no value: the default (ms)
    CHECK(sample::guid_layout_ms(L"ms"));
    CHECK_FALSE(sample::guid_layout_ms(L"raw"));
    CHECK_FALSE(sample::guid_layout_ms(L"text"));
}
