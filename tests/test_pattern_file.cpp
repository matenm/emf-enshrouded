#include <doctest.h>

#include <cstdint>
#include <string>

#include "common/pattern_file.h"

using namespace emf;

TEST_CASE("pattern file: entries with options; comments, blank lines and CRLF are fine") {
    const std::string text =
        "# my mod's patterns\r\n"
        "\r\n"
        "game_log = 48 89 5C 24 08 57 48 83 EC 20\r\n"
        "daytime_ptr = 48 8B 05 ?? ?? ?? ?? 48 85 C0 ; offset=3 ; rel32\r\n"
        "  flag = 83 3D ?? ?? ?? ?? 01 ; offset=2 ; rel32 ; tail=1   \r\n"
        "name_table = 4E 61 6D 65 ; section=.rdata\n";
    const PatternFile f = parse_pattern_file(text);
    CHECK(f.errors.empty());
    REQUIRE(f.entries.size() == 4);
    CHECK(f.entries[0].name == "game_log");
    CHECK(f.entries[0].query.bytes == "48 89 5C 24 08 57 48 83 EC 20");
    CHECK(f.entries[0].section == ".text");
    CHECK(f.entries[0].line == 3);
    CHECK(f.entries[1].query.offset == 3);
    CHECK(f.entries[1].query.follow_rel32);
    CHECK(f.entries[2].name == "flag");
    CHECK(f.entries[2].query.rel32_tail == 1);
    CHECK(f.entries[3].section == ".rdata");
}

TEST_CASE("pattern file: bad lines are reported with their line number and skipped") {
    const std::string text =
        "good = 90 90\n"
        "no equals sign here\n"
        "bad_bytes = 48 ZZ\n"
        "bad_option = 90 ; colour=red\n"
        " = 90 90\n"
        "bad_offset = 90 ; offset=abc\n";
    const PatternFile f = parse_pattern_file(text);
    REQUIRE(f.entries.size() == 1);
    CHECK(f.entries[0].name == "good");
    REQUIRE(f.errors.size() == 5);
    CHECK(f.errors[0].find("line 2") != std::string::npos);
    CHECK(f.errors[1].find("line 3") != std::string::npos);
    CHECK(f.errors[2].find("colour") != std::string::npos);
    CHECK(f.errors[3].find("line 5") != std::string::npos);
    CHECK(f.errors[4].find("offset") != std::string::npos);
}

TEST_CASE("pattern file: a UTF-8 BOM (Notepad) is not part of the first name") {
    const PatternFile f = parse_pattern_file("\xEF\xBB\xBFgame_log = 48 89 5C\n");
    CHECK(f.errors.empty());
    REQUIRE(f.entries.size() == 1);
    CHECK(f.entries[0].name == "game_log");
}

TEST_CASE("pattern file: offsets are decimal (a leading 0 too, not octal) or 0x hex, and may be negative") {
    const PatternFile f = parse_pattern_file(
        "a = 90 ; offset=010\n"
        "b = 90 ; offset=0x10\n"
        "c = 90 ; offset=0X1f ; tail=0x2\n"
        "d = 90 ; offset=-3\n"
        "e = 90 ; offset=-0x10\n"
        "f = 90 ; offset=08\n");
    CHECK(f.errors.empty());
    REQUIRE(f.entries.size() == 6);
    CHECK(f.entries[0].query.offset == 10);
    CHECK(f.entries[1].query.offset == 16);
    CHECK(f.entries[2].query.offset == 31);
    CHECK(f.entries[2].query.rel32_tail == 2);
    CHECK(f.entries[3].query.offset == -3);
    CHECK(f.entries[4].query.offset == -16);
    CHECK(f.entries[5].query.offset == 8);
}

TEST_CASE("pattern file: a number that does not fit is refused with a message, not wrapped") {
    const PatternFile f = parse_pattern_file(
        "big = 90 ; offset=4294967296\n"
        "big_hex = 90 ; offset=0x80000000\n"
        "huge = 90 ; tail=99999999999999999999999\n"
        "max = 90 ; offset=2147483647\n"
        "min = 90 ; offset=-2147483648\n"
        "half = 90 ; offset=0x\n");
    REQUIRE(f.entries.size() == 2);
    CHECK(f.entries[0].query.offset == 2147483647);
    CHECK(f.entries[1].query.offset == INT32_MIN);
    REQUIRE(f.errors.size() == 4);
    CHECK(f.errors[0].find("line 1") != std::string::npos);
    CHECK(f.errors[0].find("out of range") != std::string::npos);
    CHECK(f.errors[1].find("out of range") != std::string::npos);
    CHECK(f.errors[2].find("tail") != std::string::npos);
    CHECK(f.errors[2].find("out of range") != std::string::npos);
    CHECK(f.errors[3].find("line 6") != std::string::npos);
}

TEST_CASE("pattern file: a trailing ';' (or an empty option) is ignored") {
    const PatternFile f = parse_pattern_file(
        "a = 90 90 ;\n"
        "b = 90 ; offset=1 ;\n"
        "c = 90 ;; rel32 ; \n");
    CHECK(f.errors.empty());
    REQUIRE(f.entries.size() == 3);
    CHECK(f.entries[0].query.bytes == "90 90");
    CHECK(f.entries[1].query.offset == 1);
    CHECK(f.entries[2].query.follow_rel32);
}

TEST_CASE("pattern file: tabs separate like spaces (around '=', ';' and between the bytes)") {
    const PatternFile f = parse_pattern_file("\tname\t=\t48\t8B\t05 ?? ;\toffset=3\t;\trel32\n");
    CHECK(f.errors.empty());
    REQUIRE(f.entries.size() == 1);
    CHECK(f.entries[0].name == "name");
    CHECK(f.entries[0].query.offset == 3);
    CHECK(f.entries[0].query.follow_rel32);
}

TEST_CASE("pattern file: a file without any pattern is an error (an empty file must not pass the check)") {
    for (const char* text : {"", "\xEF\xBB\xBF", "# only a comment\n\n   \n"}) {
        const PatternFile f = parse_pattern_file(text);
        CHECK(f.entries.empty());
        REQUIRE(f.errors.size() == 1);
        CHECK(f.errors[0].find("no patterns") != std::string::npos);
    }
    // a file whose every line is bad reports those lines (not also "no patterns")
    const PatternFile bad = parse_pattern_file("nope\n");
    REQUIRE(bad.errors.size() == 1);
    CHECK(bad.errors[0].find("line 1") != std::string::npos);
}

#include <fstream>
#include <sstream>

#include "../mods/sim_rate/sim_rate.h"

TEST_CASE("sim_rate: steps per second, rounded; its pattern file parses") {
    CHECK(simrate::per_second(90, 1000) == 90);
    CHECK(simrate::per_second(91, 1010) == 90);
    CHECK(simrate::per_second(45, 500) == 90);
    CHECK(simrate::per_second(5, 0) == 0);
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/mods/sim_rate/sim_rate.patterns", std::ios::binary);
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const auto pf = parse_pattern_file(ss.str());
    CHECK(pf.errors.empty());
    REQUIRE(pf.entries.size() == 1);
    CHECK(pf.entries[0].name == "sim_step");
}
