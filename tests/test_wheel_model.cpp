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
KeyEvent key(uint16_t scan, uint16_t vk, bool down, bool extended = false) {
    KeyEvent e;
    e.scan = scan;
    e.vkey = vk;
    e.down = down;
    e.extended = extended;
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

namespace {
std::string lines_text(const std::vector<emf::core::WheelLabelLine>& lines) {
    std::string out;
    for (const auto& l : lines) out += (out.empty() ? "" : "|") + std::string(l.dim ? "~" : "") + l.text;
    return out;
}
}  // namespace

TEST_CASE("wheel labels: a name too wide for its slice wraps (after a shared wheel's section first), its detail below, dimmed") {
    const auto ten_per_char = [](const std::string& s) { return 10.0f * static_cast<float>(s.size()); };
    using emf::core::wheel_label_layout;
    // fits: as before (one line, the detail dimmed below)
    CHECK(lines_text(wheel_label_layout("Mace (6 Wood Logs, 1 Stone)", 300.0f, ten_per_char)) == "Mace|~(6 Wood Logs, 1 Stone)");
    CHECK(lines_text(wheel_label_layout("Stop working", 300.0f, ten_per_char)) == "Stop working");
    CHECK(lines_text(wheel_label_layout("Undead: Gather", 160.0f, ten_per_char)) == "Undead: Gather");
    // a shared wheel's "Section: " goes on its own dimmed line when the whole name does not fit
    CHECK(lines_text(wheel_label_layout("Beasts: Call Sabretooth (rests 1:20)", 160.0f, ten_per_char)) ==
          "~Beasts:|Call Sabretooth|~(rests 1:20)");
    // and what is left still too wide wraps at a space
    CHECK(lines_text(wheel_label_layout("Beasts: Dismiss Sabretooth 2", 160.0f, ten_per_char)) ==
          "~Beasts:|Dismiss|Sabretooth 2");
    // no section: the name wraps at the last space that fits
    CHECK(lines_text(wheel_label_layout("Raise Bone Golem Champion (0/1)", 120.0f, ten_per_char)) ==
          "Raise Bone|Golem Champion|~(0/1)");
    // one long word: one line (the drawing cuts it)
    CHECK(lines_text(wheel_label_layout("Supercalifragilistic", 120.0f, ten_per_char)) == "Supercalifragilistic");
    // a colon that is not a section (no space after it, or the name fits) stays where it is
    CHECK(lines_text(wheel_label_layout("Time 12:30", 120.0f, ten_per_char)) == "Time 12:30");
}

TEST_CASE("wheel labels: a page of 8 slices gives a label about 13 characters of the overlay's font a line, at any screen size") {
    for (float h : {1080.0f, 1440.0f, 2160.0f}) {
        const float w = emf::core::wheel_label_width(ImVec2(h * 16.0f / 9.0f, h), 8);
        const float ch = 0.55f * (h / 54.0f);  // Consolas at height / 54 pixels (overlay.cpp's setup_fonts)
        CHECK(13.0f * ch <= w);
        CHECK(15.0f * ch > w);
    }
    CHECK(emf::core::wheel_label_width(ImVec2(1920, 1080), 2) > emf::core::wheel_label_width(ImVec2(1920, 1080), 8));
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

TEST_CASE("wheel: 1-8 choose the slice with that number on the page shown (top row by scan code, numpad by key)") {
    WheelModel w;
    w.open("Orders", names(5), false, 0, 0);
    CHECK(w.key(key(0x02, '1', true), 10) == 0);  // 1: the top slice
    w.open("Orders", names(5), false, 0, 0);
    CHECK(w.key(key(0x06, 0x00DE, true), 10) == 4);  // 5 on an AZERTY keyboard types '(' : its scan code counts
    w.open("Orders", names(5), false, 0, 0);
    CHECK(!w.key(key(0x07, '6', true), 10));  // no sixth entry: nothing
    CHECK(w.is_open());
    CHECK(!w.key(key(0x03, '2', false), 10));  // a release picks nothing
    CHECK(w.key(key(0x4F, 0x61, true), 10) == 0);  // numpad 1
    w.open("Recipes", names(12), true, kJ, 0);
    w.scroll(-1.0f);                                 // the second page: entries 8-11
    CHECK(w.key(key(0x03, '2', true), 10) == 9);
    w.open("Recipes", names(12), true, kJ, 0);
    CHECK(!w.key(key(0x02, '1', true, true), 10));  // an extended key with that scan code is another key
}

TEST_CASE("wheel: the numpad chooses with NumLock off too (its keys then come as arrows, home, end, without E0)") {
    WheelModel w;
    w.open("Orders", names(8), false, 0, 0);
    CHECK(w.key(key(0x4B, 0x25, true), 10) == 3);  // numpad 4 (VK_LEFT without E0): the fourth slice, not a page
    w.open("Orders", names(8), false, 0, 0);
    CHECK(w.key(key(0x4F, 0x23, true), 10) == 0);  // numpad 1 (VK_END)
    w.open("Orders", names(8), false, 0, 0);
    CHECK(w.key(key(0x48, 0x26, true), 10) == 7);  // numpad 8 (VK_UP)
    w.open("Orders", names(8), false, 0, 0);
    CHECK(!w.key(key(0x48, 0x26, true, true), 10));  // the arrow Up (E0): nothing
    CHECK(w.is_open());
}

TEST_CASE("wheel: Left and Right turn the pages") {
    WheelModel w;
    w.open("Recipes", names(20), false, 0, 0);
    CHECK(w.page() == 0);
    CHECK(!w.key(key(0x4D, 0x27, true, true), 10));  // Right
    CHECK(w.page() == 1);
    w.key(key(0x4B, 0x25, true, true), 10);  // Left
    w.key(key(0x4B, 0x25, true, true), 10);  // and round to the last page
    CHECK(w.page() == 2);
    CHECK(w.is_open());
}

TEST_CASE("wheel: a wheel opened with a number key does not pick with that key's own presses (its key repeat)") {
    WheelModel w;
    w.open("Orders", names(5), true, '2', 0);  // a mod's wheel key is "2", held
    CHECK(!w.key(key(0x03, '2', true), 50));    // the held key repeating
    CHECK(w.is_open());
    CHECK(w.key(key(0x04, '3', true), 60) == 2);  // other numbers still choose
}
