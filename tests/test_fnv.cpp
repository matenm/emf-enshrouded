#include <doctest.h>
#include "common/fnv.h"

TEST_CASE("fnv1a32 matches the engine's name hash") {
    CHECK(emf::fnv1a32("LocalPlayerData") == 0x9117771du);
    CHECK(emf::fnv1a32("g38_daytime::Daytime") == 0x6ed63ed4u);
    CHECK(emf::fnv1a32("") == 0x811c9dc5u);
    static_assert(emf::fnv1a32("") == 0x811c9dc5u, "usable at compile time");
}

TEST_CASE("fnv1a32 over raw bytes equals the string overload") {
    const char s[] = "LocalPlayerData";
    CHECK(emf::fnv1a32(s, sizeof(s) - 1) == emf::fnv1a32("LocalPlayerData"));
}
