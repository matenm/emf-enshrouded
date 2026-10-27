// Version ranges in mod.json "depends": SemVer ranges (^ ~ >= > <= < =, bare versions, 1.2.x, hyphens, AND with ','
// or spaces, OR with ||), the old ">=x" and "=x" forms with their old meaning, and how a version misses a range.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/manifest.h"

using namespace emf::core;

namespace {
Version v(const char* text) { return *parse_version(text); }

// Every version in `in` satisfies `range`, none in `out` does.
void matches(const char* range, std::vector<const char*> in, std::vector<const char*> out) {
    std::string error;
    const auto c = parse_constraint(range, &error);
    REQUIRE_MESSAGE(c, (std::string(range) + ": " + error));
    for (const char* x : in) CHECK_MESSAGE(satisfies(v(x), *c), (std::string(range) + " should take " + x));
    for (const char* x : out) CHECK_MESSAGE(!satisfies(v(x), *c), (std::string(range) + " should refuse " + x));
}

std::string refused(const char* range) {
    std::string error;
    const auto c = parse_constraint(range, &error);
    CHECK_MESSAGE(!c, (std::string(range) + " should be refused"));
    CHECK_MESSAGE(!error.empty(), (std::string(range) + " needs a reason"));
    return error;
}
}  // namespace

TEST_CASE("version range: * and x take every version") {
    for (const char* any : {"*", "x", "X", " * "}) {
        const auto c = parse_constraint(any);
        REQUIRE(c);
        CHECK(c->any());
        CHECK(satisfies(v("0.0.1"), *c));
        CHECK(satisfies(v("99.0.0"), *c));
        CHECK(c->describe() == "any version");
    }
}

TEST_CASE("version range: the forms EMF read before keep their meaning (>=x and =x, missing parts are 0)") {
    matches(">=1.1", {"1.1.0", "1.2.0", "2.0.0", "10.0.0"}, {"1.0.9", "0.9.0"});  // the shipped Lua mods' "lua": ">=1.1"
    matches(" >= 2 ", {"2.0.0", "3.1.0"}, {"1.9.9"});
    matches("=1.2.3", {"1.2.3"}, {"1.2.4", "1.2.2"});
    matches("=1.2", {"1.2.0"}, {"1.2.1", "1.1.0"});  // exactly 1.2.0, as before (a mod version "1.2" is 1.2.0)
    const auto c = parse_constraint(">=1.2");
    REQUIRE(c);
    CHECK(c->text() == ">=1.2");  // shown as the mod wrote it
    CHECK(c->describe() == "1.2.0 or newer");
}

TEST_CASE("version range: ^ allows changes that keep the left-most non-zero number") {
    matches("^1.2.0", {"1.2.0", "1.2.9", "1.9.9"}, {"1.1.9", "2.0.0", "0.9.0"});
    matches("^1.2", {"1.2.0", "1.9.0"}, {"1.1.0", "2.0.0"});
    matches("^1", {"1.0.0", "1.9.9"}, {"0.9.9", "2.0.0"});
    matches("^0.2.3", {"0.2.3", "0.2.9"}, {"0.2.2", "0.3.0", "1.0.0"});
    matches("^0.0.3", {"0.0.3"}, {"0.0.2", "0.0.4", "0.1.0"});
    matches("^0.0", {"0.0.0", "0.0.9"}, {"0.1.0"});
    matches("^0", {"0.0.0", "0.9.9"}, {"1.0.0"});
    matches("^1.x", {"1.0.0", "1.5.0"}, {"2.0.0"});
    CHECK(parse_constraint("^1.2.0")->describe() == "1.2.0 or newer, below 2.0.0");
}

TEST_CASE("version range: ~ allows patch changes (or minor changes when only the major is given)") {
    matches("~1.2.3", {"1.2.3", "1.2.9"}, {"1.2.2", "1.3.0"});
    matches("~1.2", {"1.2.0", "1.2.5"}, {"1.1.9", "1.3.0"});
    matches("~1", {"1.0.0", "1.9.0"}, {"0.9.0", "2.0.0"});
    matches("~0.2", {"0.2.0", "0.2.7"}, {"0.3.0"});
    CHECK(parse_constraint("~1.2")->describe() == "1.2.0 or newer, below 1.3.0");
}

TEST_CASE("version range: comparisons, joined with a comma or a space (all must hold)") {
    matches(">=1.0.0, <2.0.0", {"1.0.0", "1.9.9"}, {"0.9.9", "2.0.0"});
    matches(">=1.0.0 <2.0.0", {"1.0.0", "1.9.9"}, {"0.9.9", "2.0.0"});
    matches(">= 1.0 , < 2", {"1.0.0", "1.9.9"}, {"2.0.0"});
    matches(">1.2.3", {"1.2.4", "2.0.0"}, {"1.2.3", "1.0.0"});
    matches(">1.2", {"1.3.0"}, {"1.2.0", "1.2.9"});  // newer than every 1.2.x
    matches(">1", {"2.0.0"}, {"1.9.9"});
    matches("<=1.2.3", {"1.2.3", "0.1.0"}, {"1.2.4"});
    matches("<=1.2", {"1.2.0", "1.2.9"}, {"1.3.0"});  // up to every 1.2.x
    matches("<2", {"1.9.9", "0.0.1"}, {"2.0.0"});
    matches("<1.2.3", {"1.2.2"}, {"1.2.3"});
    CHECK(parse_constraint(">=1.0.0, <2.0.0")->describe() == "1.0.0 or newer, below 2.0.0");
    CHECK(parse_constraint("<=1.2.3")->describe() == "1.2.3 or older");
    CHECK(parse_constraint(">1.2.3")->describe() == "newer than 1.2.3");
    CHECK(parse_constraint("=1.2.3")->describe() == "exactly 1.2.3");
    CHECK(parse_constraint("<2")->describe() == "below 2.0.0");
}

