// nxm:// links, what Nexus's "Mod Manager Download" button opens: parsed strictly, Enshrouded only, and the link's
// one-time key never echoed in a message.
#include <doctest.h>

#include <string>

#include "nexus/nxm.h"

using namespace emf::nexus;

TEST_CASE("nxm: a Mod Manager Download link gives the mod, the file, the key, its expiry and the user") {
    const auto r = parse_nxm("nxm://enshrouded/mods/170/files/1197?key=Ab3_x-Y9&expires=1792790875&user_id=12345");
    REQUIRE_MESSAGE(r.link, r.error);
    CHECK(r.link->game == "enshrouded");
    CHECK(r.link->mod_id == 170);
    CHECK(r.link->file_id == 1197);
    CHECK(r.link->key == "Ab3_x-Y9");
    CHECK(r.link->expires == 1792790875);
    CHECK(r.link->user_id == 12345);
    CHECK(r.error.empty());
}

TEST_CASE("nxm: case in the scheme and the game, a trailing slash and a fragment are accepted") {
    auto r = parse_nxm("NXM://Enshrouded/mods/170/files/1197/?key=k1&expires=5#x");
    REQUIRE_MESSAGE(r.link, r.error);
    CHECK(r.link->game == "enshrouded");
    CHECK(r.link->key == "k1");
    CHECK(r.link->expires == 5);
    r = parse_nxm("  nxm://enshrouded/mods/5/files/8  ");  // spaces around, as a command line can leave them
    REQUIRE_MESSAGE(r.link, r.error);
    CHECK(r.link->mod_id == 5);
}

TEST_CASE("nxm: a link without a key (a Premium member's) is still a link; unknown parameters are ignored") {
    const auto r = parse_nxm("nxm://enshrouded/mods/10/files/146?campaign=x");
    REQUIRE_MESSAGE(r.link, r.error);
    CHECK(r.link->key.empty());
    CHECK(r.link->expires == 0);
    CHECK(r.link->user_id == 0);
}

TEST_CASE("nxm: a percent-encoded key is decoded") {
    const auto r = parse_nxm("nxm://enshrouded/mods/1/files/2?key=ab%2Bcd%3D%3D&expires=9");
    REQUIRE_MESSAGE(r.link, r.error);
    CHECK(r.link->key == "ab+cd==");
}

TEST_CASE("nxm: links for other games are refused with a message that names the game and not the key") {
    const auto r = parse_nxm("nxm://skyrimspecialedition/mods/266/files/1000?key=SECRETKEY&expires=1&user_id=2");
    CHECK_FALSE(r.link);
    CHECK(r.other_game);
    CHECK(r.error.find("skyrimspecialedition") != std::string::npos);
    CHECK(r.error.find("Enshrouded") != std::string::npos);
    CHECK(r.error.find("SECRETKEY") == std::string::npos);
}

TEST_CASE("nxm: collections, odd paths and bad numbers are refused, never echoing the query") {
    const char* bad[] = {
        "nxm://enshrouded/collections/abcd/revisions/3?key=SECRETKEY",
        "nxm://enshrouded/mods/170?key=SECRETKEY",
        "nxm://enshrouded/mods/170/files/?key=SECRETKEY",
        "nxm://enshrouded/mods/0/files/5?key=SECRETKEY",
        "nxm://enshrouded/mods/abc/files/5?key=SECRETKEY",
        "nxm://enshrouded/mods/1234567890123/files/5?key=SECRETKEY",
        "nxm://enshrouded/mods/1/files/2/extra?key=SECRETKEY",
        "nxm://enshrouded/mods/-1/files/2?key=SECRETKEY",
        "nxm://enshrouded/mods/1/files/2?key=SECRETKEY&expires=soon",
        "nxm://enshrouded/mods/1/files/2?key=SECRETKEY&user_id=x",
        "nxm:///mods/1/files/2?key=SECRETKEY",
        "nxm://en$hrouded/mods/1/files/2?key=SECRETKEY",
    };
    for (const char* url : bad) {
        const auto r = parse_nxm(url);
        CHECK_MESSAGE(!r.link, url);
        CHECK_MESSAGE(!r.error.empty(), url);
        CHECK_MESSAGE(r.error.find("SECRETKEY") == std::string::npos, url);
    }
    CHECK(parse_nxm("nxm://enshrouded/collections/abcd/revisions/3").error.find("collection") != std::string::npos);
}

TEST_CASE("nxm: a key with characters a key never has is refused") {
    CHECK_FALSE(parse_nxm("nxm://enshrouded/mods/1/files/2?key=a%0Db&expires=1").link);   // CR
    CHECK_FALSE(parse_nxm("nxm://enshrouded/mods/1/files/2?key=a%20b&expires=1").link);   // space
    CHECK_FALSE(parse_nxm("nxm://enshrouded/mods/1/files/2?key=a\"b&expires=1").link);
    CHECK_FALSE(parse_nxm("nxm://enshrouded/mods/1/files/2?key=" + std::string(300, 'a')).link);
}

TEST_CASE("nxm: anything that is not an nxm:// link is refused; so is a very long one") {
    CHECK_FALSE(parse_nxm("https://www.nexusmods.com/enshrouded/mods/170").link);
    CHECK_FALSE(parse_nxm("").link);
    CHECK_FALSE(parse_nxm("nxm:enshrouded/mods/1/files/2").link);
    CHECK_FALSE(parse_nxm("nxm://enshrouded/mods/1/files/2?" + std::string(5000, 'a')).link);
    CHECK_FALSE(parse_nxm(std::string("nxm://enshrouded/mods/1/files/2\0x", 33)).link);  // a NUL inside
}

TEST_CASE("nxm: expiry and the file's page on Nexus") {
    const auto r = parse_nxm("nxm://enshrouded/mods/170/files/1197?key=k&expires=1000");
    REQUIRE(r.link);
    CHECK_FALSE(nxm_expired(*r.link, 999));
    CHECK(nxm_expired(*r.link, 1000));
    CHECK(nxm_expired(*r.link, 2000));
    NxmLink no_expiry = *r.link;
    no_expiry.expires = 0;
    CHECK_FALSE(nxm_expired(no_expiry, 2000000000));
    CHECK(nexus_file_page(170, 1197) == "https://www.nexusmods.com/enshrouded/mods/170?tab=files&file_id=1197");
}
