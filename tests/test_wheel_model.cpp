// The radial wheel's logic: which entry the mouse direction highlights, pages, and how a pick or cancel happens.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/overlay/wheel_model.h"
#include "core/overlay/wheel_ui.h"

using emf::core::KeyEvent;
using emf::core::WheelModel;

namespace {
std::vector<std::string> names(int n) {
    std::vector<std::string> v;
    for (int i = 0; i < n; ++i) v.push_back("item" + std::to_string(i));
    return v;
}
KeyEvent key(uint16_t scan, uint16_t vk, bool down) {
    KeyEvent e;
    e.scan = scan;
    e.vkey = vk;
    e.down = down;
    return e;
}
constexpr uint16_t kJ = 0x4A;
}  // namespace

TEST_CASE("wheel: the mouse direction highlights a slice, slice 0 at the top, clockwise") {
    WheelModel w;
    w.open("Recipes", names(8), true, kJ, 0);
    REQUIRE(w.is_open());
    CHECK(w.highlighted() == -1);  // no movement yet: centre
    w.mouse(0, -100);               // up
    CHECK(w.highlighted() == 0);
    w.mouse(100, 100);              // now (100, 0): right
    CHECK(w.highlighted() == 2);
    w.mouse(-100, 100);             // (0, 100): down
    CHECK(w.highlighted() == 4);
    w.mouse(-100, -100);            // (-100, 0): left
    CHECK(w.highlighted() == 6);

    WheelModel four;
    four.open("", names(4), true, kJ, 0);
    four.mouse(100, 0);  // right with 4 entries (90 degree slices)
    CHECK(four.highlighted() == 1);
    four.mouse(-60, 60);  // (40, 60): down-right, nearer down
    CHECK(four.highlighted() == 2);
}

TEST_CASE("wheel: inside the dead zone nothing is highlighted; the radius is clamped but keeps the direction") {
    WheelModel w;
    w.open("", names(8), true, kJ, 0);
    w.mouse(WheelModel::kDeadZone - 5, 0);
    CHECK(w.highlighted() == -1);
    w.mouse(5000, 0);  // far right
    const auto d = w.direction();
    CHECK(d.first == doctest::Approx(WheelModel::kMaxRadius));
    CHECK(d.second == doctest::Approx(0.0f));
    w.mouse(0, -400);  // a bit up from the clamped point: still mostly right
    CHECK(w.highlighted() == 1);
}

TEST_CASE("wheel: 8 entries per page; scrolling pages (and wraps); the highlight is on the shown page") {
    WheelModel w;
    w.open("", names(19), true, kJ, 0);
    CHECK(w.pages() == 3);
    CHECK(w.page() == 0);
    w.mouse(0, -100);
    CHECK(w.highlighted() == 0);
    w.scroll(-1);  // wheel down: next page
    CHECK(w.page() == 1);
    CHECK(w.highlighted() == 8);
    w.scroll(-1);
    CHECK(w.page() == 2);  // the last page has 3 entries (16..18)
    CHECK(w.highlighted() == 16);
    w.scroll(-1);
    CHECK(w.page() == 0);  // wraps
    w.scroll(1);
    CHECK(w.page() == 2);
}

TEST_CASE("wheel: releasing the hold key picks the highlighted entry, or cancels in the centre") {
    WheelModel w;
    w.open("", names(8), true, kJ, 0);
    w.mouse(100, 0);
    CHECK_FALSE(w.key(key(0x24, kJ, true), 1000).has_value());  // the key going down again does nothing
    CHECK_FALSE(w.key(key(0x11, 'W', false), 1000).has_value());  // another key's release does nothing
    const auto r = w.key(key(0x24, kJ, false), 1000);
    REQUIRE(r.has_value());
    CHECK(*r == 2);
    CHECK_FALSE(w.is_open());

    w.open("", names(8), true, kJ, 0);
    const auto c = w.key(key(0x24, kJ, false), 1000);  // released without moving
    REQUIRE(c.has_value());
    CHECK(*c == -1);
}

TEST_CASE("wheel: Esc and right click cancel; left click picks only a highlighted entry") {
    WheelModel w;
    w.open("", names(8), false, kJ, 0);  // click mode
    CHECK_FALSE(w.key(key(0x24, kJ, false), 1000).has_value());  // the hold key means nothing in click mode
    CHECK_FALSE(w.button(0).has_value());                   // nothing highlighted yet
    w.mouse(0, 100);
    const auto r = w.button(0);
    REQUIRE(r.has_value());
    CHECK(*r == 4);

    w.open("", names(8), false, kJ, 0);
    const auto esc = w.key(key(0x01, VK_ESCAPE, true), 1000);
    REQUIRE(esc.has_value());
    CHECK(*esc == -1);

    w.open("", names(8), false, kJ, 0);
    const auto right = w.button(1);
    REQUIRE(right.has_value());
    CHECK(*right == -1);

    w.open("", names(3), false, kJ, 0);
    const auto cancelled = w.cancel();
    REQUIRE(cancelled.has_value());
    CHECK(*cancelled == -1);
    CHECK_FALSE(w.cancel().has_value());  // already closed
}