TEST_CASE("version range: a bare version means that version or a compatible newer one (as ^)") {
    matches("1.2.3", {"1.2.3", "1.9.0"}, {"1.2.2", "2.0.0"});
    matches("1.2", {"1.2.0", "1.9.0"}, {"1.1.0", "2.0.0"});
    matches("0.3", {"0.3.0", "0.3.9"}, {"0.2.9", "0.4.0"});
    matches("2", {"2.0.0", "2.5.0"}, {"1.9.0", "3.0.0"});
}

TEST_CASE("version range: x and * stand for any number at their place") {
    matches("1.x", {"1.0.0", "1.9.9"}, {"0.9.0", "2.0.0"});
    matches("1.*", {"1.0.0", "1.9.9"}, {"2.0.0"});
    matches("1.X.x", {"1.0.0", "1.9.9"}, {"2.0.0"});
    matches("1.2.x", {"1.2.0", "1.2.9"}, {"1.1.9", "1.3.0"});
    matches("1.2.*", {"1.2.0", "1.2.9"}, {"1.3.0"});
}

TEST_CASE("version range: a hyphen range includes both ends") {
    matches("1.2.3 - 2.3.4", {"1.2.3", "2.0.0", "2.3.4"}, {"1.2.2", "2.3.5"});
    matches("1.2 - 2.3", {"1.2.0", "2.3.9"}, {"1.1.9", "2.4.0"});  // up to every 2.3.x
    CHECK(parse_constraint("1.2.3 - 2.3.4")->describe() == "1.2.3 or newer, up to 2.3.4");
}

TEST_CASE("version range: || takes a version that fits either side") {
    matches("1.x || >=3.0.0", {"1.0.0", "1.9.0", "3.0.0", "4.2.0"}, {"0.9.0", "2.0.0", "2.9.9"});
    matches("^1.0 || ^2.0", {"1.0.0", "2.9.9"}, {"3.0.0", "0.9.9"});
    matches(">=1, <2 || >=3", {"1.5.0", "3.0.0"}, {"2.5.0"});
    CHECK(parse_constraint("^1.0 || ^2.0")->describe() == "1.0.0 or newer, below 2.0.0; or 2.0.0 or newer, below 3.0.0");
}

TEST_CASE("version range: what EMF cannot read is refused with a reason that shows the right forms") {
    CHECK(refused(">>1").find("^1.2.0") != std::string::npos);  // the reason lists forms that work
    refused("");
    refused("^");
    refused(">=");
    refused("abc");
    refused("1.2.3.4");
    refused("1.2.3 -");
    refused("1.0 ||");
    refused("|| 1.0");
    refused(">=1.0,");
    refused("1.x.3");      // nothing after a wildcard
    refused("v1.2");       // versions are numbers, as in "version"
    CHECK(refused(">=1.x").find("write \">=1.0.0\"") != std::string::npos);  // wildcards only stand alone or after ^ / ~
    CHECK(refused("<=1.x").find("write \"<2.0.0\"") != std::string::npos);    // the hint means what npm reads
    CHECK(refused(">1.2.x").find("write \">=1.3.0\"") != std::string::npos);
    CHECK(refused("=1.x").find("write \"1.x\"") != std::string::npos);
    CHECK(refused("1.2.3-beta").find("pre-release") != std::string::npos);
    CHECK(refused(">=2.0 <1.0").find("no version") != std::string::npos);  // matches nothing: surely a mistake
    refused("=1.0 =2.0");
    CHECK(refused("<0").find("no version") != std::string::npos);  // nothing is below 0.0.0
    CHECK(refused("<0.0.0").find("no version") != std::string::npos);
    CHECK(refused("<0.0").find("no version") != std::string::npos);
    matches("<=0", {"0.0.0", "0.9.9"}, {"1.0.0"});  // still fine: up to every 0.x
}

TEST_CASE("version range: how a version misses a range (too old, too new, in a gap)") {
    const auto caret = *parse_constraint("^1.2");
    CHECK(fit(v("1.5.0"), caret) == Fit::Ok);
    CHECK(fit(v("1.1.0"), caret) == Fit::TooOld);
    CHECK(fit(v("2.0.0"), caret) == Fit::TooNew);
    const auto two = *parse_constraint("1.x || 3.x");
    CHECK(fit(v("2.1.0"), two) == Fit::Between);
    CHECK(fit(v("0.5.0"), two) == Fit::TooOld);
    CHECK(fit(v("4.0.0"), two) == Fit::TooNew);
    CHECK(fit(v("0.0.1"), *parse_constraint("*")) == Fit::Ok);
    CHECK(fit(v("1.0.0"), *parse_constraint("<=0.9")) == Fit::TooNew);
}
