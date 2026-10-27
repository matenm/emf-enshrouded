// examples/cpp/waypoints: the waypoint book (names, the per-world text), the direction from the camera and
// the line on screen. The game side (waypoints.cpp) is built with the repository and checked in game.
#include <doctest.h>

#include <string>

#include "../examples/cpp/waypoints/src/waypoint_book.h"

using waypoints::Book;
using waypoints::Vec;

TEST_CASE("waypoints: a name is 1-24 letters, digits, _ or - (one word, so a command line can carry it)") {
    CHECK(waypoints::valid_name("home"));
    CHECK(waypoints::valid_name("Copper_mine-2"));
    CHECK(waypoints::valid_name(std::string(24, 'a')));
    CHECK_FALSE(waypoints::valid_name(""));
    CHECK_FALSE(waypoints::valid_name(std::string(25, 'a')));
    CHECK_FALSE(waypoints::valid_name("my home"));
    CHECK_FALSE(waypoints::valid_name("caf\xc3\xa9"));  // UTF-8 letters: not in a name
}

TEST_CASE("waypoints: set adds or moves one (names ignore case), find and erase ignore case, the book is sorted by name") {
    Book b;
    CHECK(b.set("mine", {1, 2, 3}) == Book::Set::Added);
    CHECK(b.set("Home", {10, 20, 30}) == Book::Set::Added);
    CHECK(b.set("HOME", {11, 21, 31}) == Book::Set::Moved);
    REQUIRE(b.all().size() == 2);
    CHECK(b.all()[0].name == "Home");  // keeps the first spelling
    CHECK(b.all()[1].name == "mine");
    const auto* h = b.find("home");
    REQUIRE(h != nullptr);
    CHECK(h->pos.x == 11);
    CHECK(b.set("not ok", {0, 0, 0}) == Book::Set::BadName);
    CHECK(b.erase("hOmE"));
    CHECK_FALSE(b.erase("home"));
    CHECK(b.find("home") == nullptr);
    CHECK(b.all().size() == 1);
}

TEST_CASE("waypoints: the book holds at most 50; moving one is still allowed when full") {
    Book b;
    for (size_t i = 0; i < Book::kMax; ++i) REQUIRE(b.set("p" + std::to_string(i), {0, 0, 0}) == Book::Set::Added);
    CHECK(b.set("one_more", {0, 0, 0}) == Book::Set::Full);
    CHECK(b.set("p7", {5, 5, 5}) == Book::Set::Moved);
}

TEST_CASE("waypoints: the per-world text is one line per waypoint and reads back the same") {
    Book b;
    b.set("mine", {1, 2, 3});
    b.set("home", {10.5, 20, -30.25});
    const std::string text = b.to_text();
    CHECK(text == "home 10.50 20.00 -30.25\nmine 1.00 2.00 3.00\n");
    const Book again = Book::from_text(text);
    REQUIRE(again.all().size() == 2);
    CHECK(again.all()[0].name == "home");
    CHECK(again.all()[0].pos.z == doctest::Approx(-30.25));
    CHECK(again.to_text() == text);
}

TEST_CASE("waypoints: lines it cannot read are skipped (an edited or older file never stops the mod)") {
    const Book b = Book::from_text(
        "home 1 2 3\r\n"
        "nonsense\n"
        "bad name! 1 2 3\n"
        "short 1 2\n"
        "letters a b c\n"
        "too 1 2 3 4\n"
        "\n"
        "Home 7 8 9\n"   // the same name again: the later line wins
        "cave -1.5 2 1e3\n");
    REQUIRE(b.all().size() == 2);
    CHECK(b.find("home")->pos.x == 7);
    CHECK(b.find("cave")->pos.z == doctest::Approx(1000));
    CHECK(Book::from_text("").all().empty());
}

TEST_CASE("waypoints: which way a point lies from where the camera looks (8 directions)") {
    const Vec at{0, 0, 0}, north{0, 0, 1};
    CHECK(waypoints::direction(at, north, {0, 0, 10}) == "ahead");
    CHECK(waypoints::direction(at, north, {10, 0, 10}) == "ahead right");
    CHECK(waypoints::direction(at, north, {10, 0, 0}) == "right");
    CHECK(waypoints::direction(at, north, {10, 0, -10}) == "behind right");
    CHECK(waypoints::direction(at, north, {0, 0, -10}) == "behind");
    CHECK(waypoints::direction(at, north, {-10, 0, -10}) == "behind left");
    CHECK(waypoints::direction(at, north, {-10, 0, 0}) == "left");
    CHECK(waypoints::direction(at, north, {-10, 0, 10}) == "ahead left");
    CHECK(waypoints::direction(at, {1, 0, 0}, {0, 0, -10}) == "right");     // turned: looking along +x
    CHECK(waypoints::direction(at, {0, -0.7, 0.7}, {0, 0, 10}) == "ahead");  // looking down a little
    CHECK(waypoints::direction(at, {0, -1, 0}, {0, 0, 10}).empty());         // straight down: no direction
    CHECK(waypoints::direction({5, 0, 5}, north, {5, 30, 5}).empty());       // straight above you
}

TEST_CASE("waypoints: the line on screen says how far, which way and how much higher or lower") {
    const Vec at{0, 0, 0}, north{0, 0, 1};
    CHECK(waypoints::guide_text("home", at, &north, {0, 4, 120}) == "home: 120 m ahead, 4 m up");
    CHECK(waypoints::guide_text("home", at, &north, {90, -10, 0}) == "home: 90 m right, 10 m down");
    CHECK(waypoints::guide_text("home", at, &north, {0, 2, 120}) == "home: 120 m ahead");  // 2 m: not worth a word
    CHECK(waypoints::guide_text("home", at, nullptr, {0, 0, 120}) == "home: 120 m");       // no camera known
    CHECK(waypoints::guide_text("tower", at, &north, {0, 50, 0}) == "tower: 0 m, 50 m up");
}

TEST_CASE("waypoints: you have arrived within 4 m") {
    CHECK(waypoints::arrived({0, 0, 0}, {3, 0, 2}));
    CHECK_FALSE(waypoints::arrived({0, 0, 0}, {3, 0, 3}));
    CHECK_FALSE(waypoints::arrived({0, 0, 0}, {0, 5, 0}));  // right below a tower top is not there
}