TEST_CASE("wheel: opening again resets the direction and the page") {
    WheelModel w;
    w.open("A", names(20), true, kJ, 0);
    w.mouse(100, 0);
    w.scroll(-1);
    w.open("B", names(5), true, kJ, 0);
    CHECK(w.page() == 0);
    CHECK(w.highlighted() == -1);
    CHECK(w.title() == "B");
    CHECK(w.items().size() == 5);
}

TEST_CASE("wheel labels: details in brackets go on a second line") {
    const auto two = emf::core::wheel_label_lines("Mace (6 Wood Logs, 1 Stone)");
    CHECK(two.first == "Mace");
    CHECK(two.second == "(6 Wood Logs, 1 Stone)");
    const auto one = emf::core::wheel_label_lines("Stop working");
    CHECK(one.first == "Stop working");
    CHECK(one.second.empty());
}

TEST_CASE("wheel title: a long title wraps at a space onto two lines") {
    const auto ten_per_char = [](const std::string& s) { return 10.0f * static_cast<float>(s.size()); };
    // "Villager 1026: make what?" is 25 chars = 250 px; 150 px fits 15 chars a line
    const auto two = emf::core::wheel_title_lines("Villager 1026: make what?", 150.0f, ten_per_char);
    CHECK(two.first == "Villager 1026:");
    CHECK(two.second == "make what?");
    const auto one = emf::core::wheel_title_lines("Villager 7", 150.0f, ten_per_char);
    CHECK(one.first == "Villager 7");
    CHECK(one.second.empty());
    // no space to break at: one line, cut later by the drawing code
    const auto word = emf::core::wheel_title_lines("Supercalifragilistic", 150.0f, ten_per_char);
    CHECK(word.first == "Supercalifragilistic");
    CHECK(word.second.empty());
    // the break goes at the last space that keeps the first line inside the width
    const auto late = emf::core::wheel_title_lines("aa bb cc dd ee ff gg", 120.0f, ten_per_char);
    CHECK(late.first == "aa bb cc dd");
    CHECK(late.second == "ee ff gg");
}

TEST_CASE("wheel: a quick tap of the hold key (released within 300 ms, nothing highlighted) keeps it open for a click") {
    WheelModel w;
    w.open("", names(8), true, kJ, 5000);
    CHECK_FALSE(w.key(key(0x24, kJ, false), 5150).has_value());  // tapped: still open
    CHECK(w.is_open());
    CHECK_FALSE(w.hold_mode());  // now a left click picks
    CHECK_FALSE(w.key(key(0x24, kJ, false), 5200).has_value());  // a second release means nothing
    w.mouse(0, -100);
    const auto r = w.button(0);
    REQUIRE(r.has_value());
    CHECK(*r == 0);

    w.open("", names(8), true, kJ, 5000);
    w.mouse(150, 0);
    const auto quick_pick = w.key(key(0x24, kJ, false), 5100);  // quick, but a clear flick toward an entry: picks it
    REQUIRE(quick_pick.has_value());
    CHECK(*quick_pick == 2);

    w.open("", names(8), true, kJ, 5000);
    const auto slow = w.key(key(0x24, kJ, false), 5300);  // held 300 ms or more and released in the centre: cancel
    REQUIRE(slow.has_value());
    CHECK(*slow == -1);
}

TEST_CASE("wheel: a quick tap picks only after a clear flick, not after a little mouse movement") {
    WheelModel w;
    w.open("", names(8), true, kJ, 5000);
    w.mouse(0, -60);  // highlights the top entry, but this is the hand settling during a tap
    CHECK(w.highlighted() == 0);
    CHECK_FALSE(w.key(key(0x24, kJ, false), 5100).has_value());
    CHECK(w.is_open());
    CHECK_FALSE(w.hold_mode());
    w.open("", names(8), true, kJ, 5000);
    w.mouse(0, -150);
    const auto r = w.key(key(0x24, kJ, false), 5100);
    REQUIRE(r.has_value());
    CHECK(*r == 0);
    w.open("", names(8), true, kJ, 5000);
    w.mouse(0, -60);  // held past the tap window: the highlighted entry is picked as before
    const auto held = w.key(key(0x24, kJ, false), 5400);
    REQUIRE(held.has_value());
    CHECK(*held == 0);
}

TEST_CASE("wheel: smooth mouse wheels turn a page per whole notch") {
    WheelModel w;
    w.open("", names(20), true, kJ, 0);
    for (int i = 0; i < 3; ++i) w.scroll(-0.25f);
    CHECK(w.page() == 0);
    w.scroll(-0.25f);
    CHECK(w.page() == 1);
    w.scroll(-2.0f);
    CHECK(w.page() == 0);  // two notches: 1 -> 2 -> 0 (wraps over 3 pages)
    w.open("", names(20), true, kJ, 0);
    w.scroll(-0.5f);
    w.open("", names(20), true, kJ, 0);  // opening again forgets a half notch
    w.scroll(-0.5f);
    CHECK(w.page() == 0);
}
