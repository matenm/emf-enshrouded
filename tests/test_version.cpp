#include <doctest.h>

#include <fstream>
#include <string>

#include "core/version.h"

TEST_CASE("version: the core's version string comes from the VERSION file") {
    std::ifstream f(EMF_SOURCE_DIR "/VERSION");
    std::string v;
    REQUIRE(std::getline(f, v));
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    CHECK(std::string(emf::core::version()) == v);
    CHECK(std::string(emf::core::version_string()) == "emf_core " + v);
}
