#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

TEST_CASE("toolchain smoke test") { CHECK(sizeof(void*) == 8); }
