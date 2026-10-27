#include <doctest.h>

#include <string>

#include "core/overlay/hud_model.h"

using emf::core::HudModel;
using emf::core::HudPlace;
using Result = emf::core::HudModel::Result;

TEST_CASE("hud: lines are set, replaced and removed by id; listed by place, then id; the version counts changes") {
    HudModel h;
    CHECK(h.empty());
    const uint64_t v0 = h.version();
    CHECK(h.set("clock.time", "14:05", 0, 0) == Result::Ok);
    CHECK(h.set("b.second", "x", 2, 0xff00ffffu) == Result::Ok);
    CHECK(h.set("a.first", "y", 2, 0) == Result::Ok);
    CHECK(h.version() > v0);
    auto lines = h.lines();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].id == "clock.time");  // top right first
    CHECK(lines[0].place == HudPlace::TopRight);
    CHECK(lines[1].id == "a.first");     // then bottom left, by id
    CHECK(lines[2].id == "b.second");
    CHECK(lines[2].rgba == 0xff00ffffu);
    const uint64_t v1 = h.version();
    CHECK(h.set("clock.time", "14:05", 0, 0) == Result::Ok);  // the same: no change
    CHECK(h.version() == v1);
    CHECK(h.set("clock.time", "14:06", 0, 0) == Result::Ok);
    CHECK(h.version() > v1);
    CHECK(h.lines()[0].text == "14:06");
    CHECK(h.set("clock.time", "", 0, 0) == Result::Removed);
    CHECK(h.lines().size() == 2);
    CHECK(h.set("never.there", "", 0, 0) == Result::Removed);  // removing an unknown line is fine
}

TEST_CASE("hud: ids, places and the line limit are checked; long text is cut") {
    HudModel h;
    CHECK(h.set("", "x", 0, 0) == Result::BadId);
    CHECK(h.set(std::string(49, 'a'), "x", 0, 0) == Result::BadId);
    CHECK(h.set("bad id", "x", 0, 0) == Result::BadId);
    CHECK(h.set("ok.id", "x", 3, 0) == Result::BadPlace);
    for (int i = 0; i < 32; ++i) REQUIRE(h.set("m.line" + std::to_string(i), "x", 1, 0) == Result::Ok);
    CHECK(h.set("m.one_more", "x", 1, 0) == Result::Full);
    CHECK(h.set("m.line0", "changed", 1, 0) == Result::Ok);  // an existing line can still change
    HudModel g;
    REQUIRE(g.set("long.text", std::string(500, 'z'), 0, 0) == Result::Ok);
    CHECK(g.lines()[0].text.size() == HudModel::kMaxText);
}

TEST_CASE("hud: shown only with lines, the console closed, and a world running (not on the main menu)") {
    CHECK(emf::core::show_hud(false, true, true));
    CHECK_FALSE(emf::core::show_hud(true, true, true));    // the console covers it
    CHECK_FALSE(emf::core::show_hud(false, false, true));  // nothing to show
    CHECK_FALSE(emf::core::show_hud(false, true, false));  // main menu / loading: the last values would be stale
}

TEST_CASE("hud: long text is cut on a character boundary (UTF-8)") {
    HudModel h;
    std::string text(HudModel::kMaxText - 1, 'a');
    text += "\xC3\xA6\xC3\xB8";  // "æø": the first character straddles the limit
    REQUIRE(h.set("utf.text", text, 0, 0) == Result::Ok);
    const std::string got = h.lines()[0].text;
    CHECK(got.size() == HudModel::kMaxText - 1);  // the half character is dropped
    CHECK(got.back() == 'a');
}
